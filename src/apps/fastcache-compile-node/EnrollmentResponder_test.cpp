// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
constexpr std::string_view JoinerId = "joiner-a";
constexpr std::string_view JoinerEndpoint = "198.51.100.4:7100";

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

    ManualClock clock;
    ManualWallClock wallClock;
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
    EnrollmentWindow window { clock };
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
    // machine that holds no cluster key, so a proof is not a state a joiner can be in.
    return SyncRun(responder.Answer(frame, PeerIdentity { .host = std::string { peer } })).bytes;
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

/// An `Enroll` frame.
/// @param id The identity claimed.
/// @param endpoint The consensus endpoint claimed; empty for a worker.
/// @param role What it asks to be.
/// @param key The key it asks under.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> EnrollFrame(std::string_view id,
                                                 std::string_view endpoint,
                                                 Wire::EnrollRole role,
                                                 Ed25519PublicKey const& key)
{
    return Wire::EncodeEnroll(
        Wire::EnrollRequest { .nodeId = id, .raftEndpoint = endpoint, .role = role, .publicKey = key });
}

/// One `Enroll` from the joiner, as a member under its own key.
/// @param seed The wired fixture.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Enroll(Seed& seed)
{
    return AnswerNow(
        seed.responder, EnrollFrame(JoinerId, JoinerEndpoint, Wire::EnrollRole::Member, JoinerKey()), JoinerAddress);
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

/// Open the window, record the joiner, and approve it.
/// @param seed The wired fixture.
void OpenAndApprove(Seed& seed)
{
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
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
/// TEXT -- `--cluster-key-file` holds base64 -- and a hand-over that forwarded the file's
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
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 0);
}

TEST_CASE("An open window records a joiner under its key and answers it with no roster bytes", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);

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
    CHECK(Unwrap(row).role == Wire::EnrollRole::Member);
}

TEST_CASE("Approving a member records it under its key AND hands it a roster that says so", "[enrollment][responder]")
{
    Seed seed;
    OpenAndApprove(seed);

    // Half one: the cluster records the joiner, under exactly the key the row holds. Both
    // halves are asserted because either alone is green under half the defect: a roster
    // handed to a machine the cluster never recorded is a joiner told it was admitted, and
    // a member recorded with no roster handed over is one that cannot tell who its peers are.
    auto const state = seed.cluster.ClusterState();
    auto const recorded = std::ranges::find(state.members, JoinerId, &Cluster::ClusterMember::id);
    REQUIRE(recorded != state.members.end());
    CHECK(recorded->raftEndpoint == JoinerEndpoint);
    CHECK(recorded->publicKey == JoinerKey());

    // Half two: the reply is the roster, and the roster records the joiner.
    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);
    auto const roster = Cluster::DecodeRoster(Unwrap(decoded).roster);
    REQUIRE(roster.has_value());
    CHECK(RosterRecordsJoiner(roster.value(), JoinerId, JoinerKey(), Wire::EnrollRole::Member));
    CHECK(RosterRecordsJoiner(roster.value(), LeaderId, Filled(0x01), Wire::EnrollRole::Member));
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRostersServed) == 1);

    // And the fingerprint of what was SENT is on the row, over the very bytes the joiner
    // received -- which is what makes the two strings an operator compares comparable.
    CHECK(Unwrap(seed.window.Find(JoinerId)).rosterFingerprint == Cluster::DigestOfRoster(Unwrap(decoded).roster));
}

TEST_CASE("Approving a worker admits a principal, never a member", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);

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
}

TEST_CASE("A role that does not suit the endpoint is refused before it reaches the list", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);

    // A member with no endpoint is a member the cluster counts and cannot reach; a worker
    // with one claims an address the principal it becomes has nowhere to keep. Both are
    // refused where they enter, because the list is what a person reads.
    CHECK(RefusalIn(AnswerNow(seed.responder, EnrollFrame("m", "", Wire::EnrollRole::Member, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(RefusalIn(AnswerNow(
              seed.responder, EnrollFrame("w", "10.0.0.9:7100", Wire::EnrollRole::Worker, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 2);
    CHECK(seed.window.Summary().second == 0);
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
    // and a cluster key as `--cluster-key-file` holds one.
    auto const leaderSeed = Filled(0x5A);
    auto const leaderPublic = Ed25519KeyPair::FromSeed(leaderSeed).value().PublicKey();
    auto secretKey = std::vector<std::byte>(leaderSeed.begin(), leaderSeed.end());
    secretKey.insert(secretKey.end(), leaderPublic.begin(), leaderPublic.end());
    auto const clusterKey = Wire::AsBytes("this-cluster-shared-secret-0123456789");
    auto const secrets = std::array<std::vector<std::byte>, 3> {
        std::vector<std::byte>(leaderSeed.begin(), leaderSeed.end()),
        secretKey,
        std::vector<std::byte>(clusterKey.begin(), clusterKey.end()),
    };

    Seed seed;
    // The leader's PUBLIC key is in the roster, which is the point of a roster; the private
    // half must not be.
    auto state = seed.cluster.ClusterState();
    state.members.front().publicKey = leaderPublic;
    // And a setting whose value is the cluster key's base64 -- a field the WHOLE state carries
    // and a roster must drop. No setting holds a secret today (`RefusedSettingTable` refuses
    // the credential-shaped ones by name); this is the defence under that one.
    state.settings.push_back(Cluster::Setting { .name = "upstream", .value = Base64Encode(secrets[2]) });
    seed.cluster.SetState(state);

    OpenAndApprove(seed);
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
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
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
    OpenAndApprove(seed);

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
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
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
    OpenAndApprove(seed);

    auto const impostor = AnswerNow(
        seed.responder, EnrollFrame(JoinerId, JoinerEndpoint, Wire::EnrollRole::Member, Filled(0x99)), JoinerAddress);
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
    // An operator's `--cluster-admit` recorded the machine first, naming the same key. The
    // cluster answers *already in force*, which is a `Satisfied` refusal: the approval
    // changes nothing but what this window answers, and with no secret at stake answering
    // the roster on that earlier decision hands out nothing it had not already made public.
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    auto state = seed.cluster.ClusterState();
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                      .key = std::string { JoinerId },
                                      .value = std::string { JoinerEndpoint },
                                      .schedulerEndpoint = {},
                                      .publicKey = JoinerKey(),
                                      .role = std::nullopt });
    seed.cluster.SetState(state);

    CHECK(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, JoinerId)) == std::nullopt);
    auto const reply = Enroll(seed);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
}

TEST_CASE("A rejected joiner is told so and stays rejected until somebody changes their mind", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Reject, JoinerId)) == std::nullopt);

    auto const reply = Enroll(seed);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).outcome == Wire::EnrollOutcome::Rejected);
    CHECK(Unwrap(decoded).roster.empty());

    // Rejecting proposes NOTHING: a machine refused at the door must not appear in the
    // cluster's member record under any reading.
    CHECK(seed.cluster.Proposed().empty());
    CHECK(std::ranges::none_of(seed.cluster.ClusterState().members,
                               [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
}

TEST_CASE("The pending list fills, refuses the next machine by name, and keeps the first", "[enrollment][responder]")
{
    Seed seed;
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);

    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
    {
        auto const id = std::format("crowd-{}", index);
        REQUIRE(RefusalIn(AnswerNow(
                    seed.responder, EnrollFrame(id, "10.0.0.1:1", Wire::EnrollRole::Member, Filled(0x66)), JoinerAddress))
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
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);
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
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Open)) == std::nullopt);

    // One byte that belongs to no UTF-8 sequence. Refused HERE rather than at the
    // approval, because an id copied into `ClusterState` is read back out of
    // `/fleet.json` by everybody -- and a consensus entry is applied after it is
    // committed, with nobody left to refuse it.
    auto const reply = AnswerNow(seed.responder,
                                 EnrollFrame("joiner-\xff"
                                             "a",
                                             JoinerEndpoint,
                                             Wire::EnrollRole::Member,
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
        auto const refused =
            merged.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } }, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalIn(Unwrap(refused)) != Wire::UnimplementedVerb);

        CHECK(RefusalIn(AnswerNow(
                  merged, EnrollFrame(JoinerId, JoinerEndpoint, Wire::EnrollRole::Member, JoinerKey()), JoinerAddress))
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
    // committed the member. The reject stops the roster being handed over -- worth having,
    // which is why it is not refused outright -- and leaves the machine in `ClusterState`,
    // counted towards quorum, while `--enroll-reject`'s help text promised *"a machine
    // refused here was never a member and needs no --cluster-forget"*.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.metrics, logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(RefusalIn(AnswerNow(
                responder, EnrollFrame(JoinerId, JoinerEndpoint, Wire::EnrollRole::Member, JoinerKey()), JoinerAddress))
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
    CHECK(warned->message.contains("--cluster-forget=joiner-a"));

    // And the member IS still there, so the warning is true rather than defensive.
    CHECK(std::ranges::any_of(seed.cluster.ClusterState().members,
                              [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
}

TEST_CASE("Rejecting an approved WORKER names no flag that would not remove it", "[enrollment][responder][security]")
{
    // The role's remedy, not the member's: `--cluster-forget` is `RemoveMember`, which never
    // touches a principal, so naming it here would send an operator to a command that
    // reports success and removes nothing. What is true is that nothing an operator can type
    // revokes a principal's key yet, and the warning says so and names the issue.
    Seed seed;
    CapturingLogger logger { LogLevel::Trace };
    EnrollmentResponder responder { seed.window, seed.service, seed.membership, seed.metrics, logger };
    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Open)) == std::nullopt);
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
    CHECK(warned->message.contains("#1555"));
    CHECK_FALSE(warned->message.contains("--cluster-forget"));
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
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(control(Wire::EnrollControlVerb::Open)) == std::nullopt);
    REQUIRE(RefusalIn(AnswerNow(
                responder, EnrollFrame(JoinerId, JoinerEndpoint, Wire::EnrollRole::Member, JoinerKey()), JoinerAddress))
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
