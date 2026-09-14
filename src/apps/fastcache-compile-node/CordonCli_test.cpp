// SPDX-License-Identifier: Apache-2.0
#include "CordonCli.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <tests/ScriptedSocket.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Parse @p args the way the binary does.
/// @param args The flags.
/// @return The configuration they produce.
[[nodiscard]] NodeConfig ParsedFrom(std::vector<char const*> const& args)
{
    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { args }, cfg);
    REQUIRE(flow.has_value());
    return cfg;
}

/// A node that answers one cordon with @p fields.
/// @param fields The state it reports.
/// @return The scripted socket.
[[nodiscard]] Testing::ScriptedSocket NodeAnswering(Wire::CordonFields const& fields)
{
    return Testing::ScriptedSocket { Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeCordonFields(fields)) };
}

} // namespace

TEST_CASE("Naming --cordon or --uncordon selects the verb, and naming neither serves", "[node][cordon]")
{
    CHECK(ParsedFrom({ "--cordon" }).cordon == CordonCommand::Cordon);
    CHECK(ParsedFrom({ "--uncordon" }).cordon == CordonCommand::Lift);
    // The last flag wins, as every one-shot verb does: the two are one decision.
    CHECK(ParsedFrom({ "--cordon", "--uncordon" }).cordon == CordonCommand::Lift);
    CHECK(ParsedFrom({ "--toolchain=/usr/bin/g++" }).cordon == CordonCommand::None);
}

TEST_CASE("A cordon dials this node's own listener, on loopback when it is bound to every address", "[node][cordon]")
{
    // A wildcard names no address to dial; loopback of the same family reaches it and is
    // certainly this machine, which is the one place the node accepts a cordon from.
    CHECK(SelfDialEndpoint(SurfaceEndpoint { .host = "0.0.0.0", .port = 6674, .role = {} }) == "127.0.0.1:6674");
    CHECK(SelfDialEndpoint(SurfaceEndpoint { .host = "::", .port = 6674, .role = {} }) == "[::1]:6674");
    CHECK(SelfDialEndpoint(SurfaceEndpoint { .host = "", .port = 6674, .role = {} }) == "127.0.0.1:6674");
    // A specific bind is where the node listens and nowhere else, so it is dialled as written.
    CHECK(SelfDialEndpoint(SurfaceEndpoint { .host = "127.0.0.1", .port = 7001, .role = {} }) == "127.0.0.1:7001");
    CHECK(SelfDialEndpoint(SurfaceEndpoint { .host = "10.1.2.3", .port = 6674, .role = {} }) == "10.1.2.3:6674");
}

TEST_CASE("A cordon sends the cordon verb carrying the action, and reports what is still running", "[node][cordon]")
{
    auto node = NodeAnswering(Wire::CordonFields { .state = Wire::WireCordonState::Draining, .inFlight = 3 });
    auto notice = Cc::CredentialNotice::Silent();
    NodeConfig const cfg;
    ConfiguredCredential const credential { cfg, nullptr };

    auto const report = PutCordonRequest(node, notice, Wire::CordonAction::Cordon, credential, "127.0.0.1:6674");

    // The bytes, not only the rendering: a verb that sent LIFT would be answered by a
    // scripted node exactly the same way.
    CHECK(node.Sent() == Wire::EncodeCordonRequest(Wire::CordonAction::Cordon));
    auto const header = Wire::DecodeRequestHeader(node.Sent());
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == 0x13);

    REQUIRE(report.has_value());
    CHECK(report->contains("draining"));
    CHECK(report->contains("3 compile(s) still running"));
    CHECK(report->contains("abandons those compiles"));
}

TEST_CASE("A drained worker is reported as safe to stop, and a serving one as serving", "[node][cordon]")
{
    CHECK(RenderCordonReply({ .state = Wire::WireCordonState::Drained, .inFlight = 0 }).contains("abandons nothing"));
    CHECK_FALSE(RenderCordonReply({ .state = Wire::WireCordonState::Draining, .inFlight = 1 }).contains("abandons nothing"));
    CHECK(RenderCordonReply({ .state = Wire::WireCordonState::Serving, .inFlight = 4 }).contains("takes compiles"));
}

TEST_CASE("A cordon the node refuses is reported with the node's reason", "[node][cordon]")
{
    Testing::ScriptedSocket node { Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember,
                                                          "a machine is cordoned from itself") };
    auto notice = Cc::CredentialNotice::Silent();
    NodeConfig const cfg;
    ConfiguredCredential const credential { cfg, nullptr };

    auto const report = PutCordonRequest(node, notice, Wire::CordonAction::Lift, credential, "10.1.2.3:6674");

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().contains("cordoned from itself"));
}

TEST_CASE("A cordon answered with a body this build cannot read is refused, not rendered", "[node][cordon]")
{
    std::vector<std::byte> const noBody;
    Testing::ScriptedSocket node { Wire::EncodeReply(Wire::Status::Ok, noBody) };
    auto notice = Cc::CredentialNotice::Silent();
    NodeConfig const cfg;
    ConfiguredCredential const credential { cfg, nullptr };

    auto const report = PutCordonRequest(node, notice, Wire::CordonAction::Cordon, credential, "127.0.0.1:6674");

    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().contains("cannot read"));
}
