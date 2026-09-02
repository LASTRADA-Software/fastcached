// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The issue that will decide which of this tier's refusals are events worth
    /// counting.
    ///
    /// Every refusal below is answered correctly and counts nothing, and none of them
    /// has been argued either way -- so they are `RefuseUntriaged` rather than
    /// `RefuseWithoutCounter`, which would claim a decision nobody has made. The test
    /// #447 extracted is "would a rise here mean something happened", and applying it
    /// per arm on this surface is
    /// [#491](https://github.com/LASTRADA-Software/fastcached/issues/491).
    /// `worker-refusals-counted` counts these and prints the total on every run, so
    /// the backlog cannot be added to unnoticed.
    constexpr std::uint32_t CacheRefusalTriage = 491;

    /// This surface's rows. The shape, the lookup and why they exist are on
    /// `Wire::RefusedVerb`; what belongs here is only which verbs and what they say.
    ///
    /// `Auth`, because a `FASTCACHE_TOKEN` launcher had a permanent 0% hit rate that
    /// presented exactly as a cache that is merely cold.
    constexpr std::array RefusedVerbs {
        Wire::RefusedVerb { .op = Wire::Op::Auth,
                            .code = Wire::UnimplementedVerb,
                            .why = "this endpoint is the node's cache and checks no credential" },
    };

    // The table is consulted from the `default:` arm only, so a row naming FETCH or
    // STORE would sit there looking like a decision and change nothing. Refused at
    // compile time rather than left to be noticed.
    static_assert(std::ranges::none_of(
                      RefusedVerbs,
                      [](Wire::Op op) { return op == Wire::Op::Fetch || op == Wire::Op::Store; },
                      &Wire::RefusedVerb::op),
                  "a refusal row for a verb this tier serves is dead: the lookup never reaches it");
} // namespace

Task<std::vector<std::byte>> CacheProxy::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: with no declared length there is nowhere to resynchronize to,
        // so there is nothing an answer could mean. The only condition that closes.
        co_return std::vector<std::byte> {};

    if (!Wire::IsSupported(header->version))
        co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::UnsupportedVersion, .issue = CacheRefusalTriage },
                                      std::format("supported versions {}..{}",
                                                  static_cast<unsigned>(Wire::MinSupportedVersion),
                                                  static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::UnknownOpcode, .issue = CacheRefusalTriage });

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::MalformedFrame, .issue = CacheRefusalTriage });

    switch (descriptor->code)
    {
        case Wire::Op::Fetch: {
            auto const key = Wire::DecodeFetchPayload(payload);
            if (!key.has_value())
                co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::MalformedFrame, .issue = CacheRefusalTriage });

            auto const found = co_await _cache.Fetch(Wire::AsStringView(*key));
            if (!found.has_value())
                // A miss is `Miss` with a zero-length payload, never `Error`. The two
                // being one byte is a defect this wire has already recorded paying
                // for: a rejected client saw an endlessly cold cache and no
                // diagnostic, and the build merely got slower forever.
                co_return Wire::EncodeReply(Wire::Status::Miss, {});
            co_return Wire::EncodeReply(Wire::Status::Ok, *found);
        }
        case Wire::Op::Store: {
            auto const fields = Wire::DecodeStorePayload(payload);
            if (!fields.has_value())
                co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::MalformedFrame, .issue = CacheRefusalTriage });

            // Canonicalized against the roots the client sent, through the one
            // recipe both servers on this wire share.
            //
            // This block used to ignore those roots, on the reasoning that
            // canonicalization is "the SHARED cache's job" and that "what this tier
            // stores is what this machine will replay". Both were true when a node
            // was a private tier in front of `fastcached`. #229 made a node the
            // shared cache, and "this machine" is not one layout -- every checkout on
            // it is a different one -- so a value stored here kept its producer's
            // absolute paths and every consumer replayed them into its build system's
            // dependency graph (#319).
            //
            // A value that does not decode is stored VERBATIM rather than refused,
            // which is where this server's policy differs from the daemon's: an
            // opaque value is not this tier's business to reject. `CanonicalStoredValue`
            // answers `nullopt` and each server says what it wants to.
            auto const canonical = CanonicalStoredValue(
                fields->value, Wire::AsStringView(fields->srcRoot), Wire::AsStringView(fields->buildTree));
            auto const toStore = canonical.has_value() ? std::span<std::byte const> { *canonical } : fields->value;

            if (!co_await _cache.Store(Wire::AsStringView(fields->key), toStore))
                co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::StorageWriteFailed, .issue = CacheRefusalTriage });
            co_return Wire::EncodeReply(Wire::Status::Ok, {});
        }
        default:
            if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
                co_return Cc::RefuseUntriaged({ .code = row->code, .issue = CacheRefusalTriage }, row->why);

            // A scheduler or worker verb at the cache port. Answered rather than
            // dropped, so a client that reached the wrong one of this node's ports
            // learns which instead of seeing something indistinguishable from a dead
            // host.
            co_return Cc::RefuseUntriaged({ .code = Wire::ErrorCode::DispatchNotPermitted, .issue = CacheRefusalTriage },
                                          "this endpoint is the node's cache; scheduling and compiles are served on "
                                          "their own ports");
    }
}

} // namespace FastCache::Node
