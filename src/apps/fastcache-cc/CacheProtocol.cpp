// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/TcpClient.hpp>

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
    [[nodiscard]] Task<CacheOutcome> RecvReply(ISocket* client)
    {
        auto const headerBytes = co_await FastCache::RecvExactly(client, Wire::ReplyHeaderSize);
        if (!headerBytes.has_value())
            co_return Plain(CacheOutcomeKind::Transport);

        auto const header = Wire::DecodeReplyHeader(*headerBytes);
        if (!header.has_value())
            co_return Plain(CacheOutcomeKind::Transport);

        std::vector<std::byte> payload;
        if (header->payloadLength > 0)
        {
            auto received = co_await FastCache::RecvExactly(client, header->payloadLength);
            if (!received.has_value())
                co_return Plain(CacheOutcomeKind::Transport);
            payload = std::move(*received);
        }

        switch (header->status)
        {
            case Wire::Status::Ok:
                co_return Hit(std::move(payload));

            case Wire::Status::Miss:
                co_return Plain(CacheOutcomeKind::Miss);

            case Wire::Status::Error: {
                auto const decoded = Wire::DecodeErrorPayload(payload);
                if (!decoded.has_value())
                    co_return Plain(CacheOutcomeKind::Transport);
                co_return Rejected(decoded->first, std::string { decoded->second });
            }
        }
        co_return Plain(CacheOutcomeKind::Transport);
    }

    /// Send a framed request — optionally preceded by an AUTH frame in the SAME
    /// write — and read the reply that belongs to the request.
    ///
    /// The two frames go out together rather than as a send/await/send sequence.
    /// Replies are strictly ordered and one-per-request, so pipelining is
    /// well-defined, and it is what keeps authentication free: the launcher opens
    /// a fresh connection per operation, so waiting for the AUTH reply would add a
    /// round trip to every translation unit in a build.
    ///
    /// The AUTH reply is consumed **first and unconditionally**. Skipping it on
    /// the assumption it succeeded would leave a whole frame in the socket and
    /// hand the caller the AUTH reply as if it were the command's — a
    /// desynchronisation that reads as a bizarre outcome rather than as the
    /// authentication failure it is.
    ///
    /// The two frames are written back-to-back but as **two calls**, not one
    /// concatenated buffer. Both spellings are equally pipelined — neither waits
    /// for a reply, which is the property that matters — but concatenating means
    /// copying the command frame, and a STORE frame carries a whole object file
    /// (up to 256 MiB by default). That would raise peak footprint from roughly
    /// twice the object to three times it, on the hot path of a parallel build,
    /// to buy nothing.
    ///
    /// @param client Connected transport.
    /// @param frame The framed request.
    /// @param credential Credential to present ahead of it; none when unconfigured.
    /// @return The command's outcome, or the AUTH refusal when the credential was
    ///         rejected (the command's own reply is still drained first).
    /// Parameters by VALUE, not by reference. clang-tidy enforces this on
    /// coroutines and it is right to: the frame outlives the call expression, so
    /// a reference would bind to storage the caller may already have destroyed.
    /// It costs nothing here because every caller moves into it -- a STORE frame
    /// carries a whole object file, and copying it would double the peak
    /// footprint on the hot path of a parallel build.
    [[nodiscard]] Task<CacheOutcome> Exchange(ISocket* client, std::vector<std::byte> frame, Credential credential)
    {
        if (!credential.Configured())
        {
            if (!co_await FastCache::SendAll(client, frame))
                co_return Plain(CacheOutcomeKind::Transport);
            co_return co_await RecvReply(client);
        }

        auto const authFrame =
            Wire::EncodeAuth(Wire::AuthRequest { .username = credential.username, .secret = credential.secret });
        if (!co_await FastCache::SendAll(client, authFrame) || !co_await FastCache::SendAll(client, frame))
            co_return Plain(CacheOutcomeKind::Transport);

        // Non-const so the returns below can move rather than copy: an outcome
        // can carry a whole cached object, and `performance-no-automatic-move`
        // rejects the const spelling outright.
        auto authOutcome = co_await RecvReply(client);
        if (authOutcome.kind == CacheOutcomeKind::Transport)
            co_return authOutcome;

        // The command's reply is read even when AUTH was refused: the server
        // answers every request it read, so leaving it in the socket would strand
        // a frame and the next command on this connection would read this one's
        // answer.
        auto commandOutcome = co_await RecvReply(client);

        // A daemon that predates the AUTH verb answers it `unknown-opcode` and —
        // because the framing was built to let a receiver step over a verb it does
        // not know and carry on — serves the command behind it perfectly well. So
        // that reply is the good one, and returning the refusal instead would give
        // a token-configured launcher a permanent 0% hit rate against every
        // not-yet-upgraded daemon in a fleet, reported as `rejected
        // (unknown-opcode)`. That is precisely the mixed-fleet case the wire's
        // extensibility exists for, and adding a verb must not break it.
        //
        // `credentialIgnored` carries the fact upward rather than swallowing it:
        // an operator who set a token believes this traffic is authenticated, and
        // it is not. A cache that silently does less than it was told to is the
        // failure mode this codebase keeps a list about.
        if (authOutcome.kind == CacheOutcomeKind::Rejected && authOutcome.code == Wire::ErrorCode::UnknownOpcode)
        {
            commandOutcome.credentialIgnored = true;
            co_return commandOutcome;
        }

        // Any other refusal is about the credential itself (wrong secret, an
        // unsupported version, a malformed frame) and is what the caller has to
        // act on; the command's own reply says only "unauthenticated", which
        // explains nothing.
        if (!authOutcome.IsHit())
            co_return authOutcome;
        co_return commandOutcome;
    }

} // namespace

Task<CacheOutcome> ExchangeFramed(ISocket* client, std::vector<std::byte> frame, Credential credential)
{
    co_return co_await Exchange(client, std::move(frame), std::move(credential));
}

std::optional<std::string> RedirectTarget(CacheOutcome const& outcome)
{
    if (outcome.kind != CacheOutcomeKind::Rejected || outcome.code != Wire::ErrorCode::NotLeader)
        return std::nullopt;
    if (!SplitHostPort(outcome.message).has_value())
        return std::nullopt;
    return outcome.message;
}

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

Task<CacheOutcome> CacheFetch(ISocket* client, std::string_view key, Credential credential)
{
    co_return co_await Exchange(client, Wire::EncodeFetch(key), std::move(credential));
}

Task<CacheOutcome> CacheStore(ISocket* client, Wire::StoreRequest request, Credential credential)
{
    co_return co_await Exchange(client, Wire::EncodeStore(request), std::move(credential));
}

} // namespace FastCache::Cc
