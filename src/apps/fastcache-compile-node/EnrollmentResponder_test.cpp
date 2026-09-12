// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Admits exactly the peers it was given, and nobody else.
///
/// Not `OpenMembership`: a fake that admits everyone cannot tell *the gate is wired*
/// from *the gate admits everyone*, and on this surface exactly one verb is meant to
/// admit everyone. A fixture that could not see the difference would report the hole
/// as correct.
class ListedMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param members Who may ask.
    explicit ListedMembership(std::vector<std::string> members) noexcept:
        _members { std::move(members) }
    {
    }

    /// @copydoc Distributed::IMembershipOracle::Classify
    [[nodiscard]] Distributed::Membership Classify(std::string_view peerAddress) const override
    {
        return std::ranges::find(_members, peerAddress) != _members.end() ? Distributed::Membership::Member
                                                                          : Distributed::Membership::Outsider;
    }

  private:
    std::vector<std::string> _members;
};

/// Records what the scheduler proposed, and answers what a state holding it would.
///
/// It APPLIES an `AddMember`, which every other stub of this seam in the tree declines
/// to do -- and that is the point here rather than gold-plating: the ticket's acceptance
/// clause is that an approval hands the key over AND puts the member in `ClusterState`,
/// because either alone is green under half the defect. A stub that recorded the
/// proposal without applying it could only assert the first half.
class RecordingCluster final: public Distributed::IClusterAdmin
{
  public:
    /// Every command offered, in order -- including ones this fake then refused,
    /// which is what lets a case tell *the proposal was never made* from *it was
    /// made and declined*. An empty list is the first of those and nothing else.
    /// @return The commands, oldest first.
    [[nodiscard]] std::vector<Cluster::Command> const& Proposed() const noexcept
    {
        return _proposed;
    }

    /// Answer @p error instead of accepting, from the next proposal onwards.
    /// @param error What consensus should refuse with.
    void RefuseWith(ConsensusError error)
    {
        _refuse = std::move(error);
    }

    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        return _state;
    }

    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
    {
        _proposed.push_back(command);
        if (_refuse.has_value())
            return std::unexpected { *_refuse };
        if (command.kind == Cluster::CommandKind::AddMember)
            _state.members.push_back(
                Cluster::ClusterMember { .id = command.key, .raftEndpoint = command.value, .schedulerEndpoint = {} });
        return {};
    }

  private:
    std::vector<Cluster::Command> _proposed;
    std::optional<ConsensusError> _refuse;
    Cluster::ClusterState _state;
};

/// The key the seed holds, chosen so a case can assert its exact bytes.
constexpr std::string_view TheKey = "this-cluster-shared-secret-0123456789";

/// Hands out a fixed key, so a case can assert the BYTES a joiner is given.
class FixedKey final: public IClusterKeySource
{
  public:
    /// @param key What to hand over; empty means the read fails.
    explicit FixedKey(std::string key) noexcept:
        _key { std::move(key) }
    {
    }

    /// @copydoc IClusterKeySource::ClusterKey
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override
    {
        if (_key.empty())
            return std::unexpected { std::string { "no key here" } };
        auto const bytes = Wire::AsBytes(_key);
        return SecureByteBuffer { bytes.begin(), bytes.end() };
    }

  private:
    std::string _key;
};

/// Readable while the approval probes it, and broken by the time the joiner collects.
///
/// **This is the only remaining route to a failed hand-over, and modelling it faithfully
/// is what the fake is for.** `AnswerDecision` now READS the key before it admits
/// anybody, so a source that was broken all along is refused at the approve and the
/// joiner never reaches the hand-over at all -- which is the fix, and it means a
/// permanently-broken source can no longer reach the code these cases are about. What
/// remains reachable, and always was, is the file breaking BETWEEN the two: an operator
/// approves, and the key is unreadable by the time that machine's next poll arrives.
/// The first read therefore succeeds and every later one fails.
///
/// **The interleaving is PLACED rather than raced for.** Optionally it also decides the
/// row on its way out, which reproduces the second condition exactly: a window between
/// `Offer` taking the one collection an id has and the key read that should have served
/// it, in which somebody decides about the row -- so the claim cannot be given back and
/// the machine is stranded by a fault the design promised to absorb. Waiting for that by
/// chance is not a test; this reaches the same state every run, which is what
/// `FleetHarness::OnCompile` does for the fleet and why it exists.
class BreaksAfterApproval final: public IClusterKeySource
{
  public:
    /// @param window The window whose row is moved before the read fails, or nullptr to
    ///               leave the row alone so the claim CAN be given back.
    /// @param subject Whose row to move; ignored when @p window is nullptr.
    explicit BreaksAfterApproval(EnrollmentWindow* window = nullptr, std::string_view subject = {}) noexcept:
        _window { window },
        _subject { subject }
    {
    }

    /// @copydoc IClusterKeySource::ClusterKey
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override
    {
        if (!_probed)
        {
            // The approval's own readability check. Answering it is what lets these
            // cases reach the hand-over they are about.
            _probed = true;
            auto const bytes = Wire::AsBytes(TheKey);
            return SecureByteBuffer { bytes.begin(), bytes.end() };
        }

        if (_window != nullptr)
            (void) _window->Decide(_subject, Wire::EnrollmentDecision::Rejected);
        return std::unexpected { std::string { "the key file is unreadable right now" } };
    }

  private:
    EnrollmentWindow* _window;
    std::string _subject;

    /// Whether the approval has already read it. `mutable` because the seam is `const`.
    mutable bool _probed { false };
};

/// The machine asking to join. Deliberately NOT on any member list.
constexpr std::string_view JoinerAddress = "198.51.100.4";

/// The operator's machine. On the member list, so it may decide.
constexpr std::string_view OperatorAddress = "10.0.0.7";

/// What a joiner claims about itself.
constexpr std::string_view JoinerId = "joiner-a";
constexpr std::string_view JoinerEndpoint = "198.51.100.4:7100";

/// Everything one case needs, wired the way `main` wires it.
struct Seed
{
    Seed()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        service.AdministerWith(cluster);
    }

    ManualClock clock;
    ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, {}, {} };
    RecordingCluster cluster;
    ListedMembership membership { { std::string { OperatorAddress } } };
    FixedKey key { std::string { TheKey } };
    EnrollmentWindow window { clock };
    EnrollmentResponder responder { window, service, membership, key, metrics, logger };
};

/// Drive one frame through the responder as a peer at @p peer.
/// @param responder What answers.
/// @param frame The request.
/// @param peer Who is asking.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> AnswerNow(IFrameResponder& responder,
                                               std::span<std::byte const> frame,
                                               std::string_view peer)
{
    return SyncRun(responder.Answer(frame, std::string { peer }));
}

/// The payload of a reply.
/// @param reply The whole reply frame.
/// @return Its payload bytes.
[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    REQUIRE(reply.size() >= Wire::ReplyHeaderSize + Unwrap(header).payloadLength);
    return std::span<std::byte const> { reply }.subspan(Wire::ReplyHeaderSize, Unwrap(header).payloadLength);
}

/// The refusal code a reply carries, or nothing when it succeeded.
/// @param reply The whole reply frame.
/// @return The code.
[[nodiscard]] std::optional<Wire::ErrorCode> RefusalIn(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    if (Unwrap(header).status != Wire::Status::Error)
        return std::nullopt;
    auto const refusal = Wire::DecodeErrorPayload(PayloadOf(reply));
    REQUIRE(refusal.has_value());
    return Unwrap(refusal).first;
}

/// One `Enroll` from the joiner.
/// @param seed The wired fixture.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Enroll(Seed& seed)
{
    return AnswerNow(seed.responder,
                     Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                     JoinerAddress);
}

/// One `EnrollControl` from the operator.
/// @param seed The wired fixture.
/// @param verb What to do.
/// @param subject Who it is about.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Control(Seed& seed, Wire::EnrollControlVerb verb, std::string_view subject = {})
{
    return AnswerNow(seed.responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
}

} // namespace

TEST_CASE("A closed window refuses enrollment by name and moves the counter that says so", "[enrollment][responder]")
{
    Seed seed;

    // The default state, and the one every probe of this port meets. Asserting only
    // that it was refused would pass under a build that refused for any reason at all;
    // the CODE is what a client acts on and the COUNTER is what an operator watches,
    // and the ticket asks for both because a refusal nothing counts is a probed port
    // that looks unused.
    auto const reply = Enroll(seed);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::EnrollmentClosed);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedClosed) == 1);

    // And nothing else moved. A counter rising somewhere is not the same fact as THIS
    // counter rising, and a fixture that only asserts its own row cannot tell the two
    // apart on a surface that shares wire codes with three others.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedFull) == 0);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 0);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 0);
}

TEST_CASE("An open window records a joiner and answers it with no key bytes at all", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);

    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);

    // **The assertion the ticket asks for, and the only one that can catch it.** A
    // server bug that handed the key out on `Pending` would answer a correct outcome
    // byte -- that byte is right in the healthy build and in the broken one -- so the
    // LENGTH is the discriminating fact. Zero bytes, and nothing that merely does not
    // equal the key.
    CHECK(Unwrap(decoded).clusterKey.empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 0);
}

TEST_CASE("Approving a joiner hands it the key AND puts it in the cluster's member record", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId))).status
            == Wire::Status::Ok);

    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);

    // Half one: the joiner has the key, byte for byte.
    auto const handed = std::string { Wire::AsStringView(Unwrap(decoded).clusterKey) };
    CHECK(handed == TheKey);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 1);

    // Half two, and BOTH are asserted because either alone is green under half the
    // defect: a build that handed the key over and proposed nothing leaves a machine
    // holding the fleet's secret that consensus has never heard of, and a build that
    // proposed and handed nothing over leaves a member the cluster counts and cannot
    // authenticate.
    auto const state = seed.cluster.ClusterState();
    REQUIRE(state.members.size() == 1);
    CHECK(state.members.front().id == JoinerId);
    CHECK(state.members.front().raftEndpoint == JoinerEndpoint);
}

TEST_CASE("The cluster is asked BEFORE the window is marked, so a refused change hands out no key",
          "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);

    // A leader that cannot accept the change right now, which is what a healthy cluster
    // answers while an earlier membership change is still replicating.
    seed.cluster.RefuseWith(ConsensusError { .code = ConsensusErrorCode::ConfigurationChangeInFlight,
                                             .context = "another change is committing",
                                             .knownLeader = std::nullopt });

    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == Wire::ErrorCode::ClusterChangeInFlight);

    // The ORDER is the property, and it is only visible from the joiner's side: the
    // window was not marked, so the machine goes on waiting rather than collecting a
    // key for a membership the cluster refused.
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(Enroll(seed)));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);
    CHECK(Unwrap(decoded).clusterKey.empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 0);
}

TEST_CASE("The cluster key is served once and a replay gets no key BYTES", "[enrollment][responder][security]")
{
    // **The acceptance case for the spend, and the assertion is the payload LENGTH.**
    // An outcome byte is equally correct in a healthy build and in one that serves the
    // key again, so a case reading the outcome passes under the defect; the only thing
    // that discriminates is that the reply carries no key.
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);

    auto const served = Enroll(seed);
    auto const first = Wire::DecodeEnrollReply(PayloadOf(served));
    REQUIRE(first.has_value());
    REQUIRE(Unwrap(first).outcome == Wire::EnrollOutcome::Approved);
    REQUIRE_FALSE(Unwrap(first).clusterKey.empty());
    REQUIRE(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 1);

    // The replay. Refused by name, and there is no reply payload to hold a key at all --
    // the arm returns before the key file is ever read, so the bytes are not withheld,
    // they never existed in this frame.
    auto const replay = Enroll(seed);
    CHECK(RefusalIn(replay) == Wire::ErrorCode::EnrollmentAlreadyCollected);
    CHECK_FALSE(Wire::DecodeEnrollReply(PayloadOf(replay)).has_value());

    // Counted, and the two counters say different things: one that the key left, one
    // that somebody asked after it had left. A healthy enrolment produces none of the
    // second, so folding them would bury the only signal here worth an alert.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 1);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedAlreadyCollected) == 1);

    // Polling harder does not help, and does not quietly become the idempotent
    // behaviour this replaced.
    CHECK(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentAlreadyCollected);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 1);
}

TEST_CASE("A claim that could not be given back is reported, not swallowed", "[enrollment][responder][security]")
{
    // **This asserts the READER, which is a different property from the outcome enum
    // existing.** `ReturnClaim`'s answer was discarded at this one call site, so the one
    // value that matters -- the repair itself failed -- reached nobody. Delete the
    // `switch` in `AnswerEnroll` and every other case on this branch still passes; only
    // this one goes red.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    BreaksAfterApproval key { &seed.window, JoinerId };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, key, seed.metrics, logger };

    REQUIRE(Unwrap(Wire::DecodeReplyHeader(
                       AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Open), OperatorAddress)))
                .status
            == Wire::Status::Ok);
    auto const enroll = [&] {
        return AnswerNow(responder,
                         Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                         JoinerAddress);
    };
    REQUIRE(RefusalIn(enroll()) == std::nullopt);
    REQUIRE(RefusalIn(
                AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, JoinerId), OperatorAddress))
            == std::nullopt);

    // The poll that takes the claim, finds the key unreadable, and cannot give it back
    // because the key source rejected the row on its way out.
    CHECK(RefusalIn(enroll()) == Wire::ErrorCode::StorageWriteFailed);

    auto const records = logger.Snapshot();
    auto const warned = std::ranges::find_if(
        records, [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn && r.message.contains(JoinerId); });
    REQUIRE(warned != records.end());

    // It names the REMEDY, because a warning an operator cannot act on is one they learn
    // to scroll past -- and this one costs a machine its only collection.
    CHECK(warned->message.contains("Approve"));

    // And no key went out, which is the property the whole repair path serves.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 0);
}

TEST_CASE("A claim given back cleanly says nothing, so the warning stays worth reading", "[enrollment][responder][security]")
{
    // The control, and it is not decoration: a responder that warned on EVERY failed key
    // read would pass the case above while making the warning meaningless -- which is the
    // same both-directions rule the mismatch mark in the listing needed. Here the repair
    // SUCCEEDS, so there is nothing to say.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    BreaksAfterApproval breaks {};
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, breaks, seed.metrics, logger };

    REQUIRE(Unwrap(Wire::DecodeReplyHeader(
                       AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Open), OperatorAddress)))
                .status
            == Wire::Status::Ok);
    auto const enroll = [&] {
        return AnswerNow(responder,
                         Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                         JoinerAddress);
    };
    REQUIRE(RefusalIn(enroll()) == std::nullopt);
    REQUIRE(RefusalIn(
                AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, JoinerId), OperatorAddress))
            == std::nullopt);

    CHECK(RefusalIn(enroll()) == Wire::ErrorCode::StorageWriteFailed);

    auto const records = logger.Snapshot();
    CHECK(std::ranges::none_of(records, [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn; }));

    // And the claim really was given back: the joiner's next poll is offered the key
    // again rather than refused as already collected.
    CHECK(Unwrap(seed.window.Find(JoinerId)).decision == Wire::EnrollmentDecision::Approved);
}

TEST_CASE("Re-approving a collected id re-arms exactly one more collection", "[enrollment][responder][security]")
{
    // **The recovery path, and it exists because the spend has a cost.** A joiner whose
    // reply was lost holds no key and is refused by every retry, so without this it is
    // stranded -- and the instruction its refusal used to offer was `--cluster-forget`,
    // a quorum change to recover from a dropped packet.
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentAlreadyCollected);

    // One operator command, on the id they already approved. The cluster is asked again
    // and answers that the member is already in force -- which is a `Satisfied` refusal
    // and not a failure, so it must not be reported as one. Reading it as a refusal is
    // what would leave the joiner polling `Pending` forever after a recovery that
    // looked like it worked.
    seed.cluster.RefuseWith(ConsensusError {
        .code = ConsensusErrorCode::MembershipUnchanged, .context = "already a member", .knownLeader = std::nullopt });
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);

    auto const again = Wire::DecodeEnrollReply(PayloadOf(Enroll(seed)));
    REQUIRE(again.has_value());
    CHECK(Unwrap(again).outcome == Wire::EnrollOutcome::Approved);
    CHECK_FALSE(Unwrap(again).clusterKey.empty());

    // The tally rises a SECOND time, because the key genuinely left a second time. A
    // recovery that hid the extra hand-over inside the first would be the counter
    // lying about the one event it exists to report.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 2);

    // And it re-armed exactly one: the next poll is refused again.
    CHECK(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentAlreadyCollected);
}

TEST_CASE("An approval that consensus refuses for any OTHER reason still hands out no key",
          "[enrollment][responder][security]")
{
    // The control for the case above, and it is what keeps that tolerance narrow: only
    // *already in force*, and only for a row that has collected, may proceed. Any other
    // refusal is a refusal, or a re-approval would serve the key on the strength of a
    // consensus answer that said no.
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);

    seed.cluster.RefuseWith(ConsensusError { .code = ConsensusErrorCode::ConfigurationChangeInFlight,
                                             .context = "another change is committing",
                                             .knownLeader = std::nullopt });
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == Wire::ErrorCode::ClusterChangeInFlight);
    CHECK(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentAlreadyCollected);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 1);
}

TEST_CASE("A rejected joiner is told so and stays rejected until somebody changes their mind", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(Enroll(seed)));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Rejected);
    CHECK(Unwrap(decoded).clusterKey.empty());

    // Rejecting proposes NOTHING: a machine refused at the door must not appear in the
    // cluster's member record under any reading.
    CHECK(seed.cluster.Proposed().empty());
    CHECK(seed.cluster.ClusterState().members.empty());
}

TEST_CASE("The pending list fills, refuses the next machine by name, and keeps the first", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);

    for (std::size_t index = 0; index < MaxPendingEnrollments; ++index)
    {
        auto const id = std::format("crowd-{}", index);
        REQUIRE(RefusalIn(AnswerNow(seed.responder,
                                    Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = id, .raftEndpoint = "10.0.0.1:1" }),
                                    JoinerAddress))
                == std::nullopt);
    }

    CHECK(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentFull);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedFull) == 1);

    // The eviction case a suite skips: the machine that arrived FIRST is still on the
    // list an operator is reading. Without this, an implementation that evicted to make
    // room and then refused would pass every assertion above.
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(Control(seed, Wire::EnrollControlVerb::List)));
    REQUIRE(report.has_value());
    REQUIRE(Unwrap(report).pending.size() == MaxPendingEnrollments);
    CHECK(Unwrap(report).pending.front().nodeId == "crowd-0");
}

TEST_CASE("Enrollment is reachable by a stranger and deciding it is not", "[enrollment][responder]")
{
    Seed seed;

    // **The hole this surface deliberately opens, and the gate beside it.** The joiner
    // is on no member list -- it is a fresh install, which is the entire problem -- so
    // `Enroll` must be admitted at the door. Asserting only that would leave the pair
    // untested in the direction that matters.
    CHECK(!seed.responder.RefusePeer(JoinerAddress, static_cast<std::uint8_t>(Wire::Op::Enroll)).has_value());

    // And the decision verb is refused to the same peer, before a payload is read, with
    // the counter that says somebody tried to approve themselves.
    auto const refused = seed.responder.RefusePeer(JoinerAddress, static_cast<std::uint8_t>(Wire::Op::EnrollControl));
    REQUIRE(refused.has_value());
    CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NotAMember);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedNotAMember) == 1);

    // The operator's machine is admitted to both, so the refusal above is about the
    // peer rather than about the verb being closed to everybody.
    CHECK(!seed.responder.RefusePeer(OperatorAddress, static_cast<std::uint8_t>(Wire::Op::EnrollControl)).has_value());
}

TEST_CASE("A follower answers enrollment with the leader's endpoint rather than a window of its own",
          "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);
    seed.service.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.1:7000", Distributed::StandaloneSchedulerTerm);

    // Both verbs, because a follower whose LIST still answered would show an operator an
    // empty list on the machine that was never going to hold the rows -- which reads as
    // *nobody is waiting* rather than as *ask somewhere else*.
    // `auto const&`, not `auto const`: the elements are `std::vector<std::byte>`, so a
    // by-value loop copies a whole reply per iteration. Apple clang reports that as
    // `-Wrange-loop-construct` and the build is `-Werror`, so it is a macOS BUILD
    // failure and green everywhere else -- a platform's leg answering a different
    // question rather than a weaker version of the same one.
    for (auto const& reply: { Enroll(seed), Control(seed, Wire::EnrollControlVerb::List) })
    {
        CHECK(RefusalIn(reply) == Wire::ErrorCode::NotLeader);
        auto const refusal = Wire::DecodeErrorPayload(PayloadOf(reply));
        REQUIRE(refusal.has_value());
        // The MESSAGE is a machine-readable endpoint a client follows, so it is
        // asserted as one: a sentence here would be followed by nobody.
        CHECK(Unwrap(refusal).second == "10.0.0.1:7000");
    }
}

TEST_CASE("A joiner that names itself in bytes that are not text is refused where it enters", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(Unwrap(Wire::DecodeReplyHeader(Control(seed, Wire::EnrollControlVerb::Open))).status == Wire::Status::Ok);

    // One byte that belongs to no UTF-8 sequence. Refused HERE rather than at the
    // approval, because an id copied into `ClusterState` is read back out of
    // `/fleet.json` by everybody -- and a consensus entry is applied after it is
    // committed, with nobody left to refuse it.
    auto const reply = AnswerNow(seed.responder,
                                 Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = "joiner-\xff"
                                                                                    "a",
                                                                          .raftEndpoint = JoinerEndpoint }),
                                 JoinerAddress);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::MalformedFrame);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 1);

    // Nothing was recorded, so the operator's list does not carry a row they cannot read.
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(Control(seed, Wire::EnrollControlVerb::List)));
    REQUIRE(report.has_value());
    CHECK(Unwrap(report).pending.empty());
}

TEST_CASE("An approved joiner whose seed cannot read its own key is told that, not that the window shut",
          "[enrollment][responder]")
{
    Seed seed;
    BreaksAfterApproval breaks {};
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, breaks, seed.metrics, seed.logger };

    REQUIRE(Unwrap(Wire::DecodeReplyHeader(
                       AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Open), OperatorAddress)))
                .status
            == Wire::Status::Ok);
    REQUIRE(
        RefusalIn(AnswerNow(responder,
                            Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                            JoinerAddress))
        == std::nullopt);
    REQUIRE(RefusalIn(
                AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, JoinerId), OperatorAddress))
            == std::nullopt);

    // Emphatically NOT `EnrollmentClosed`: the window is open and the decision was
    // taken, and telling the joiner otherwise would send an operator to re-open a
    // window that is already open.
    auto const reply =
        AnswerNow(responder,
                  Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                  JoinerAddress);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::StorageWriteFailed);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentKeysHandedOver) == 0);
}

TEST_CASE("Opening counts once and re-opening does not", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentWindowsOpened) == 1);

    // The audit trail counts WINDOWS rather than commands: an operator who types the
    // verb twice has opened one window, and a counter that read two would report a
    // second minute of exposure that never happened.
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == Wire::ErrorCode::ClusterChangeNotNeeded);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentWindowsOpened) == 1);
}

TEST_CASE("A control frame naming a subject the verb does not take is refused", "[enrollment][responder]")
{
    Seed seed;

    // The decoder's arity rule reaching the surface: `Close` names nobody and `Approve`
    // must. Answering either by ignoring the mismatch is how an operator comes to
    // believe they approved somebody.
    CHECK(RefusalIn(AnswerNow(
              seed.responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Close, JoinerId), OperatorAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(RefusalIn(AnswerNow(seed.responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve), OperatorAddress))
          == Wire::ErrorCode::MalformedFrame);
}

TEST_CASE("A node with no enrollment component refuses the whole family at the door", "[enrollment][responder][merged]")
{
    // The ordinary deployment: a worker with no consensus has no cluster to let anybody
    // into, so both verbs are unserved rather than being answered by a surface that
    // exists only to say no.
    MergedResponder merged { SurfaceComponents {} };
    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::Enroll)) == nullptr);
    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::EnrollControl)) == nullptr);

    // **`NoCluster`, and the assertion that it is NOT `UnimplementedVerb` is the one
    // that means anything here.** Both codes refuse, both are "unserved", and a case
    // asserting only that the peer was refused passes under either -- which is how this
    // shipped answering the wrong one. The client maps `UnimplementedVerb` onto
    // `UnknownOpcode` and reports *the seed is running a build older than this one*,
    // and since the documented flow points `--enroll-from` at ANY member while most
    // members run no consensus, that wrong sentence was the likeliest thing a healthy
    // fleet would ever print. Pinned on both verbs, because they are refused through
    // different routes -- `RefusePeer` at the door and `Answer` for a frame that got
    // past it -- and one route fixed alone reads exactly like both.
    for (auto const op: { Wire::Op::Enroll, Wire::Op::EnrollControl })
    {
        auto const refused = merged.RefusePeer(JoinerAddress, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalIn(Unwrap(refused)) != Wire::UnimplementedVerb);

        CHECK(RefusalIn(
                  AnswerNow(merged,
                            Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                            JoinerAddress))
              == Wire::ErrorCode::NoCluster);
    }

    // And the families this does NOT cover still answer the unserved sentence, or the
    // fix above would have been "call everything NoCluster", which tells a launcher
    // asking a worker for a cache verb that it is in the wrong cluster.
    auto const cacheVerb = merged.RefusePeer(JoinerAddress, static_cast<std::uint8_t>(Wire::Op::Fetch));
    REQUIRE(cacheVerb.has_value());
    CHECK(RefusalIn(Unwrap(cacheVerb)) == Wire::UnimplementedVerb);
}

TEST_CASE("Rejecting a machine that was already approved says the cluster still holds it",
          "[enrollment][responder][security]")
{
    // **`Reject` never reaches `ClusterAdmit`, and for a PENDING row that is exactly
    // right -- nothing was committed, so nothing needs removing.** But
    // `EnrollmentWindow::Decide` permits `Approved -> Rejected`, which is the path an
    // operator correcting a mis-approval takes, and there the approval has already
    // committed the member. The reject stops the key hand-over -- worth having, which is
    // why it is not refused outright -- and leaves the machine in `ClusterState`,
    // counted towards quorum, while `--enroll-reject`'s help text promised *"a machine
    // refused here was never a member and needs no --cluster-forget"*.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.key, seed.metrics, logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(
        RefusalIn(AnswerNow(responder,
                            Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                            JoinerAddress))
        == std::nullopt);
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);

    // The approval really did commit it, or the rest of this case would be asserting
    // a warning about a state the cluster is not in.
    REQUIRE(std::ranges::any_of(seed.cluster.ClusterState().members,
                                [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    auto const records = logger.Snapshot();
    auto const warned = std::ranges::find_if(
        records, [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn && r.message.contains(JoinerId); });
    REQUIRE(warned != records.end());

    // It names the REMEDY, which is the whole point: an operator who read the flag's
    // own description believes there is nothing left to do.
    CHECK(warned->message.contains("--cluster-forget"));

    // And the member IS still there, so the warning is true rather than defensive.
    CHECK(std::ranges::any_of(seed.cluster.ClusterState().members,
                              [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
}

TEST_CASE("Rejecting a machine that was only waiting says nothing, so the warning stays worth reading",
          "[enrollment][responder][security]")
{
    // **The control, and it is what makes the case above mean anything.** A responder
    // that warned on EVERY reject would pass that one while making the warning noise --
    // and this is the ordinary path, the one an operator uses to turn a stranger away,
    // where the help text's promise is entirely true.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.key, seed.metrics, logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(
        RefusalIn(AnswerNow(responder,
                            Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                            JoinerAddress))
        == std::nullopt);

    // Straight to `Reject`, so the row never left `Pending`.
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    CHECK(
        std::ranges::none_of(logger.Snapshot(), [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn; }));

    // Nothing was ever offered to consensus, which is why there is nothing to warn about.
    CHECK(seed.cluster.Proposed().empty());
}

TEST_CASE("A seed that cannot read its own key admits nobody, and tells the operator so",
          "[enrollment][responder][security]")
{
    // **The ordering defect, asserted where it is decided.** `AnswerDecision` used to
    // commit `ClusterAdmit` and consult the key only on the joiner's NEXT poll, so a
    // node whose key file was named but unreadable answered the operator `Ok`, grew the
    // replicated configuration -- and therefore the quorum -- by a machine that could
    // never collect, and then served that joiner `StorageWriteFailed` forever. A
    // phantom member counted towards every future election, from a command that
    // reported success.
    Seed seed;
    FixedKey unreadable { {} };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, unreadable, seed.metrics, seed.logger };

    REQUIRE(Unwrap(Wire::DecodeReplyHeader(
                       AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Open), OperatorAddress)))
                .status
            == Wire::Status::Ok);
    REQUIRE(
        RefusalIn(AnswerNow(responder,
                            Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = JoinerId, .raftEndpoint = JoinerEndpoint }),
                            JoinerAddress))
        == std::nullopt);

    CHECK(RefusalIn(
              AnswerNow(responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, JoinerId), OperatorAddress))
          == Wire::ErrorCode::StorageWriteFailed);

    // **The half that distinguishes.** A refusal alone is green under a build that
    // refused the operator AFTER telling the cluster -- the quorum would already have
    // grown, which is the whole defect -- so what is asserted is that consensus was
    // offered NOTHING. `RecordingCluster` records every command it is handed, including
    // ones it would have refused, so an empty list means the proposal was never made
    // rather than never accepted.
    CHECK(seed.cluster.Proposed().empty());

    // And the row is untouched, so an operator who repairs the key file approves the
    // same id again rather than finding it half-decided.
    CHECK(Unwrap(seed.window.Find(JoinerId)).decision == Wire::EnrollmentDecision::Pending);
}

TEST_CASE("A node that runs enrollment routes both verbs to it and nothing else", "[enrollment][responder][merged]")
{
    Seed seed;
    MergedResponder merged { SurfaceComponents { .enrollment = &seed.responder } };

    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::Enroll)) == &seed.responder);
    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::EnrollControl)) == &seed.responder);

    // And the routing is by FAMILY rather than by a pair of opcodes: every other verb
    // this build knows lands somewhere else, which on this fixture is nowhere. Swept
    // over the whole byte range because a claim about routing stops being true without
    // anybody editing the sentence that states it.
    for (std::uint32_t raw = 0; raw <= 0xFFU; ++raw)
    {
        auto const byte = static_cast<std::uint8_t>(raw);
        auto const isEnrollment = Wire::FamilyOf(byte) == Wire::VerbFamily::Enrollment;
        CHECK((merged.OwnerOf(byte) == &seed.responder) == isEnrollment);
    }
}
