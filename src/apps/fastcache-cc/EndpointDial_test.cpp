// SPDX-License-Identifier: Apache-2.0
//
// The join between an endpoint written as text and a connected socket.
//
// What is worth testing here is the *refusals*, because each of them is a
// configuration mistake that must fail loudly rather than dial something
// plausible. The successful path is a socket, and `Net/TcpClient_test` owns it.
#include "EndpointDial.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A connector that records what it was asked to dial and connects to nothing.
///
/// It is what turns these from "the result was nullptr" into the stronger claim
/// the refusals are actually about: that a malformed endpoint reaches no
/// connector at all. A nullptr can also mean "dialled and failed", which is a
/// different fact -- and asserting the weaker one let the old version of this
/// file make a real 300ms network attempt inside a unit suite.
class RecordingConnector final: public IConnector
{
  public:
    [[nodiscard]] Task<SocketResult> Connect(std::string host,
                                             std::uint16_t port,
                                             std::chrono::milliseconds /*connectTimeout*/) override
    {
        _dials.emplace_back(std::move(host), port);
        co_return std::unexpected(
            NetError { .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" });
    }

    [[nodiscard]] std::size_t Dials() const noexcept
    {
        return _dials.size();
    }

    [[nodiscard]] std::pair<std::string, std::uint16_t> const& Last() const
    {
        return _dials.back();
    }

  private:
    std::vector<std::pair<std::string, std::uint16_t>> _dials;
};

/// Drive one dial synchronously. Sound because the connector above never
/// suspends, which is the same precondition `DialEndpointBlocking` encodes in its
/// parameter type.
[[nodiscard]] std::unique_ptr<ISocket> Dial(RecordingConnector& connector, std::string_view hostPort)
{
    return SyncRun(Cc::DialEndpoint(&connector, hostPort, 100ms));
}

} // namespace

TEST_CASE("DialEndpoint refuses text that names no port")
{
    RecordingConnector connector;
    CHECK(Dial(connector, "no-colon-here") == nullptr);
    CHECK(Dial(connector, "") == nullptr);
    CHECK(Dial(connector, "host:") == nullptr);
    CHECK(Dial(connector, "host:notanumber") == nullptr);

    // The point: nothing was dialled. A nullptr alone would also be produced by
    // dialling and failing, which is a different outcome for a different reason.
    CHECK(connector.Dials() == 0);
}

TEST_CASE("DialEndpoint refuses a bare port rather than assuming this machine")
{
    // `Core/HostPort::ParseEndpoint` would accept this and supply a default host,
    // which is right for a *bind* address an operator types and wrong here: every
    // caller is dialling something it was configured with, so text with no host in
    // it is a misconfiguration. Silently trying loopback would turn a typo into a
    // connection to whatever happens to be listening on this machine.
    RecordingConnector connector;
    CHECK(Dial(connector, "6674") == nullptr);
    CHECK(connector.Dials() == 0);
}

TEST_CASE("DialEndpoint refuses an empty host, which is a bare port respelled")
{
    // `:6674` and `[]:6674` split cleanly into an empty host and a valid port, so
    // the bare-port case above does not cover them. `Detail::RunConnectFlow`
    // refuses an empty host, so production is not reachable through this -- but
    // that makes the refusal a property of the connector, and this file's claim is
    // the stronger one: a malformed endpoint reaches no connector at all. Asserted
    // on `Dials()` for exactly that reason; a nullptr alone would be satisfied by
    // handing the empty host over and having it refused a layer down.
    RecordingConnector connector;
    CHECK(Dial(connector, ":6674") == nullptr);
    CHECK(Dial(connector, "[]:6674") == nullptr);
    CHECK(connector.Dials() == 0);
}

TEST_CASE("DialEndpoint refuses a port outside the 16-bit range")
{
    RecordingConnector connector;
    CHECK(Dial(connector, "127.0.0.1:65536") == nullptr);
    CHECK(Dial(connector, "127.0.0.1:-1") == nullptr);
    CHECK(connector.Dials() == 0);
}

TEST_CASE("DialEndpoint splits a well-formed endpoint and hands it over unbracketed")
{
    RecordingConnector connector;
    CHECK(Dial(connector, "cache.example.com:6674") == nullptr); // the connector refuses
    REQUIRE(connector.Dials() == 1);
    CHECK(connector.Last().first == "cache.example.com");
    CHECK(connector.Last().second == 6674);
}

TEST_CASE("DialEndpoint reports a refused peer as no socket, not as a throw")
{
    // Callers treat nullptr as "no cache" and fall back to compiling, which is the
    // whole reason this returns a pointer instead of throwing. Asserted against a
    // scripted refusal rather than a real unroutable address: the old version
    // dialled RFC 5737 TEST-NET-1 and waited 300ms for it, inside a unit suite.
    RecordingConnector connector;
    CHECK(Dial(connector, "192.0.2.1:9") == nullptr);
    CHECK(connector.Dials() == 1);
}
