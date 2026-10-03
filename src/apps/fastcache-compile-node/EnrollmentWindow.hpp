// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollAutoApprove.hpp"
#include "NodeConditions.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// How many machines may be waiting at once.
///
/// **A refusal past this, never an eviction**, and that is the decision the bound
/// exists to express rather than an arbitrary size. The list is ungated by design --
/// the explicit approve is the gate -- so anybody who can reach the port can add rows;
/// evicting to make room would let a flooder push the real joiner off the list the
/// operator is reading, which is silent from both ends and undetectable afterwards. A
/// full list refuses by name, counts, and is visible.
///
/// Sixty-four rather than a number scaled to the fleet, because the list is what a
/// PERSON reads before deciding, and a list longer than a terminal is a list nobody
/// checks a mismatch against. A rollout larger than this is approved in batches, which
/// is what an operator does anyway.
inline constexpr std::size_t MaxPendingEnrollments = 64;

/// How many rows nobody has decided about one source HOST may hold on the list at once.
///
/// **Without it one peer that keeps polling holds every row forever**, and every legitimate
/// joiner is refused `Full` until the leader restarts: the refusal-not-eviction rule above is
/// what keeps a flooder from pushing a real joiner OFF the list, and this is what keeps one
/// from filling it in the first place. Enrollment is TCP, so the host is the kernel's and not
/// a claim the peer chose.
///
/// Four, because the honest case for several machines behind ONE address is NAT or a host
/// running a few VMs, which is a handful rather than dozens, and a larger rollout from one
/// address is approved a few at a time as its rows are decided -- a decided row stops counting
/// at once. It is a sixteenth of the list, so a flooder needs sixteen hosts to fill it, and an
/// operator who meets that has `--enroll-clear`.
///
/// Counted by the address a row was CREATED from (`EnrollmentPendingEntry::firstPeerId`), which
/// a later poll never changes, and with an IPv4-mapped IPv6 address folded to its IPv4 form. **Not grouped by IPv6 /64**: an
/// office LAN is one /64, and twenty machines asking from it on the day it goes live are twenty legitimate joiners, which a
/// /64 bound would refuse after four.
inline constexpr std::size_t MaxPendingEnrollmentsPerHost = 4;

/// How often a leader with machines waiting says so again.
///
/// **Repeating rather than one line when the first one asks**, because a one-shot line
/// scrolls away, and a machine waiting for a person is waiting for somebody who may not be
/// looking. Sixty seconds is short enough that a joiner left waiting over a lunch break is
/// unmissable in the log and long enough that it is not itself noise.
inline constexpr std::chrono::seconds EnrollmentWarningInterval { 60 };

/// How long a request row is kept without a poll: how a leader forgets a joiner that went away.
///
/// A joiner polls every couple of seconds while it waits, so ten minutes of silence is a
/// machine that stopped asking -- switched off, or given up -- and not one between polls. The
/// row goes, counted if nobody had decided about it (`EnrollmentRequestsExpired`), and the
/// machine's next poll, if one ever comes, records it again as a new row: the list an operator
/// reads is the machines that are still asking.
inline constexpr std::chrono::minutes PendingRowLifetime { 10 };

/// What the window decided about one `Enroll`.
///
/// A private enum: nothing transmits or persists these ordinals. The wire's answer is
/// `CompileCacheWire::EnrollOutcome` for the three that are replies and an
/// `ErrorCode` for the one that is a refusal, and the mapping is the responder's. There is
/// no `Closed`: nothing has to be opened before a machine may ask.
enum class EnrollDecision : std::uint8_t
{
    Full,        ///< The list is full and this id is not already on it. Refused, nothing recorded.
    Pending,     ///< Recorded and waiting for a person -- or asked under a key nobody decided about.
    Approved,    ///< A person approved exactly this claim; the caller hands over the roster.
    Rejected,    ///< A person refused it.
    AutoApprove, ///< An armed deadline had not passed: the caller admits it on the leader's own authority.
    HostFull,    ///< This host already holds `MaxPendingEnrollmentsPerHost` undecided rows. Refused, nothing recorded.
};

/// What an enrollment ROLE means on this node (#178).
struct EnrollRoleRow
{
    CompileCacheWire::EnrollRole role; ///< The wire role.
    std::string_view name;             ///< Its one spelling in a list and in a sentence.

    /// Whether a request in this role states an endpoint. A learner does not: it dials the
    /// leader rather than being dialled.
    bool statesEndpoint;

    /// The seat `ClusterAdmit` records the joiner in. A learner's is `Learner`: it dials in, so it
    /// is admitted with no consensus endpoint, and it holds no vote until an operator promotes it.
    Cluster::MemberSeat seat;
};

/// One row per role a joiner may ask for: a machine joins ONE way, as a learner.
///
/// A plain array rather than an `EnumTable`, for `KnownEnrollmentDecisions`' reason: a WIRE enum
/// carries no `Last`. Every row names a role the wire knows; a role the wire still decodes and no
/// row names is one this build refuses at the door (`ServesEnrollRole`).
inline constexpr std::array EnrollRoleTable {
    EnrollRoleRow { .role = CompileCacheWire::EnrollRole::Learner,
                    .name = "learner",
                    .statesEndpoint = false,
                    .seat = Cluster::MemberSeat::Learner },
};

/// Whether every row names a role the wire knows, and no role has two rows.
/// @return True when `EnrollRoleTable` is well formed.
[[nodiscard]] consteval bool EveryEnrollRoleRowIsKnownAndUnique() noexcept
{
    return std::ranges::all_of(EnrollRoleTable, [](EnrollRoleRow const& row) {
        return std::ranges::count(CompileCacheWire::KnownEnrollRoles, row.role) == 1
               && std::ranges::count(EnrollRoleTable, row.role, &EnrollRoleRow::role) == 1;
    });
}

static_assert(EveryEnrollRoleRowIsKnownAndUnique(), "every EnrollRoleTable row names one role the wire knows, once");

/// Whether a joiner may ask to be @p role: a row names it.
/// @param role A role the decoder accepted.
/// @return True when this build admits a joiner in that role.
[[nodiscard]] constexpr bool ServesEnrollRole(CompileCacheWire::EnrollRole role) noexcept
{
    return std::ranges::count(EnrollRoleTable, role, &EnrollRoleRow::role) == 1;
}

/// The row for @p role.
/// @param role A role `ServesEnrollRole` accepted.
/// @return Its row; the first row for a value no row names, which the enrollment door refuses before
///         anything asks.
[[nodiscard]] constexpr EnrollRoleRow const& EnrollRoleRowFor(CompileCacheWire::EnrollRole role) noexcept
{
    for (auto const& row: EnrollRoleTable)
        if (row.role == role)
            return row;
    return EnrollRoleTable.front();
}

/// Whether @p roster records @p nodeId holding @p key as a member, in @p role.
///
/// What the leader asks of its own roster before it answers `Approved`, and what the joiner asks
/// of the roster it is handed before it believes it was admitted: one question, so the two ends
/// cannot come to mean different things by "admitted".
/// @param roster The roster.
/// @param nodeId The joiner's id.
/// @param key The key it asked under.
/// @param role What it asked to be.
/// @return True when the roster says exactly that.
[[nodiscard]] bool RosterRecordsJoiner(Cluster::Roster const& roster,
                                       std::string_view nodeId,
                                       Ed25519PublicKey const& key,
                                       CompileCacheWire::EnrollRole role);

/// What one joiner claims about itself, as a single request carried it.
///
/// One value rather than four parameters, because two of them are strings and two adjacent
/// strings at a call site are two a caller can silently transpose.
struct JoinerClaim
{
    std::string_view nodeId;                                                      ///< The identity it claims.
    std::string_view nodeEndpoint;                                                ///< The `0xFC` endpoint it claims.
    CompileCacheWire::EnrollRole role { CompileCacheWire::EnrollRole::Learner };  ///< What it asks to be.
    std::array<std::byte, CompileCacheWire::IdentityPublicKeyBytes> publicKey {}; ///< The key it asks under.
};

/// What an operator's decision did.
///
/// A private enum: nothing transmits these ordinals. The responder maps them onto wire
/// codes, which is where the operator-facing vocabulary lives.
enum class EnrollControlOutcome : std::uint8_t
{
    Done,           ///< Applied.
    AlreadyInForce, ///< Nothing to do: the id was already decided this way.
    UnknownSubject, ///< No pending entry carries that id.
};

/// The leader's list of machines asking to join, and what a person decided about each.
///
/// **In memory, not a flag and not replicated, and each of those is a decision.**
///
/// Nothing opens it: every machine that asks is recorded, bounded by `MaxPendingEnrollments`,
/// because since #178 no secret crosses and the APPROVAL is the gate. A window in front of the
/// approval only made a machine joining over the LAN need a person twice.
///
/// Not replicated, because the list is one leader's record of who asked it in the last few
/// minutes, while the ADMISSION it leads to is replicated through the existing `ClusterAdmit`
/// path and outlives everything. A restart forgets it, and a machine still asking is recorded
/// again at its next poll.
///
/// Thread-safe: the responder reaches it from a reactor thread and the warning ticker
/// from its own, so every method takes the lock. The decisions are pure with respect to
/// I/O -- no socket, no file, no consensus -- which is what lets the whole of this be
/// exercised against a `core::platform::ManualClock`.
class EnrollmentWindow
{
  public:
    /// @param clock Where "now" comes from; must outlive this.
    /// @param conditions Where waiting machines are RAISED and cleared (#1364), or null on a node
    ///        that serves no enrollment -- whose rows its scope then answers, rather than this
    ///        object reporting a reassuring `clear` for a list nothing can reach. Must outlive this.
    /// @param metrics Where a row forgotten before anybody decided about it is counted, or null
    ///        where nothing counts it. Must outlive this.
    /// @param wallClock Where the absolute instants an armed window's condition names come from;
    ///        the system's unless a test pins one. Read ONLY to spell them: every decision reads
    ///        @p clock. Must outlive this.
    explicit EnrollmentWindow(core::platform::IClock const& clock,
                              NodeConditions* conditions = nullptr,
                              IMetricsSink* metrics = nullptr,
                              core::platform::WallClockRef wallClock = core::platform::defaultSystemWallClock()):
        _clock { clock },
        _conditions { conditions },
        _metrics { metrics },
        _wallClock { wallClock }
    {
        // NOT clear at construction: the list lives in the LEADER's memory alone, and this node does
        // not know yet whether it leads. A follower's empty list says nothing about who is waiting,
        // so both rows read not-evaluated until leadership says otherwise (`OnRoleChanged`).
        if (_conditions != nullptr)
        {
            _conditions->NotEvaluated(NodeCondition::EnrollmentWindowOpen, NotLeadingReason({}));
            _conditions->NotEvaluated(NodeCondition::EnrollmentRequestsWaiting, NotLeadingReason({}));
        }
    }

    EnrollmentWindow(EnrollmentWindow const&) = delete;
    EnrollmentWindow(EnrollmentWindow&&) = delete;
    EnrollmentWindow& operator=(EnrollmentWindow const&) = delete;
    EnrollmentWindow& operator=(EnrollmentWindow&&) = delete;
    ~EnrollmentWindow() = default;

    /// Record a request, or answer the decision already taken about it.
    ///
    /// A joiner POLLS, so this is called repeatedly for one id and `attempts` counts
    /// that. A repeat from a different peer host updates the recorded `peerId` rather
    /// than being refused -- see `EnrollmentPendingEntry::peerId` for why the two hosts
    /// are shown and never enforced against each other.
    ///
    /// **The KEY is the exception, and it is never refreshed** (#178). The first key an id
    /// asks under is the one the row holds and the one an approval admits; a later poll under
    /// the same id with another key is another machine, so it is counted in `claimsChanged`,
    /// answered `Pending`, and never recorded. Refreshing it would let whoever polled last
    /// replace the key an operator had just compared, between the list and the approval.
    /// The role is held the same way, for the same reason.
    ///
    /// No answer here is spent: an approved claim is answered `Approved` on every poll,
    /// because what it leads to is a roster, which is no secret (#178).
    ///
    /// Rows nobody polled for `PendingRowLifetime` are forgotten FIRST, so a machine that went
    /// away and came back is recorded afresh rather than answered from a stale row.
    /// @param claim What the joiner claims about itself.
    /// @param peerId The host the kernel says the request came from.
    /// @return What to answer.
    [[nodiscard]] EnrollDecision Offer(JoinerClaim const& claim, std::string_view peerId);

    /// Record the fingerprint of the roster just handed to @p nodeId.
    ///
    /// What `--enroll-list` shows beside the row, so the operator compares the fingerprint the
    /// leader SENT with the one the joiner printed. The latest one wins, because the latest is
    /// the one the joiner holds: it stops polling once it is told `Approved`.
    /// @param nodeId Who was handed it.
    /// @param fingerprint Its `Cluster::DigestOfRoster`.
    void NoteServed(std::string_view nodeId,
                    std::array<std::byte, CompileCacheWire::RosterFingerprintBytes> const& fingerprint);

    /// The entry carrying @p nodeId, as a copy.
    ///
    /// By value on purpose: the caller uses it to propose a membership change, which is
    /// consensus I/O, and holding this object's lock across that is exactly the shape
    /// this class avoids by being pure.
    /// @param nodeId Who to look for.
    /// @return The entry, or nothing.
    [[nodiscard]] std::optional<CompileCacheWire::EnrollmentPendingEntry> Find(std::string_view nodeId) const;

    /// Record a person's decision about one waiting id.
    ///
    /// @param nodeId Who was decided about.
    /// @param decision `Approved` or `Rejected`; anything else is a programmer error.
    /// @return What happened.
    [[nodiscard]] EnrollControlOutcome Decide(std::string_view nodeId, CompileCacheWire::EnrollmentDecision decision);

    /// Arm the auto-approve deadline `now + duration`, or re-arm it from now.
    ///
    /// **A DEADLINE, not a mode**: nothing wakes when it passes. Every `Offer` compares
    /// `now < deadline` when it arrives, and a lapsed one is cleared at the next call. It lives
    /// here and nowhere else -- never replicated, never persisted -- so a restart ends it, and a
    /// demotion ends it (`OnRoleChanged`): the operator armed a window on a LEADER, not on a
    /// cluster.
    /// @param duration How long from now; judged by `JudgeAutoApprove`.
    /// @return Nothing, or the rule that refuses the duration.
    [[nodiscard]] std::expected<void, AutoApproveRefusal> ArmAutoApprove(std::chrono::seconds duration);

    /// End the auto-approve deadline.
    /// @return `Done`, or `AlreadyInForce` when nothing was armed.
    [[nodiscard]] EnrollControlOutcome DisarmAutoApprove();

    /// Drop every row nobody has decided about: what `--enroll-clear` asks.
    ///
    /// Approved and rejected rows stay, because each is a RECORD of what a person decided -- the
    /// list an operator reads afterwards still says who was admitted and who was refused. A
    /// machine still asking is recorded again at its next poll, so this makes room rather than
    /// banning anybody: the per-host cap is what bounds a host that keeps asking.
    /// @return The ids dropped, in list order, for the caller to count and to log.
    [[nodiscard]] std::vector<std::string> ClearPending();

    /// How long the armed deadline has left.
    /// @return The whole seconds left, or nothing when none is armed or it has lapsed.
    [[nodiscard]] std::optional<std::chrono::seconds> AutoApproveLeft() const;

    /// Record on @p nodeId's row that it was admitted by the armed window, and when that window was
    /// armed, which is what `--enroll-list` shows beside it.
    /// @param nodeId Who was admitted.
    void MarkAutoApproved(std::string_view nodeId);

    /// Follow this node's scheduler role: any role but `Leader` ends an armed window and forgets
    /// the list.
    ///
    /// **And the rows follow the role, because the list lives in the leader's memory alone.** On the
    /// leader they are answered from the list; anywhere else they read NOT EVALUATED, naming the leader
    /// to ask -- never `clear`, which on a follower would be a statement about a list it does not hold.
    ///
    /// **At DEMOTION**, so a leader that loses leadership and regains it inside the deadline does
    /// not resume admitting -- a new leader never had a deadline, and this one was a new leader
    /// the moment it lost. The list goes for the same reason: a demoted node answers every
    /// enrollment verb `NotLeader`, so its rows are ones nobody can decide about. One method for `main` and for a test to
    /// call, so the wiring a test drives is the wiring that ships.
    /// @param role The role this node now holds.
    /// @param leaderEndpoint Where the leader answers, when one is known; empty otherwise.
    void OnRoleChanged(Distributed::SchedulerRole role, std::string_view leaderEndpoint);

    /// Why a node that does not lead answers the enrollment rows not-evaluated.
    /// @param leaderEndpoint Where the leader answers; empty when none is known.
    /// @return The reason, naming the leader when one is known.
    [[nodiscard]] static std::string NotLeadingReason(std::string_view leaderEndpoint);

    /// The window and everything waiting, as the wire reports it.
    /// @return The report, oldest entry first.
    [[nodiscard]] CompileCacheWire::EnrollmentReport Report() const;

    /// How the window decides a joiner, and how many are waiting -- for `NodeStatus`.
    ///
    /// Separate from `Report()` because the status path wants two numbers and not a
    /// copy of every row: `NodeStatus` is answered on every operator poll, and a node
    /// with a full list would otherwise copy 64 rows of strings to report a count.
    /// @return The state and the pending count.
    [[nodiscard]] std::pair<CompileCacheWire::WireEnrollmentState, std::uint32_t> Summary() const;

    /// The warning this list owes right now, if any, consuming it.
    ///
    /// **Pure and pulled rather than a thread of its own inside this class**, so the
    /// whole repeating-warning property is exercisable against a `core::platform::ManualClock`: a test
    /// advances the clock and asserts which calls answer. Whoever drives it decides how
    /// often to ask; asking more often than `EnrollmentWarningInterval` costs a lock and
    /// a comparison and answers nothing.
    ///
    /// Owed while any machine is WAITING -- undecided -- and not otherwise. The first is due
    /// the moment one starts waiting, not one interval later: a machine asking is the thing
    /// worth saying, and a person who started a join and watches the log for a minute before
    /// seeing anything concludes it did not work.
    /// @return The line to log, or nothing when none is due.
    [[nodiscard]] std::optional<std::string> TakeDueWarning();

  private:
    /// The entry carrying @p nodeId, or null. Caller holds `_mutex`.
    /// @param nodeId Who to look for.
    /// @return A pointer into `_pending`, or nullptr.
    [[nodiscard]] CompileCacheWire::EnrollmentPendingEntry* FindLocked(std::string_view nodeId) noexcept;

    /// Keep exactly the rows at @p kept, in that order, rebuilding every parallel list from them so
    /// they stay parallel. Caller holds `_mutex`.
    /// @param kept Indices into `_pending`, ascending.
    void KeepLocked(std::vector<std::size_t> const& kept) const;

    /// Forget every row nobody polled for `PendingRowLifetime`, counting each that nobody had
    /// decided about, and report the waiting rows again if any went. Caller holds `_mutex`.
    ///
    /// `const`, and over `mutable` rows, because a READ is where a lapse is noticed: `Report`
    /// and `Summary` answer an operator, and a list that still showed a machine that stopped
    /// asking an hour ago would be the stale answer this exists to prevent.
    void SweepLocked() const;

    /// Raise `EnrollmentRequestsWaiting` naming who waits, or clear it when nobody does.
    /// Caller holds `_mutex`.
    void ReportWaitingLocked() const;

    /// Say the auto-approve window is shut: `clear` on the leader, not-evaluated anywhere else.
    /// The caller holds `_mutex`.
    void ReportWindowClosedLocked() const;

    /// Clear an auto-approve deadline that has passed, and its condition. Caller holds `_mutex`.
    /// `const` over `mutable` state for `SweepLocked`'s reason: a read is where a lapse is noticed.
    void LapseLocked() const;

    /// Whether an armed deadline has not passed. Caller holds `_mutex`, and has lapsed it.
    /// @return True while `now < deadline`.
    [[nodiscard]] bool AutoApprovingLocked() const noexcept;

    /// Whole seconds until the armed deadline, zero when none is armed or it has passed. Caller
    /// holds `_mutex`.
    /// @return The seconds left.
    [[nodiscard]] std::uint32_t SecondsUntilLocked() const noexcept;

    /// Seconds from @p since to now, floored at zero.
    /// @param since The earlier instant.
    /// @return The elapsed whole seconds.
    [[nodiscard]] std::uint64_t SecondsSince(core::platform::SteadyTimePoint since) const noexcept;

    core::platform::IClock const& _clock;

    /// Where the list's state is reported as conditions; null on a node that serves none.
    /// Written under `_mutex`, so two racing changes cannot report in the other order.
    NodeConditions* _conditions;

    /// Whether this node leads, as the scheduler last said (`OnRoleChanged`). Guarded by `_mutex`.
    bool _leading { false };

    /// Where the leader answers, when this node does not lead and one is known. Guarded by `_mutex`.
    std::string _leaderEndpoint;

    /// Where a row forgotten undecided is counted; null where nothing counts it.
    IMetricsSink* _metrics;

    /// Where the instants an armed window's condition names come from. A `WallClockRef`, which
    /// refuses a temporary, never a raw pointer to one (#1032, `ctest -R wall-clock-borrow`).
    core::platform::WallClockRef _wallClock;

    mutable std::mutex _mutex;

    /// Whether the warning is armed: a machine has been waiting since the last time nobody was.
    /// Guarded by `_mutex`.
    bool _warning { false };

    /// When the next warning falls due; meaningless while `_warning` is false. Guarded by `_mutex`.
    core::platform::SteadyTimePoint _warnDueAt {};

    /// Who asked, oldest first. Guarded by `_mutex`; `mutable` for `SweepLocked`.
    ///
    /// A vector rather than a map, and insertion order rather than a sort: 64 rows is
    /// nothing to scan, and the ORDER is what the operator reads -- oldest first, so the
    /// machine that has been waiting longest is the one at the top of the list.
    ///
    /// It holds the entries in the same shape the wire reports them, minus the ages,
    /// which are computed at render time because a duration on a report is not a state.
    mutable std::vector<CompileCacheWire::EnrollmentPendingEntry> _pending;

    /// When each entry first asked, parallel to `_pending`. Guarded by `_mutex`.
    ///
    /// Beside the list rather than inside `EnrollmentPendingEntry`, because that type is
    /// the WIRE's and carries an age in seconds: a `core::platform::SteadyTimePoint` on it would be an instant
    /// on a report, which is the shape this project refuses -- a receiver differences it
    /// against its own clock, which is a different clock.
    mutable std::vector<core::platform::SteadyTimePoint> _firstSeen;

    /// When each entry last asked, parallel to `_pending`, for `PendingRowLifetime`. Guarded by
    /// `_mutex`, and beside the list for `_firstSeen`'s reason.
    mutable std::vector<core::platform::SteadyTimePoint> _lastSeen;

    /// When the window that admitted each entry was armed, parallel to `_pending`; absent for a
    /// row nobody's deadline admitted. Guarded by `_mutex`, beside the list for `_firstSeen`'s
    /// reason.
    mutable std::vector<std::optional<core::platform::SteadyTimePoint>> _autoApprovedArmedAt;

    /// The armed auto-approve deadline, or nothing. Guarded by `_mutex`; `mutable` for `LapseLocked`.
    mutable std::optional<core::platform::SteadyTimePoint> _autoApproveUntil;

    /// When the armed deadline was armed; meaningful only while `_autoApproveUntil` is. Guarded by
    /// `_mutex`.
    core::platform::SteadyTimePoint _autoApproveArmedAt {};
};

} // namespace FastCache::Node
