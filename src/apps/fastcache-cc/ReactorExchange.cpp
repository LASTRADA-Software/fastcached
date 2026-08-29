// SPDX-License-Identifier: Apache-2.0
#include "EndpointDial.hpp"
#include "ReactorExchange.hpp"

#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/PlatformConnector.hpp>
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

    /// The whole exchange, as one coroutine.
    ///
    /// A free function taking raw pointers rather than a member or a capturing
    /// lambda: a coroutine's frame outlives the expression that created it.
    /// @param reactor Loop to stop when the exchange ends.
    /// @param connector How to dial.
    /// @param hostPort Where to dial. By value, for the coroutine-frame reason.
    /// @param frame The request; moved in so a STORE's object file is not copied.
    /// @param credential Presented with the request.
    /// @param budget The two deadlines.
    /// @param out Where to leave the outcome.
    DetachedTask RunExchange(IReactor* reactor,
                             IConnector* connector,
                             std::string hostPort,
                             std::vector<std::byte> frame,
                             Credential credential,
                             ExchangeBudget budget,
                             CacheOutcome* out)
    {
        auto client = co_await DialEndpoint(connector, hostPort, budget.connect);
        if (client == nullptr)
        {
            // Unreachable is a transport failure, which every caller answers by
            // compiling. An optional accelerator must never be able to fail a build.
            *out = CacheOutcome {};
            reactor->Stop();
            co_return;
        }

        {
            // Bounds the exchange by CLOSING the socket rather than by stopping the
            // reactor, and the difference is a leak. Stopping the reactor would leave
            // this coroutine parked on a read nobody will ever complete; closing
            // completes it, so the task reaches its own end and frees its own frame.
            //
            // A non-positive budget arms NOTHING, which is what `FASTCACHE_TIMEOUT_MS=0`
            // has always been documented to mean. Left to the arithmetic it would put
            // the deadline at `Now()` and abort every exchange on the reactor's next
            // turn -- a knob that reads as "turn the ceiling off" and turns the cache
            // off instead, silently, because every caller answers a Transport failure
            // by compiling. `std::optional` rather than a sentinel deadline: a timer
            // that is not armed is the honest spelling of no bound.
            std::optional<DeadlineTimer> bound;
            if (budget.total > std::chrono::milliseconds::zero())
                bound.emplace(
                    *reactor,
                    reactor->Clock().Now() + budget.total,
                    [](void* socket) { static_cast<ISocket*>(socket)->Close(); },
                    client.get());

            *out = co_await ExchangeFramed(client.get(), std::move(frame), std::move(credential));
        }

        client->Close();
        reactor->Stop();
        co_return;
    }

} // namespace

ReactorExchange::ReactorExchange(IReactor& reactor, IConnector& connector) noexcept:
    _reactor { reactor },
    _connector { connector }
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

    RunExchange(&_reactor, &_connector, std::string { hostPort }, std::move(frame), std::move(credential), budget, &outcome);
    _reactor.Run();
    return outcome;
}

CacheOutcome RunOneExchange(std::string_view hostPort,
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

    ReactorExchange exchange { reactor, connector };
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
    class TcpExchange final: public IEndpointExchange
    {
      public:
        [[nodiscard]] CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Credential const& credential,
                                            ExchangeBudget budget) override
        {
            return RunOneExchange(hostPort, std::move(frame), credential, budget);
        }
    };
} // namespace

std::unique_ptr<IEndpointExchange> MakeTcpExchange()
{
    return std::make_unique<TcpExchange>();
}

} // namespace FastCache::Cc
