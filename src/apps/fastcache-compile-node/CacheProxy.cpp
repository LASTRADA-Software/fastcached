// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"

#include <format>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;
} // namespace

std::vector<std::byte> CacheProxy::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: with no declared length there is nowhere to resynchronize to,
        // so there is nothing an answer could mean. The only condition that closes.
        return {};

    if (!Wire::IsSupported(header->version))
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                      std::format("supported versions {}..{}",
                                                  static_cast<unsigned>(Wire::MinSupportedVersion),
                                                  static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

    switch (descriptor->code)
    {
        case Wire::Op::Fetch: {
            auto const key = Wire::DecodeFetchPayload(payload);
            if (!key.has_value())
                return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

            auto const found = _cache.Fetch(Wire::AsStringView(*key));
            if (!found.has_value())
                // A miss is `Miss` with a zero-length payload, never `Error`. The two
                // being one byte is a defect this wire has already recorded paying
                // for: a rejected client saw an endlessly cold cache and no
                // diagnostic, and the build merely got slower forever.
                return Wire::EncodeReply(Wire::Status::Miss, {});
            return Wire::EncodeReply(Wire::Status::Ok, *found);
        }
        case Wire::Op::Store: {
            auto const fields = Wire::DecodeStorePayload(payload);
            if (!fields.has_value())
                return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

            // The roots the client sent are deliberately ignored. Canonicalization is
            // the SHARED cache's job -- it is what makes a stored object layout-neutral
            // for every machine -- and doing it here as well would rewrite a value
            // twice against two different layouts. What this tier stores is what this
            // machine will replay, and what it forwards is what the client produced.
            if (!_cache.Store(Wire::AsStringView(fields->key), fields->value))
                return Wire::EncodeErrorReply(Wire::ErrorCode::StorageWriteFailed);
            return Wire::EncodeReply(Wire::Status::Ok, {});
        }
        default:
            // A scheduler or worker verb at the cache port. Answered rather than
            // dropped, so a client that reached the wrong one of this node's ports
            // learns which instead of seeing something indistinguishable from a dead
            // host.
            return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted,
                                          "this endpoint is the node's cache; scheduling and compiles are served on "
                                          "their own ports");
    }
}

} // namespace FastCache::Node
