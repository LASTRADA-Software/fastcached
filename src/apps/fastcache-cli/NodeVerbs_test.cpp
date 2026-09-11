// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"
#include "CliVerbs.hpp"
#include "ScriptedExchange.hpp"

#include <FastCache/Cluster/ClusterState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

TEST_CASE("`node-metrics` reports every counter the node sent", "[cli][node][verbs]")
{
    auto const row = [](std::string_view name, std::uint64_t value) {
        return WireFields::Encode({ Cc::AsBytes(name), std::span<std::byte const> { Cc::EncodeU64Field(value) } });
    };
    auto const busy = row("fastcache_worker_jobs_completed_total", 12);
    auto const idle = row("fastcache_worker_jobs_refused_no_capacity_total", 0);
    auto const payload =
        WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> { busy, idle } });

    ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok, payload) } };

    auto const answer = RunNodeVerb("node-metrics", node);
    CHECK(answer.outcome == Outcome::Affirmative);
    REQUIRE(node.Sent().size() == 1);
    CHECK(OpOf(node.Sent()[0]) == static_cast<std::uint8_t>(Cc::Op::NodeMetrics));

    CHECK(RequiredCell(answer, "fastcache_worker_jobs_completed_total").lexical == "12");

    // The zero row is PRESENT, which is the property a reading of only the non-zero
    // counter cannot see.
    CHECK(RequiredCell(answer, "fastcache_worker_jobs_refused_no_capacity_total").lexical == "0");
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

    SECTION("and node-metrics does the same with its own body")
    {
        ScriptedNodeExchange node { { Cc::EncodeReply(Cc::Status::Ok,
                                                      Cc::AsBytes(std::string_view { "\xFF\xFE not fields" })) } };
        auto const answer = RunNodeVerb("node-metrics", node);
        CHECK(answer.outcome == Outcome::Protocol);
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
        // accepts and the handler cannot act on.
        CHECK(verb->modifiers == Modifier::None);

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

TEST_CASE("Exactly one verb carries a 0xFC fallback today, and it is `version`", "[cli][node][fallback]")
{
    // A census, and it is the kind that has to state its PATTERN: a row gaining a
    // fallback is a decision about whether a node can answer that QUESTION, and one
    // added without a test is a verb that silently starts asking a node about a keyspace
    // it does not have.
    std::vector<std::string_view> withFallback;
    for (auto const& verb: Verbs())
        if (verb.nodeFallback != nullptr)
            withFallback.push_back(verb.name);

    REQUIRE(withFallback.size() == 1);
    CHECK(withFallback[0] == "version");

    // And the control that says the census could have found more: every node verb was
    // examined, so an empty `Verbs()` would fail here rather than passing vacuously.
    CHECK(Verbs().size() > 20);
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

/// Every advisory joined, for a `contains` check.
///
/// A refusal's sentence is an ADVISORY here rather than a field of `Answer` -- remarks
/// go to stderr in every format so stdout stays parseable -- and which advisory carries
/// it is not a property worth pinning.
/// @param answer The answer.
/// @return The advisories, newline separated.
[[nodiscard]] std::string AdvisoryText(Answer const& answer)
{
    std::string out;
    for (auto const& advisory: answer.advisories)
    {
        out += advisory;
        out.push_back('\n');
    }
    return out;
}

} // namespace

TEST_CASE("cluster-members reports who the cluster agreed on, and an unled member as ABSENT", "[cli][node][cluster]")
{
    // The two states a `schedulerEndpoint` has, side by side in one reply. A member that
    // has never led carries none -- a leader announces its own record on election -- so
    // the empty string is the ORDINARY case and rendering it as a value would hand an
    // operator an address to paste that reaches nothing.
    ScriptedNodeExchange node { { ClusterStatusReply(
        { .members = { { .id = "node-a", .raftEndpoint = "10.0.0.7:6675", .schedulerEndpoint = "10.0.0.7:6674" },
                       { .id = "node-b", .raftEndpoint = "10.0.0.8:6675", .schedulerEndpoint = {} } },
          .settings = {} }) } };

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
    CHECK(RequiredCell(answer, "consensus-endpoint-as-recorded").lexical == "10.0.0.9:6675");

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
// #1300: `fleet` relays one of the leader's tables into a terminal.
// ---------------------------------------------------------------------------

namespace
{

/// An admin surface that answers one scripted document.
///
/// A fake rather than a real one because the verb's subject is the DOCUMENT: where the
/// surface is, whether it is TLS and whether the port collides are `ResolveAdmin`'s
/// questions and are tested where they are decided.
class ScriptedAdmin final: public IAdminDocument
{
  public:
    /// @param answer What `FetchAdmin` returns, whatever it is asked for.
    explicit ScriptedAdmin(std::expected<std::string, AdminError> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::expected<std::string, AdminError> FetchAdmin(std::string_view path) override
    {
        _asked.emplace_back(path);
        return _answer;
    }

    /// Every path this was asked for, in order.
    [[nodiscard]] std::vector<std::string> const& Asked() const noexcept
    {
        return _asked;
    }

  private:
    std::expected<std::string, AdminError> _answer;
    std::vector<std::string> _asked;
};

/// Run `fleet` against a scripted admin surface.
/// @param admin What the surface answers.
/// @param operands The positional arguments.
/// @return The answer.
[[nodiscard]] Answer RunFleet(IAdminDocument* admin, std::vector<std::string> const& operands)
{
    auto const* const verb = FindVerb("fleet");
    REQUIRE(verb != nullptr);
    // A node connection the verb never sends on: the wire column requires one to be
    // OPEN, because that is what discovers the admin port in production, and the
    // discovery itself lives behind the seam this fake replaces.
    ScriptedNodeExchange node { {} };
    return RunVerb(*verb, VerbContext { .operands = operands, .node = &node, .admin = admin });
}

} // namespace

TEST_CASE("fleet renders the section the leader sent, with the leader's own columns", "[cli][node][fleet]")
{
    // The columns come off the document and are never named here: they are
    // `FleetColumn`'s, decided on the leader, and a list in this binary would be a
    // second place for them to be spelled.
    ScriptedAdmin admin { std::string { "id\ttoolchain\tslots\nw1\tgcc-13-abcdef\t8\n" } };

    auto const answer = RunFleet(&admin, { "workers" });

    CHECK(answer.outcome == Outcome::Affirmative);
    // The section reaches the surface as a query parameter rather than being filtered
    // here -- filtering client-side would need this binary to know which columns belong
    // to which section, which is the mapping it is deliberately without.
    REQUIRE(admin.Asked().size() == 1);
    CHECK(admin.Asked()[0] == "/fleet.txt?section=workers");

    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json });
    CHECK(rendered.contains("toolchain"));
    CHECK(rendered.contains("gcc-13-abcdef"));
}

TEST_CASE("fleet turns the leader's dash into a real absent cell", "[cli][node][fleet]")
{
    // Not text that happens to look absent: `--absent` and JSON `null` have to behave
    // here as they do for every other verb, and a `-` left as text would render as the
    // string "-" in JSON where every other absence is `null`.
    ScriptedAdmin admin { std::string { "id\tlast-picked-age\nw1\t-\n" } };

    auto const answer = RunFleet(&admin, { "workers" });

    REQUIRE(answer.outcome == Outcome::Affirmative);
    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Json });
    CHECK(rendered.contains("null"));
    // And the dash is GONE rather than both present -- otherwise this passes against a
    // renderer that emitted the text and a null side by side.
    CHECK_FALSE(rendered.contains("\"-\""));
}

TEST_CASE("fleet keeps the escaping the leader applied", "[cli][node][fleet]")
{
    // A tab a peer put in its own display name arrives spelled, and stays spelled. This
    // tool is a RELAY rather than a second author of the leader's rule: unescaping here
    // would re-derive one convention in two binaries, free to drift, and would put a
    // real tab back into a cell that `--format=csv` carries raw and the human format
    // prints into its own aligned columns.
    ScriptedAdmin admin { std::string { "endpoint\tname\n10.0.0.2:7100\tbuild\\tnode\n" } };

    auto const answer = RunFleet(&admin, { "machines" });

    REQUIRE(answer.outcome == Outcome::Affirmative);
    auto const rendered = RenderValue(answer.value, RenderOptions { .format = OutputFormat::Tsv });

    // **What distinguishes moved when #1327 landed, and this is the assertion that
    // followed it.** The cell holds a backslash and a `t` -- that is what the leader
    // sent -- so this client's own TSV writer, now that it escapes rather than passing
    // text through, DOUBLES that backslash. A build that unescaped the leader's text
    // would hold a real tab, which the same writer would spell as a single `\t`.
    //
    // So the two hypotheses differ in the output by one backslash, and neither shifts a
    // column any more. The tab count below was the load-bearing half before #1327 and
    // is now the weaker one: it still refuses an invented column, but an unescaping
    // client would pass it, because the writer would spell the tab it had introduced.
    CHECK(rendered.contains("build\\\\tnode"));
    CHECK_FALSE(rendered.contains("build\\tnode"));
    CHECK(std::ranges::count(rendered, '\t') == std::ranges::count(std::string_view { "endpoint\tname" }, '\t') * 2);
}

TEST_CASE("fleet relays a refusal as a refusal, not as an unreachable fleet", "[cli][node][fleet]")
{
    // The discrimination this verb exists to keep: a leader that ANSWERED and declined
    // is a different exit code from nothing answering, and the server's words carry the
    // accepted keys. Reported as unreachable, an operator goes to check a listener that
    // is running perfectly and never reads the sentence naming their typo.
    ScriptedAdmin admin { std::unexpected(
        AdminError { .kind = AdminFailure::Refused,
                     .detail = "/fleet.txt?section=worker answered HTTP 400: unknown section; this build serves:" }) };

    auto const answer = RunFleet(&admin, { "worker" });

    CHECK(answer.outcome == Outcome::Refused);
    CHECK(std::ranges::any_of(answer.advisories, [](std::string const& line) { return line.contains("unknown section"); }));
}

TEST_CASE("fleet reports a surface it could not reach as unreachable", "[cli][node][fleet]")
{
    // The other half of the same discrimination, asserted separately: without this the
    // case above passes against a verb that answers `Refused` for everything.
    ScriptedAdmin admin { std::unexpected(
        AdminError { .kind = AdminFailure::Unreachable, .detail = "10.0.0.7:6674 runs no admin surface" }) };

    auto const answer = RunFleet(&admin, { "workers" });

    CHECK(answer.outcome == Outcome::Unreachable);
    CHECK(std::ranges::any_of(answer.advisories, [](std::string const& line) { return line.contains("no admin surface"); }));
}

TEST_CASE("fleet with no admin surface available says so rather than dialling nothing", "[cli][node][fleet]")
{
    // Nothing was CONFIGURED to ask, which is not the same as asking and failing -- the
    // three-state rule the stats ladder already keeps, at a verb that could otherwise
    // dereference a null.
    auto const answer = RunFleet(nullptr, { "workers" });

    CHECK(answer.outcome == Outcome::Usage);
}

TEST_CASE("fleet refuses a table whose row does not match its header", "[cli][node][fleet]")
{
    // Reachable, and the reason is the transport: `HttpGet` sends `Connection: close`
    // and reads to EOF, so a transfer cut short arrives as a complete-looking body
    // whose last line is half a row. Every renderer then walks `row[index]` across the
    // HEADER's columns -- an out-of-range read on a vector, not a narrow table.
    ScriptedAdmin admin { std::string { "id\ttoolchain\tslots\nw1\tgcc-13\n" } };

    auto const answer = RunFleet(&admin, { "workers" });

    // `Protocol` and not `Refused`: nothing declined anything. Bytes arrived that this
    // client cannot read as a table, which is a different thing to tell an operator.
    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(std::ranges::any_of(answer.advisories, [](std::string const& line) { return line.contains("header names 3"); }));
}

TEST_CASE("fleet refuses a document with no header line", "[cli][node][fleet]")
{
    // An empty SECTION still renders its header -- that is why the renderer emits one
    // for a table with no rows at all. A document without one is not an empty fleet,
    // it is a body this client cannot read, and rendering it as a table with no
    // columns would show an operator an empty result for a question that failed.
    ScriptedAdmin admin { std::string {} };

    auto const answer = RunFleet(&admin, { "workers" });

    CHECK(answer.outcome == Outcome::Protocol);
    CHECK(std::ranges::any_of(answer.advisories, [](std::string const& line) { return line.contains("no header line"); }));
}
