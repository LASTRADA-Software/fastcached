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

using namespace FastCache;
using namespace std::chrono_literals;

TEST_CASE("DialEndpoint refuses text that names no port")
{
    CHECK(Cc::DialEndpoint("no-colon-here", 100ms) == nullptr);
    CHECK(Cc::DialEndpoint("", 100ms) == nullptr);
    CHECK(Cc::DialEndpoint("host:", 100ms) == nullptr);
    CHECK(Cc::DialEndpoint("host:notanumber", 100ms) == nullptr);
}

TEST_CASE("DialEndpoint refuses a bare port rather than assuming this machine")
{
    // `Core/HostPort::ParseEndpoint` would accept this and supply a default host,
    // which is right for a *bind* address an operator types and wrong here: every
    // caller is dialling something it was configured with, so text with no host in
    // it is a misconfiguration. Silently trying loopback would turn a typo into a
    // connection to whatever happens to be listening on this machine.
    CHECK(Cc::DialEndpoint("6674", 100ms) == nullptr);
}

TEST_CASE("DialEndpoint refuses a port outside the 16-bit range")
{
    CHECK(Cc::DialEndpoint("127.0.0.1:65536", 100ms) == nullptr);
    CHECK(Cc::DialEndpoint("127.0.0.1:-1", 100ms) == nullptr);
}

TEST_CASE("DialEndpoint reports an unreachable peer as no socket, not as a throw")
{
    // RFC 5737 TEST-NET-1: not routable anywhere, so this is the "peer is down"
    // path rather than a parse failure. Callers treat nullptr as "no cache" and
    // fall back to compiling, which is the whole reason this returns a pointer
    // instead of throwing.
    CHECK(Cc::DialEndpoint("192.0.2.1:9", 300ms) == nullptr);
}
