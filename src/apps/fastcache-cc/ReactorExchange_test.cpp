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

class ScriptedPeer;

/// What a scripted peer did, kept where the TEST can still read it.
///
/// Not fields on the peer, and that is the whole point: the socket a connector
/// hands over is owned by the coroutine frame that dialled it, so it is destroyed
/// the moment that frame finishes -- which for a case asserting what the exchange
/// did to the socket is *before* the assertion runs. A raw back-pointer therefore
/// reads freed memory, and freed memory usually still says what it said: this file
/// passed 300 consecutive runs on one platform and failed every run on another,
/// and adding one `int` member to the peer was enough to flip the first. The log
/// outlives the socket, so every claim below is about memory somebody still owns.
struct PeerLog
{
    std::size_t sent { 0 };   ///< Bytes the exchange handed the peer.
    int reads { 0 };          ///< Reads the exchange issued, parked ones included.
    bool closed { false };    ///< Whether `Close()` was called.
    bool destroyed { false }; ///< Whether the peer was freed -- i.e. the dialling frame finished.

    /// The peer while it is alive, and null the moment it is not.
    ///
    /// The one thing a case may hold across the dialling frame's end, for exactly
    /// the reason the class note gives: a raw back-pointer to the socket outlives
    /// nothing, and freed memory usually still answers. Cleared by the destructor,
    /// so a driver running beside the exchange asks this before touching the peer.
    ScriptedPeer* live { nullptr };
};

/// A socket that answers with scripted bytes, or never answers at all.
class ScriptedPeer final: public ISocket
{
  public:
    /// @param reply What a read should hand back, byte by byte. Empty means the
    ///        peer accepts and then says nothing -- the case a dial timeout cannot
    ///        catch and only an exchange budget can.
    /// @param log Where to record what happened; must outlive this peer.
    ScriptedPeer(std::vector<std::byte> reply, PeerLog& log, bool dribble) noexcept:
        _reply { std::move(reply) },
        _log { &log },
        _dribble { dribble }
    {
        _log->live = this;
    }

    ~ScriptedPeer() override
    {
        _log->live = nullptr;
        _log->destroyed = true;
    }

    ScriptedPeer(ScriptedPeer const&) = delete;
    ScriptedPeer(ScriptedPeer&&) = delete;
    ScriptedPeer& operator=(ScriptedPeer const&) = delete;
    ScriptedPeer& operator=(ScriptedPeer&&) = delete;

    /// Hand a parked read exactly ONE byte of the scripted reply.
    ///
    /// Only meaningful on a dribbling peer, and only from the reactor's thread.
    /// Completing the awaitable resumes the exchange inline, which parks the next
    /// read before this returns -- so `_parked` is cleared first.
    /// @return False when there is no parked read, nothing left to send, or the
    ///         socket has been closed. A driver reads that as "stop".
    bool DeliverOneByte()
    {
        if (_closed || _parked == nullptr || _pending.empty() || _offset >= _reply.size())
            return false;
        _pending[0] = _reply[_offset];
        _offset += 1;
        _pending = {};
        auto* const parked = std::exchange(_parked, nullptr);
        parked->Complete(IoResult { 1 });
        return true;
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        _log->reads += 1;
        if (_closed)
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "closed" }) };

        // A dribbler parks EVERY read, however much reply is left: it never says
        // nothing and it never finishes, which is the shape no per-call ceiling
        // catches. The buffer stays live until the awaitable resumes -- `ISocket`'s
        // own contract -- so `DeliverOneByte` may write into it later.
        if (_dribble && _offset < _reply.size())
        {
            _pending = buffer;
            IoAwaitable parked;
            parked.SetSuspendCallback(
                [](IoAwaitable* self, std::coroutine_handle<>) {
                    static_cast<ScriptedPeer*>(self->CallbackState())->_parked = self;
                },
                this);
            return parked;
        }

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
        _log->sent += buffer.size();
        return IoAwaitable { IoResult { buffer.size() } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        std::size_t total = 0;
        for (auto const& segment: segments)
            total += segment.size();
        _log->sent += total;
        return IoAwaitable { IoResult { total } };
    }

    void Close() noexcept override
    {
        _closed = true;
        _log->closed = true;
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

  private:
    std::vector<std::byte> _reply;
    PeerLog* _log;
    /// The buffer of the currently parked read, valid until it resumes.
    std::span<std::byte> _pending;
    std::size_t _offset { 0 };
    IoAwaitable* _parked { nullptr };
    bool _dribble { false };
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

        auto peer = std::make_unique<ScriptedPeer>(_reply, _log, _dribble);
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

    /// Hand the reply over one byte per turn instead of in one go.
    void DribbleReplies() noexcept
    {
        _dribble = true;
    }

    /// The record, writable, for a driver that has to reach the live peer.
    /// @return The log; valid for this connector's lifetime.
    [[nodiscard]] PeerLog& MutableLog() noexcept
    {
        return _log;
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

    /// What the peer this connector handed out did.
    ///
    /// A log rather than the peer itself: the socket belongs to the frame that
    /// dialled it and is gone by the time a case asks. See `PeerLog`.
    /// @return The record, valid for this connector's lifetime.
    [[nodiscard]] PeerLog const& Log() const noexcept
    {
        return _log;
    }

  private:
    std::vector<std::byte> _reply;
    std::string _lastHost;
    PeerLog _log;
    int _dials { 0 };
    std::uint16_t _lastPort { 0 };
    bool _refuse { false };
    bool _dribble { false };
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
    auto const start = clock.Now();
    advance(&reactor, &clock, Budget.total * 4);

    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Budget);

    // Every step, in the order it happens, because the last claim alone cannot say
    // which of them did not: "the peer was never closed" is the same observation
    // whether the reactor never ran, the exchange never wrote, or the deadline never
    // fired.
    auto const& log = connector.Log();
    INFO("dials=" << connector.Dials() << " sent=" << log.sent << " reads=" << log.reads << " closed=" << log.closed
                  << " destroyed=" << log.destroyed << " advanced="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count() << "ms"
                  << " pendingSubmissions=" << reactor.PendingSubmissions() << " pendingTimers=" << reactor.PendingTimers());
    CHECK(connector.Dials() == 1);
    // The reactor ran at all: the advance task is the first thing in its queue.
    CHECK(clock.Now() > start);
    // The exchange got past the dial and wrote its request...
    CHECK(log.sent > 0);
    // ...and then parked on a read nobody was ever going to answer.
    CHECK(log.reads > 0);

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
    // Closed rather than merely abandoned. A budget that only stopped WAITING would
    // leave the coroutine parked on a read nobody completes, which is a leaked frame
    // and a leaked descriptor on a path the launcher walks once per translation unit.
    CHECK(log.closed);
    // And the frame that owned the socket finished, which is the other half of the
    // same claim: a budget that closed the socket but left the coroutine parked would
    // still leak, and the socket's own destructor is what says it did not.
    CHECK(log.destroyed);
}

TEST_CASE("A peer that dribbles a byte at a time is bounded by the total budget")
{
    // The case a per-call ceiling can NEVER catch, and the reason `total` is not a
    // second spelling of `SO_RCVTIMEO`. This peer is never silent and never
    // finished: every read is answered, so every per-call timer is reset, and the
    // reply is perfectly well-formed. Only a bound on the whole exchange stops it.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply(Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte>(4096, std::byte { 0x2A })));
    connector.DribbleReplies();

    constexpr Cc::ExchangeBudget Budget {};
    constexpr auto PerByte = 100ms;
    // More turns than the budget can survive, so the exchange stops because it ran
    // out of time and not because the peer ran out of reply -- which would assert
    // nothing at all.
    constexpr int Turns = 4096;
    static_assert(PerByte * Turns > Budget.total);

    // One byte per turn, each costing `PerByte` on the reactor's clock. A task ON
    // the reactor, for the reason the quiet-peer case gives: `TestReactor::Run`
    // returns as soon as its queues drain, so nothing outside the loop gets to move
    // the clock while an exchange is in flight.
    auto dribble =
        [](TestReactor* loop, ManualClock* c, PeerLog* log, std::chrono::milliseconds perByte, int turns) -> DetachedTask {
        for (int turn = 0; turn < turns; ++turn)
        {
            co_await ResumeOn { *loop };
            c->Advance(perByte);
            if (log->live == nullptr || !log->live->DeliverOneByte())
                co_return;
        }
    };

    Cc::ReactorExchange exchange { reactor, connector };
    auto const start = clock.Now();
    dribble(&reactor, &clock, &connector.MutableLog(), PerByte, Turns);

    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Budget);

    // Drained after the exchange stopped the reactor, so the driver reaches its own
    // `co_return` and frees its frame. `Run()` breaks on `Stop()` and would leave a
    // queued handle behind, which is a leak a sanitizer build reports and a plain
    // one does not.
    (void) reactor.Drain();

    auto const& log = connector.Log();
    INFO("reads=" << log.reads << " closed=" << log.closed << " destroyed=" << log.destroyed << " elapsed="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count() << "ms");
    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
    // It really was making progress, over and over: this is not the quiet peer
    // wearing a different name.
    CHECK(log.reads > 10);
    // And it was abandoned at the budget rather than when the reply ran out.
    CHECK(log.closed);
    CHECK(log.destroyed);
    CHECK(clock.Now() - start >= Budget.total);
    CHECK(clock.Now() - start < PerByte * Turns);
}

TEST_CASE("A budget of zero arms no deadline at all")
{
    // What `FASTCACHE_TIMEOUT_MS=0` has always been documented to mean, and what the
    // arithmetic alone would NOT do: a zero total puts the deadline at `Now()`, so
    // every exchange would be closed on the reactor's next turn. A knob that reads
    // as "turn the ceiling off" would have turned the cache off instead -- silently,
    // because every caller answers a transport failure by compiling.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply(Wire::EncodeReply(Wire::Status::Miss, {}));

    Cc::ReactorExchange exchange { reactor, connector };
    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Cc::ExchangeBudget { .total = 0ms });

    // The daemon's own answer, not the transport failure a fired deadline produces.
    CHECK(outcome.kind == Cc::CacheOutcomeKind::Miss);
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
