// SPDX-License-Identifier: Apache-2.0
//
// The launcher's one exchange, driven on a reactor.
//
// Every case here uses a `TestReactor` and a scripted connector, so the rules --
// the budget, an unreachable endpoint, a peer that accepts and goes quiet -- are
// asserted with no socket and no clock of the machine's.
#include "ReactorExchange.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A socket that answers with scripted bytes, or never answers at all.
class ScriptedPeer final: public ISocket
{
  public:
    /// @param reply What a read should hand back, byte by byte. Empty means the
    ///        peer accepts and then says nothing -- the case a dial timeout cannot
    ///        catch and only an exchange budget can.
    explicit ScriptedPeer(std::vector<std::byte> reply) noexcept:
        _reply { std::move(reply) }
    {
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        if (_closed)
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "closed" }) };

        if (_offset >= _reply.size())
        {
            // Parked forever: nothing completes this but `Close()`, which is exactly
            // what a peer that accepted and then went quiet looks like.
            IoAwaitable parked;
            parked.SetSuspendCallback(
                [](IoAwaitable* self, std::coroutine_handle<>) {
                    static_cast<ScriptedPeer*>(self->CallbackState())->_parked = self;
                },
                this);
            return parked;
        }

        auto const n = std::min(buffer.size(), _reply.size() - _offset);
        std::copy_n(_reply.begin() + static_cast<std::ptrdiff_t>(_offset), n, buffer.begin());
        _offset += n;
        return IoAwaitable { IoResult { n } };
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        _sent.insert(_sent.end(), buffer.begin(), buffer.end());
        return IoAwaitable { IoResult { buffer.size() } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        std::size_t total = 0;
        for (auto const& segment: segments)
        {
            _sent.insert(_sent.end(), segment.begin(), segment.end());
            total += segment.size();
        }
        return IoAwaitable { IoResult { total } };
    }

    void Close() noexcept override
    {
        _closed = true;
        // Completing the parked read is what `Close` MEANS on a reactor socket, and
        // it is the whole mechanism the exchange budget relies on.
        if (auto* const parked = std::exchange(_parked, nullptr); parked != nullptr)
            parked->Complete(
                std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "socket closed" }));
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

    [[nodiscard]] bool WasClosed() const noexcept
    {
        return _closed;
    }

  private:
    std::vector<std::byte> _reply;
    std::vector<std::byte> _sent;
    std::size_t _offset { 0 };
    IoAwaitable* _parked { nullptr };
    bool _closed { false };
};

} // namespace

namespace
{

/// A connector that hands out a scripted peer, or refuses.
class ScriptedConnector final: public IConnector
{
  public:
    [[nodiscard]] Task<SocketResult> Connect(std::string host,
                                             std::uint16_t port,
                                             std::chrono::milliseconds /*connectTimeout*/) override
    {
        _dials += 1;
        _lastHost = std::move(host);
        _lastPort = port;
        if (_refuse)
            co_return std::unexpected(
                NetError { .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" });

        auto peer = std::make_unique<ScriptedPeer>(_reply);
        _peer = peer.get();
        co_return peer;
    }

    void Refuse() noexcept
    {
        _refuse = true;
    }

    void Reply(std::vector<std::byte> bytes)
    {
        _reply = std::move(bytes);
    }

    [[nodiscard]] int Dials() const noexcept
    {
        return _dials;
    }

    [[nodiscard]] std::string const& LastHost() const noexcept
    {
        return _lastHost;
    }

    [[nodiscard]] std::uint16_t LastPort() const noexcept
    {
        return _lastPort;
    }

    [[nodiscard]] ScriptedPeer* Peer() const noexcept
    {
        return _peer;
    }

  private:
    std::vector<std::byte> _reply;
    std::string _lastHost;
    ScriptedPeer* _peer { nullptr };
    int _dials { 0 };
    std::uint16_t _lastPort { 0 };
    bool _refuse { false };
};

} // namespace

TEST_CASE("An exchange returns the daemon's answer")
{
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply(Wire::EncodeReply(Wire::Status::Miss, {}));

    Cc::ReactorExchange exchange { reactor, connector };
    auto const outcome = exchange.Run("cache.example.com:6674", Wire::EncodeFetch("k"), {}, {});

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Miss);
    CHECK(connector.Dials() == 1);
    // Split and handed over unbracketed, which is `DialEndpoint`'s job and is worth
    // asserting here because this is the only path that exercises it end to end.
    CHECK(connector.LastHost() == "cache.example.com");
    CHECK(connector.LastPort() == 6674);
}

TEST_CASE("An unreachable endpoint is a transport failure, not a throw")
{
    // Every caller answers a transport failure by compiling, which is the whole
    // reason an optional accelerator can never fail a build.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Refuse();

    Cc::ReactorExchange exchange { reactor, connector };
    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, {});

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
}

TEST_CASE("Text that names no port is refused without dialling")
{
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;

    Cc::ReactorExchange exchange { reactor, connector };
    auto const outcome = exchange.Run("6674", Wire::EncodeFetch("k"), {}, {});

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
    // Nothing was dialled: a bare port is a misconfiguration, and quietly trying
    // loopback would turn a typo into a connection to whatever is listening locally.
    CHECK(connector.Dials() == 0);
}

TEST_CASE("A peer that accepts and then goes quiet is bounded by the total budget")
{
    // THE property `SO_RCVTIMEO` could not give, and the reason the launcher needed
    // an end-to-end bound at all. A dial that succeeds says the peer accepted and
    // nothing about whether it will ever answer -- and the old per-call socket
    // timeout bounded one `recv`, so a daemon dribbling a byte at a time could hold
    // a compile open indefinitely while never once exceeding it.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply({}); // accepts, answers nothing

    Cc::ReactorExchange exchange { reactor, connector };

    // The reactor is driven by `Run()`, and `TestReactor::Run` returns as soon as
    // both queues drain -- so the clock has to be advanced from a task ON the
    // reactor rather than from here. This one does nothing else.
    //
    // ONE jump well past the deadline, not a run of small ones. The deadline is a
    // bounded poll (`IReactor::Schedule` cannot be cancelled, so `DeadlineTimer`
    // re-arms rather than parking once), which means a run of N advances only makes
    // N steps of progress toward it -- so how far the clock has to move depends on
    // the poll interval, a constant this test has no business knowing. Jumping past
    // the deadline in one go lets the timer chew through the remaining steps inline
    // and removes the dependency entirely. It also stopped this passing on Linux and
    // failing on macOS, which is what a hidden dependency on step counting looks like.
    auto advance = [](TestReactor* loop, ManualClock* c, std::chrono::milliseconds past) -> DetachedTask {
        co_await ResumeOn { *loop };
        c->Advance(past);
        co_return;
    };

    constexpr Cc::ExchangeBudget Budget {};
    advance(&reactor, &clock, Budget.total * 4);

    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Budget);

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
    REQUIRE(connector.Peer() != nullptr);
    // Closed rather than merely abandoned. A budget that only stopped WAITING would
    // leave the coroutine parked on a read nobody completes, which is a leaked frame
    // and a leaked descriptor on a path the launcher walks once per translation unit.
    CHECK(connector.Peer()->WasClosed());
}

TEST_CASE("A reactor exchange runs once")
{
    // `IReactor::Run` returns only on `Stop()`, and no reactor here clears that
    // flag -- so a second `Run()` returns immediately and the exchange silently does
    // not happen. The launcher would answer that by compiling locally, every time,
    // with nothing anywhere saying why. Asserted rather than documented.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply(Wire::EncodeReply(Wire::Status::Miss, {}));

    Cc::ReactorExchange exchange { reactor, connector };
    CHECK(exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, {}).kind == Cc::CacheOutcomeKind::Miss);

    // A second run on the same instance is a programmer error, caught by the assert
    // in a debug build. Only the first is asserted here, because a death test for an
    // assertion is not portable and the guard's value is that it fires during
    // development rather than that a suite can observe it.
    CHECK(connector.Dials() == 1);
}
