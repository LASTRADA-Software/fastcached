// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// An ITcpClient that replays canned reply bytes and records what was sent.
///
/// TcpClient_test drives a real loopback peer, which is right for asserting
/// timeouts but cannot pose as a daemon of the wrong version. This fake can, and
/// it exposes its read cursor so a test can prove the client consumed exactly
/// one reply frame and no more.
class ScriptedTcpClient final: public ITcpClient
{
  public:
    /// @param replies The bytes the "daemon" will return, in order.
    explicit ScriptedTcpClient(std::vector<std::byte> replies):
        _replies(std::move(replies))
    {
    }

    bool SendAll(std::span<std::byte const> bytes) override
    {
        _sent.insert(_sent.end(), bytes.begin(), bytes.end());
        return true;
    }

    std::optional<std::vector<std::byte>> RecvExactly(std::size_t count) override
    {
        if (_replies.size() - _cursor < count)
            return std::nullopt;
        std::vector<std::byte> out { _replies.begin() + static_cast<std::ptrdiff_t>(_cursor),
                                     _replies.begin() + static_cast<std::ptrdiff_t>(_cursor + count) };
        _cursor += count;
        return out;
    }

    /// Everything the client wrote.
    [[nodiscard]] std::vector<std::byte> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many reply bytes the client consumed.
    [[nodiscard]] std::size_t Cursor() const noexcept
    {
        return _cursor;
    }

  private:
    std::vector<std::byte> _replies;
    std::vector<std::byte> _sent;
    std::size_t _cursor { 0 };
};

/// A client whose socket fails on send.
class FailingTcpClient final: public ITcpClient
{
  public:
    bool SendAll(std::span<std::byte const> /*bytes*/) override
    {
        return false;
    }
    std::optional<std::vector<std::byte>> RecvExactly(std::size_t /*count*/) override
    {
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("CacheFetch sends exactly what the wire module specifies")
{
    // Asserting against the shared encoder rather than a hand-written literal is
    // what makes client and server agree by construction: both sides frame
    // through the same function, so they cannot drift apart independently.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
    (void) CacheFetch(client, "the-key");

    CHECK(client.Sent() == Wire::EncodeFetch("the-key"));
}

TEST_CASE("CacheFetch returns the payload on a hit")
{
    auto const stored = std::vector<std::byte> { std::byte { 0xDE }, std::byte { 0xAD } };
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, stored) };

    auto const outcome = CacheFetch(client, "k");
    CHECK(outcome.kind == CacheOutcomeKind::Hit);
    CHECK(outcome.IsHit());
    CHECK(outcome.value == stored);
}

TEST_CASE("CacheFetch reports a version rejection distinctly from a miss")
{
    // The defect this whole change exists to fix. On the pre-version wire both
    // answers were the byte 0x00, so a launcher pointed at a daemon that could
    // not serve it reported "not cached" forever and the build merely got
    // slower with no explanation.
    auto const missOutcome = [] {
        ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
        return CacheFetch(client, "k");
    }();

    auto const rejectedOutcome = [] {
        ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                                          "unsupported wire version 2; this server speaks 1..1") };
        return CacheFetch(client, "k");
    }();

    CHECK(missOutcome.kind == CacheOutcomeKind::Miss);
    CHECK(rejectedOutcome.kind == CacheOutcomeKind::Rejected);
    CHECK(rejectedOutcome.kind != missOutcome.kind);

    CHECK(rejectedOutcome.code == Wire::ErrorCode::UnsupportedVersion);
    // The daemon's own words must reach the caller: they name the supported
    // range, which is the only actionable part of the diagnosis.
    CHECK(rejectedOutcome.message.contains("1..1"));
    CHECK(DescribeOutcome(rejectedOutcome).contains("unsupported-version"));
    CHECK(DescribeOutcome(rejectedOutcome).contains("1..1"));
}

TEST_CASE("CacheStore drains a refusal by its declared length")
{
    // Prove the reply is consumed whole. The old client read one acknowledgement
    // byte and left the error's message in the socket; harmless only because a
    // connection was never reused, and a desynchronisation waiting to happen.
    auto reply = Wire::EncodeErrorReply(Wire::ErrorCode::StorageWriteFailed, "storage write failed");
    auto const replyLength = reply.size();

    // A sentinel the client must NOT touch.
    auto const sentinel = std::vector<std::byte> { std::byte { 0x5A }, std::byte { 0x5A }, std::byte { 0x5A } };
    reply.insert(reply.end(), sentinel.begin(), sentinel.end());

    ScriptedTcpClient client { reply };
    auto const value = std::vector<std::byte> { std::byte { 0x01 } };
    auto const outcome = CacheStore(client,
                                    Wire::StoreRequest { .key = "k",
                                                         .prefetchGroup = "c",
                                                         .srcRoot = "/s",
                                                         .buildTree = "/b",
                                                         .value = std::span<std::byte const> { value } });

    CHECK(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(outcome.code == Wire::ErrorCode::StorageWriteFailed);
    CHECK(client.Cursor() == replyLength);
}

TEST_CASE("CacheStore sends exactly what the wire module specifies")
{
    auto const value = std::vector<std::byte> { std::byte { 0x77 } };
    Wire::StoreRequest const request {
        .key = "k", .prefetchGroup = "c", .srcRoot = "/s", .buildTree = "/b", .value = std::span<std::byte const> { value }
    };

    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, {}) };
    auto const outcome = CacheStore(client, request);

    CHECK(outcome.kind == CacheOutcomeKind::Hit);
    CHECK(client.Sent() == Wire::EncodeStore(request));
}

TEST_CASE("A transport failure is not mistaken for a miss or a refusal")
{
    // A build must fall back to a real compile in every one of these cases, but
    // only one of them is worth telling the user about.
    SECTION("send fails")
    {
        FailingTcpClient client;
        CHECK(CacheFetch(client, "k").kind == CacheOutcomeKind::Transport);
    }

    SECTION("the peer closes before a full reply header")
    {
        ScriptedTcpClient client { std::vector<std::byte> { std::byte { 0x01 } } };
        CHECK(CacheFetch(client, "k").kind == CacheOutcomeKind::Transport);
    }

    SECTION("the peer declares more payload than it sends")
    {
        auto truncated = Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte>(8, std::byte { 0x11 }));
        truncated.resize(truncated.size() - 2);
        ScriptedTcpClient client { truncated };
        CHECK(CacheFetch(client, "k").kind == CacheOutcomeKind::Transport);
    }
}
