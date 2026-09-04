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
    #include <ranges>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>

    #include <openssl/bio.h>
    #include <openssl/err.h>
    #include <openssl/ssl.h>

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

/// A hand-driven TLS **client**, over the same pair of `InMemoryPipe`s the server's
/// raw transport uses as its wire.
///
/// `TlsSocket` cannot play this part: it is server-side by construction
/// (`SSL_set_accept_state`). And the cases below need a peer that closes the way the
/// protocol says to -- a `close_notify` RECORD, then the transport FIN -- at an
/// instant the test picks, which is exactly the sequence #712 is about. Driving
/// OpenSSL directly is also what keeps the wire inspectable: the ciphertext is
/// pushed into, and pulled out of, the pipes by hand, so a case can stop between the
/// alert and the FIN.
class TlsPeer
{
  public:
    /// Build a client SSL with memory BIOs, verification off (the fixture
    /// certificate is self-signed and identity is not what these cases test).
    TlsPeer():
        _ctx { SSL_CTX_new(TLS_client_method()) }
    {
        REQUIRE(_ctx != nullptr);
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);
        _ssl = SSL_new(_ctx);
        REQUIRE(_ssl != nullptr);
        auto* const incoming = BIO_new(BIO_s_mem());
        auto* const outgoing = BIO_new(BIO_s_mem());
        REQUIRE(incoming != nullptr);
        REQUIRE(outgoing != nullptr);
        SSL_set_bio(_ssl, incoming, outgoing); // SSL takes ownership of both
        _incoming = incoming;
        SSL_set_connect_state(_ssl);
    }

    TlsPeer(TlsPeer const&) = delete;
    TlsPeer(TlsPeer&&) = delete;
    TlsPeer& operator=(TlsPeer const&) = delete;
    TlsPeer& operator=(TlsPeer&&) = delete;

    ~TlsPeer()
    {
        if (_ssl != nullptr)
            SSL_free(_ssl); // frees both BIOs handed to SSL_set_bio
        if (_ctx != nullptr)
            SSL_CTX_free(_ctx);
    }

    /// @return The underlying OpenSSL client object.
    [[nodiscard]] SSL* Native() const noexcept
    {
        return _ssl;
    }

    /// Move every byte OpenSSL has queued outbound onto the wire towards the server.
    /// @param wire The pipe the server reads from.
    void Flush(InMemoryPipe& wire)
    {
        std::array<std::byte, 16384> staging {};
        while (true)
        {
            int const n = BIO_read(SSL_get_wbio(_ssl), staging.data(), static_cast<int>(staging.size()));
            if (n <= 0)
                return;
            auto const bytes = std::span<std::byte const> { staging.data(), static_cast<std::size_t>(n) };
            REQUIRE(wire.Push(bytes) == bytes.size());
        }
    }

    /// Drain everything the server has written and hand it to OpenSSL.
    /// @param wire The pipe the server writes into.
    void Feed(InMemoryPipe& wire)
    {
        std::array<std::byte, 16384> staging {};
        while (true)
        {
            auto const got = wire.TryPull(std::span<std::byte> { staging });
            if (got == 0)
                return;
            REQUIRE(BIO_write(_incoming, staging.data(), static_cast<int>(got)) == static_cast<int>(got));
        }
    }

    /// Send a `close_notify` alert and put it on the wire -- the first half of the
    /// ordinary TLS close, and the half a raw socket reads as pending data.
    /// @param wire The pipe the server reads from.
    void SendCloseNotify(InMemoryPipe& wire)
    {
        ERR_clear_error();
        static_cast<void>(SSL_shutdown(_ssl)); // 0 == our alert sent, peer's not seen
        Flush(wire);
    }

    /// Encrypt @p text as application data and put it on the wire.
    /// @param wire The pipe the server reads from.
    /// @param text Plaintext to send.
    void SendApplicationData(InMemoryPipe& wire, std::string_view text)
    {
        ERR_clear_error();
        REQUIRE(SSL_write(_ssl, text.data(), static_cast<int>(text.size())) == static_cast<int>(text.size()));
        Flush(wire);
    }

  private:
    SSL_CTX* _ctx { nullptr };
    SSL* _ssl { nullptr };
    BIO* _incoming { nullptr }; ///< network -> SSL; owned by _ssl.
};

/// What the server's handshake resolved to, published out of its detached task.
struct HandshakeOutcome
{
    bool done { false }; ///< The handshake task has finished.
    bool ok { false };   ///< ... and it succeeded.
};

/// What one `WaitReadable` resolved to, and whether it had resolved YET.
///
/// The `resolved` flag is what separates "parked until the peer acted" from
/// "answered on the spot" -- and a case that does not look at it passes under the
/// defect, because the pre-#712 delegation answers `1` synchronously.
struct ReadableOutcome
{
    bool resolved { false }; ///< The await has returned.
    bool hasValue { false }; ///< It carried a count rather than a NetError.
    std::size_t count { 0 }; ///< `0` == the peer has finished sending.
};

/// What one `Read` resolved to.
struct ReadOutcome
{
    bool resolved { false }; ///< The await has returned.
    bool hasValue { false }; ///< It carried bytes rather than a NetError.
    std::string text;        ///< Those bytes, as text.
};

/// Drive the server's handshake to completion in the background.
///
/// Detached rather than `SyncRun`, because the handshake genuinely suspends: it
/// parks on a raw read whenever the client has not spoken yet, and the test's own
/// loop is what unparks it.
/// @param server The TLS server socket.
/// @param out Where the outcome is published; must outlive the task.
/// @return The detached task.
DetachedTask DriveServerHandshake(TlsSocket* server, HandshakeOutcome* out)
{
    auto const result = co_await server->HandshakeIfNeeded();
    out->ok = result.has_value();
    out->done = true;
}

/// Park a `WaitReadable` on the server and publish what it answers.
/// @param server The TLS server socket.
/// @param out Where the observation is published; must outlive the task.
/// @return The detached task.
DetachedTask ObserveReadable(TlsSocket* server, ReadableOutcome* out)
{
    auto const readable = co_await server->WaitReadable();
    out->hasValue = readable.has_value();
    if (readable.has_value())
        out->count = *readable;
    out->resolved = true;
}

/// Read once from the server socket and publish what came back.
/// @param server The TLS server socket.
/// @param into Destination staging buffer; must outlive the task.
/// @param out Where the result is published; must outlive the task.
/// @return The detached task.
DetachedTask ObserveRead(TlsSocket* server, std::span<std::byte> into, ReadOutcome* out)
{
    auto const got = co_await server->Read(into);
    out->hasValue = got.has_value();
    if (got.has_value())
        out->text.assign(reinterpret_cast<char const*>(into.data()), *got);
    out->resolved = true;
}

/// One connected, handshaken TLS conversation: a real client, a real `TlsSocket`,
/// and the two pipes between them, which the test drives byte by byte.
struct TlsConversation
{
    std::shared_ptr<InMemoryPipe> toServer { std::make_shared<InMemoryPipe>() };
    std::shared_ptr<InMemoryPipe> toClient { std::make_shared<InMemoryPipe>() };
    TlsPeer client;
    std::unique_ptr<TlsSocket> server;

    /// Wire the server over the pipes and run the handshake to completion.
    /// @param context The server context holding the fixture certificate.
    explicit TlsConversation(TlsContext& context):
        server { std::make_unique<TlsSocket>(std::make_unique<InMemorySocket>(toServer, toClient), context) }
    {
        HandshakeOutcome outcome;
        DriveServerHandshake(server.get(), &outcome); // parks on its first raw read

        bool clientDone = false;
        for ([[maybe_unused]] auto const step: std::views::iota(0, 64))
        {
            if (clientDone && outcome.done)
                break;
            ERR_clear_error();
            int const r = SSL_do_handshake(client.Native());
            if (r == 1)
                clientDone = true;
            else
            {
                auto const err = SSL_get_error(client.Native(), r);
                REQUIRE((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE));
            }
            // Pushing resumes the server's parked raw read inline, so by the time
            // this returns the server has consumed the flight and answered.
            client.Flush(*toServer);
            client.Feed(*toClient);
        }

        REQUIRE(clientDone);
        REQUIRE(outcome.done);
        REQUIRE(outcome.ok);
    }
};

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

// -- #712: what a TLS peer's ordinary close looks like to WaitReadable -------
//
// **The raw socket cannot answer this, and the delegation hid that.** A TLS peer
// closes by sending a `close_notify` RECORD and only then the transport FIN, so at
// the instant it goes away there are bytes on the wire: `_raw->WaitReadable()`
// answers `>0` -- *data pending* -- for a peer that has finished sending, and #673's
// EOF arm therefore never fires for a TLS client. Over TLS that is the ORDINARY
// close, not a corner.
//
// **Platforms measured: Linux x86-64 only** (GCC 15 / libstdc++, OpenSSL 3.5). The
// cases run over `InMemoryPipe`s rather than kernel sockets, deliberately: the
// defect is in the decorator's own reasoning about records, so nothing here depends
// on a reactor and every CI leg built with `FASTCACHED_ENABLE_TLS=ON` runs these
// same assertions on its own platform. What is NOT covered here is a TLS client on a
// real socket with a parked `XREAD BLOCK 0`: that spans the daemon and lives outside
// `Net/`.

TEST_CASE("TlsSocket: WaitReadable reports EOF for a close_notify record", "[tls][net][waitreadable]")
{
    // The pure TLS case, and the one a raw peek cannot see AT ALL: the peer has said
    // "I have finished sending" in a record, and has not yet closed the transport. So
    // `Buffered() > 0` on the wire and `IsWriteClosed()` is false -- every signal the
    // pre-fix delegation had said "data pending".
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    TlsConversation talk { **context };

    talk.client.SendCloseNotify(*talk.toServer);
    REQUIRE(talk.toServer->Buffered() > 0);        // there ARE bytes on the wire
    REQUIRE_FALSE(talk.toServer->IsWriteClosed()); // and no transport FIN

    ReadableOutcome observed;
    ObserveReadable(talk.server.get(), &observed);

    REQUIRE(observed.resolved);
    REQUIRE(observed.hasValue); // EOF is an answer, not an error
    CHECK(observed.count == 0);
}

TEST_CASE("TlsSocket: WaitReadable reports EOF for close_notify followed by FIN", "[tls][net][waitreadable]")
{
    // The full, well-behaved close: alert, then FIN. Still `Buffered() > 0` when the
    // wait is asked, which is why the FIN does not rescue the delegating version --
    // `InMemorySocket::WaitReadable` reports EOF only once the buffer is EMPTY and
    // the write side is closed, and a real socket's `recv(MSG_PEEK)` reports the
    // pending alert bytes for exactly the same reason.
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    TlsConversation talk { **context };

    talk.client.SendCloseNotify(*talk.toServer);
    talk.toServer->CloseWrite();
    REQUIRE(talk.toServer->Buffered() > 0);

    ReadableOutcome observed;
    ObserveReadable(talk.server.get(), &observed);

    REQUIRE(observed.resolved);
    REQUIRE(observed.hasValue);
    CHECK(observed.count == 0);
}

TEST_CASE("TlsSocket: a PARKED WaitReadable resolves with EOF when the peer closes cleanly", "[tls][net][waitreadable]")
{
    // **The ticket's own sentence, as a test.** The wait is armed while the wire is
    // idle, so it must PARK; the peer then closes the way the protocol says to, and
    // the wait must come back saying the peer has finished sending.
    //
    // The `resolved` check before the close is what makes this case worth anything:
    // the pre-fix delegation answers `1` on the spot -- an idle in-memory wire is
    // neither drained-and-closed nor holding data -- so without it the case would be
    // asserting the wrong path while still going red, for the wrong reason.
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    TlsConversation talk { **context };

    ReadableOutcome observed;
    ObserveReadable(talk.server.get(), &observed);
    CHECK_FALSE(observed.resolved); // parked: nothing has happened on the wire yet

    talk.client.SendCloseNotify(*talk.toServer);
    talk.toServer->CloseWrite();

    REQUIRE(observed.resolved);
    REQUIRE(observed.hasValue);
    CHECK(observed.count == 0);
}

TEST_CASE("TlsSocket: WaitReadable reports data pending and consumes none of it", "[tls][net][waitreadable]")
{
    // **The control, and it is not optional**: without it "report EOF" and "report
    // EOF always" are the same passing test. The follow-up `Read` is the half that
    // matters -- the probe decrypts a record to answer, and every plaintext byte of
    // it has to still be there for the caller.
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    TlsConversation talk { **context };

    talk.client.SendApplicationData(*talk.toServer, "PING\r\n");

    ReadableOutcome observed;
    ObserveReadable(talk.server.get(), &observed);
    REQUIRE(observed.resolved);
    REQUIRE(observed.hasValue);
    CHECK(observed.count > 0);

    std::array<std::byte, 64> staging {};
    ReadOutcome got;
    ObserveRead(talk.server.get(), std::span<std::byte> { staging }, &got);
    REQUIRE(got.resolved);
    REQUIRE(got.hasValue);
    CHECK(got.text == "PING\r\n");
}

TEST_CASE("TlsSocket: a WaitReadable parked at teardown is retrieved, not leaked", "[tls][net][waitreadable]")
{
    // **The lifetime half, and the one this fix newly makes reachable.** Before #712
    // a TLS `WaitReadable` delegated to the raw socket, and over the in-memory wire
    // that answers synchronously -- so nothing ever parked and there was no frame to
    // lose. Now it parks a detached pump on a raw `Read`, and the only thing that can
    // retrieve a parked wait is `Close()`.
    //
    // What this asserts is that the wait RESOLVES, because a wait that never resolves
    // is a coroutine frame nobody frees. The leak itself is caught by the sanitizer
    // rather than by an assertion: a parked frame is a live unreachable allocation at
    // exit, which LeakSanitizer reports as a direct leak naming the coroutine. So this
    // case is red two ways under the defect -- the `resolved` check fails here, and
    // the ASan legs fail the whole binary.
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());

    ReadableOutcome observed;
    {
        TlsConversation talk { **context };
        ObserveReadable(talk.server.get(), &observed);
        REQUIRE_FALSE(observed.resolved); // parked, with nothing on the wire
    }
    // The conversation -- and with it the TlsSocket and its raw transport -- is gone.
    // The peer did nothing: no data, no `close_notify`, no FIN. The only reason the
    // pump can come back is teardown retrieving it.
    CHECK(observed.resolved);
    CHECK_FALSE(observed.hasValue); // a cancelled wait is an error, not an EOF count
}

TEST_CASE("TlsSocket: WaitReadable reports EOF for a truncated stream", "[tls][net][waitreadable]")
{
    // A FIN with no `close_notify` at all. This one the delegation already answered
    // correctly, and it is here so the fix is pinned on BOTH sides of the record: a
    // peer that vanishes without saying goodbye has also finished sending, and must
    // not start reporting data pending now that the answer comes from OpenSSL.
    auto context = TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    TlsConversation talk { **context };

    talk.toServer->CloseWrite();

    ReadableOutcome observed;
    ObserveReadable(talk.server.get(), &observed);
    REQUIRE(observed.resolved);
    REQUIRE(observed.hasValue);
    CHECK(observed.count == 0);
}

#endif // FC_TLS_ENABLED
