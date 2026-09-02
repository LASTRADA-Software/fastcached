// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Net/ConnectFlow.hpp>

#include <cstdint>
#include <format>
#include <utility>

namespace FastCache::Detail
{

namespace
{

    /// The deadline this whole call must finish inside.
    ///
    /// A non-positive budget means the caller did not ask for one, and
    /// `TimePoint::max()` is how "no deadline" is spelled everywhere else here --
    /// a default-constructed TimePoint would mean "already expired", which is the
    /// `NoExpiry` mistake the cache layer records having made once.
    [[nodiscard]] TimePoint BudgetDeadline(IClock* clock, std::chrono::milliseconds connectTimeout) noexcept
    {
        if (clock == nullptr || connectTimeout <= std::chrono::milliseconds::zero())
            return TimePoint::max();
        return clock->Now() + connectTimeout;
    }

} // namespace

Task<SocketResult> RunConnectFlow(IAsyncAddressResolver* resolver,
                                  IReactor* reactor,
                                  IClock* clock,
                                  std::string host,
                                  std::uint16_t port,
                                  DialOptions options,
                                  DialStep dial,
                                  void* dialState)
{
    // Refused before the resolver is touched. An empty host resolves to the
    // wildcard address, which is a bind target and not a dial target -- and
    // connecting to it reaches localhost on Linux rather than failing, so the
    // mistake would be silent.
    if (host.empty())
        co_return std::unexpected(NetError { .code = NetErrorCode::AddressNotAvail,
                                             .systemCode = 0,
                                             .context = std::format("no host to dial for port {}", port) });

    auto const deadline = BudgetDeadline(clock, options.connectTimeout);

    auto resolved = co_await resolver->Resolve(host, port, reactor);
    if (!resolved.has_value())
        co_return std::unexpected(resolved.error());
    if (resolved->empty())
        co_return std::unexpected(ResolveFailure(host, port, "no usable address"));

    // Seeded so a resolver that hands back an empty list -- which the guard above
    // makes unreachable, but which a future resolver could -- still produces an
    // error naming the endpoint rather than a default-constructed one.
    auto failure = NetError { .code = NetErrorCode::AddressNotAvail,
                              .systemCode = 0,
                              .context = std::format("no usable address for {}:{}", host, port) };

    auto remainingCandidates = resolved->size();
    for (auto const& candidate: *resolved)
    {
        // Checked per candidate rather than once, which is what makes the budget
        // a total: without this the second candidate would start a fresh attempt
        // after the first had already consumed the whole allowance.
        if (clock != nullptr && clock->Now() >= deadline)
        {
            failure = NetError { .code = NetErrorCode::Timeout,
                                 .systemCode = 0,
                                 .context = std::format(
                                     "connect to {}:{} timed out after {} ms", host, port, options.connectTimeout.count()) };
            break;
        }

        // Each candidate gets an equal share of what is LEFT, not the whole of
        // it. Both halves matter and they pull against each other:
        //
        // - Handing every candidate the full budget is what the old
        //   per-candidate timeout did, and it means a caller asking for two
        //   seconds can wait four. A bound that multiplies by the number of
        //   addresses a name happens to have is not a bound.
        // - Handing the FIRST candidate the whole remaining budget defeats the
        //   fallback entirely whenever that candidate black-holes rather than
        //   refuses -- which is the ordinary case for an AAAA on a machine with
        //   no IPv6 route, and the exact situation trying every candidate exists
        //   for. It is also not hypothetical: a closed loopback port is silently
        //   dropped behind a host firewall and reset without one, so the same
        //   dial consumes its whole allowance on one machine and returns at once
        //   on the next.
        //
        // Dividing gives the caller the total it asked for and still leaves every
        // candidate a real chance. A candidate that finishes early hands what it
        // did not use to the ones after it, because the share is recomputed from
        // the clock each time rather than fixed up front.
        auto candidateDeadline = deadline;
        if (clock != nullptr && deadline != TimePoint::max() && remainingCandidates > 1)
        {
            auto const left = deadline - clock->Now();
            candidateDeadline = clock->Now() + (left / static_cast<std::int64_t>(remainingCandidates));
        }
        --remainingCandidates;

        auto attempt = co_await dial(dialState, candidate, candidateDeadline, options.keepAlive);
        if (attempt.has_value())
            co_return std::move(*attempt);

        // The LAST failure wins: an AAAA that cannot be routed followed by an A
        // that can is a healthy host, and reporting the first would describe this
        // machine's routing rather than the peer.
        failure = std::move(attempt.error());
    }

    co_return std::unexpected(std::move(failure));
}

} // namespace FastCache::Detail
