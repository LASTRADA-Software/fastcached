// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "NodeIdentity.hpp"

#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <CacheProtocol.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reply the seed sent, as the exchange layer hands it over.
/// @param outcome What the leader decided.
/// @param key The bytes it sent with it.
/// @return The outcome a client would read.
[[nodiscard]] Cc::CacheOutcome Served(Wire::EnrollOutcome outcome, std::string_view key = {})
{
    auto const bytes = Wire::AsBytes(key);
    return Cc::CacheOutcome { .kind = Cc::CacheOutcomeKind::Hit,
                              .value = Wire::EncodeEnrollReply(outcome, bytes),
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

/// Bytes as a `SecureByteBuffer`, so a case can hand `StoreClusterKey` a key.
/// @param text What the leader sent.
/// @return The buffer.
[[nodiscard]] SecureByteBuffer KeyOf(std::string_view text)
{
    auto const bytes = Wire::AsBytes(text);
    return SecureByteBuffer { bytes.begin(), bytes.end() };
}

} // namespace

TEST_CASE("A pending reply is a wait, and an approved one carries the key", "[enrollment][client]")
{
    auto const waiting = ReadEnrollReply(Served(Wire::EnrollOutcome::Pending));
    CHECK(waiting.progress == EnrollProgress::Waiting);
    CHECK(waiting.clusterKey.empty());

    auto const admitted = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved, "a-cluster-key-long-enough"));
    CHECK(admitted.progress == EnrollProgress::Admitted);
    CHECK(std::string { Wire::AsStringView(admitted.clusterKey) } == "a-cluster-key-long-enough");

    // A person said no. Distinguished from every WAIT above because asking again will
    // not change it, and a client that read it as a wait would poll until its bound ran
    // out and then report a timeout for a decision that was already taken.
    auto const refused = ReadEnrollReply(Served(Wire::EnrollOutcome::Rejected));
    CHECK(refused.progress == EnrollProgress::Refused);
}

TEST_CASE("An approval carrying no key is a fault rather than an admission", "[enrollment][client]")
{
    // **The client's half of the assertion the server side also makes.** A seed that
    // approved and sent nothing would have this node write a zero-length key file and
    // fail much later, on a machine nobody is watching -- and the outcome byte is
    // correct in that build, so the LENGTH is the only thing that separates them.
    auto const reading = ReadEnrollReply(Served(Wire::EnrollOutcome::Approved));
    CHECK(reading.progress == EnrollProgress::Fatal);
    CHECK(reading.detail.contains("no key"));
}

TEST_CASE("A closed or full window is a wait, and a seed too old to know the verb is not", "[enrollment][client]")
{
    // Three refusals a client answers by asking again: two say *not yet* and the third
    // says *ask in a moment, somebody is being elected*. Collapsing any of them into
    // the fatal arm would make a joiner give up on a fleet that was about to admit it.
    for (auto const code: { Wire::ErrorCode::EnrollmentClosed, Wire::ErrorCode::EnrollmentFull, Wire::ErrorCode::NotLeader })
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
    NodeConfig cfg;

    // A node that names no consensus member of its own has nothing to be admitted AS,
    // and the refusal says which two flags supply the halves rather than reporting an
    // empty string.
    auto const nothing = EnrollClaim(cfg);
    REQUIRE(!nothing.has_value());
    CHECK(nothing.error().contains("--listen-raft"));
    CHECK(nothing.error().contains("--raft-self"));

    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";
    ApplyNodeIdentity(cfg, NodeIdentity { .id = "joiner-a", .origin = NodeIdentityOrigin::Minted });

    auto const claim = EnrollClaim(cfg);
    REQUIRE(claim.has_value());

    // BOTH halves, and both taken from `ClusterSelfMember`: an id with no address is a
    // member the cluster counts and cannot reach, and an address derived a second way
    // here could disagree with the one consensus will run under.
    CHECK(claim->first == "joiner-a");
    CHECK(claim->second == "198.51.100.4:7100");
}

TEST_CASE("A stored key lands where the node will read it and is not left broadly readable", "[enrollment][client]")
{
    Testing::ScratchDirectory scratch { "enroll-key" };
    auto const path = scratch / "sub" / "cluster.key";

    REQUIRE(StoreClusterKey(path, KeyOf("a-cluster-key-long-enough")).has_value());
    REQUIRE(std::filesystem::exists(path));
    CHECK(std::filesystem::file_size(path) == std::string_view { "a-cluster-key-long-enough" }.size());

    // The property asked BACK rather than the call's own success trusted: a key written
    // into a machine-wide directory inherits that directory's broad read on Windows, and
    // this is the one file whose exposure admits a machine to the fleet.
    CHECK(SecretFileExposure(path) == SecretExposure::None);

    // And the parent was created, because a fresh install has nothing there yet -- which
    // is the deployment this whole mode exists for.
    CHECK(std::filesystem::is_directory(path.parent_path()));
}

TEST_CASE("A key file that already exists is never written over", "[enrollment][client]")
{
    Testing::ScratchDirectory scratch { "enroll-key-exists" };
    auto const path = scratch / "cluster.key";
    // The two keys must differ in LENGTH, because the surviving-content assertion below
    // can only be a size comparison -- see there. Asserted rather than left to whoever
    // next edits a string, since two keys of equal length would make that check pass
    // under the very truncation it is here to catch.
    constexpr auto original = std::string_view { "the-first-cluster-key-here" };
    constexpr auto replacement = std::string_view { "a-different-cluster-key!!" };
    static_assert(original.size() != replacement.size(),
                  "the refusal is judged by the file's SIZE, so a replacement of equal length would be "
                  "indistinguishable from the original surviving");

    REQUIRE(StoreClusterKey(path, KeyOf(original)).has_value());

    // A machine that already holds a key is either already a member or is being pointed
    // at a second cluster, and both are decisions an operator takes by removing the file
    // deliberately. Overwriting would silently move a running node between clusters.
    auto const again = StoreClusterKey(path, KeyOf(replacement));
    REQUIRE(!again.has_value());
    CHECK(again.error().contains("already exists"));

    // And the original survived, which is the half that matters: a refusal that had
    // already truncated the file would leave the node keyless.
    //
    // By SIZE and not by reading the bytes back, which is not a style choice: the store
    // ends in `SecureSecretFileForServices`, so on Windows the file's own access list
    // has been narrowed to SYSTEM, Administrators and the service account -- and a
    // non-elevated test process is none of those, so `ReadClusterKey` answers
    // *cannot open* here. Measured, not assumed. `file_size` still succeeds, needing
    // only the directory's traverse right, which is what leaves it as the instrument.
    // The `static_assert` above is what makes a size sufficient.
    //
    // **This case is also what confirms that `"wbx"` is honoured.** `StoreClusterKey`
    // refuses through the exclusive CREATE with no `exists()` in front of it, so a libc
    // that ignored the `x` would truncate here and redden this assertion -- on
    // whichever platform ignored it, which is the only place the question can be
    // answered. Putting any pre-flight existence check back into that function makes
    // this case pass without ever reaching the property it is named for.
    CHECK(std::filesystem::file_size(path) == original.size());
}

TEST_CASE("Enrolling with nowhere to put the key is refused before anything is asked of anybody", "[enrollment][client]")
{
    // The mode's own precondition, refused by name at its own door rather than through
    // the startup table -- which judges a configuration this node will SERVE with, and
    // this one serves nothing. `RunClusterAdmin` refuses its own `--scheduler` the same
    // way and for the same reason.
    NodeConfig cfg;
    cfg.enrollFrom = "10.0.0.1:7000";

    SystemRandomSource random;
    auto const refused = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().contains("--cluster-key-file"));
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
    NodeConfig cfg;
    cfg.clusterDir = scratch.Path();
    cfg.clusterKeyFile = scratch / "cluster.key";
    cfg.enrollFrom = "10.0.0.1";

    SystemRandomSource random;
    auto const refused = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random);
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

TEST_CASE("A machine that already holds a cluster key is refused without spending its enrollment",
          "[enrollment][client][security]")
{
    // **`StoreClusterKey` refuses this too, and by then it is too late.** The grant is
    // spendable once: the seed takes `Approved -> Collected` when it serves the key, so
    // a machine with a pre-placed key file would dial, be approved, BURN the one
    // collection its id has, and only then discover locally that it had nowhere to put
    // what it was given. Recovering needs an operator to re-approve an id that looks
    // already handled.
    //
    // The two checks are not alternatives and both stay: this one is ADVISORY and is
    // allowed to be wrong, since anything may create that file during the ten minutes
    // this mode spends waiting for a person; the one inside `StoreClusterKey` is the
    // exclusive CREATE itself, which is what makes the guarantee atomic.
    Testing::ScratchDirectory scratch { "enroll-key-held" };
    auto const path = scratch / "cluster.key";
    REQUIRE(StoreClusterKey(path, KeyOf("a-cluster-key-long-enough")).has_value());

    NodeConfig cfg;
    cfg.clusterDir = scratch / "state";
    cfg.clusterKeyFile = path;
    cfg.enrollFrom = "10.0.0.1:7000";

    SystemRandomSource random;
    auto const refused = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random);
    REQUIRE(!refused.has_value());
    CHECK(refused.error().contains("already exists"));

    // It says the seed was never asked, because that is the fact an operator needs: a
    // refusal that looked like the post-exchange one would send somebody hunting for a
    // spent grant that does not exist.
    CHECK(refused.error().contains("no enrollment request was spent"));

    // And nothing was minted, so this refusal really is ahead of everything the mode
    // does rather than merely ahead of the dial.
    CHECK(!std::filesystem::exists(cfg.clusterDir));
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
        .state = Wire::WireEnrollmentState::Open,
        .openForSeconds = 42,
        .pending = { Wire::EnrollmentPendingEntry { .nodeId = "honest",
                                                    .raftEndpoint = "10.0.0.9:7100",
                                                    .peerId = "10.0.0.9",
                                                    .firstSeenSecondsAgo = 3,
                                                    .attempts = 2,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending },
                     Wire::EnrollmentPendingEntry { .nodeId = "elsewhere",
                                                    .raftEndpoint = "10.0.0.9:7100",
                                                    .peerId = "203.0.113.7",
                                                    .firstSeenSecondsAgo = 9,
                                                    .attempts = 5,
                                                    .claimsChanged = 0,
                                                    .decision = Wire::EnrollmentDecision::Pending },
                     Wire::EnrollmentPendingEntry { .nodeId = "drifted",
                                                    .raftEndpoint = "10.0.0.4:6680",
                                                    .peerId = "10.0.0.4",
                                                    .firstSeenSecondsAgo = 60,
                                                    .attempts = 30,
                                                    .claimsChanged = 4,
                                                    .decision = Wire::EnrollmentDecision::Collected } }
    };

    auto const text = RenderEnrollmentReport(report);

    // The window's own state, because a list of rows with no heading is one an operator
    // can read while the window is shut and act on as though it were open.
    CHECK(text.contains("OPEN"));
    CHECK(text.contains("42"));

    // Every row is present and carries BOTH addresses. The claimed endpoint and the
    // observed host are the comparison this list exists to make, so a renderer that
    // dropped either would leave the mark describing nothing.
    for (auto const& row: report.pending)
    {
        INFO("row " << row.nodeId);
        CHECK(text.contains(row.nodeId));
        CHECK(text.contains(row.peerId));
    }
    CHECK(text.contains("10.0.0.9:7100"));

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

    // The decision label, because "collected" and "approved" send an operator to
    // opposite actions and the list is where they tell them apart.
    CHECK(lineFor("drifted").contains("collected"));
    CHECK(lineFor("honest").contains("pending"));
}

TEST_CASE("A closed window with nothing waiting renders as a reading, not as an empty page", "[enrollment][client]")
{
    // Absent is not zero, at the surface a person reads: a list that rendered nothing
    // for a shut window and nothing for an open empty one would make an operator who
    // forgot to open it believe nobody has asked.
    auto const shut = RenderEnrollmentReport(Wire::EnrollmentReport {});
    CHECK(shut.contains("closed"));
    CHECK(shut.contains("nothing waiting"));

    auto const openAndEmpty = RenderEnrollmentReport(
        Wire::EnrollmentReport { .state = Wire::WireEnrollmentState::Open, .openForSeconds = 7, .pending = {} });
    CHECK(openAndEmpty.contains("OPEN"));
    CHECK(openAndEmpty.contains("nothing waiting"));
    CHECK(openAndEmpty != shut);
}
