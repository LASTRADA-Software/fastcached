// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ListedMembership;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Records what the scheduler proposed, and applies it the way a committed entry is applied.
///
/// It APPLIES, through the real `Cluster::Apply` rather than a hand-rolled copy, which every
/// other stub of this seam in the tree declines to do -- and that is the point here rather
/// than gold-plating: an approved joiner is answered only once the leader's roster RECORDS it
/// under the key it asked with (#178), so a stub that recorded the proposal without applying
/// it could never reach the answer these cases are about. A hand-rolled apply would also be a
/// second model of `AddMember`'s key rule and `AdmitPrincipal`'s, free to disagree with the
/// one that ships.
///
/// `HoldApplies` models the other half of a real leader: `ProposeToCluster` returns once the
/// entry is APPENDED, and the state moves only when it commits.
class RecordingCluster final: public Distributed::IClusterAdmin
{
  public:
    /// Every command offered, in order -- including ones this fake then refused, which is
    /// what lets a case tell *the proposal was never made* from *it was made and declined*.
    /// An empty list is the first of those and nothing else.
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

    /// Accept proposals from now on without applying them, as a leader does between the
    /// append and the commit.
    void HoldApplies() noexcept
    {
        _hold = true;
    }

    /// Apply everything held, as the commit would.
    void CommitHeld()
    {
        for (auto const& command: _held)
            Cluster::Apply(_state, command);
        _held.clear();
        _hold = false;
    }

    /// Replace the state outright, for a case arranging what an earlier commit left.
    /// @param state The state to hold.
    void SetState(Cluster::ClusterState state)
    {
        _state = std::move(state);
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
        if (_hold)
            _held.push_back(command);
        else
            Cluster::Apply(_state, command);
        return {};
    }

  private:
    std::vector<Cluster::Command> _proposed;
    std::vector<Cluster::Command> _held;
    std::optional<ConsensusError> _refuse;
    Cluster::ClusterState _state;
    bool _hold { false };
};

/// A 32-byte value whose every byte is @p fill.
/// @param fill The byte.
/// @return The bytes.
[[nodiscard]] std::array<std::byte, 32> Filled(std::uint8_t fill)
{
    std::array<std::byte, 32> bytes {};
    bytes.fill(static_cast<std::byte>(fill));
    return bytes;
}

/// The key the joiner mints and asks under. Any 32 bytes: nothing on this surface verifies
/// a signature under it, and the roster records whatever the approval names.
[[nodiscard]] Ed25519PublicKey JoinerKey()
{
    return Filled(0x42);
}

/// The machine asking to join. Deliberately NOT on any member list.
constexpr std::string_view JoinerAddress = "198.51.100.4";

/// The operator's machine. On the member list, so it may decide.
constexpr std::string_view OperatorAddress = "10.0.0.7";

/// What a joiner claims about itself.
///
/// A WORKER, stating no endpoint: the role the one-shot `--enroll-from` asks as. A learner's
/// admission has cases of its own below.
constexpr std::string_view JoinerId = "joiner-a";
constexpr Wire::EnrollRole JoinerRole = Wire::EnrollRole::Worker;

/// The leader's own member record, which every roster it hands out carries.
constexpr std::string_view LeaderId = "leader";
constexpr std::string_view LeaderEndpoint = "10.0.0.1:7100";

/// Everything one case needs, wired the way `main` wires it.
struct Seed
{
    Seed()
    {
        service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
        service.AdministerWith(cluster);

        auto state = Cluster::ClusterState {};
        state.members.push_back(
            Cluster::ClusterMember { .id = std::string { LeaderId },
                                     .raftEndpoint = std::string { LeaderEndpoint },
                                     .schedulerEndpoint = {},
                                     .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                     .seat = Cluster::MemberSeat::Voter,
                                     .publicKey = Filled(0x01) });
        cluster.SetState(std::move(state));
    }

    core::platform::ManualClock clock;
    core::platform::ManualWallClock wallClock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
    RecordingCluster cluster;
    // A LIST, not `OpenMembership`: a fake that admits everyone cannot tell *the gate is
    // wired* from *the gate admits everyone*, and on this surface exactly one verb is meant
    // to admit everyone -- so a fixture that could not see the difference would report the
    // hole as correct. The route is named HERE rather than defaulted in the shared fake,
    // because which route admits is this case's fact to state (#1497).
    ListedMembership membership { { std::string { OperatorAddress } }, Distributed::MembershipParticipant::FleetMemberList };
    NodeConditions conditions;
    // Bound as `main` binds it: the node's own sink and the wall clock the scheduler reads, never
    // the defaults a fixture finds more convenient.
    EnrollmentWindow window { clock, &conditions, &metrics, wallClock };
    EnrollmentResponder responder { window, service, membership, metrics, logger };
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
    // Nothing PROVED, which is what every case here is about: the enrollment pair is for a
    // machine the cluster has not admitted yet, so a proof is not a state a joiner can be in.
    return core::async::syncRun(responder.Answer(frame, PeerIdentity { .host = std::string { peer } })).bytes;
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

/// The sentence a refusal carries, copied out.
/// @param reply The whole reply frame, a refusal.
/// @return Its sentence.
[[nodiscard]] std::string RefusalSentenceIn(std::span<std::byte const> reply)
{
    auto const refusal = Wire::DecodeErrorPayload(PayloadOf(reply));
    REQUIRE(refusal.has_value());
    return std::string { Unwrap(refusal).second };
}

/// An `Enroll` frame.
/// @param id The identity claimed.
/// @param endpoint The endpoint claimed; empty for every live role.
/// @param role What it asks to be.
/// @param key The key it asks under.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> EnrollFrame(std::string_view id,
                                                 std::string_view endpoint,
                                                 Wire::EnrollRole role,
                                                 Ed25519PublicKey const& key)
{
    return Wire::EncodeEnroll(
        Wire::EnrollRequest { .nodeId = id, .nodeEndpoint = endpoint, .role = role, .publicKey = key });
}

/// One `Enroll` from the joiner, in its role and under its own key.
/// @param seed The wired fixture.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Enroll(Seed& seed)
{
    return AnswerNow(seed.responder, EnrollFrame(JoinerId, "", JoinerRole, JoinerKey()), JoinerAddress);
}

/// One approval from the operator, naming @p id and @p key.
/// @param seed The wired fixture.
/// @param id Who it is about.
/// @param key The key it names.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> ApproveUnder(Seed& seed, std::string_view id, Ed25519PublicKey const& key)
{
    return AnswerNow(seed.responder, Wire::EncodeEnrollApprove(id, key), OperatorAddress);
}

/// One `EnrollControl` from the operator.
///
/// An `Approve` names the key the row under @p subject holds -- what an operator pasting the line
/// `--enroll-list` prints sends -- or no row's key when there is no row. A case about the key
/// itself calls `ApproveUnder`.
/// @param seed The wired fixture.
/// @param verb What to do.
/// @param subject Who it is about.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Control(Seed& seed, Wire::EnrollControlVerb verb, std::string_view subject = {})
{
    if (verb == Wire::EnrollControlVerb::Approve)
        return ApproveUnder(
            seed,
            subject,
            seed.window.Find(subject)
                .transform([](Wire::EnrollmentPendingEntry const& row) { return Ed25519PublicKey { row.publicKey }; })
                .value_or(Ed25519PublicKey {}));
    return AnswerNow(seed.responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
}

/// One `Enroll` from a machine asking to join as a LEARNER, under @p key.
/// @param seed The wired fixture.
/// @param id The identity it claims.
/// @param key The key it asks under.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> LearnerRequest(Seed& seed, std::string_view id, Ed25519PublicKey const& key)
{
    return AnswerNow(seed.responder, EnrollFrame(id, "", Wire::EnrollRole::Learner, key), JoinerAddress);
}

/// Record the joiner, and approve it.
/// @param seed The wired fixture.
void RecordAndApprove(Seed& seed)
{
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
}

/// @p bytes as lowercase or uppercase hex.
/// @param bytes What to spell.
/// @param upper Whether the digits are uppercase.
/// @return The spelling.
[[nodiscard]] std::string Hex(std::span<std::byte const> bytes, bool upper)
{
    std::string text;
    for (auto const byte: bytes)
        text +=
            upper ? std::format("{:02X}", static_cast<unsigned>(byte)) : std::format("{:02x}", static_cast<unsigned>(byte));
    return text;
}

/// Whether @p haystack carries @p secret in any spelling a key file, a log or a wire has
/// ever used for one: raw, standard base64, unpadded base64url, and hex in either case.
///
/// **Every spelling, because a leak does not have to be raw.** A key read from a file is
/// TEXT -- the retired `--cluster-key-file` held base64 -- and a hand-over that forwarded the file's
/// contents would carry no raw run of the key's bytes at all. A scan for the raw bytes alone
/// passes under exactly that defect.
/// @param haystack What was sent.
/// @param secret What must not be in it.
/// @return True when any spelling of @p secret occurs in @p haystack.
[[nodiscard]] bool Carries(std::span<std::byte const> haystack, std::span<std::byte const> secret)
{
    auto const contains = [haystack](std::span<std::byte const> needle) {
        return !needle.empty() && !std::ranges::search(haystack, needle).empty();
    };
    if (contains(secret))
        return true;
    return std::ranges::any_of(
        std::array { Base64Encode(secret), Base64UrlEncode(secret), Hex(secret, false), Hex(secret, true) },
        [&contains](std::string const& spelling) { return contains(Wire::AsBytes(spelling)); });
}

} // namespace

TEST_CASE("A joiner is recorded under its key and answered with no roster bytes", "[enrollment][responder]")
{
    Seed seed;

    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);

    // The LENGTH is the discriminating fact: an outcome byte is right in the healthy build
    // and in one that answered a waiting machine with the roster anyway.
    CHECK(Unwrap(decoded).roster.empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);

    // What the operator will compare is the key the machine ASKED with, recorded whole.
    auto const row = seed.window.Find(JoinerId);
    REQUIRE(row.has_value());
    CHECK(Unwrap(row).publicKey == JoinerKey());
    CHECK(Unwrap(row).role == JoinerRole);
}

TEST_CASE("Approving a worker admits a principal, never a member", "[enrollment][responder]")
{
    Seed seed;

    auto const workerKey = Filled(0x77);
    auto const ask = [&] {
        return AnswerNow(seed.responder, EnrollFrame("worker-a", "", Wire::EnrollRole::Worker, workerKey), JoinerAddress);
    };
    REQUIRE(RefusalIn(ask()) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "worker-a")) == std::nullopt);

    // A principal in the worker role, under its key -- and NOT a member, which is the half
    // that discriminates: a worker counted towards quorum is a machine that never runs
    // consensus holding a vote nobody can collect.
    auto const state = seed.cluster.ClusterState();
    auto const principal = std::ranges::find(state.principals, "worker-a", &Cluster::ClusterPrincipal::id);
    REQUIRE(principal != state.principals.end());
    CHECK(principal->publicKey == workerKey);
    CHECK(principal->role == Cluster::PrincipalRole::Worker);
    CHECK(std::ranges::none_of(state.members, [](Cluster::ClusterMember const& m) { return m.id == "worker-a"; }));

    auto const reply = ask();
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    REQUIRE(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);
    auto const roster = Cluster::DecodeRoster(Unwrap(decoded).roster);
    REQUIRE(roster.has_value());
    CHECK(RosterRecordsJoiner(roster.value(), "worker-a", workerKey, Wire::EnrollRole::Worker));
    CHECK(RosterRecordsJoiner(roster.value(), LeaderId, Filled(0x01), Wire::EnrollRole::Learner));
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 1);

    // And the fingerprint of what was SENT is on the row, over the very bytes the joiner
    // received -- which is what makes the two strings an operator compares comparable.
    CHECK(Unwrap(seed.window.Find("worker-a")).rosterFingerprint == Cluster::DigestOfRoster(Unwrap(decoded).roster));
}

TEST_CASE("A role that does not suit the endpoint is refused before it reaches the list", "[enrollment][responder]")
{
    Seed seed;

    // Neither live role states an endpoint: a learner dials the leader rather than being
    // dialled, and a worker's principal record has nowhere to keep one. Both are refused
    // where they enter, because the list is what a person reads.
    CHECK(RefusalIn(AnswerNow(
              seed.responder, EnrollFrame("l", "10.0.0.9:6674", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(RefusalIn(AnswerNow(
              seed.responder, EnrollFrame("w", "10.0.0.9:7100", Wire::EnrollRole::Worker, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 2);
    CHECK(seed.window.Summary().second == 0);

    // The control: the same learner stating none is recorded, so the refusal above is about
    // the endpoint and not about the role.
    CHECK(RefusalIn(AnswerNow(seed.responder, EnrollFrame("l", "", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == std::nullopt);
    CHECK(seed.window.Summary().second == 1);
}

TEST_CASE("Approving a learner admits it as a learner with no endpoint under the key it asked with",
          "[enrollment][responder][formation]")
{
    Seed seed;
    auto const laptopKey = Filled(0x6C);
    REQUIRE(RefusalIn(LearnerRequest(seed, "laptop", laptopKey)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop")) == std::nullopt);

    // What was PROPOSED, which is what distinguishes the seat: a member proposed as a voter
    // with no endpoint is refused by the cluster, and one proposed as a voter WITH none would
    // be counted by a quorum it can never answer.
    auto const& proposed = seed.cluster.Proposed();
    REQUIRE(proposed.size() == 1);
    CHECK(proposed[0].kind == Cluster::CommandKind::AddLearner);
    CHECK(proposed[0].key == "laptop");
    CHECK(proposed[0].value.empty());
    CHECK(proposed[0].publicKey == std::optional { laptopKey });
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsManual) == 1);

    // And the cluster RECORDS it so, which is what the joiner's next poll is answered from.
    auto const state = seed.cluster.ClusterState();
    auto const member = std::ranges::find(state.members, "laptop", &Cluster::ClusterMember::id);
    REQUIRE(member != state.members.end());
    CHECK(member->seat == Cluster::MemberSeat::Learner);
    CHECK(member->raftEndpoint.empty());
    auto const reply = LearnerRequest(seed, "laptop", laptopKey);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
}

TEST_CASE("Approving an id the cluster already seats is refused by name and changes no seat",
          "[enrollment][responder][formation]")
{
    // A learner row under the VOTER's id, asked under another key: an approval of it must neither
    // demote the voter nor read as succeeding while it changes nothing (#1449).
    Seed seed;
    REQUIRE(RefusalIn(LearnerRequest(seed, LeaderId, Filled(0x5E))) == std::nullopt);
    auto const reply = Control(seed, Wire::EnrollControlVerb::Approve, LeaderId);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::InvalidClusterChange);
    auto const refusal = Wire::DecodeErrorPayload(PayloadOf(reply));
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).second
          == std::format("{} is already a member as voter; an approval does not change a member's seat", LeaderId));

    auto const state = seed.cluster.ClusterState();
    auto const leader = std::ranges::find(state.members, LeaderId, &Cluster::ClusterMember::id);
    REQUIRE(leader != state.members.end());
    CHECK(leader->seat == Cluster::MemberSeat::Voter);
    CHECK(seed.cluster.Proposed().empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsManual) == 0);
    CHECK(Unwrap(seed.window.Find(LeaderId)).decision == Wire::EnrollmentDecision::Pending);
}

TEST_CASE("A machine the cluster already records is answered the roster with no row to approve",
          "[enrollment][responder][formation]")
{
    // The list is one leader's memory and the admission is replicated: a joiner admitted just
    // before the leader changed asks the new one, which holds no row for it.
    Seed seed;
    auto state = seed.cluster.ClusterState();
    state.members.push_back(
        Cluster::ClusterMember { .id = "laptop",
                                 .raftEndpoint = {},
                                 .schedulerEndpoint = {},
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                 .seat = Cluster::MemberSeat::Learner,
                                 .publicKey = Filled(0x6C) });
    seed.cluster.SetState(std::move(state));

    auto const reply = LearnerRequest(seed, "laptop", Filled(0x6C));
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
    CHECK_FALSE(seed.window.Find("laptop").has_value());

    // The control: the same id under ANOTHER key is recorded and waits for a person -- and
    // "another" is exact: a key one bit away from the recorded one is another key.
    auto nearMiss = Filled(0x6C);
    nearMiss.back() ^= std::byte { 0x01 };
    auto const other = LearnerRequest(seed, "laptop", nearMiss);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(other))).outcome == Wire::EnrollOutcome::Pending);
    REQUIRE(seed.window.Find("laptop").has_value());
    CHECK(Unwrap(seed.window.Find("laptop")).publicKey == nearMiss);

    // And once that row exists it decides, whatever the cluster records: the recorded key asking
    // again is ANOTHER MACHINE to this row, counted in `claimsChanged` and answered `Pending`.
    auto const recordedAgain = LearnerRequest(seed, "laptop", Filled(0x6C));
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(recordedAgain))).outcome == Wire::EnrollOutcome::Pending);
    CHECK(Unwrap(seed.window.Find("laptop")).claimsChanged == 1);
}

TEST_CASE("A refused approval is not counted as one", "[enrollment][responder][formation]")
{
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    seed.cluster.RefuseWith(ConsensusError { .code = ConsensusErrorCode::ConfigurationChangeInFlight,
                                             .context = "another change is committing",
                                             .knownLeader = std::nullopt });
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) != std::nullopt);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsManual) == 0);
}

TEST_CASE("The approve reply carries no private key, and a planted one IS found by the same scan",
          "[enrollment][responder][security]")
{
    // **The acceptance case for #178 PR 4.** An approval used to hand the joiner the
    // cluster's pre-shared key in cleartext; it now hands over the roster, which is every
    // member's PUBLIC key. This scans the WHOLE approve reply -- header, outcome and roster
    // -- for every secret a leader holds, in every spelling one has ever been written in.
    //
    // The secrets are the real shapes: the leader's identity key as `NodeKey` stores it (the
    // 32-byte seed) and as Monocypher signs with it (the seed followed by the public key),
    // and a shared secret as the retired `--cluster-key-file` held one.
    auto const leaderSeed = Filled(0x5A);
    auto const leaderPublic = Ed25519KeyPair::FromSeed(leaderSeed).value().PublicKey();
    auto secretKey = std::vector<std::byte>(leaderSeed.begin(), leaderSeed.end());
    secretKey.insert(secretKey.end(), leaderPublic.begin(), leaderPublic.end());
    auto const sharedSecret = Wire::AsBytes("this-cluster-shared-secret-0123456789");
    auto const secrets = std::array<std::vector<std::byte>, 3> {
        std::vector<std::byte>(leaderSeed.begin(), leaderSeed.end()),
        secretKey,
        std::vector<std::byte>(sharedSecret.begin(), sharedSecret.end()),
    };

    Seed seed;
    // The leader's PUBLIC key is in the roster, which is the point of a roster; the private
    // half must not be.
    auto state = seed.cluster.ClusterState();
    state.members.front().publicKey = leaderPublic;
    // And a setting whose value is the shared secret's base64 -- a field the WHOLE state carries
    // and a roster must drop. No setting holds a secret today (`RefusedSettingTable` refuses
    // the credential-shaped ones by name); this is the defence under that one.
    state.settings.push_back(Cluster::Setting { .name = "upstream", .value = Base64Encode(secrets[2]) });
    seed.cluster.SetState(state);

    RecordAndApprove(seed);
    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    REQUIRE(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);

    // The reply is NOT empty -- a scan of nothing finds nothing, and would pass under a
    // responder that answered `Approved` with no roster at all.
    REQUIRE_FALSE(Unwrap(decoded).roster.empty());
    CHECK(Carries(reply, leaderPublic));

    for (auto const& secret: secrets)
        CHECK_FALSE(Carries(reply, secret));

    // **The positive control, through the SAME scan and the SAME encoder.** The planted
    // setting is really there -- the whole state carries it -- so the roster not carrying
    // it is the roster's doing and not the plant's absence.
    CHECK(Carries(Cluster::Encode(seed.cluster.ClusterState()), secrets[2]));

    // And a key planted where a roster DOES travel is found in the reply, raw and as text:
    // the seed's base64url as a principal's id, and its raw bytes as a revoked key. Without
    // this, every check above passes under a scan that cannot see into the roster at all.
    auto planted = seed.cluster.ClusterState();
    planted.principals.push_back(Cluster::ClusterPrincipal {
        .id = Base64UrlEncode(secrets[0]), .publicKey = Filled(0x33), .role = Cluster::PrincipalRole::Worker });
    planted.revokedKeys.push_back(Cluster::RevokedKey { .id = "planted", .publicKey = leaderSeed });
    seed.cluster.SetState(planted);
    auto const leaking = Enroll(seed);
    REQUIRE(Unwrap(Wire::DecodeEnrollReply(PayloadOf(leaking))).outcome == Wire::EnrollOutcome::Approved);
    CHECK(Carries(leaking, secrets[0]));
}

TEST_CASE("The cluster is asked BEFORE the window is marked, so a refused change hands out nothing",
          "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);

    // A leader that cannot accept the change right now, which is what a healthy cluster
    // answers while an earlier membership change is still replicating.
    seed.cluster.RefuseWith(ConsensusError { .code = ConsensusErrorCode::ConfigurationChangeInFlight,
                                             .context = "another change is committing",
                                             .knownLeader = std::nullopt });

    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == Wire::ErrorCode::ClusterChangeInFlight);

    // The ORDER is the property, and it is only visible from the joiner's side: the
    // window was not marked, so the machine goes on waiting rather than being told it was
    // admitted to a membership the cluster refused.
    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);
    CHECK(Unwrap(decoded).roster.empty());
    CHECK(Unwrap(seed.window.Find(JoinerId)).decision == Wire::EnrollmentDecision::Pending);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);
}

TEST_CASE("An approved joiner is answered on every poll, so a lost reply strands nobody",
          "[enrollment][responder][security]")
{
    // **The cost the spend used to carry, and the reason it is gone** (#178). While an
    // approval handed over the cluster key, the key was spendable once -- and a joiner whose
    // one reply was lost was refused by every retry until an operator re-approved it. The
    // roster is no secret, so it is answered every time, and the second and third answers
    // are the ones that tell the two designs apart.
    Seed seed;
    RecordAndApprove(seed);

    for (auto const poll: std::views::iota(1, 4))
    {
        auto const reply = Enroll(seed);
        auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
        REQUIRE(decoded.has_value());
        CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);
        CHECK_FALSE(Unwrap(decoded).roster.empty());

        // Counted per roster SERVED, not per joiner: a rise without new approvals is a
        // joiner asking again, which is harmless and is what the counter says.
        CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == static_cast<std::uint64_t>(poll));
    }
}

TEST_CASE("An approval is answered only once the leader's own roster records the joiner", "[enrollment][responder]")
{
    // `ClusterAdmit` returns once the entry is APPENDED, and a joiner polls every couple of
    // seconds. Answering `Approved` in between would hand it a roster that lacks its own
    // entry -- which is exactly what the joiner is told to refuse, so it would report a
    // healthy approval as a roster somebody else produced.
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    seed.cluster.HoldApplies();
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
    REQUIRE(Unwrap(seed.window.Find(JoinerId)).decision == Wire::EnrollmentDecision::Approved);

    auto const early = Enroll(seed);
    auto const waiting = Wire::DecodeEnrollReply(PayloadOf(early));
    REQUIRE(waiting.has_value());
    CHECK(Unwrap(waiting).outcome == Wire::EnrollOutcome::Pending);
    CHECK(Unwrap(waiting).roster.empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);

    // The control: once it commits, the same poll is answered. Without it the case above
    // passes under a responder that never answers `Approved` at all.
    seed.cluster.CommitHeld();
    auto const late = Enroll(seed);
    auto const admitted = Wire::DecodeEnrollReply(PayloadOf(late));
    REQUIRE(admitted.has_value());
    CHECK(Unwrap(admitted).outcome == Wire::EnrollOutcome::Approved);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 1);
}

TEST_CASE("A poll under the approved id with another key is told nothing", "[enrollment][responder][security]")
{
    // The id is not the credential; the key the operator compared is. A second machine
    // asking under an approved id with its own key is answered `Pending` and handed no
    // roster -- it is not admitted, and telling it so would be telling it who is.
    Seed seed;
    RecordAndApprove(seed);

    auto const impostor = AnswerNow(seed.responder, EnrollFrame(JoinerId, "", JoinerRole, Filled(0x99)), JoinerAddress);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(impostor));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);
    CHECK(Unwrap(decoded).roster.empty());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);

    // Visible on the row an operator reads, and the machine that asked first is still the
    // one the approval answers.
    CHECK(Unwrap(seed.window.Find(JoinerId)).claimsChanged == 1);
    auto const genuine = Enroll(seed);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(genuine))).outcome == Wire::EnrollOutcome::Approved);
}

TEST_CASE("Approving a machine already recorded under its key is satisfied rather than refused", "[enrollment][responder]")
{
    // An operator's admission recorded the machine first, naming the same key. The
    // cluster answers *already in force*, which is a `Satisfied` refusal: the approval
    // changes nothing but what this window answers, and with no secret at stake answering
    // the roster on that earlier decision hands out nothing it had not already made public.
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    auto state = seed.cluster.ClusterState();
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::AdmitPrincipal,
                                      .key = std::string { JoinerId },
                                      .value = {},
                                      .schedulerEndpoint = {},
                                      .publicKey = JoinerKey(),
                                      .role = Cluster::PrincipalRole::Worker });
    seed.cluster.SetState(state);

    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
    auto const reply = Enroll(seed);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
}

TEST_CASE("A rejected joiner is told so and stays rejected until somebody changes their mind", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Rejected);
    CHECK(Unwrap(decoded).roster.empty());

    // Rejecting proposes NOTHING: a machine refused at the door must not appear in the
    // cluster's record under any reading.
    CHECK(seed.cluster.Proposed().empty());
    CHECK(std::ranges::none_of(seed.cluster.ClusterState().members,
                               [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
    CHECK(std::ranges::none_of(seed.cluster.ClusterState().principals,
                               [](Cluster::ClusterPrincipal const& p) { return p.id == JoinerId; }));
}

TEST_CASE("The pending list fills, refuses the next machine by name, and keeps the first", "[enrollment][responder]")
{
    Seed seed;

    // Each from a host of its own, so the list's bound is what is reached and not a host's.
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
    {
        auto const id = std::format("crowd-{}", index);
        auto const host = std::format("203.0.113.{}", index);
        REQUIRE(RefusalIn(AnswerNow(seed.responder, EnrollFrame(id, "", Wire::EnrollRole::Learner, Filled(0x66)), host))
                == std::nullopt);
    }

    CHECK(RefusalIn(Enroll(seed)) == Wire::ErrorCode::EnrollmentFull);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedFull) == 1);

    // The eviction case a suite skips: the machine that arrived FIRST is still on the
    // list an operator is reading. Without this, an implementation that evicted to make
    // room and then refused would pass every assertion above.
    auto const listing = Control(seed, Wire::EnrollControlVerb::List);
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(listing));
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
    CHECK(
        !seed.responder
             .RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } }, static_cast<std::uint8_t>(Wire::Op::Enroll))
             .has_value());

    // And the decision verb is refused to the same peer, before a payload is read, with
    // the counter that says somebody tried to approve themselves.
    auto const refused = seed.responder.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } },
                                                   static_cast<std::uint8_t>(Wire::Op::EnrollControl));
    REQUIRE(refused.has_value());
    CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NotAMember);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedNotAMember) == 1);

    // The operator's machine is admitted to both, so the refusal above is about the
    // peer rather than about the verb being closed to everybody.
    CHECK(!seed.responder
               .RefusePeer(PeerIdentity { .host = std::string { OperatorAddress } },
                           static_cast<std::uint8_t>(Wire::Op::EnrollControl))
               .has_value());
}

TEST_CASE("A follower answers enrollment with the leader's endpoint rather than a window of its own",
          "[enrollment][responder]")
{
    Seed seed;
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

    // One byte that belongs to no UTF-8 sequence. Refused HERE rather than at the
    // approval, because an id copied into `ClusterState` is read back out of
    // `/fleet.json` by everybody -- and a consensus entry is applied after it is
    // committed, with nobody left to refuse it.
    auto const reply = AnswerNow(seed.responder,
                                 EnrollFrame("joiner-\xff"
                                             "a",
                                             "",
                                             Wire::EnrollRole::Learner,
                                             JoinerKey()),
                                 JoinerAddress);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::MalformedFrame);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 1);

    // Nothing was recorded, so the operator's list does not carry a row they cannot read.
    auto const listing = Control(seed, Wire::EnrollControlVerb::List);
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(listing));
    REQUIRE(report.has_value());
    CHECK(Unwrap(report).pending.empty());
}

TEST_CASE("A joiner's id is held to the one id bound where it enters", "[enrollment][responder]")
{
    // An id past the bound would make a row `--enroll-reject` refuses to name, removable only by
    // `--enroll-clear`, which drops every honest row with it. So it is never recorded.
    Seed seed;
    auto const atBound = std::string(Wire::MaxIdBytes, 'a');
    auto const pastBound = std::string(Wire::MaxIdBytes + 1, 'b');

    auto const accepted = LearnerRequest(seed, atBound, Filled(0x41));
    REQUIRE(RefusalIn(accepted) == std::nullopt);
    auto const reply = Wire::DecodeEnrollReply(PayloadOf(accepted));
    REQUIRE(reply.has_value()); // a default reply reads Pending, so an undecodable one must not pass as one
    CHECK(Unwrap(reply).outcome == Wire::EnrollOutcome::Pending);

    auto const refused = LearnerRequest(seed, pastBound, Filled(0x42));
    CHECK(RefusalIn(refused) == Wire::ErrorCode::MalformedFrame);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedIdTooLong) == 1);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 0); // its own row

    // The list holds the row at the bound, and nothing an operator could not name.
    auto const listing = Control(seed, Wire::EnrollControlVerb::List);
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(listing));
    REQUIRE(report.has_value());
    REQUIRE(Unwrap(report).pending.size() == 1);
    CHECK(Unwrap(report).pending[0].nodeId == atBound);
}

TEST_CASE("An armed window admits a learner as the leader and counts it apart from manual approvals",
          "[enrollment][auto-approve][formation]")
{
    Seed seed;
    auto const laptopKey = Filled(0x6C);
    REQUIRE(RefusalIn(AnswerNow(seed.responder, Wire::EncodeEnrollAutoApprove(std::chrono::minutes { 10 }), OperatorAddress))
            == std::nullopt);

    // Admitted on the leader's own authority, and answered `Pending`: the roster is not applied
    // yet on a real leader, and the joiner's next poll is the one answered `Approved`.
    seed.cluster.HoldApplies();
    auto const first = LearnerRequest(seed, "laptop", laptopKey);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(first))).outcome == Wire::EnrollOutcome::Pending);
    REQUIRE(seed.cluster.Proposed().size() == 1);
    CHECK(seed.cluster.Proposed()[0].kind == Cluster::CommandKind::AddLearner);
    CHECK(seed.cluster.Proposed()[0].publicKey == std::optional { laptopKey });
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsAuto) == 1);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsManual) == 0);

    seed.cluster.CommitHeld();
    auto const second = LearnerRequest(seed, "laptop", laptopKey);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(second))).outcome == Wire::EnrollOutcome::Approved);

    // The audit line: the row says the window admitted it, and when the window was armed.
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(Control(seed, Wire::EnrollControlVerb::List)));
    REQUIRE(report.has_value());
    CHECK(Unwrap(report).state == Wire::WireEnrollmentState::AutoApprove);
    REQUIRE(Unwrap(report).pending.size() == 1);
    CHECK(Unwrap(report).pending[0].autoApprovedArmedSecondsAgo.has_value());
    CHECK(Unwrap(report).pending[0].decision == Wire::EnrollmentDecision::Approved);
}

TEST_CASE("A demoted leader's window is disarmed and stays disarmed when it leads again",
          "[enrollment][auto-approve][formation]")
{
    // Through the scheduler's SetRole, which is the call consensus makes and the wiring the
    // responder installs: a leader that loses and regains leadership inside the deadline must
    // not resume admitting (RF-4).
    Seed seed;
    REQUIRE(RefusalIn(AnswerNow(seed.responder, Wire::EncodeEnrollAutoApprove(std::chrono::minutes { 10 }), OperatorAddress))
            == std::nullopt);
    seed.service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, Distributed::StandaloneSchedulerTerm);
    seed.service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);

    auto const reply = LearnerRequest(seed, "laptop", Filled(0x6C));
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Pending);
    CHECK(seed.cluster.Proposed().empty());
    CHECK(seed.window.Summary().first == Wire::WireEnrollmentState::Manual);
}

TEST_CASE("The leader refuses a zero or over-ceiling duration with the table's sentence",
          "[enrollment][auto-approve][formation]")
{
    Seed seed;
    for (auto const& [duration, refusal]:
         { std::pair { std::chrono::seconds { 0 }, AutoApproveRefusal::Zero },
           std::pair { std::chrono::seconds { AutoApproveCeiling } + std::chrono::seconds { 1 },
                       AutoApproveRefusal::OverCeiling } })
    {
        INFO(duration.count());
        auto const reply = AnswerNow(seed.responder, Wire::EncodeEnrollAutoApprove(duration), OperatorAddress);
        auto const header = Wire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        REQUIRE(Unwrap(header).status == Wire::Status::Error);
        auto const refused = Wire::DecodeErrorPayload(PayloadOf(reply));
        REQUIRE(refused.has_value());
        CHECK(Unwrap(refused).first == Wire::ErrorCode::InvalidClusterChange);
        CHECK(Unwrap(refused).second == AutoApproveSentence(refusal));
    }
    CHECK(seed.window.Summary().first == Wire::WireEnrollmentState::Manual);
}

TEST_CASE("Off ends an armed window, and ending one that is not armed is answered all the same",
          "[enrollment][auto-approve][formation]")
{
    Seed seed;
    REQUIRE(RefusalIn(AnswerNow(seed.responder, Wire::EncodeEnrollAutoApprove(std::chrono::minutes { 10 }), OperatorAddress))
            == std::nullopt);
    CHECK(seed.window.Summary().first == Wire::WireEnrollmentState::AutoApprove);
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::AutoApproveOff)) == std::nullopt);
    CHECK(seed.window.Summary().first == Wire::WireEnrollmentState::Manual);
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::AutoApproveOff)) == std::nullopt);
    CHECK(seed.cluster.Proposed().empty());
}

TEST_CASE("A control frame naming a subject the verb does not take is refused", "[enrollment][responder]")
{
    Seed seed;

    // The decoder's arity rule reaching the surface: `List` names nobody and `Approve`
    // must. Answering either by ignoring the mismatch is how an operator comes to
    // believe they approved somebody.
    CHECK(RefusalIn(
              AnswerNow(seed.responder, Wire::EncodeEnrollControl(Wire::EnrollControlVerb::List, JoinerId), OperatorAddress))
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
        auto const refused =
            merged.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } }, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalIn(Unwrap(refused)) != Wire::UnimplementedVerb);

        CHECK(RefusalIn(AnswerNow(merged, EnrollFrame(JoinerId, "", JoinerRole, JoinerKey()), JoinerAddress))
              == Wire::ErrorCode::NoCluster);
    }

    // And the families this does NOT cover still answer the unserved sentence, or the
    // fix above would have been "call everything NoCluster", which tells a launcher
    // asking a worker for a cache verb that it is in the wrong cluster.
    auto const cacheVerb = merged.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } },
                                             static_cast<std::uint8_t>(Wire::Op::Fetch));
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
    // committed the machine. The reject stops the roster being handed over -- worth having,
    // which is why it is not refused outright -- and leaves the machine in `ClusterState`,
    // holding its key, while `--enroll-reject`'s help text promised *"a machine refused here
    // was never a member and needs no --cluster-forget"*.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.metrics, logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        // An approval names the key the row holds, as the line `--enroll-list` prints does.
        if (verb == Wire::EnrollControlVerb::Approve)
            return AnswerNow(responder,
                             Wire::EncodeEnrollApprove(
                                 subject, seed.window.Find(subject).value_or(Wire::EnrollmentPendingEntry {}).publicKey),
                             OperatorAddress);
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(AnswerNow(responder, EnrollFrame(JoinerId, "", JoinerRole, JoinerKey()), JoinerAddress))
            == std::nullopt);
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);

    // The approval really did commit it, or the rest of this case would be asserting
    // a warning about a state the cluster is not in.
    REQUIRE(std::ranges::any_of(seed.cluster.ClusterState().principals,
                                [](Cluster::ClusterPrincipal const& p) { return p.id == JoinerId; }));

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    auto const records = logger.Snapshot();
    auto const warned = std::ranges::find_if(
        records, [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn && r.message.contains(JoinerId); });
    REQUIRE(warned != records.end());

    // It names the REMEDY, which is the whole point: an operator who read the flag's
    // own description believes there is nothing left to do.
    CHECK(warned->message.contains("--cluster-forget=joiner-a"));

    // And the machine IS still there, so the warning is true rather than defensive.
    CHECK(std::ranges::any_of(seed.cluster.ClusterState().principals,
                              [](Cluster::ClusterPrincipal const& p) { return p.id == JoinerId; }));
}

TEST_CASE("Rejecting an approved WORKER names the forget that removes it", "[enrollment][responder][security]")
{
    // One remedy for both roles since #1555: `--cluster-forget` takes the id out of whichever
    // list records it -- a principal included -- and revokes its key. The case below is what
    // makes naming it here true rather than hopeful.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.metrics, logger };
    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        // An approval names the key the row holds, as the line `--enroll-list` prints does.
        if (verb == Wire::EnrollControlVerb::Approve)
            return AnswerNow(responder,
                             Wire::EncodeEnrollApprove(
                                 subject, seed.window.Find(subject).value_or(Wire::EnrollmentPendingEntry {}).publicKey),
                             OperatorAddress);
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(
        RefusalIn(AnswerNow(responder, EnrollFrame("worker-a", "", Wire::EnrollRole::Worker, JoinerKey()), JoinerAddress))
        == std::nullopt);
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Approve, "worker-a")) == std::nullopt);
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Reject, "worker-a")) == std::nullopt);

    auto const records = logger.Snapshot();
    auto const warned = std::ranges::find_if(records, [](CapturingLogger::Record const& r) {
        return r.level == LogLevel::Warn && r.message.contains("worker-a");
    });
    REQUIRE(warned != records.end());
    CHECK(warned->message.contains("--cluster-forget=worker-a"));
    CHECK(warned->message.contains("revokes that key"));
}

TEST_CASE("A forgotten worker's key is revoked: its next enrollment is refused at the door, and never approved again",
          "[enrollment][responder][security][forget]")
{
    // #1555, end to end on the one wire a worker principal's key is presented on in this
    // build. The operator approves worker-a, then forgets it through the verb
    // `--cluster-forget` reaches; the forget takes its principal row and revokes its key. The
    // neuter is a forget that leaves the principal where it was, under which the next poll is
    // answered `Approved`, roster and all -- still admitted.
    Seed seed;
    auto const workerKey = Filled(0x57);
    auto const enroll = [&](std::string_view id) {
        return AnswerNow(seed.responder, EnrollFrame(id, "", Wire::EnrollRole::Worker, workerKey), JoinerAddress);
    };

    REQUIRE(RefusalIn(enroll("worker-a")) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "worker-a")) == std::nullopt);

    // Admitted, which is what makes the rest mean something: it is handed the roster.
    auto const admitted = enroll("worker-a");
    REQUIRE(RefusalIn(admitted) == std::nullopt);
    REQUIRE(Unwrap(Wire::DecodeEnrollReply(PayloadOf(admitted))).outcome == Wire::EnrollOutcome::Approved);

    auto const forgot =
        seed.service.ClusterForget(Distributed::CallerContext { .membership = Distributed::Membership::Member,
                                                                .peerId = std::string { OperatorAddress } },
                                   "worker-a");
    REQUIRE(forgot.status == Wire::Status::Ok);

    auto const state = seed.cluster.ClusterState();
    CHECK(state.principals.empty());
    CHECK(state.IsRevoked(workerKey));
    CHECK(std::ranges::contains(state.revokedKeys, Cluster::RevokedKey { .id = "worker-a", .publicKey = workerKey }));

    // The next poll is REFUSED, by name and counted -- not `Pending`, which would keep a
    // removed machine polling for an answer that cannot come, and not `Approved`.
    auto const refused = enroll("worker-a");
    CHECK(RefusalIn(refused) == Wire::ErrorCode::InvalidClusterChange);
    auto const message = Unwrap(Wire::DecodeErrorPayload(PayloadOf(refused))).second;
    CHECK(message.contains("never admitted again"));
    CHECK(message.contains(FormatEd25519PublicKey(workerKey)));
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 1);

    // Under another id too: the key is what is refused, not the name it arrives with -- and
    // nothing is recorded for the operator to be shown.
    CHECK(RefusalIn(enroll("worker-b")) == Wire::ErrorCode::InvalidClusterChange);
    CHECK_FALSE(seed.window.Find("worker-b").has_value());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 2);

    // And the row the window still holds cannot be approved back: rejected, then approved
    // again, the approval reaches the cluster and is refused there as a revoked key.
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Reject, "worker-a")) == std::nullopt);
    auto const again = Control(seed, Wire::EnrollControlVerb::Approve, "worker-a");
    CHECK(RefusalIn(again) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(Unwrap(Wire::DecodeErrorPayload(PayloadOf(again))).second.contains("a revoked key is never admitted again"));
    CHECK(seed.cluster.ClusterState().principals.empty());
}

TEST_CASE("A joiner asking under a small-order or non-canonical key is refused at the door, by name",
          "[enrollment][responder][security][identity]")
{
    // A row an operator might approve is a key the cluster would then admit, and under a
    // small-order key the all-zero signature verifies every message -- so the approval would admit
    // whoever cares to claim it. Refused before the window records anything, for either role, on
    // the malformed row: no build of this software mints such a key. The control is an ordinary
    // key asking the same way, which the window records.
    auto const nonCanonical = [] {
        auto key = Filled(0xFF);
        key.front() = std::byte { 0xF0 };
        key.back() = std::byte { 0x7F };
        return key;
    }();
    Seed seed;

    auto refusals = std::uint64_t { 0 };
    for (auto const role: { Wire::EnrollRole::Learner, Wire::EnrollRole::Worker })
    {
        for (auto const& [key, fault]: { std::pair { Ed25519PublicKey {}, PublicKeyFault::SmallOrder },
                                         std::pair { nonCanonical, PublicKeyFault::NonCanonical } })
        {
            INFO("role " << static_cast<int>(role) << ", key " << FormatEd25519PublicKey(key));
            auto const refused = AnswerNow(seed.responder, EnrollFrame("joiner-x", "", role, key), JoinerAddress);
            CHECK(RefusalIn(refused) == Wire::ErrorCode::MalformedFrame);
            auto const message = Unwrap(Wire::DecodeErrorPayload(PayloadOf(refused))).second;
            CHECK(message.contains(DescribePublicKeyFault(fault)));
            CHECK(message.contains(FormatEd25519PublicKey(key)));
            CHECK_FALSE(seed.window.Find("joiner-x").has_value());
            CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == ++refusals);
        }
    }

    auto const control =
        AnswerNow(seed.responder, EnrollFrame("joiner-x", "", Wire::EnrollRole::Learner, Filled(0x59)), JoinerAddress);
    CHECK(RefusalIn(control) == std::nullopt);
    CHECK(seed.window.Find("joiner-x").has_value());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == refusals);
}

TEST_CASE("A machine nobody forgot enrolls as before, beside a revoked key", "[enrollment][responder][forget]")
{
    // The control for the door: a revocation refuses the key it names and nothing else, so a
    // cluster holding one still records and admits a stranger asking under its own key.
    Seed seed;
    auto state = seed.cluster.ClusterState();
    state.revokedKeys.push_back(Cluster::RevokedKey { .id = "gone", .publicKey = Filled(0x58) });
    seed.cluster.SetState(std::move(state));

    auto const reply = Enroll(seed);
    CHECK(RefusalIn(reply) == std::nullopt);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Pending);
    CHECK(seed.window.Find(JoinerId).has_value());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 0);
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
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.metrics, logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        // An approval names the key the row holds, as the line `--enroll-list` prints does.
        if (verb == Wire::EnrollControlVerb::Approve)
            return AnswerNow(responder,
                             Wire::EncodeEnrollApprove(
                                 subject, seed.window.Find(subject).value_or(Wire::EnrollmentPendingEntry {}).publicKey),
                             OperatorAddress);
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(AnswerNow(responder, EnrollFrame(JoinerId, "", JoinerRole, JoinerKey()), JoinerAddress))
            == std::nullopt);

    // Straight to `Reject`, so the row never left `Pending`.
    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    CHECK(
        std::ranges::none_of(logger.Snapshot(), [](CapturingLogger::Record const& r) { return r.level == LogLevel::Warn; }));

    // Nothing was ever offered to consensus, which is why there is nothing to warn about.
    CHECK(seed.cluster.Proposed().empty());
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
    for (auto const raw: std::views::iota(std::uint32_t { 0 }, 0xFFU + 1))
    {
        auto const byte = static_cast<std::uint8_t>(raw);
        auto const isEnrollment = Wire::FamilyOf(byte) == Wire::VerbFamily::Enrollment;
        CHECK((merged.OwnerOf(byte) == &seed.responder) == isEnrollment);
    }
}

TEST_CASE("One host past its cap is refused by its own code and counter while another host enrolls",
          "[enrollment][responder][security]")
{
    Seed seed;
    constexpr std::string_view Flooder = "203.0.113.7";
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(RefusalIn(AnswerNow(seed.responder,
                                    EnrollFrame(std::format("junk-{}", index), "", Wire::EnrollRole::Learner, Filled(0x66)),
                                    Flooder))
                == std::nullopt);

    auto const refused =
        AnswerNow(seed.responder, EnrollFrame("junk-more", "", Wire::EnrollRole::Learner, Filled(0x66)), Flooder);
    CHECK(RefusalIn(refused) == Wire::ErrorCode::EnrollmentHostFull);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedHostCap) == 1);
    // Not the full list's: the two send an operator to different places.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedFull) == 0);

    // The genuine joiner, from its own host, still gets a row.
    CHECK(RefusalIn(Enroll(seed)) == std::nullopt);
    CHECK(seed.window.Find(JoinerId).has_value());
}

TEST_CASE("The clear verb drops the undecided rows, keeps an approved one, and counts what it dropped",
          "[enrollment][responder][security]")
{
    Seed seed;
    RecordAndApprove(seed);
    for (auto const index: std::views::iota(0, 3))
        REQUIRE(RefusalIn(AnswerNow(seed.responder,
                                    EnrollFrame(std::format("junk-{}", index), "", Wire::EnrollRole::Learner, Filled(0x66)),
                                    std::format("203.0.113.{}", index)))
                == std::nullopt);

    auto const reply = Control(seed, Wire::EnrollControlVerb::Clear);
    REQUIRE(RefusalIn(reply) == std::nullopt);
    auto const report = Wire::DecodeEnrollmentReport(PayloadOf(reply));
    REQUIRE(report.has_value());
    REQUIRE(Unwrap(report).pending.size() == 1);
    CHECK(Unwrap(report).pending[0].nodeId == JoinerId);
    CHECK(Unwrap(report).pending[0].decision == Wire::EnrollmentDecision::Approved);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsCleared) == 3);

    // Leader-only, like every enrollment control verb.
    seed.service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, Distributed::StandaloneSchedulerTerm);
    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Clear)) == Wire::ErrorCode::NotLeader);
}

TEST_CASE("An approval naming the key of a row that lapsed is refused once another machine took its id",
          "[enrollment][responder][security]")
{
    // The operator compared `first`; that machine stopped asking, its row lapsed, and another
    // machine asked under the same id with `second`. An approval naming `first` must admit
    // nothing -- by id alone it would have admitted `second`, which nobody compared.
    Seed seed;
    auto const first = Filled(0x6A);
    auto const second = Filled(0x6B);
    REQUIRE(RefusalIn(LearnerRequest(seed, "laptop", first)) == std::nullopt);
    seed.clock.advance(PendingRowLifetime + std::chrono::seconds { 1 });
    REQUIRE(RefusalIn(LearnerRequest(seed, "laptop", second)) == std::nullopt);
    REQUIRE(Unwrap(seed.window.Find("laptop")).publicKey == second);

    auto const refused = ApproveUnder(seed, "laptop", first);
    CHECK(RefusalIn(refused) == Wire::ErrorCode::InvalidClusterChange);
    auto const refusal = Wire::DecodeErrorPayload(PayloadOf(refused));
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).second.contains(FormatEd25519PublicKey(first)));
    CHECK(Unwrap(refusal).second.contains(FormatEd25519PublicKey(second)));
    CHECK(Unwrap(refusal).second.contains("nothing was admitted"));
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsRefusedKeyMismatch) == 1);
    CHECK(seed.cluster.Proposed().empty());
    CHECK(Unwrap(seed.window.Find("laptop")).decision == Wire::EnrollmentDecision::Pending);

    // The control: the key the row DOES hold is admitted.
    CHECK(RefusalIn(ApproveUnder(seed, "laptop", second)) == std::nullopt);
    REQUIRE(seed.cluster.Proposed().size() == 1);
    CHECK(seed.cluster.Proposed()[0].publicKey == std::optional { second });
}

TEST_CASE("A demoted leader forgets its list, lowers the waiting condition and sends a poll to the new leader",
          "[enrollment][responder][formation]")
{
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(seed.conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);

    seed.service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, Distributed::StandaloneSchedulerTerm);
    CHECK(seed.conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Clear);
    CHECK(seed.window.Report().pending.empty());

    auto const poll = Enroll(seed);
    CHECK(RefusalIn(poll) == Wire::ErrorCode::NotLeader);
    auto const refusal = Wire::DecodeErrorPayload(PayloadOf(poll));
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).second == LeaderEndpoint);

    // Leading again brings nothing back: the list was this leadership's.
    seed.service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    CHECK(seed.window.Report().pending.empty());
}

TEST_CASE("A revoked key never qualifies for the recorded-joiner answer, even while a record names it",
          "[enrollment][responder][forget]")
{
    // The revocation is asked FIRST: a record that still names a revoked key -- which `Apply`
    // does not produce, and a store from elsewhere might -- is refused at the door, never answered
    // the roster.
    Seed seed;
    auto state = seed.cluster.ClusterState();
    state.members.push_back(
        Cluster::ClusterMember { .id = "laptop",
                                 .raftEndpoint = {},
                                 .schedulerEndpoint = {},
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                 .seat = Cluster::MemberSeat::Learner,
                                 .publicKey = Filled(0x6C) });
    state.revokedKeys.push_back(Cluster::RevokedKey { .id = "laptop", .publicKey = Filled(0x6C) });
    seed.cluster.SetState(std::move(state));

    auto const reply = LearnerRequest(seed, "laptop", Filled(0x6C));
    CHECK(RefusalIn(reply) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 1);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);
    CHECK_FALSE(seed.window.Find("laptop").has_value());
}

TEST_CASE("A joiner admitted just before a change of leader is answered the roster, not stranded",
          "[enrollment][responder][formation]")
{
    // The strand this answer exists to prevent: approved and recorded, then the leader changes
    // and drops its list before the joiner's next poll. Without the recorded-joiner answer that
    // poll would be recorded afresh, and approving it refused as *already a member* -- a machine
    // the cluster admitted, polling forever.
    Seed seed;
    auto const key = Filled(0x6C);
    REQUIRE(RefusalIn(LearnerRequest(seed, "laptop", key)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop")) == std::nullopt);
    REQUIRE(std::ranges::any_of(seed.cluster.ClusterState().members,
                                [](Cluster::ClusterMember const& member) { return member.id == "laptop"; }));

    seed.service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, Distributed::StandaloneSchedulerTerm);
    seed.service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    REQUIRE_FALSE(seed.window.Find("laptop").has_value());

    auto const poll = LearnerRequest(seed, "laptop", key);
    REQUIRE(RefusalIn(poll) == std::nullopt);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(poll));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);
    CHECK_FALSE(Unwrap(decoded).roster.empty());
    // Nothing waits for an approval that would be refused.
    CHECK_FALSE(seed.window.Find("laptop").has_value());
}

TEST_CASE("A machine that re-polls its rows from a second address is still refused at its first address's cap",
          "[enrollment][responder][security]")
{
    Seed seed;
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
    {
        auto const frame = EnrollFrame(std::format("junk-{}", index), "", Wire::EnrollRole::Learner, Filled(0x66));
        REQUIRE(RefusalIn(AnswerNow(seed.responder, frame, "198.51.100.7")) == std::nullopt);
        REQUIRE(RefusalIn(AnswerNow(seed.responder, frame, "2001:db8::7")) == std::nullopt);
    }
    auto const again =
        AnswerNow(seed.responder, EnrollFrame("junk-more", "", Wire::EnrollRole::Learner, Filled(0x66)), "198.51.100.7");
    CHECK(RefusalIn(again) == Wire::ErrorCode::EnrollmentHostFull);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedHostCap) == 1);

    // And the operator can find the rows the refusal is about: it names the host as the list
    // shows it on each of them, `first from`, although every one now shows the second address.
    auto const sentence = RefusalSentenceIn(again);
    CHECK(sentence.contains("198.51.100.7 already has 4 undecided request(s)"));
    CHECK(sentence.contains("first from 198.51.100.7"));
    auto const listed = seed.window.Report().pending;
    REQUIRE(listed.size() == MaxPendingEnrollmentsPerHost);
    for (auto const& row: listed)
    {
        INFO(row.nodeId);
        CHECK(row.peerId == "2001:db8::7");
        CHECK(row.firstPeerId == "198.51.100.7");
    }
}

TEST_CASE("A host refused at its cap is named as the list shows it, an IPv4-mapped spelling folded",
          "[enrollment][responder][security]")
{
    // A dual-stack listener reports an IPv4 client as `::ffff:a.b.c.d`. The bound folds that, so
    // the refusal names the folded host -- the spelling on the rows it counts.
    Seed seed;
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollmentsPerHost))
        REQUIRE(RefusalIn(AnswerNow(seed.responder,
                                    EnrollFrame(std::format("m-{}", index), "", Wire::EnrollRole::Learner, Filled(0x66)),
                                    "198.51.100.9"))
                == std::nullopt);
    auto const refused =
        AnswerNow(seed.responder, EnrollFrame("m-more", "", Wire::EnrollRole::Learner, Filled(0x66)), "::ffff:198.51.100.9");
    REQUIRE(RefusalIn(refused) == Wire::ErrorCode::EnrollmentHostFull);
    CHECK(RefusalSentenceIn(refused).starts_with("198.51.100.9 already has"));
    CHECK_FALSE(RefusalSentenceIn(refused).contains("::ffff:"));
}
