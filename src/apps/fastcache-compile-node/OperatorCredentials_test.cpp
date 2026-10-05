// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "OperatorCredentials.hpp"

#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

TEST_CASE("An operator verb mints from this machine's node, at the port --listen-node names", "[node][ticket]")
{
    // A loopback LITERAL whatever the node binds -- the node mints only for a caller on its own
    // machine -- and at the configured port, because the verb is run with the node's configuration.
    struct Row
    {
        char const* listen;
        char const* source;
    };
    for (auto const& row: { Row { .listen = "127.0.0.1:6674", .source = "127.0.0.1:6674" },
                            Row { .listen = "0.0.0.0:7700", .source = "127.0.0.1:7700" },
                            Row { .listen = "[::]:7700", .source = "127.0.0.1:7700" },
                            Row { .listen = "node.example.com:7700", .source = "127.0.0.1:7700" },
                            Row { .listen = "7700", .source = "127.0.0.1:7700" },
                            // A loopback literal is already this machine, and is kept.
                            Row { .listen = "[::1]:7700", .source = "[::1]:7700" } })
    {
        INFO(row.listen);
        NodeConfig cfg;
        cfg.nodeListen = row.listen;
        CHECK(OwnNodeTicketSource(cfg) == std::optional<std::string> { row.source });
    }
    // Unset: the default the node itself would bind.
    CHECK(OwnNodeTicketSource(NodeConfig {}) == std::optional<std::string> { "127.0.0.1:6674" });
}

TEST_CASE("An operator verb shows a scheduler a minted ticket, --upstream the password, and loopback nothing",
          "[node][ticket]")
{
    NodeConfig cfg;
    cfg.nodeListen = "0.0.0.0:7700";
    cfg.upstream = "cache.example.com:6379";
    cfg.requirePass = SecureString { "the-password" };
    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes(std::string_view { "minted" })) } };
    auto said = std::vector<std::string> {};
    OperatorCredentials operatorCredentials { cfg, dialer, [&](std::string_view line) { said.emplace_back(line); } };
    auto& credentials = operatorCredentials.Credentials();

    // The scheduler: one mint, asked of this machine's node, for exactly that audience.
    auto const toScheduler = credentials.Present("sched-a.internal:6675");
    CHECK_FALSE(toScheduler.missing.has_value());
    CHECK(toScheduler.credential.kind == Wire::AuthKind::MachineTicket);
    CHECK(toScheduler.credential.secret.View() == "minted");
    REQUIRE(dialer.Dialed() == std::vector<std::string> { "127.0.0.1:7700" });
    auto const mint = dialer.SentOn(0);
    auto const header = Wire::DecodeRequestHeader(mint);
    REQUIRE(header.has_value());
    // The mint itself presents nothing: it is admitted as this machine, by the connection.
    REQUIRE(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::MintTicket));
    auto const audience = Wire::DecodeMintTicketPayload(mint.subspan(Wire::RequestHeaderSize));
    REQUIRE(audience.has_value());
    CHECK(Unwrap(audience) == "sched-a.internal:6675");

    // The password goes to the one endpoint it belongs to, and nothing is minted for it.
    auto const toUpstream = credentials.Present("cache.example.com:6379");
    CHECK(toUpstream.credential.kind == Wire::AuthKind::Password);
    CHECK(toUpstream.credential.secret.View() == "the-password");
    // Loopback admits this machine as itself.
    CHECK_FALSE(credentials.Present("127.0.0.1:6675").credential.Configured());
    CHECK(dialer.Dialed().size() == 1);
    CHECK(said.empty());
}

TEST_CASE("An operator verb whose mint fails presents nothing, names why, and says so once", "[node][ticket]")
{
    NodeConfig cfg;
    Testing::ScriptedDialer dialer { { {} } };
    auto said = std::vector<std::string> {};
    OperatorCredentials operatorCredentials { cfg, dialer, [&](std::string_view line) { said.emplace_back(line); } };

    auto const presented = operatorCredentials.Credentials().Present("sched-a.internal:6675");

    CHECK_FALSE(presented.credential.Configured());
    CHECK(presented.missing == std::optional { Cc::MintFailure::Unreachable });
    REQUIRE(said.size() == 1);
    CHECK(said.front().contains(Cc::ReasonFor(Cc::MintFailure::Unreachable)));
    CHECK(said.front().contains("127.0.0.1:6674"));
}
