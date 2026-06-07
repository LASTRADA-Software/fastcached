// SPDX-License-Identifier: Apache-2.0
//
// TlsSocket unit tests. Compiled only in TLS-enabled builds (FASTCACHED_ENABLE_
// TLS); an empty translation unit otherwise. These drive the decorator over an
// in-memory transport (no real socket), covering construction/teardown, the
// handshake pump's error path, and — crucially — the Read pump completing
// SYNCHRONOUSLY (raw reads resolve inline from the in-memory pipe), which routes
// through IoAwaitable::Complete() from inside await_suspend: the exact shape of
// the re-entrancy bug. The end-to-end success path (real client handshake +
// PONG) is covered separately by the `tls-smoke` CTest.
#if defined(FC_TLS_ENABLED)

    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Bytes.hpp>
    #include <FastCache/Net/ISocket.hpp>
    #include <FastCache/Net/InMemoryTransport.hpp>
    #include <FastCache/Net/TlsContext.hpp>
    #include <FastCache/Net/TlsSocket.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <cstddef>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>

using namespace FastCache;

namespace
{

/// Absolute path to a checked-in test fixture under testdata/tls/.
[[nodiscard]] std::string TlsFixture(char const* name)
{
    return std::string { FASTCACHED_TESTDATA_DIR } + "/tls/" + name;
}

[[nodiscard]] Task<bool> WriteStr(ISocket* socket, std::string_view data)
{
    auto const result = co_await socket->Write(AsBytes(data));
    co_return result.has_value();
}

[[nodiscard]] Task<IoResult> ReadInto(ISocket* socket, std::span<std::byte> out)
{
    co_return co_await socket->Read(out);
}

} // namespace

TEST_CASE("TlsSocket: handshake on non-TLS input fails cleanly instead of hanging", "[tls][net]")
{
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());

    auto pair = InMemorySocketPair::Create();
    // The peer sends garbage rather than a ClientHello, then half-closes so the
    // pump observes EOF rather than parking forever.
    REQUIRE(SyncRun(WriteStr(pair.client.get(), "this is definitely not a TLS ClientHello\r\n")));
    pair.client->ShutdownWrite();

    auto server = std::make_unique<TlsSocket>(std::move(pair.server), **context);
    auto const handshake = SyncRun(server->HandshakeIfNeeded());
    CHECK_FALSE(handshake.has_value()); // resolves to an error — not a hang, not a crash
}

TEST_CASE("TlsSocket: Read resolves (no re-entrant resume) when the pump completes synchronously", "[tls][net]")
{
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());

    auto pair = InMemorySocketPair::Create();
    // Pre-buffer bytes then EOF: every raw read the TLS pump issues resolves
    // inline from the in-memory pipe, so DriveRead calls IoAwaitable::Complete()
    // from within Read()'s await_suspend. Pre-fix this resumed re-entrantly; the
    // assertion here is simply that Read RESOLVES.
    REQUIRE(SyncRun(WriteStr(pair.client.get(), "not a valid TLS record")));
    pair.client->ShutdownWrite();

    auto server = std::make_unique<TlsSocket>(std::move(pair.server), **context);
    std::array<std::byte, 64> buffer {};
    auto const result = SyncRun(ReadInto(server.get(), std::span<std::byte> { buffer.data(), buffer.size() }));
    // Handshake never completed (garbage in), so the read resolves to EOF(0) or an
    // error. The point of the test is that it resolves at all.
    CHECK((!result.has_value() || *result == 0));
}

#endif // FC_TLS_ENABLED
