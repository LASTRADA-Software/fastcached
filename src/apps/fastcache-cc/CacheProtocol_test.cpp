// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// An ISocket that replays canned reply bytes and records what was sent.
///
/// `Net/TcpClient_test` drives the transfer loops themselves and
/// `Net/BlockingSocket_test` drives a real loopback peer; neither can pose as a
/// daemon of the wrong version. This fake can, and it exposes its read cursor so
/// a test can prove the client consumed exactly one reply frame and no more.
///
/// It takes and returns whole buffers in one call, which is what makes the trace
/// below readable -- the partial-transfer behaviour is `SendAll`/`RecvExactly`'s
/// own business and is tested where those live.
class ScriptedTcpClient final: public ISocket
{
  public:
    /// @param replies The bytes the "daemon" will return, in order.
    explicit ScriptedTcpClient(std::vector<std::byte> replies):
        _replies(std::move(replies))
    {
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
    {
        ++_sendCalls;
        _trace.push_back('S');
        _sent.insert(_sent.end(), bytes.begin(), bytes.end());
        return IoAwaitable { IoResult { bytes.size() } };
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        // Recorded before the short-read check: an attempted read is still a read
        // for the purpose of "did this client wait for a reply mid-conversation".
        if (_trace.empty() || _trace.back() != 'R')
            _trace.push_back('R');
        auto const available = _replies.size() - _cursor;
        auto const take = std::min(available, buffer.size());
        // Zero is EOF, which is how RecvExactly learns the peer ran out -- the
        // same thing the old fake said by returning nullopt on a short read.
        std::copy_n(_replies.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
        _cursor += take;
        return IoAwaitable { IoResult { take } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        return IoAwaitable { IoResult { 0 } };
    }

    void Close() noexcept override
    {
        _closed = true;
    }
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return "scripted";
    }

    /// Everything the client wrote.
    [[nodiscard]] std::vector<std::byte> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many reply bytes the client consumed.
    [[nodiscard]] std::size_t Cursor() const noexcept
    {
        return _cursor;
    }

    /// How many separate writes the client made.
    [[nodiscard]] std::size_t SendCalls() const noexcept
    {
        return _sendCalls;
    }

    /// The order in which the client wrote and read, collapsed to one character
    /// per run: "SSRR" is two writes then two reads, "SRSR" is a round trip
    /// between them.
    ///
    /// This, not the write count, is what pipelining actually means. Two SendAll
    /// calls back-to-back are exactly as pipelined as one concatenated buffer --
    /// neither waits for a reply -- and demanding a single write would force the
    /// client to COPY a STORE frame carrying a whole object file just to satisfy
    /// the test. Asserting the interleaving instead states the round-trip property
    /// directly and leaves the client free to avoid the copy.
    [[nodiscard]] std::string const& Trace() const noexcept
    {
        return _trace;
    }

  private:
    std::vector<std::byte> _replies;
    std::vector<std::byte> _sent;
    std::size_t _cursor { 0 };
    std::size_t _sendCalls { 0 };
    std::string _trace;
    bool _closed { false };
};

/// A client whose socket fails on every call.
class FailingTcpClient final: public ISocket
{
  public:
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> /*bytes*/) override
    {
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted write failure" }) };
    }
    [[nodiscard]] IoAwaitable Read(std::span<std::byte> /*buffer*/) override
    {
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted read failure" }) };
    }
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        return IoAwaitable { IoResult { 0 } };
    }
    void Close() noexcept override {}
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return "failing";
    }
};

} // namespace

TEST_CASE("CacheFetch sends exactly what the wire module specifies")
{
    // Asserting against the shared encoder rather than a hand-written literal is
    // what makes client and server agree by construction: both sides frame
    // through the same function, so they cannot drift apart independently.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
    (void) SyncRun(CacheFetch(&client, "the-key"));

    CHECK(client.Sent() == Wire::EncodeFetch("the-key"));
}

TEST_CASE("CacheFetch returns the payload on a hit")
{
    auto const stored = std::vector<std::byte> { std::byte { 0xDE }, std::byte { 0xAD } };
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, stored) };

    auto const outcome = SyncRun(CacheFetch(&client, "k"));
    CHECK(outcome.kind == CacheOutcomeKind::Hit);
    CHECK(outcome.IsHit());
    CHECK(outcome.value == stored);
}

TEST_CASE("CacheFetch reports a version rejection distinctly from a miss")
{
    // The defect this whole change exists to fix. On the pre-version wire both
    // answers were the byte 0x00, so a launcher pointed at a daemon that could
    // not serve it reported "not cached" forever and the build merely got
    // slower with no explanation.
    auto const missOutcome = [] {
        ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
        return SyncRun(CacheFetch(&client, "k"));
    }();

    auto const rejectedOutcome = [] {
        ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                                          "unsupported wire version 2; this server speaks 1..1") };
        return SyncRun(CacheFetch(&client, "k"));
    }();

    CHECK(missOutcome.kind == CacheOutcomeKind::Miss);
    CHECK(rejectedOutcome.kind == CacheOutcomeKind::Rejected);
    CHECK(rejectedOutcome.kind != missOutcome.kind);

    CHECK(rejectedOutcome.code == Wire::ErrorCode::UnsupportedVersion);
    // The daemon's own words must reach the caller: they name the supported
    // range, which is the only actionable part of the diagnosis.
    CHECK(rejectedOutcome.message.contains("1..1"));
    CHECK(DescribeOutcome(rejectedOutcome).contains("unsupported-version"));
    CHECK(DescribeOutcome(rejectedOutcome).contains("1..1"));
}

TEST_CASE("CacheStore drains a refusal by its declared length")
{
    // Prove the reply is consumed whole. The old client read one acknowledgement
    // byte and left the error's message in the socket; harmless only because a
    // connection was never reused, and a desynchronisation waiting to happen.
    auto reply = Wire::EncodeErrorReply(Wire::ErrorCode::StorageWriteFailed, "storage write failed");
    auto const replyLength = reply.size();

    // A sentinel the client must NOT touch.
    auto const sentinel = std::vector<std::byte> { std::byte { 0x5A }, std::byte { 0x5A }, std::byte { 0x5A } };
    reply.insert(reply.end(), sentinel.begin(), sentinel.end());

    ScriptedTcpClient client { reply };
    auto const value = std::vector<std::byte> { std::byte { 0x01 } };
    auto const outcome = SyncRun(CacheStore(&client,
                                            Wire::StoreRequest { .key = "k",
                                                                 .prefetchGroup = "c",
                                                                 .srcRoot = "/s",
                                                                 .buildTree = "/b",
                                                                 .value = std::span<std::byte const> { value } }));

    CHECK(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(outcome.code == Wire::ErrorCode::StorageWriteFailed);
    CHECK(client.Cursor() == replyLength);
}

TEST_CASE("CacheStore sends exactly what the wire module specifies")
{
    auto const value = std::vector<std::byte> { std::byte { 0x77 } };
    Wire::StoreRequest const request {
        .key = "k", .prefetchGroup = "c", .srcRoot = "/s", .buildTree = "/b", .value = std::span<std::byte const> { value }
    };

    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, {}) };
    auto const outcome = SyncRun(CacheStore(&client, request));

    CHECK(outcome.kind == CacheOutcomeKind::Hit);
    CHECK(client.Sent() == Wire::EncodeStore(request));
}

TEST_CASE("A transport failure is not mistaken for a miss or a refusal")
{
    // A build must fall back to a real compile in every one of these cases, but
    // only one of them is worth telling the user about.
    SECTION("send fails")
    {
        FailingTcpClient client;
        CHECK(SyncRun(CacheFetch(&client, "k")).kind == CacheOutcomeKind::Transport);
    }

    SECTION("the peer closes before a full reply header")
    {
        ScriptedTcpClient client { std::vector<std::byte> { std::byte { 0x01 } } };
        CHECK(SyncRun(CacheFetch(&client, "k")).kind == CacheOutcomeKind::Transport);
    }

    SECTION("the peer declares more payload than it sends")
    {
        auto truncated = Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte>(8, std::byte { 0x11 }));
        truncated.resize(truncated.size() - 2);
        ScriptedTcpClient client { truncated };
        CHECK(SyncRun(CacheFetch(&client, "k")).kind == CacheOutcomeKind::Transport);
    }
}

TEST_CASE("Only a daemon that answered is one worth sending a second command to")
{
    // What this predicate answers, and the far more important thing it does not,
    // are in its own doc comment. Here it is only pinned: a daemon that answered
    // -- with the value or without it -- is serving, and one that refused the
    // command or was never reached is not.
    SECTION("an answer about the key, with or without a value")
    {
        CHECK(CacheIsServing(CacheOutcomeKind::Hit));
        CHECK(CacheIsServing(CacheOutcomeKind::Miss));
    }

    SECTION("no answer about the key at all")
    {
        CHECK_FALSE(CacheIsServing(CacheOutcomeKind::Rejected));
        CHECK_FALSE(CacheIsServing(CacheOutcomeKind::Transport));
    }

    SECTION("the default-constructed outcome is not serving")
    {
        // `CacheOutcome` defaults to `Transport` precisely so a code path that
        // forgot to fill one in fails safe. The predicate has to agree, or a
        // never-completed exchange would be offered a store.
        CHECK_FALSE(CacheIsServing(CacheOutcome {}.kind));
    }
}

TEST_CASE("The store-size limit declines the pathological value and nothing else")
{
    // A compiler cache must never fail a build, so the ceiling's job is to skip
    // one cache write, not to become a new way to break: everything at or under
    // it stores, and a zero limit disables the check rather than rejecting
    // everything -- the reading that would silently turn caching off wholesale.
    SECTION("under, at, and over the limit")
    {
        CHECK(IsStorableSize(0, 1024));
        CHECK(IsStorableSize(1023, 1024));
        CHECK(IsStorableSize(1024, 1024)); // the limit is inclusive
        CHECK_FALSE(IsStorableSize(1025, 1024));
    }

    SECTION("zero means no limit, not zero bytes")
    {
        CHECK(IsStorableSize(0, 0));
        CHECK(IsStorableSize(1, 0));
        CHECK(IsStorableSize(DefaultMaxStoreBytes * 4, 0));
    }

    SECTION("the default admits an ordinary object and declines the reported one")
    {
        // 356 MB of C++23 templates plus -g debug info is what issue #68 was
        // reported against; a few MB is what an ordinary translation unit emits.
        CHECK(IsStorableSize(4UL * 1024UL * 1024UL, DefaultMaxStoreBytes));
        CHECK_FALSE(IsStorableSize(356UL * 1000UL * 1000UL, DefaultMaxStoreBytes));
    }
}

// --- credentials ------------------------------------------------------------

namespace
{

/// Concatenate reply frames into one scripted stream.
[[nodiscard]] std::vector<std::byte> Replies(std::initializer_list<std::vector<std::byte>> frames)
{
    std::vector<std::byte> out;
    for (auto const& frame: frames)
        out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

/// A credential with a secret, i.e. one that will actually be presented.
[[nodiscard]] Credential Token(std::string_view secret, std::string_view username = {})
{
    return Credential { .username = std::string { username }, .secret = std::string { secret } };
}

} // namespace

TEST_CASE("An unconfigured credential changes nothing on the wire")
{
    // The migration property: a launcher that has never been given a token must
    // send byte-for-byte what it always sent. Anything else would make upgrading
    // the launcher a wire change for every daemon in a fleet.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
    (void) SyncRun(CacheFetch(&client, "the-key", Credential {}));

    CHECK(client.Sent() == Wire::EncodeFetch("the-key"));
}

TEST_CASE("A username without a secret is not a credential")
{
    // FASTCACHE_USER alone is a misconfiguration, not a request to authenticate.
    // Sending an AUTH carrying an empty secret would be refused by every server
    // that requires one, turning a harmless typo into a build with no cache.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
    (void) SyncRun(CacheFetch(&client, "k", Credential { .username = "bob", .secret = "" }));

    CHECK(client.Sent() == Wire::EncodeFetch("k"));
}

TEST_CASE("A credential is pipelined ahead of the command, with no round trip between them")
{
    // The round-trip-count property, and the reason it is asserted on the write/read
    // ORDER rather than on the outcome: correctness would also hold if the client
    // sent AUTH, waited for its reply, then sent the FETCH -- and that spelling
    // costs a round trip per translation unit, which on the launcher's
    // connection-per-operation model is the regression the wire's "no handshake"
    // note exists to prevent. "SSR" is both frames out before either reply is
    // read; the failure this rejects would read "SRSR".
    ScriptedTcpClient client { Replies(
        { Wire::EncodeReply(Wire::Status::Ok, {}), Wire::EncodeReply(Wire::Status::Miss, {}) }) };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("s3cret")));
    CHECK(outcome.kind == CacheOutcomeKind::Miss);

    auto expected = Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = "s3cret" });
    auto const fetch = Wire::EncodeFetch("k");
    expected.insert(expected.end(), fetch.begin(), fetch.end());
    CHECK(client.Sent() == expected);
    // Both frames go out before either reply is read. The write COUNT is
    // deliberately not asserted here: two back-to-back writes are just as
    // pipelined as one, and requiring a single write would mean copying the
    // command frame for nothing.
    CHECK(client.Trace() == "SSR");
}

TEST_CASE("A hit behind a credential is served, and both replies are consumed")
{
    // Leaving the AUTH reply in the socket would be invisible here but fatal on a
    // reused connection: the next command would read the previous command's
    // answer. Asserting the cursor is what proves the stream stayed in sync.
    auto const stored = std::vector<std::byte> { std::byte { 0xBE }, std::byte { 0xEF } };
    auto const script = Replies({ Wire::EncodeReply(Wire::Status::Ok, {}), Wire::EncodeReply(Wire::Status::Ok, stored) });
    ScriptedTcpClient client { script };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("s3cret")));
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);
    CHECK(client.Cursor() == script.size());
}

TEST_CASE("A rejected credential surfaces as the credential's refusal, not the command's")
{
    // Both replies arrive; the AUTH refusal is the one that explains the build,
    // so it is the one returned. Reporting the command's own Unauthenticated
    // instead would say "this key is not available" when the truth is "this
    // launcher's token is wrong" -- and the two want different fixes.
    auto const script = Replies({ Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, "authentication failed"),
                                  Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, {}) });
    ScriptedTcpClient client { script };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("wrong")));
    REQUIRE(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(outcome.code == Wire::ErrorCode::Unauthenticated);
    CHECK(outcome.message == "authentication failed");
    CHECK(DescribeOutcome(outcome).contains("unauthenticated"));

    // The command's reply is drained even though it is discarded, so the
    // connection is left usable rather than one frame behind.
    CHECK(client.Cursor() == script.size());
}

TEST_CASE("CacheStore presents the credential the same way CacheFetch does")
{
    auto const body = std::vector<std::byte> { std::byte { 0x01 } };
    ScriptedTcpClient client { Replies(
        { Wire::EncodeReply(Wire::Status::Ok, {}), Wire::EncodeReply(Wire::Status::Ok, {}) }) };

    auto const request = Wire::StoreRequest { .key = "k",
                                              .prefetchGroup = "g",
                                              .srcRoot = "/src",
                                              .buildTree = "/build",
                                              .value = std::span<std::byte const> { body } };
    auto const outcome = SyncRun(CacheStore(&client, request, Token("s3cret", "bob")));
    CHECK(outcome.IsHit());

    auto expected = Wire::EncodeAuth(Wire::AuthRequest { .username = "bob", .secret = "s3cret" });
    auto const store = Wire::EncodeStore(request);
    expected.insert(expected.end(), store.begin(), store.end());
    CHECK(client.Sent() == expected);
    CHECK(client.Trace() == "SSR");

    // A STORE frame carries the object file, so the credential must not be
    // prepended by copying it: that would raise peak footprint from about twice
    // the object to three times it, on the hot path of a parallel build, and buy
    // no round trip at all.
    CHECK(client.SendCalls() == 2);
}

TEST_CASE("A daemon that dies after the AUTH reply is a transport failure, not a hit")
{
    // A truncated stream must not be read as success. The client asked for two
    // replies and got one, so there is no command outcome to report.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, {}) };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("s3cret")));
    CHECK(outcome.kind == CacheOutcomeKind::Transport);
    CHECK_FALSE(outcome.IsHit());
}

TEST_CASE("A daemon predating the AUTH verb still serves the command behind it")
{
    // The mixed-fleet case, and the one this whole design promises is safe. The
    // AUTH opcode was added WITHOUT bumping the wire version -- deliberately, since
    // the framing was built so a receiver can step over a verb it does not know --
    // so an older daemon answers AUTH `unknown-opcode` and then serves the
    // pipelined command perfectly well.
    //
    // Treating that refusal as the exchange's outcome would give a
    // token-configured launcher a permanent 0% hit rate against every
    // not-yet-upgraded daemon, reported as `rejected (unknown-opcode)` -- a
    // regression with no obvious cause and a plausible-looking error message.
    auto const stored = std::vector<std::byte> { std::byte { 0x42 } };
    ScriptedTcpClient client { Replies({ Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode, "unknown opcode 0x03"),
                                         Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("s3cret")));
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);

    // ...but the operator asked for authentication and did not get it. A cache
    // that silently does less than it was told to is worse than one that says so.
    CHECK(outcome.credentialIgnored);
}

TEST_CASE("An ordinary refusal is not mistaken for an absent AUTH verb")
{
    // The complement of the case above: only `unknown-opcode` means "this daemon
    // has no AUTH verb". Every other refusal is about the credential itself and
    // must still be reported, or a wrong token would silently degrade to
    // unauthenticated access against a daemon that does require one.
    ScriptedTcpClient client { Replies({ Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, "authentication failed"),
                                         Wire::EncodeErrorReply(Wire::ErrorCode::Unauthenticated, {}) }) };

    auto const outcome = SyncRun(CacheFetch(&client, "k", Token("wrong")));
    CHECK(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(outcome.code == Wire::ErrorCode::Unauthenticated);
    CHECK_FALSE(outcome.credentialIgnored);
}

TEST_CASE("An uncredentialed exchange never reports an ignored credential")
{
    // `credentialIgnored` must mean "you asked for authentication and did not get
    // it", not "no AUTH frame was involved". A launcher with no token configured
    // asked for nothing and must say nothing.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Miss, {}) };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));
    CHECK_FALSE(outcome.credentialIgnored);
}

TEST_CASE("An empty NotLeader message reaches the wire as prose, not as nothing")
{
    // The premise the whole redirect rests on, pinned against the REAL encoder
    // rather than against a comment. `SchedulerService` hands `Refuse` an empty
    // string when no election has concluded (`SchedulerService_test`, "an undecided
    // node refuses, and names nobody") -- but `EncodeErrorReply` substitutes the
    // error table's default sentence for an empty message, so what a client
    // actually receives is "this node does not lead the cluster".
    //
    // If this ever stopped being true, testing `message.empty()` would become a
    // correct way to spot an election and `RedirectTarget`'s parse would be
    // needless ceremony. It is the reason the parse exists, so it is asserted here
    // and not merely asserted about.
    ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, {}) };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));

    REQUIRE(outcome.kind == CacheOutcomeKind::Rejected);
    REQUIRE(outcome.code == Wire::ErrorCode::NotLeader);
    CHECK_FALSE(outcome.message.empty());
    CHECK(RedirectTarget(outcome) == std::nullopt);
}

TEST_CASE("RedirectTarget returns the endpoint a NotLeader names")
{
    // The other half of the same round trip: a follower names the leader, and that
    // string is what the client must dial. Driven through the encoder for the same
    // reason as above -- a hand-built `CacheOutcome` would prove only that the
    // struct can hold an endpoint.
    ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "10.0.0.1:7000") };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));

    REQUIRE(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(RedirectTarget(outcome) == std::optional<std::string> { "10.0.0.1:7000" });
}

TEST_CASE("RedirectTarget parses an endpoint rather than merely splitting one")
{
    // `SplitHostPort` takes the LAST colon and hands back whatever follows it, so a
    // sentence containing a colon splits perfectly happily. `ClusterAdminCli` only
    // ever PRINTED this message; since #237 a launcher DIALS it, which is the moment
    // the message stopped being a note for a person and became data a machine acts
    // on -- and the point at which "it splits" stopped being a sufficient test.
    //
    // Every row here splits. None of them is an address.
    auto const prose = GENERATE(as<std::string_view> {},
                                "this node does not lead the cluster", // the table's default: no colon at all
                                "no leader: try again",                // splits; the port is not a number
                                "leader is at host",                   // no colon
                                ":7000",                               // splits; no host
                                "[]:7000",                             // splits; no host, bracketed form
                                "10.0.0.1:",                           // splits; no port
                                "10.0.0.1:99999");                     // splits; the port does not fit

    CAPTURE(prose);
    ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, prose) };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));

    REQUIRE(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(RedirectTarget(outcome) == std::nullopt);
}

TEST_CASE("Only NotLeader is an instruction, whatever else a refusal carries")
{
    // A refusal that happens to name an address is still an answer about the fleet.
    // `NoWorker` with a message that parses must not become a redirect, or a client
    // would follow a diagnostic somewhere no scheduler is listening.
    ScriptedTcpClient client { Wire::EncodeErrorReply(Wire::ErrorCode::NoWorker, "10.0.0.1:7000") };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));

    REQUIRE(outcome.kind == CacheOutcomeKind::Rejected);
    CHECK(RedirectTarget(outcome) == std::nullopt);
}

TEST_CASE("A served object is not a redirect")
{
    // The `kind` test, which costs one branch and rules out every non-refusal
    // outcome: a served object whose bytes happened to read as `host:port` must
    // never be mistaken for somewhere else to ask.
    ScriptedTcpClient client { Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes(std::string_view { "10.0.0.1:7000" })) };
    auto const outcome = SyncRun(CacheFetch(&client, "k", Credential {}));

    REQUIRE(outcome.kind == CacheOutcomeKind::Hit);
    CHECK(RedirectTarget(outcome) == std::nullopt);
}
