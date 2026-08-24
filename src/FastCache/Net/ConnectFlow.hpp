// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IAsyncAddressResolver.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/NetError.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace FastCache
{

class IReactor;

namespace Detail
{

    /// One candidate dial attempt, supplied by the platform connector.
    ///
    /// `endpoint` is taken **by value**: this produces a coroutine, whose frame
    /// outlives the call expression, so a `ResolvedEndpoint const&` could dangle
    /// at the first suspend. A 144-byte copy per attempt is not worth a hazard.
    ///
    /// A function pointer plus an opaque state pointer rather than a
    /// `std::function`, matching `EpollFdHandler` and `IoAwaitable`'s suspend
    /// callback -- the house shape, and no allocation on a path that runs per
    /// dial.
    using DialStep = Task<SocketResult> (*)(void* state, ResolvedEndpoint endpoint, TimePoint deadline);

    /// The platform-free half of every connector.
    ///
    /// "Guard the host, budget the whole call, resolve, try every candidate in
    /// preference order, report the LAST failure" exists once and is shared by
    /// the blocking connector and all three reactor connectors. Each of those
    /// rules was a defect somewhere before it was a rule:
    ///
    /// - **An empty host is refused before the resolver is touched.** The shared
    ///   resolver is bind-shaped -- it passes `AI_PASSIVE` and turns an empty host
    ///   into the wildcard address -- which is exactly right for a bind and is not
    ///   something you can dial. Connecting to `0.0.0.0` reaches localhost on
    ///   Linux, so the failure would not even be loud.
    /// - **The budget covers the whole call**, resolution and every candidate
    ///   included. A host with both an AAAA and an A record used to be able to
    ///   take twice what the caller asked for, which is a bound that is not one.
    /// - **Every candidate is tried, and the LAST failure is what is reported.**
    ///   A peer whose name has both records, on a machine with no IPv6 route, is
    ///   reachable through the second -- and a dial that gave up after the first
    ///   would report a healthy peer as down for a reason that is about this
    ///   machine.
    ///
    /// @param resolver Name resolution seam; must not be null.
    /// @param reactor Where a suspended resolution resumes. Null means the caller
    ///        is on a thread that may block, and nothing here will suspend.
    /// @param clock Source of the deadline. Injected so the total-budget rule is a
    ///        `ManualClock` unit test rather than a sleep.
    /// @param host Target host, unbracketed. By value, for the coroutine-frame
    ///        reason `IConnector::Connect` documents.
    /// @param port Target port in host byte order.
    /// @param connectTimeout Total budget for the whole call; non-positive means
    ///        no deadline of ours, leaving the platform's own.
    /// @param dial Per-candidate attempt.
    /// @param dialState Opaque pointer handed to `dial`.
    /// @return The connected socket, or why no candidate produced one.
    [[nodiscard]] Task<SocketResult> RunConnectFlow(IAsyncAddressResolver* resolver,
                                                    IReactor* reactor,
                                                    IClock* clock,
                                                    std::string host,
                                                    std::uint16_t port,
                                                    std::chrono::milliseconds connectTimeout,
                                                    DialStep dial,
                                                    void* dialState);

} // namespace Detail

} // namespace FastCache
