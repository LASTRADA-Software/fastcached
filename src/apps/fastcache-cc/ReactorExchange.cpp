// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"
#include "ReactorExchange.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/PlatformConnector.hpp>
#include <FastCache/Net/SocketDeadline.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace FastCache::Cc
{

namespace
{

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

            *out = co_await ExchangeFramed(client.get(), notice, std::move(frame), std::move(credential));

            // Asked of the TIMER, never inferred from elapsed time. Both endings
            // arrive here as a broken socket, and only the timer knows which one it
            // was: `expired` means this side gave up, and its absence means the
            // connection died on its own -- which, on a keepalive-armed dial, is a
            // host that went away rather than a compile that was slow.
            if (out->kind == CacheOutcomeKind::Transport)
                out->transportFailure = target.expired ? TransportFailure::Expired : TransportFailure::PeerLost;
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
