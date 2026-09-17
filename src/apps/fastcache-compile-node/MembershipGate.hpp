// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "PeerIdentity.hpp"

#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
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

/// Refuse @p peerAddress unless this node's oracle admits it, IGNORING any proof.
///
/// **Choosing this spelling is a claim, not a convenience**: it says the cluster-key proof
/// must not widen this particular gate, and the caller has to say why. Same idea as
/// `Cc::RefuseWithoutCounter` beside `Cc::Refuse` -- two spellings so that *decided* cannot
/// be written the way *forgot* is.
///
/// One caller, and it is `LiveStatsResponder`. A subscription is re-gated on EVERY tick
/// through `Protocol::ILiveGate::Recheck`, which takes an address because `fastcached` shares
/// that seam and holds no cluster key; a door that honoured the proof while the re-gate did
/// not would admit a proven watcher and drop it one tick later, which is worse than refusing
/// it. So the two agree, and they agree in the closed direction.
///
/// @param membership The node's oracle.
/// @param metrics Where the refusal is counted.
/// @param peerAddress The caller's host, as the kernel reported it.
/// @param stranger What THIS surface answers a host nobody listed.
/// @param strangerWhy The words that ride with it.
/// @param proofExcluded Why a cluster-key proof may not widen this gate. Read by a person,
///        never by the code -- it is here so the claim cannot be made silently.
/// @return The refusal to answer, or nullopt when the caller is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> RefuseUnlessMemberAtAddress(
    Distributed::IMembershipOracle const& membership,
    IMetricsSink& metrics,
    std::string_view peerAddress,
    Cc::SurfaceRefusal stranger,
    std::string_view strangerWhy,
    std::string_view /*proofExcluded*/)
{
    return AnswerMembership(membership.Classify(peerAddress), metrics, stranger, strangerWhy);
}

} // namespace FastCache::Node
