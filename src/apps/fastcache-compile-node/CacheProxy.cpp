// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>

#include <format>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;
} // namespace

Task<std::vector<std::byte>> CacheProxy::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: with no declared length there is nowhere to resynchronize to,
        // so there is nothing an answer could mean. The only condition that closes.
        co_return std::vector<std::byte> {};

    if (!Wire::IsSupported(header->version))
        co_return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                         std::format("supported versions {}..{}",
                                                     static_cast<unsigned>(Wire::MinSupportedVersion),
                                                     static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        co_return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        co_return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

    switch (descriptor->code)
    {
        case Wire::Op::Fetch: {
            auto const key = Wire::DecodeFetchPayload(payload);
            if (!key.has_value())
                co_return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

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
                co_return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

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
                co_return Wire::EncodeErrorReply(Wire::ErrorCode::StorageWriteFailed);
            co_return Wire::EncodeReply(Wire::Status::Ok, {});
        }
        default:
            // A scheduler or worker verb at the cache port. Answered rather than
            // dropped, so a client that reached the wrong one of this node's ports
            // learns which instead of seeing something indistinguishable from a dead
            // host.
            co_return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted,
                                             "this endpoint is the node's cache; scheduling and compiles are served on "
                                             "their own ports");
    }
}

} // namespace FastCache::Node
