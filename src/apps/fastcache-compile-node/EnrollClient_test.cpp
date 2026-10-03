// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "EnrollClient.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CacheProtocol.hpp>
#include <tests/NodeFlagNames.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/NodeKeyFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reply the seed sent, as the exchange layer hands it over.
/// @param outcome What the leader decided.
/// @param roster The roster it sent with it.
/// @return The outcome a client would read.
[[nodiscard]] Cc::CacheOutcome Served(Wire::EnrollOutcome outcome, std::span<std::byte const> roster = {})
{
    return Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Hit,
                              .value = Wire::EncodeEnrollReply(outcome, roster),
                              .message = {},
                              .credentialIgnored = false,
                              .transportFailure = Cc::TransportFailure::None };
}

/// A refusal the seed sent.
/// @param code Why.
/// @param message Its own words.
/// @return The outcome a client would read.
[[nodiscard]] Cc::CacheOutcome Refused(Wire::ErrorCode code, std::string message = {})
{
    return Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Rejected,
                              .value = {},
                              .code = code,
                              .message = std::move(message),
                              .credentialIgnored = false,
                              .transportFailure = Cc::TransportFailure::None };
}

/// A 32-byte key whose every byte is @p fill.
/// @param fill The byte.
/// @return The key.
[[nodiscard]] Ed25519PublicKey KeyOf(std::uint8_t fill)
{
    Ed25519PublicKey key {};
    key.fill(static_cast<std::byte>(fill));
    return key;
}

/// A member record, every field named.
/// @param id Its id.
/// @param endpoint Its consensus endpoint.
/// @param key Its key.
/// @return The member.
[[nodiscard]] Cluster::ClusterMember Member(std::string_view id, std::string_view endpoint, Ed25519PublicKey const& key)
{
    return Cluster::ClusterMember { .id = std::string { id },
                                    .raftEndpoint = std::string { endpoint },
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = Cluster::MemberSeat::Voter,
                                    .publicKey = key };
}

/// The roster a two-member cluster hands a joiner: the leader, and @p joiner.
/// @param joiner The joiner's record as the leader holds it.
/// @return The encoded roster.
[[nodiscard]] std::vector<std::byte> RosterWith(Cluster::ClusterMember joiner)
{
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    state.members.push_back(std::move(joiner));
    return Cluster::EncodeRoster(Cluster::ProjectRoster(state));
}

} // namespace

TEST_CASE("A pending reply is a wait, and an approved one carries the roster", "[enrollment][client]")
{
    auto const waiting = ReadEnrollReply(Served(Wire::EnrollOutcome::Pending));
    CHECK(waiting.progress == EnrollProgress::Waiting);
    CHECK(waiting.roster.empty());

    auto const roster = RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42)));
    auto const admitted = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved, roster));
    CHECK(admitted.progress == EnrollProgress::Admitted);
    CHECK(admitted.roster == roster);

    // A person said no. Distinguished from every WAIT above because asking again will
    // not change it, and a client that read it as a wait would poll until its bound ran
    // out and then report a timeout for a decision that was already taken.
    auto const refused = ReadEnrollReply(Served(Wire::EnrollOutcome::Rejected));
    CHECK(refused.progress == EnrollProgress::Refused);
}

TEST_CASE("An approval carrying no roster is a fault rather than an admission", "[enrollment][client]")
{
    // **The client's half of the assertion the server side also makes.** A seed that
    // approved and sent nothing would have this node report an admission it can check
    // nothing about -- and the outcome byte is correct in that build, so the LENGTH is the
    // only thing that separates them.
    auto const reading = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved));
    CHECK(reading.progress == EnrollProgress::Fatal);
    CHECK(reading.detail.contains("no roster"));
}

TEST_CASE("A full list or no leader is a wait and a seed too old to know the verb is not", "[enrollment][client]")
{
    // Two refusals a client answers by asking again: one says *not yet* and the other
    // says *ask in a moment, somebody is being elected*. Collapsing either into the
    // fatal arm would make a joiner give up on a fleet that was about to admit it.
    for (auto const code:
         { Wire::ErrorCode::EnrollmentFull, Wire::ErrorCode::EnrollmentHostFull, Wire::ErrorCode::NotLeader })
        CHECK(ReadEnrollReply(Refused(code)).progress == EnrollProgress::Closed);

    // And the one that says the SEED is the problem rather than this machine or this
    // moment. A build that does not implement the verb will not start implementing it
    // while this loop polls, so stopping is the only useful answer -- and the message
    // says which machine to upgrade.
    auto const old = ReadEnrollReply(Refused(Wire::ErrorCode::UnknownOpcode));
    CHECK(old.progress == EnrollProgress::Fatal);
    CHECK(old.detail.contains("older"));
}

TEST_CASE("A seed that runs no consensus is told apart from a seed that is too old", "[enrollment][client]")
{
    // **The commonest operator mistake, and it used to produce a confident wrong
    // diagnosis.** The documented flow points `--enroll-from` at ANY member, and most
    // members run no consensus -- so the node with nothing to join is what a healthy
    // fleet hands you most often. It answered `UnimplementedVerb`, which arrives as
    // `UnknownOpcode`, and this client reported *the seed is running a build older than
    // this one*: somebody is sent to upgrade a node that is already current, and the
    // real remedy -- a different ADDRESS -- is never mentioned.
    auto const noCluster = ReadEnrollReply(Refused(Wire::ErrorCode::NoCluster));

    // Fatal rather than a wait: a node that runs no consensus will not start running it
    // while this loop polls, so retrying for ten minutes helps nobody.
    CHECK(noCluster.progress == EnrollProgress::Fatal);

    // **Assert what DISTINGUISHES.** Both readings are `Fatal` and both carry a
    // sentence, so a case checking only the progress passes under the defect this is
    // named for. What separates them is which machine the operator is sent to look at,
    // so the text is the assertion: this one must NOT blame the seed's build, and must
    // name the remedy.
    CHECK(!noCluster.detail.contains("older"));
    CHECK(noCluster.detail.contains("consensus"));

    // And the two codes really do reach different sentences, which is the property one
    // arm alone cannot show -- a build that folded them would pass every check above
    // for whichever text it kept.
    CHECK(noCluster.detail != ReadEnrollReply(Refused(Wire::ErrorCode::UnknownOpcode)).detail);
}

TEST_CASE("A NotLeader naming an endpoint is followed, and one naming a sentence is not", "[enrollment][client]")
{
    // Judged by PARSING the message rather than by testing it for empty: an empty one is
    // replaced by the error table's default sentence, so *no leader known* and *the
    // leader is at h:p* arrive the same shape.
    auto const redirect = ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader, "10.0.0.1:7000"));
    CHECK(redirect.progress == EnrollProgress::Redirect);
    CHECK(redirect.detail == "10.0.0.1:7000");

    // `SplitHostPort` takes the LAST colon, so a sentence splits into a host and a port
    // of " try again" -- and this client DIALS what it is given. A build that split
    // rather than parsed would dial that.
    CHECK(ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader, "no leader: try again")).progress == EnrollProgress::Closed);
    CHECK(ReadEnrollReply(Refused(Wire::ErrorCode::NotLeader)).progress == EnrollProgress::Closed);
}

TEST_CASE("A seed that could not be reached, or answered a body this build cannot read, is fatal", "[enrollment][client]")
{
    auto const unreachable = Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Transport,
                                                .value = {},
                                                .message = {},
                                                .credentialIgnored = false,
                                                .transportFailure = Cc::TransportFailure::Unreached };
    CHECK(ReadEnrollReply(unreachable).progress == EnrollProgress::Fatal);

    auto const garbage = Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Hit,
                                            .value = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 } },
                                            .message = {},
                                            .credentialIgnored = false,
                                            .transportFailure = Cc::TransportFailure::None };
    CHECK(ReadEnrollReply(garbage).progress == EnrollProgress::Fatal);
}

TEST_CASE("What a joiner claims is derived from the configuration it will actually run as", "[enrollment][client]")
{
    auto cfg = Testing::FirstStart(NodeConfig {});

    // A node that names no consensus member of its own has nothing to be admitted AS,
    // and the refusal says which two flags supply the halves rather than reporting an
    // empty string.
    auto const nothing = EnrollClaim(cfg);
    REQUIRE(!nothing.has_value());
    CHECK(nothing.error().contains("--listen-raft"));
    CHECK(nothing.error().contains("--raft-self"));

    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";
    ApplyNodeIdentity(cfg,
                      NodeIdentity { .id = "joiner-a", .origin = NodeIdentityOrigin::Minted, .publicKey = std::nullopt });

    auto const claim = EnrollClaim(cfg);
    REQUIRE(claim.has_value());

    // BOTH halves, and both taken from `ClusterSelfMember`: an id with no address is a
    // member the cluster counts and cannot reach, and an address derived a second way
    // here could disagree with the one consensus will run under.
    CHECK(claim->first == "joiner-a");
    CHECK(claim->second == "198.51.100.4:7100");
}

TEST_CASE("An admission says what the seed recorded and where this machine wrote it, and names no step it lacks",
          "[enrollment][client][learner][formation]")
{
    // What is TRUE on this build: the seed recorded the key, and this machine kept the identity it
    // asked under -- and, for a member, nothing else. It names no step the binary does not have:
    // every `--flag` it prints must parse, which is how a `--raft-join` instruction outlived the
    // flag it named.
    auto const where = std::filesystem::path { "state" } / "node-a";
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    auto laptop = Member("joiner-a", "", KeyOf(0x42));
    laptop.seat = Cluster::MemberSeat::Learner;
    state.members.push_back(laptop);
    state.principals.push_back(
        Cluster::ClusterPrincipal { .id = "worker-a", .publicKey = KeyOf(0x77), .role = Cluster::PrincipalRole::Worker });
    auto const roster = Cluster::EncodeRoster(Cluster::ProjectRoster(state));

    auto const member = DescribeAdmission(
        JoinerIdentity {
            .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42) },
        roster,
        where);
    REQUIRE(member.has_value());
    CHECK(member->contains("as a learner"));
    CHECK(member->contains(where.string()));
    CHECK(member->contains("nothing else"));
    CHECK(Testing::FlagsNoNodeRowAccepts(*member).empty());

    auto const worker = DescribeAdmission(
        JoinerIdentity {
            .nodeId = "worker-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Worker, .publicKey = KeyOf(0x77) },
        roster,
        where);
    REQUIRE(worker.has_value());
    CHECK(worker->contains("as a worker"));
    CHECK(worker->contains(where.string()));
    CHECK(worker->contains("joins no consensus"));
    CHECK(Testing::FlagsNoNodeRowAccepts(*worker).empty());
}

TEST_CASE("An admission is believed only when the roster records this machine under its own key",
          "[enrollment][client][security]")
{
    auto const self = JoinerIdentity {
        .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42)
    };

    // The honest roster: the joiner is told what to compare and what was recorded.
    auto const roster = RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42)));
    auto const admitted = DescribeAdmission(self, roster, "state");
    REQUIRE(admitted.has_value());
    CHECK(admitted->contains(Cluster::RenderRosterFingerprint(Cluster::DigestOfRoster(roster))));
    CHECK(admitted->contains("The seed recorded"));

    // **The refusal, and it is the one that makes the roster worth checking.** The same id
    // under ANOTHER key is either an operator who approved the wrong row or a reply somebody
    // rewrote -- and in both, every key the roster names is untrustworthy, so nothing it
    // says is printed as advice.
    auto const swapped = DescribeAdmission(self, RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x99))), "state");
    REQUIRE_FALSE(swapped.has_value());
    CHECK(swapped.error().contains("does not record"));
    CHECK(swapped.error().contains(FormatEd25519PublicKey(KeyOf(0x42))));
    CHECK_FALSE(swapped.error().contains("The seed recorded"));

    // And bytes that are not a roster are refused as that, never as an admission.
    auto const garbage = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 } };
    CHECK_FALSE(DescribeAdmission(self, garbage, "state").has_value());
}

TEST_CASE("A worker is admitted as a principal, and a member row does not stand in for one", "[enrollment][client]")
{
    auto const self = JoinerIdentity {
        .nodeId = "worker-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Worker, .publicKey = KeyOf(0x77)
    };

    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    state.principals.push_back(
        Cluster::ClusterPrincipal { .id = "worker-a", .publicKey = KeyOf(0x77), .role = Cluster::PrincipalRole::Worker });
    auto const admitted = DescribeAdmission(self, Cluster::EncodeRoster(Cluster::ProjectRoster(state)), "state");
    REQUIRE(admitted.has_value());
    CHECK(admitted->contains("worker"));
    CHECK(admitted->contains("joins no consensus"));

    // The same id and key as a MEMBER is not what this machine asked to be: a worker counted
    // towards quorum is a vote nobody can collect.
    CHECK_FALSE(DescribeAdmission(self, RosterWith(Member("worker-a", "10.0.0.9:7100", KeyOf(0x77))), "state").has_value());
}

TEST_CASE("An admission asks the joiner to place nothing by hand", "[enrollment][client]")
{
    // #178 PR 4 left a member joiner needing `--cluster-key-file` placed by hand, and said so at
    // this moment; PR 6 retired the key, so the admission is the whole of joining. A sentence
    // still naming the flag would send an operator after a file nothing reads.
    auto const member = JoinerIdentity {
        .nodeId = "joiner-a", .nodeEndpoint = {}, .role = Wire::EnrollRole::Learner, .publicKey = KeyOf(0x42)
    };
    auto const admitted =
        DescribeAdmission(member, RosterWith(Member("joiner-a", "198.51.100.4:7100", KeyOf(0x42))), "state");
    REQUIRE(admitted.has_value());
    CHECK_FALSE(admitted->contains("--cluster-key-file"));
    // The control: the admission is really there, so the absence above is not an empty string's.
    CHECK(admitted->contains("The seed recorded"));
}

namespace
{

/// A wall-clock instant the certificate cases are judged at.
constexpr auto EnrolledAt = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// The roster a worker compares when it is admitted: voters @p voters, each under its test key,
/// and the worker itself as a principal.
/// @param voters The voters.
/// @return The roster.
[[nodiscard]] Cluster::Roster EnrollmentRoster(std::vector<std::string> const& voters)
{
    auto state = Cluster::ClusterState {};
    for (auto const& id: voters)
        state.members.push_back(Member(id, id + ":6680", Testing::TestKeyPair(id).PublicKey()));
    state.principals.push_back(
        Cluster::ClusterPrincipal { .id = "worker-a", .publicKey = KeyOf(0x77), .role = Cluster::PrincipalRole::Worker });
    return Cluster::ProjectRoster(state);
}

/// @p roster at @p version, endorsed by @p endorsers until an hour after `EnrolledAt`.
/// @param roster The roster.
/// @param version Its version.
/// @param endorsers Who signs it, each with its test key.
/// @return The certificate's bytes.
[[nodiscard]] std::vector<std::byte> CertificateOf(Cluster::Roster const& roster,
                                                   std::uint64_t version,
                                                   std::vector<std::string> const& endorsers)
{
    auto certified = Cluster::CertifiedRoster {
        .clusterId = "fleet", .version = version, .roster = Cluster::EncodeRoster(roster), .endorsements = {}
    };
    for (auto const& endorser: endorsers)
    {
        auto const key = Testing::TestKeyPair(endorser);
        certified.endorsements.push_back(
            Cluster::SignEndorsement(Cluster::RosterEndorsement { .clusterId = "fleet",
                                                                  .version = version,
                                                                  .rosterDigest = Cluster::DigestOfRoster(certified.roster),
                                                                  .notAfter = EnrolledAt + std::chrono::hours { 1 },
                                                                  .endorser = endorser,
                                                                  .signature = {} },
                                     [&key](LabelledMessage const& message) { return SignLabelled(key, message); }));
    }
    return Cluster::EncodeCertifiedRoster(certified);
}

} // namespace

TEST_CASE("An enrolled worker keeps the leader's certified roster, certified against the roster it compared",
          "[enrollment][client][roster]")
{
    // #178 PR 6: the worker enrollment admits is handed the cluster's certified roster and keeps
    // it as its trust root, so it checks grants from its first start with no `--voter-key`. The
    // certificate is judged by the voters of the roster the OPERATOR compared -- never taken on
    // the leader's word, which is the one thing a relayed reply could change.
    Testing::ScratchDirectory const scratch { "enroll-keep-roster" };
    auto const path = scratch.Path() / Distributed::RosterFileName;
    Distributed::FileRosterStore store { path };

    auto const compared = Cluster::EncodeRoster(EnrollmentRoster({ "n1", "n2", "n3" }));
    auto const certificate = CertificateOf(EnrollmentRoster({ "n1", "n2", "n3" }), 5, { "n1", "n2" });

    auto const said = KeepEnrolledRoster(compared, certificate, EnrolledAt, store);
    CHECK(said.contains("version 5"));
    CHECK(said.contains(Distributed::RosterFileName));
    CHECK(Testing::FlagsNoNodeRowAccepts(said).empty());

    auto const kept = Distributed::LoadPersistedRoster(path);
    REQUIRE(kept.has_value());
    REQUIRE(Unwrap(kept).has_value());
    CHECK(Unwrap(Unwrap(kept)).certificate == Unwrap(Cluster::DecodeCertifiedRoster(certificate)));
}

TEST_CASE("A certificate the compared roster's voters did not endorse is not kept", "[enrollment][client][roster]")
{
    // The refusals, each against the SAME compared roster, so what differs is the certificate.
    Testing::ScratchDirectory const scratch { "enroll-refuse-roster" };
    auto const path = scratch.Path() / Distributed::RosterFileName;
    Distributed::FileRosterStore store { path };
    auto const compared = Cluster::EncodeRoster(EnrollmentRoster({ "n1", "n2", "n3" }));

    SECTION("one voter of three is no majority")
    {
        auto const said = KeepEnrolledRoster(
            compared, CertificateOf(EnrollmentRoster({ "n1", "n2", "n3" }), 5, { "n1" }), EnrolledAt, store);
        CHECK(said.contains("was not kept"));
    }

    SECTION("machines that are not the compared roster's voters")
    {
        // A relayed reply could carry a certificate its sender's own friends signed; the voters
        // the operator compared never did.
        auto const said = KeepEnrolledRoster(
            compared, CertificateOf(EnrollmentRoster({ "x1", "x2", "x3" }), 5, { "x1", "x2" }), EnrolledAt, store);
        CHECK(said.contains("was not kept"));
    }

    SECTION("no certificate at all")
    {
        auto const said = KeepEnrolledRoster(compared, {}, EnrolledAt, store);
        CHECK(said.contains("holds no certified roster"));
    }

    SECTION("bytes that are not a certificate")
    {
        auto const garbage = std::vector<std::byte> { std::byte { 0x01 }, std::byte { 0x02 } };
        auto const said = KeepEnrolledRoster(compared, garbage, EnrolledAt, store);
        CHECK(said.contains("could not be read"));
    }

    // Nothing was written in any of them: a worker that kept a roster its operator's voters never
    // endorsed would check every grant against a trust root nobody chose.
    auto const kept = Distributed::LoadPersistedRoster(path);
    REQUIRE(kept.has_value());
    CHECK_FALSE(Unwrap(kept).has_value());
}

TEST_CASE("A seed address that is not an address to dial is refused before any state is written", "[enrollment][client]")
{
    // **A reachability fix, not a second opinion.** `--enroll-from` has a
    // `StartupPolicyRejection` row of its own, and on this path that row cannot fire:
    // `main` dispatches `--enroll-from` and RETURNS before the startup table is
    // consulted, so it was reachable only through `--print-surfaces`. Left to the dial,
    // a bare host was reported as *cannot reach the seed* -- the wrong problem -- and
    // reported it only AFTER this node had minted its identity into `--cluster-dir`, so
    // a typo wrote durable state.
    Testing::ScratchDirectory scratch { "enroll-shape" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.clusterDir = scratch.Path();
    cfg.enrollFrom = "10.0.0.1";

    SystemSecureRandom random;
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto const refused = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random, keyGuard);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().contains("--enroll-from"));

    // **The half that distinguishes.** Every refusal on this path produces an error
    // string, so matching one proves nothing about WHEN it fired. What separates a
    // refusal taken at the door from the old one taken after the dial is that no state
    // exists: an identity minted into `--cluster-dir` is the durable thing a typo used
    // to leave behind, and it is the reason this check had to move rather than merely
    // being reworded.
    CHECK(std::filesystem::is_empty(scratch.Path()));
}

TEST_CASE("The pending list marks a row whose two addresses disagree, and leaves an honest one alone",
          "[enrollment][client][security]")
{
    // **The gate is a person reading this text, so the text is the gate.** Every other
    // case on this branch asserts what the node DECIDED; this one asserts what the
    // operator is shown before they decide, which is the only place the mismatch and
    // drift marks exist at all.
    //
    // BOTH directions, and that is the whole case rather than thoroughness: a renderer
    // that marked every row would pass a case that only checks the marked one, and it
    // would make the mark worthless at exactly forty rows -- which is the size at which
    // an operator stops reading and starts scanning for the marker.
    Wire::EnrollmentReport const report {
        .state = Wire::WireEnrollmentState::AutoApprove,
        .autoApproveSecondsLeft = 42 * 60,
        .pending = { Wire::EnrollmentPendingEntry { .nodeId = "honest",
                                                    .nodeEndpoint = "10.0.0.9:6674",
                                                    .peerId = "10.0.0.9",
                                                    .firstSeenSecondsAgo = 3,
                                                    .attempts = 2,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x11),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.9" },
                     Wire::EnrollmentPendingEntry { .nodeId = "elsewhere",
                                                    .nodeEndpoint = "10.0.0.9:6674",
                                                    .peerId = "203.0.113.7",
                                                    .firstSeenSecondsAgo = 9,
                                                    .attempts = 5,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x22),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "203.0.113.7" },
                     Wire::EnrollmentPendingEntry { .nodeId = "drifted",
                                                    .nodeEndpoint = "10.0.0.4:6674",
                                                    .peerId = "10.0.0.4",
                                                    .firstSeenSecondsAgo = 60,
                                                    .attempts = 30,
                                                    .claimsChanged = 4,
                                                    .decision = Wire::EnrollmentDecision::Approved,
                                                    .role = Wire::EnrollRole::Learner,
                                                    .publicKey = KeyOf(0x33),
                                                    .rosterFingerprint = Cluster::DigestOfRoster(
                                                        RosterWith(Member("drifted", "10.0.0.4:6680", KeyOf(0x33)))),
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.4" },
                     Wire::EnrollmentPendingEntry { .nodeId = "worker-w",
                                                    .nodeEndpoint = {},
                                                    .peerId = "10.0.0.5",
                                                    .firstSeenSecondsAgo = 1,
                                                    .attempts = 1,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending,
                                                    .role = Wire::EnrollRole::Worker,
                                                    .publicKey = KeyOf(0x44),
                                                    .rosterFingerprint = std::nullopt,
                                                    .autoApprovedArmedSecondsAgo = std::nullopt,
                                                    .firstPeerId = "10.0.0.5" } }
    };

    auto const text = RenderEnrollmentReport(report, "10.0.0.1:6675");

    // The window's own mode, because a list of rows with no heading is one an operator
    // can read while a deadline admits machines and act on as though a person decided each.
    CHECK(text.contains("auto-approve"));
    CHECK(text.contains("42 min left"));

    // Every row is present and carries BOTH addresses. The claimed endpoint and the
    // observed host are the comparison this list exists to make, so a renderer that
    // dropped either would leave the mark describing nothing.
    for (auto const& row: report.pending)
    {
        INFO("row " << row.nodeId);
        CHECK(text.contains(row.nodeId));
        CHECK(text.contains(row.peerId));
    }
    CHECK(text.contains("10.0.0.9:6674"));

    // The marks, per row and in both directions. `honest` and `elsewhere` claim the
    // SAME endpoint and differ only in where the request came from, so a renderer
    // keying on anything but that pair cannot separate them.
    // The WHOLE line, walked back to the preceding newline as well as forward to the
    // next one. The decision label is rendered to the LEFT of the id, so a helper that
    // only reads forward silently excludes it -- and the two assertions that need it
    // would then fail for a reason that has nothing to do with the renderer.
    auto const lineFor = [&text](std::string_view id) {
        auto const at = text.find(id);
        REQUIRE(at != std::string::npos);
        auto const begin = text.rfind('\n', at);
        auto const from = begin == std::string::npos ? 0 : begin + 1;
        auto const end = text.find('\n', at);
        return text.substr(from, end == std::string::npos ? std::string::npos : end - from);
    };

    CHECK(!lineFor("honest").contains("does not match"));
    CHECK(lineFor("elsewhere").contains("does not match"));

    // And the drift mark, which answers a different question: this row stopped tracking
    // the machine when somebody decided about it, and something has polled since
    // claiming otherwise. A row whose addresses agree can still have drifted, which is
    // why `drifted` is arranged with a matching pair.
    CHECK(!lineFor("drifted").contains("does not match"));
    CHECK(lineFor("drifted").contains("4 later poll"));
    CHECK(!lineFor("honest").contains("later poll"));

    // The decision label, because "approved" and "pending" send an operator to opposite
    // actions and the list is where they tell them apart.
    CHECK(lineFor("drifted").contains("approved"));
    CHECK(lineFor("honest").contains("pending"));

    // **The KEY, whole, for every row** (#178): it is the string an operator compares with
    // what the machine printed, and the comparison is the whole strength of an approval. A
    // prefix would be a comparison of the part an impostor can choose.
    for (auto const& row: report.pending)
    {
        INFO("row " << row.nodeId);
        CHECK(text.contains(FormatEd25519PublicKey(row.publicKey)));
    }

    // The roster fingerprint only where one was served -- absent renders nothing, never a
    // zero digest the joiner could not have printed.
    CHECK(text.contains(Cluster::RenderRosterFingerprint(Unwrap(report.pending[2].rosterFingerprint))));
    CHECK(text.find("SHA256:") == text.rfind("SHA256:"));

    // A worker states no endpoint, so it is shown as having none and carries no mismatch
    // mark: there is nothing to compare its host against.
    CHECK(lineFor("worker-w").contains("worker"));
    CHECK(lineFor("worker-w").contains("no endpoint"));
    CHECK_FALSE(lineFor("worker-w").contains("does not match"));
}

TEST_CASE("A window with nothing waiting renders as a reading and not as an empty page", "[enrollment][client]")
{
    // Absent is not zero, at the surface a person reads: a list that rendered nothing
    // for a manual window and nothing for an armed empty one would make an operator who
    // armed a deadline believe it had not taken.
    auto const manual = RenderEnrollmentReport(Wire::EnrollmentReport {}, "10.0.0.1:6675");
    CHECK(manual.contains("enrollment: manual"));
    CHECK(manual.contains("nothing waiting"));

    auto const armedAndEmpty = RenderEnrollmentReport(
        Wire::EnrollmentReport {
            .state = Wire::WireEnrollmentState::AutoApprove, .autoApproveSecondsLeft = 7 * 60, .pending = {} },
        "10.0.0.1:6675");
    CHECK(armedAndEmpty.contains("enrollment: auto-approve (7 min left)"));
    CHECK(armedAndEmpty.contains("nothing waiting"));
    CHECK(armedAndEmpty != manual);
}

namespace
{

/// A wait that advances its own clock by exactly what was requested and never blocks.
///
/// The pause a loop REQUESTS is exact and host-independent, which is what makes this
/// assertable where a real sleep would only be slow.
class InstantWait final: public IDrainWait
{
  public:
    /// @return The accumulated instant.
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return _now;
    }

    /// @param requested Added to the clock rather than slept.
    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        _now += requested;
    }

  private:
    core::platform::SteadyTimePoint _now {};
};

/// A seed answering `NotLeader` and naming where to go instead.
/// @param leader The endpoint the reply names.
/// @return The framed refusal.
[[nodiscard]] std::vector<std::byte> RedirectTo(std::string_view leader)
{
    return Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, std::string { leader });
}

/// A seed that recorded the request and is waiting for a person.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> Recorded()
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Pending, {}));
}

/// A seed whose operator refused this machine.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> Rejected()
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Rejected, {}));
}

} // namespace

TEST_CASE("A node that answered on its own behalf breaks the redirect chain", "[enrollment][client]")
{
    // **The property #1348 exists for, and it was untestable until the dial became a
    // seam.** `MaxEnrollRedirects` bounds a CONSECUTIVE chain -- the loop two nodes with a
    // stale `_knownLeader` make by naming each other -- and NOT the whole run. This
    // mode waits up to ten minutes for a person, which is hundreds of polls, so a
    // budget accumulated across the run would abort a healthy enrolment after three
    // ordinary leadership changes, reporting "gave up after 3 leader redirect(s)"
    // about a loop that never happened.
    //
    // Four redirects with a `Waiting` in the middle: two, then an answer on the
    // seed's own behalf, then two more. Neither run reaches the bound.
    //
    // **Two rather than three on each side, deliberately.** `MaxEnrollRedirects` is 3 and
    // the guard is `>=`, so three consecutive redirects are still followed and the
    // fourth aborts -- a post-reset run of three would pass on the last value that
    // still works, with zero margin. An off-by-one anywhere else, the bound moving or
    // `>=` becoming `>`, would then redden THIS case and read as *the reset is
    // broken*: a true failure carrying a false diagnosis. The bound itself is pinned
    // by the case below, which is named for it.
    Testing::ScratchDirectory scratch { "enroll-redirect-chain" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.clusterDir = scratch.Path();
    cfg.enrollFrom = "10.0.0.1:7000";

    // A consensus port of its own, so it asks as a LEARNER: the role follows from what
    // the node runs. The same pair `EnrollTrap_test` stages.
    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";

    Testing::ScriptedDialer dialer { {
        RedirectTo("10.0.0.2:7000"),
        RedirectTo("10.0.0.3:7000"),
        Recorded(),
        RedirectTo("10.0.0.4:7000"),
        RedirectTo("10.0.0.5:7000"),
        Rejected(),
    } };

    InstantWait wait;
    SystemSecureRandom random;
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto const outcome = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random, keyGuard, wait, dialer);

    REQUIRE(!outcome.has_value());

    // Named on the failure path: every refusal on this route produces an error string,
    // so a case that fails without printing WHICH one cannot be diagnosed from its output.
    INFO("refusal: " << outcome.error());
    INFO("dialled: " << dialer.Dialed().size() << " endpoint(s)");

    // **The assertion that DISTINGUISHES.** Both readings end in an error, so
    // asserting that this failed proves nothing. With the reset the run reaches the
    // sixth reply and ends on the operator's refusal; without it the count
    // accumulates and the FIFTH reply trips the bound instead. Remove `redirects = 0`
    // from the `Waiting`/`Closed` arm and all three of these flip together.
    CHECK(outcome.error().contains("refused this machine"));
    CHECK_FALSE(outcome.error().contains("gave up after"));
    // REQUIRE, and last of the three so the two above still report: the reads below take
    // `front()` and `back()`, and on a run that dialled nothing a CHECK here let them read an
    // empty vector -- the process died mid-report and took every later case's verdict with it.
    REQUIRE(dialer.Dialed().size() == 6);

    // And it followed each redirect to the endpoint the reply named rather than
    // re-asking the seed, which is what makes the five above a chain at all.
    CHECK(dialer.Dialed().front() == "10.0.0.1:7000");
    CHECK(dialer.Dialed().back() == "10.0.0.5:7000");
}

TEST_CASE("A consecutive redirect chain is still bounded", "[enrollment][client]")
{
    // The control, and the direction the reset could have broken: the anti-loop
    // property has to survive the fix. Four back-to-back redirects with nothing
    // answering on its own behalf, so the count never resets and the bound bites.
    // Without this case, deleting `MaxEnrollRedirects` entirely would leave the case above
    // green.
    Testing::ScratchDirectory scratch { "enroll-redirect-loop" };
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.clusterDir = scratch.Path();
    cfg.enrollFrom = "10.0.0.1:7000";

    // A consensus port of its own, so it asks as a LEARNER: the role follows from what
    // the node runs. The same pair `EnrollTrap_test` stages.
    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";

    Testing::ScriptedDialer dialer { {
        RedirectTo("10.0.0.2:7000"),
        RedirectTo("10.0.0.1:7000"),
        RedirectTo("10.0.0.2:7000"),
        RedirectTo("10.0.0.1:7000"),
    } };

    InstantWait wait;
    SystemSecureRandom random;
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto const outcome = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random, keyGuard, wait, dialer);

    REQUIRE(!outcome.has_value());

    // Named on the failure path: every refusal on this route produces an error string,
    // so a case that fails without printing WHICH one cannot be diagnosed from its output.
    INFO("refusal: " << outcome.error());
    INFO("dialled: " << dialer.Dialed().size() << " endpoint(s)");
    CHECK(outcome.error().contains("gave up after"));

    // Four dials rather than all four replies consumed: the fourth REPLY is never
    // read, because the bound is checked before the redirect is followed.
    CHECK(dialer.Dialed().size() == 4);
}

namespace
{

/// A seed that approved this machine and sent @p roster.
/// @param roster The roster.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> ApprovedWith(std::span<std::byte const> roster)
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Approved, roster));
}

/// A joiner's configuration, and the identity and key it will mint -- minted HERE first, so a
/// case can build the roster a seed would hand it. `RunEnrollClient` reads both back rather
/// than minting afresh, which is the property that makes a key an identity at all.
struct MintedJoiner
{
    NodeConfig cfg;
    std::string id;
    Ed25519PublicKey key {};
};

/// A roster in which the leader is a member and @p id a WORKER principal under @p key: what a seed
/// hands a machine the one-shot `--enroll-from` admitted, since it asks as a worker.
/// @param id The principal's id.
/// @param key Its key.
/// @return The encoded roster.
[[nodiscard]] std::vector<std::byte> WorkerRosterWith(std::string_view id, Ed25519PublicKey const& key)
{
    auto state = Cluster::ClusterState {};
    state.members.push_back(Member("leader", "10.0.0.1:7100", KeyOf(0x01)));
    state.principals.push_back(
        Cluster::ClusterPrincipal { .id = std::string { id }, .publicKey = key, .role = Cluster::PrincipalRole::Worker });
    return Cluster::EncodeRoster(Cluster::ProjectRoster(state));
}

/// Mint a joiner's identity and key into @p stateDirectory.
/// @param stateDirectory Its `--cluster-dir`.
/// @return The configuration, and the id and key the seed would record it under.
[[nodiscard]] MintedJoiner MintJoiner(std::filesystem::path const& stateDirectory)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.clusterDir = stateDirectory;
    cfg.enrollFrom = "10.0.0.1:7000";
    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";

    SystemSecureRandom random;
    auto identity = ResolveNodeIdentity(NodeStateDirectory(cfg), cfg.nodeId, random);
    REQUIRE(identity.has_value());
    auto resolved = cfg;
    ApplyNodeIdentity(resolved, *identity);
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto key = ResolveNodeKeyFor(resolved, random, keyGuard);
    REQUIRE(key.has_value());
    return MintedJoiner { .cfg = cfg, .id = resolved.nodeId, .key = key->pair.PublicKey() };
}

} // namespace

TEST_CASE("A joiner asks under the key it minted and believes a roster that records exactly that key",
          "[enrollment][client][security]")
{
    Testing::ScratchDirectory scratch { "enroll-admitted" };
    auto const joiner = MintJoiner(scratch.Path());
    auto const roster = WorkerRosterWith(joiner.id, joiner.key);

    Testing::ScriptedDialer dialer { { Recorded(), ApprovedWith(roster) } };
    InstantWait wait;
    SystemSecureRandom random;
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto const admitted =
        RunEnrollClient(joiner.cfg, ConfiguredCredential { joiner.cfg, nullptr }, random, keyGuard, wait, dialer);

    INFO("result: " << admitted.value_or(admitted.error_or("")));
    REQUIRE(admitted.has_value());
    CHECK(admitted->contains(Cluster::RenderRosterFingerprint(Cluster::DigestOfRoster(roster))));

    // **What it ASKED with, read off the wire**: the key in the request is the key minted
    // into the state directory, on every poll. A client that minted a second key per run,
    // or sent none, would be admitted under a key it does not hold.
    for (auto const index: { std::size_t { 0 }, std::size_t { 1 } })
    {
        auto const sent = dialer.SentOn(index);
        auto const header = Wire::DecodeRequestHeader(sent);
        REQUIRE(header.has_value());
        auto const request = Wire::DecodeEnrollPayload(sent.subspan(Wire::RequestHeaderSize));
        REQUIRE(request.has_value());
        CHECK(Unwrap(request).publicKey == joiner.key);
        // A WORKER, even on a node that runs consensus: a member joins through its formation,
        // and the one-shot verb admits a worker by its key. It states no endpoint.
        CHECK(Unwrap(request).role == Wire::EnrollRole::Worker);
        CHECK(Unwrap(request).nodeEndpoint.empty());
    }
}

TEST_CASE("A joiner handed a roster naming it under another key is not admitted", "[enrollment][client][security]")
{
    // The control for the case above, through the same client and the same dialer: only the
    // KEY in the roster differs, so this failing and that passing is the self check.
    Testing::ScratchDirectory scratch { "enroll-swapped" };
    auto const joiner = MintJoiner(scratch.Path());

    Testing::ScriptedDialer dialer { { ApprovedWith(WorkerRosterWith(joiner.id, KeyOf(0x99))) } };
    InstantWait wait;
    SystemSecureRandom random;
    auto keyGuard = Testing::ScriptedNodeKeyGuard::OwnerOnly();
    auto const refused =
        RunEnrollClient(joiner.cfg, ConfiguredCredential { joiner.cfg, nullptr }, random, keyGuard, wait, dialer);

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("does not record"));
}

namespace
{
constexpr std::string_view FirstScheduler = "10.0.0.1:7000";
constexpr std::string_view SecondScheduler = "10.0.0.2:7000";

/// A leader answering `--enroll-list` with a manual window and nothing waiting.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> QuietWindow()
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(Wire::EnrollmentReport {}));
}

/// An operator's node configured with both schedulers, in order.
/// @return The configuration.
[[nodiscard]] NodeConfig TwoSchedulers()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { std::string { FirstScheduler }, std::string { SecondScheduler } };
    return cfg;
}
} // namespace

TEST_CASE("An enrollment command asks the next --scheduler when the first cannot be reached",
          "[enrollment][client][fallback]")
{
    // #1310: the operator's verbs read the same list the heartbeat does. Asserted by
    // WHICH endpoint was sent the request, because a verb that asked only the first
    // passes every case whose first entry answers.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { {}, QuietWindow() } };

    auto const listed = RunEnrollAdmin(
        cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, ConfiguredCredential { cfg, nullptr }, dialer);

    INFO("result: " << listed.value_or(listed.error_or("")));
    REQUIRE(listed.has_value());
    CHECK(listed->contains("manual"));
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler }, std::string { SecondScheduler } });
    CHECK(dialer.SentOn(0).empty());
    CHECK_FALSE(dialer.SentOn(1).empty());
}

TEST_CASE("An enrollment command that reached a scheduler is never sent to another", "[enrollment][client][fallback]")
{
    // The line `DialFirstReachable` draws: a fallback only where nothing was SENT.
    // `--enroll-approve` commits `ClusterAdmit` before it answers, so an approval that
    // may have been applied where it landed must not be proposed a second time elsewhere. The first scheduler connects and
    // then answers nothing readable; a second dial would run this script out, which the fake reports in its own voice.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { std::vector<std::byte> { std::byte { 0xFF } } } };

    auto const approved =
        RunEnrollAdmin(cfg,
                       EnrollCommand { .action = EnrollAction::Approve, .subject = "n9", .key = Ed25519PublicKey {} },
                       ConfiguredCredential { cfg, nullptr },
                       dialer);

    REQUIRE_FALSE(approved.has_value());
    INFO("refusal: " << approved.error());
    CHECK(approved.error().contains(FirstScheduler));
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler } });
}

TEST_CASE("An enrollment command that reaches no --scheduler names every one it tried", "[enrollment][client][fallback]")
{
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { {}, {} } };

    auto const listed = RunEnrollAdmin(
        cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, ConfiguredCredential { cfg, nullptr }, dialer);

    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().contains(std::format("{}, {}", FirstScheduler, SecondScheduler)));
}

TEST_CASE("A NotLeader sends an enrollment command to the leader it names, not down the --scheduler list",
          "[enrollment][client][fallback]")
{
    // A redirect is an instruction. Consulting the list for it would send the request
    // to `SecondScheduler`, which the first has just said does not lead.
    auto const cfg = TwoSchedulers();
    Testing::ScriptedDialer dialer { { RedirectTo("10.0.0.9:7000"), QuietWindow() } };

    auto const listed = RunEnrollAdmin(
        cfg, EnrollCommand { .action = EnrollAction::List, .subject = {} }, ConfiguredCredential { cfg, nullptr }, dialer);

    REQUIRE(listed.has_value());
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler }, "10.0.0.9:7000" });
}

TEST_CASE("The enrollment list marks a row an armed window admitted, with when it was armed",
          "[enrollment][client][auto-approve]")
{
    auto row = Wire::EnrollmentPendingEntry { .nodeId = "laptop",
                                              .nodeEndpoint = {},
                                              .peerId = "10.1.2.3",
                                              .firstSeenSecondsAgo = 40,
                                              .attempts = 3,
                                              .claimsChanged = 0,
                                              .decision = Wire::EnrollmentDecision::Approved,
                                              .role = Wire::EnrollRole::Learner,
                                              .publicKey = KeyOf(0x42),
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = 120,
                                              .firstPeerId = "10.1.2.3" };
    auto const rendered = RenderEnrollmentReport(Wire::EnrollmentReport { .state = Wire::WireEnrollmentState::AutoApprove,
                                                                          .autoApproveSecondsLeft = 480,
                                                                          .pending = { row } },
                                                 "10.0.0.1:6675");
    CHECK(rendered.contains("enrollment: auto-approve (8 min left)"));
    CHECK(rendered.contains("auto-approved (window armed 120s ago)"));

    // The control: a row a person approved carries no such line.
    row.autoApprovedArmedSecondsAgo = std::nullopt;
    CHECK_FALSE(
        RenderEnrollmentReport(Wire::EnrollmentReport { .pending = { row } }, "10.0.0.1:6675").contains("auto-approved"));
}

TEST_CASE("The list prints the approval of each waiting row, ready to paste, naming its key", "[enrollment][client]")
{
    auto waiting = Wire::EnrollmentPendingEntry { .nodeId = "laptop",
                                                  .nodeEndpoint = {},
                                                  .peerId = "10.1.2.3",
                                                  .firstSeenSecondsAgo = 5,
                                                  .attempts = 2,
                                                  .claimsChanged = 0,
                                                  .decision = Wire::EnrollmentDecision::Pending,
                                                  .role = Wire::EnrollRole::Learner,
                                                  .publicKey = KeyOf(0x42),
                                                  .rosterFingerprint = std::nullopt,
                                                  .autoApprovedArmedSecondsAgo = std::nullopt,
                                                  .firstPeerId = "10.1.2.3" };
    auto decided = waiting;
    decided.nodeId = "desk";
    decided.decision = Wire::EnrollmentDecision::Approved;

    auto const rendered =
        RenderEnrollmentReport(Wire::EnrollmentReport { .pending = { waiting, decided } }, "10.0.0.1:6675");
    auto const line = std::format("fastcache-compile-node --scheduler=10.0.0.1:6675 --enroll-approve=laptop@{}",
                                  FormatEd25519PublicKey(KeyOf(0x42)));
    CHECK(rendered.contains(line));
    // Only a row nobody decided about: a decided one needs no approval.
    CHECK_FALSE(rendered.contains("--enroll-approve=desk@"));

    // And the line is one this binary PARSES back into the approval it names.
    auto const flag = std::format("--enroll-approve=laptop@{}", FormatEd25519PublicKey(KeyOf(0x42)));
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const argv = std::vector<char const*> { "--scheduler=10.0.0.1:6675", flag.c_str() };
    REQUIRE(ParseNodeCommandLine(std::span<char const* const> { argv }, cfg).has_value());
    CHECK(cfg.enroll.subject == "laptop");
    CHECK(cfg.enroll.key == std::optional { KeyOf(0x42) });
}

TEST_CASE("The list shows where each row first asked from and marks a row asked since from another address",
          "[enrollment][client][security]")
{
    // The per-host cap counts a row by the address it FIRST asked from, and a refusal names that
    // host. So every row shows it, and a row whose machine has since asked from somewhere else is
    // marked: otherwise a host's rows hide under another host's name, and the refusal names rows
    // nobody can find.
    auto const row = [](std::string_view id, std::string_view peer, std::string_view first) {
        return Wire::EnrollmentPendingEntry { .nodeId = std::string { id },
                                              .nodeEndpoint = {},
                                              .peerId = std::string { peer },
                                              .firstSeenSecondsAgo = 5,
                                              .attempts = 2,
                                              .claimsChanged = 0,
                                              .decision = Wire::EnrollmentDecision::Pending,
                                              .role = Wire::EnrollRole::Learner,
                                              .publicKey = KeyOf(0x42),
                                              .rosterFingerprint = std::nullopt,
                                              .autoApprovedArmedSecondsAgo = std::nullopt,
                                              .firstPeerId = std::string { first } };
    };
    auto const text = RenderEnrollmentReport(
        Wire::EnrollmentReport { .pending = { row("moved", "2001:db8::7", "198.51.100.7"),
                                              row("stayed", "198.51.100.8", "198.51.100.8"),
                                              row("mapped", "::ffff:198.51.100.9", "198.51.100.9") } },
        "10.0.0.1:6675");
    auto const lineFor = [&text](std::string_view id) {
        auto const at = text.find(std::format("  {}  ", id));
        REQUIRE(at != std::string::npos);
        auto const end = text.find('\n', at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    };

    // Every row shows its first address, beside the current one.
    CHECK(lineFor("moved").contains("from 2001:db8::7  first from 198.51.100.7"));
    CHECK(lineFor("stayed").contains("from 198.51.100.8  first from 198.51.100.8"));

    // Marked in one direction only: a renderer marking every row would pass a case that checked
    // the moved one alone. An IPv4-mapped spelling of the same address is no move, as the cap
    // folds it.
    CHECK(lineFor("moved").contains("asked since from another address"));
    CHECK_FALSE(lineFor("stayed").contains("asked since from another address"));
    CHECK_FALSE(lineFor("mapped").contains("asked since from another address"));
}
