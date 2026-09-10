// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of consensus-layer errors.
///
/// Deliberately small, and grown one enumerator at a time as a phase of the
/// consensus library actually produces one — the taxonomy is not invented up
/// front.
enum class ConsensusErrorCode : std::uint8_t
{
    InvalidConfiguration = 0, ///< The cluster configuration is not self-consistent.

    // The two conditions `RaftNode::ProposeMembership` used to answer
    // `InvalidConfiguration` for. They are neither invalid nor configuration: the
    // command is well formed and would be accepted at another instant, and a table
    // over this enum could therefore say nothing true about that enumerator (#196).
    // Splitting them is what lets `SubjectOf` be a fact about the CODE rather than
    // about which producer a caller happened to reach.
    ConfigurationChangeInFlight, ///< One membership change is already uncommitted; wait for it.
    MembershipUnchanged,         ///< The proposed member set is the one already in force.

    NotLeader,      ///< Only a leader may accept a proposal; see `knownLeader`.
    StorageFailure, ///< Durable state could not be written or read back.

    // Peer-wire decode failures. Three rather than one because a reader's
    // correct response differs: an unknown message type or an unsupported
    // version is a peer running another build, which is stepped over and logged
    // once, while a malformed frame means this reader and that sender disagree
    // about the bytes and the connection is no longer trustworthy.
    MalformedFrame,     ///< The payload does not match the shape its type declares.
    UnknownMessageType, ///< A type byte this build does not know; skip the frame.
    UnsupportedVersion, ///< A frame version outside the range this build decodes.
    Last,               ///< Not a code, and has no row: the length of a table keyed by one.
};

/// What a consensus refusal is *about*.
///
/// The distinction a retrying caller needs, and the one that has no other spelling:
/// a refusal either describes the **command** -- in which case offering it again
/// changes nothing, ever -- or it describes the **moment**, in which case the command
/// is fine and the next one would be refused identically.
///
/// It exists because `ConsensusTier::Reconcile` proposes a LIST. Abandoning the pass
/// at the first refusal is right for a moment-shaped one, and it was the only kind
/// that could arise when that code was written; a command-shaped one silently costs
/// the cluster everything after it in the list -- including the quorum reconciliation
/// that follows -- on every pass, forever, with one log line per interval as the
/// symptom. That is half of the trap #159 records.
///
/// **`Satisfied` is a third state and not a shade of the other two** (#196). "The
/// proposed member set is the one already in force" is a refusal only in the sense
/// that nothing was appended: the caller's goal is TRUE, which neither *this change
/// can never work* nor *ask again later* can express. Folded into `Command` it is
/// reported at Warn as a record somebody must go and correct; folded into `Moment` it
/// abandons a pass that had nothing left to do anyway. Both are the misleading
/// symptom this classification exists to remove, so the state is named -- which is
/// this repository's own rule that skipped, absent, unstarted and failed are four
/// states and every convenient summary collapses them.
enum class RefusalSubject : std::uint8_t
{
    Command,   ///< This change, permanently. Skip it; the rest of the list may be fine.
    Moment,    ///< This node, or now. The rest of the list would be refused identically.
    Satisfied, ///< Already true. Nothing was appended because nothing needed to be.
};

/// What each refusal is about, one row per `ConsensusErrorCode`.
///
/// A table rather than a comparison against the one enumerator that is different
/// today, because the failure it guards against is a code appended later and
/// classified by whichever way an `if` happened to be written -- which is a silent
/// cluster-wide stall if it lands on the wrong side.
struct RefusalSubjectRow
{
    ConsensusErrorCode code; ///< The refusal.
    RefusalSubject subject;  ///< What it is about.
};

/// One row per `ConsensusErrorCode`, in enumerator order.
///
/// `InvalidConfiguration` describes a command, `MembershipUnchanged` describes a goal
/// already met, and everything else describes this node or this instant.
/// `StorageFailure` is the one worth pausing on and it is a moment: the write that
/// failed says nothing about the command, and the next command would fail the same
/// way -- so a caller should stop, not skip.
///
/// **The classification is a property of the CODE, and it is now true of every
/// producer** (#196). It was not: `RaftNode::ProposeMembership` answered
/// `InvalidConfiguration` for two conditions that are not permanent, so this table
/// was right for a refusal from proposing a `Cluster::Command` -- where
/// `Cluster::Validate` is the sole producer -- and wrong for one from proposing a
/// membership change. That was guarded by a sentence in `SubjectOf`'s own doc and by
/// `ConsensusTier::ReconcileQuorum` declining to consult it, which is the weakest
/// kind of guard: the obvious next tidy-up would have wired it in and reported *wait
/// for the change in flight to commit* as **can never be recorded as it stands**, at
/// Warn, every reconcile interval, for a condition that resolves itself in one
/// commit. Splitting the enumerator is what makes the table say something true
/// wherever it is consulted -- and it fails the BUILD until each new code has decided
/// what it says on the wire, because `SchedulerService`'s `WireCodeFor` is an
/// `EnumTable` over this enum.
inline constexpr EnumTable<ConsensusErrorCode, RefusalSubjectRow> RefusalSubjects { {
    { .code = ConsensusErrorCode::InvalidConfiguration, .subject = RefusalSubject::Command },
    { .code = ConsensusErrorCode::ConfigurationChangeInFlight, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::MembershipUnchanged, .subject = RefusalSubject::Satisfied },
    { .code = ConsensusErrorCode::NotLeader, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::StorageFailure, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::MalformedFrame, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::UnknownMessageType, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::UnsupportedVersion, .subject = RefusalSubject::Moment },
} };

static_assert(RowsInEnumeratorOrder(RefusalSubjects, &RefusalSubjectRow::code),
              "RefusalSubjects must hold one row per ConsensusErrorCode, in enumerator order");

/// What @p code is about.
///
/// **Every path**, which is what #196 bought: a caller no longer has to know which
/// producer it reached. The sentence that used to stand here -- *not for a
/// membership-change refusal* -- was a documentation guard around
/// `InvalidConfiguration` carrying two opposite meanings, and it is gone because the
/// meanings are now two codes.
/// @param code The refusal.
/// @return Whether it describes the command, the moment, or a goal already met.
[[nodiscard]] constexpr RefusalSubject SubjectOf(ConsensusErrorCode code) noexcept
{
    return RefusalSubjects[static_cast<std::size_t>(code)].subject;
}

/// Structured consensus error.
struct ConsensusError
{
    ConsensusErrorCode code = ConsensusErrorCode::InvalidConfiguration;

    /// What specifically was wrong, in terms an operator can act on.
    ///
    /// Carried rather than derived from `code`, because every current use is a
    /// configuration rejection and "invalid configuration" on its own tells
    /// somebody editing a file nothing about which field to look at.
    std::string context;

    /// For `NotLeader`: who to ask instead, when this node knows.
    ///
    /// Carried in the error rather than left for the caller to go and look up,
    /// because the two answers are different and the difference is actionable:
    /// "somebody else leads, ask them" is a redirect, while "nobody leads right
    /// now" means an election is in progress and the caller should fall back
    /// rather than chase it. A bare refusal cannot express the second, and this
    /// system's whole distribution story rests on being able to give up
    /// immediately and compile locally.
    std::optional<std::string> knownLeader;

    /// Render for a log line or a startup refusal.
    /// @return The formatted error.
    [[nodiscard]] std::string ToString() const
    {
        return std::format("ConsensusError(code={} context={})", static_cast<unsigned>(code), context);
    }
};

/// Build an `InvalidConfiguration` error.
/// @param context What was wrong with it.
/// @return The error.
[[nodiscard]] inline ConsensusError InvalidConfiguration(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::InvalidConfiguration,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `ConfigurationChangeInFlight` error.
///
/// A MOMENT, and the whole reason #196 split it out of `InvalidConfiguration`: it
/// resolves itself the instant the change in flight commits, so a caller that reads
/// it as permanent reports a healthy cluster as a broken one once per interval.
/// @param context Which change is in flight, in terms a log line can carry.
/// @return The error.
[[nodiscard]] inline ConsensusError ConfigurationChangeInFlight(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::ConfigurationChangeInFlight,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `MembershipUnchanged` error.
///
/// Not a fault in either direction: the proposal was well formed and the set it names
/// is the set already in force, so nothing was appended because nothing needed to be.
/// It stays an error rather than becoming a success carrying no index, because a
/// success would have to invent a `Proposal` naming an entry that does not exist --
/// and `Cluster::NextQuorumChange` never proposes an unchanged set, so the state a
/// success would force every caller to handle is one production does not reach. What
/// it is NOT is a refusal an operator should be shown at Warn; `RefusalSubject`
/// carries that.
/// @param context The set, in terms a log line can carry.
/// @return The error.
[[nodiscard]] inline ConsensusError MembershipUnchanged(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::MembershipUnchanged,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `NotLeader` error.
/// @param knownLeader Who leads instead, when this node knows.
/// @return The error.
[[nodiscard]] inline ConsensusError NotLeader(std::optional<std::string> knownLeader)
{
    return ConsensusError { .code = ConsensusErrorCode::NotLeader,
                            .context = knownLeader.has_value() ? std::format("not the leader; {} is", *knownLeader)
                                                               : std::string { "not the leader, and none is known" },
                            .knownLeader = std::move(knownLeader) };
}

/// Build a `StorageFailure` error.
/// @param context What failed, and where.
/// @return The error.
[[nodiscard]] inline ConsensusError StorageFailure(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::StorageFailure,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build a `MalformedFrame` error.
/// @param context Which message, and what about it did not parse.
/// @return The error.
[[nodiscard]] inline ConsensusError MalformedWireFrame(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::MalformedFrame,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build an `UnknownMessageType` error.
///
/// Separate from `MalformedFrame` because it is the *expected* condition in a
/// fleet that is mid-upgrade: the frame is well-formed and simply says something
/// this build has no opinion about, so the reader steps over it and carries on.
/// Reporting it as malformed would make a rolling upgrade look like corruption.
/// @param context Which type code, in terms a log line can carry.
/// @return The error.
[[nodiscard]] inline ConsensusError UnknownWireMessage(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::UnknownMessageType,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

/// Build an `UnsupportedVersion` error.
/// @param context The offending version and the range that would have worked.
/// @return The error.
[[nodiscard]] inline ConsensusError UnsupportedWireVersion(std::string_view context)
{
    return ConsensusError { .code = ConsensusErrorCode::UnsupportedVersion,
                            .context = std::string { context },
                            .knownLeader = std::nullopt };
}

} // namespace FastCache
