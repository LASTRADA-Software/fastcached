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
    NotLeader,                ///< Only a leader may accept a proposal; see `knownLeader`.
    StorageFailure,           ///< Durable state could not be written or read back.

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
enum class RefusalSubject : std::uint8_t
{
    Command, ///< This change, permanently. Skip it; the rest of the list may be fine.
    Moment,  ///< This node, or now. The rest of the list would be refused identically.
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
/// Only `InvalidConfiguration` describes a command; everything else describes this
/// node or this instant. `StorageFailure` is the one worth pausing on and it is a
/// moment: the write that failed says nothing about the command, and the next
/// command would fail the same way -- so a caller should stop, not skip.
///
/// **The classification is a property of the code, and one producer disagrees.**
/// `RaftNode::ProposeMembership` also answers `InvalidConfiguration` for two
/// conditions that are plainly moments -- a configuration change already in flight,
/// and a proposed member set equal to the current one -- so this table is right for a
/// refusal that came back from proposing a `Cluster::Command`, where
/// `Cluster::Validate` is the only producer, and wrong for one that came back from
/// proposing a *membership change*. `SubjectOf` says so at its own doc, and
/// `ConsensusTier::ReconcileQuorum` deliberately does not consult it. Making the code
/// carry the property properly means splitting the enumerator, which is
/// https://github.com/LASTRADA-Software/fastcached/issues/196.
inline constexpr EnumTable<ConsensusErrorCode, RefusalSubjectRow> RefusalSubjects { {
    { .code = ConsensusErrorCode::InvalidConfiguration, .subject = RefusalSubject::Command },
    { .code = ConsensusErrorCode::NotLeader, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::StorageFailure, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::MalformedFrame, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::UnknownMessageType, .subject = RefusalSubject::Moment },
    { .code = ConsensusErrorCode::UnsupportedVersion, .subject = RefusalSubject::Moment },
} };

static_assert(RowsInEnumeratorOrder(RefusalSubjects, &RefusalSubjectRow::code),
              "RefusalSubjects must hold one row per ConsensusErrorCode, in enumerator order");

/// What @p code is about, for a refusal produced by proposing a `Cluster::Command`.
///
/// **Not for a membership-change refusal.** `RaftNode::ProposeMembership` answers
/// `InvalidConfiguration` for transient conditions as well, so a caller on that path
/// would read "wait for the change in flight to commit" as permanent and report it
/// as such every interval -- which is the misleading-symptom failure the split this
/// table exists for was meant to remove. See `RefusalSubjects`.
/// @param code The refusal.
/// @return Whether it describes the command or the moment.
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
