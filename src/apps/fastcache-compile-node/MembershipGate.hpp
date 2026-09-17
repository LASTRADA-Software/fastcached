// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "PeerIdentity.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/LiveWatcher.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// @file MembershipGate.hpp
/// What a `0xFC` surface answers a caller its membership oracle did not admit.
///
/// **One function because there are now TWO ways not to be a member**, and five surfaces
/// spelled the one way by hand: `Classify(peer) == Member` or refuse. Five copies of a
/// one-armed decision were exactly as correct as one, which is why they were fine --
/// until a second arm arrived (#1309) and five files each had to grow it. That is the
/// shape this repository records as a table in disguise: the surfaces differ by a ROW and
/// a SENTENCE, so those are parameters and the decision is written once.
///
/// The alternative was a sixth copy per site, and the cost is not the typing. A surface
/// that was missed keeps compiling, keeps refusing the host correctly, and reports it as
/// a stranger -- a wrong diagnosis with nothing to notice, in a counter an operator reads
/// exactly when something has gone wrong.

/// The refusal a host the cluster has FORGOTTEN gets, wherever it knocks.
///
/// One row for every surface: see `NodeRequestsRefusedHostForgotten` for why the diagnosis
/// belongs to the host rather than to the door.
///
/// `NotAMember` on the wire, and the same code a stranger gets, because a CLIENT acts on
/// the two identically -- `fastcache-cc` steps over the refusal and compiles locally
/// either way, and a code of its own would be an unknown one to every launcher already
/// deployed. One code, two counters, which is the split `SurfaceRefusal` exists to hold.
inline constexpr Cc::SurfaceRefusal HostForgotten {
    .code = CompileCacheWire::ErrorCode::NotAMember,
    .counter = IMetricsSink::Counter::NodeRequestsRefusedHostForgotten,
};

/// What a forgotten host is told, in one place because it is one refusal.
///
/// It names the ACT and not a flag. A sentence naming the spelling an operator types is a
/// sentence that has to be revisited when the spelling arrives, and a remedy that has
/// gone stale is worse than a general one: it is the part of a guard nobody tests and the
/// only part most people read.
inline constexpr std::string_view HostForgottenWhy =
    "this fleet was told to forget this host; it is served nothing here until an operator admits it again";

/// Answer one already-taken verdict, or nullopt when it admits the caller.
///
/// Split out when the second way of ASKING arrived (#1428): the two spellings below differ
/// only in what they ask about -- a connection, or an address -- and the answer they give is
/// one decision that must not be written twice.
/// @param verdict What the fold concluded.
/// @param metrics Where the refusal is counted.
/// @param stranger What THIS surface answers a host nobody listed.
/// @param strangerWhy The words that ride with it.
/// @return The refusal to answer, or nullopt when the caller is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> AnswerMembership(Distributed::Membership verdict,
                                                                            IMetricsSink& metrics,
                                                                            Cc::SurfaceRefusal stranger,
                                                                            std::string_view strangerWhy)
{
    switch (verdict)
    {
        case Distributed::Membership::Member:
            return std::nullopt;
        case Distributed::Membership::Forgotten:
            return Cc::Refuse(metrics, HostForgotten, HostForgottenWhy);
        case Distributed::Membership::Outsider:
        case Distributed::Membership::Last:
            break;
    }

    // `Outsider`, and anything a switch cannot name: a gate fails closed. `Last` is not a
    // verdict and reaching it would be a bug, so it is refused rather than admitted --
    // the direction where being wrong costs a build instead of a machine.
    return Cc::Refuse(metrics, stranger, strangerWhy);
}

/// Refuse @p peer unless this node's oracle admits it, or unless this CONNECTION proved the
/// cluster key.
///
/// **The proof is folded here and nowhere else**, which is what makes *a key holder is a
/// member* true at every gate that asks this question rather than at the ones somebody
/// remembered (#1428). `Distributed::ExplainConnection` is the fold, on the same
/// `PrecedenceOf` the address routes are composed with -- so a forgotten host stays refused
/// and a connection that proved nothing gets exactly what the oracle answered.
///
/// @param membership The node's oracle, bound once by the surface -- never re-asked for,
///        which is what makes a removal reach a running surface at all.
/// @param metrics Where the refusal is counted.
/// @param peer The caller: the host the kernel reported, and what this connection proved.
/// @param stranger What THIS surface answers a host nobody listed: its own row and its
///        own sentence, because stranger traffic at each door is a different story.
/// @param strangerWhy The words that ride with it.
/// @return The refusal to answer, or nullopt when the caller is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> RefuseUnlessMember(
    Distributed::IMembershipOracle const& membership,
    IMetricsSink& metrics,
    PeerIdentity const& peer,
    Cc::SurfaceRefusal stranger,
    std::string_view strangerWhy)
{
    auto const decision = Distributed::ExplainConnection(membership, peer.host, peer.provenNodeId.has_value());
    return AnswerMembership(decision.verdict, metrics, stranger, strangerWhy);
}

/// Refuse @p watcher unless this node's oracle admits it, or unless its CONNECTION proved the
/// cluster key.
///
/// The same fold as the overload above, reached from the live-stream path, where what is
/// available is a `LiveWatcher` rather than the connection's full identity -- narrower on
/// purpose, and narrower in the one field a gate could not have checked anyway.
///
/// **It is an overload rather than a `(host, bool)` parameter pair deliberately.** A bare
/// `bool` argument at a call site says nothing about which question it answers, and this
/// header already carries the rule that a claim must not be spellable the way a default is.
/// Two named types, one fold, and no call site that could pass the wrong one silently.
///
/// **Two gates reach this for ONE subscription** -- `RefuseWatcher` at the door and `Recheck`
/// on every tick -- and that is the property #1512 turns on: they must answer alike, or a
/// proven watcher is admitted and dropped a tick later.
/// @param membership The node's oracle, bound once by the surface.
/// @param metrics Where the refusal is counted.
/// @param watcher Who is watching: the host, and whether the cluster key was proved.
/// @param stranger What THIS surface answers a host nobody listed.
/// @param strangerWhy The words that ride with it.
/// @return The refusal to answer, or nullopt when the watcher is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> RefuseUnlessMember(
    Distributed::IMembershipOracle const& membership,
    IMetricsSink& metrics,
    LiveWatcher const& watcher,
    Cc::SurfaceRefusal stranger,
    std::string_view strangerWhy)
{
    auto const decision = Distributed::ExplainConnection(membership, watcher.host, watcher.provedClusterKey);
    return AnswerMembership(decision.verdict, metrics, stranger, strangerWhy);
}

// `RefuseUnlessMemberAtAddress` stood here until #1512, and it is GONE rather than left
// unused. It existed to say *the cluster-key proof must not widen this gate*, and its one
// caller -- the live-stats door -- chose it for a reason about the SEAM rather than about the
// subject: `Protocol::ILiveGate` took a bare host, so the per-tick `Recheck` could not have
// seen a proof, and a door honouring one alone would admit a proven watcher and end its
// stream a tick later. #1512 widened the seam, so both ends now fold through
// `ExplainConnection` and the claim has no subject. Keeping the spelling would leave a
// policy that reads as available and describes nothing, which is how a table stops
// describing the file it is about.

} // namespace FastCache::Node
