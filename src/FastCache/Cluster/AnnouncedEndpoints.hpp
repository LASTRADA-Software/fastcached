// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

/// @file AnnouncedEndpoints.hpp
/// Where every member's `0xFC` port answers, as members PROVED and announced it, turned into
/// what a leader proposes so the replicated record says the same.

namespace FastCache::Cluster
{

/// The latest `0xFC` endpoint each member announced, by the id it PROVED on that connection.
using AnnouncedEndpointMap = std::map<Consensus::NodeId, std::string, std::less<>>;

/// The members whose re-proposed record has not committed yet.
using MembersInFlight = std::set<Consensus::NodeId, std::less<>>;

/// What a leader should propose for members whose PROVEN announcements disagree with their records.
///
/// A pure function for `MembershipProposals`' reason: every rule below is a table of cases rather
/// than a cluster and a sleep. Its output is ordinary desires, so the proposal that follows is the
/// ordinary re-proposal of that member's record -- its seat kept (`MembershipProposals` keeps a
/// recorded seat), its key kept (`publicKey` absent keeps the recorded one).
///
/// It proposes nothing for:
/// - a member the state does not RECORD: an announcement admits nobody;
/// - an endpoint `ParseDialEndpoint` refuses: a record nobody can dial is worse than an old one;
/// - an endpoint the record already holds: re-proposing it would cost a log entry per announcement;
/// - a member with a change already in flight: at most one per member, so two announcements in
///   quick succession cannot race two proposals whose commit order nobody controls.
/// @param state The cluster's state as the leader last applied it.
/// @param announced The latest endpoint each member announced, by the id it proved.
/// @param inFlight The members whose re-proposed record has not committed yet.
/// @return One desire per member to re-propose: its recorded id and consensus endpoint, the
///         announced `0xFC` endpoint, and no key.
[[nodiscard]] std::vector<DesiredMember> AnnouncedEndpointDesires(ClusterState const& state,
                                                                  AnnouncedEndpointMap const& announced,
                                                                  MembersInFlight const& inFlight);

/// @p desired with each of @p announcements folded in: the announced endpoint replaces an existing
/// desire's when that desire has NO opinion about it, and an announcement for a member nothing else
/// desires is appended.
///
/// Folded rather than appended beside, because `MembershipProposals` proposes each desire that
/// differs, and two desires for one member are two whole records proposed in a fixed order -- the
/// second undoing whatever the first changed. A desire that ASSERTS an endpoint (a node's own record
/// of itself) outranks an announcement about it: that node is the authority on where it answers.
/// @param desired What the leader desires already.
/// @param announcements `AnnouncedEndpointDesires`' answer.
/// @return The merged desires, in @p desired order and then the appended ones.
[[nodiscard]] std::vector<DesiredMember> WithAnnouncedEndpoints(std::vector<DesiredMember> desired,
                                                                std::span<DesiredMember const> announcements);

} // namespace FastCache::Cluster
