// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "LocalCache.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>

using namespace FastCache;

namespace
{
namespace Wire = CompileCacheWire;

/// A socket that replays a fixed byte stream and records what was written.
///
/// The launcher's own `ScriptedTcpClient`, minimally. It is duplicated rather than
/// shared because the point of this file is that these two binaries share *nothing*
/// but the wire: a fixture reaching across would be the coupling the test denies.
class ScriptedSocket final: public ISocket
{
  public:
    /// @param replies The bytes the "server" returns, in order.
    explicit ScriptedSocket(std::vector<std::byte> replies):
        _replies { std::move(replies) }
    {
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
    {
        return IoAwaitable { IoResult { bytes.size() } };
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        auto const take = std::min(_replies.size() - _cursor, buffer.size());
        std::copy_n(_replies.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
        _cursor += take;
        // Zero is EOF, which is how `RecvExactly` learns the peer ran out.
        return IoAwaitable { IoResult { take } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        std::size_t total = 0;
        for (auto const& segment: segments)
            total += segment.size();
        return IoAwaitable { IoResult { total } };
    }

    void Close() noexcept override {}

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }

  private:
    std::vector<std::byte> _replies;
    std::size_t _cursor = 0;
};

/// Concatenate reply frames into one scripted stream.
/// @param frames The frames, in the order the server would send them.
/// @return Their concatenation.
[[nodiscard]] std::vector<std::byte> Replies(std::initializer_list<std::vector<std::byte>> frames)
{
    std::vector<std::byte> out;
    for (auto const& frame: frames)
        out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

/// What a scheduler really answers an AUTH with.
///
/// Produced by the production `SchedulerProtocol`, never hand-written: a test
/// asserting against `EncodeErrorReply(UnimplementedVerb, ...)` would pass while the
/// server sent something else entirely, which is the whole failure being regressed.
/// @return The scheduler's refusal frame.
[[nodiscard]] std::vector<std::byte> SchedulerAuthRefusal()
{
    ManualClock clock;
    ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    // No `SetRole`: the refusal is answered before `Route`, so before any `Gate()`.
    // Leadership is not part of this contract, and a line setting it would tell a
    // reader it is.
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, {} };
    Distributed::SchedulerProtocol protocol { service };

    auto const auth = Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" });
    return protocol.Answer(auth, Distributed::CallerContext { .peerId = "127.0.0.1" });
}

} // namespace

TEST_CASE("A credentialled client reaches a scheduler that has no AUTH and still gets its answer", "[node][auth-contract]")
{
    // **The acceptance of #340, and deliberately not "the refusal code changed".**
    // That assertion passes the moment a constant is edited; this one fails unless
    // the two binaries actually agree, because the bytes come out of the real
    // `SchedulerProtocol` and go into the real `Cc::CacheProtocol`.
    //
    // The two link nothing in common -- `fastcache-cc` compiles `CompileCacheWire.hpp`
    // in and links none of `FastCache` -- so the enumerator each names is the only
    // thing holding them together, and this is the only place both are present.
    //
    // What it regresses: with `FASTCACHE_TOKEN` set, a scheduler answering AUTH with
    // `DispatchNotPermitted` had that refusal returned in place of the answer to the
    // request the client actually sent. Every LEASE was declined, every compile
    // happened locally, and the build went green while the fleet distributed nothing.
    auto const stored = std::vector<std::byte> { std::byte { 0x42 } };
    ScriptedSocket socket { Replies({ SchedulerAuthRefusal(), Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    auto const outcome = SyncRun(Cc::CacheFetch(&socket, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    // The command behind the credential is served. This is the half that was broken.
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);

    // And the operator is still told their token went unchecked. A surface that
    // silently does less than it was configured to is the failure this codebase keeps
    // a list about -- restoring the answer must not also swallow that.
    CHECK(outcome.credentialIgnored);
}

TEST_CASE("A credentialled client reaches a cache tier that has no AUTH and still gets its answer", "[node][auth-contract]")
{
    // The third surface, and the one that started this. #283 corrected its refusal
    // code but asserted only the enumerator -- which is the assertion this file
    // exists to say is not enough, so leaving the cache tier without a behavioural
    // case would have shipped a rule with a counterexample beside it.
    //
    // Same contract, third server: the bytes come out of the real `CacheProxy` and go
    // into the real `Cc::CacheProtocol`.
    InMemoryLruStorage local { 64 * 1024 };
    Node::NoUpstream upstream;
    ManualClock clock;
    AtomicMetricsSink metrics;
    Node::LocalCache cache { local, upstream, clock, metrics };
    Node::CacheProxy proxy { cache };

    auto const refusal = SyncRun(proxy.Answer(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" })));
    REQUIRE_FALSE(refusal.empty());

    auto const stored = std::vector<std::byte> { std::byte { 0x9 } };
    ScriptedSocket socket { Replies({ refusal, Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    auto const outcome = SyncRun(Cc::CacheFetch(&socket, "k", Cc::Credential { .username = {}, .secret = "s3cret" }));

    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);
    CHECK(outcome.credentialIgnored);
}
