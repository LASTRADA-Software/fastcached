// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
/// and this is the one state in which this machine hands its cluster's key to a
/// stranger that asked. Sixty seconds is short enough that a window left open over a
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
    Pending,  ///< Recorded and waiting for a person.
    Approved, ///< A person approved it; the caller hands the key over. **Reachable once per id.**

    /// This id already collected the key. Refused, and **no key bytes are returned**.
    ///
    /// Distinct from `Approved` rather than folded into it, and that distinction is the
    /// whole of what makes a grant spendable ONCE: while the two shared an enumerator
    /// the responder served the key on every poll and only the counter was gated, so a
    /// second hand-over was invisible -- the tally stayed at one and the real joiner
    /// still got its key on its next poll, which is what an ordinary enrolment looks
    /// like to an operator.
    ///
    /// **The cost of this is real and was accepted rather than argued away.** A joiner
    /// whose reply was lost is on a machine an operator has already admitted, and it is
    /// now stranded until somebody approves it again. The idempotent reading that would
    /// have spared it is honest, and it loses on the trade: one operator action against
    /// handing the fleet's pre-shared key to anybody who can route to this port and name
    /// an id that was approved. `peerId` cannot close the gap instead -- #242 settled
    /// that comparing hosts refuses the documented setup (DNS names, a node dialling
    /// itself, NAT, VPN, multi-homing) and stops only a third host -- so the spend is
    /// what carries it, exactly as a lease grant's is (#614). Refused-until-somebody-
    /// acts is also the direction this codebase fails in by preference: self-healing,
    /// and visible from the machine being refused.
    Collected,

    Rejected, ///< A person refused it.
};

/// What giving an unserved claim back did.
///
/// Its own enum rather than `EnrollControlOutcome`, because the question is not an
/// operator's: those enumerators answer *what did your command do*, and none of them
/// can honestly carry *the row moved underneath me*. Reusing `AlreadyInForce` for that
/// would be a name that reads as reassurance for the one outcome an operator must act
/// on.
///
/// A private enum: nothing transmits or persists these ordinals.
enum class ClaimReturn : std::uint8_t
{
    /// The row was still `Collected` and is `Approved` again. The joiner's next poll
    /// collects, and nothing was lost.
    Returned,

    /// No row of that id is on the list any more -- the window was closed, most
    /// plausibly, which also forgets every pending row.
    NoSuchRow,

    /// The row is on the list and is not `Collected`: somebody decided about it between
    /// the claim being taken and the key read failing. Rare, and the case worth saying
    /// out loud, since the machine is left holding a consumed collection.
    AlreadyMoved,
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
/// exercised against a `ManualClock`.
class EnrollmentWindow
{
  public:
    /// @param clock Where "now" comes from; must outlive this.
    explicit EnrollmentWindow(IClock const& clock) noexcept:
        _clock { clock }
    {
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
    /// @param nodeId The identity the joiner claims.
    /// @param raftEndpoint The consensus address it claims.
    /// @param peerId The host the kernel says the request came from.
    /// @return What to answer. `Approved` is returned at most once per approval, and the
    ///         caller either serves the key or calls `ReturnClaim`.
    [[nodiscard]] EnrollDecision Offer(std::string_view nodeId, std::string_view raftEndpoint, std::string_view peerId);

    /// Give back the one collection an `Approved` answer took, because it was not served.
    ///
    /// **The other half of the spend, and it exists so a transient fault does not cost a
    /// machine its only collection.** `Offer` takes the transition before the caller has
    /// the key in hand -- it has to, or two polls arriving together both get served --
    /// so a caller that cannot then read the key file must put it back. Without this, a
    /// filesystem permission error lasting one second leaves a joiner that can never
    /// collect and an operator with no signal saying why.
    ///
    /// Only a row still `Collected` is returned, so a decision an operator took in
    /// between is never overwritten by a late failure path.
    ///
    /// **It says WHICH nothing it did, and the caller reads it.** A `bool` here was
    /// discarded at its one call site, which made it a claim with no reader -- and the
    /// unread value is the one that matters: anything but `Returned` means the repair
    /// ITSELF failed, leaving a machine holding a consumed collection with nothing
    /// saying so. That is the state this function exists to prevent, arriving through
    /// its own fix.
    /// @param nodeId The id whose claim is being returned.
    /// @return What happened, so a caller can report the case where nothing did.
    [[nodiscard]] ClaimReturn ReturnClaim(std::string_view nodeId);

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
    /// whole repeating-warning property is exercisable against a `ManualClock`: a test
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
    [[nodiscard]] std::uint64_t SecondsSince(TimePoint since) const noexcept;

    IClock const& _clock;

    mutable std::mutex _mutex;

    /// Whether the window is taking requests. Guarded by `_mutex`.
    bool _open { false };

    /// When it was opened; meaningless while closed. Guarded by `_mutex`.
    TimePoint _openedAt {};

    /// When the next warning falls due. Guarded by `_mutex`.
    TimePoint _warnDueAt {};

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
    /// the WIRE's and carries an age in seconds: a `TimePoint` on it would be an instant
    /// on a report, which is the shape this project refuses -- a receiver differences it
    /// against its own clock, which is a different clock.
    std::vector<TimePoint> _firstSeen;
};

} // namespace FastCache::Node
