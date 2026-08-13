// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"

#include <format>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Build an outcome that carries nothing but its kind (Miss, Transport).
    [[nodiscard]] CacheOutcome Plain(CacheOutcomeKind kind)
    {
        CacheOutcome outcome;
        outcome.kind = kind;
        return outcome;
    }

    /// Build a Hit outcome around the served bytes.
    [[nodiscard]] CacheOutcome Hit(std::vector<std::byte> value)
    {
        CacheOutcome outcome;
        outcome.kind = CacheOutcomeKind::Hit;
        outcome.value = std::move(value);
        return outcome;
    }

    /// Build a Rejected outcome carrying the daemon's own reason.
    [[nodiscard]] CacheOutcome Rejected(Wire::ErrorCode code, std::string message)
    {
        CacheOutcome outcome;
        outcome.kind = CacheOutcomeKind::Rejected;
        outcome.code = code;
        outcome.message = std::move(message);
        return outcome;
    }

    /// Read exactly one reply frame, header first and then precisely the number
    /// of bytes the header declared.
    ///
    /// Draining by the declared length is what keeps a connection usable after a
    /// refusal. The pre-version client read a single acknowledgement byte and
    /// left an error's trailing message in the socket — harmless only because a
    /// fresh connection was opened per operation, and a latent desynchronisation
    /// the moment one was ever reused. Here it cannot be forgotten: there is one
    /// reader, and it always consumes a whole frame.
    /// @param client Connected transport.
    /// @return The outcome, with the payload attached.
    [[nodiscard]] CacheOutcome RecvReply(ITcpClient& client)
    {
        auto const headerBytes = client.RecvExactly(Wire::ReplyHeaderSize);
        if (!headerBytes.has_value())
            return Plain(CacheOutcomeKind::Transport);

        auto const header = Wire::DecodeReplyHeader(*headerBytes);
        if (!header.has_value())
            return Plain(CacheOutcomeKind::Transport);

        std::vector<std::byte> payload;
        if (header->payloadLength > 0)
        {
            auto received = client.RecvExactly(header->payloadLength);
            if (!received.has_value())
                return Plain(CacheOutcomeKind::Transport);
            payload = std::move(*received);
        }

        switch (header->status)
        {
            case Wire::Status::Ok:
                return Hit(std::move(payload));

            case Wire::Status::Miss:
                return Plain(CacheOutcomeKind::Miss);

            case Wire::Status::Error: {
                auto const decoded = Wire::DecodeErrorPayload(payload);
                if (!decoded.has_value())
                    return Plain(CacheOutcomeKind::Transport);
                return Rejected(decoded->first, std::string { decoded->second });
            }
        }
        return Plain(CacheOutcomeKind::Transport);
    }

    /// Send a framed request and read its reply.
    /// @param client Connected transport.
    /// @param frame The framed request.
    /// @return The outcome.
    [[nodiscard]] CacheOutcome Exchange(ITcpClient& client, std::vector<std::byte> const& frame)
    {
        if (!client.SendAll(frame))
            return Plain(CacheOutcomeKind::Transport);
        return RecvReply(client);
    }

} // namespace

std::string DescribeOutcome(CacheOutcome const& outcome)
{
    switch (outcome.kind)
    {
        case CacheOutcomeKind::Hit:
            return {};
        case CacheOutcomeKind::Miss:
            return "not cached";
        case CacheOutcomeKind::Transport:
            return "transport failure";
        case CacheOutcomeKind::Rejected: {
            auto const* descriptor = Wire::Describe(outcome.code);
            auto const name = descriptor != nullptr ? descriptor->name : std::string_view { "unknown" };
            return outcome.message.empty() ? std::format("rejected ({})", name)
                                           : std::format("rejected ({}): {}", name, outcome.message);
        }
    }
    return "unknown outcome";
}

CacheOutcome CacheFetch(ITcpClient& client, std::string_view key)
{
    return Exchange(client, Wire::EncodeFetch(key));
}

CacheOutcome CacheStore(ITcpClient& client, Wire::StoreRequest const& request)
{
    return Exchange(client, Wire::EncodeStore(request));
}

} // namespace FastCache::Cc
