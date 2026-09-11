// SPDX-License-Identifier: Apache-2.0
#include "NodeClient.hpp"
#include "ScriptedExchange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Frame a reply the way a server does.
/// @param status What it says.
/// @param payload Its body.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> Reply(Wire::Status status, std::span<std::byte const> payload)
{
    return Wire::EncodeReply(status, payload);
}

/// Frame a refusal the way a server does.
/// @param code Which refusal.
/// @param detail Words for a person.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> Refusal(Wire::ErrorCode code, std::string_view detail)
{
    return Wire::EncodeErrorReply(code, detail);
}

} // namespace

TEST_CASE("A client tells the three refusal states apart", "[cli][node][refusal]")
{
    // **The whole design, and the reason this classification exists rather than a
    // boolean.** #283/#340: a `FASTCACHE_TOKEN` launcher read `DispatchNotPermitted` as
    // *not implemented*, stepped over it, and earned a permanent 0% hit rate that read
    // as a cold cache -- correct objects, green build, no counter moving.
    CHECK(ClassifyRefusal(Wire::UnimplementedVerb) == NodeRefusalKind::Unimplemented);
    CHECK(ClassifyRefusal(Wire::ErrorCode::DispatchNotPermitted) == NodeRefusalKind::ServedElsewhere);

    SECTION("and everything else is an ordinary refusal, which is the SAFE default")
    {
        // Asserted over codes a node actually answers rather than one invented value: a
        // refusal nobody enumerated must be REPORTED, because stepping over one is how a
        // client comes to ignore something it should have stopped on.
        CHECK(ClassifyRefusal(Wire::ErrorCode::NotAMember) == NodeRefusalKind::Reported);
        CHECK(ClassifyRefusal(Wire::ErrorCode::Unauthenticated) == NodeRefusalKind::Reported);
        CHECK(ClassifyRefusal(Wire::ErrorCode::NotLeader) == NodeRefusalKind::Reported);
        CHECK(ClassifyRefusal(Wire::ErrorCode::PayloadTooLarge) == NodeRefusalKind::Reported);
        CHECK(ClassifyRefusal(Wire::ErrorCode::EndpointBusy) == NodeRefusalKind::Reported);
    }

    SECTION("and a code this build has no name for is reported rather than stepped over")
    {
        // The direction that matters: a newer server's refusal must not be silently
        // treated as *not implemented*, which would make a client proceed past something
        // it was told to stop for.
        CHECK(ClassifyRefusal(static_cast<Wire::ErrorCode>(0xEE)) == NodeRefusalKind::Reported);
    }

    SECTION("and the two special codes are distinct BYTES, not merely distinct names")
    {
        // A symbol both ends spell can only test the NAME. The byte is the half a
        // deployed launcher tolerates and nobody here can recompile, so it is pinned
        // separately -- the rulebook's *a wire constant has two facts* rule.
        CHECK(static_cast<std::uint8_t>(Wire::UnimplementedVerb) == 0x02);
        CHECK(static_cast<std::uint8_t>(Wire::ErrorCode::DispatchNotPermitted) == 0x0C);
    }
}

TEST_CASE("A refusal is explained differently depending on which state it is", "[cli][node][refusal]")
{
    // The wording is the whole user interface of a failed command, so each arm is
    // asserted on what DISTINGUISHES it -- not merely that some sentence came back.
    auto const explained = [](Wire::ErrorCode code, std::string_view detail) {
        NodeReply reply;
        reply.status = Wire::Status::Error;
        reply.code = code;
        reply.detail = std::string { detail };
        return ExplainRefusal("node", "10.0.0.7:6674", reply);
    };

    SECTION("unimplemented names the endpoint, because the remedy is a different one")
    {
        auto const text = explained(Wire::UnimplementedVerb, "this node serves no component for that verb");
        CHECK(text.contains("10.0.0.7:6674"));
        CHECK(text.contains("does not implement"));
        CHECK(text.contains("`node`"));
        CHECK(text.contains("this node serves no component for that verb"));
    }

    SECTION("served-elsewhere must NOT read as a version problem")
    {
        // `UnknownOpcode` there would tell a client this endpoint is too OLD when it is
        // in fact too new, and sending somebody to upgrade a node that works correctly
        // is worse than saying nothing. Asserted as an absence AND a presence, because
        // the absence alone passes over an empty string.
        auto const text = explained(Wire::ErrorCode::DispatchNotPermitted, {});
        CHECK(text.contains("another endpoint"));
        CHECK_FALSE(text.contains("upgrade"));
        CHECK_FALSE(text.contains("version"));
    }

    SECTION("an ordinary refusal is relayed BY NAME with the server's own words")
    {
        auto const text = explained(Wire::ErrorCode::NotAMember, "fleet members only");
        CHECK(text.contains("not-a-member"));
        CHECK(text.contains("fleet members only"));
    }

    SECTION("a code with no row is reported as its byte rather than as nothing")
    {
        auto const text = explained(static_cast<Wire::ErrorCode>(0xEE), {});
        CHECK(text.contains("0xee"));
    }

    SECTION("and a reply carrying no code at all explains nothing")
    {
        // An `Ok` reply reaching here is a caller bug, and inventing a sentence for it
        // would put a refusal in front of an operator whose command succeeded.
        CHECK(ExplainRefusal("node", "10.0.0.7:6674", NodeReply {}).empty());
    }
}

TEST_CASE("A framed reply decodes into what a handler reads", "[cli][node][codec]")
{
    SECTION("an Ok reply hands back its body")
    {
        auto const body = Wire::EncodeNodeStatus({ .version = "1.2.3",
                                                   .nodeId = "node-a",
                                                   .uptimeSeconds = 42,
                                                   .surfaces = {},
                                                   .components = Wire::NodeComponentBit::Worker });
        auto const decoded = DecodeNodeReply(Reply(Wire::Status::Ok, body));
        REQUIRE(decoded.has_value());
        CHECK(decoded->status == Wire::Status::Ok);
        CHECK_FALSE(decoded->code.has_value());

        // Read back through the wire's own decoder: this client OWNS the payload rather
        // than viewing the read buffer, and a copy that truncated would still decode a
        // prefix, so the assertion is on a field near the END.
        auto const fields = Wire::DecodeNodeStatus(decoded->payload);
        REQUIRE(fields.has_value());
        CHECK(Unwrap(fields).components == Wire::NodeComponentBit::Worker);
    }

    SECTION("an error reply hands back its code and words, and no body")
    {
        auto const decoded = DecodeNodeReply(Refusal(Wire::ErrorCode::NotAMember, "fleet members only"));
        REQUIRE(decoded.has_value());
        CHECK(decoded->status == Wire::Status::Error);
        REQUIRE(decoded->code.has_value());
        CHECK(decoded->code == Wire::ErrorCode::NotAMember);
        CHECK(decoded->detail == "fleet members only");
        CHECK(decoded->payload.empty());
    }

    SECTION("bytes that are not a frame are refused rather than read as one")
    {
        std::vector<std::byte> const junk(4, std::byte { 0x7B });
        auto const decoded = DecodeNodeReply(junk);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().kind == ExchangeFailure::Malformed);
    }

    SECTION("a frame declaring more than arrived is refused rather than indexed past")
    {
        // **A peer-declared length sizes nothing until it is checked against what
        // arrived.** Built by truncating a real frame, so the header is valid and only
        // the arithmetic is wrong -- which is the case a length check must catch and a
        // header check cannot.
        auto whole = Reply(Wire::Status::Ok, Wire::AsBytes(std::string_view { "abcdefgh" }));
        REQUIRE(whole.size() > Wire::ReplyHeaderSize + 2);
        whole.resize(whole.size() - 2);

        auto const decoded = DecodeNodeReply(whole);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().kind == ExchangeFailure::Malformed);
        CHECK(decoded.error().detail.contains("arrived"));
    }

    SECTION("an error reply with no code is refused, not reported as a nameless refusal")
    {
        // A refusal whose code cannot be read is a PROTOCOL failure, not a refusal: a
        // client that turned it into one would print *the server declined* for bytes it
        // could not read, which sends an operator to the wrong file.
        auto const decoded = DecodeNodeReply(Reply(Wire::Status::Error, {}));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().kind == ExchangeFailure::Malformed);
    }
}

TEST_CASE("A NodeMetrics body decodes into a record that keeps its zeroes", "[cli][node][codec]")
{
    // **A counter is a tally, so zero is the truth about events that never happened.**
    // The node goes out of its way to send every row; a client that dropped the zeroes
    // would undo exactly the distinction that encoding preserved, and no round-trip test
    // that only checks a non-zero counter can see it.
    auto const row = [](std::string_view name, std::uint64_t value) {
        return WireFields::Encode({ Wire::AsBytes(name), std::span<std::byte const> { Wire::EncodeU64Field(value) } });
    };

    auto const a = row("fastcache_a_total", 7);
    auto const b = row("fastcache_b_total", 0);
    auto const payload = WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> { a, b } });

    auto const record = DecodeNodeCounters(payload);
    REQUIRE(record.has_value());
    REQUIRE(Unwrap(record).shape == Shape::Record);
    REQUIRE(Unwrap(record).fields.size() == 2);

    // Order is preserved, because the node sends them in catalogue order and an operator
    // diffing two nodes' output wants the same rows on the same lines.
    CHECK(Unwrap(record).fields[0].name == "fastcache_a_total");
    CHECK(Unwrap(record).fields[0].value.lexical == "7");
    CHECK(Unwrap(record).fields[1].name == "fastcache_b_total");
    CHECK(Unwrap(record).fields[1].value.kind == CellKind::Number);
    CHECK(Unwrap(record).fields[1].value.lexical == "0");

    SECTION("and a counter name that is not text is refused rather than repaired")
    {
        // One byte that is not UTF-8 makes `--format=json` unparseable for the whole
        // record, which is the fleet page's own rule arriving on another wire. Refused,
        // never substituted -- a repair is the failure that is quiet.
        auto const bad = row(std::string_view { "\xFF\xFE" }, 1);
        auto const broken = WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> { bad } });
        CHECK_FALSE(DecodeNodeCounters(broken).has_value());
    }

    SECTION("and a row that is not a name/value pair is refused")
    {
        auto const lonely = WireFields::Encode({ Wire::AsBytes(std::string_view { "only-a-name" }) });
        auto const broken = WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> { lonely } });
        CHECK_FALSE(DecodeNodeCounters(broken).has_value());
    }

    SECTION("and an empty body is an empty record rather than a failure")
    {
        // A node with no counters is not a node that answered badly. The ladder's rung
        // is what decides an empty reading is unusable; the codec's job is to say what
        // arrived.
        auto const empty =
            DecodeNodeCounters(WireFields::Encode(WireFields::FieldList { std::vector<std::span<std::byte const>> {} }));
        REQUIRE(empty.has_value());
        CHECK(Unwrap(empty).fields.empty());
    }
}

TEST_CASE("A probe tells the three kinds of endpoint apart", "[cli][node][probe]")
{
    // **Three states, and the third is the one that gets collapsed.** A client that can
    // only say *reachable* or *not* reports `the server closed the connection without
    // answering` for a compile node -- true, and useless, because it describes what
    // happened rather than what to do.
    SECTION("an endpoint that answers node-status is a compile node")
    {
        Cli::Testing::ScriptedNodeExchange node { { Reply(
            Wire::Status::Ok,
            Wire::EncodeNodeStatus(
                { .version = "1.2.3", .nodeId = {}, .uptimeSeconds = 1, .surfaces = {}, .components = 0 })) } };
        CHECK(ProbeRemote(node) == RemoteKind::CompileNode);
    }

    SECTION("an endpoint that FRAMES a refusal speaks the wire and is not narrowed further")
    {
        // `UnimplementedVerb` from a daemon and from a pre-#431 node are the same bytes.
        // A client that read one as *this is your cache daemon* would send somebody
        // debugging a daemon that is in fact a node one release behind, so every refusal
        // lands in one state -- asserted over three different codes, because a single
        // one passes under an implementation that special-cases it.
        for (auto const code:
             { Wire::UnimplementedVerb, Wire::ErrorCode::NotAMember, Wire::ErrorCode::DispatchNotPermitted })
        {
            Cli::Testing::ScriptedNodeExchange node { { Refusal(code, {}) } };
            CHECK(ProbeRemote(node) == RemoteKind::FastcacheWireOnly);
        }
    }

    SECTION("an endpoint that frames nothing is not this protocol")
    {
        Cli::Testing::ScriptedNodeExchange node { { Cli::Testing::NodeFailure(ExchangeFailure::Transport,
                                                                              "the server closed the connection") } };
        CHECK(ProbeRemote(node) == RemoteKind::NotFastcacheWire);
    }

    SECTION("and the probe asks node-status, not something a node might answer by accident")
    {
        // Read back out of the FRAME. A probe sending the wrong verb would classify from
        // an answer about a different question, and the fake answers from a script
        // regardless of what it was asked -- so nothing else in this file could see it.
        Cli::Testing::ScriptedNodeExchange node { { Refusal(Wire::UnimplementedVerb, {}) } };
        (void) ProbeRemote(node);
        REQUIRE(node.Sent().size() == 1);
        auto const header = Wire::DecodeRequestHeader(node.Sent()[0]);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::NodeStatus));
    }
}

TEST_CASE("An identified endpoint is explained in terms an operator can act on", "[cli][node][probe]")
{
    SECTION("a compile node names itself AND what to do instead")
    {
        auto const text = ExplainRemoteKind("get", "10.0.0.7:6674", RemoteKind::CompileNode);
        CHECK(text.contains("fastcache-compile-node"));
        CHECK(text.contains("10.0.0.7:6674"));
        CHECK(text.contains("`get`"));
        // The remedy, which is the half a bare *not supported* leaves out: the endpoint
        // is fine and the address is wrong.
        CHECK(text.contains("--addr"));
    }

    SECTION("a 0xFC-only endpoint names BOTH possibilities rather than guessing")
    {
        // A confident wrong signal is worse than a vague right one. Asserted as two
        // presences, because a sentence naming only one would still contain the other's
        // first word.
        auto const text = ExplainRemoteKind("get", "10.0.0.7:6674", RemoteKind::FastcacheWireOnly);
        CHECK(text.contains("fastcached"));
        CHECK(text.contains("older"));
    }

    SECTION("and an endpoint that is not this protocol is explained by NOBODY")
    {
        // The caller's own transport diagnostic already said the port did not answer.
        // A second sentence about one fault makes it read as two, so the empty string
        // here is a decision rather than a gap.
        CHECK(ExplainRemoteKind("get", "10.0.0.7:6674", RemoteKind::NotFastcacheWire).empty());
    }
}
