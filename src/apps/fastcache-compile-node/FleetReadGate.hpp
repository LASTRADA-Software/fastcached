// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// @file FleetReadGate.hpp
/// Who may read the fleet over `0xFC`, decided once for the two verbs that serve it: the fleet
/// subject of `Subscribe`, and `FleetText` (#1391).
///
/// **One decision, and each verb maps it to its own counters.** The fleet map is behind the
/// dashboard credential on `/fleet`, and a follower's registry is a fraction presented as the
/// whole, so both verbs ask the same three questions in the same order. Two copies of that order
/// are two places for a later edit to put leadership before the credential -- which tells a caller
/// without it where the leader is -- in one of them.

/// What `DecideFleetRead` concluded.
///
/// **A PRIVATE enum**: nothing transmits it and nothing stores it, so it states no ordinals.
enum class FleetReadDecision : std::uint8_t
{
    Admitted,        ///< Read the fleet here.
    NoScheduler,     ///< This node runs no scheduler, so it has no fleet; the fleet is served elsewhere.
    Unauthenticated, ///< No dashboard credential, or a remote caller while none is configured.
    NotLeader,       ///< This node follows; the leader is elsewhere.
};

/// A decision and the words a refusal carries.
struct FleetReadVerdict
{
    FleetReadDecision decision { FleetReadDecision::Unauthenticated }; ///< What was decided.

    /// For a person, beside a refusal -- except under `NotLeader`, where it IS the leader's
    /// endpoint, or empty during an election, because a client parses it and follows it. Empty
    /// when admitted.
    std::string detail {};
};

/// Decide whether @p peer may read the fleet here.
///
/// The order is the rule: no scheduler first, since there is then nothing to guard; the credential
/// BEFORE leadership, so a caller without it is not told where the leader is; leadership last.
/// @param leadership Who leads, or nullopt when this node runs no scheduler.
/// @param dashboard The dashboard credential; a default one when no token file is named, and then
///        the fleet is served to this machine only.
/// @param token The credential the request carried, or empty.
/// @param peer The caller's host, as the kernel reported it.
/// @return The decision, with its words.
[[nodiscard]] FleetReadVerdict DecideFleetRead(std::optional<LiveLeadership> const& leadership,
                                               AdminCredential const& dashboard,
                                               std::string_view token,
                                               std::string_view peer);

} // namespace FastCache::Node
