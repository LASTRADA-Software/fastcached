// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
/// exists to express rather than an arbitrary size. An open window is ungated by
/// design -- the explicit approve is the gate -- so anybody who can reach the port can
/// add rows; evicting to make room would let a flooder push the real joiner off the
/// list the operator is reading, which is silent from both ends and undetectable
/// afterwards. A full list refuses by name, counts, and is visible.
///
/// Sixty-four rather than a number scaled to the fleet, because the list is what a
/// PERSON reads before deciding, and a list longer than a terminal is a list nobody
/// checks a mismatch against. A rollout larger than this is approved in batches, which
/// is what an operator does anyway.
inline constexpr std::size_t MaxPendingEnrollments = 64;

/// How often an open window says so again.
///
/// **Repeating rather than one line at open**, because a one-shot line scrolls away
/// and this is the one state in which anybody who can reach this machine may put a row
/// on the list an operator approves from. Sixty seconds is short enough that a window left open over a
/// lunch break is unmissable in the log and long enough that it is not itself noise.
inline constexpr std::chrono::seconds EnrollmentWarningInterval { 60 };

/// What the window decided about one `Enroll`.
///
/// A private enum: nothing transmits or persists these ordinals. The wire's answer is
/// `CompileCacheWire::EnrollOutcome` for the three that are replies and an
/// `ErrorCode` for the two that are refusals, and the mapping is the responder's.
enum class EnrollDecision : std::uint8_t
{
    Closed,   ///< No window is open. Refused.
    Full,     ///< The list is full and this id is not already on it. Refused, nothing recorded.
    Pending,  ///< Recorded and waiting for a person -- or asked under a key nobody decided about.
    Approved, ///< A person approved exactly this claim; the caller hands over the roster.
    Rejected, ///< A person refused it.
};

/// What an enrollment ROLE means on this node (#178).
struct EnrollRoleRow
{
    CompileCacheWire::EnrollRole role; ///< The wire role.
    std::string_view name;             ///< Its one spelling in a list and in a sentence.

    /// Whether a request in this role states a consensus endpoint: a member must, because an
    /// id with no address is a member the cluster counts and cannot reach, and a worker must
    /// NOT, because a principal has no address anybody dials.
    bool statesEndpoint;

    /// The principal role an approval records, or absent for a member, which `ClusterAdmit`
    /// records instead.
    std::optional<Cluster::PrincipalRole> principal;
};

/// One row per role this build implements.
///
/// A plain array rather than an `EnumTable`, for `KnownEnrollmentDecisions`' reason: a WIRE enum
/// carries no `Last`. Completeness is asserted against `KnownEnrollRoles` instead.
inline constexpr std::array EnrollRoleTable {
    EnrollRoleRow {
        .role = CompileCacheWire::EnrollRole::Member, .name = "member", .statesEndpoint = true, .principal = std::nullopt },
    EnrollRoleRow { .role = CompileCacheWire::EnrollRole::Worker,
                    .name = "worker",
                    .statesEndpoint = false,
                    .principal = Cluster::PrincipalRole::Worker },
};

/// Whether every role this build knows has exactly one row.
/// @return True when `EnrollRoleTable` is complete and has no duplicate.
[[nodiscard]] consteval bool EveryEnrollRoleHasARow() noexcept
{
    return std::ranges::all_of(CompileCacheWire::KnownEnrollRoles, [](CompileCacheWire::EnrollRole role) {
        return std::ranges::count(EnrollRoleTable, role, &EnrollRoleRow::role) == 1;
    });
}

static_assert(EveryEnrollRoleHasARow(), "every enrollment role needs one EnrollRoleTable row");

/// The row for @p role.
/// @param role A role the decoder accepted.
/// @return Its row; the first row for a value no row names, which the assertion above makes unreachable.
[[nodiscard]] constexpr EnrollRoleRow const& EnrollRoleRowFor(CompileCacheWire::EnrollRole role) noexcept
{
    for (auto const& row: EnrollRoleTable)
        if (row.role == role)
            return row;
    return EnrollRoleTable.front();
}

/// Whether @p roster records @p nodeId holding @p key in @p role -- as a member for a member, and
/// as a principal in the role's principal role for anything else.
///
/// What the leader asks of its own roster before it answers `Approved`, and what the joiner asks
/// of the roster it is handed before it believes it was admitted: one question, so the two ends
/// cannot come to mean different things by "admitted". A member row with no key recorded records
/// no key, and a principal is not a member.
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
    std::string_view nodeId;       ///< The identity it claims.
    std::string_view raftEndpoint; ///< The consensus address it claims; empty for a worker.
    CompileCacheWire::EnrollRole role { CompileCacheWire::EnrollRole::Member };   ///< What it asks to be.
    std::array<std::byte, CompileCacheWire::IdentityPublicKeyBytes> publicKey {}; ///< The key it asks under.
};

/// What an operator's decision did.
///
/// A private enum: nothing transmits these ordinals. The responder maps them onto wire
/// codes, which is where the operator-facing vocabulary lives.
enum class EnrollControlOutcome : std::uint8_t
{
    Done,           ///< Applied.
    AlreadyInForce, ///< Nothing to do: the window was already open, or the id already decided this way.
    UnknownSubject, ///< No pending entry carries that id.
    Closed,         ///< A decision was asked for while no window is open.
};

/// The runtime enrollment window: who is asking to join, and what a person decided.
///
/// **In memory, not a flag and not replicated, and each of those is a decision.**
///
/// Not a flag, because `--install-service` registers a command line and replays it
/// forever: a permanently-open window baked into forty unit files is worse than the
/// hand-placement of forty keys it exists to replace.
///
/// Not replicated, because the window is one machine's willingness to answer a
/// question for the next few minutes, while the ADMISSION it leads to is replicated
/// through the existing `ClusterAdmit` path and outlives everything. Committing the
/// window would make a forgotten one survive every reboot of every member.
///
/// **A restart therefore closes it, and that is the absence of persistence rather than
/// a second closing mechanism.** The log line at open says so, because an operator who
/// does not know it will look for a way to close a window that is already shut.
///
/// Thread-safe: the responder reaches it from a reactor thread and the warning ticker
/// from its own, so every method takes the lock. The decisions are pure with respect to
/// I/O -- no socket, no file, no consensus -- which is what lets the whole of this be
/// exercised against a `core::platform::ManualClock`.
class EnrollmentWindow
{
  public:
    /// @param clock Where "now" comes from; must outlive this.
    /// @param conditions Where an open window is RAISED and a closed one cleared (#1364), or null on
    ///        a node that serves no window -- whose row its scope then answers, rather than this
    ///        object reporting a reassuring `clear` for a window nothing can open. Must outlive this.
    explicit EnrollmentWindow(core::platform::IClock const& clock, NodeConditions* conditions = nullptr):
        _clock { clock },
        _conditions { conditions }
    {
        // Closed at construction, and checked rather than assumed: a window is held in memory
        // and nowhere else, so a process starts with none open.
        if (_conditions != nullptr)
            _conditions->Clear(NodeCondition::EnrollmentWindowOpen);
    }

    EnrollmentWindow(EnrollmentWindow const&) = delete;
    EnrollmentWindow(EnrollmentWindow&&) = delete;
    EnrollmentWindow& operator=(EnrollmentWindow const&) = delete;
    EnrollmentWindow& operator=(EnrollmentWindow&&) = delete;
    ~EnrollmentWindow() = default;

    /// Start accepting requests.
    ///
    /// Opening an open window changes nothing and says so, rather than restarting its
    /// age: an operator who runs the verb twice has not asked for the warning clock to
    /// be reset, and resetting it would let a window be held open indefinitely without
    /// its age ever growing.
    /// @return `Done`, or `AlreadyInForce` when it was already open.
    [[nodiscard]] EnrollControlOutcome Open();

    /// Stop accepting requests, and forget everything that was waiting.
    ///
    /// **The pending list goes with it**, which is the same statement the restart makes:
    /// the list is a record of who asked during one window, and carrying it into the
    /// next one would let a request made before an operator was watching be approved
    /// after they stopped.
    /// @return `Done`, or `AlreadyInForce` when it was already closed.
    [[nodiscard]] EnrollControlOutcome Close();

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

    /// The window and everything waiting, as the wire reports it.
    /// @return The report, oldest entry first.
    [[nodiscard]] CompileCacheWire::EnrollmentReport Report() const;

    /// Whether the window is open, and how many are waiting -- for `NodeStatus`.
    ///
    /// Separate from `Report()` because the status path wants two numbers and not a
    /// copy of every row: `NodeStatus` is answered on every operator poll, and a node
    /// with a full list would otherwise copy 64 rows of strings to report a count.
    /// @return The state and the pending count.
    [[nodiscard]] std::pair<CompileCacheWire::WireEnrollmentState, std::uint32_t> Summary() const;

    /// The warning this window owes right now, if any, consuming it.
    ///
    /// **Pure and pulled rather than a thread of its own inside this class**, so the
    /// whole repeating-warning property is exercisable against a `core::platform::ManualClock`: a test
    /// advances the clock and asserts which calls answer. Whoever drives it decides how
    /// often to ask; asking more often than `EnrollmentWarningInterval` costs a lock and
    /// a comparison and answers nothing.
    ///
    /// The first warning is due at the moment the window OPENS, not one interval later:
    /// the open itself is the thing worth saying, and an operator who opens a window and
    /// watches the log for a minute before seeing anything concludes it did not work.
    /// @return The line to log, or nothing when none is due.
    [[nodiscard]] std::optional<std::string> TakeDueWarning();

  private:
    /// The entry carrying @p nodeId, or null. Caller holds `_mutex`.
    /// @param nodeId Who to look for.
    /// @return A pointer into `_pending`, or nullptr.
    [[nodiscard]] CompileCacheWire::EnrollmentPendingEntry* FindLocked(std::string_view nodeId) noexcept;

    /// Seconds from @p since to now, floored at zero.
    /// @param since The earlier instant.
    /// @return The elapsed whole seconds.
    [[nodiscard]] std::uint64_t SecondsSince(core::platform::SteadyTimePoint since) const noexcept;

    core::platform::IClock const& _clock;

    /// Where the window's state is reported as a condition; null on a node that serves none.
    /// Written under `_mutex`, so a racing open and close cannot report in the other order.
    NodeConditions* _conditions;

    mutable std::mutex _mutex;

    /// Whether the window is taking requests. Guarded by `_mutex`.
    bool _open { false };

    /// When it was opened; meaningless while closed. Guarded by `_mutex`.
    core::platform::SteadyTimePoint _openedAt {};

    /// When the next warning falls due. Guarded by `_mutex`.
    core::platform::SteadyTimePoint _warnDueAt {};

    /// Who is waiting, oldest first. Guarded by `_mutex`.
    ///
    /// A vector rather than a map, and insertion order rather than a sort: 64 rows is
    /// nothing to scan, and the ORDER is what the operator reads -- oldest first, so the
    /// machine that has been waiting longest is the one at the top of the list.
    ///
    /// It holds the entries in the same shape the wire reports them, minus the ages,
    /// which are computed at render time because a duration on a report is not a state.
    std::vector<CompileCacheWire::EnrollmentPendingEntry> _pending;

    /// When each entry first asked, parallel to `_pending`. Guarded by `_mutex`.
    ///
    /// Beside the list rather than inside `EnrollmentPendingEntry`, because that type is
    /// the WIRE's and carries an age in seconds: a `core::platform::SteadyTimePoint` on it would be an instant
    /// on a report, which is the shape this project refuses -- a receiver differences it
    /// against its own clock, which is a different clock.
    std::vector<core::platform::SteadyTimePoint> _firstSeen;
};

} // namespace FastCache::Node
