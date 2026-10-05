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

/// The refusal a connection that proved a REVOKED identity key, or presented a ticket signed by
/// one, gets wherever it knocks (#178, #1555).
///
/// A machine is forgotten by its key, so this is the one refusal a forgotten machine gets: the
/// removed MACHINE itself, still holding its key and still dialling in. One row for every
/// surface, because the diagnosis belongs to the machine rather than to the door.
///
/// `NotAMember` on the wire, and the same code a stranger gets, because a CLIENT acts on
/// the two identically -- `fastcache-cc` steps over the refusal and compiles locally
/// either way, and a code of its own would be an unknown one to every launcher already
/// deployed. One code, two counters, which is the split `SurfaceRefusal` exists to hold.
inline constexpr Cc::SurfaceRefusal KeyForgotten {
    .code = CompileCacheWire::ErrorCode::NotAMember,
    .counter = IMetricsSink::Counter::NodeRequestsRefusedKeyRevoked,
};

/// What a machine that PROVED a key the cluster revoked is told.
///
/// **Only a proof is told this** (`Distributed::RevocationIsProven`). A connection whose revoked key
/// arrived in a TICKET is refused on the same row and counted on the same counter, and answered with
/// the surface's own stranger sentence: a ticket is bytes anybody may have captured, and this
/// sentence would tell its holder that a third party was forgotten -- the fact AUTH's refusal
/// already declines to say (`TicketRefusalSays`). What stays different is the counter, read on the
/// node. **On a `--fleet-open` node it DOES disclose**: an unknown ticket is served there and a
/// revoked one is not, and the self form of `explain-admission` answers the revoked ticket
/// `Outsider` where a stranger gets `Member` by `fleet-open` -- so a captured ticket's holder learns
/// its machine was forgotten. That is the price of the forget biting under the flag at all.
inline constexpr std::string_view KeyForgottenWhy =
    "this fleet revoked the identity key this connection proved; the machine was forgotten, and it is served "
    "nothing here from any address until it is admitted again under a new key";

/// Answer one already-taken decision, or nullopt when it admits the caller.
///
/// Split out when the second way of ASKING arrived (#1428): the two spellings below differ
/// only in what they ask about -- a connection, or an address -- and the answer they give is
/// one decision that must not be written twice.
///
/// Takes the DECISION rather than the verdict since #178. `Forgotten` has one author, the key
/// roster (`KeyTombstone`): a machine is forgotten by its key, and no address route answers it. The
/// decision also says how the revoked key was shown, which decides the WORDS and nothing else: the
/// row is `KeyForgotten` either way.
/// @param decision What the fold concluded, which routes concluded it, and how a revoked key was shown.
/// @param metrics Where the refusal is counted.
/// @param stranger What THIS surface answers a host nobody listed.
/// @param strangerWhy The words that ride with it, and with a revocation only a ticket showed.
/// @return The refusal to answer, or nullopt when the caller is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> AnswerMembership(Distributed::MembershipDecision decision,
                                                                            IMetricsSink& metrics,
                                                                            Cc::SurfaceRefusal stranger,
                                                                            std::string_view strangerWhy)
{
    switch (decision.verdict)
    {
        case Distributed::Membership::Member:
            return std::nullopt;
        case Distributed::Membership::Forgotten:
            return Cc::Refuse(
                metrics, KeyForgotten, Distributed::RevocationIsProven(decision) ? KeyForgottenWhy : strangerWhy);
        case Distributed::Membership::Outsider:
        case Distributed::Membership::Last:
            break;
    }

    // `Outsider`, and anything a switch cannot name: a gate fails closed. `Last` is not a
    // verdict and reaching it would be a bug, so it is refused rather than admitted --
    // the direction where being wrong costs a build instead of a machine.
    return Cc::Refuse(metrics, stranger, strangerWhy);
}

/// Refuse the caller unless this node's oracle admits it -- as this machine, under `--fleet-open`,
/// or by a key its CONNECTION proved or presented.
///
/// **The keys are folded here and nowhere else**, which is what makes *a machine with a live key is
/// a member, and a revoked one is not* true at every gate that asks this question rather than at
/// the ones somebody remembered (#1428, #178). `Distributed::ExplainConnection` is the fold, on the
/// same `PrecedenceOf` the address routes are composed with -- so a revoked key is refused whatever
/// its address, and a connection that established nothing gets exactly what the oracle answered.
///
/// **One function for a connection and for a live watcher**, because they are one type
/// (`ConnectionFacts`): two gates reach this for ONE subscription -- `RefuseWatcher` at the door and
/// `Recheck` on every tick -- and they must answer alike, or a watcher admitted at the door is
/// dropped a tick later (#1512).
///
/// @param membership The node's oracle, bound once by the surface -- never re-asked for,
///        which is what makes a removal reach a running surface at all.
/// @param metrics Where the refusal is counted.
/// @param facts The caller: the host the kernel reported, and what this connection established.
/// @param stranger What THIS surface answers a caller nothing admits: its own row and its own
///        sentence, because stranger traffic at each door is a different story.
/// @param strangerWhy The words that ride with it.
/// @return The refusal to answer, or nullopt when the caller is a member.
[[nodiscard]] inline std::optional<std::vector<std::byte>> RefuseUnlessMember(
    Distributed::IMembershipOracle const& membership,
    IMetricsSink& metrics,
    ConnectionFacts const& facts,
    Cc::SurfaceRefusal stranger,
    std::string_view strangerWhy)
{
    return AnswerMembership(Distributed::ExplainConnection(membership, facts), metrics, stranger, strangerWhy);
}

// `RefuseUnlessMemberAtAddress` stood here until #1512, and it is GONE rather than left
// unused. It existed to say *the node proof must not widen this gate*, and its one
// caller -- the live-stats door -- chose it for a reason about the SEAM rather than about the
// subject: `Protocol::ILiveGate` took a bare host, so the per-tick `Recheck` could not have
// seen a proof, and a door honouring one alone would admit a proven watcher and end its
// stream a tick later. #1512 widened the seam, so both ends now fold through
// `ExplainConnection` and the claim has no subject. Keeping the spelling would leave a
// policy that reads as available and describes nothing, which is how a table stops
// describing the file it is about.

} // namespace FastCache::Node
