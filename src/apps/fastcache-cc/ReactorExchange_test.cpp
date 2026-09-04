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
#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A notice these cases do not inspect.
///
/// Shared on purpose: these assert the exchange's transport behaviour, not the
/// credential diagnostic. A case that asserted the diagnostic would need its own,
/// because the notice reports once and a shared one couples cases to Catch2's order.
/// @return A notice with no sink.
[[nodiscard]] FastCache::Cc::CredentialNotice& Unwatched()
{
    static FastCache::Cc::CredentialNotice notice = FastCache::Cc::CredentialNotice::Silent();
    return notice;
}

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
    ScriptedPeer(std::vector<std::byte> reply,
                 PeerLog& log,
                 bool dribble,
                 bool die,
                 ManualClock* clock,
                 std::chrono::milliseconds advanceOnRead) noexcept:
        _reply { std::move(reply) },
        _log { &log },
        _clock { clock },
        _advanceOnRead { advanceOnRead },
        _dribble { dribble },
        _die { die }
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

        // ONE jump past the deadline, from inside the reactor, on the first read --
        // which is the same mechanism "A peer that accepts and then goes quiet"
        // describes and the same reason for one jump rather than a run of small
        // ones. Done HERE rather than from a detached task beside the exchange,
        // because a peer that answers synchronously finishes the whole exchange
        // before `Run()` is ever entered: the reactor then returns on its stop flag
        // with that task still parked, and ASan reports the frame as leaked. The
        // clock has to move from something the exchange itself drives.
        if (_advanceOnRead > std::chrono::milliseconds::zero() && _log->reads == 1)
            _clock->Advance(_advanceOnRead);

        // A connection that DIED, as distinct from one this side closed. It is what
        // a keepalive probe that goes unanswered produces at the socket layer, and
        // the whole point of the distinction is that nothing here calls `Close()` --
        // so `SocketDeadlineTarget::expired` stays false and the exchange can tell
        // the two apart (#247).
        if (_die)
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted peer died" }) };

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
    /// Where the jump below lands; null when no case asked for one.
    ManualClock* _clock { nullptr };
    std::chrono::milliseconds _advanceOnRead { 0 };
    bool _dribble { false };
    /// The connection breaks on its own; see `ScriptedConnector::DieMidExchange`.
    bool _die { false };
    bool _closed { false };
};

} // namespace

namespace
{

/// A connector that hands out a scripted peer, or refuses.
class ScriptedConnector final: public IConnector
{
  public:
    [[nodiscard]] Task<SocketResult> Connect(std::string host, std::uint16_t port, DialOptions /*options*/) override
    {
        _dials += 1;
        _lastHost = std::move(host);
        _lastPort = port;
        if (_refuse)
            co_return std::unexpected(
                NetError { .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" });

        auto peer = std::make_unique<ScriptedPeer>(_reply, _log, _dribble, _die, _clock, _advanceOnRead);
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

    /// Accept, then have the connection break on its own.
    ///
    /// The half a dribbler cannot express: a dribbler is ALIVE and slow, and is
    /// bounded by the total budget. This peer is GONE, and nothing but the
    /// connection breaking says so -- which is what keepalive buys and what the
    /// exchange has to be able to name separately.
    void DieMidExchange() noexcept
    {
        _die = true;
    }

    /// Move @p past onto `clock` when the peer is first read.
    ///
    /// From inside the exchange rather than beside it, so it works for a peer that
    /// answers synchronously as well as one that parks. See `ScriptedPeer::Read`.
    /// @param clock The clock to move; must outlive the peer.
    /// @param past How far to jump.
    void AdvanceOnFirstRead(ManualClock& clock, std::chrono::milliseconds past) noexcept
    {
        _clock = &clock;
        _advanceOnRead = past;
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
    ///
    /// Non-const, because a driver running beside the exchange reaches the live peer
    /// through it. One accessor rather than a const one and a mutable twin: two names
    /// for one member is two things a later case has to choose between.
    /// @return The record, valid for this connector's lifetime.
    [[nodiscard]] PeerLog& Log() noexcept
    {
        return _log;
    }

  private:
    std::vector<std::byte> _reply;
    std::string _lastHost;
    PeerLog _log;
    int _dials { 0 };
    std::uint16_t _lastPort { 0 };
    ManualClock* _clock { nullptr };
    std::chrono::milliseconds _advanceOnRead { 0 };
    bool _refuse { false };
    bool _dribble { false };
    bool _die { false };
};

} // namespace

TEST_CASE("An exchange returns the daemon's answer")
{
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply(Wire::EncodeReply(Wire::Status::Miss, {}));

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
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

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, {});

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
}

TEST_CASE("Text that names no port is refused without dialling")
{
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
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

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };

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

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const start = clock.Now();
    dribble(&reactor, &clock, &connector.Log(), PerByte, Turns);

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

TEST_CASE("A worker that goes quiet is abandoned at the idle bound, not at the total")
{
    // **The whole of #245, from the client side.** A dispatched compile's total budget
    // has to be as long as the slowest translation unit anybody compiles -- ten minutes
    // here, derived from the scheduler's lease timeout -- so before this the same number
    // was also how long a worker that had stopped making progress went unnoticed. Its
    // host answers every keepalive probe, so nothing below the protocol can see it.
    //
    // The two bounds are set an order of magnitude apart and the clock is moved past the
    // SMALLER one only, so the case can only pass if the idle deadline is the one that
    // fired. Under a build with no idle bound the total never elapses, nothing closes
    // the socket, and the seeded outcome comes back naming nothing.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Reply({}); // accepts, answers nothing -- not even a pulse

    constexpr Cc::ExchangeBudget Budget { .connect = 1s, .total = 600s, .idle = 30s };
    static_assert(Budget.idle * 4 < Budget.total,
                  "the advance below must stay well inside the total, or this case cannot tell the two bounds apart");

    // ONE jump past the idle deadline, for the reason the quiet-peer case above gives:
    // the deadline is a bounded poll, so a run of small advances only makes a step of
    // progress each and how far the clock must move would depend on a poll interval this
    // case has no business knowing.
    auto advance = [](TestReactor* loop, ManualClock* c, std::chrono::milliseconds past) -> DetachedTask {
        co_await ResumeOn { *loop };
        c->Advance(past);
        co_return;
    };

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const start = clock.Now();
    advance(&reactor, &clock, Budget.idle * 4);

    auto const outcome = exchange.Run("127.0.0.1:6676", Wire::EncodeFetch("k"), {}, Budget);

    auto const& log = connector.Log();
    INFO("sent=" << log.sent << " reads=" << log.reads << " closed=" << log.closed << " destroyed=" << log.destroyed
                 << " elapsed=" << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count()
                 << "ms");
    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);

    // **Named, and that is half the point.** `Expired` says "this compile was slower
    // than we were prepared to wait" and sends an operator to raise a timeout;
    // `PeerLost` says "that machine is gone". Neither is true here, and either would
    // send somebody to fix something that was never wrong.
    CHECK(outcome.transportFailure == Cc::TransportFailure::Silent);

    // Closed and the owning frame finished, for the reason the total's own case gives: a
    // bound that only stopped WAITING leaves a coroutine parked on a read nobody
    // completes, which is a leaked frame and a leaked descriptor per translation unit.
    CHECK(log.closed);
    CHECK(log.destroyed);

    // And it happened well inside the total, which is the improvement stated as a
    // measurement rather than as a claim.
    CHECK(clock.Now() - start < Budget.total);
}

TEST_CASE("A pulse pushes the idle bound out, so a worker that keeps reporting is not abandoned")
{
    // The other direction, and the one that fails if the deadline is armed once instead
    // of being re-armed: the exchange runs LONGER than the idle bound in total, and is
    // abandoned only if a pulse does not reset it.
    //
    // Dribbled, and that is what makes this case mean anything -- the same reasoning the
    // zero-budget case below states. A peer whose reads resolve inline finishes the whole
    // exchange before `Run()` is ever entered, so the reactor never takes a turn and no
    // deadline of any kind gets to fire; such a case would pass against the very code it
    // exists to reject.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;

    // A pulse, then the answer. Five bytes each, handed over one byte per reactor turn.
    auto stream = Wire::EncodeProgressReply();
    auto const answer = Wire::EncodeReply(Wire::Status::Ok, {});
    stream.insert(stream.end(), answer.begin(), answer.end());
    REQUIRE(stream.size() == 2 * Wire::ReplyHeaderSize);
    connector.Reply(stream);
    connector.DribbleReplies();

    constexpr auto PerByte = 1s;
    constexpr Cc::ExchangeBudget Budget { .connect = 1s, .total = 600s, .idle = 8s };

    // The arithmetic, spelled out because it is the whole case. The pulse completes at
    // byte 5, so at 5s; the answer's header completes at byte 10, so at 10s.
    //
    //   armed once  -> the window opened at 0s expires at 8s, before byte 10 arrives.
    //   re-armed    -> the pulse at 5s opens a window to 13s, and byte 10 lands at 10s.
    //
    // Both margins are checked by the static_asserts rather than left to a reader.
    static_assert(PerByte * Wire::ReplyHeaderSize < Budget.idle, "the pulse must arrive INSIDE the first window");
    static_assert(PerByte * 2 * Wire::ReplyHeaderSize > Budget.idle,
                  "the answer must arrive OUTSIDE it, or a build that never re-arms would pass");
    static_assert(PerByte * 2 * Wire::ReplyHeaderSize < PerByte * Wire::ReplyHeaderSize + Budget.idle,
                  "and inside the window the pulse opens, or the fixed build would fail too");

    constexpr int Turns = 32; // comfortably more than the stream is long
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

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const start = clock.Now();
    dribble(&reactor, &clock, &connector.Log(), PerByte, Turns);

    auto const outcome = exchange.Run("127.0.0.1:6676", Wire::EncodeFetch("k"), {}, Budget);

    // Drained after the exchange stopped the reactor, so the driver reaches its own
    // `co_return` and frees its frame -- the same reason the dribble case above drains.
    (void) reactor.Drain();

    auto const& log = connector.Log();
    INFO("reads=" << log.reads << " closed=" << log.closed << " failure=" << static_cast<int>(outcome.transportFailure)
                  << " elapsed=" << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count()
                  << "ms");

    // The answer arrived, which it cannot have if the idle deadline fired at 8s.
    CHECK(outcome.kind == Cc::CacheOutcomeKind::Hit);

    // Asserted as "not the silence bound" rather than as `None`, and that is a
    // statement about the existing type rather than a hedge: `CacheOutcome` seeds
    // `transportFailure` to `Unreached` so that an exchange which never ran cannot read
    // as a hit, and the successful paths do not clear it -- the field is only ever
    // written on the `Transport` arm. So `None` is not what a hit carries today, while
    // `Silent` is exactly what a wrongly-fired idle bound would leave here.
    CHECK(outcome.transportFailure != Cc::TransportFailure::Silent);

    // And it really did outlive the bound rather than arriving early: without this the
    // case would also pass against a peer that answered in one byte.
    CHECK(clock.Now() - start > Budget.idle);
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
    // Dribbled, and that is what makes this case mean anything. A peer whose reads
    // resolve INLINE finishes the whole exchange before `Run()` is ever called, so
    // the reactor never takes a turn, so a deadline armed at `Now()` never gets to
    // fire -- and the case would pass against the very code it exists to reject.
    // Handing the reply over one byte per reactor turn, with the clock moving each
    // time, is what puts the exchange on the far side of a turn the timer would
    // have used.
    connector.DribbleReplies();

    // Comfortably more turns than `ReplyHeaderSize`, so the exchange stops because
    // it read a whole reply and not because the driver ran out of turns.
    constexpr int Turns = 32;
    auto dribble = [](TestReactor* loop, ManualClock* c, PeerLog* log, int turns) -> DetachedTask {
        for (int turn = 0; turn < turns; ++turn)
        {
            co_await ResumeOn { *loop };
            c->Advance(1s);
            if (log->live == nullptr || !log->live->DeliverOneByte())
                co_return;
        }
    };

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const start = clock.Now();
    dribble(&reactor, &clock, &connector.Log(), Turns);

    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Cc::ExchangeBudget { .total = 0ms });

    // Drained so the driver reaches its own `co_return` and frees its frame; see the
    // dribbling case above for why `Run()` cannot be relied on to do it.
    (void) reactor.Drain();

    auto const& log = connector.Log();
    INFO("reads=" << log.reads << " advanced="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - start).count() << "ms");
    // The reactor really did turn, and the clock really did move past where a
    // `Now() + 0` deadline would have sat -- which is the whole discriminator.
    CHECK(clock.Now() > start);
    CHECK(log.reads > 1);
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

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    CHECK(exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, {}).kind == Cc::CacheOutcomeKind::Miss);

    // A second run on the same instance is a programmer error, caught by the assert
    // in a debug build. Only the first is asserted here, because a death test for an
    // assertion is not portable and the guard's value is that it fires during
    // development rather than that a suite can observe it.
    CHECK(connector.Dials() == 1);
}

TEST_CASE("A peer that dies and a peer that is merely slow are recorded differently")
{
    // **This is the case that makes #247 worth anything.** Keepalive makes a
    // dispatch against a vanished host fail in seconds instead of at the compile
    // deadline minutes later -- and if both endings still produce the same recorded
    // answer, the only observable difference is that a build got slower somewhere
    // less often. Nobody can see that, and nobody can act on it.
    //
    // The two are indistinguishable at the socket: expiry CLOSES the connection, so
    // a caller looking at the error alone sees a broken socket either way. Only the
    // deadline knows which, which is why it now records it
    // (`SocketDeadlineTarget::expired`) instead of the caller inferring it from
    // elapsed time.
    //
    // Both halves in one case, deliberately. Either assertion alone passes under a
    // reason that is hardcoded to the value that half expects, and the defect being
    // guarded against is precisely that the two collapse.
    auto const runAgainst = [](bool die) {
        // Inside, so nothing has to be captured: clang refuses an implicit capture
        // of a `constexpr` object even where MSVC allows it, and a capture-default
        // here would exist only to satisfy the compiler.
        constexpr Cc::ExchangeBudget Budget {};

        ManualClock clock;
        TestReactor reactor { clock };
        ScriptedConnector connector;
        connector.Reply({}); // accepts, and answers nothing on its own
        // The clock moves in BOTH arms, and by the same mechanism. A dead peer
        // answers before the deadline can matter, so the jump changes nothing for
        // it -- but running the two arms under different fixtures would leave the
        // difference explainable by the fixture rather than by the peer, which is
        // the one thing this case must not allow.
        connector.AdvanceOnFirstRead(clock, Budget.total * 4);
        if (die)
            connector.DieMidExchange();

        Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
        return exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, Budget);
    };

    auto const died = runAgainst(true);
    auto const slow = runAgainst(false);

    // Same OUTCOME -- both answer "compile it locally", and an optional accelerator
    // must never fail a build.
    REQUIRE(died.kind == Cc::CacheOutcomeKind::Transport);
    REQUIRE(slow.kind == Cc::CacheOutcomeKind::Transport);

    // Different CAUSE, which is the whole point.
    CHECK(died.transportFailure == Cc::TransportFailure::PeerLost);
    CHECK(slow.transportFailure == Cc::TransportFailure::Expired);
    CHECK(died.transportFailure != slow.transportFailure);

    // And the cause survives into words an operator reads. Asserted through the
    // shared table rather than against a literal, so the sentences cannot drift from
    // the taxonomy -- but asserted as DIFFERENT, because a table that returned one
    // string for every row would satisfy every other check here.
    CHECK(Cc::DescribeTransportFailure(died.transportFailure) != Cc::DescribeTransportFailure(slow.transportFailure));
}

TEST_CASE("An endpoint that was never reached is not reported as a lost peer")
{
    // The third state, and it is not a pedantic one: "the address is wrong or that
    // machine is off" and "the machine was there and went away mid-compile" send an
    // operator to different places. `Unreached` is also the seeded default, so this
    // pins that an exchange which never ran cannot read as a peer that was contacted.
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector;
    connector.Refuse();

    Cc::ReactorExchange exchange { reactor, connector, Unwatched() };
    auto const outcome = exchange.Run("127.0.0.1:6674", Wire::EncodeFetch("k"), {}, {});

    CHECK(outcome.kind == Cc::CacheOutcomeKind::Transport);
    CHECK(outcome.transportFailure == Cc::TransportFailure::Unreached);
}

TEST_CASE("Every transport failure has words of its own")
{
    // A table that answered the same phrase twice would pass the pairwise check
    // above for the pair it happens to separate, and lose the distinction for the
    // next one added. Asserted over the whole enumeration instead, which is what
    // makes it hold for a row nobody has written yet.
    constexpr auto All = std::to_array({ Cc::TransportFailure::None,
                                         Cc::TransportFailure::Unreached,
                                         Cc::TransportFailure::PeerLost,
                                         Cc::TransportFailure::Expired,
                                         Cc::TransportFailure::Silent });

    std::vector<std::string_view> phrases;
    for (auto const failure: All)
    {
        auto const phrase = Cc::DescribeTransportFailure(failure);
        CHECK_FALSE(phrase.empty());
        phrases.push_back(phrase);
    }

    std::ranges::sort(phrases);
    CHECK(std::ranges::adjacent_find(phrases) == phrases.end());
}
