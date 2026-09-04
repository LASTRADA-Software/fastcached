// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"
#include "ReactorExchange.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/PlatformConnector.hpp>
#include <FastCache/Net/SocketDeadline.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>

namespace FastCache::Cc
{

namespace
{

    /// The idle half of an exchange's budget: a deadline thrown away and armed again
    /// every time the peer proves it is still working.
    ///
    /// **A sliding bound, which is a different thing from the fixed one beside it.**
    /// `budget.total` is armed once and answers *has this exchange taken too long*;
    /// this answers *has anything happened lately*, and only the second can be short on
    /// a dispatched compile (#245).
    ///
    /// **Re-armed by destroying and rebuilding the timer**, because `DeadlineTimer`
    /// captures its deadline in its own coroutine frame and is deliberately immovable:
    /// destroying one disarms it and takes its wait straight back off the reactor
    /// (`CancelPending`), and constructing the next arms the following window. Those are
    /// two operations that type already gets exactly right, and a `Rearm` on it would
    /// put a second writer on an instant whose whole safety argument is that a late fire
    /// is harmless. The cost is one coroutine frame per pulse -- one per
    /// `DefaultProgressInterval` per dispatched compile -- against the minutes it is
    /// measuring.
    ///
    /// **It cannot go through `ArmSocketDeadline`, and that is a language fact rather
    /// than a choice**: that helper hands back a `std::optional<DeadlineTimer>`, which an
    /// immovable element makes initialize-only -- there is no assignment, and no
    /// `emplace` that can take one. So what is shared with it is the
    /// `SocketDeadlineTarget` type and its ordering contract (record the firing BEFORE
    /// the close, so a caller resumed by that close never sees a shut socket with no
    /// reason attached), and the *non-positive means unbounded* rule is asked of
    /// `ExchangeBudget::BoundsIdle()` -- the one place this project states it, and the
    /// reason that predicate lives on the type rather than at its consumers.
    ///
    /// Everything here runs on the reactor thread: `MovedForward` is called from the
    /// exchange coroutine, which this reactor resumes. So the timer is built and torn
    /// down on the thread that owns it, as `DeadlineTimer` requires.
    class IdleWindow final: public IExchangeLiveness
    {
      public:
        /// @param reactor Where the deadline is armed; must outlive this.
        /// @param budget The exchange's budget; only its idle bound is read.
        /// @param target What to close on expiry and where the firing is recorded; must
        ///        outlive this.
        IdleWindow(IReactor& reactor, ExchangeBudget budget, SocketDeadlineTarget* target) noexcept:
            _reactor { &reactor },
            _budget { budget },
            _target { target }
        {
        }

        /// @copydoc IExchangeLiveness::MovedForward
        ///
        /// Also how the FIRST window is armed: an exchange that has just connected has
        /// moved forward by definition, and giving the arming one spelling means there is
        /// no start-up path that could arm it differently from the pulses.
        void MovedForward() noexcept override
        {
            if (!_budget.BoundsIdle())
                return; // Unbounded, and it stays unbounded on every pulse.

            // Destroyed before the next is built rather than leaving that to `emplace`:
            // `emplace` does destroy first, and saying so keeps "two timers are never
            // armed at once" from being a reading of the standard somebody has to do at
            // this call site.
            _timer.reset();
            _timer.emplace(*_reactor, _reactor->Clock().Now() + _budget.idle, &OnSilence, _target);
        }

      private:
        /// Close the socket and say that the silence bound is what did it.
        ///
        /// Recorded BEFORE the close, which is `SocketDeadlineTarget`'s own rule: the
        /// close resumes whoever is parked on that socket, so a flag set afterwards
        /// would be read by a caller that has already decided what happened.
        /// @param state The `SocketDeadlineTarget`, as a `void*`.
        static void OnSilence(void* state) noexcept
        {
            auto& fired = *static_cast<SocketDeadlineTarget*>(state);
            fired.expired = true;
            fired.socket->Close();
        }

        IReactor* _reactor;
        ExchangeBudget _budget;
        SocketDeadlineTarget* _target;
        std::optional<DeadlineTimer> _timer;
    };

    /// The whole exchange, as one coroutine.
    ///
    /// A free function taking raw pointers rather than a member or a capturing
    /// lambda: a coroutine's frame outlives the expression that created it.
    /// @param reactor Loop to stop when the exchange ends.
    /// @param connector How to dial.
    /// @param notice Where an ignored credential is reported; not owned.
    /// @param hostPort Where to dial. By value, for the coroutine-frame reason.
    /// @param frame The request; moved in so a STORE's object file is not copied.
    /// @param credential Presented with the request.
    /// @param budget The two deadlines.
    /// @param out Where to leave the outcome.
    DetachedTask RunExchange(IReactor* reactor,
                             IConnector* connector,
                             CredentialNotice* notice,
                             std::string hostPort,
                             std::vector<std::byte> frame,
                             Credential credential,
                             ExchangeBudget budget,
                             CacheOutcome* out)
    {
        // The budget carries both: how long the dial may take, and whether this is the
        // exchange that must notice a peer whose host went away. See
        // `ExchangeBudget::keepAlive` for why one number cannot answer both questions.
        auto client = co_await DialEndpoint(
            connector, hostPort, DialOptions { .connectTimeout = budget.connect, .keepAlive = budget.keepAlive });
        if (client == nullptr)
        {
            // Unreachable is a transport failure, which every caller answers by
            // compiling. An optional accelerator must never be able to fail a build.
            //
            // `Unreached` and not `PeerLost`: nothing was ever connected here, so
            // there is no peer to have lost. The default-constructed outcome already
            // says so; stated rather than relied upon, since the two live in
            // different headers.
            *out = CacheOutcome {};
            out->transportFailure = TransportFailure::Unreached;
            reactor->Stop();
            co_return;
        }

        {
            // Bounds the exchange by CLOSING the socket rather than by stopping the
            // reactor, and the difference is a leak. Stopping the reactor would leave
            // this coroutine parked on a read nobody will ever complete; closing
            // completes it, so the task reaches its own end and frees its own frame.
            //
            // An unbounded budget arms NOTHING, which is what `FASTCACHE_TIMEOUT_MS=0`
            // has always been documented to mean. That rule now lives in
            // `ArmSocketDeadline`, which the node's upstream shares (#248) -- it was
            // implemented here and again in `RemoteUpstream`, and only one of the two
            // had a regression test.
            SocketDeadlineTarget target { .socket = client.get() };
            auto const bound = ArmSocketDeadline(reactor, budget.total, &target);

            // The idle bound is a SECOND deadline on the same socket, and the two are
            // not redundant: `total` asks how long this exchange may legitimately take
            // and `idle` asks how long since anything happened. On a dispatched compile
            // the first must stay minutes long, which is what left a worker that had
            // stopped making progress unnoticed for the whole of it (#245).
            //
            // Armed here rather than after the request goes out, so a peer that accepts
            // the connection and then says nothing at all is inside it from the start.
            SocketDeadlineTarget silence { .socket = client.get() };
            IdleWindow idle { *reactor, budget, &silence };
            idle.MovedForward();

            *out = co_await ExchangeFramed(client.get(), notice, std::move(frame), std::move(credential), &idle);

            // Asked of the TIMER, never inferred from elapsed time. Both endings
            // arrive here as a broken socket, and only the timer knows which one it
            // was: `expired` means this side gave up, and its absence means the
            // connection died on its own -- which, on a keepalive-armed dial, is a
            // host that went away rather than a compile that was slow.
            //
            // Three timers now, so three answers. The idle one is asked FIRST: it is the
            // more specific fact and the two cannot both be true in a way that matters
            // -- a silence that ran out inside a total that also ran out is still a peer
            // that went quiet, and reporting the total would name the bound the operator
            // is least able to act on.
            if (out->kind == CacheOutcomeKind::Transport)
            {
                if (silence.expired)
                    out->transportFailure = TransportFailure::Silent;
                else
                    out->transportFailure = target.expired ? TransportFailure::Expired : TransportFailure::PeerLost;
            }
        }

        client->Close();
        reactor->Stop();
        co_return;
    }

} // namespace

ReactorExchange::ReactorExchange(IReactor& reactor, IConnector& connector, CredentialNotice& notice) noexcept:
    _reactor { reactor },
    _connector { connector },
    _notice { notice }
{
}

CacheOutcome ReactorExchange::Run(std::string_view hostPort,
                                  std::vector<std::byte> frame,
                                  Credential credential,
                                  ExchangeBudget budget)
{
    // Asserted rather than trusted. A reused instance would return immediately from
    // `Run()` below -- the reactor's stop flag is never cleared -- and report a
    // transport failure for an exchange that never happened, which the launcher would
    // answer by compiling locally. Every build would get slower and nothing would say
    // why. See the class note.
    assert(!_used && "a ReactorExchange runs once; its reactor cannot be restarted");
    _used = true;

    // Seeded with the answer an exchange that never runs must give, so a reactor that
    // returns without the task having completed cannot be read as a hit.
    // Default-constructed, which IS a transport failure -- `CacheOutcomeKind` is
    // ordered so the safe answer is the default. Seeded here rather than left
    // uninitialised so a reactor that returns without the task having completed
    // cannot be read as a hit.
    CacheOutcome outcome;

    RunExchange(&_reactor,
                &_connector,
                &_notice,
                std::string { hostPort },
                std::move(frame),
                std::move(credential),
                budget,
                &outcome);
    _reactor.Run();
    return outcome;
}

CacheOutcome RunOneExchange(std::string_view hostPort,
                            CredentialNotice& notice,
                            std::vector<std::byte> frame,
                            Credential credential,
                            ExchangeBudget budget)
{
    SteadyClock clock;
    PlatformReactor reactor { clock };

    // Two threads at most, started only if a hostname is actually looked up. The
    // launcher's endpoint is nearly always a literal, which never reaches the pool --
    // so the common case pays nothing for the one that matters.
    ThreadedAddressResolver resolver;
    PlatformConnector connector { reactor, resolver, clock };

    ReactorExchange exchange { reactor, connector, notice };
    auto outcome = exchange.Run(hostPort, std::move(frame), std::move(credential), budget);

    // Stopped before the reactor goes out of scope: a worker handing a result back
    // Submits to a reactor whose `Run` has already returned would queue a handle
    // nobody resumes, which is a leaked coroutine frame.
    resolver.Stop();
    return outcome;
}

namespace
{
    /// The exchange that talks over real sockets.
    ///
    /// A reactor per exchange, not a blocking socket, and that is what the budget
    /// rests on: `SO_RCVTIMEO` bounds one `recv`, so a worker dribbling a byte
    /// before each expiry could hold a build forever. `RunOneExchange` arms a
    /// `DeadlineTimer` that CLOSES the socket, which bounds the whole conversation.
    ///
    /// Stateless, so a dispatch builds and tears down a reactor and a resolver three
    /// times rather than reusing one connector, as the blocking dialler this replaced
    /// did. That is a deliberate consequence rather than an oversight: a
    /// `ReactorExchange` runs exactly once (its reactor's stop flag is never
    /// cleared), so a shared one would perform the LEASE and silently skip the
    /// compile. The cost is an `epoll_create1`/`eventfd` pair per verb -- microseconds
    /// against the 45 ms preprocess this path has already paid, and against the
    /// seconds of remote compile it exists to buy.
    class TcpExchange final: public IEndpointExchange
    {
      public:
        /// @param notice Where an ignored credential is reported; must outlive this.
        explicit TcpExchange(CredentialNotice& notice) noexcept:
            _notice { notice }
        {
        }

        [[nodiscard]] CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Credential const& credential,
                                            ExchangeBudget budget) override
        {
            return RunOneExchange(hostPort, _notice, std::move(frame), credential, budget);
        }

      private:
        CredentialNotice& _notice;
    };
} // namespace

std::unique_ptr<IEndpointExchange> MakeTcpExchange(CredentialNotice& notice)
{
    return std::make_unique<TcpExchange>(notice);
}

} // namespace FastCache::Cc
