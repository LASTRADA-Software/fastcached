// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/Ranges.hpp>

/// @file SplitEvidence.hpp
/// Whether a proven fleet of another cluster is THIS fleet split in two, decided only on a fact a key
/// this fleet already held verifies.
///
/// Two fleets that each admitted a machine of the other -- a one-sided or mutual admission, or a
/// network split that formed on both sides -- are one fleet. But a proven summary's member list is its
/// speaker's CLAIM: the signature proves who spoke, not what it records, and ids ride every beacon.
/// Believed on its own, a fleet claiming to be older and listing this fleet's leader would make an
/// established fleet dissolve into whoever minted it -- a one-beacon takeover. So a split is
/// recognised on exactly two facts, each tied to a key this side held BEFORE the encounter:
///
/// - **(A) Their speaker is our member**: recorded in this fleet's state under the id it speaks as,
///   WITH the key it proved. Forging it needs that member's private key. This is what the side that
///   RECORDED the other's machine sees.
/// - **(C) We asked them, and they list us**: a machine of this fleet once asked that fleet to admit
///   it, under the key that proves this summary now, and the summary names that machine. This is what
///   the side that was RECORDED sees.
///
/// **Only (A) by a VOTER heals by itself** (`SplitEvidenceRow::healing`). (C) is trust on FIRST USE: a
/// solitary machine asks whichever fleet beaconed oldest, so the key behind a memo is one anybody who
/// beacons can have it hold -- and a fleet that refused the asker, never answered it, or admitted it and
/// then dissolved itself away leaves the same memo as a real split does. A learner's key is first-use
/// trust too, since auto-approval admits learners. Both are READ and told to an operator, as
/// `fleet-split-healing`; neither moves a fleet.
namespace FastCache::Cluster
{

/// A machine of this fleet that once asked another fleet to admit it: a memo from this node's own
/// record, or one a member announced.
struct AskedJoinBy
{
    std::string askerId;           ///< The machine that asked.
    std::string clusterId;         ///< The fleet it asked.
    Ed25519PublicKey provenKey {}; ///< The key that proved that fleet's summary when it asked.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(AskedJoinBy const&, AskedJoinBy const&) = default;
};

/// Where the join memos a fleet's members announced are read from: what `SplitEvidenceFor` takes as
/// its announced memos, on the node that decides -- its leader, which every member announces to.
class IAnnouncedJoinMemos
{
  public:
    IAnnouncedJoinMemos() = default;
    IAnnouncedJoinMemos(IAnnouncedJoinMemos const&) = delete;
    IAnnouncedJoinMemos(IAnnouncedJoinMemos&&) = delete;
    IAnnouncedJoinMemos& operator=(IAnnouncedJoinMemos const&) = delete;
    IAnnouncedJoinMemos& operator=(IAnnouncedJoinMemos&&) = delete;
    virtual ~IAnnouncedJoinMemos() = default;

    /// @return Every memo a recorded member announced, each under the id it PROVED.
    [[nodiscard]] virtual std::vector<AskedJoinBy> AnnouncedJoinMemos() const = 0;
};

/// The seat @p seen's speaker holds in @p state, when it is recorded there under the id it speaks as,
/// holding the key that proved it: evidence (A).
/// @param state This fleet's applied state.
/// @param seen The other fleet's summary, as its signature proved it.
/// @return Its seat when the member record's key IS the proving key, and that key is not revoked here;
///         nothing otherwise.
[[nodiscard]] inline std::optional<MemberSeat> SpeakerSeatHere(ClusterState const& state, ProvenFleetSummary const& seen)
{
    auto const& speaker = seen.Summary().nodeId;
    if (speaker.empty() || state.IsRevoked(seen.Key()))
        return std::nullopt;
    auto const* const recorded = core::findIfOrNull(state.members, [&speaker, &seen](ClusterMember const& member) {
        return member.id == speaker && member.publicKey == std::optional { seen.Key() };
    });
    return recorded != nullptr ? std::optional { recorded->seat } : std::nullopt;
}

/// Whether @p seen's speaker is recorded in @p state under the id it speaks as, holding the key that
/// proved it, in any seat.
/// @param state This fleet's applied state.
/// @param seen The other fleet's summary, as its signature proved it.
/// @return True when `SpeakerSeatHere` names a seat.
[[nodiscard]] inline bool SpeakerIsOurMember(ClusterState const& state, ProvenFleetSummary const& seen)
{
    return SpeakerSeatHere(state, seen).has_value();
}

/// Which kind of evidence (A) is, by the seat its speaker holds here.
struct SpeakerSeatEvidence
{
    MemberSeat seat;        ///< The speaker's seat in this fleet.
    SplitEvidence evidence; ///< The evidence that makes.
};

/// One row per seat: a voter's speech is the evidence that heals by itself, a learner's is told.
inline constexpr EnumTable<MemberSeat, SpeakerSeatEvidence> SpeakerEvidenceBySeat { {
    { .seat = MemberSeat::Voter, .evidence = SplitEvidence::TheirSpeakerIsOurVoter },
    { .seat = MemberSeat::Learner, .evidence = SplitEvidence::TheirSpeakerIsOurLearner },
} };
static_assert(RowsInEnumeratorOrder(SpeakerEvidenceBySeat, &SpeakerSeatEvidence::seat),
              "SpeakerEvidenceBySeat must hold one row per MemberSeat, in enumerator order");

/// Whether @p memo is a machine of this fleet asking @p seen's fleet under the key that proves @p seen
/// now, and @p seen lists that machine: evidence (C).
/// @param memo Who asked which fleet, under which key.
/// @param seen The other fleet's summary, as its signature proved it.
/// @return True when all three hold.
[[nodiscard]] inline bool WeAskedAndTheyListUs(AskedJoinBy const& memo, ProvenFleetSummary const& seen)
{
    auto const& summary = seen.Summary();
    return memo.clusterId == summary.clusterId && memo.provenKey == seen.Key()
           && std::ranges::contains(summary.members, memo.askerId);
}

/// The memos a fleet's split evidence is read from: this node's own, as its record keeps them, and
/// those a member announced -- only while that member is still recorded in @p state, since a machine
/// the fleet no longer records speaks for nobody in it.
/// @param state This fleet's applied state.
/// @param selfId This node's id.
/// @param record This node's formation record.
/// @param announced What members announced.
/// @return Every memo that counts.
[[nodiscard]] inline std::vector<AskedJoinBy> CountedMemos(ClusterState const& state,
                                                           std::string_view selfId,
                                                           FormationRecord const& record,
                                                           std::span<AskedJoinBy const> announced)
{
    auto memos = std::vector<AskedJoinBy> {};
    for (auto const& own: record.askedJoins)
        memos.push_back(
            AskedJoinBy { .askerId = std::string { selfId }, .clusterId = own.clusterId, .provenKey = own.provenKey });
    for (auto const& memo: announced)
    {
        auto const recorded =
            std::ranges::any_of(state.members, [&memo](ClusterMember const& member) { return member.id == memo.askerId; });
        if (recorded)
            memos.push_back(memo);
    }
    return memos;
}

/// What this fleet can say about a proven fleet of another cluster: the evidence it is this fleet
/// split and the machine of this fleet that evidence rests on -- or, where there is none, a machine of
/// this fleet the other CLAIMS to record.
///
/// The machine is what an operator asks: the one that spoke for the other fleet, or the one that once
/// asked it. The claim is what an operator reads differently from "no machine in common": either
/// somebody is minting a fleet that names this one's machines, or a list a datagram cut left the
/// evidence out.
struct SplitReading
{
    SplitEvidence evidence { SplitEvidence::None }; ///< Why it is this fleet split, if it is.
    std::string claimedMember {}; ///< With no evidence, a machine of this fleet it lists; empty when it names none.
    std::string witness {};       ///< With evidence, the machine of this fleet it rests on: the speaker, or the asker.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(SplitReading const&, SplitReading const&) = default;
};

/// Read @p seen against this fleet: (A) first, by its speaker's seat, then (C); and with neither, the
/// first machine of this fleet -- this node, or one its state records -- that the other's list names.
///
/// `None` is the answer for a fleet that merely LISTS a machine of this one -- the attack signature,
/// and the residual of a list a datagram had to cut.
/// @param state This fleet's applied state.
/// @param selfId This node's id.
/// @param record This node's formation record: its own join memos.
/// @param announced The join memos members announced.
/// @param seen The other fleet's summary, as its signature proved it.
/// @return The reading.
[[nodiscard]] inline SplitReading ReadSplit(ClusterState const& state,
                                            std::string_view selfId,
                                            FormationRecord const& record,
                                            std::span<AskedJoinBy const> announced,
                                            ProvenFleetSummary const& seen)
{
    if (auto const seat = SpeakerSeatHere(state, seen); seat.has_value())
        return SplitReading { .evidence = SpeakerEvidenceBySeat[static_cast<std::size_t>(*seat)].evidence,
                              .claimedMember = {},
                              .witness = seen.Summary().nodeId };
    auto const memos = CountedMemos(state, selfId, record, announced);
    auto const* const asked =
        core::findIfOrNull(memos, [&seen](AskedJoinBy const& memo) { return WeAskedAndTheyListUs(memo, seen); });
    if (asked != nullptr)
        return SplitReading { .evidence = SplitEvidence::WeAskedAndTheyListUs,
                              .claimedMember = {},
                              .witness = asked->askerId };
    for (auto const& listed: seen.Summary().members)
        if (listed == selfId || std::ranges::contains(state.members, listed, &ClusterMember::id))
            return SplitReading { .evidence = SplitEvidence::None, .claimedMember = listed, .witness = {} };
    return SplitReading {};
}

/// Whether @p seen is this fleet split in two, and on which evidence: `ReadSplit`'s evidence.
/// @param state This fleet's applied state.
/// @param selfId This node's id.
/// @param record This node's formation record: its own join memos.
/// @param announced The join memos members announced.
/// @param seen The other fleet's summary, as its signature proved it.
/// @return The evidence, or `SplitEvidence::None`.
[[nodiscard]] inline SplitEvidence SplitEvidenceFor(ClusterState const& state,
                                                    std::string_view selfId,
                                                    FormationRecord const& record,
                                                    std::span<AskedJoinBy const> announced,
                                                    ProvenFleetSummary const& seen)
{
    return ReadSplit(state, selfId, record, announced, seen).evidence;
}

/// Where a proven fleet's split evidence is read from, as this node stands now: what
/// `foreign-fleet-visible` and `fleet-split-healing` are raised from, so the rows and the decision
/// to heal agree.
class ISplitEvidenceSource
{
  public:
    ISplitEvidenceSource() = default;
    ISplitEvidenceSource(ISplitEvidenceSource const&) = delete;
    ISplitEvidenceSource(ISplitEvidenceSource&&) = delete;
    ISplitEvidenceSource& operator=(ISplitEvidenceSource const&) = delete;
    ISplitEvidenceSource& operator=(ISplitEvidenceSource&&) = delete;
    virtual ~ISplitEvidenceSource() = default;

    /// @param seen A proven fleet of another cluster.
    /// @return What this node can say about it (`ReadSplit`).
    [[nodiscard]] virtual SplitReading ReadSplit(ProvenFleetSummary const& seen) const = 0;
};

/// The member ids this fleet's summary lists, in the order a carrier's cut keeps them.
///
/// **The ids another fleet most needs come first**: the machines of this fleet seen SPEAKING for
/// another proven fleet, newest first -- exactly the set evidence (C) needs from the other side, since
/// a machine that asked a fleet is one that fleet may have admitted -- then the voters, then the
/// learners, each by id. So a datagram's cut (`MaxFleetSummaryMembers`) drops that set last: only once
/// more of this fleet's machines speak for other fleets at once than a datagram holds. Past that, the
/// other side reads this fleet as foreign, naming the machine it claims, which fails closed: (C) is
/// told to an operator and moves nothing either way.
/// @param state This fleet's applied state.
/// @param spokeElsewhere Ids of this fleet's machines seen speaking for another proven fleet, newest
///        first; an id the state does not record is skipped.
/// @return Every member's id, each once.
[[nodiscard]] inline std::vector<std::string> SummaryMembers(ClusterState const& state,
                                                             std::span<std::string const> spokeElsewhere)
{
    auto ordered = std::vector<std::string> {};
    ordered.reserve(state.members.size());
    auto const recorded = [&state](std::string_view id) {
        return std::ranges::contains(state.members, id, &ClusterMember::id);
    };
    for (auto const& id: spokeElsewhere)
        if (recorded(id) && !std::ranges::contains(ordered, id))
            ordered.push_back(id);
    for (auto const seat: { MemberSeat::Voter, MemberSeat::Learner })
    {
        auto seated = std::vector<std::string> {};
        for (auto const& member: state.members)
            if (member.seat == seat && !std::ranges::contains(ordered, member.id))
                seated.push_back(member.id);
        std::ranges::sort(seated);
        ordered.insert(ordered.end(), seated.begin(), seated.end());
    }
    return ordered;
}

} // namespace FastCache::Cluster
