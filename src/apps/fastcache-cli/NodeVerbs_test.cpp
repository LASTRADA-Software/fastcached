// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"
#include "ScriptedExchange.hpp"

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

    REQUIRE(CellOf(answer, "version") != nullptr);
    CHECK(CellOf(answer, "version")->lexical == "0.2.0-124-gd911b33e");
    CHECK(CellOf(answer, "node-id")->lexical == "node-a");
    CHECK(CellOf(answer, "uptime-seconds")->lexical == "3600");
    CHECK(CellOf(answer, "components")->lexical == "cache-tier, worker");
    CHECK(CellOf(answer, "admin-port")->lexical == "9101");
    CHECK(CellOf(answer, "raft-port")->lexical == "9102");
    CHECK(CellOf(answer, "admin-tls")->lexical == "false");
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
        REQUIRE(CellOf(answer, "node-id") != nullptr);
        CHECK(CellOf(answer, "node-id")->kind == CellKind::Absent);
    }

    SECTION("and a node running no component says `none` rather than nothing")
    {
        // The converse of the rule above, and it is the half that gets collapsed: a node
        // that runs no component IS a reading. Absent there would say *I could not find
        // out*, which sends an operator to check a connection that worked.
        REQUIRE(CellOf(answer, "components") != nullptr);
        CHECK(CellOf(answer, "components")->kind == CellKind::Text);
        CHECK(CellOf(answer, "components")->lexical == "none");
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
    REQUIRE(CellOf(answer, "components") != nullptr);
    CHECK(CellOf(answer, "components")->lexical.contains("worker"));
    CHECK(CellOf(answer, "components")->lexical.contains("unknown(0x80)"));
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

    REQUIRE(CellOf(answer, "fastcache_worker_jobs_completed_total") != nullptr);
    CHECK(CellOf(answer, "fastcache_worker_jobs_completed_total")->lexical == "12");

    // The zero row is PRESENT, which is the property a reading of only the non-zero
    // counter cannot see.
    REQUIRE(CellOf(answer, "fastcache_worker_jobs_refused_no_capacity_total") != nullptr);
    CHECK(CellOf(answer, "fastcache_worker_jobs_refused_no_capacity_total")->lexical == "0");
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
    CHECK(nodeVerbs.size() == 2);

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
        CHECK(verb->modifiers == Modifier::None);
        CHECK(verb->minOperands == 0);
        CHECK(verb->maxOperands == 0);
        CHECK_FALSE(verb->protocolCommand.empty());
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
    CHECK(CellOf(answer, "server")->lexical == "0.2.0-125-g6ba32b30");
    CHECK(CellOf(answer, "client") != nullptr);

    // WHICH kind of server, so two version strings are not read as two builds of one
    // binary: a node and a daemon version alike and are different programs.
    CHECK(CellOf(answer, "server_kind")->lexical == "fastcache-compile-node");

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

    // The outcome is UNCHANGED -- the command still failed, and the exit code still says
    // so. Only the explanation is added.
    CHECK(answer.outcome == Outcome::Unreachable);
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
        CHECK(answer.outcome == Outcome::Unreachable);
        REQUIRE(answer.advisories.size() == 2);
        CHECK(answer.advisories[0].contains("0xFC"));
    }

    SECTION("and an endpoint that is not this protocol leaves the answer UNCHANGED")
    {
        // The probe explained nothing the caller does not already know, so nothing is
        // added: one fault must not read as two.
        auto const answer = RunNodeFallback(*verb, VerbContext { .node = &node }, RemoteKind::NotFastcacheWire, primary);
        CHECK(node.Sent().empty());
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
    REQUIRE(CellOf(answer, "server") != nullptr);
    CHECK(CellOf(answer, "server")->kind == CellKind::Absent);
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
