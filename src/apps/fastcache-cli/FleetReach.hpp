// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache::Cli
{

/// @file FleetReach.hpp
/// How this client reaches the fleet: at the node that leads it, holding the dashboard
/// credential. Two rules the fleet's two readers -- `live-stats fleet`, which subscribes, and
/// `fleet`, which reads once -- would otherwise each spell (#1391).

/// How many `NotLeader` redirects one request follows before it counts as failed.
///
/// Two, as a lease and a registration bound theirs: a leader that moved while the redirect was in
/// flight is one more hop, and a pair of nodes each naming the other stale leader is not a leader
/// at all.
inline constexpr int MaxLeaderRedirects = 2;

/// What a refusal means for a request that has already followed some redirects.
///
/// **A PRIVATE enum**: nothing transmits it and nothing stores it.
enum class LeaderHopKind : std::uint8_t
{
    NotARedirect, ///< Not a `NotLeader` naming an address: relay it as the refusal it is.
    Follow,       ///< Ask the leader it names.
    Exhausted,    ///< It names a leader, and this request has followed as many as it may.
};

/// A decision about one refusal, with where it points.
struct LeaderHop
{
    LeaderHopKind kind { LeaderHopKind::NotARedirect }; ///< What to do.
    Endpoint next {};                                   ///< Where to ask, for `Follow`.
    std::string_view named {}; ///< The leader the refusal named, for `Follow` and `Exhausted`; borrows it.
};

/// Decide what one refusal means for a request that has followed @p hopsTaken redirects.
///
/// The ONE rule both readers follow: a `NotLeader` is an instruction when its message parses as an
/// address (`LeaderRedirectTarget`), bounded by `MaxLeaderRedirects`; anything else -- an election
/// with no leader known included -- is relayed rather than followed.
/// @param code What the node refused with.
/// @param message What it said; borrowed by the result.
/// @param hopsTaken How many redirects this request has already followed.
/// @return The decision.
[[nodiscard]] inline LeaderHop DecideLeaderHop(CompileCacheWire::ErrorCode code, std::string_view message, int hopsTaken)
{
    auto const leader = LeaderRedirectTarget(code, message);
    if (!leader.has_value())
        return {};
    auto const parsed = ParseDialEndpoint(*leader);
    if (!parsed.has_value())
        return {};
    if (hopsTaken >= MaxLeaderRedirects)
        return LeaderHop { .kind = LeaderHopKind::Exhausted, .next = {}, .named = *leader };
    return LeaderHop { .kind = LeaderHopKind::Follow,
                       .next = Endpoint { .host = parsed->first, .port = parsed->second },
                       .named = *leader };
}

/// What this client can change about a fleet refused `Unauthenticated`, as the tail of a note.
///
/// **Turned on whether the request carried the credential**, never a restatement of the node's
/// words: without one, where to pass it; with one, that it was not accepted -- which is true both of
/// a wrong secret and of a leader that names no `--dashboard-token-file` and serves its own machine
/// only, two cases one code cannot tell apart and the node's detail does.
/// @param presented Whether the request carried a dashboard credential.
/// @return The remedy.
[[nodiscard]] constexpr std::string_view DashboardCredentialRemedy(bool presented) noexcept
{
    return presented ? "; the dashboard credential from --dashboard-token-file was not accepted"
                     : "; present the dashboard credential with --dashboard-token-file";
}

} // namespace FastCache::Cli
