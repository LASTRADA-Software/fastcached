// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

bool RosterRecordsJoiner(Cluster::Roster const& roster,
                         std::string_view nodeId,
                         Ed25519PublicKey const& key,
                         Wire::EnrollRole role)
{
    auto const& row = EnrollRoleRowFor(role);
    if (row.principal.has_value())
        return std::ranges::any_of(roster.principals, [&](Cluster::ClusterPrincipal const& principal) {
            return principal.id == nodeId && principal.publicKey == key && principal.role == *row.principal;
        });
    return std::ranges::any_of(roster.members, [&](Cluster::RosterMember const& member) {
        return member.id == nodeId && member.publicKey == std::optional { key };
    });
}

EnrollDecision EnrollmentWindow::Offer(JoinerClaim const& claim, std::string_view peerId)
{
    std::scoped_lock const guard { _mutex };
    // Before the lookup, so a machine that went away and came back is a new row rather than an
    // answer read off one nobody has been asking about.
    SweepLocked();
    LapseLocked();
    if (auto* const existing = FindLocked(claim.nodeId); existing != nullptr)
    {
        ++existing->attempts;
        // Every poll keeps the row, whatever it claims: the lifetime is about a machine that
        // stopped ASKING, and one asking under another key is still somebody asking.
        _lastSeen[static_cast<std::size_t>(existing - _pending.data())] = _clock.now();

        // **The key and the role are the row's, whatever is asked later** (#178). A poll under
        // this id with another key is ANOTHER MACHINE: counted, answered `Pending`, never
        // recorded, and never handed what an approval of the first one leads to. Refreshing
        // them would let whoever polled last swap the key an operator compared on the list for
        // one they never saw, in the minutes between `--enroll-list` and `--enroll-approve`.
        auto const sameMachine = existing->publicKey == claim.publicKey && existing->role == claim.role;
        if (!sameMachine)
        {
            ++existing->claimsChanged;
            return EnrollDecision::Pending;
        }

        // **A row a person has decided about is a RECORD of what they decided about,
        // and stops tracking the machine.** The claim refreshes only while the row is
        // still `Pending`.
        //
        // The earlier reading refreshed unconditionally, on the honest argument that an
        // operator should read whichever address was most recently true. That argument
        // is about DISPLAY and it does not carry to what gets COMMITTED: `--enroll-list`
        // and `--enroll-approve` are two one-shot processes minutes apart and a joiner
        // polls every couple of seconds in between, so the endpoint `ClusterAdmit` was
        // handed was not necessarily the endpoint the eye that approved it had read.
        // The gate here is a person looking at a list, so the value they acted on has
        // to be the value that travels.
        //
        // A machine that has genuinely moved is therefore refused and enrolled again --
        // one operator action, and the same answer `--cluster-admit` already gives for
        // recording a move.
        if (existing->decision == Wire::EnrollmentDecision::Pending)
        {
            existing->nodeEndpoint = std::string { claim.nodeEndpoint };
            existing->peerId = std::string { peerId };
        }
        else if (existing->nodeEndpoint != claim.nodeEndpoint || existing->peerId != peerId)
        {
            // Marked and counted, never gated. #242 settled that comparing hosts refuses
            // the documented setup -- DNS names, a node dialling itself, NAT, VPN,
            // multi-homing -- and stops only a third host, so a disagreement is shown to
            // the person rather than acted on by the node. What makes it worth showing
            // is that it is also the signature of somebody else claiming a decided id.
            ++existing->claimsChanged;
        }

        switch (existing->decision)
        {
            case Wire::EnrollmentDecision::Pending:
                // The comparison, when the joiner asks: nothing wakes when the deadline passes.
                // Only the machine that asked FIRST under this id is reached here -- a later key
                // was answered above -- so a `claimsChanged` poll is never auto-approved.
                return AutoApprovingLocked() ? EnrollDecision::AutoApprove : EnrollDecision::Pending;
            case Wire::EnrollmentDecision::Approved:
                // Every poll, and nothing is spent: what an approval leads to is the roster,
                // which is no secret (#178). A joiner whose reply was lost simply asks again,
                // which is the whole recovery path and needs no operator.
                return EnrollDecision::Approved;
            case Wire::EnrollmentDecision::Rejected:
                return EnrollDecision::Rejected;
        }
        return EnrollDecision::Pending;
    }

    // The bounds are asked only of a request that would ADD a row. A machine already on
    // the list goes on polling whether or not the list is full, which is what keeps a
    // flood from silencing the joiners that arrived before it.
    //
    // The HOST's bound first: a host at its cap is told so, and counted as itself, whether or
    // not the list as a whole has room -- which keeps `Full` meaning *many hosts are waiting*
    // rather than *one host is asking a lot*, the two answers an operator acts on differently.
    // Undecided rows only: a decided one is a record, and its machine has its answer.
    //
    // By the address each row was CREATED from (`firstPeerId`), never the displayed one a later
    // poll refreshes: re-polling rows from a second address must not move them out of the first
    // one's bound. On the row itself rather than beside it, so no sweep or clear can leave the
    // bound charging a row to another row's host.
    auto const host = UnmappedHost(peerId);
    auto const fromHost =
        static_cast<std::size_t>(std::ranges::count_if(_pending, [host](Wire::EnrollmentPendingEntry const& row) {
            return row.firstPeerId == host && row.decision == Wire::EnrollmentDecision::Pending;
        }));
    if (fromHost >= MaxPendingEnrollmentsPerHost)
        return EnrollDecision::HostFull;
    if (_pending.size() >= MaxPendingEnrollments)
        return EnrollDecision::Full;

    _pending.push_back(Wire::EnrollmentPendingEntry { .nodeId = std::string { claim.nodeId },
                                                      .nodeEndpoint = std::string { claim.nodeEndpoint },
                                                      .peerId = std::string { peerId },
                                                      .firstSeenSecondsAgo = 0,
                                                      .attempts = 1,
                                                      .claimsChanged = 0,
                                                      .decision = Wire::EnrollmentDecision::Pending,
                                                      .role = claim.role,
                                                      .publicKey = claim.publicKey,
                                                      .rosterFingerprint = std::nullopt,
                                                      .autoApprovedArmedSecondsAgo = std::nullopt,
                                                      .firstPeerId = std::string { host } });
    _firstSeen.push_back(_clock.now());
    _lastSeen.push_back(_clock.now());
    _autoApprovedArmedAt.emplace_back(std::nullopt);
    ReportWaitingLocked();
    return AutoApprovingLocked() ? EnrollDecision::AutoApprove : EnrollDecision::Pending;
}

void EnrollmentWindow::NoteServed(std::string_view nodeId,
                                  std::array<std::byte, Wire::RosterFingerprintBytes> const& fingerprint)
{
    std::scoped_lock const guard { _mutex };
    // A row that went -- forgotten between the answer and this -- has nobody to show it to, so
    // there is nothing to record and nothing to report.
    if (auto* const entry = FindLocked(nodeId); entry != nullptr)
        entry->rosterFingerprint = fingerprint;
}

std::optional<Wire::EnrollmentPendingEntry> EnrollmentWindow::Find(std::string_view nodeId) const
{
    std::scoped_lock const guard { _mutex };
    SweepLocked();
    auto const found = std::ranges::find(_pending, nodeId, &Wire::EnrollmentPendingEntry::nodeId);
    if (found == _pending.end())
        return std::nullopt;

    auto copy = *found;
    copy.firstSeenSecondsAgo = SecondsSince(_firstSeen[static_cast<std::size_t>(found - _pending.begin())]);
    return copy;
}

EnrollControlOutcome EnrollmentWindow::Decide(std::string_view nodeId, Wire::EnrollmentDecision decision)
{
    std::scoped_lock const guard { _mutex };
    auto* const entry = FindLocked(nodeId);
    if (entry == nullptr)
        return EnrollControlOutcome::UnknownSubject;

    if (entry->decision == decision)
        return EnrollControlOutcome::AlreadyInForce;

    // A `Rejected` entry may be approved afterwards: an operator who refused the wrong
    // row and corrected it would otherwise have to wait for the row to be forgotten and
    // the joiner to ask again.
    entry->decision = decision;
    ReportWaitingLocked();
    return EnrollControlOutcome::Done;
}

Wire::EnrollmentReport EnrollmentWindow::Report() const
{
    std::scoped_lock const guard { _mutex };
    SweepLocked();
    LapseLocked();
    auto const armed = AutoApprovingLocked();
    Wire::EnrollmentReport report { .state =
                                        armed ? Wire::WireEnrollmentState::AutoApprove : Wire::WireEnrollmentState::Manual,
                                    .autoApproveSecondsLeft = armed ? SecondsUntilLocked() : 0,
                                    .pending = _pending };
    for (auto const index: std::views::iota(std::size_t { 0 }, report.pending.size()))
    {
        report.pending[index].firstSeenSecondsAgo = SecondsSince(_firstSeen[index]);
        report.pending[index].autoApprovedArmedSecondsAgo =
            _autoApprovedArmedAt[index].transform([this](core::platform::SteadyTimePoint at) { return SecondsSince(at); });
    }
    return report;
}

std::pair<Wire::WireEnrollmentState, std::uint32_t> EnrollmentWindow::Summary() const
{
    std::scoped_lock const guard { _mutex };
    SweepLocked();
    LapseLocked();
    return { AutoApprovingLocked() ? Wire::WireEnrollmentState::AutoApprove : Wire::WireEnrollmentState::Manual,
             static_cast<std::uint32_t>(_pending.size()) };
}

std::optional<std::string> EnrollmentWindow::TakeDueWarning()
{
    std::scoped_lock const guard { _mutex };
    // The deadline too, because this tick is the one call a quiet leader makes: nobody polls or
    // lists, and without it an Alert would go on saying a window is open long after it shut.
    LapseLocked();
    SweepLocked();

    auto const waiting =
        std::ranges::count(_pending, Wire::EnrollmentDecision::Pending, &Wire::EnrollmentPendingEntry::decision);
    if (waiting == 0)
    {
        // Disarmed, so the next machine to ask is said at once rather than an interval after
        // the last line about somebody else.
        _warning = false;
        return std::nullopt;
    }

    auto const now = _clock.now();
    if (!_warning)
    {
        _warning = true;
        _warnDueAt = now;
    }
    if (now < _warnDueAt)
        return std::nullopt;

    // Advanced from NOW rather than by adding an interval to the old deadline: a driver
    // that was late, or a clock that jumped, must not then owe a burst of warnings for
    // the intervals it slept through. The reading an operator wants is *somebody is still
    // waiting*, and one line per interval of real time is what says that.
    _warnDueAt = now + EnrollmentWarningInterval;

    // The oldest first-seen of the waiting rows, which is how long somebody has been waiting.
    auto oldest = std::uint64_t { 0 };
    auto ids = std::string {};
    for (auto const index: std::views::iota(std::size_t { 0 }, _pending.size()))
    {
        if (_pending[index].decision != Wire::EnrollmentDecision::Pending)
            continue;
        oldest = std::max(oldest, SecondsSince(_firstSeen[index]));
        ids += ids.empty() ? _pending[index].nodeId : std::format(", {}", _pending[index].nodeId);
    }
    return std::format(
        "{} machine(s) waiting to join this cluster, the longest for {}s: {}. Approve one with "
        "--enroll-approve=<id>@<key> only after comparing the key --enroll-list shows for it with the one that "
        "machine printed -- an approval admits exactly that key. {} listed in total.",
        waiting,
        oldest,
        ids,
        _pending.size());
}

void EnrollmentWindow::SweepLocked() const
{
    auto const now = _clock.now();
    auto kept = std::vector<std::size_t> {};
    kept.reserve(_pending.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, _pending.size()))
    {
        if (now - _lastSeen[index] <= PendingRowLifetime)
        {
            kept.push_back(index);
            continue;
        }
        // Counted only while nobody had decided about it: that is a request the leader forgot
        // with an operator never having answered it, which is the event worth a tally. A row
        // somebody decided about goes quietly -- its machine stopped asking because it had
        // its answer.
        if (_metrics != nullptr && _pending[index].decision == Wire::EnrollmentDecision::Pending)
            _metrics->Increment(IMetricsSink::Counter::EnrollmentRequestsExpired);
    }
    if (kept.size() == _pending.size())
        return;
    KeepLocked(kept);
    ReportWaitingLocked();
}

void EnrollmentWindow::KeepLocked(std::vector<std::size_t> const& kept) const
{
    auto const pick = [&kept](auto const& from) {
        auto out = std::remove_cvref_t<decltype(from)> {};
        out.reserve(kept.size());
        for (auto const index: kept)
            out.push_back(from[index]);
        return out;
    };
    _pending = pick(_pending);
    _firstSeen = pick(_firstSeen);
    _lastSeen = pick(_lastSeen);
    _autoApprovedArmedAt = pick(_autoApprovedArmedAt);
}

std::vector<std::string> EnrollmentWindow::ClearPending()
{
    std::scoped_lock const guard { _mutex };
    auto kept = std::vector<std::size_t> {};
    auto cleared = std::vector<std::string> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, _pending.size()))
    {
        if (_pending[index].decision == Wire::EnrollmentDecision::Pending)
            cleared.push_back(_pending[index].nodeId);
        else
            kept.push_back(index);
    }
    if (!cleared.empty())
    {
        KeepLocked(kept);
        ReportWaitingLocked();
    }
    return cleared;
}

void EnrollmentWindow::ReportWaitingLocked() const
{
    if (_conditions == nullptr)
        return;
    auto ids = std::string {};
    auto waiting = std::size_t { 0 };
    for (auto const& row: _pending)
    {
        if (row.decision != Wire::EnrollmentDecision::Pending)
            continue;
        ++waiting;
        ids += ids.empty() ? row.nodeId : std::format(", {}", row.nodeId);
    }
    if (waiting == 0)
    {
        _conditions->Clear(NodeCondition::EnrollmentRequestsWaiting);
        return;
    }
    _conditions->Raise(
        NodeCondition::EnrollmentRequestsWaiting,
        std::format("{} machine(s) waiting: {} -- approve with --enroll-approve=<id>@<key> after comparing its "
                    "key with --enroll-list",
                    waiting,
                    ids));
}

std::expected<void, AutoApproveRefusal> EnrollmentWindow::ArmAutoApprove(std::chrono::seconds duration)
{
    if (auto const refusal = JudgeAutoApprove(duration); refusal.has_value())
        return std::unexpected { *refusal };

    std::scoped_lock const guard { _mutex };
    auto const now = _clock.now();
    _autoApproveUntil = now + duration;
    _autoApproveArmedAt = now;
    // Live, and alert: this is the one state in which anybody who can reach the port is admitted
    // under the key they ask with, with nobody comparing it.
    //
    // The detail names the DEADLINE and when it was armed as absolute instants, never the time
    // left: the detail is a snapshot, set here and read for as long as the condition stands, and
    // "N min left" would go on claiming N minutes. `--node-status` computes the seconds left.
    //
    // Whole seconds, rounded OUTWARD: the deadline up and the arming down, so the stated window
    // is never shorter than the one that admits. Flooring the deadline stated it up to a second
    // early, while the steady deadline above still admitted until the fraction ran out.
    if (_conditions != nullptr)
    {
        auto const wallNow = _wallClock.now();
        auto const armedAt = std::chrono::floor<std::chrono::seconds>(wallNow);
        auto const until = std::chrono::ceil<std::chrono::seconds>(wallNow + duration);
        _conditions->Raise(NodeCondition::EnrollmentWindowOpen,
                           std::format("auto-approve until {:%FT%TZ} (armed {:%FT%TZ}); any machine that asks is "
                                       "admitted as a learner under the key it asks with, until then",
                                       until,
                                       armedAt));
    }
    return {};
}

EnrollControlOutcome EnrollmentWindow::DisarmAutoApprove()
{
    std::scoped_lock const guard { _mutex };
    LapseLocked();
    if (!_autoApproveUntil.has_value())
        return EnrollControlOutcome::AlreadyInForce;
    _autoApproveUntil.reset();
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::EnrollmentWindowOpen);
    return EnrollControlOutcome::Done;
}

std::optional<std::chrono::seconds> EnrollmentWindow::AutoApproveLeft() const
{
    std::scoped_lock const guard { _mutex };
    LapseLocked();
    if (!AutoApprovingLocked())
        return std::nullopt;
    return std::chrono::seconds { SecondsUntilLocked() };
}

void EnrollmentWindow::MarkAutoApproved(std::string_view nodeId)
{
    std::scoped_lock const guard { _mutex };
    if (auto* const entry = FindLocked(nodeId); entry != nullptr)
        _autoApprovedArmedAt[static_cast<std::size_t>(entry - _pending.data())] = _autoApproveArmedAt;
}

void EnrollmentWindow::OnRoleChanged(Distributed::SchedulerRole role)
{
    if (role == Distributed::SchedulerRole::Leader)
        return;
    (void) DisarmAutoApprove();
    // And the LIST goes with the leadership: every enrollment verb is answered `NotLeader` from
    // here on, so a row kept would be one nobody can decide about, and `waiting` would keep
    // telling an operator to act on a node that now sends them elsewhere. Every row, decided
    // ones included -- a joiner the cluster admitted is answered the roster by whichever node
    // leads, from the replicated state and not from this list.
    std::scoped_lock const guard { _mutex };
    _pending.clear();
    _firstSeen.clear();
    _lastSeen.clear();
    _autoApprovedArmedAt.clear();
    ReportWaitingLocked();
}

void EnrollmentWindow::LapseLocked() const
{
    if (!_autoApproveUntil.has_value() || _clock.now() < *_autoApproveUntil)
        return;
    _autoApproveUntil.reset();
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::EnrollmentWindowOpen);
}

bool EnrollmentWindow::AutoApprovingLocked() const noexcept
{
    return _autoApproveUntil.has_value() && _clock.now() < *_autoApproveUntil;
}

std::uint32_t EnrollmentWindow::SecondsUntilLocked() const noexcept
{
    if (!_autoApproveUntil.has_value())
        return 0;
    auto const now = _clock.now();
    if (*_autoApproveUntil <= now)
        return 0;
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(*_autoApproveUntil - now).count());
}

Wire::EnrollmentPendingEntry* EnrollmentWindow::FindLocked(std::string_view nodeId) noexcept
{
    auto const found = std::ranges::find(_pending, nodeId, &Wire::EnrollmentPendingEntry::nodeId);
    return found == _pending.end() ? nullptr : &*found;
}

std::uint64_t EnrollmentWindow::SecondsSince(core::platform::SteadyTimePoint since) const noexcept
{
    auto const now = _clock.now();
    if (now <= since)
        return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now - since).count());
}

} // namespace FastCache::Node
