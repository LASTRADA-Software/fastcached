// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"
#include "CliVerbs.hpp"
#include "FleetReach.hpp"
#include "ScriptedExchange.hpp"
#include "StatsGatherer.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;
using namespace FastCache::Cli::Testing;

// `Cc` and not `Wire`: `Cli::Wire` is this tool's own enum of the three transports,
// and a `namespace Wire` alias beside it is ambiguous at every use.
namespace Cc = FastCache::CompileCacheWire;

namespace
{

/// Run @p name against a scripted node.
/// @param name The verb as an operator types it.
/// @param node What the node answers.
/// @param operands The positional arguments.
/// @return The answer.
[[nodiscard]] Answer RunNodeVerb(std::string_view name, INodeExchange& node, std::vector<std::string> const& operands = {})
{
    auto const* const verb = FindVerb(name);
    REQUIRE(verb != nullptr);
    return RunVerb(*verb, VerbContext { .operands = operands, .node = &node });
}

/// A framed `NodeStatus` reply carrying @p fields.
/// @param fields What the node is.
/// @return The reply frame.
[[nodiscard]] std::vector<std::byte> StatusReply(Cc::NodeStatusFields const& fields)
{
    return Cc::EncodeReply(Cc::Status::Ok, Cc::EncodeNodeStatus(fields));
}

/// A framed refusal.
/// @param code Which refusal.
/// @param detail Words for a person.
/// @return The reply frame.
[[nodiscard]] std::vector<std::byte> RefusalReply(Cc::ErrorCode code, std::string_view detail = {})
{
    return Cc::EncodeErrorReply(code, detail);
}

/// The value of a record field, or nullptr.
/// @param answer The answer.
/// @param name The field.
/// @return The cell, or nullptr.
[[nodiscard]] Cell const* CellOf(Answer const& answer, std::string_view name)
{
    auto const* const field = FindField(answer.value, name);
    return field == nullptr ? nullptr : &field->value;
}

/// The value of a record field, with its presence REQUIRED.
///
/// **Returns a REFERENCE, and that is the fix rather than a convenience.**
/// `CellOf(answer, name)->lexical` reads a `std::string` through a pointer that is null
/// for a field the answer does not carry, so a missing field SEGFAULTS the binary
/// instead of failing the check -- and a crashed Catch2 binary reports zero failures in
/// every summary format there is, so the test written to detect a missing field is the
/// one that cannot report it.
///
/// GCC says so at `-O3`, where inlining lets `-Wnull-dereference` see the null return
/// edge reach the dereference; no Debug build can produce that diagnostic, which is why
/// a green local gate was not evidence here. Handing back a `Cell const&` retires the
/// question at the CALL SITE by type -- there is no pointer left to forget to check --
/// where seventeen hand-written guards would each have to be remembered, and nine of the
/// ten that existed guarded a DIFFERENT key from the one being read.
/// @param answer The answer.
/// @param name The field.
/// @return The cell. Fails the case rather than returning when the field is absent.
[[nodiscard]] Cell const& RequiredCell(Answer const& answer, std::string_view name)
{
    INFO("required record field: " << name);
    auto const* const cell = CellOf(answer, name);
    REQUIRE(cell != nullptr);
    return *cell;
}

/// The op a framed request names.
/// @param request What was sent.
/// @return The opcode.
[[nodiscard]] std::uint8_t OpOf(std::span<std::byte const> request)
{
    auto const header = Cc::DecodeRequestHeader(request);
    REQUIRE(header.has_value());
    return Unwrap(header).opRaw;
}

/// A `NodeMetrics` reply as a node sends it: one encoded `StatsReading`.
/// @param jobsCompleted What `WorkerJobsCompleted` reads; every other counter reads zero.
/// @param storage The cache tier's figures the reading carries.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> NodeMetricsReply(std::uint64_t jobsCompleted, StorageStats storage)
{
    auto sink = AtomicMetricsSink {};
    sink.Increment(IMetricsSink::Counter::WorkerJobsCompleted, jobsCompleted);
    auto snapshot = MetricsSnapshot {};
    snapshot.storage = storage;
    return Cc::EncodeReply(Cc::Status::Ok, EncodeStatsReading(CaptureStatsReading(sink, snapshot)));
}

} // namespace

TEST_CASE("`node` reports what the endpoint is", "[cli][node][verbs]")
{
    ScriptedNodeExchange node { { StatusReply(
        { .version = "0.2.0-124-gd911b33e",
          .nodeId = "node-a",
          .uptimeSeconds = 3600,
          .surfaces = { { .surface = Cc::WireSurface::Admin, .port = 9101, .tls = false },
                        { .surface = Cc::WireSurface::Raft, .port = 9102, .tls = false } },
          .components = Cc::NodeComponentBit::CacheTier | Cc::NodeComponentBit::Worker }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(answer.value.shape == Shape::Record);

    // The verb sent is the one the row names, read back out of the FRAME rather than
    // taken on the handler's word -- a handler sending the wrong opcode would otherwise
    // pass every case, the fake answering from a script regardless of what it was asked.
    REQUIRE(node.Sent().size() == 1);
    CHECK(OpOf(node.Sent()[0]) == static_cast<std::uint8_t>(Cc::Op::NodeStatus));
    CHECK(node.Unused() == 0);

    CHECK(RequiredCell(answer, "version").lexical == "0.2.0-124-gd911b33e");
    CHECK(RequiredCell(answer, "node-id").lexical == "node-a");
    CHECK(RequiredCell(answer, "uptime-seconds").lexical == "3600");
    CHECK(RequiredCell(answer, "components").lexical == "cache-tier, worker");
    CHECK(RequiredCell(answer, "admin-port").lexical == "9101");
    CHECK(RequiredCell(answer, "raft-port").lexical == "9102");
    CHECK(RequiredCell(answer, "admin-tls").lexical == "false");
}

TEST_CASE("`node` renders an absent surface as an absent FIELD, never a zero port", "[cli][node][verbs]")
{
    // **Absent is not zero**, and here it is structural: a surface the node does not run
    // gets no field at all. A `0` renders as a dialable-looking number in every one of
    // the five formats, and an operator who tries it reaches whatever is on port 0 --
    // which is nothing, reported as the surface being down.
    ScriptedNodeExchange node { { StatusReply(
        { .version = "1.2.3", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {}, .components = 0 }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(CellOf(answer, "admin-port") == nullptr);
    CHECK(CellOf(answer, "raft-port") == nullptr);
    CHECK(CellOf(answer, "discovery-port") == nullptr);

    // And no `admin-tls` either: a scheme for a surface that does not exist is a claim
    // about nothing, and `false` there would read as *plain HTTP*.
    CHECK(CellOf(answer, "admin-tls") == nullptr);

    SECTION("a node with no minted identity reports node-id ABSENT, not empty")
    {
        // An empty string renders as a value somebody could paste into `--raft-peer`.
        CHECK(RequiredCell(answer, "node-id").kind == CellKind::Absent);
    }

    SECTION("and a node running no component says `none` rather than nothing")
    {
        // The converse of the rule above, and it is the half that gets collapsed: a node
        // that runs no component IS a reading. Absent there would say *I could not find
        // out*, which sends an operator to check a connection that worked.
        CHECK(RequiredCell(answer, "components").kind == CellKind::Text);
        CHECK(RequiredCell(answer, "components").lexical == "none");
    }
}

TEST_CASE("`node` says what the worker is DOING, which the component bits cannot", "[cli][node][verbs]")
{
    // **The rendered half of #1295.** `components` carries a `worker` bit that is a
    // constant on the node binary, so it reads the same whether that worker is still
    // walking its include trees or is serving compiles. These three cells are what
    // separate them, and the case drives all three states because an implementation that
    // hard-codes one agrees with whichever arm happens to name it.
    struct Row
    {
        Cc::ToolchainState state;
        std::string_view rendered;
        std::uint32_t served;
    };
    auto const rows = std::array {
        Row { .state = Cc::ToolchainState::Surveying, .rendered = "surveying", .served = 0 },
        Row { .state = Cc::ToolchainState::Serving, .rendered = "serving", .served = 4 },
        Row { .state = Cc::ToolchainState::NothingToServe, .rendered = "nothing-to-serve", .served = 0 },
    };

    for (auto const& row: rows)
    {
        INFO("state " << row.rendered);
        ScriptedNodeExchange node { { StatusReply(
            { .version = "1.2.3",
              .nodeId = {},
              .uptimeSeconds = 5,
              .surfaces = {},
              .components = Cc::NodeComponentBit::Worker,
              .runtime = { .toolchains = row.state, .toolchainsServed = row.served, .toolchainsDiscovered = 4 } }) } };

        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(RequiredCell(answer, "toolchains").lexical == row.rendered);
        CHECK(RequiredCell(answer, "toolchains-served").lexical == std::to_string(row.served));
        CHECK(RequiredCell(answer, "toolchains-discovered").lexical == "4");

        // Numbers rather than text, because this record is also rendered as JSON and
        // CSV: a consumer compares two figures instead of parsing `4 of 4` back out of a
        // sentence. The state is the only one of the three that is a name.
        CHECK(RequiredCell(answer, "toolchains-served").kind == CellKind::Number);
        CHECK(RequiredCell(answer, "toolchains-discovered").kind == CellKind::Number);
        CHECK(RequiredCell(answer, "toolchains").kind == CellKind::Text);

        // And the bit that could not answer this is unchanged across all three.
        CHECK(RequiredCell(answer, "components").lexical == "worker");
    }
}

TEST_CASE("`node` renders a node that published no runtime facts as ABSENT fields", "[cli][node][verbs]")
{
    // **Absent is not zero, and here all three cells go together.** A node too old to
    // carry the record, or one running no worker, said nothing -- and `surveying, 0 of 0`
    // is a reading an operator would act on. The discriminating assertion is that the
    // fields are MISSING; one that rendered zeroes passes any check that only reads
    // numbers.
    ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                .nodeId = {},
                                                .uptimeSeconds = 5,
                                                .surfaces = {},
                                                .components = Cc::NodeComponentBit::Worker }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(CellOf(answer, "toolchains") == nullptr);
    CHECK(CellOf(answer, "toolchains-served") == nullptr);
    CHECK(CellOf(answer, "toolchains-discovered") == nullptr);

    // The rest of the record is unaffected: a missing runtime record must not cost an
    // operator the facts the node DID send, which is what refusing the reply would do.
    CHECK(RequiredCell(answer, "version").lexical == "1.2.3");
    CHECK(RequiredCell(answer, "components").lexical == "worker");
}

TEST_CASE("`node` reports capacity, registration and consensus role", "[cli][node][verbs]")
{
    // The three facts #1294 is for, rendered together because that is how an operator
    // reads them: a follower with full slots and no registrations is a different fault
    // from a leader with none free.
    ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                .nodeId = "node-a",
                                                .uptimeSeconds = 60,
                                                .surfaces = {},
                                                .components = Cc::NodeComponentBit::Worker | Cc::NodeComponentBit::Scheduler,
                                                .runtime = { .toolchains = Cc::ToolchainState::Serving,
                                                             .toolchainsServed = 2,
                                                             .toolchainsDiscovered = 2,
                                                             .compileSlots = 8,
                                                             .compilesInFlight = 3,
                                                             .schedulerRole = Cc::WireSchedulerRole::Follower,
                                                             .leaderEndpoint = "10.0.0.9:6676",
                                                             .registrarsRegistered = 2,
                                                             .registrarsTotal = 3,
                                                             .lastRegistrationSecondsAgo = 41 } }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    CHECK(RequiredCell(answer, "compile-slots").lexical == "8");
    CHECK(RequiredCell(answer, "compiles-in-flight").lexical == "3");
    CHECK(RequiredCell(answer, "registrars-registered").lexical == "2");
    CHECK(RequiredCell(answer, "registrars-total").lexical == "3");
    CHECK(RequiredCell(answer, "last-registration-seconds-ago").lexical == "41");
    CHECK(RequiredCell(answer, "scheduler-role").lexical == "follower");
    CHECK(RequiredCell(answer, "leader").lexical == "10.0.0.9:6676");
}

TEST_CASE("`node` renders a never-registered node and an election as ABSENT, not as zero", "[cli][node][verbs]")
{
    // **Two absences that a number would report as readings.** A node that has never
    // reached a scheduler is not one that registered zero seconds ago, and a cluster
    // mid-election has no leader rather than one called "". Both are the shape that
    // sends an operator to the wrong machine.
    ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                .nodeId = {},
                                                .uptimeSeconds = 5,
                                                .surfaces = {},
                                                .components = Cc::NodeComponentBit::Scheduler,
                                                .runtime = { .schedulerRole = Cc::WireSchedulerRole::Undecided,
                                                             .registrarsRegistered = 0,
                                                             .registrarsTotal = 3 } }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    // Registered nowhere is a READING -- zero of three -- and is reported as one.
    CHECK(RequiredCell(answer, "registrars-registered").lexical == "0");
    CHECK(RequiredCell(answer, "registrars-total").lexical == "3");
    // Never having got through is not a duration, so there is no field at all.
    CHECK(CellOf(answer, "last-registration-seconds-ago") == nullptr);

    // The role is present and the leader it would name is absent AT THE CELL: the field
    // exists, because this node was in a position to know, and it holds no value.
    CHECK(RequiredCell(answer, "scheduler-role").lexical == "undecided");
    CHECK(RequiredCell(answer, "leader").kind == CellKind::Absent);

    // A node running no worker tier offers no slots, which is not zero slots free.
    CHECK(CellOf(answer, "compile-slots") == nullptr);
    CHECK(CellOf(answer, "compiles-in-flight") == nullptr);
}

TEST_CASE("`node` omits the scheduler fields entirely on a node that runs none", "[cli][node][verbs]")
{
    // The distinction the case above cannot make on its own: `undecided` with an absent
    // leader is a node IN an election, and a plain worker is not in one. If the role
    // field appeared here at all, those two would render alike.
    ScriptedNodeExchange node { { StatusReply(
        { .version = "1.2.3",
          .nodeId = {},
          .uptimeSeconds = 5,
          .surfaces = {},
          .components = Cc::NodeComponentBit::Worker,
          .runtime = { .toolchains = Cc::ToolchainState::Serving, .toolchainsServed = 1, .toolchainsDiscovered = 1 } }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(CellOf(answer, "scheduler-role") == nullptr);
    CHECK(CellOf(answer, "leader") == nullptr);
}

TEST_CASE("`node` renders the enrollment window, and says nothing where there is none", "[cli][node][verbs]")
{
    // **The field an operator is TOLD to read to find a window they left open.** The
    // node encoded it and the wire carried it, and no renderer anywhere consumed
    // either cell -- so `--node-status` reported nothing about the one state in which
    // this machine hands its cluster's key to a stranger that asked and was approved.
    //
    // And the third section is the one that matters, because this field exists to
    // carry a distinction the counters cannot. Both enrollment series are rendered by
    // every node and read zero on a machine that has no window at all, so *no window
    // here* and *a window nothing has come through* are the same number. ABSENT
    // against `closed` is where those part company, which is why a case asserting only
    // the open reading would leave the field's whole purpose untested.
    SECTION("an open window names itself and says how many are waiting")
    {
        ScriptedNodeExchange node { { StatusReply(
            { .version = "1.2.3",
              .nodeId = "node-a",
              .uptimeSeconds = 90,
              .surfaces = {},
              .components = Cc::NodeComponentBit::Scheduler | Cc::NodeComponentBit::Consensus,
              .runtime = { .schedulerRole = Cc::WireSchedulerRole::Leader,
                           .enrollment = Cc::WireEnrollmentState::Open,
                           .enrollmentPending = 3 } }) } };

        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(RequiredCell(answer, "enrollment").lexical == "open");
        CHECK(RequiredCell(answer, "enrollment-pending").lexical == "3");
        CHECK(RequiredCell(answer, "enrollment-pending").kind == CellKind::Number);
    }

    SECTION("a shut window on a node that HAS one is reported shut, not omitted")
    {
        ScriptedNodeExchange node { { StatusReply(
            { .version = "1.2.3",
              .nodeId = "node-a",
              .uptimeSeconds = 90,
              .surfaces = {},
              .components = Cc::NodeComponentBit::Scheduler | Cc::NodeComponentBit::Consensus,
              .runtime = { .schedulerRole = Cc::WireSchedulerRole::Leader,
                           .enrollment = Cc::WireEnrollmentState::Closed,
                           .enrollmentPending = 0 } }) } };

        auto const answer = RunNodeVerb("node", node);
        CHECK(RequiredCell(answer, "enrollment").lexical == "closed");

        // Zero pending is a READING on a node that has a window, so it is a number
        // rather than a missing field -- the counters' zero is what cannot say this.
        CHECK(RequiredCell(answer, "enrollment-pending").lexical == "0");
    }

    SECTION("a node that runs no consensus says NOTHING rather than a reassuring closed")
    {
        ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                    .nodeId = "node-a",
                                                    .uptimeSeconds = 5,
                                                    .surfaces = {},
                                                    .components = Cc::NodeComponentBit::Worker,
                                                    .runtime = { .toolchains = Cc::ToolchainState::Serving,
                                                                 .toolchainsServed = 1,
                                                                 .toolchainsDiscovered = 1 } }) } };

        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(CellOf(answer, "enrollment") == nullptr);
        CHECK(CellOf(answer, "enrollment-pending") == nullptr);
    }
}

TEST_CASE("`node` reports a component bit this client has no name for", "[cli][node][verbs]")
{
    // An older client meeting a newer node must say *there is something here I do not
    // know about* rather than under-reporting what the node runs. The known bits are
    // still named, which is what separates this from *give up and print the mask*.
    constexpr std::uint32_t Unknown = 0b1000'0000;
    ScriptedNodeExchange node { { StatusReply({ .version = "9.9.9",
                                                .nodeId = "node-a",
                                                .uptimeSeconds = 1,
                                                .surfaces = {},
                                                .components = Cc::NodeComponentBit::Worker | Unknown }) } };

    auto const answer = RunNodeVerb("node", node);
    CHECK(RequiredCell(answer, "components").lexical.contains("worker"));
    CHECK(RequiredCell(answer, "components").lexical.contains("unknown(0x80)"));
}

TEST_CASE("`node-metrics` reports every figure the node's reading carries", "[cli][node][verbs]")
{
    // #1406: the cache tier's figures beside the counters. A node answering the counter catalogue
    // alone would satisfy the counter and the zero, and fail the storage rows.
    ScriptedNodeExchange node { { NodeMetricsReply(12, StorageStats { .itemCount = 3, .deleteHits = 2 }) } };

    auto const answer = RunNodeVerb("node-metrics", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(node.Sent().size() == 1);
    CHECK(OpOf(node.Sent()[0]) == static_cast<std::uint8_t>(Cc::Op::NodeMetrics));

    CHECK(RequiredCell(answer, "fastcache_worker_jobs_completed_total").lexical == "12");
    CHECK(RequiredCell(answer, "fastcached_items").lexical == "3");
    CHECK(RequiredCell(answer, "fastcached_delete_hits_total").lexical == "2");

    // The zero row is PRESENT, which is the property a reading of only the non-zero
    // counter cannot see.
    CHECK(RequiredCell(answer, "fastcache_worker_jobs_started_total").lexical == "0");

    // And every catalogue counter is a field named by its `prometheusName`, which is what this verb's
    // counters were named before it carried a reading: a script reading `--format=kv` keys (the
    // cluster end-to-end fixture does) keeps its keys, and gains the tier's beside them.
    for (auto const& row: CounterTable)
    {
        INFO(row.prometheusName);
        CHECK(CellOf(answer, row.prometheusName) != nullptr);
    }
}

TEST_CASE("`node-metrics` against a node of the previous wire version is refused by that node, by name",
          "[cli][node][verbs]")
{
    // What a version-9 node really answers (#1406). A reply header carries no version, so this client
    // never meets a version-9 BODY: the node's request check refuses the version-10 frame first, as
    // `UnsupportedVersion` naming its range. That refusal is the realistic old-build case, and it
    // must reach the operator as a refusal that names both versions -- not as an unreadable reading.
    ScriptedNodeExchange node { { RefusalReply(Cc::ErrorCode::UnsupportedVersion,
                                               "unsupported wire version 10; this server speaks 9..9") } };

    auto const answer = RunNodeVerb("node-metrics", node);
    REQUIRE(node.Sent().size() == 1);
    CHECK(static_cast<std::uint8_t>(node.Sent()[0][1]) == Cc::CurrentVersion);
    CHECK(answer.outcome == Outcome::Refused);
    REQUIRE(answer.advisories.size() == 1);
    CHECK(answer.advisories[0].contains("this server speaks 9..9"));
    CHECK_FALSE(answer.advisories[0].contains("reading"));
}

TEST_CASE("A node verb's refusal is reported by name and exits `refused`", "[cli][node][verbs]")
{
    // **`Refused`, never `Unreachable`.** The node answered -- a script that retries on
    // an unreachable endpoint and gives up on a refusal cannot be written if the two
    // share a code, which is the pair `OutcomeTable` exists to keep apart.
    SECTION("a non-member is told which gate refused it")
    {
        ScriptedNodeExchange node { { RefusalReply(Cc::ErrorCode::NotAMember,
                                                   "this node reports its identity and counters to fleet members only") } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Refused);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0].contains("not-a-member"));
        CHECK(answer.advisories[0].contains("fleet members only"));
    }

    SECTION("an endpoint that does not implement the verb says so, and names itself")
    {
        // The `fastcached` daemon's answer: it speaks `0xFC` and serves the cache verbs,
        // and has no node component at all. This is the sentence that replaces *the
        // server closed the connection without answering*.
        ScriptedNodeExchange node { { RefusalReply(Cc::UnimplementedVerb) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Refused);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0].contains("does not implement"));
        CHECK(answer.advisories[0].contains("10.0.0.7:6674"));
    }

    SECTION("and served-elsewhere is NOT reported as an old endpoint")
    {
        ScriptedNodeExchange node { { RefusalReply(Cc::ErrorCode::DispatchNotPermitted) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Refused);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0].contains("another endpoint"));
        CHECK_FALSE(answer.advisories[0].contains("does not implement"));
    }
}

TEST_CASE("A node verb separates *nothing answered* from *the answer was unreadable*", "[cli][node][verbs]")
{
    SECTION("an unreachable node exits `unreachable`")
    {
        ScriptedNodeExchange node { { NodeFailure(ExchangeFailure::Unreachable, "cannot reach 10.0.0.7:6674") } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Unreachable);
    }

    SECTION("a body this client cannot read exits `protocol`, not `refused`")
    {
        // Different people fix those: a refusal is a configuration somebody changes, and
        // an unreadable body is a version mismatch between two ends that agree on the
        // framing. Built as a well-framed reply with a body that is not a node status,
        // so only the BODY decode fails -- a garbage frame would fail one step earlier
        // and prove something else.
        ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok,
                                                      Cc::AsBytes(std::string_view { "not a node status" })) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Protocol);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0].contains("cannot read"));
    }

    SECTION("and node-metrics does the same with its own body, naming why the reading would not decode")
    {
        // A well-framed reply whose reading another build laid out: the digest the encoding starts
        // with differs, so only the reading decode fails.
        auto body = EncodeStatsReading(StatsReading {});
        body[7] ^= std::byte { 0x01 };
        ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, body) } };
        auto const answer = RunNodeVerb("node-metrics", node);
        CHECK(answer.outcome == Outcome::Protocol);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0].contains("answered node-metrics with a reading that is laid out by a build other"));
    }
}

TEST_CASE("A node verb with no 0xFC connection is refused by name rather than crashing", "[cli][node][verbs]")
{
    // `RunVerb` is the one place a null collaborator becomes a refusal, and the wire row
    // owns the wording. Asserted through the real table so a row that forgot its
    // `available` column would fail here rather than dereference a null in production.
    auto const* const verb = FindVerb("node");
    REQUIRE(verb != nullptr);
    auto const answer = RunVerb(*verb, VerbContext {});
    CHECK(answer.outcome == Outcome::Unreachable);
    REQUIRE(answer.advisories.size() == 1);
    CHECK(answer.advisories[0].contains("0xFC"));
}

TEST_CASE("Every node verb declares the 0xFC wire and needs only that connection", "[cli][node][verbs]")
{
    // The columns `main` reads to decide which sockets to open. Asserted from the table
    // rather than from `main`, which is in no test target -- a row claiming it needs RESP
    // would make every node verb dial a port a compile node does not serve, and the
    // symptom would be an unreachable diagnostic against a node that was working.
    std::vector<VerbSpec const*> nodeVerbs;
    for (auto const& verb: Verbs())
        if (verb.wire == Wire::Node)
            nodeVerbs.push_back(&verb);

    // The positive half, and it is not decoration: every `CHECK` in the loop below is
    // vacuous over an empty set, so a renamed enumerator would leave this case green and
    // testing nothing.
    //
    // NOT a count. It was `== 2`, which is a hand-kept census of a DERIVED set and is a
    // second claim about the verb table rather than a check on it -- it went red the
    // moment the cluster verbs arrived, having found nothing wrong. Non-emptiness is the
    // whole of what the vacuity argument needs.
    CHECK_FALSE(nodeVerbs.empty());

    auto const& row = WireTable[static_cast<std::size_t>(Wire::Node)];
    CHECK(row.needsNode);
    CHECK_FALSE(row.needsResp);
    CHECK_FALSE(row.needsMemcached);

    // **`authenticable` is TRUE here and false on the memcached wire**, which is the
    // column doing real work: `AUTH` is a `0xFC` verb, so a refusal about a credential on
    // this wire is about the credential and is worth reporting as such.
    CHECK(row.authenticable);

    for (auto const* const verb: nodeVerbs)
    {
        INFO("verb: " << verb->name);

        // `--ttl`, `--nx`, `--xx`, `--raw` and `--all` are keyspace concepts and a node
        // holds no keyspace, so a node verb honouring one would be a flag the parser
        // accepts and the handler cannot act on. Those bits and no others: `fleet` honours
        // `--range`, which is the leader's window rather than a key (#1390).
        CHECK((verb->modifiers & (Modifier::Ttl | Modifier::Exclusivity | Modifier::Raw | Modifier::Everything)) == 0);

        // Every node verb names the `0xFC` verb it sends. It is what the per-verb help
        // renders and what a reader matches against `CompileCacheWire`'s own table.
        CHECK_FALSE(verb->protocolCommand.empty());

        // The arity was pinned at `0, 0` here, which was true of the two verbs that
        // existed and was never a property of the wire: `cluster-set` takes a name and a
        // value. What IS a property is that the bounds describe a range -- a row whose
        // minimum exceeds its maximum accepts nothing at all, and `OperandCountAccepted`
        // would refuse every invocation of it with a message naming an arity no operand
        // count can satisfy.
        CHECK(verb->minOperands <= verb->maxOperands);
    }
}

TEST_CASE("`version` is ANSWERED by a compile node rather than merely explained", "[cli][node][fallback]")
{
    // **The verb the whole fallback exists for.** `fastcache-compile-node` speaks no
    // RESP, so `version` reported *the server closed the connection without answering*
    // against the one binary an operator most often points this tool at.
    ScriptedNodeExchange node { { StatusReply({ .version = "0.2.0-125-g6ba32b30",
                                                .nodeId = "node-a",
                                                .uptimeSeconds = 9,
                                                .surfaces = {},
                                                .components = Cc::NodeComponentBit::Worker }) } };

    auto const* const verb = FindVerb("version");
    REQUIRE(verb != nullptr);

    // What the RESP wire concluded, which is what `main` hands over.
    auto const primary = Concluded(Outcome::Unreachable, "the server closed the connection without answering");

    auto const answer = RunNodeFallback(*verb, VerbContext { .node = &node }, RemoteKind::CompileNode, primary);

    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(answer.value.shape == Shape::Record);
    CHECK(RequiredCell(answer, "server").lexical == "0.2.0-125-g6ba32b30");
    CHECK(CellOf(answer, "client") != nullptr);

    // WHICH kind of server, so two version strings are not read as two builds of one
    // binary: a node and a daemon version alike and are different programs.
    CHECK(RequiredCell(answer, "server_kind").lexical == "fastcache-compile-node");

    // The primary failure is GONE rather than carried alongside: the command succeeded,
    // and a leftover *could not be read* beside an answer reads as a partial failure.
    CHECK(answer.advisories.empty());
}

TEST_CASE("A verb with no 0xFC equivalent is explained, never retried", "[cli][node][fallback]")
{
    // A compile node holds no user keyspace, so `get` has no `0xFC` equivalent and never
    // will. The honest answer is a refusal naming what the endpoint IS -- a second
    // attempt that cannot work would spend a round trip to produce a worse sentence.
    ScriptedNodeExchange node { {} };
    auto const* const verb = FindVerb("get");
    REQUIRE(verb != nullptr);
    REQUIRE(verb->nodeFallback == nullptr);

    auto const primary = Concluded(Outcome::Unreachable, "the server closed the connection without answering");
    auto const answer = RunNodeFallback(*verb, VerbContext { .node = &node }, RemoteKind::CompileNode, primary);

    // Nothing was sent. The assertion that matters: an implementation that "helpfully"
    // asked the node something would pass every check on the text below.
    CHECK(node.Sent().empty());

    // The command still failed, and the outcome still says so -- but it says WHICH
    // failure. `Unreachable` means *the server could not be reached*, which this probe
    // has just disproved: a frame came back. The identification above is the operator's
    // half of that correction and this is the script's, and the exit code is the half
    // this tool publishes as a contract.
    //
    // This case's own first paragraph already called the honest answer "a refusal naming
    // what the endpoint IS"; the assertion used to pin `Unreachable` beside it, which is
    // the prose and the check disagreeing inside one test.
    CHECK(answer.outcome == Outcome::Refused);
    REQUIRE(answer.advisories.size() == 2);

    // The identification goes FIRST: an operator reading downwards wants *this is a
    // compile node* before *the server closed the connection*, which is the consequence
    // rather than the cause.
    CHECK(answer.advisories[0].contains("fastcache-compile-node"));
    CHECK(answer.advisories[1] == "the server closed the connection without answering");
}

TEST_CASE("A fallback is not run against an endpoint that is not a node", "[cli][node][fallback]")
{
    // `version` HAS a fallback, and it must not be asked a question the endpoint cannot
    // hear. This is the arm a `verb.nodeFallback != nullptr` check alone gets wrong, and
    // the failure would be a second timeout on an already-failed command.
    ScriptedNodeExchange node { {} };
    auto const* const verb = FindVerb("version");
    REQUIRE(verb != nullptr);
    REQUIRE(verb->nodeFallback != nullptr);

    auto const primary = Concluded(Outcome::Unreachable, "the server closed the connection without answering");

    SECTION("a 0xFC-only endpoint is explained, and nothing is sent")
    {
        auto const answer = RunNodeFallback(*verb, VerbContext { .node = &node }, RemoteKind::FastcacheWireOnly, primary);
        CHECK(node.Sent().empty());

        // Reached and answered, so `Unreachable` is corrected here exactly as it is for a
        // node -- the endpoint framed a refusal, which is the proof, and WHICH refusal
        // deliberately narrows it no further.
        CHECK(answer.outcome == Outcome::Refused);
        REQUIRE(answer.advisories.size() == 2);
        CHECK(answer.advisories[0].contains("0xFC"));
    }

    SECTION("and an endpoint that is not this protocol leaves the answer UNCHANGED")
    {
        // The probe explained nothing the caller does not already know, so nothing is
        // added: one fault must not read as two.
        auto const answer = RunNodeFallback(*verb, VerbContext { .node = &node }, RemoteKind::NotFastcacheWire, primary);
        CHECK(node.Sent().empty());

        // **The CONTROL for the two cases above**, and the reason they mean anything: no
        // frame came back here, so nothing was established and the outcome is left where
        // the transport put it.
        //
        // What it catches is an implementation that stamps the outcome BEFORE asking
        // whether the probe explained anything. What it does NOT catch -- measured, not
        // assumed -- is a `RemoteKindTable` that answers `Refused` for every kind: this
        // path returns early on the empty explanation and never reaches the table, so
        // that mutation leaves this case green. `EstablishedBy`'s own control in
        // `NodeClient_test.cpp` is what covers the table, and it is not redundant with
        // this one.
        CHECK(answer.outcome == Outcome::Unreachable);
        REQUIRE(answer.advisories.size() == 1);
        CHECK(answer.advisories[0] == "the server closed the connection without answering");
    }
}

TEST_CASE("A fallback that cannot reach the node still reports the client half", "[cli][node][fallback]")
{
    // The client version is knowable without a server, so throwing it away because the
    // server half failed would discard the half that answered -- which is the rule the
    // RESP `version` handler already holds, now held on this wire too.
    ScriptedNodeExchange node { { NodeFailure(ExchangeFailure::Transport, "the connection failed") } };
    auto const* const verb = FindVerb("version");
    REQUIRE(verb != nullptr);

    auto const answer = RunNodeFallback(*verb,
                                        VerbContext { .node = &node },
                                        RemoteKind::CompileNode,
                                        Concluded(Outcome::Unreachable, "whatever RESP said"));

    CHECK(answer.outcome == Outcome::Unreachable);
    REQUIRE(answer.value.shape == Shape::Record);
    CHECK(CellOf(answer, "client") != nullptr);

    // **Present, carrying an Absent KIND** -- which is a different claim from the field
    // being missing, and the two must not collapse into one. `RequiredCell` keeps them
    // apart: it fails the case when there is no cell at all, so what this line asserts is
    // the kind of a cell that is there.
    CHECK(RequiredCell(answer, "server").kind == CellKind::Absent);
}

TEST_CASE("Exactly two verbs carry a 0xFC fallback today, `del` and `version`", "[cli][node][fallback]")
{
    // A census, and it is the kind that has to state its PATTERN: a row gaining a
    // fallback is a decision about whether a node can answer that QUESTION, and one
    // added without a test is a verb that silently starts asking a node about a keyspace
    // it does not have. `del` is the second, and its question is one a node CAN answer --
    // its cache tier holds keys an operator may have to remove (#1276) -- where `get`'s is
    // a user keyspace no node has.
    std::vector<std::string_view> withFallback;
    for (auto const& verb: Verbs())
        if (verb.nodeFallback != nullptr)
            withFallback.push_back(verb.name);
    std::ranges::sort(withFallback);

    REQUIRE(withFallback.size() == 2);
    CHECK(withFallback[0] == "del");
    CHECK(withFallback[1] == "version");

    // And the control that says the census could have found more: every node verb was
    // examined, so an empty `Verbs()` would fail here rather than passing vacuously.
    CHECK(Verbs().size() > 20);
}

// ---------------------------------------------------------------------------------
// `del` against a compile node (#1276)
// ---------------------------------------------------------------------------------

namespace
{

/// Run `del` with @p keys as a node fallback, the way `main` reaches it after RESP failed.
/// @param node The scripted node.
/// @param keys The operands.
/// @return The answer.
[[nodiscard]] Answer DelOnNode(ScriptedNodeExchange& node, std::vector<std::string> const& keys)
{
    auto const* const verb = FindVerb("del");
    REQUIRE(verb != nullptr);
    return RunNodeFallback(*verb,
                           VerbContext { .operands = keys, .node = &node },
                           RemoteKind::CompileNode,
                           Concluded(Outcome::Unreachable, "the server closed the connection without answering"));
}

/// Whether any advisory contains @p needle.
[[nodiscard]] bool Advises(Answer const& answer, std::string_view needle)
{
    return std::ranges::any_of(answer.advisories, [needle](std::string const& line) { return line.contains(needle); });
}

} // namespace

TEST_CASE("`del` on a node sends one cache-drop per key and counts what was removed", "[cli][node][fallback][cache-drop]")
{
    ScriptedNodeExchange node {
        { Cc::EncodeReply(Cc::Status::Ok, {}), Cc::EncodeReply(Cc::Status::Miss, {}), Cc::EncodeReply(Cc::Status::Ok, {}) }
    };

    auto const answer = DelOnNode(node, { "a", "b", "c" });

    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(answer.value.shape == Shape::Scalar);
    CHECK(answer.value.scalar.lexical == "2");

    // What went out: three frames, each the drop verb, each naming its key in order.
    constexpr std::array<std::string_view, 3> Keys { "a", "b", "c" };
    REQUIRE(node.Sent().size() == Keys.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, Keys.size()))
    {
        auto const& sent = node.Sent()[index];
        auto const key = Keys[index];
        CHECK(OpOf(sent) == static_cast<std::uint8_t>(Cc::Op::CacheDrop));
        auto const payload = Cc::DecodeCacheDropPayload(std::span<std::byte const> { sent }.subspan(Cc::RequestHeaderSize));
        REQUIRE(payload.has_value());
        CHECK(Cc::AsStringView(Unwrap(payload)) == key);
    }

    // The reach is said, because it surprises: a shared cache behind the node refills it.
    CHECK(Advises(answer, "own cache tier only"));
    CHECK(Advises(answer, "--upstream"));
}

TEST_CASE("`del` on a node that holds none of the keys answers no, which is not a refusal",
          "[cli][node][fallback][cache-drop]")
{
    // A repair's second run. `Negative` exits 1 -- an ANSWER -- where a refusal exits 4, and
    // an operator scripting the repair must be able to tell the two apart.
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Miss, {}), Cc::EncodeReply(Cc::Status::Miss, {}) } };

    auto const answer = DelOnNode(node, { "a", "b" });

    CHECK(answer.outcome == Outcome::Negative);
    CHECK(answer.value.scalar.lexical == "0");
    CHECK(Advises(answer, "no key matched"));
    CHECK(node.Sent().size() == 2);
}

TEST_CASE("`del` on a node stops at a refusal, names it, and says how far it got", "[cli][node][fallback][cache-drop]")
{
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, {}),
                                  RefusalReply(Cc::ErrorCode::NotAMember,
                                               "this node serves its cache to its own machine only"),
                                  Cc::EncodeReply(Cc::Status::Ok, {}) } };

    auto const answer = DelOnNode(node, { "a", "b", "c" });

    CHECK(answer.outcome == Outcome::Refused);
    // The node's own sentence, so the operator learns it is WHERE they ran it from.
    CHECK(Advises(answer, "own machine only"));
    CHECK(Advises(answer, "1 of 3 key(s) were removed before `b`"));
    // And the third key was never sent: a node refusing this caller refuses every key.
    CHECK(node.Sent().size() == 2);
}

TEST_CASE("`del` on a node too old to know the verb says so rather than miscounting", "[cli][node][fallback][cache-drop]")
{
    // A node built before #1276 answers `UnimplementedVerb`. Counted as a miss it would print
    // `0` and exit 1 -- *nothing matched* -- about keys that are all still there.
    ScriptedNodeExchange node { { RefusalReply(Cc::UnimplementedVerb, "unknown opcode 0x15") } };

    auto const answer = DelOnNode(node, { "a" });

    CHECK(answer.outcome == Outcome::Refused);
    CHECK(Advises(answer, "does not implement `del`"));
}

// ---------------------------------------------------------------------------------
// The cluster verbs
// ---------------------------------------------------------------------------------

namespace
{

/// A framed `ClusterStatus` reply carrying @p state.
/// @param state What the cluster has agreed.
/// @return The reply frame.
[[nodiscard]] std::vector<std::byte> ClusterStatusReply(Cluster::ClusterState const& state)
{
    return Cc::EncodeReply(Cc::Status::Ok, Cluster::Encode(state));
}

/// The rows of a table answer.
/// @param answer The answer.
/// @return Its rows; empty when it is not a table.
[[nodiscard]] std::vector<std::vector<Cell>> const& RowsOf(Answer const& answer)
{
    REQUIRE(answer.value.shape == Shape::Table);
    return answer.value.rows;
}

/// The index of @p column in a table answer.
/// @param answer The answer.
/// @param column The column name.
/// @return Its index.
[[nodiscard]] std::size_t ColumnOf(Answer const& answer, std::string_view column)
{
    REQUIRE(answer.value.shape == Shape::Table);
    auto const at = std::ranges::find(answer.value.columns, column);
    INFO("column: " << column);
    REQUIRE(at != answer.value.columns.end());
    return static_cast<std::size_t>(std::ranges::distance(answer.value.columns.begin(), at));
}

} // namespace

TEST_CASE("cluster-members reports who the cluster agreed on, and an unled member as ABSENT", "[cli][node][cluster]")
{
    // The two states a `schedulerEndpoint` has, side by side in one reply. A member that
    // has never led carries none -- a leader announces its own record on election -- so
    // the empty string is the ORDINARY case and rendering it as a value would hand an
    // operator an address to paste that reaches nothing. Built through `Apply`, which is how a
    // leader acquires the state this reply carries: a member literal with an endpoint and no
    // announcement recorded is a state `DecodeState` refuses, because `Apply` never makes it.
    Cluster::ClusterState state;
    Apply(state,
          Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                             .key = "node-a",
                             .value = "10.0.0.7:6675",
                             .schedulerEndpoint = "10.0.0.7:6674" });
    Apply(state,
          Cluster::Command {
              .kind = Cluster::CommandKind::AddMember, .key = "node-b", .value = "10.0.0.8:6675", .schedulerEndpoint = {} });
    ScriptedNodeExchange node { { ClusterStatusReply(state) } };

    auto const answer = RunNodeVerb("cluster-members", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    auto const& rows = RowsOf(answer);
    REQUIRE(rows.size() == 2);

    auto const id = ColumnOf(answer, "id");
    auto const raft = ColumnOf(answer, "raft");
    auto const scheduler = ColumnOf(answer, "scheduler");

    CHECK(rows[0][id].lexical == "node-a");
    CHECK(rows[0][raft].lexical == "10.0.0.7:6675");
    CHECK_FALSE(rows[0][scheduler].kind == CellKind::Absent);
    CHECK(rows[0][scheduler].lexical == "10.0.0.7:6674");

    // The discrimination. Asserting only that the cell is empty would pass under a
    // renderer that dropped the absent marker, which is the bug worth catching: an empty
    // TSV field collapses under `IFS=$'\t' read` and shifts every field after it.
    CHECK(rows[1][id].lexical == "node-b");
    CHECK(rows[1][scheduler].kind == CellKind::Absent);
}

TEST_CASE("cluster-members says which absence a scheduler endpoint is: never announced or cleared", "[cli][node][cluster]")
{
    // #1340. Both members' `scheduler` cell is ABSENT, and must stay so -- only one of
    // them ever had an address, and a re-admit applied wholesale took it. So the
    // discrimination is in `scheduler-state`, asserted on BOTH rows: a case rendering one
    // of them passes on a build where both read alike. Built through `Apply`, which is how
    // a leader acquires the state this reply carries.
    Cluster::ClusterState state;
    auto const admit = [&state](std::string id, std::string raft, std::string scheduler) {
        Apply(state,
              Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                 .key = std::move(id),
                                 .value = std::move(raft),
                                 .schedulerEndpoint = std::move(scheduler) });
    };
    admit("node-b", "10.0.0.8:6675", {});
    admit("node-c", "10.0.0.9:6675", "10.0.0.9:6674");
    admit("node-c", "10.0.0.9:6675", {});

    ScriptedNodeExchange node { { ClusterStatusReply(state) } };
    auto const answer = RunNodeVerb("cluster-members", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    auto const& rows = RowsOf(answer);
    REQUIRE(rows.size() == 2);
    auto const id = ColumnOf(answer, "id");
    auto const scheduler = ColumnOf(answer, "scheduler");
    auto const schedulerState = ColumnOf(answer, "scheduler-state");

    CHECK(rows[0][id].lexical == "node-b");
    CHECK(rows[1][id].lexical == "node-c");
    CHECK(rows[0][scheduler].kind == CellKind::Absent);
    CHECK(rows[1][scheduler].kind == CellKind::Absent);
    CHECK(rows[0][schedulerState].lexical == "never-announced");
    CHECK(rows[1][schedulerState].lexical == "cleared");
}

TEST_CASE("cluster-settings names every key this build knows, set or not", "[cli][node][cluster]")
{
    // An operator's real question is usually *what CAN I set*, and a report listing only
    // what somebody already set answers it wrongly by omission.
    REQUIRE_FALSE(Cluster::SettingTable.empty());
    auto const known = std::string { Cluster::SettingTable[0].name };

    ScriptedNodeExchange node { { ClusterStatusReply({ .members = {}, .settings = {} }) } };

    auto const answer = RunNodeVerb("cluster-settings", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    auto const& rows = RowsOf(answer);
    auto const name = ColumnOf(answer, "name");
    auto const value = ColumnOf(answer, "value");

    // Every row of the build's table is present although the cluster agreed nothing.
    CHECK(rows.size() >= Cluster::SettingTable.size());
    auto const row = std::ranges::find(rows, known, [name](auto const& r) { return r[name].lexical; });
    REQUIRE(row != rows.end());

    // ABSENT, not empty: *the cluster has agreed nothing for this* is a different fact
    // from a setting whose agreed value happens to be the empty string.
    CHECK((*row)[value].kind == CellKind::Absent);
}

TEST_CASE("cluster-settings keeps a setting this build does not know", "[cli][node][cluster]")
{
    // A fleet is permanently mid-upgrade, so the leader may have agreed a key this
    // client's table has never heard of. Dropping the row would hide a live fact
    // because the READER is the older binary -- and the operator would be told the
    // cluster agrees something it does not.
    ScriptedNodeExchange node { { ClusterStatusReply(
        { .members = {}, .settings = { { .name = "a-key-from-a-newer-build", .value = "7" } } }) } };

    auto const answer = RunNodeVerb("cluster-settings", node);
    CHECK(answer.outcome == Outcome::Affirmative);

    auto const& rows = RowsOf(answer);
    auto const name = ColumnOf(answer, "name");
    auto const value = ColumnOf(answer, "value");
    auto const summary = ColumnOf(answer, "summary");

    auto const row = std::ranges::find(rows, std::string_view { "a-key-from-a-newer-build" }, [name](auto const& r) {
        return std::string_view { r[name].lexical };
    });
    REQUIRE(row != rows.end());
    CHECK((*row)[value].lexical == "7");
    // No summary, because this build genuinely has none. Absent rather than invented.
    CHECK((*row)[summary].kind == CellKind::Absent);
}

TEST_CASE("a cluster change reports ACCEPTED, never committed", "[cli][node][cluster]")
{
    // The leader cannot know whether a change committed until a majority answers, so a
    // tool claiming it did is the one thing a report like this must not do.
    //
    // TWO verbs, not three. `cluster-admit` used to share this loop and now answers with
    // a receipt (#1296), so it is a case of its own below rather than an omission here:
    // a verb quietly dropped from a loop is a verb nothing asserts about.
    for (auto const& spec:
         { std::pair { std::string_view { "cluster-set" }, std::vector<std::string> { "fleet-open", "1" } },
           std::pair { std::string_view { "cluster-forget" }, std::vector<std::string> { "node-b" } } })
    {
        INFO("verb: " << spec.first);
        // A typed empty payload: `{}` is ambiguous against `std::span`, and these two
        // verbs are acknowledged with a reply that carries no body at all.
        std::vector<std::byte> const noPayload;
        ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, noPayload) } };
        auto const answer = RunNodeVerb(spec.first, node, spec.second);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(RequiredCell(answer, "state").lexical == "replicating");
    }
}

TEST_CASE("cluster-admit reports what the leader RECORDED, never what is in force", "[cli][node][cluster]")
{
    // #1296. The address typed here and the one the joining node answers consensus on
    // are two spellings of one address that nothing compared; when they disagree the
    // member is in the configuration and contacts nobody, which presents as an election
    // storm rather than as a typo. So the leader's half is printed.
    //
    // What it RECORDED it knows instantly and alone; whether a majority has taken it, it
    // cannot know. The field names carry that distinction, so they are what this asserts.
    auto const receipt =
        Cc::EncodeClusterAdmitReceipt(Cc::ClusterAdmitReceipt { .memberId = "node-c", .raftEndpoint = "10.0.0.9:6675" });
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, receipt) } };

    auto const answer = RunNodeVerb("cluster-admit", node, { "node-c", "10.0.0.9:6675" });
    REQUIRE(answer.outcome == Outcome::Affirmative);

    // Distinct values, neither a substring of the other, so a transposed pair reddens
    // rather than agreeing with itself.
    CHECK(RequiredCell(answer, "member-id-as-received").lexical == "node-c");
    CHECK(RequiredCell(answer, std::format("{}-as-recorded", Cc::ConsensusEndpointField)).lexical == "10.0.0.9:6675");

    // The ceiling, asserted on the WORD: `SchedulerService::Offer`'s own phrase, not a
    // second spelling of one state. Anything stronger here would be the confident wrong
    // signal the whole change exists to avoid.
    CHECK(RequiredCell(answer, "state").lexical == "appended, not committed");
    CHECK(RequiredCell(answer, "state").lexical != "replicating");
}

TEST_CASE("cluster-admit refuses a reply whose receipt it cannot read", "[cli][node][cluster]")
{
    // Not rendered with blank cells, which is the missing string this change exists to
    // prevent arriving through the renderer -- an operator comparing two empty columns
    // finds them equal and stops looking.
    //
    // The empty body is exactly what the other two verbs are acknowledged with, which is
    // what makes this the discriminating arrangement rather than a malformed-bytes one.
    std::vector<std::byte> const noPayload;
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, noPayload) } };

    auto const answer = RunNodeVerb("cluster-admit", node, { "node-c", "10.0.0.9:6675" });
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(AdvisoryText(answer).contains("cannot read"));
}

TEST_CASE("NotLeader is followed to the endpoint it names, and separated from an election", "[cli][node][cluster]")
{
    // ONE code, TWO opposite facts, and only *does this message parse as an address*
    // separates them -- an empty message never reaches the wire, being replaced by the
    // error table's default sentence, so testing for empty gets neither.
    SECTION("a message that parses names where to ask instead")
    {
        ScriptedNodeExchange node { { RefusalReply(Cc::ErrorCode::NotLeader, "10.0.0.9:6674") } };
        auto const answer = RunNodeVerb("cluster-members", node);
        CHECK(answer.outcome == Outcome::Refused);
        CHECK(AdvisoryText(answer).contains("10.0.0.9:6674"));
        CHECK(AdvisoryText(answer).contains("does not lead"));
    }

    SECTION("a message that does not parse is an election, and offers no address")
    {
        // **Splitting is not parsing.** `SplitHostPort` takes the LAST colon, so this
        // sentence splits into a host and a port of ` try again` -- and a client that
        // dialled it would spend a hop the real leader never hears. The assertion is
        // that no address is offered, which is what separates the two arms; asserting
        // only that the diagnostic mentions the leader passes under both.
        ScriptedNodeExchange node { { RefusalReply(Cc::ErrorCode::NotLeader, "no leader: try again") } };
        auto const answer = RunNodeVerb("cluster-members", node);
        CHECK(answer.outcome == Outcome::Refused);
        CHECK(AdvisoryText(answer).contains("no leader is known"));
        CHECK_FALSE(AdvisoryText(answer).contains("ask "));
    }
}

TEST_CASE("a cluster reply this build cannot read is REFUSED, not rendered as an empty cluster", "[cli][node][cluster]")
{
    // A partial read looks exactly like a fleet that admits nobody, and that would be
    // read as a fact rather than as a failure to decode.
    std::vector<std::byte> const notAClusterState { std::byte { 0xFF }, std::byte { 0xFE } };
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, notAClusterState) } };

    auto const answer = RunNodeVerb("cluster-members", node);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(AdvisoryText(answer).contains("cannot read"));
}

TEST_CASE("a cluster reply another build encoded is refused by its version", "[cli][node][cluster]")
{
    // The same refusal as above, for the cause an upgrade produces -- and it says so,
    // because *cannot read* alone fits a damaged body too and the two send an operator
    // to different machines.
    auto body = Cluster::Encode(Cluster::ClusterState { .members = {}, .settings = {} });
    // The state's version is the first field's only byte, after its u32 length prefix.
    REQUIRE(body.size() > 4);
    body[4] = std::byte { 2 };
    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, body) } };

    auto const answer = RunNodeVerb("cluster-members", node);
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(AdvisoryText(answer).contains("cannot read"));
    CHECK(AdvisoryText(answer).contains("version 2"));
}

TEST_CASE("the cluster verbs send the opcodes the wire table names", "[cli][node][cluster]")
{
    // The bytes, not only the outcome: a verb that parsed, dialled and sent the WRONG
    // opcode is answered by the leader and does something else entirely -- and every
    // assertion about the rendered answer passes, because the reply is scripted.
    struct Expectation
    {
        std::string_view verb;
        std::vector<std::string> operands;
        Cc::Op op;
    };

    for (auto const& expectation:
         { Expectation { .verb = "cluster-members", .operands = {}, .op = Cc::Op::ClusterStatus },
           Expectation { .verb = "cluster-settings", .operands = {}, .op = Cc::Op::ClusterStatus },
           Expectation { .verb = "cluster-set", .operands = { "fleet-open", "1" }, .op = Cc::Op::ClusterSet },
           Expectation { .verb = "cluster-forget", .operands = { "node-b" }, .op = Cc::Op::ClusterForget },
           Expectation { .verb = "cluster-admit", .operands = { "node-c", "10.0.0.9:6675" }, .op = Cc::Op::ClusterAdmit } })
    {
        INFO("verb: " << expectation.verb);
        ScriptedNodeExchange node { { ClusterStatusReply({ .members = {}, .settings = {} }) } };
        (void) RunNodeVerb(expectation.verb, node, expectation.operands);

        REQUIRE(node.Sent().size() == 1);
        auto const header = Cc::DecodeRequestHeader(node.Sent()[0]);
        REQUIRE(header.has_value());
        // `opRaw` and not an `Op`: the header decoder deliberately hands back the BYTE,
        // unvalidated against `OpTable`, so a verb this build does not carry is still
        // readable. Compared against the enumerator's value, which pins the byte as well
        // as the name -- a wire constant has two facts and a symbol both ends spell can
        // only test the first.
        CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(expectation.op));
    }
}

// ---------------------------------------------------------------------------
// #1300, #1391: `fleet` relays one of the leader's tables into a terminal, read over 0xFC.
// ---------------------------------------------------------------------------

namespace
{

/// A node connection a dialler hands out while the case keeps the scripted one it forwards to.
///
/// `INodeDialer::Dial` gives its connection away, and a case still has to read what was sent
/// on it afterwards.
class BorrowedNode final: public INodeExchange
{
  public:
    /// @param node The scripted connection; it must outlive this.
    explicit BorrowedNode(ScriptedNodeExchange& node) noexcept:
        _node { &node }
    {
    }

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Send(std::span<std::byte const> request) override
    {
        return _node->Send(request);
    }

    [[nodiscard]] std::string_view Address() const override
    {
        return _node->Address();
    }

  private:
    ScriptedNodeExchange* _node;
};

/// A dialler answering from scripted nodes keyed by address, recording every dial.
class ScriptedDialer final: public INodeDialer
{
  public:
    /// @param nodes The nodes it can reach; each must outlive this. A dial to any other address fails.
    explicit ScriptedDialer(std::vector<ScriptedNodeExchange*> nodes):
        _nodes { std::move(nodes) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<INodeExchange>, ExchangeError> Dial(Endpoint const& endpoint) override
    {
        auto const address = EndpointText(endpoint);
        _dialled.push_back(address);
        for (auto* const node: _nodes)
            if (node->Address() == address)
                return std::make_unique<BorrowedNode>(*node);
        return std::unexpected(
            ExchangeError { .kind = ExchangeFailure::Unreachable, .detail = std::format("cannot reach {}", address) });
    }

    /// Every address dialled, in order.
    [[nodiscard]] std::vector<std::string> const& Dialled() const noexcept
    {
        return _dialled;
    }

  private:
    std::vector<ScriptedNodeExchange*> _nodes;
    std::vector<std::string> _dialled;
};

/// A leader's `Ok` carrying @p document.
[[nodiscard]] ScriptedNodeExchange::Outcome FleetDocumentReply(std::string_view document)
{
    return Cc::EncodeReply(Cc::Status::Ok, Cc::AsBytes(document));
}

/// A refusal of @p code saying @p message.
[[nodiscard]] ScriptedNodeExchange::Outcome Refusal(Cc::ErrorCode code, std::string_view message)
{
    return Cc::EncodeErrorReply(code, message);
}

/// Run `fleet` against @p node, following redirects through @p dial.
/// @param node The first node asked.
/// @param dial How a leader is reached, or null.
/// @param operands The positional arguments.
/// @param token The dashboard credential, or empty.
/// @return The answer.
[[nodiscard]] Answer RunFleet(INodeExchange& node,
                              INodeDialer* dial,
                              std::vector<std::string> const& operands,
                              std::string_view token = {})
{
    auto const* const verb = FindVerb("fleet");
    REQUIRE(verb != nullptr);
    return RunVerb(*verb, VerbContext { .operands = operands, .node = &node, .dial = dial, .dashboardToken = token });
}

/// The FLEET-TEXT request frame @p frame carries.
[[nodiscard]] Cc::FleetTextRequest FleetRequestOf(std::vector<std::byte> const& frame)
{
    REQUIRE(frame.size() >= Cc::RequestHeaderSize);
    REQUIRE(frame[2] == static_cast<std::byte>(Cc::Op::FleetText));
    auto const request = Cc::DecodeFleetTextRequest(std::span { frame }.subspan(Cc::RequestHeaderSize));
    REQUIRE(request.has_value());
    return Unwrap(request);
}

/// Whether any of @p answer's remarks contains @p needle.
[[nodiscard]] bool Remarks(Answer const& answer, std::string_view needle)
{
    return std::ranges::any_of(answer.advisories, [needle](std::string const& line) { return line.contains(needle); });
}

} // namespace

TEST_CASE("fleet asks the node for the section named and renders the leader's own columns", "[cli][node][fleet]")
{
    // The columns come off the document and are never named here: they are `FleetColumn`'s,
    // decided on the leader, and a list in this binary would be a second place to spell them.
    ScriptedNodeExchange node { { FleetDocumentReply("id\ttoolchain\tslots\nw1\tgcc-13-abcdef\t8\n") } };

    auto const answer = RunFleet(node, nullptr, { "workers" }, "s3cret");

    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(answer.advisories.empty());
    // The section travels as the word typed rather than being checked here: which sections exist is
    // the leader's table, and this binary deliberately holds no copy to disagree with it.
    REQUIRE(node.Sent().size() == 1);
    auto const request = FleetRequestOf(node.Sent()[0]);
    CHECK(request.section == "workers");
    CHECK(request.range.empty());
    CHECK(request.dashboardToken == "s3cret");

    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json });
    CHECK(rendered.contains("toolchain"));
    CHECK(rendered.contains("gcc-13-abcdef"));
}

TEST_CASE("fleet series asks for the range given and draws a gap as an absent cell", "[cli][node][fleet]")
{
    // #1390. The history, one row per bucket and one column per series. The range travels as the
    // key typed -- the leader refuses one it does not serve -- and a bucket with no reading is ABSENT
    // in every format, never a zero a chart or a spreadsheet would read as a quiet fleet.
    ScriptedNodeExchange node { { FleetDocumentReply("start\tcoverage\tbackfilled\tdispatched\n"
                                                     "1757800800\t1\tno\t4\n"
                                                     "1757801100\t0\tno\t-\n") } };

    auto const* const verb = FindVerb("fleet");
    REQUIRE(verb != nullptr);
    auto const operands = std::vector<std::string> { "series" };
    auto const answer = RunVerb(
        *verb,
        VerbContext { .operands = operands, .options = VerbOptions { .range = std::string { "7d" } }, .node = &node });

    REQUIRE(answer.outcome == Outcome::Affirmative);
    REQUIRE(node.Sent().size() == 1);
    auto const request = FleetRequestOf(node.Sent()[0]);
    CHECK(request.section == "series");
    CHECK(request.range == "7d");

    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json });
    CHECK(rendered.contains("dispatched"));
    CHECK(rendered.contains("null"));
    CHECK_FALSE(rendered.contains("\"-\""));

    SECTION("and with no range the leader's default is asked for, not a key this client chose")
    {
        ScriptedNodeExchange defaulted { { FleetDocumentReply("start\tcoverage\tbackfilled\n") } };
        (void) RunFleet(defaulted, nullptr, { "series" });
        REQUIRE(defaulted.Sent().size() == 1);
        CHECK(FleetRequestOf(defaulted.Sent()[0]).range.empty());
    }
}

TEST_CASE("fleet turns the leader's dash into a real absent cell", "[cli][node][fleet]")
{
    // Not text that happens to look absent: `--absent` and JSON `null` have to behave here as
    // they do for every other verb.
    ScriptedNodeExchange node { { FleetDocumentReply("id\tlast-picked-age\nw1\t-\n") } };

    auto const answer = RunFleet(node, nullptr, { "workers" });

    REQUIRE(answer.outcome == Outcome::Affirmative);
    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json });
    CHECK(rendered.contains("null"));
    // And the dash is GONE rather than both present.
    CHECK_FALSE(rendered.contains("\"-\""));
}

TEST_CASE("fleet keeps the escaping the leader applied", "[cli][node][fleet]")
{
    // A tab a peer put in its own display name arrives spelled, and stays spelled: this tool is a
    // RELAY rather than a second author of the leader's rule.
    ScriptedNodeExchange node { { FleetDocumentReply("endpoint\tname\n10.0.0.2:7100\tbuild\\tnode\n") } };

    auto const answer = RunFleet(node, nullptr, { "machines" });

    REQUIRE(answer.outcome == Outcome::Affirmative);
    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Tsv });

    // The cell holds a backslash and a `t`, which this client's TSV writer doubles; a build that
    // unescaped the leader's text would hold a real tab, which the writer spells as a single `\t`.
    CHECK(rendered.contains("build\\\\tnode"));
    CHECK_FALSE(rendered.contains("build\\tnode"));
    CHECK(std::ranges::count(rendered, '\t') == std::ranges::count(std::string_view { "endpoint\tname" }, '\t') * 2);
}

TEST_CASE("fleet follows a follower to the leader it names and says so on stderr alone", "[cli][node][fleet]")
{
    // #1391. A follower's registry is a fraction presented as the whole, so it answers NotLeader
    // naming the leader -- an instruction, which this verb follows rather than relays. The same
    // request goes to the leader, and the table on stdout is the leader's with nothing added.
    ScriptedNodeExchange follower { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.2:6674") }, "10.0.0.7:6674" };
    ScriptedNodeExchange leader { { FleetDocumentReply("id\tslots\nw1\t8\n") }, "10.0.0.2:6674" };
    ScriptedDialer dial { { &leader } };

    auto const answer = RunFleet(follower, &dial, { "workers" }, "s3cret");

    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(dial.Dialled() == std::vector<std::string> { "10.0.0.2:6674" });
    REQUIRE(leader.Sent().size() == 1);
    REQUIRE(follower.Sent().size() == 1);
    CHECK(leader.Sent()[0] == follower.Sent()[0]);
    CHECK(RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json }).contains("w1"));

    // One remark naming both, so an operator who pointed at a follower learns where the leader is.
    REQUIRE(answer.advisories.size() == 1);
    CHECK(answer.advisories[0].contains("10.0.0.7:6674 does not lead the fleet"));
    CHECK(answer.advisories[0].contains("10.0.0.2:6674"));
}

TEST_CASE("fleet stops following at the bound and names every node it asked", "[cli][node][fleet]")
{
    // Three nodes each naming the next as leader, the last naming the first: no leader at all.
    // Bounded by `MaxLeaderRedirects`, which the fleet subscription follows too.
    REQUIRE(MaxLeaderRedirects == 2);
    ScriptedNodeExchange first { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.2:6674") }, "10.0.0.1:6674" };
    ScriptedNodeExchange second { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.3:6674") }, "10.0.0.2:6674" };
    ScriptedNodeExchange third { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.1:6674") }, "10.0.0.3:6674" };
    ScriptedDialer dial { { &first, &second, &third } };

    auto const answer = RunFleet(first, &dial, { "workers" });

    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(dial.Dialled() == std::vector<std::string> { "10.0.0.2:6674", "10.0.0.3:6674" });
    CHECK(Remarks(answer, "followed 2 leader redirects"));
    CHECK(Remarks(answer, "10.0.0.1:6674 -> 10.0.0.2:6674 -> 10.0.0.3:6674"));
}

TEST_CASE("fleet relays a NotLeader that names nobody rather than dialling it", "[cli][node][fleet]")
{
    // An election: the refusal carries no address, which is a different fact from somebody else
    // leading, and there is nothing to follow.
    ScriptedNodeExchange node { { Refusal(Cc::ErrorCode::NotLeader, {}) } };
    ScriptedDialer dial { {} };

    auto const answer = RunFleet(node, &dial, { "workers" });

    CHECK(answer.outcome == Outcome::Refused);
    CHECK(dial.Dialled().empty());
    CHECK(Remarks(answer, "no leader is known"));

    SECTION("and a leader line that does not parse as an address is relayed in one line, never dialled")
    {
        // Splitting is not parsing: `SplitHostPort` would read `no leader: try again` as a host and a
        // port of ` try again`. The one predicate `DecideLeaderHop` asks refuses it.
        ScriptedNodeExchange unparseable { { Refusal(Cc::ErrorCode::NotLeader, "no leader: try again") } };
        ScriptedDialer none { {} };
        auto const relayed = RunFleet(unparseable, &none, { "workers" });
        CHECK(relayed.outcome == Outcome::Refused);
        CHECK(none.Dialled().empty());
        REQUIRE(relayed.advisories.size() == 1);
        CHECK(relayed.advisories[0].contains("no leader is known"));
        CHECK_FALSE(relayed.advisories[0].contains('\n'));
    }

    SECTION("and with no dialler a named leader is relayed rather than followed")
    {
        ScriptedNodeExchange follower { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.2:6674") } };
        auto const relayed = RunFleet(follower, nullptr, { "workers" });
        CHECK(relayed.outcome == Outcome::Refused);
        CHECK(Remarks(relayed, "ask 10.0.0.2:6674 instead"));
    }
}

TEST_CASE("fleet relays the leader's list of sections for a key it does not serve", "[cli][node][fleet]")
{
    // The leader's words, one key per line, as they arrived -- not folded into a parenthesised
    // suffix -- and a refusal, which is a different exit code from nothing answering.
    ScriptedNodeExchange node { { Refusal(Cc::ErrorCode::UnknownFleetSelector,
                                          "unknown section; this build serves:\n  kpi       the headline figures\n") } };

    auto const answer = RunFleet(node, nullptr, { "worker" });

    CHECK(answer.outcome == Outcome::Refused);
    REQUIRE(answer.advisories.size() == 1);
    CHECK(answer.advisories[0].contains("unknown section; this build serves:\n  kpi"));
    CHECK_FALSE(answer.advisories[0].ends_with('\n'));
    CHECK_FALSE(answer.advisories[0].contains("unknown-fleet-selector ("));
}

TEST_CASE("fleet names the dashboard credential flag when the leader refuses the caller", "[cli][node][fleet]")
{
    auto const refusal =
        Refusal(Cc::ErrorCode::Unauthenticated, "the fleet is served to a caller presenting the dashboard credential");

    ScriptedNodeExchange bare { { refusal } };
    auto const withoutToken = RunFleet(bare, nullptr, { "workers" });
    CHECK(withoutToken.outcome == Outcome::Refused);
    CHECK(Remarks(withoutToken, "present the dashboard credential with --dashboard-token-file"));

    // WHAT DISTINGUISHES: a presented credential that was refused is not told to present one.
    ScriptedNodeExchange guarded { { refusal } };
    auto const withToken = RunFleet(guarded, nullptr, { "workers" }, "guess");
    CHECK(Remarks(withToken, "was not accepted"));
    CHECK_FALSE(Remarks(withToken, "present the dashboard credential with"));
}

TEST_CASE("fleet reports a node or a leader it could not reach as unreachable", "[cli][node][fleet]")
{
    ScriptedNodeExchange silent { { NodeFailure(ExchangeFailure::Unreachable, "cannot reach 10.0.0.7:6674") } };
    auto const unreached = RunFleet(silent, nullptr, { "workers" });
    CHECK(unreached.outcome == Outcome::Unreachable);
    CHECK(Remarks(unreached, "cannot reach 10.0.0.7:6674"));

    // A leader that does not answer is the leader's address in the remark, not the follower's.
    ScriptedNodeExchange follower { { Refusal(Cc::ErrorCode::NotLeader, "10.0.0.9:6674") } };
    ScriptedDialer dial { {} };
    auto const leaderGone = RunFleet(follower, &dial, { "workers" });
    CHECK(leaderGone.outcome == Outcome::Unreachable);
    CHECK(Remarks(leaderGone, "cannot reach 10.0.0.9:6674"));
}

TEST_CASE("fleet refuses a table whose row does not match its header", "[cli][node][fleet]")
{
    // Every renderer walks `row[index]` across the HEADER's columns, so a short row is refused
    // rather than read past its end.
    ScriptedNodeExchange node { { FleetDocumentReply("id\ttoolchain\tslots\nw1\tgcc-13\n") } };

    auto const answer = RunFleet(node, nullptr, { "workers" });

    // `Protocol` and not `Refused`: nothing declined anything.
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Remarks(answer, "header names 3"));
}

TEST_CASE("fleet refuses a document with no header line", "[cli][node][fleet]")
{
    // An empty SECTION still renders its header, so a document without one is not an empty fleet.
    ScriptedNodeExchange node { { FleetDocumentReply({}) } };

    auto const answer = RunFleet(node, nullptr, { "workers" });

    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(Remarks(answer, "no header line"));
}

// ---------------------------------------------------------------------------
// #1329: `ResolveAdmin`'s "runs no admin surface" arm says what to SET.
//
// The first tests `LadderGatherer` has ever had: the class was constructed only in `main.cpp`.
// Asked through `Gather`'s `/metrics` rung, the one caller `ResolveAdmin` has since `fleet`
// moved onto 0xFC (#1391).
// ---------------------------------------------------------------------------

namespace
{

/// A gatherer pointed at a scripted node, with no admin override and no cache.
///
/// The admin endpoint is left UNCONFIGURED deliberately: a configured one wins at
/// `ResolveAdmin`'s first line, so a fixture that supplied one would exercise the
/// override and reach none of the four discovery arms these cases are about.
/// @param node The scripted `0xFC` connection; it must outlive the gatherer.
/// @param cachePort The port this invocation already dialled, which the collision arm
///        compares a reported admin port against.
/// @return The gatherer.
[[nodiscard]] LadderGatherer GathererFor(INodeExchange& node, std::uint16_t cachePort = 6674)
{
    return LadderGatherer { Endpoint {},     Endpoint { .host = "10.0.0.4", .port = cachePort },
                            DialTimeouts {}, std::nullopt,
                            nullptr,         &node };
}

/// What the `/metrics` rung says it did not ask, for a gatherer.
/// @param gatherer The gatherer to run.
/// @return The rung's note; the rung must not have asked.
[[nodiscard]] std::string MetricsNoteOf(LadderGatherer& gatherer)
{
    auto const attempts = gatherer.Gather();
    auto const* const metrics =
        FindIfOrNull(attempts, [](StatsAttempt const& attempt) { return attempt.origin == StatsOrigin::Metrics; });
    REQUIRE(metrics != nullptr);
    REQUIRE_FALSE(metrics->asked);
    return metrics->note;
}

/// What `ResolveAdmin` refuses with, for a node reporting @p surfaces.
/// @param surfaces Every surface the node says it opened.
/// @param cachePort The port this invocation is already talking `0xFC` to.
/// @return The refusal text, as both callers receive it.
[[nodiscard]] std::string AdminRefusalFor(std::vector<Cc::SurfaceReport> surfaces, std::uint16_t cachePort = 6674)
{
    ScriptedNodeExchange node {
        { StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = std::move(surfaces) }) },
        "10.0.0.4:6674"
    };
    auto gatherer = GathererFor(node, cachePort);
    return MetricsNoteOf(gatherer);
}

} // namespace

TEST_CASE("a node that opened no admin surface is told which flag opens one", "[cli][node][stats][admin]")
{
    // `--admin-listen` is off by default and that default was defended on review, so
    // this arm is what an ORDINARY node answers -- and it is therefore the first thing
    // a new `fleet` user meets. It used to say only what was true and name nothing to
    // do about it, where the other three arms each already point somewhere.
    auto const detail = AdminRefusalFor({ Cc::SurfaceReport { .surface = Cc::WireSurface::Raft, .port = 6680 } });

    CHECK(detail.contains("--admin-listen"));
    // The load-bearing clause. Without it the sentence reads as something being WRONG
    // on the operator's machine, when a node with no admin surface is a configuration
    // choice somebody made.
    CHECK(detail.contains("off unless asked for"));
    // Still names WHICH node, which is the half a rewrite drops first: an operator with
    // several nodes cannot act on a remedy that does not say where to apply it.
    CHECK(detail.contains("10.0.0.4:6674"));
}

TEST_CASE("the other three admin arms name no flag, because none of them is a node lacking one", "[cli][node][stats][admin]")
{
    // **The load-bearing negative.** "The refusal names `--admin-listen`" is satisfied
    // by a build that names it in EVERY arm -- which would tell an operator whose admin
    // surface is running perfectly, or is TLS, or collided on a port, to go and switch
    // on a flag they already set. That is a confident wrong signal inside a refusal,
    // which is the failure this tree catalogues most often, and asserting only the
    // positive direction cannot see it.
    ScriptedNodeExchange silent { { NodeFailure(ExchangeFailure::Transport, "connection refused") }, "10.0.0.4:6674" };
    auto silentGatherer = GathererFor(silent);
    auto const undiscovered = MetricsNoteOf(silentGatherer);

    auto const tls = AdminRefusalFor({ Cc::SurfaceReport { .surface = Cc::WireSurface::Admin, .port = 9000, .tls = true } });
    auto const collides = AdminRefusalFor({ Cc::SurfaceReport { .surface = Cc::WireSurface::Admin, .port = 6674 } });

    CHECK_FALSE(undiscovered.contains("--admin-listen"));
    CHECK_FALSE(tls.contains("--admin-listen"));
    CHECK_FALSE(collides.contains("--admin-listen"));
}

TEST_CASE("the stats ladder relays the same remedy when it skips /metrics", "[cli][node][stats][admin]")
{
    // The note the cases above read, as the ladder renders it: folded into "<source> was not
    // asked: <note>". A remedy that only reads correctly as the bare note is half a fix.
    ScriptedNodeExchange node {
        { StatusReply({ .version = "0.2.0",
                        .nodeId = {},
                        .uptimeSeconds = 5,
                        .surfaces = { Cc::SurfaceReport { .surface = Cc::WireSurface::Raft, .port = 6680 } } }),
          NodeFailure(ExchangeFailure::Transport, "the node-metrics verb went unanswered") },
        "10.0.0.4:6674"
    };
    auto gatherer = GathererFor(node);

    auto const answer = ChooseStats(gatherer.Gather());

    CHECK(answer.outcome == Outcome::Unreachable);
    // `was not asked` AND the flag in ONE line: asserted separately they would pass on
    // a build that put the remedy on some other source's advisory.
    CHECK(std::ranges::any_of(answer.advisories, [](std::string const& line) {
        return line.contains("was not asked") && line.contains("--admin-listen");
    }));
}

TEST_CASE("the stats ladder records where it asked each source, and no address for one it did not ask", "[cli][node][stats]")
{
    // A panel's source line names the address that ANSWERED, so each attempt carries its own: the node's
    // `0xFC` address for node-metrics, the cache's for INFO, an IPv6 literal bracketed so its port reads.
    // WHAT DISTINGUISHES: /metrics was not asked, and an address beside it would name a listener nothing
    // dialled -- and node-metrics was asked and failed, which still says where.
    ScriptedNodeExchange node {
        { StatusReply({ .version = "0.2.0",
                        .nodeId = {},
                        .uptimeSeconds = 5,
                        .surfaces = { Cc::SurfaceReport { .surface = Cc::WireSurface::Raft, .port = 6680 } } }),
          NodeFailure(ExchangeFailure::Transport, "the node-metrics verb went unanswered") },
        "10.0.0.4:6674"
    };
    ScriptedExchange resp { Answers({ Bulk("fastcached_version:0.4.1\r\n") }) };
    auto gatherer =
        LadderGatherer { Endpoint {}, Endpoint { .host = "fd00::9", .port = 6379 }, DialTimeouts {}, std::nullopt, &resp,
                         &node };

    auto const attempts = gatherer.Gather();
    auto const whereOf = [&attempts](StatsOrigin origin) {
        auto const* attempt = FindIfOrNull(attempts, [origin](StatsAttempt const& one) { return one.origin == origin; });
        return attempt == nullptr ? std::optional<std::string> {} : std::optional<std::string> { attempt->where };
    };
    CHECK(whereOf(StatsOrigin::Metrics) == std::string {});
    CHECK(whereOf(StatsOrigin::NodeMetrics) == std::string { "10.0.0.4:6674" });
    CHECK(whereOf(StatsOrigin::Info) == std::string { "[fd00::9]:6379" });
}

TEST_CASE("the stats ladder records where it asked /metrics, whatever the scrape then did", "[cli][node][stats]")
{
    // The /metrics rung's address, which the case above cannot reach: that rung only asks an admin surface it
    // can DIAL. So this one gives it a real one -- a loopback listener this process holds, which accepts into
    // its backlog and never answers -- and a read bound short enough that the scrape gives up at once. WHAT
    // DISTINGUISHES: the attempt was asked and failed, and still says where; nothing about the address comes
    // from the reply, because there is none.
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound() || listener->BoundPort() == 0)
        SKIP("this host would not bind a loopback listener on any port; the /metrics rung cannot be dialled here");
    auto const admin = Endpoint { .host = "127.0.0.1", .port = listener->BoundPort() };
    auto gatherer =
        LadderGatherer { admin,
                         Endpoint { .host = "10.0.0.4", .port = 6674 },
                         DialTimeouts { .connect = std::chrono::seconds { 5 }, .io = std::chrono::milliseconds { 50 } },
                         std::nullopt,
                         nullptr,
                         nullptr };

    auto const attempts = gatherer.Gather();
    auto const* metrics = FindIfOrNull(attempts, [](StatsAttempt const& one) { return one.origin == StatsOrigin::Metrics; });
    REQUIRE(metrics != nullptr);
    CHECK(metrics->asked);
    CHECK_FALSE(metrics->record.has_value());
    CHECK(metrics->where == std::format("127.0.0.1:{}", admin.port));
}

// ---------------------------------------------------------------------------
// #134: an endpoint's identity, TYPED.
//
// `live-stats` infers its subject from what the endpoint is. The gatherer used to answer
// that only as `std::expected<NodeStatusFields, std::string>`, whose error side held
// three different states as three sentences of one type -- so a verb deciding from it
// would have been matching prose.
// ---------------------------------------------------------------------------

namespace
{

/// A node-status reply that says `Ok` and carries a body no decoder can read.
///
/// The fourth arm, and the one where two classifiers would most naturally drift: the
/// STATUS says compile node while the fields say nothing, so a second copy of the
/// decision that keyed on "did the fields decode" would call it something else.
/// @return The reply frame.
[[nodiscard]] std::vector<std::byte> OkWithUnreadableBody()
{
    auto const garbage = std::to_array({ std::byte { 0xFF } });
    return Cc::EncodeReply(Cc::Status::Ok, garbage);
}

/// The identity a gatherer reports for one scripted reply.
/// @param reply What the endpoint answers node-status with.
/// @return The identification.
[[nodiscard]] EndpointIdentity IdentityFor(ScriptedNodeExchange::Outcome reply)
{
    ScriptedNodeExchange node { { std::move(reply) }, "10.0.0.4:6674" };
    auto gatherer = GathererFor(node);
    return gatherer.IdentifyEndpoint();
}

} // namespace

TEST_CASE("an endpoint's identity keeps its four states apart by type", "[cli][node][identity]")
{
    // THE state that must not collapse is the first: nobody could ask. Inferring `cache`
    // for it reports *INFO did not answer* against a port that may speak no RESP.
    auto notAskedGatherer =
        LadderGatherer { Endpoint {}, Endpoint { .host = "10.0.0.4", .port = 6674 }, DialTimeouts {}, std::nullopt, nullptr,
                         nullptr };
    auto const notAsked = notAskedGatherer.IdentifyEndpoint();
    auto const silent = IdentityFor(NodeFailure(ExchangeFailure::Transport, "the server closed the connection"));
    auto const daemon = IdentityFor(RefusalReply(Cc::UnimplementedVerb));
    auto const node = IdentityFor(StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {} }));

    CHECK_FALSE(notAsked.kind.has_value());
    CHECK(silent.kind == RemoteKind::NotFastcacheWire);
    CHECK(daemon.kind == RemoteKind::FastcacheWireOnly);
    CHECK(node.kind == RemoteKind::CompileNode);

    // Each answered state names the endpoint, which is what a refusal quoting it needs.
    for (auto const& identity: { silent, daemon, node })
        CHECK(identity.detail.contains("10.0.0.4:6674"));
    CHECK(notAsked.detail.contains("no 0xFC connection"));
}

TEST_CASE("the identity and the probe classify every reply the same way", "[cli][node][identity]")
{
    // Two askers, one decision. Driven over every reply shape, and the unreadable `Ok`
    // body is the row that distinguishes: it is a compile node by status and nothing by
    // fields, which is exactly where a second copy of the decision would part company.
    auto const replies = std::to_array<ScriptedNodeExchange::Outcome>({
        StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {} }),
        OkWithUnreadableBody(),
        RefusalReply(Cc::UnimplementedVerb),
        RefusalReply(Cc::ErrorCode::NotAMember),
        NodeFailure(ExchangeFailure::Transport, "the server closed the connection"),
    });

    for (auto const& reply: replies)
    {
        ScriptedNodeExchange probed { { reply } };
        auto const probe = ProbeRemote(probed);
        auto const identity = IdentityFor(reply);
        CAPTURE(static_cast<int>(probe), identity.detail);
        CHECK(identity.kind == probe);
    }

    // And the unreadable body is still a NODE, not merely consistent with the probe: a
    // probe and an identity that both drifted to `FastcacheWireOnly` would agree.
    CHECK(IdentityFor(OkWithUnreadableBody()).kind == RemoteKind::CompileNode);
}

TEST_CASE("an endpoint is identified once however many callers ask", "[cli][node][identity]")
{
    // A verb asking what the endpoint is, and a rung then reading the same fact, must pay
    // ONE node-status round trip between them -- a second is a second request a daemon
    // refuses. The `/metrics` rung reads the identification: this node reports no admin
    // surface, so it dials nothing.
    ScriptedNodeExchange node { { StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {} }),
                                  NodeFailure(ExchangeFailure::Transport, "the node-metrics verb went unanswered") },
                                "10.0.0.4:6674" };
    auto gatherer = GathererFor(node);

    CHECK(gatherer.IdentifyEndpoint().kind == RemoteKind::CompileNode);
    CHECK(gatherer.IdentifyEndpoint().kind == RemoteKind::CompileNode);
    (void) MetricsNoteOf(gatherer);

    // Counted by VERB, since the node-metrics rung sends its own request on the same connection.
    auto const statusRequests = std::ranges::count_if(node.Sent(), [](std::vector<std::byte> const& frame) {
        return frame.size() > 2 && frame[2] == static_cast<std::byte>(Cc::Op::NodeStatus);
    });
    CHECK(statusRequests == 1);
    CHECK(node.Unused() == 0);
}

TEST_CASE("a compile node answers stats through the ladder's node-metrics rung", "[cli][node][stats]")
{
    // The FACT a verb page's "on a compile node" cell must agree with. `stats` is on the
    // `Stats` wire, carries no fallback and is not a node verb -- once the only two ways
    // that cell knew a node could answer -- yet a node answers it, through the rung that
    // asks the node for its own counters. Asserting only what the page SAYS would prove the text
    // changed; this proves the text is true.
    ScriptedNodeExchange node { { StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {} }),
                                  NodeMetricsReply(12, StorageStats { .itemCount = 3 }) },
                                "10.0.0.4:6674" };
    auto gatherer = GathererFor(node);

    auto const* const stats = FindVerb("stats");
    REQUIRE(stats != nullptr);
    auto const answer = RunVerb(*stats, VerbContext { .node = &node, .stats = &gatherer });

    CHECK(answer.outcome == Outcome::Affirmative);
    CHECK(RequiredCell(answer, "source").lexical == "node-metrics");
    CHECK(RequiredCell(answer, "fastcache_worker_jobs_completed_total").lexical == "12");
    // And the cache tier's figure, which the rung's old counters-only body could not carry (#1406) --
    // so `stats` against a node reads the same fields `/metrics` would have given it.
    CHECK(RequiredCell(answer, "fastcached_items").lexical == "3");
}

TEST_CASE("a node whose description this client cannot read is a node it cannot read", "[cli][node][identity]")
{
    // Still a node -- a verb inferring a subject must not drift to `cache` -- and flagged, so a
    // verb deciding whether to start asks a field. The control beside it: a readable node is not.
    auto const unreadable = IdentityFor(OkWithUnreadableBody());
    CHECK(unreadable.kind == RemoteKind::CompileNode);
    CHECK(unreadable.unreadable);
    CHECK(unreadable.detail.contains("cannot read"));

    auto const readable = IdentityFor(StatusReply({ .version = "0.2.0", .nodeId = {}, .uptimeSeconds = 5, .surfaces = {} }));
    CHECK_FALSE(readable.unreadable);

    // A refusal is not a node at all, and says nothing about a description.
    CHECK_FALSE(IdentityFor(RefusalReply(Cc::UnimplementedVerb)).unreadable);
}

TEST_CASE("a node-status refused on the wire version names both versions and does not deny a node", "[cli][node][identity]")
{
    // Refused before the verb was read, so nothing says whether a node is there: the sentence
    // names this client's wire and carries the server's own range, and never claims "not a
    // compile node", which is what every other refusal is worded as.
    auto const refused =
        IdentityFor(RefusalReply(Cc::ErrorCode::UnsupportedVersion, "unsupported wire version 8; this server speaks 6..6"));
    CHECK(refused.kind == RemoteKind::FastcacheWireOnly);
    CHECK(refused.detail.contains(std::format("0xFC wire {}", static_cast<unsigned>(Cc::CurrentVersion))));
    CHECK(refused.detail.contains("this server speaks 6..6"));
    CHECK_FALSE(refused.detail.contains("not a compile node"));

    // The control: an unimplemented verb keeps the daemon's sentence.
    CHECK(IdentityFor(RefusalReply(Cc::UnimplementedVerb)).detail.contains("not a compile node"));
}

// ---------------------------------------------------------------------------
// #1303: `cordon` and `uncordon` take this machine's worker out of the fleet and back.
// ---------------------------------------------------------------------------

TEST_CASE("cordon and uncordon send the cordon verb carrying the action each names", "[cli][node][cordon]")
{
    // The bytes, not only the rendering: a scripted node answers both actions the same
    // way, so a verb that sent the wrong one would render exactly as a right one.
    for (auto const& [verb, action]: { std::pair { std::string_view { "cordon" }, Cc::CordonAction::Cordon },
                                       std::pair { std::string_view { "uncordon" }, Cc::CordonAction::Lift } })
    {
        INFO("verb: " << verb);
        ScriptedNodeExchange node { { Cc::EncodeReply(
            Cc::Status::Ok, Cc::EncodeCordonFields({ .state = Cc::WireCordonState::Serving, .inFlight = 0 })) } };
        (void) RunNodeVerb(verb, node);

        REQUIRE(node.Sent().size() == 1);
        CHECK(OpOf(node.Sent()[0]) == 0x13);
        CHECK(node.Sent()[0] == Cc::EncodeCordonRequest(action));
    }
}

TEST_CASE("cordon reports the state it left the worker in and what is still running", "[cli][node][cordon]")
{
    SECTION("draining names the running compiles and what `node` will say when they are gone")
    {
        ScriptedNodeExchange node { { Cc::EncodeReply(
            Cc::Status::Ok, Cc::EncodeCordonFields({ .state = Cc::WireCordonState::Draining, .inFlight = 2 })) } };
        auto const answer = RunNodeVerb("cordon", node);

        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(RequiredCell(answer, "cordon").lexical == "draining");
        CHECK(RequiredCell(answer, "compiles-in-flight").lexical == "2");
        CHECK(RequiredCell(answer, "compiles-in-flight").kind == CellKind::Number);
        CHECK(AdvisoryText(answer).contains("2 compile(s) still running"));
        // And that the cordon ends with the process, which is what an operator who
        // expected it to persist across a reboot has to be told.
        CHECK(AdvisoryText(answer).contains("NOT persisted"));
    }

    SECTION("drained says nothing is running, and lifting it warns of nothing")
    {
        ScriptedNodeExchange node { { Cc::EncodeReply(
            Cc::Status::Ok, Cc::EncodeCordonFields({ .state = Cc::WireCordonState::Serving, .inFlight = 0 })) } };
        auto const answer = RunNodeVerb("uncordon", node);

        CHECK(RequiredCell(answer, "cordon").lexical == "serving");
        CHECK_FALSE(AdvisoryText(answer).contains("still running"));
        CHECK_FALSE(AdvisoryText(answer).contains("NOT persisted"));
    }

    SECTION("a reply body this build cannot read is refused, not rendered")
    {
        std::vector<std::byte> const noPayload;
        ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, noPayload) } };
        auto const answer = RunNodeVerb("cordon", node);

        CHECK(answer.outcome == Outcome::Protocol);
        CHECK(AdvisoryText(answer).contains("cannot read"));
    }
}

TEST_CASE("`node` renders the cordon, and says nothing on a node with no worker", "[cli][node][verbs][cordon]")
{
    SECTION("a cordoned worker is shown draining")
    {
        ScriptedNodeExchange node { { StatusReply(
            { .version = "1.2.3",
              .nodeId = "node-a",
              .uptimeSeconds = 5,
              .surfaces = {},
              .components = Cc::NodeComponentBit::Worker,
              .runtime = { .compileSlots = 4, .compilesInFlight = 1, .cordon = Cc::WireCordonState::Draining } }) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(RequiredCell(answer, "cordon").lexical == "draining");
    }

    SECTION("a node that runs no worker says NOTHING rather than a reassuring serving")
    {
        ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                    .nodeId = "node-a",
                                                    .uptimeSeconds = 5,
                                                    .surfaces = {},
                                                    .components = Cc::NodeComponentBit::Scheduler,
                                                    .runtime = {} }) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(CellOf(answer, "cordon") == nullptr);
    }
}

TEST_CASE("`node` reports where peers dial its consensus, apart from the port it bound", "[cli][node][verbs][consensus]")
{
    // #1328: the half of `--cluster-admit`'s receipt an operator holds against the joiner.
    SECTION("a consensus node names the address, and the raft port stays the BOUND port")
    {
        ScriptedNodeExchange node { { StatusReply(
            { .version = "1.2.3",
              .nodeId = "node-a",
              .uptimeSeconds = 5,
              .surfaces = { { .surface = Cc::WireSurface::Raft, .port = 6680, .tls = false } },
              .components = Cc::NodeComponentBit::Scheduler,
              .runtime = { .consensusEndpoint = "10.0.0.4:6680" } }) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(RequiredCell(answer, Cc::ConsensusEndpointField).lexical == "10.0.0.4:6680");
        CHECK(RequiredCell(answer, Cc::ConsensusEndpointField).kind == CellKind::Text);
        // Two cells, each carrying its own fact: a renderer that put the endpoint in the
        // port's place, or the port in the endpoint's, fails one of these.
        CHECK(RequiredCell(answer, "raft-port").lexical == "6680");
    }

    SECTION("a node running no consensus has no such field, rather than an empty one")
    {
        ScriptedNodeExchange node { { StatusReply({ .version = "1.2.3",
                                                    .nodeId = "node-a",
                                                    .uptimeSeconds = 5,
                                                    .surfaces = {},
                                                    .components = Cc::NodeComponentBit::Worker,
                                                    .runtime = {} }) } };
        auto const answer = RunNodeVerb("node", node);
        CHECK(answer.outcome == Outcome::Affirmative);
        CHECK(CellOf(answer, Cc::ConsensusEndpointField) == nullptr);
    }
}
