// SPDX-License-Identifier: Apache-2.0
//
// The inbound peer listener. Two properties matter most. Nothing is read from a
// connection that has not proved a member's id, and every way one is refused moves
// its own counter (#1308, #178). And a frame this build cannot interpret must be stepped
// over rather than end the connection, because a node running a newer build would
// otherwise partition itself from every older peer in a fleet nobody upgrades
// atomically.
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <tests/HalfClose.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

/// Records what the server decoded, and runs a case's own step on each delivery.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        received.push_back(std::move(message));
        if (onDelivery)
            onDelivery();
    }

    std::vector<RaftMessage> received; ///< In arrival order.

    /// Called after each delivery; what a case uses to change the world between two frames.
    std::function<void()> onDelivery;
};

/// The server's id in every case here. Dialled as, and never the sender of a message.
constexpr std::string_view ServerId = "n1";

/// The dialler's id: the voter in every vote these cases send.
constexpr std::string_view DiallerId = "n2";

/// The roster both ends of a case hold: every member, each under its own key.
[[nodiscard]] std::shared_ptr<Testing::SharedRoster> Roster()
{
    return Testing::SharedRoster::Of({ "n1", "n2", "n3" });
}

/// The bytes the server draws -- its nonce, then its ephemeral secret -- so a case can answer
/// its challenge without reading it, which is what lets a whole connection be written before the
/// server runs. Exactly one nonce long: the script cycles, so the ephemeral secret is the same
/// bytes again, which is a legal X25519 secret like any other.
[[nodiscard]] std::vector<std::byte> ServerScript()
{
    std::vector<std::byte> script;
    for (auto const fill: { 0x11U, 0x22U, 0x33U, 0x44U })
        script.insert(script.end(), NonceBytes / 4, static_cast<std::byte>(fill));
    return script;
}

/// The server's half of a handshake as the server under test will run it: the same identity
/// and the same script, so the same challenge -- and, since a signature is deterministic, the
/// same verdict for the same proof.
struct MirroredServer
{
    Testing::TestPeerIdentity identity;
    Testing::ScriptedSecureRandom random { ServerScript() };
    AcceptorHandshake handshake;

    /// @param roster What the server believes about everybody's keys.
    explicit MirroredServer(std::shared_ptr<Testing::SharedRoster const> const& roster):
        identity { NodeId { ServerId }, Testing::TestKeyPair(std::string { ServerId }), roster },
        handshake { AcceptorHandshake::Create(identity, random).value() }
    {
    }
};

/// The challenge the server will send, from the same script it draws from.
[[nodiscard]] RaftWire::ChallengeFrame ExpectedChallenge()
{
    MirroredServer const mirror { Roster() };
    return mirror.handshake.Challenge();
}

/// A vote response carrying `term`, the smallest message that round-trips.
/// @param term Term to carry.
/// @param voter Who it names as its sender.
/// @return The framed message, untagged.
[[nodiscard]] std::vector<std::byte> VoteFrame(std::uint64_t term, std::string_view voter = DiallerId)
{
    return RaftWire::Encode(RaftMessage { RequestVoteResponse {
        .term = Term { .value = term }, .decision = VoteDecision::Granted, .voterId = NodeId { voter } } });
}

/// Write every byte of `bytes` to `socket`.
///
/// By pointer, not reference: a coroutine's reference parameter is bound before
/// the first suspension and outlives the frame that kept it alive.
/// @param socket Destination; never null.
/// @param bytes What to send.
/// @return Whether all of it was accepted.
[[nodiscard]] core::async::Task<bool> WriteAll(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    auto const written = co_await socket->write(bytes);
    co_return written.has_value() && *written == bytes.size();
}

/// Read what is buffered for `socket`.
/// @param socket Source; never null.
/// @param buffer Where to put it.
/// @return How many bytes arrived; zero at the end or on an error.
[[nodiscard]] core::async::Task<std::size_t> ReadSome(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    auto const read = co_await socket->read(buffer);
    co_return read.has_value() ? *read : std::size_t { 0 };
}

/// Append `bytes` to `out`.
/// @param out Destination.
/// @param bytes What to append.
void Append(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// The proof @p handshake sends in answer to `ExpectedChallenge()`.
/// @param handshake The dialler's half.
/// @return The proof; the case fails if the dialler refuses to send one.
[[nodiscard]] RaftWire::ProofFrame AnswerExpectedChallenge(DiallerHandshake& handshake)
{
    auto proof = handshake.Answer(ExpectedChallenge());
    REQUIRE(proof.has_value());
    return *std::move(proof);
}

/// Seal @p frame, split the way a writer sends it.
/// @param sealer The session's sealer.
/// @param frame A frame as `RaftWire::Encode` produced it.
/// @return Its tag.
[[nodiscard]] SessionTag SealWhole(FrameSealer& sealer, std::span<std::byte const> frame)
{
    return sealer.Seal(frame.first(RaftWire::HeaderSize), frame.subspan(RaftWire::HeaderSize));
}

/// A session key nobody at either end agreed: what a machine without the session holds.
/// @return The key.
[[nodiscard]] SessionKey StrangerSession()
{
    SystemSecureRandom random;
    auto const secret = DrawNonce(random).value();
    return DeriveSessionKey(secret, secret, WireFields::AsBytes("a session nobody agreed")).value();
}

/// Who dials, as whom, and what it holds.
struct DiallerShape
{
    std::string id { DiallerId };      ///< The id it proves itself as.
    std::string target { ServerId };   ///< The id it believes it dialled.
    std::string machine { DiallerId }; ///< Whose private key it signs with.

    /// What it believes about everybody's keys, and what the mirrored server judges it by.
    std::shared_ptr<Testing::SharedRoster const> roster { Roster() };
};

/// Everything one dialler sends on one connection, built before the server runs.
///
/// The proof answers `ExpectedChallenge()`, so it is the proof the server's real challenge
/// will be answered by, and the verdict a mirrored server returns for it is the one the real
/// server will send -- which is what gives the dialler its session before any byte is written.
/// Every frame after the proof is sealed in that session, in order; a dialler the server will
/// refuse has no session, and its frames carry tags nothing verifies.
class Dialler
{
  public:
    /// @param shape Who dials, as whom, and what it holds.
    explicit Dialler(DiallerShape const& shape = {}):
        _identity { NodeId { shape.id }, Testing::TestKeyPair(shape.machine), shape.roster },
        _handshake { DiallerHandshake::Create(_identity, NodeId { shape.target }, _random).value() },
        _proof { AnswerExpectedChallenge(_handshake) },
        _wire { RaftWire::EncodeProof(_proof) }
    {
        MirroredServer mirror { shape.roster };
        auto const judgement = mirror.handshake.Judge(_proof);
        if (!judgement.verdict.has_value())
            return;
        auto conclusion = _handshake.Conclude(*judgement.verdict);
        if (conclusion.session.has_value())
            _sealer.emplace(*std::move(conclusion.session));
    }

    /// Seal @p frame in this session and append it.
    /// @param frame A frame as `RaftWire::Encode` produced it.
    /// @return The frame and its tag, as appended.
    std::vector<std::byte> Send(std::vector<std::byte> const& frame)
    {
        auto sealed = frame;
        auto const tag = _sealer.has_value() ? SealWhole(*_sealer, frame) : SessionTag {};
        Append(sealed, tag);
        Append(_wire, sealed);
        return sealed;
    }

    /// Append bytes as they are, sealed by nothing.
    /// @param bytes What to append.
    void SendRaw(std::span<std::byte const> bytes)
    {
        Append(_wire, bytes);
    }

    /// @return Everything to send, proof first.
    [[nodiscard]] std::vector<std::byte>& Wire() noexcept
    {
        return _wire;
    }

    /// @return The proof this dialler sent.
    [[nodiscard]] RaftWire::ProofFrame const& Proof() const noexcept
    {
        return _proof;
    }

    /// @return This dialler's half of the handshake, to read a verdict with.
    [[nodiscard]] DiallerHandshake const& Handshake() const noexcept
    {
        return _handshake;
    }

  private:
    Testing::TestPeerIdentity _identity;
    SystemSecureRandom _random;
    DiallerHandshake _handshake;
    RaftWire::ProofFrame _proof;
    std::optional<FrameSealer> _sealer;
    std::vector<std::byte> _wire;
};

/// One run of the server over one connection, and what came of it.
struct Served
{
    std::unique_ptr<AtomicMetricsSink> metrics { std::make_unique<AtomicMetricsSink>() };
    std::unique_ptr<RaftPeerServer> server;

    /// What the server wrote back: its challenge, then any verdict.
    std::vector<std::byte> replied;

    /// @param refusal A refusal.
    /// @return How many times it was counted.
    [[nodiscard]] std::uint64_t Refused(AcceptorRefusal refusal) const
    {
        return metrics->Read(RowFor(refusal).counter);
    }

    /// @return How many refusals of any kind were counted.
    [[nodiscard]] std::uint64_t AnyRefusals() const
    {
        auto total = std::uint64_t { 0 };
        for (auto const& row: AcceptorRefusals)
            total += metrics->Read(row.counter);
        return total;
    }

    /// The verdict the server wrote after its challenge, if it wrote one.
    /// @return The decoded verdict.
    [[nodiscard]] std::optional<RaftWire::VerdictFrame> Verdict() const
    {
        auto const bytes = std::span<std::byte const> { replied };
        auto const challengeHeader = RaftWire::DecodeHeader(bytes);
        if (!challengeHeader.has_value())
            return std::nullopt;
        auto const rest = bytes.subspan(RaftWire::HeaderSize + challengeHeader->payloadLength);
        auto const header = RaftWire::DecodeHeader(rest);
        if (!header.has_value())
            return std::nullopt;
        auto verdict = RaftWire::DecodeVerdict(*header, rest.subspan(RaftWire::HeaderSize, header->payloadLength));
        return verdict.has_value() ? std::optional { *std::move(verdict) } : std::nullopt;
    }
};

/// Drive a server over one in-memory connection carrying `wire`, to completion.
///
/// The client half writes everything and then shuts its write side, so the server's
/// reader sees a clean EOF and its connection coroutine finishes -- which is what lets
/// each case assert on a settled result rather than racing one. Its READ side stays
/// open, so the case can see what the server wrote back.
///
/// The handshake bound is off: the reactor here is never turned, so a deadline could
/// never fire and would only be a timer left parked on it. The bound has a case of its
/// own over a `core::net::testing::TestLoop`.
/// @param wire The bytes a peer sends.
/// @param sink Where decoded messages land.
/// @param random Where the server draws its challenge from.
/// @param logger Where the server reports.
/// @param options Limits to run the server under.
/// @param roster What the server believes about everybody's keys.
/// @return The server and what it counted and wrote, after its accept loop has ended.
[[nodiscard]] Served RunOnceWith(std::vector<std::byte> const& wire,
                                 RecordingSink& sink,
                                 ISecureRandom& random,
                                 ILogger& logger,
                                 PeerServerOptions options = { .handshakeBound = 0ms },
                                 std::shared_ptr<Testing::SharedRoster const> const& roster = Roster())
{
    Served served;
    core::net::testing::InMemoryListener listener;
    core::platform::SteadyClock clock;
    core::net::PlatformLoop reactor { clock };
    Testing::TestPeerIdentity const identity { NodeId { ServerId }, Testing::TestKeyPair(std::string { ServerId }), roster };
    served.server =
        std::make_unique<RaftPeerServer>(listener, reactor, sink, logger, *served.metrics, identity, random, options);

    auto client = listener.connectClient();
    if (!wire.empty())
        REQUIRE(core::async::syncRun(WriteAll(client.get(), wire)));
    REQUIRE(FastCache::Testing::ShutdownWrite(*client).has_value());

    // The listener drains queued connections before reporting itself closed, so
    // closing here still delivers the one connection and then ends the loop.
    listener.close();
    core::async::syncRun(served.server->Run());

    auto buffer = std::array<std::byte, 4096> {};
    while (true)
    {
        auto const got = core::async::syncRun(ReadSome(client.get(), buffer));
        if (got == 0)
            break;
        Append(served.replied, std::span<std::byte const> { buffer }.first(got));
    }
    return served;
}

/// `RunOnceWith` over the scripted challenge every case here answers, reporting nowhere.
/// @param wire The bytes a peer sends.
/// @param sink Where decoded messages land.
/// @param options Limits to run the server under.
/// @param roster What the server believes about everybody's keys.
/// @return The server and what it counted and wrote, after its accept loop has ended.
[[nodiscard]] Served RunOnce(std::vector<std::byte> const& wire,
                             RecordingSink& sink,
                             PeerServerOptions options = { .handshakeBound = 0ms },
                             std::shared_ptr<Testing::SharedRoster const> const& roster = Roster())
{
    Testing::ScriptedSecureRandom random { ServerScript() };
    NullLogger logger;
    return RunOnceWith(wire, sink, random, logger, options, roster);
}

} // namespace

TEST_CASE("A framed message on a connection that proved its id is decoded and delivered", "[consensus][raft][peerserver]")
{
    Dialler dialler;
    dialler.Send(VoteFrame(7));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    REQUIRE(sink.received.size() == 1);
    REQUIRE(std::holds_alternative<RequestVoteResponse>(sink.received[0]));
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 7 });
    CHECK(served.server->DeliveredMessages() == 1);
    CHECK(served.AnyRefusals() == 0);

    // And the server said yes, signed: the verdict the dialler's own half accepts.
    auto const verdict = served.Verdict();
    REQUIRE(verdict.has_value());
    CHECK(Unwrap(verdict).verdict == RaftWire::HandshakeVerdict::Accepted);
    CHECK(dialler.Handshake().Conclude(Unwrap(verdict)).outcome == VerdictOutcome::Accepted);
}

TEST_CASE("Several messages on one connection all arrive", "[consensus][raft][peerserver]")
{
    // A peer connection is long-lived and carries a stream, so reading exactly
    // one frame per connection would work in every single-message test and
    // deliver one heartbeat per reconnect in a real cluster.
    Dialler dialler;
    for (auto const term: std::views::iota(std::uint64_t { 1 }, std::uint64_t { 6 }))
        dialler.Send(VoteFrame(term));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    REQUIRE(sink.received.size() == 5);
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 5 }))
        CHECK(std::get<RequestVoteResponse>(sink.received[index]).term == Term { .value = index + 1 });
    CHECK(served.server->DeliveredMessages() == 5);
}

TEST_CASE("An unknown message type is stepped over, not fatal", "[consensus][raft][peerserver]")
{
    // The mixed-fleet property. The frame between the two known ones carries a
    // type this build has never heard of; the one after it must still arrive. It is
    // sealed like any other, so it is stepped over only once its tag has verified.
    auto unknown = VoteFrame(2);
    unknown[2] = std::byte { 0x7F };

    Dialler dialler;
    dialler.Send(VoteFrame(1));
    dialler.Send(unknown);
    dialler.Send(VoteFrame(3));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    REQUIRE(sink.received.size() == 2);
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 1 });
    CHECK(std::get<RequestVoteResponse>(sink.received[1]).term == Term { .value = 3 });
    CHECK(served.server->SkippedFrames() == 1);
}

TEST_CASE("A frame at a version other than the handshake's ends the connection", "[consensus][raft][peerserver]")
{
    // This was stepped over before the handshake existed, and the case said so. The
    // version is settled by the handshake now: a frame claiming another one cannot be
    // stepped over without guessing whether a tag follows it, so it ends the connection
    // -- even sealed correctly, which only the other end of the session could do.
    auto const newer = RaftWire::Encode(RaftMessage { RequestVoteResponse { .term = Term { .value = 9 },
                                                                            .decision = VoteDecision::Denied,
                                                                            .voterId = NodeId { DiallerId } } },
                                        RaftWire::CurrentVersion + 1);

    Dialler dialler;
    dialler.Send(newer);
    dialler.Send(VoteFrame(4));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.server->SkippedFrames() == 0);
}

TEST_CASE("A wrong magic ends the connection", "[consensus][raft][peerserver]")
{
    // The one condition under which the reader cannot find where the frame ends,
    // so there is nothing to resynchronize to. The frame after it must NOT be
    // delivered -- reading on would be guessing at where it starts.
    auto bad = VoteFrame(1);
    bad[0] = std::byte { 0xFC };

    Dialler dialler;
    dialler.Send(bad);
    dialler.Send(VoteFrame(2));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.server->SkippedFrames() == 0);
}

TEST_CASE("A malformed payload ends the connection", "[consensus][raft][peerserver]")
{
    // Distinct from an unknown type on purpose: the header decoded and the
    // payload was consumed, so the reader is still in sync -- but the sender and
    // this reader disagree about what the bytes mean, which no later frame
    // repairs.
    auto bad = VoteFrame(1);
    // Corrupt the vote decision to a value naming no enumerator. The payload's
    // first field is the term (4-byte prefix + 8 bytes), so the decision's own
    // prefix follows it and its single byte comes after that.
    bad[RaftWire::HeaderSize + 4 + 8 + 4] = std::byte { 0x7F };

    Dialler dialler;
    dialler.Send(bad);
    dialler.Send(VoteFrame(2));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.server->SkippedFrames() == 0);
}

TEST_CASE("An over-large declared frame is refused before it is buffered", "[consensus][raft][peerserver]")
{
    // The declared length is a u32, so a peer can claim four gigabytes. Without
    // a cap the declaration IS the allocation, which makes one frame a
    // memory-exhaustion vector.
    auto oversized = VoteFrame(1);
    // Rewrite the declared payload length to something past the cap, leaving the
    // rest of the frame alone.
    oversized[3] = std::byte { 0x00 };
    oversized[4] = std::byte { 0x40 };
    oversized[5] = std::byte { 0x00 };
    oversized[6] = std::byte { 0x00 };

    Dialler dialler;
    dialler.SendRaw(oversized);

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink, PeerServerOptions { .maxFrameBytes = 1024, .handshakeBound = 0ms });

    CHECK(sink.received.empty());
    CHECK(served.server->DeliveredMessages() == 0);
}

TEST_CASE("A truncated frame ends the connection without delivering", "[consensus][raft][peerserver]")
{
    // What a peer that died mid-write produces. Ordinary, and it must not be
    // half-decoded into a message.
    Dialler dialler;
    dialler.Send(VoteFrame(1));
    dialler.Wire().resize(dialler.Wire().size() - 3);

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.server->DeliveredMessages() == 0);
}

TEST_CASE("A connection that sends a Raft message first is refused, and nothing is delivered",
          "[consensus][raft][peerserver][handshake]")
{
    // What a build from before the handshake sends: a message, with no proof in front of
    // it. The counter is the refusal's own, and nothing reaches the node.
    auto wire = std::vector<std::byte> {};
    Append(wire, VoteFrame(1));
    Append(wire, VoteFrame(2));

    RecordingSink sink;
    auto const served = RunOnce(wire, sink);

    CHECK(sink.received.empty());
    CHECK(served.Refused(AcceptorRefusal::NoHandshake) == 1);
    CHECK(served.AnyRefusals() == 1);
}

TEST_CASE("An acceptor that cannot draw a nonce closes the connection unchallenged and blames itself",
          "[consensus][raft][peerserver][handshake]")
{
    // A connection this node cannot challenge with a FRESH nonce is one it must not challenge
    // at all (#1527): nothing is written, nothing is read, nothing is delivered. And it is not
    // an `AcceptorRefusal` -- every one of those names something about the peer -- so no peer
    // counter moves, and the Error names this host's generator instead.
    Dialler dialler;
    dialler.Send(VoteFrame(1));

    RecordingSink sink;
    Testing::ScriptedSecureRandom denied { Testing::ScriptedSecureRandom::DeniedFailure() };
    CapturingLogger logger;
    auto const served = RunOnceWith(dialler.Wire(), sink, denied, logger);

    CHECK(denied.FillCount() == 1);
    CHECK(served.replied.empty());
    CHECK(sink.received.empty());
    CHECK(served.server->DeliveredMessages() == 0);
    CHECK(served.AnyRefusals() == 0);

    auto const lines = logger.Snapshot();
    CHECK(std::ranges::any_of(lines, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Error && record.message.contains("cannot draw a handshake nonce")
               && record.message.contains(Testing::ScriptedSecureRandom::DeniedFailure().primitive);
    }));
}

TEST_CASE("A proof at a version before this grammar is refused", "[consensus][raft][peerserver][handshake]")
{
    // Version 1 authenticated nothing and version 3 proved the cluster's pre-shared key. A
    // server that read either would be the per-connection fallback #1308 and #178 refuse.
    for (auto const version: { std::uint8_t { 1 }, std::uint8_t { 3 } })
    {
        CAPTURE(version);
        Dialler dialler;

        RecordingSink sink;
        auto const served = RunOnce(RaftWire::EncodeProof(dialler.Proof(), version), sink);

        CHECK(served.Refused(AcceptorRefusal::NoHandshake) == 1);
        CHECK_FALSE(served.Verdict().has_value());
    }
}

TEST_CASE("A dialler signing with a key that is not its id's is refused, counted, and told nothing",
          "[consensus][raft][peerserver][handshake]")
{
    SECTION("a machine the cluster never admitted, borrowing n2's id")
    {
        Dialler dialler { { .id = std::string { DiallerId }, .target = std::string { ServerId }, .machine = "stranger" } };
        dialler.Send(VoteFrame(1));

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);

        CHECK(sink.received.empty());
        CHECK(served.Refused(AcceptorRefusal::Proof) == 1);
        CHECK(served.AnyRefusals() == 1);

        // A verdict exists only for a proof whose signature verified; this one gets the
        // challenge and a close.
        CHECK_FALSE(served.Verdict().has_value());
    }

    SECTION("n3, holding every byte it ever held, claiming n2's id")
    {
        // A member, with its own key and the whole roster. Under the pre-shared key this
        // proved n2; under its own key it proves nothing it claims.
        Dialler dialler { { .id = std::string { DiallerId }, .target = std::string { ServerId }, .machine = "n3" } };
        dialler.Send(VoteFrame(1));

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);

        CHECK(sink.received.empty());
        CHECK(served.Refused(AcceptorRefusal::Proof) == 1);
        CHECK(served.AnyRefusals() == 1);
        CHECK_FALSE(served.Verdict().has_value());
    }
}

TEST_CASE("A dialler the roster holds no key for is refused under its own counter, and told nothing",
          "[consensus][raft][peerserver][handshake]")
{
    // Apart from a forged proof, because the remedy is apart: a key that was never given, not
    // somebody impersonating a member.
    Dialler dialler { { .id = "n9", .target = std::string { ServerId }, .machine = "n9" } };
    dialler.Send(VoteFrame(1, "n9"));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.Refused(AcceptorRefusal::UnknownKey) == 1);
    CHECK(served.AnyRefusals() == 1);
    CHECK_FALSE(served.Verdict().has_value());
}

TEST_CASE("A dialler whose key the cluster revoked is refused with a signed verdict saying so",
          "[consensus][raft][peerserver][handshake]")
{
    auto const roster = Roster();
    roster->Revoke(std::string { DiallerId });
    Dialler dialler { { .roster = roster } };
    dialler.Send(VoteFrame(1));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink, { .handshakeBound = 0ms }, roster);

    CHECK(sink.received.empty());
    CHECK(served.Refused(AcceptorRefusal::RevokedKey) == 1);
    CHECK(served.AnyRefusals() == 1);

    auto const verdict = served.Verdict();
    REQUIRE(verdict.has_value());
    CHECK(Unwrap(verdict).verdict == RaftWire::HandshakeVerdict::KeyRevoked);
    CHECK(dialler.Handshake().Conclude(Unwrap(verdict)).outcome == VerdictOutcome::OwnKeyRevoked);
}

TEST_CASE("A dialler with no key at all is refused and counted", "[consensus][raft][peerserver][handshake]")
{
    // The signature is whatever a peer holding nothing can put there.
    auto const proof = RaftWire::ProofFrame { .dialler = NodeId { DiallerId },
                                              .target = NodeId { ServerId },
                                              .nonce = ExpectedChallenge().nonce,
                                              .ephemeral = {},
                                              .signature = {} };

    RecordingSink sink;
    auto const served = RunOnce(RaftWire::EncodeProof(proof), sink);

    CHECK(served.Refused(AcceptorRefusal::Proof) == 1);
    CHECK_FALSE(served.Verdict().has_value());
}

TEST_CASE("A proof recorded against another challenge is refused", "[consensus][raft][peerserver][handshake]")
{
    // A genuine proof, harvested from a connection whose challenge was different. The
    // server's challenge on THIS connection is what it answers, and it does not.
    Testing::TestPeerIdentity const identity { NodeId { DiallerId },
                                               Testing::TestKeyPair(std::string { DiallerId }),
                                               Roster() };
    SystemSecureRandom random;
    auto handshake = DiallerHandshake::Create(identity, NodeId { ServerId }, random).value();
    auto elsewhere = ExpectedChallenge();
    elsewhere.nonce[0] ^= std::byte { 0x01 };
    auto const recorded = handshake.Answer(elsewhere);
    REQUIRE(recorded.has_value());

    RecordingSink sink;
    auto const served = RunOnce(RaftWire::EncodeProof(*recorded), sink);

    CHECK(served.Refused(AcceptorRefusal::Proof) == 1);
    CHECK_FALSE(served.Verdict().has_value());
}

TEST_CASE("A proven dialler that dialled another member is refused with a signed verdict",
          "[consensus][raft][peerserver][handshake]")
{
    Dialler dialler { { .id = std::string { DiallerId }, .target = "n3", .machine = std::string { DiallerId } } };
    dialler.Send(VoteFrame(1));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(sink.received.empty());
    CHECK(served.Refused(AcceptorRefusal::WrongTarget) == 1);
    CHECK(served.AnyRefusals() == 1);

    auto const verdict = served.Verdict();
    REQUIRE(verdict.has_value());
    CHECK(Unwrap(verdict).verdict == RaftWire::HandshakeVerdict::WrongTarget);
    CHECK(dialler.Handshake().Conclude(Unwrap(verdict)).outcome == VerdictOutcome::WrongTarget);
}

TEST_CASE("A proven dialler under this node's own id is refused with a signed verdict",
          "[consensus][raft][peerserver][handshake]")
{
    // A copied state directory: a second machine holding n1's own private key.
    Dialler dialler {
        { .id = std::string { ServerId }, .target = std::string { ServerId }, .machine = std::string { ServerId } }
    };

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    CHECK(served.Refused(AcceptorRefusal::OwnId) == 1);
    auto const verdict = served.Verdict();
    REQUIRE(verdict.has_value());
    CHECK(dialler.Handshake().Conclude(Unwrap(verdict)).outcome == VerdictOutcome::OwnId);
}

TEST_CASE("A proof declaring more than its ceiling is refused before it is read", "[consensus][raft][peerserver][handshake]")
{
    // Only the header is sent. A server that believed the declaration would sit waiting
    // for a payload that is not coming, see the connection end, and count nothing -- so
    // the counter moving is what shows the refusal came first.
    auto header = std::vector<std::byte>(RaftWire::HeaderSize);
    WireFrame::PutHeader(header,
                         RaftWire::Magic,
                         RaftWire::CurrentVersion,
                         static_cast<std::uint8_t>(RaftWire::MessageType::Proof),
                         static_cast<std::uint32_t>(RaftWire::MaxHandshakePayload + 1));

    RecordingSink sink;
    auto const served = RunOnce(header, sink);

    CHECK(served.Refused(AcceptorRefusal::NoHandshake) == 1);
}

TEST_CASE("A peer that sends nothing is challenged, closed, and not counted", "[consensus][raft][peerserver][handshake]")
{
    // A peer that sent NOTHING asked nothing. It is still sent the challenge -- first, and
    // before anything is read -- and a close with no counter is the ordinary answer to a
    // connection that ends.
    RecordingSink sink;
    auto const served = RunOnce({}, sink);

    CHECK(served.AnyRefusals() == 0);
    auto const header = RaftWire::DecodeHeader(served.replied);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).kindRaw == static_cast<std::uint8_t>(RaftWire::MessageType::Challenge));
}

TEST_CASE("A frame whose tag does not verify ends the connection and is counted", "[consensus][raft][peerserver][handshake]")
{
    SECTION("its bytes changed on the way")
    {
        Dialler dialler;
        dialler.Send(VoteFrame(1));
        dialler.Wire()[dialler.Wire().size() - RaftWire::TagSize - 1] ^= std::byte { 0x01 };
        dialler.Send(VoteFrame(2));

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);
        CHECK(sink.received.empty());
        CHECK(served.Refused(AcceptorRefusal::FrameTag) == 1);
    }

    SECTION("the same frame, sent twice")
    {
        Dialler dialler;
        auto const once = dialler.Send(VoteFrame(1));
        dialler.SendRaw(once);

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);
        CHECK(sink.received.size() == 1);
        CHECK(served.Refused(AcceptorRefusal::FrameTag) == 1);
    }

    SECTION("a frame from another connection's session between the same two members")
    {
        // Another connection's session: same ids, same keys -- and a different handshake, so
        // a different agreed key.
        Dialler dialler;
        Dialler other;
        auto const frame = VoteFrame(1);
        auto const sealedElsewhere = other.Send(frame);
        dialler.SendRaw(sealedElsewhere);

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);
        CHECK(sink.received.empty());
        CHECK(served.Refused(AcceptorRefusal::FrameTag) == 1);
    }

    SECTION("a frame sealed by something without the session")
    {
        // Whatever holds every key the cluster has, public and private, but was not an end of
        // THIS exchange: it holds neither ephemeral secret, so it cannot hold the key.
        Dialler dialler;
        FrameSealer forging { StrangerSession() };
        auto const frame = VoteFrame(1);
        dialler.SendRaw(frame);
        dialler.SendRaw(SealWhole(forging, frame));

        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink);
        CHECK(sink.received.empty());
        CHECK(served.Refused(AcceptorRefusal::FrameTag) == 1);
    }
}

TEST_CASE("A key revoked while its connection is open ends the connection at the next frame, counted",
          "[consensus][raft][peerserver][handshake][revocation]")
{
    // #178: an applied forget closes the sessions the forgotten member's key proved. The roster moves between
    // the first frame and the second -- the sink revokes on delivery -- and the second, sealed
    // correctly in a session that was valid when it began, is refused rather than delivered.
    auto const roster = Roster();
    Dialler dialler { { .roster = roster } };
    dialler.Send(VoteFrame(1));
    dialler.Send(VoteFrame(2));

    SECTION("revoked after the first frame")
    {
        RecordingSink sink;
        sink.onDelivery = [&roster, &sink] {
            if (sink.received.size() == 1)
                roster->Revoke(std::string { DiallerId });
        };
        auto const served = RunOnce(dialler.Wire(), sink, { .handshakeBound = 0ms }, roster);

        REQUIRE(sink.received.size() == 1);
        CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 1 });
        CHECK(served.Refused(AcceptorRefusal::KeyWithdrawn) == 1);
        CHECK(served.AnyRefusals() == 1);
    }

    SECTION("the control: the same connection with nothing revoked delivers both")
    {
        RecordingSink sink;
        auto const served = RunOnce(dialler.Wire(), sink, { .handshakeBound = 0ms }, roster);
        CHECK(sink.received.size() == 2);
        CHECK(served.AnyRefusals() == 0);
    }
}

TEST_CASE("A verified message naming another sender ends the connection and is counted",
          "[consensus][raft][peerserver][handshake]")
{
    // The member a connection proved is the one that speaks on it. A vote naming n3,
    // on a connection n2 proved, is refused rather than delivered under n3's name.
    Dialler dialler;
    dialler.Send(VoteFrame(1));
    dialler.Send(VoteFrame(2, "n3"));
    dialler.Send(VoteFrame(3));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink);

    REQUIRE(sink.received.size() == 1);
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).voterId == DiallerId);
    CHECK(served.Refused(AcceptorRefusal::FrameSender) == 1);
}

TEST_CASE("A connection over the listener's cap is closed and counted", "[consensus][raft][peerserver][handshake]")
{
    Dialler dialler;
    dialler.Send(VoteFrame(1));

    RecordingSink sink;
    auto const served = RunOnce(dialler.Wire(), sink, PeerServerOptions { .maxConnections = 0, .handshakeBound = 0ms });

    CHECK(sink.received.empty());
    CHECK(served.Refused(AcceptorRefusal::Full) == 1);
    CHECK(served.replied.empty());
}

TEST_CASE("A connection that does not prove an id within the bound is closed and counted",
          "[consensus][raft][peerserver][handshake]")
{
    // Before the handshake existed, a connection that sent nothing held a slot for as long
    // as its socket lived. Driven over a `core::net::testing::TestLoop`, so the bound fires when the clock
    // says and not when a host is slow enough.
    constexpr auto Bound = 500ms;

    core::net::testing::InMemoryListener listener;
    NullLogger logger;
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    RecordingSink sink;
    AtomicMetricsSink metrics;
    Testing::TestPeerIdentity const identity { NodeId { ServerId },
                                               Testing::TestKeyPair(std::string { ServerId }),
                                               Roster() };
    Testing::ScriptedSecureRandom random { ServerScript() };
    RaftPeerServer server { listener, reactor,  sink,   logger,
                            metrics,  identity, random, PeerServerOptions { .handshakeBound = Bound } };

    auto accepting = [](RaftPeerServer* s) -> core::async::DetachedTask {
        co_await s->Run();
    };
    accepting(&server);

    auto const client = listener.connectClient();
    reactor.drain();

    auto const timedOut = [&metrics] {
        return metrics.Read(RowFor(AcceptorRefusal::HandshakeTimeout).counter);
    };

    clock.advance(Bound / 2);
    reactor.drain();
    CHECK(timedOut() == 0);

    clock.advance(Bound);
    reactor.drain();
    CHECK(timedOut() == 1);

    // The control the bound needs beside it: nothing else was counted, so the close was
    // the bound's and not a refusal of something the peer sent.
    CHECK(metrics.Read(RowFor(AcceptorRefusal::NoHandshake).counter) == 0);
    CHECK(sink.received.empty());

    listener.close();
    reactor.drain();
    CHECK(reactor.pendingTimers() == 0);
}

namespace
{

/// Wait until @p ready holds, or give up. Bounded, and it says what it waited for.
///
/// Through `DrainWithin` rather than a spin of its own, for the reason that helper
/// exists: counting the sleeps it ASKED for states a ceiling and enforces a
/// multiple of it, because a sleep costs what the host's timer granularity says.
/// @param ready What is being waited for; called until it answers true.
/// @return True when it happened inside the ceiling, false when it never did.
template <typename Predicate>
[[nodiscard]] bool WaitFor(Predicate ready)
{
    return DrainWithin([&ready] { return !ready(); }, DrainBound { .ceiling = std::chrono::seconds { 15 } })
           == DrainResult::Drained;
}

/// A reactor turning on a thread of its own, stopped and joined by its destructor.
///
/// RAII rather than a `Stop()` at the end of the case, and that is not tidiness: a
/// failing `REQUIRE` unwinds past such a call, `~jthread` then joins a loop nobody
/// stopped, and the case HANGS instead of reporting. A case about teardown ordering
/// is the last one that may turn a red into a timeout -- a hang says nothing, where
/// a red says which half of the rule was missing.
class RunningReactor
{
  public:
    RunningReactor():
        _thread { [this] { _reactor.run(); } }
    {
    }

    ~RunningReactor()
    {
        // Asked to stop BEFORE the member sweep, because `_thread` is declared last
        // and so is joined first -- a join without this would wait on a loop with no
        // reason to return.
        _reactor.stop();
    }

    RunningReactor(RunningReactor const&) = delete;
    RunningReactor(RunningReactor&&) = delete;
    RunningReactor& operator=(RunningReactor const&) = delete;
    RunningReactor& operator=(RunningReactor&&) = delete;

    /// @return The reactor, for handing to whatever is under test.
    [[nodiscard]] core::net::EventLoop& Get() noexcept
    {
        return _reactor;
    }

    /// @return The worker thread's id, which is what a teardown case compares against.
    [[nodiscard]] std::thread::id WorkerId() const noexcept
    {
        return _thread.get_id();
    }

  private:
    core::platform::SteadyClock _clock;
    core::net::PlatformLoop _reactor { _clock };
    std::jthread _thread;
};

/// A listener that records the thread its `Close()` ran on, and nothing else.
///
/// The whole of #885 is *which thread* closes, so the fake records a thread id
/// rather than a boolean: "it was closed" is true under the defect and under the
/// fix alike, and a case asserting that could never fail for the reason it exists.
class ClosingThreadListener final: public core::net::IListener
{
  public:
    /// Never yields a connection; this case drives `Shutdown()`, not `Run()`.
    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        co_return core::net::AcceptResult {
            std::unexpect, core::net::NetError { .code = core::net::NetErrorCode::Cancelled, .systemCode = 0, .context = {} }
        };
    }

    void close() noexcept override
    {
        _closedOn.store(std::this_thread::get_id(), std::memory_order_release);
        _closes.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override
    {
        return 0;
    }

    /// @return How many times `close()` has been called.
    [[nodiscard]] std::size_t Closes() const noexcept
    {
        return _closes.load(std::memory_order_acquire);
    }

    /// @return The thread of the most recent `Close()`; default-constructed if none.
    [[nodiscard]] std::thread::id ClosedOn() const noexcept
    {
        return _closedOn.load(std::memory_order_acquire);
    }

  private:
    std::atomic<std::thread::id> _closedOn {};
    std::atomic<std::size_t> _closes { 0 };
};

} // namespace

TEST_CASE("Shutdown closes the peer listener on the reactor, not on the calling thread",
          "[consensus][raft][peerserver][teardown]")
{
    // #885. `Shutdown()` used to close the listener and every accepted connection
    // from whatever thread was tearing the node down. On fastcached's own reactor,
    // `Close` on epoll and kqueue completed a parked read by resuming its coroutine
    // INLINE, so that ran each per-connection task on the stopping thread -- while the
    // reactor thread was still driving the others -- and destroyed the socket owned by
    // that task's frame off the reactor, which is #668's rule. IOCP did not resume
    // inline, so the platform that gated least was the one that could never show it.
    // core-cpp resumes a woken task in the loop's next drain on every backend (0.2.1,
    // guarantee G2), and a close still belongs on the loop's thread, which is what
    // this case pins.
    //
    // Asserted as a THREAD IDENTITY, because that is what distinguishes: the
    // listener is closed either way, and a case checking only that it was closed
    // passes under the defect forever.
    // Declared so that the listener OUTLIVES the reactor thread: destruction runs in
    // reverse, so `server` goes first, then `loop` (which stops and joins), and only
    // then the listener the reactor was closing. The other order leaves a window in
    // which the worker thread could touch a destroyed fake.
    ClosingThreadListener listener;
    RecordingSink sink;
    NullLogger logger;
    AtomicMetricsSink metrics;
    Testing::TestPeerIdentity const identity { NodeId { ServerId },
                                               Testing::TestKeyPair(std::string { ServerId }),
                                               Roster() };
    SystemSecureRandom random;
    RunningReactor loop;

    // The arrangement, asserted rather than assumed: if the reactor were not running,
    // the closes would be posted where nothing runs them and the identity below would
    // be comparing two things that mean nothing.
    REQUIRE(WaitFor([&loop] { return loop.Get().running(); }));
    REQUIRE_FALSE(loop.Get().isOnWorkerThread());

    RaftPeerServer server { listener, loop.Get(), sink, logger, metrics, identity, random };

    server.Shutdown();

    // Returned only once the posted closes had run -- which is also what makes the
    // task's borrowed `this` safe, so it is checked rather than assumed.
    REQUIRE(listener.Closes() == 1);

    INFO("the listener must have been closed on the reactor's worker thread, not on the thread that called Shutdown");
    CHECK(listener.ClosedOn() == loop.WorkerId());
    CHECK_FALSE(listener.ClosedOn() == std::this_thread::get_id());
}
