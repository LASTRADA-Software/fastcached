// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>

#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
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

/// The Raft endpoint a member's record should hold once it announced @p announced: the announced host
/// on the recorded Raft port when the recorded Raft and `0xFC` hosts are the same host, else the
/// recorded Raft endpoint unchanged.
///
/// **The host-coupling rule**, and the reason no announcement carries a Raft endpoint of its own: a
/// NODE-ANNOUNCE states the `0xFC` endpoint alone, and adding a field is a fleet flag day. A node
/// whose two endpoints share a host -- every node advertising the routed address, which is the
/// default -- moved BOTH when it roamed, so its Raft endpoint follows; a node that pins them apart
/// said where each answers and is never coupled. An endpoint `ParseDialEndpoint` refuses, on either
/// side of the record or in the announcement, couples nothing: a learner's empty Raft endpoint stays
/// empty. Hosts compare through `SameHost`, and the result is joined by `FormatHostPort`, so an IPv6
/// literal keeps its brackets.
/// @param recorded The member's record.
/// @param announced The `0xFC` endpoint it announced.
/// @return The Raft endpoint its record should hold.
[[nodiscard]] std::string CoupledRaftEndpoint(ClusterMember const& recorded, std::string_view announced);

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
/// @return One desire per member to re-propose: its recorded id, its consensus endpoint as
///         `CoupledRaftEndpoint` moves it, the announced `0xFC` endpoint, and no key.
[[nodiscard]] std::vector<DesiredMember> AnnouncedEndpointDesires(ClusterState const& state,
                                                                  AnnouncedEndpointMap const& announced,
                                                                  MembersInFlight const& inFlight);

/// @p desired with each of @p announcements folded in: the announced endpoint replaces an existing
/// desire's when that desire has NO opinion about it, and an announcement for a member nothing else
/// desires is appended. A COUPLED announcement (`SpeaksForRaftEndpoint`) replaces that desire's Raft
/// endpoint as well: the desire with no `0xFC` opinion is discovery's, whose Raft endpoint is what a
/// beacon said BEFORE the move, and keeping it would re-propose the old address beside the new one.
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

/// Whether @p announcement speaks for its member's Raft endpoint too: its Raft and `0xFC` endpoints
/// name the same host, which `CoupledRaftEndpoint` produces exactly for a member it coupled.
///
/// A learner's empty Raft endpoint, and a member that pins its two endpoints apart, speak for none.
/// @param announcement One of `AnnouncedEndpointDesires`' answers.
/// @return True when its Raft endpoint is the announced host's.
[[nodiscard]] bool SpeaksForRaftEndpoint(DesiredMember const& announcement);

/// @p held with each COUPLED announcement's Raft endpoint adopted by the desire for that member that
/// has no `0xFC` opinion -- discovery's, which a beacon stated before the member moved.
///
/// What a leader KEEPS, where `WithAnnouncedEndpoints` is what one pass proposes. An announcement
/// stops producing a desire once its record commits, and a held discovery desire still naming the old
/// Raft endpoint would then re-propose that address over the agreed one -- a member moved, and moved
/// straight back, one pass later. The newer word replaces the older, which is the rule `Desire`
/// already keeps for two beacons; a later beacon replaces this in turn. The `0xFC` endpoint is NOT
/// adopted: discovery's absent opinion is what lets the next announcement fold in at all.
/// @param held What the leader holds desired.
/// @param announcements `AnnouncedEndpointDesires`' answer.
/// @return @p held, the coupled Raft endpoints adopted.
[[nodiscard]] std::vector<DesiredMember> WithAnnouncedRaftEndpoints(std::vector<DesiredMember> held,
                                                                    std::span<DesiredMember const> announcements);

} // namespace FastCache::Cluster
