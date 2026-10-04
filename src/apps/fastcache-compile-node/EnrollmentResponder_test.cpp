// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentAbsence.hpp"
#include "EnrollmentResponder.hpp"
#include "EnrollmentWindow.hpp"
#include "Responders.hpp"

#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Cluster/EnrollRequestSignature.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/NodeConditionFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
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
/// second model of `AddMember`'s key rule, free to disagree with the one that ships.
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

    /// @copydoc Distributed::IClusterAdmin::NoteAnnouncedEndpoint
    void NoteAnnouncedEndpoint(Consensus::NodeId const& /*member*/, std::string /*endpoint*/) override {}

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

/// The key pairs a joiner in this file signs its `Enroll` with, one per byte value: REAL keys,
/// because the responder verifies every request's signature under the key it states. Derived once
/// and never changed after.
/// @return Every pair, indexed by the byte that names it.
[[nodiscard]] std::vector<Ed25519KeyPair> const& JoinerPairs()
{
    static auto const pairs = [] {
        auto all = std::vector<Ed25519KeyPair> {};
        for (auto const which: std::views::iota(0, 256))
            all.push_back(Testing::TestKeyPair(std::format("enroll-joiner-{}", which)));
        return all;
    }();
    return pairs;
}

/// A joiner's public key, named by one byte: one of `JoinerPairs`, so a request under it can be signed.
/// @param which The byte that names it.
/// @return The key.
[[nodiscard]] Ed25519PublicKey KeyOf(std::uint8_t which)
{
    return JoinerPairs()[which].PublicKey();
}

/// The pair behind @p key, which every `Enroll` this file sends is signed with.
/// @param key One of `KeyOf`'s.
/// @return Its pair.
[[nodiscard]] Ed25519KeyPair const& PairBehind(Ed25519PublicKey const& key)
{
    auto const* const pair =
        core::findIfOrNull(JoinerPairs(), [&key](Ed25519KeyPair const& one) { return one.PublicKey() == key; });
    if (pair == nullptr)
        throw std::logic_error { "every Enroll here is signed, so it asks under a key KeyOf names" };
    return *pair;
}

/// The key the joiner mints and asks under. The roster records whatever the approval names; the
/// request is signed with its pair.
[[nodiscard]] Ed25519PublicKey JoinerKey()
{
    return KeyOf(0x42);
}

/// The machine asking to join. Deliberately NOT on any member list.
constexpr std::string_view JoinerAddress = "198.51.100.4";

/// The operator's machine. On the member list, so it may decide.
constexpr std::string_view OperatorAddress = "10.0.0.7";

/// What a joiner claims about itself.
///
/// A LEARNER, stating no endpoint: the one role a machine joins as.
constexpr std::string_view JoinerId = "joiner-a";
constexpr Wire::EnrollRole JoinerRole = Wire::EnrollRole::Learner;

/// The cluster this seed's summary states, which every admission it answers is signed for.
constexpr std::string_view SeedClusterId = "c-seed";

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
    //
    // `MachineTicket`: an operator on another machine sends the enrollment decisions with the
    // ticket its own node mints, and an operator's control verb needs a route that IDENTIFIES the
    // caller -- `--fleet-open` admits nobody to it -- whose machine holds a VOTER's seat: a ticket
    // proves a machine, not an operator (W-1). The list stands in for the ticket the endpoint
    // verified; the open-policy caller and a learner's ticket are their own cases below.
    ListedMembership membership { { std::string { OperatorAddress } },
                                  Distributed::MembershipParticipant::MachineTicket,
                                  Distributed::KeyEvidenceSet {}.Add(Distributed::KeyEvidence::MachineTicket) };
    NodeConditions conditions;
    // Bound as `main` binds it: the node's own sink and the wall clock the scheduler reads, never
    // the defaults a fixture finds more convenient.
    EnrollmentWindow window { clock, &conditions, &metrics, wallClock };
    // What this node says about itself, and the key that signs every admission it answers.
    Testing::ScriptedSummarySource self { Wire::FleetSummary { .clusterId = std::string { SeedClusterId },
                                                               .state = Wire::FleetState::Established,
                                                               .leaderId = std::string { LeaderId },
                                                               .nodeId = std::string { LeaderId } } };
    Ed25519KeyPair const identity = Testing::TestKeyPair(std::string { LeaderId });
    // Where each row's challenge is drawn: eight distinct 32-byte draws before the script cycles,
    // more than any case here asks for, so two challenges a case compares are two different ones.
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(256) };
    EnrollmentResponder responder { window, service, membership, self, identity, random, metrics, logger };
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

/// An `Enroll` frame, signed by the pair behind @p key as a joiner signs it.
/// @param id The identity claimed.
/// @param endpoint The endpoint claimed; empty for every live role.
/// @param role What it asks to be.
/// @param key The key it asks under: one of `KeyOf`'s.
/// @param nonce What it drew for the request; zeroes unless a case says otherwise.
/// @param challenge The leader's challenge it answers; none, as on a first ask, unless a case says otherwise.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> EnrollFrame(std::string_view id,
                                                 std::string_view endpoint,
                                                 Wire::EnrollRole role,
                                                 Ed25519PublicKey const& key,
                                                 std::array<std::byte, Wire::NodeChallengeBytes> const& nonce = {},
                                                 std::optional<Wire::EnrollChallenge> const& challenge = std::nullopt)
{
    auto const signature =
        Cluster::SignEnrollRequest(PairBehind(key),
                                   Cluster::EnrollRequestClaim { .nodeId = id,
                                                                 .nodeEndpoint = endpoint,
                                                                 .role = role,
                                                                 .publicKey = key,
                                                                 .nonce = nonce,
                                                                 .challenge = Wire::ChallengeBytes(challenge) });
    return Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = id,
                                                    .nodeEndpoint = endpoint,
                                                    .role = role,
                                                    .publicKey = key,
                                                    .nonce = nonce,
                                                    .challenge = Wire::ChallengeBytes(challenge),
                                                    .signature = signature });
}

/// An `Enroll` under a key no pair stands behind -- a small-order point, a non-canonical spelling --
/// so it carries an all-zero signature. What it tests is refused before any signature is read.
/// @param id The identity claimed.
/// @param key The key it asks under, as a LEARNER stating its own `0xFC` port.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> UnsignableJoinFrame(std::string_view id, Ed25519PublicKey const& key)
{
    auto const endpoint = std::format("{}:6674", id);
    auto const nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    auto const signature = std::array<std::byte, Wire::NodeSignatureBytes> {};
    return Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = id,
                                                    .nodeEndpoint = endpoint,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = key,
                                                    .nonce = nonce,
                                                    .challenge = {},
                                                    .signature = signature });
}

/// An `Enroll` from a joiner stating what its role states: its own `0xFC` port, on the host its id
/// names, exactly when the role's row says it states one -- the column the responder judges by.
/// @param id The identity it claims.
/// @param role The role it asks for.
/// @param key The key it asks under.
/// @param nonce The challenge it asks the approval be signed over.
/// @param challenge The leader's challenge it answers; none, as on a first ask, unless a case says otherwise.
/// @return The encoded request.
[[nodiscard]] std::vector<std::byte> JoinFrame(std::string_view id,
                                               Wire::EnrollRole role,
                                               Ed25519PublicKey const& key,
                                               std::array<std::byte, Wire::NodeChallengeBytes> const& nonce = {},
                                               std::optional<Wire::EnrollChallenge> const& challenge = std::nullopt)
{
    auto const endpoint = EnrollRoleRowFor(role).statesEndpoint ? std::format("{}:6674", id) : std::string {};
    return EnrollFrame(id, endpoint, role, key, nonce, challenge);
}

/// One `Enroll` from the joiner, in its role and under its own key.
/// @param seed The wired fixture.
/// @return The encoded reply.
[[nodiscard]] std::vector<std::byte> Enroll(Seed& seed)
{
    return AnswerNow(seed.responder, JoinFrame(JoinerId, JoinerRole, JoinerKey()), JoinerAddress);
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
    return AnswerNow(seed.responder, JoinFrame(id, Wire::EnrollRole::Learner, key), JoinerAddress);
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

TEST_CASE("A role that does not suit the endpoint is refused before it reaches the list", "[enrollment][responder]")
{
    Seed seed;

    // A learner states where its `0xFC` port answers, which the approval records as its member
    // endpoint. A learner stating none, or one that names no machine, is refused where it enters,
    // because the list is what a person reads and the record is what every resolver dials.
    CHECK(RefusalIn(AnswerNow(seed.responder, EnrollFrame("l", "", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    CHECK(RefusalIn(
              AnswerNow(seed.responder, EnrollFrame("l", ":6674", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == Wire::ErrorCode::MalformedFrame);
    // An endpoint that parses but sends whoever dials it back to ITSELF is no endpoint either: the
    // rule the record is held to (`IsPeerDialableEndpoint`), asked where the claim enters.
    for (auto const* const only: { "127.0.0.1:6674", "localhost:6674", "0.0.0.0:6674" })
    {
        INFO(only);
        CHECK(RefusalIn(
                  AnswerNow(seed.responder, EnrollFrame("l", only, Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
              == Wire::ErrorCode::MalformedFrame);
    }
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == 5);
    CHECK(seed.window.Summary().second == 0);

    // The control: the same learner stating a dialable endpoint is recorded, so the refusals above
    // are about the endpoint and not about the role.
    CHECK(RefusalIn(AnswerNow(
              seed.responder, EnrollFrame("l", "10.0.0.9:6674", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == std::nullopt);
    CHECK(seed.window.Summary().second == 1);
}

TEST_CASE("Approving a learner admits it as a learner at the endpoint it stated under the key it asked with",
          "[enrollment][responder][formation]")
{
    Seed seed;
    auto const laptopKey = KeyOf(0x6C);
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
    CHECK(proposed[0].schedulerEndpoint == "laptop:6674"); // what its `Enroll` stated
    CHECK(proposed[0].publicKey == std::optional { laptopKey });
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentApprovalsManual) == 1);

    // And the cluster RECORDS it so, which is what the joiner's next poll is answered from.
    auto const state = seed.cluster.ClusterState();
    auto const member = std::ranges::find(state.members, "laptop", &Cluster::ClusterMember::id);
    REQUIRE(member != state.members.end());
    CHECK(member->seat == Cluster::MemberSeat::Learner);
    CHECK(member->raftEndpoint.empty());
    CHECK(member->schedulerEndpoint == "laptop:6674"); // every resolver's answer for this machine
    auto const reply = LearnerRequest(seed, "laptop", laptopKey);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
}

TEST_CASE("An approval is signed over the joiner's own nonce by this node's key, for the cluster its summary states",
          "[enrollment][responder][formation]")
{
    // The roster is public, so this signature is the whole of what binds the answer to this fleet:
    // over the nonce the joiner drew, naming the joiner and its key, the cluster id the FLEET-SUMMARY
    // answer states -- the one a joiner proves this key for -- the outcome and the roster sent.
    Seed seed;
    auto const laptopKey = KeyOf(0x6C);
    auto const waiting = LearnerRequest(seed, "laptop", laptopKey);
    auto const pending = Wire::DecodeEnrollReply(PayloadOf(waiting));
    REQUIRE(pending.has_value());
    REQUIRE(Unwrap(pending).outcome == Wire::EnrollOutcome::Pending);
    // A "not yet" is signed too, as a "not yet": it keeps a joiner waiting, so it is the fleet's word
    // or none. `LearnerRequest` asks over a nonce of zeroes. It hands the row's challenge INSIDE what
    // is signed, so nobody between the ends can swap the one the joiner signs over next.
    auto const zeroes = std::array<std::byte, Wire::NodeChallengeBytes> {};
    REQUIRE(Unwrap(pending).challenge.has_value());
    CHECK(Unwrap(pending).challenge == seed.window.ChallengeFor("laptop"));
    auto const pendingClaim = Cluster::AdmissionClaim { .nonce = zeroes,
                                                        .joinerId = "laptop",
                                                        .joinerKey = laptopKey,
                                                        .clusterId = SeedClusterId,
                                                        .outcome = Wire::EnrollOutcome::Pending,
                                                        .roster = {},
                                                        .challenge = Wire::ChallengeBytes(Unwrap(pending).challenge) };
    CHECK(Cluster::VerifyAdmission(pendingClaim, Unwrap(pending).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Verified);
    auto swapped = pendingClaim;
    auto const otherChallenge = Wire::EnrollChallenge {};
    swapped.challenge = otherChallenge;
    CHECK(Cluster::VerifyAdmission(swapped, Unwrap(pending).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Forged);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop")) == std::nullopt);

    auto nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    nonce.fill(std::byte { 0x3C });
    auto const reply =
        AnswerNow(seed.responder, JoinFrame("laptop", Wire::EnrollRole::Learner, laptopKey, nonce), JoinerAddress);
    auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
    REQUIRE(decoded.has_value());
    REQUIRE(Unwrap(decoded).outcome == Wire::EnrollOutcome::Approved);
    REQUIRE(Unwrap(decoded).signature.has_value());
    CHECK(Unwrap(Unwrap(decoded).signature).publicKey == seed.identity.PublicKey());

    auto const claim = Cluster::AdmissionClaim { .nonce = nonce,
                                                 .joinerId = "laptop",
                                                 .joinerKey = laptopKey,
                                                 .clusterId = SeedClusterId,
                                                 .outcome = Wire::EnrollOutcome::Approved,
                                                 .roster = Unwrap(decoded).roster,
                                                 .challenge = {} };
    CHECK(Cluster::VerifyAdmission(claim, Unwrap(decoded).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Verified);

    // Over THIS request: the same answer held to another nonce, or to another cluster, is forged.
    auto otherNonce = nonce;
    otherNonce.fill(std::byte { 0x3D });
    auto replayed = claim;
    replayed.nonce = otherNonce;
    CHECK(Cluster::VerifyAdmission(replayed, Unwrap(decoded).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Forged);
    auto elsewhere = claim;
    elsewhere.clusterId = "c-other";
    CHECK(Cluster::VerifyAdmission(elsewhere, Unwrap(decoded).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Forged);
}

TEST_CASE("Approving an id the cluster already seats is refused by name and changes no seat",
          "[enrollment][responder][formation]")
{
    // A learner row under the VOTER's id, asked under another key: an approval of it must neither
    // demote the voter nor read as succeeding while it changes nothing (#1449).
    Seed seed;
    REQUIRE(RefusalIn(LearnerRequest(seed, LeaderId, KeyOf(0x5E))) == std::nullopt);
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
                                 .publicKey = KeyOf(0x6C) });
    seed.cluster.SetState(std::move(state));

    auto const reply = LearnerRequest(seed, "laptop", KeyOf(0x6C));
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(reply))).outcome == Wire::EnrollOutcome::Approved);
    CHECK_FALSE(seed.window.Find("laptop").has_value());

    // The control: the same id under ANOTHER key -- signed by that key's holder -- is recorded and
    // waits for a person.
    auto const nearMiss = KeyOf(0x6D);
    auto const other = LearnerRequest(seed, "laptop", nearMiss);
    CHECK(Unwrap(Wire::DecodeEnrollReply(PayloadOf(other))).outcome == Wire::EnrollOutcome::Pending);
    REQUIRE(seed.window.Find("laptop").has_value());
    CHECK(Unwrap(seed.window.Find("laptop")).publicKey == nearMiss);

    // And once that row exists it decides, whatever the cluster records: the recorded key asking
    // again is ANOTHER MACHINE to this row, counted in `claimsChanged` and answered `Pending`.
    auto const recordedAgain = LearnerRequest(seed, "laptop", KeyOf(0x6C));
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
    // the seed's base64url as a learner's id, and its raw bytes as a revoked key. Without
    // this, every check above passes under a scan that cannot see into the roster at all.
    auto planted = seed.cluster.ClusterState();
    planted.members.push_back(
        Cluster::ClusterMember { .id = Base64UrlEncode(secrets[0]),
                                 .raftEndpoint = {},
                                 .schedulerEndpoint = {},
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                 .seat = Cluster::MemberSeat::Learner,
                                 .publicKey = Filled(0x33) });
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

    auto const impostor = AnswerNow(seed.responder, JoinFrame(JoinerId, JoinerRole, KeyOf(0x99)), JoinerAddress);
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
    // Signed as a refusal, by this node's key, over the joiner's request: a refusal sends a joiner
    // away for an hour, so it is this fleet's word or none. `Enroll` asks over a nonce of zeroes.
    auto const zeroes = std::array<std::byte, Wire::NodeChallengeBytes> {};
    auto const refusal = Cluster::AdmissionClaim { .nonce = zeroes,
                                                   .joinerId = JoinerId,
                                                   .joinerKey = JoinerKey(),
                                                   .clusterId = SeedClusterId,
                                                   .outcome = Wire::EnrollOutcome::Rejected,
                                                   .roster = {},
                                                   .challenge = {} };
    CHECK(Cluster::VerifyAdmission(refusal, Unwrap(decoded).signature, seed.identity.PublicKey())
          == Cluster::AdmissionSignature::Verified);

    // Rejecting proposes NOTHING: a machine refused at the door must not appear in the
    // cluster's record under any reading.
    CHECK(seed.cluster.Proposed().empty());
    CHECK(std::ranges::none_of(seed.cluster.ClusterState().members,
                               [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
}

TEST_CASE("The pending list fills, refuses the next machine by name, and keeps the first", "[enrollment][responder]")
{
    Seed seed;

    // Each from a host of its own, so the list's bound is what is reached and not a host's.
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPendingEnrollments))
    {
        auto const id = std::format("crowd-{}", index);
        auto const host = std::format("203.0.113.{}", index);
        REQUIRE(RefusalIn(AnswerNow(seed.responder, JoinFrame(id, Wire::EnrollRole::Learner, KeyOf(0x66)), host))
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
                                             "10.0.0.9:6674",
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

    auto const accepted = LearnerRequest(seed, atBound, KeyOf(0x41));
    REQUIRE(RefusalIn(accepted) == std::nullopt);
    auto const reply = Wire::DecodeEnrollReply(PayloadOf(accepted));
    REQUIRE(reply.has_value()); // a default reply reads Pending, so an undecodable one must not pass as one
    CHECK(Unwrap(reply).outcome == Wire::EnrollOutcome::Pending);

    auto const refused = LearnerRequest(seed, pastBound, KeyOf(0x42));
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
    auto const laptopKey = KeyOf(0x6C);
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

    auto const reply = LearnerRequest(seed, "laptop", KeyOf(0x6C));
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
    // and a joiner pointed at a node that runs no consensus is the commonest mistake, so
    // that wrong sentence was the likeliest thing a healthy fleet would ever print. Pinned on both verbs, because they are
    // refused through different routes -- `RefusePeer` at the door and `Answer` for a frame that got past it -- and one
    // route fixed alone reads exactly like both.
    for (auto const op: { Wire::Op::Enroll, Wire::Op::EnrollControl })
    {
        auto const refused =
            merged.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } }, static_cast<std::uint8_t>(op));
        REQUIRE(refused.has_value());
        CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalIn(Unwrap(refused)) != Wire::UnimplementedVerb);

        CHECK(RefusalIn(AnswerNow(merged, JoinFrame(JoinerId, JoinerRole, JoinerKey()), JoinerAddress))
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

TEST_CASE("A node with no enrollment component tells a joiner the reason it knows, on both routes",
          "[enrollment][responder][merged][pin]")
{
    // A node pinned elsewhere, or a promoted voter its own pin does not name, RUNS consensus: told
    // *runs no consensus*, its joiner's operator goes looking in the wrong place. Each reason reaches
    // the joiner in its own words, and those words name no flag this node does not have.
    for (auto const absence: Enumerators<EnrollmentAbsence>())
    {
        INFO(static_cast<int>(absence));
        auto components = SurfaceComponents {};
        components.enrollmentAbsence = absence;
        MergedResponder merged { components };
        auto const atTheDoor = merged.RefusePeer(PeerIdentity { .host = std::string { JoinerAddress } },
                                                 static_cast<std::uint8_t>(Wire::Op::Enroll));
        REQUIRE(atTheDoor.has_value());
        CHECK(RefusalIn(Unwrap(atTheDoor)) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalSentenceIn(Unwrap(atTheDoor)) == EnrollmentAbsenceDetail(absence));

        auto const answered = AnswerNow(merged, JoinFrame(JoinerId, JoinerRole, JoinerKey()), JoinerAddress);
        CHECK(RefusalIn(answered) == Wire::ErrorCode::NoCluster);
        CHECK(RefusalSentenceIn(answered) == EnrollmentAbsenceDetail(absence));
        CHECK_FALSE(RefusalSentenceIn(answered).contains("--node-status"));
    }

    // WHICH words, for the two the pin produces: neither claims the node runs no consensus.
    for (auto const absence: { EnrollmentAbsence::PinnedToAnotherCluster, EnrollmentAbsence::NotAPinnedVoter })
    {
        INFO(static_cast<int>(absence));
        CHECK(EnrollmentAbsenceDetail(absence).contains("--fleet-id"));
        CHECK_FALSE(EnrollmentAbsenceDetail(absence).contains("runs no consensus"));
    }
    CHECK(EnrollmentAbsenceDetail(EnrollmentAbsence::NotAPinnedVoter).contains("add its own key"));
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
    EnrollmentResponder responder { seed.window,   seed.service, seed.membership, seed.self,
                                    seed.identity, seed.random,  seed.metrics,    logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        // An approval names the key the row holds, as the line `--enroll-list` prints does.
        if (verb == Wire::EnrollControlVerb::Approve)
            return AnswerNow(responder,
                             Wire::EncodeEnrollApprove(
                                 subject, seed.window.Find(subject).value_or(Wire::EnrollmentPendingEntry {}).publicKey),
                             OperatorAddress);
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(AnswerNow(responder, JoinFrame(JoinerId, JoinerRole, JoinerKey()), JoinerAddress)) == std::nullopt);
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
    CHECK(warned->message.contains("revokes that key"));

    // And the machine IS still there, so the warning is true rather than defensive.
    CHECK(std::ranges::any_of(seed.cluster.ClusterState().members,
                              [](Cluster::ClusterMember const& m) { return m.id == JoinerId; }));
}

TEST_CASE("A forgotten learner's key is revoked: its next enrollment is refused at the door, and never approved again",
          "[enrollment][responder][security][forget]")
{
    // #1555, end to end on the wire a joiner's key is presented on. The operator approves
    // laptop-a, then forgets it through the verb `--cluster-forget` reaches; the forget takes its
    // member record and revokes its key. The neuter is a forget that leaves the member where it
    // was, under which the next poll is answered `Approved`, roster and all -- still admitted.
    Seed seed;
    auto const laptopKey = KeyOf(0x57);
    auto const enroll = [&](std::string_view id) {
        return AnswerNow(seed.responder, JoinFrame(id, Wire::EnrollRole::Learner, laptopKey), JoinerAddress);
    };
    auto const recorded = [&seed](std::string_view id) {
        return std::ranges::any_of(seed.cluster.ClusterState().members,
                                   [id](Cluster::ClusterMember const& m) { return m.id == id; });
    };

    REQUIRE(RefusalIn(enroll("laptop-a")) == std::nullopt);
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop-a")) == std::nullopt);

    // Admitted, which is what makes the rest mean something: it is handed the roster.
    auto const admitted = enroll("laptop-a");
    REQUIRE(RefusalIn(admitted) == std::nullopt);
    REQUIRE(Unwrap(Wire::DecodeEnrollReply(PayloadOf(admitted))).outcome == Wire::EnrollOutcome::Approved);
    REQUIRE(recorded("laptop-a"));

    auto const forgot =
        seed.service.ClusterForget(Distributed::CallerContext { .membership = Distributed::Membership::Member,
                                                                .peerId = std::string { OperatorAddress } },
                                   "laptop-a");
    REQUIRE(forgot.status == Wire::Status::Ok);

    auto const state = seed.cluster.ClusterState();
    CHECK_FALSE(recorded("laptop-a"));
    CHECK(state.IsRevoked(laptopKey));
    CHECK(std::ranges::contains(state.revokedKeys, Cluster::RevokedKey { .id = "laptop-a", .publicKey = laptopKey }));

    // The next poll is REFUSED, by name and counted -- not `Pending`, which would keep a
    // removed machine polling for an answer that cannot come, and not `Approved`.
    auto const refused = enroll("laptop-a");
    CHECK(RefusalIn(refused) == Wire::ErrorCode::InvalidClusterChange);
    auto const message = Unwrap(Wire::DecodeErrorPayload(PayloadOf(refused))).second;
    CHECK(message.contains("never admitted again"));
    CHECK(message.contains(FormatEd25519PublicKey(laptopKey)));
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 1);

    // Under another id too: the key is what is refused, not the name it arrives with -- and
    // nothing is recorded for the operator to be shown.
    CHECK(RefusalIn(enroll("laptop-b")) == Wire::ErrorCode::InvalidClusterChange);
    CHECK_FALSE(seed.window.Find("laptop-b").has_value());
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 2);

    // And the row the window still holds cannot be approved back: rejected, then approved
    // again, the approval reaches the cluster and is refused there as a revoked key.
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Reject, "laptop-a")) == std::nullopt);
    auto const again = Control(seed, Wire::EnrollControlVerb::Approve, "laptop-a");
    CHECK(RefusalIn(again) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(Unwrap(Wire::DecodeErrorPayload(PayloadOf(again))).second.contains("a revoked key is never admitted again"));
    CHECK_FALSE(recorded("laptop-a"));
}

TEST_CASE("A request carrying a retired role byte is refused as malformed and nothing is recorded",
          "[enrollment][responder][formation]")
{
    // A machine joins ONE way, as a learner; 0x01 (member) and 0x02 (worker, a principal) are
    // retired, and the DECODER refuses them -- so the door answers MalformedFrame, counted, before
    // the list records a row an operator could approve into a record no build reads.
    Seed seed;
    for (auto const retired: Wire::RetiredEnrollRoles)
    {
        INFO("role byte " << static_cast<int>(retired));
        auto const reply = AnswerNow(seed.responder,
                                     EnrollFrame("w", "w.example:6674", static_cast<Wire::EnrollRole>(retired), JoinerKey()),
                                     JoinerAddress);
        CHECK(RefusalIn(reply) == Wire::ErrorCode::MalformedFrame);
        CHECK(Unwrap(Wire::DecodeErrorPayload(PayloadOf(reply))).second.contains("learner"));
    }
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == Wire::RetiredEnrollRoles.size());
    CHECK(seed.window.Summary().second == 0);
    CHECK(seed.cluster.Proposed().empty());

    // The control: the same request as a learner is recorded, so the refusal is the role's.
    CHECK(RefusalIn(AnswerNow(seed.responder, JoinFrame("w", Wire::EnrollRole::Learner, JoinerKey()), JoinerAddress))
          == std::nullopt);
    CHECK(seed.window.Summary().second == 1);
}

TEST_CASE("A joiner asking under a small-order or non-canonical key is refused at the door, by name",
          "[enrollment][responder][security][identity]")
{
    // A row an operator might approve is a key the cluster would then admit, and under a
    // small-order key the all-zero signature verifies every message -- so the approval would admit
    // whoever cares to claim it. Refused before the window records anything, on the malformed row:
    // no build of this software mints such a key. Asked as a LEARNER, the one role a machine enrolls
    // as -- the decoder refuses any other byte (`RetiredEnrollRoles`). The control is an
    // ordinary key asking the same way, which the window records.
    auto const nonCanonical = [] {
        auto key = Filled(0xFF);
        key.front() = std::byte { 0xF0 };
        key.back() = std::byte { 0x7F };
        return key;
    }();
    Seed seed;

    auto refusals = std::uint64_t { 0 };
    for (auto const& [key, fault]: { std::pair { Ed25519PublicKey {}, PublicKeyFault::SmallOrder },
                                     std::pair { nonCanonical, PublicKeyFault::NonCanonical } })
    {
        INFO("key " << FormatEd25519PublicKey(key));
        // Unsigned, since nothing can sign under such a key -- and refused as MALFORMED rather than
        // forged: the key's shape is judged before its signature.
        auto const refused = AnswerNow(seed.responder, UnsignableJoinFrame("joiner-x", key), JoinerAddress);
        CHECK(RefusalIn(refused) == Wire::ErrorCode::MalformedFrame);
        auto const message = Unwrap(Wire::DecodeErrorPayload(PayloadOf(refused))).second;
        CHECK(message.contains(DescribePublicKeyFault(fault)));
        CHECK(message.contains(FormatEd25519PublicKey(key)));
        CHECK_FALSE(seed.window.Find("joiner-x").has_value());
        CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed) == ++refusals);
    }

    auto const control =
        AnswerNow(seed.responder, JoinFrame("joiner-x", Wire::EnrollRole::Learner, KeyOf(0x59)), JoinerAddress);
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
    EnrollmentResponder responder { seed.window,   seed.service, seed.membership, seed.self,
                                    seed.identity, seed.random,  seed.metrics,    logger };

    auto const control = [&](Wire::EnrollControlVerb verb, std::string_view subject = {}) {
        // An approval names the key the row holds, as the line `--enroll-list` prints does.
        if (verb == Wire::EnrollControlVerb::Approve)
            return AnswerNow(responder,
                             Wire::EncodeEnrollApprove(
                                 subject, seed.window.Find(subject).value_or(Wire::EnrollmentPendingEntry {}).publicKey),
                             OperatorAddress);
        return AnswerNow(responder, Wire::EncodeEnrollControl(verb, subject), OperatorAddress);
    };

    REQUIRE(RefusalIn(AnswerNow(responder, JoinFrame(JoinerId, JoinerRole, JoinerKey()), JoinerAddress)) == std::nullopt);

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
                                    JoinFrame(std::format("junk-{}", index), Wire::EnrollRole::Learner, KeyOf(0x66)),
                                    Flooder))
                == std::nullopt);

    auto const refused = AnswerNow(seed.responder, JoinFrame("junk-more", Wire::EnrollRole::Learner, KeyOf(0x66)), Flooder);
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
                                    JoinFrame(std::format("junk-{}", index), Wire::EnrollRole::Learner, KeyOf(0x66)),
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
    auto const first = KeyOf(0x6A);
    auto const second = KeyOf(0x6B);
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

TEST_CASE("A surface built after its scheduler took a role answers the rows by that role at once",
          "[enrollment][responder][formation][conditions]")
{
    // The role arrives through the scheduler's observer, which only hears what is set AFTER it is
    // installed. In `main` the scheduler tier starts before this surface is built, so the role it
    // already holds is told at construction -- or a leader's rows would read not-evaluated until the
    // next election, and a follower's would never name the leader.
    for (auto const role: { Distributed::SchedulerRole::Leader, Distributed::SchedulerRole::Follower })
    {
        core::platform::ManualClock clock;
        core::platform::ManualWallClock wallClock;
        AtomicMetricsSink metrics;
        NullLogger logger;
        Distributed::KeyPairLeaseSigner const signer = Testing::TestLeaseSigner();
        Distributed::SchedulerService service { clock, wallClock, metrics, logger, signer, {} };
        service.SetRole(role,
                        role == Distributed::SchedulerRole::Leader ? std::string_view {} : LeaderEndpoint,
                        Distributed::StandaloneSchedulerTerm);
        ListedMembership membership { { std::string { OperatorAddress } },
                                      Distributed::MembershipParticipant::MachineTicket };
        NodeConditions conditions;
        EnrollmentWindow window { clock, &conditions, &metrics, wallClock };
        Testing::ScriptedSummarySource self { Wire::FleetSummary { .clusterId = std::string { SeedClusterId } } };
        Ed25519KeyPair const identity = Testing::TestKeyPair(std::string { LeaderId });
        Testing::ScriptedSecureRandom random;
        EnrollmentResponder const responder { window, service, membership, self, identity, random, metrics, logger };

        auto const expected =
            role == Distributed::SchedulerRole::Leader ? Wire::ConditionState::Clear : Wire::ConditionState::NotEvaluated;
        CHECK(conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == expected);
        CHECK(conditions.StateOf(NodeCondition::EnrollmentWindowOpen) == expected);
        if (role == Distributed::SchedulerRole::Follower)
            CHECK(Testing::DetailOf(conditions, NodeCondition::EnrollmentRequestsWaiting).contains(LeaderEndpoint));
    }
}

TEST_CASE("A demoted leader forgets its list, lowers the waiting condition and sends a poll to the new leader",
          "[enrollment][responder][formation]")
{
    Seed seed;
    REQUIRE(RefusalIn(Enroll(seed)) == std::nullopt);
    REQUIRE(seed.conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::Raised);

    seed.service.SetRole(Distributed::SchedulerRole::Follower, LeaderEndpoint, Distributed::StandaloneSchedulerTerm);
    // Not clear: the list lives in the leader's memory, and the row names where to ask -- through the
    // responder's wiring of the scheduler's role and the leader it names.
    CHECK(seed.conditions.StateOf(NodeCondition::EnrollmentRequestsWaiting) == Wire::ConditionState::NotEvaluated);
    CHECK(Testing::DetailOf(seed.conditions, NodeCondition::EnrollmentRequestsWaiting).contains(LeaderEndpoint));
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
                                 .publicKey = KeyOf(0x6C) });
    state.revokedKeys.push_back(Cluster::RevokedKey { .id = "laptop", .publicKey = KeyOf(0x6C) });
    seed.cluster.SetState(std::move(state));

    auto const reply = LearnerRequest(seed, "laptop", KeyOf(0x6C));
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
    auto const key = KeyOf(0x6C);
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
        // The second poll is the row's own joiner, answering the challenge the first was handed.
        auto const id = std::format("junk-{}", index);
        REQUIRE(RefusalIn(AnswerNow(seed.responder, JoinFrame(id, Wire::EnrollRole::Learner, KeyOf(0x66)), "198.51.100.7"))
                == std::nullopt);
        auto const again = JoinFrame(id, Wire::EnrollRole::Learner, KeyOf(0x66), {}, seed.window.ChallengeFor(id));
        REQUIRE(RefusalIn(AnswerNow(seed.responder, again, "2001:db8::7")) == std::nullopt);
    }
    auto const again =
        AnswerNow(seed.responder, JoinFrame("junk-more", Wire::EnrollRole::Learner, KeyOf(0x66)), "198.51.100.7");
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
                                    JoinFrame(std::format("m-{}", index), Wire::EnrollRole::Learner, KeyOf(0x66)),
                                    "198.51.100.9"))
                == std::nullopt);
    auto const refused =
        AnswerNow(seed.responder, JoinFrame("m-more", Wire::EnrollRole::Learner, KeyOf(0x66)), "::ffff:198.51.100.9");
    REQUIRE(RefusalIn(refused) == Wire::ErrorCode::EnrollmentHostFull);
    CHECK(RefusalSentenceIn(refused).starts_with("198.51.100.9 already has"));
    CHECK_FALSE(RefusalSentenceIn(refused).contains("::ffff:"));
}

TEST_CASE("An enrollment decision is refused a caller only --fleet-open admitted, by name and counted",
          "[enrollment][responder][admission][security]")
{
    // The window's open door is `Enroll`; the decision behind it is an operator's control verb. On
    // a --fleet-open node an anonymous caller is a member, and without the verb column it could
    // approve its own request or arm an auto-approve window. Asked of the production fold, so the
    // route that admitted each caller is the route production would name.
    Seed seed;
    Testing::OpenFleetFold fold;
    NullLogger logger;
    EnrollmentResponder responder { seed.window,   seed.service, fold.admitted, seed.self,
                                    seed.identity, seed.random,  seed.metrics,  logger };
    auto const control = static_cast<std::uint8_t>(Wire::Op::EnrollControl);
    auto const counted = [&seed] {
        return seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedIdentifiedCallerRequired);
    };

    // At the door, before a payload is read.
    auto const refused = responder.RefusePeer(Testing::OpenFleetFold::Anonymous(), control);
    REQUIRE(refused.has_value());
    CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::IdentifiedCallerRequired);
    CHECK(counted() == 1);
    // Not the stranger's row: the caller IS admitted, and that series is for one nothing admitted.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedNotAMember) == 0);

    // And after it, for a caller of `Answer` that never asked the door: every decision the verb
    // carries -- approve, arm auto-approve, the list -- is refused the same way, counted once each.
    auto const frames = std::array {
        Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, std::string { JoinerId }),
        Wire::EncodeEnrollControl(Wire::EnrollControlVerb::List, {}),
    };
    auto expected = std::uint64_t { 1 };
    for (auto const& frame: frames)
    {
        auto const reply = core::async::syncRun(responder.Answer(frame, Testing::OpenFleetFold::Anonymous())).bytes;
        CHECK(RefusalIn(reply) == Wire::ErrorCode::IdentifiedCallerRequired);
        CHECK(counted() == ++expected);
    }

    // What identifies a caller passes the door: a ticket, a proof, this machine.
    for (auto const& [what, facts]: { std::pair { "a ticket", Testing::OpenFleetFold::Ticketed() },
                                      std::pair { "a proof", Testing::OpenFleetFold::Proven() },
                                      std::pair { "loopback", Testing::OpenFleetFold::Local() } })
    {
        INFO(what);
        CHECK_FALSE(responder.RefusePeer(facts, control).has_value());
    }
    CHECK(counted() == expected);

    // And the open door stays open to the anonymous caller: `Enroll` is the one verb meant to admit it.
    CHECK_FALSE(
        responder.RefusePeer(Testing::OpenFleetFold::Anonymous(), static_cast<std::uint8_t>(Wire::Op::Enroll)).has_value());
}

TEST_CASE("An enrollment decision is refused a learner's ticket and a learner's proven key, by name and counted",
          "[enrollment][responder][admission][security][operator-standing]")
{
    // W-1: a machine ticket proves a fleet MACHINE, not an operator. Without the operator's standing
    // a process on any auto-approved laptop could approve, reject, or arm a 24 h auto-approve window
    // through its own node's ticket. Every decision the verb carries, read FROM THE TABLE.
    Seed seed;
    Testing::OpenFleetFold fold;
    NullLogger logger;
    EnrollmentResponder responder { seed.window,   seed.service, fold.admitted, seed.self,
                                    seed.identity, seed.random,  seed.metrics,  logger };
    auto const control = static_cast<std::uint8_t>(Wire::Op::EnrollControl);
    auto const counted = [&seed] {
        return seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedOperatorStandingRequired);
    };
    auto const frameFor = [](Wire::EnrollControlVerbRow const& row) {
        switch (row.subject)
        {
            case Wire::EnrollSubject::None:
                return Wire::EncodeEnrollControl(row.verb);
            case Wire::EnrollSubject::NodeId:
                return Wire::EncodeEnrollControl(row.verb, JoinerId);
            case Wire::EnrollSubject::Seconds:
                return Wire::EncodeEnrollControl(row.verb, "86400");
            case Wire::EnrollSubject::NodeIdAndKey:
                return Wire::EncodeEnrollApprove(JoinerId, JoinerKey());
        }
        throw std::logic_error { "every EnrollSubject has a frame" };
    };

    auto expected = std::uint64_t { 0 };
    for (auto const& [what, facts]: { std::pair { "a learner's ticket", Testing::OpenFleetFold::LearnerTicketed() },
                                      std::pair { "a learner's proven key", Testing::OpenFleetFold::LearnerProven() } })
    {
        INFO(what);
        // At the door, before a payload is read.
        auto const refused = responder.RefusePeer(facts, control);
        REQUIRE(refused.has_value());
        CHECK(RefusalIn(Unwrap(refused)) == Wire::ErrorCode::OperatorStandingRequired);
        CHECK(RefusalSentenceIn(Unwrap(refused)).contains("run it on a voter, or promote this machine"));
        CHECK(counted() == ++expected);
        // And after it, for every decision the verb carries.
        for (auto const& row: Wire::EnrollControlVerbTable)
        {
            INFO(row.name);
            auto const reply = core::async::syncRun(responder.Answer(frameFor(row), facts)).bytes;
            CHECK(RefusalIn(reply) == Wire::ErrorCode::OperatorStandingRequired);
            CHECK(counted() == ++expected);
        }
        // Enroll stays the open door: a learner still polls for itself.
        CHECK_FALSE(responder.RefusePeer(facts, static_cast<std::uint8_t>(Wire::Op::Enroll)).has_value());
    }
    // Not the anonymous caller's series: these callers were identified.
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedIdentifiedCallerRequired) == 0);

    // A voter's ticket, a voter's proof and this machine pass the door, and nothing more is counted.
    for (auto const& [what, facts]: { std::pair { "a voter's ticket", Testing::OpenFleetFold::Ticketed() },
                                      std::pair { "a voter's proven key", Testing::OpenFleetFold::Proven() },
                                      std::pair { "loopback", Testing::OpenFleetFold::Local() } })
    {
        INFO(what);
        CHECK_FALSE(responder.RefusePeer(facts, control).has_value());
    }
    CHECK(counted() == expected);
}

TEST_CASE("A stranger reaching enrollment control through Answer is told not-a-member, as at the door",
          "[enrollment][responder][admission]")
{
    // The identity column is asked only of a caller the surface ADMITS. A caller of `Answer` that
    // never asked the door is told what the door tells it -- `NotAMember`, on the not-a-member
    // series -- and the identified-caller series, which counts ADMITTED callers trying the
    // decision, does not move. Neutered to the old order (the identity column asked first), the
    // stranger is told identified-caller-required and that series moves.
    Seed seed;
    auto const stranger = PeerIdentity { .host = std::string { JoinerAddress } };
    for (auto const& frame: { Wire::EncodeEnrollControl(Wire::EnrollControlVerb::List, {}),
                              Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Approve, std::string { JoinerId }) })
    {
        auto const reply = core::async::syncRun(seed.responder.Answer(frame, stranger)).bytes;
        CHECK(RefusalIn(reply) == Wire::ErrorCode::NotAMember);
    }
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedNotAMember) == 2);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentControlRefusedIdentifiedCallerRequired) == 0);
}

TEST_CASE("An Enroll its key's holder did not sign is refused before it is listed or refreshes a row",
          "[enrollment][responder][enroll-signature]")
{
    // The id and the key are public -- a beacon carries the key, the roster both -- so a host that
    // polls under a joiner's pair with an endpoint of its own must not set what the record keeps.
    // Only the key's holder can sign; a request it did not sign is refused, and nothing is recorded.
    Seed seed;
    auto const laptopKey = KeyOf(0x6C);
    REQUIRE(RefusalIn(LearnerRequest(seed, "laptop", laptopKey)) == std::nullopt);
    REQUIRE(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop:6674");

    // A forger: the laptop's id and key, an endpoint of its own, signed by the only key it holds.
    auto const forged = [&laptopKey](std::string_view id, std::string_view endpoint) {
        auto const nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
        auto const signature = Cluster::SignEnrollRequest(PairBehind(KeyOf(0x99)),
                                                          Cluster::EnrollRequestClaim { .nodeId = id,
                                                                                        .nodeEndpoint = endpoint,
                                                                                        .role = Wire::EnrollRole::Learner,
                                                                                        .publicKey = laptopKey,
                                                                                        .nonce = nonce,
                                                                                        .challenge = {} });
        return Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = id,
                                                        .nodeEndpoint = endpoint,
                                                        .role = Wire::EnrollRole::Learner,
                                                        .publicKey = laptopKey,
                                                        .nonce = nonce,
                                                        .challenge = {},
                                                        .signature = signature });
    };
    CHECK(RefusalIn(AnswerNow(seed.responder, forged("laptop", "attacker:6674"), "203.0.113.9"))
          == Wire::ErrorCode::NodeProofRejected);
    CHECK(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop:6674"); // the row did not move
    CHECK(Unwrap(seed.window.Find("laptop")).attempts == 1);                 // nor was it even counted

    // A genuine signature over a request somebody changed on the way verifies over nothing.
    auto altered = EnrollFrame("laptop", "laptop:6674", Wire::EnrollRole::Learner, laptopKey);
    auto const at = std::ranges::search(altered, Wire::AsBytes("laptop:6674"));
    REQUIRE_FALSE(at.empty());
    at.front() = std::byte { 'L' };
    CHECK(RefusalIn(AnswerNow(seed.responder, altered, "203.0.113.9")) == Wire::ErrorCode::NodeProofRejected);

    // A stranger's row is never created from a forgery either.
    CHECK(RefusalIn(AnswerNow(seed.responder, forged("desk", "desk:6674"), "203.0.113.9"))
          == Wire::ErrorCode::NodeProofRejected);
    CHECK_FALSE(seed.window.Find("desk").has_value());

    // The holder's own signature replayed under another nonce verifies over nothing: the nonce is
    // one of the fields it covers.
    auto drawn = std::array<std::byte, Wire::NodeChallengeBytes> {};
    drawn.fill(std::byte { 0x11 });
    auto replayed = EnrollFrame("laptop", "laptop:6674", Wire::EnrollRole::Learner, laptopKey, drawn);
    auto const nonceAt = std::ranges::search(replayed, drawn);
    REQUIRE_FALSE(nonceAt.empty());
    nonceAt.front() = std::byte { 0x12 };
    CHECK(RefusalIn(AnswerNow(seed.responder, replayed, "203.0.113.9")) == Wire::ErrorCode::NodeProofRejected);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedForged) == 4);
    CHECK(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop:6674");

    // The control: the HOLDER refreshing its endpoint, signed over the row's current challenge, is
    // accepted and moves the row -- the refresh is still what keeps a pending row current, only now it
    // is the holder's alone.
    REQUIRE(RefusalIn(AnswerNow(seed.responder,
                                EnrollFrame("laptop",
                                            "laptop.corp.example:6674",
                                            Wire::EnrollRole::Learner,
                                            laptopKey,
                                            {},
                                            seed.window.ChallengeFor("laptop")),
                                JoinerAddress))
            == std::nullopt);
    CHECK(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop.corp.example:6674");

    // So what the approval records is the endpoint the laptop itself last stated.
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop")) == std::nullopt);
    REQUIRE(seed.cluster.Proposed().size() == 1);
    CHECK(seed.cluster.Proposed()[0].schedulerEndpoint == "laptop.corp.example:6674");
}

TEST_CASE("A genuine Enroll replayed after its row moved on rolls nothing back", "[enrollment][responder][enroll-signature]")
{
    // The holder's own signature, verbatim, with nothing changed: it verifies, and before the leader
    // issued challenges it refreshed the row to whatever endpoint the joiner stated back then. Each
    // `Pending` answer hands the row's challenge out, signed, and only a request over the challenge the
    // row holds NOW refreshes it -- so a recording is worth nothing once the joiner has asked again.
    Seed seed;
    auto const laptopKey = KeyOf(0x6C);
    auto const ask = [&](std::string_view endpoint, std::optional<Wire::EnrollChallenge> const& challenge) {
        return EnrollFrame("laptop", endpoint, Wire::EnrollRole::Learner, laptopKey, {}, challenge);
    };
    auto const handed = [](std::span<std::byte const> reply) {
        auto const decoded = Wire::DecodeEnrollReply(PayloadOf(reply));
        REQUIRE(decoded.has_value());
        REQUIRE(Unwrap(decoded).outcome == Wire::EnrollOutcome::Pending);
        return Unwrap(decoded).challenge;
    };

    auto const first = handed(AnswerNow(seed.responder, ask("laptop:6674", std::nullopt), JoinerAddress));
    REQUIRE(first.has_value());
    auto const recorded = ask("laptop.old.example:6674", first); // what somebody on the way keeps
    auto const second = handed(AnswerNow(seed.responder, recorded, JoinerAddress));
    REQUIRE(second.has_value());
    CHECK(second != first); // the refresh replaced the challenge it answered
    auto const third = handed(AnswerNow(seed.responder, ask("laptop.new.example:6674", second), JoinerAddress));
    REQUIRE(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop.new.example:6674");

    // The recording, verbatim: still a genuine request, so answered and never counted forged -- and the
    // row keeps the endpoint the joiner states now, and the challenge it holds now.
    auto const replayed = handed(AnswerNow(seed.responder, recorded, "203.0.113.9"));
    CHECK(replayed == third);
    CHECK(Unwrap(seed.window.Find("laptop")).nodeEndpoint == "laptop.new.example:6674");
    CHECK(Unwrap(seed.window.Find("laptop")).claimsChanged == 1);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedForged) == 0);

    // So what an approval records is what the laptop itself said LAST.
    REQUIRE(RefusalIn(Control(seed, Wire::EnrollControlVerb::Approve, "laptop")) == std::nullopt);
    REQUIRE(seed.cluster.Proposed().size() == 1);
    CHECK(seed.cluster.Proposed()[0].schedulerEndpoint == "laptop.new.example:6674");
}

TEST_CASE("A leader that cannot draw a challenge records nothing and says why", "[enrollment][responder][security]")
{
    // A challenge drawn from anywhere weaker is the replay a challenge exists to stop, so a failed draw
    // is a refusal -- `NoCluster`, uncounted, as `NodeProofResponder`'s failed nonce is.
    Seed seed;
    Testing::ScriptedSecureRandom denied { Testing::ScriptedSecureRandom::DeniedFailure() };
    EnrollmentResponder responder { seed.window,   seed.service, seed.membership, seed.self,
                                    seed.identity, denied,       seed.metrics,    seed.logger };
    auto const reply = AnswerNow(responder, JoinFrame("laptop", Wire::EnrollRole::Learner, KeyOf(0x6C)), JoinerAddress);
    CHECK(RefusalIn(reply) == Wire::ErrorCode::NoCluster);
    CHECK(RefusalSentenceIn(reply).contains("scripted-getrandom"));
    CHECK_FALSE(seed.window.Find("laptop").has_value());
    CHECK(denied.FillCount() == 1);
}

TEST_CASE("A forged Enroll under a revoked key is told it is forged, not that the key is revoked",
          "[enrollment][responder][enroll-signature][forget]")
{
    // The signature is checked before any other claim is reported on: a request nobody holding the key
    // made learns nothing about that key here, its revocation included.
    Seed seed;
    auto const revoked = KeyOf(0x6C);
    auto state = seed.cluster.ClusterState();
    state.revokedKeys.push_back(Cluster::RevokedKey { .id = "laptop", .publicKey = revoked });
    seed.cluster.SetState(std::move(state));

    auto const nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    auto const unsignedBytes = std::array<std::byte, Wire::NodeSignatureBytes> {};
    auto const frame = Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = "laptop",
                                                                .nodeEndpoint = "laptop:6674",
                                                                .role = Wire::EnrollRole::Learner,
                                                                .publicKey = revoked,
                                                                .nonce = nonce,
                                                                .challenge = {},
                                                                .signature = unsignedBytes });
    CHECK(RefusalIn(AnswerNow(seed.responder, frame, JoinerAddress)) == Wire::ErrorCode::NodeProofRejected);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 0);

    // The control: the holder asking is told the key is revoked.
    CHECK(RefusalIn(LearnerRequest(seed, "laptop", revoked)) == Wire::ErrorCode::InvalidClusterChange);
    CHECK(seed.metrics.Read(IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey) == 1);
}
