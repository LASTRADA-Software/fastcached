// SPDX-License-Identifier: Apache-2.0
//
// `ReadProvenSession` on its own (#1596's zero-config fleet work, Task 5): the read loop Task 5
// pulled out of `RaftPeerServer::Serve` so both ends of a proven Raft peer session enforce one
// set of frame rules instead of two copies that can drift. Driven directly over a hand-sealed
// in-memory stream, with no `RaftPeerServer` and no `RaftPeerTransport` in the way, so a case
// here is about the reader's OWN checks -- the tag, the roster, the sender -- and not about the
// handshake that produces its inputs (`RaftPeerSession_test.cpp`) or the whole connection
// (`RaftPeerServer_test.cpp`, `RaftPeerLink_test.cpp`), which still exercise this same reader
// through `Serve` and must keep passing unchanged.
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftSessionReader.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/ISecureRandom.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/SyncRun.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <tests/HalfClose.hpp>
#include <tests/ListenerConnector.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;

namespace
{

/// Records every message the reader delivers, in arrival order.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<RaftMessage> received;
};

/// Records what `ReadProvenSession` reported: a count of each, and every skip's own context.
class RecordingObserver final: public IProvenSessionObserver
{
  public:
    void OnDelivered() override
    {
        delivered.fetch_add(1, std::memory_order_relaxed);
    }

    void OnSkipped(ConsensusError const& error) override
    {
        skipped.fetch_add(1, std::memory_order_relaxed);
        skippedContexts.push_back(error.context);
    }

    std::atomic<std::uint64_t> delivered { 0 };
    std::atomic<std::uint64_t> skipped { 0 };
    std::vector<std::string> skippedContexts;
};

/// Write every byte of `bytes` to `socket`.
/// @param socket Destination; never null.
/// @param bytes What to send.
/// @return Whether all of it was accepted.
[[nodiscard]] core::async::Task<bool> WriteAll(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    auto const written = co_await socket->write(bytes);
    co_return written.has_value() && *written == bytes.size();
}

/// Append `bytes` to `out`.
/// @param out Destination.
/// @param bytes What to append.
void Append(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// @param frame A frame as `RaftWire::Encode` produced it.
/// @return Its header bytes.
[[nodiscard]] std::span<std::byte const> HeaderOf(std::span<std::byte const> frame) noexcept
{
    return frame.first(RaftWire::HeaderSize);
}

/// @param frame A frame as `RaftWire::Encode` produced it.
/// @return Its payload bytes.
[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> frame) noexcept
{
    return frame.subspan(RaftWire::HeaderSize);
}

/// One proven Raft peer session, agreed by running the real handshake off-socket, and the pair
/// of in-memory sockets `ReadProvenSession` and this fixture's writer use.
///
/// `reader` is who runs `ReadProvenSession` -- the acceptor's identity, checked against the
/// roster on every frame. `sender` is who the session is proved AS: this fixture's `FrameSealer`
/// signs as `sender`, so a case can send a message naming a different sender in its PAYLOAD
/// (`WrongSender`) apart from the session's own proof, which never changes once agreed.
struct SessionFixture
{
    /// @param readerId Who runs `ReadProvenSession` -- the acceptor.
    /// @param senderId Who the session is proved as -- the dialler this fixture writes for.
    SessionFixture(std::string const& readerId, std::string const& senderId):
        SessionFixture { readerId, senderId, Prepare(readerId, senderId) }
    {
    }

    SessionFixture(SessionFixture const&) = delete;
    SessionFixture(SessionFixture&&) = delete;
    SessionFixture& operator=(SessionFixture const&) = delete;
    SessionFixture& operator=(SessionFixture&&) = delete;

    /// Ends the session, if it has not already, before any member below is destroyed.
    ///
    /// A `REQUIRE` inside `SendSealed`/`SendTampered` that fails unwinds straight past
    /// `ReadUntilEnd`, so without this the detached reader can be left parked, holding pointers
    /// into `byteReader`, `opener` and `sink` that member destruction is about to free out from
    /// under it (testing.md: a `REQUIRE` above an explicit stop turns a red into a hang; the fix
    /// is RAII, and a destructor's own body -- unlike a member's -- runs before every member
    /// here regardless of their declaration order).
    ///
    /// Not wrapped in a `try`/`catch`: this half-close is not expected to throw, and swallowing
    /// it if it somehow did would hide exactly the kind of stuck reader this destructor exists
    /// to prevent. `~SessionFixture` stays implicitly `noexcept`, so a genuine throw here ends
    /// the process rather than passing silently, which is the louder and more honest failure.
    ~SessionFixture()
    {
        if (writer)
            (void) Testing::ShutdownWrite(*writer);
    }

    /// Seal `message` under the session and write it.
    /// @param message What to send.
    void SendSealed(RaftMessage const& message)
    {
        auto sealed = RaftWire::Encode(message);
        auto const tag = sealer.Seal(HeaderOf(sealed), PayloadOf(sealed));
        Append(sealed, tag);
        REQUIRE(core::async::syncRun(WriteAll(writer.get(), sealed)));
    }

    /// As `SendSealed`, but flips one payload byte after sealing, so the tag that follows it no
    /// longer verifies.
    /// @param message What to send, tampered with.
    void SendTampered(RaftMessage const& message)
    {
        auto sealed = RaftWire::Encode(message);
        auto const tag = sealer.Seal(HeaderOf(sealed), PayloadOf(sealed));
        sealed[RaftWire::HeaderSize] ^= std::byte { 0x01 };
        Append(sealed, tag);
        REQUIRE(core::async::syncRun(WriteAll(writer.get(), sealed)));
    }

    /// Close the writing end, so a reader still parked waiting for a frame past the last one this
    /// case sent sees a clean end-of-file, and return why the session ended.
    ///
    /// Every case's own frames have already run the reader forward as far as they can by the
    /// time this is called (see the constructor); this only settles a reader that is still
    /// parked waiting for one more header, which the close wakes inline exactly as a write does.
    /// @return Why the session ended.
    [[nodiscard]] SessionEnding ReadUntilEnd() const
    {
        REQUIRE(Testing::ShutdownWrite(*writer).has_value());
        REQUIRE(ending.has_value());
        return Testing::Unwrap(ending);
    }

    static constexpr std::size_t MaxFrameBytes = 8U * 1024U * 1024U;

    std::string reader; ///< Who runs `ReadProvenSession`.
    std::string sender; ///< Who the session was proved as.
    std::shared_ptr<Testing::SharedRoster> roster;
    std::unique_ptr<Testing::TestPeerIdentity const> readerIdentity;
    Ed25519PublicKey provenKey {};
    FrameSealer sealer;
    FrameOpener opener;
    std::unique_ptr<core::net::ISocket> writer;
    std::unique_ptr<core::net::ISocket> readerSocket;
    ByteReader byteReader;
    RecordingSink sink;
    RecordingObserver observer;
    std::optional<SessionEnding> ending; ///< Set once the spawned reader reaches its end.

    /// What `Prepare` hands the delegating constructor: everything a real handshake and a real
    /// connected pair produce, before any of it is a member this fixture has to re-prove is
    /// engaged -- `sealer`, `opener` and `byteReader` are ordinary members below, never a
    /// `std::optional` a later member function has no way to see was already filled in.
    struct Built
    {
        std::shared_ptr<Testing::SharedRoster> roster;
        std::unique_ptr<Testing::TestPeerIdentity const> readerIdentity;
        Ed25519PublicKey provenKey {};
        FrameSealer sealer;
        FrameOpener opener;
        std::unique_ptr<core::net::ISocket> writer;
        std::unique_ptr<core::net::ISocket> readerSocket;
    };

    /// Run the real handshake off-socket, exactly as `RaftClusterHarness::Authenticate` does, and
    /// set up the connected in-memory pair `RaftPeerLink_test`'s `Link` gets from a listener and a
    /// connector over it -- local here, since neither needs to outlive the sockets it hands back.
    /// @param readerId Who runs `ReadProvenSession`.
    /// @param senderId Who the session is proved as.
    /// @return Everything the delegating constructor needs.
    [[nodiscard]] static Built Prepare(std::string const& readerId, std::string const& senderId)
    {
        auto roster = Testing::SharedRoster::Of({ readerId, senderId });
        auto readerIdentity = Testing::TestPeerIdentity::Honest(NodeId { readerId }, roster);
        auto senderIdentity = Testing::TestPeerIdentity::Honest(NodeId { senderId }, roster);

        SystemSecureRandom random;
        auto acceptorBegun = AcceptorHandshake::Create(*readerIdentity, random);
        REQUIRE(acceptorBegun.has_value());
        auto& acceptor = *acceptorBegun;

        auto diallerBegun =
            DiallerHandshake::Create(*senderIdentity, NodeId { readerId }, RaftWire::SessionDirection::OneWay, random);
        REQUIRE(diallerBegun.has_value());
        auto& dialler = *diallerBegun;

        auto const proof = dialler.Answer(acceptor.Challenge());
        REQUIRE(proof.has_value());

        auto judgement = acceptor.Judge(*proof);
        REQUIRE(judgement.outcome == ProofOutcome::Accepted);
        if (!judgement.verdict.has_value() || !judgement.session.has_value())
            throw std::logic_error { "this fixture's own honest proof was not accepted with a session" };

        auto conclusion = dialler.Conclude(*judgement.verdict);
        REQUIRE(conclusion.outcome == VerdictOutcome::Accepted);
        if (!conclusion.session.has_value())
            throw std::logic_error { "this fixture's own accepted verdict carried no session" };

        // The pair `RaftPeerLink_test`'s `Link` gets from a listener and a connector over it,
        // local here since neither needs to outlive the sockets it hands back.
        core::net::testing::InMemoryListener listener;
        Testing::ListenerConnector connector { listener };
        auto client = core::async::syncRun(connector.connect("in-memory", 1, {}));
        REQUIRE(client.has_value());
        auto accepted = core::async::syncRun(listener.accept());
        REQUIRE(accepted.has_value());

        // Each end seals and opens under the key IT agreed for the dialler's direction, exactly
        // as `RaftClusterHarness::Authenticate` does: the two are the same DH output, but two
        // independent `SessionKey` objects, each counting its own position from zero.
        return Built {
            .roster = std::move(roster),
            .readerIdentity = std::move(readerIdentity),
            .provenKey = judgement.provenKey,
            .sealer = FrameSealer { std::move(conclusion.session->diallerToAcceptor) },
            .opener = FrameOpener { std::move(judgement.session->diallerToAcceptor) },
            .writer = std::move(*client),
            .readerSocket = std::move(*accepted),
        };
    }

    /// @param readerId Who runs `ReadProvenSession`.
    /// @param senderId Who the session is proved as.
    /// @param built Everything `Prepare` produced.
    SessionFixture(std::string readerId, std::string senderId, Built built):
        reader { std::move(readerId) },
        sender { std::move(senderId) },
        roster { std::move(built.roster) },
        readerIdentity { std::move(built.readerIdentity) },
        provenKey { built.provenKey },
        sealer { std::move(built.sealer) },
        opener { std::move(built.opener) },
        writer { std::move(built.writer) },
        readerSocket { std::move(built.readerSocket) },
        byteReader { *readerSocket, 1, MaxFrameBytes + RaftWire::TagSize }
    {
        // Started here, before any frame is sent, so it parks on its first header read.
        // `InMemorySocket` completes inline, on the calling thread: a `SendSealed` call below
        // wakes this parked read and runs it forward, synchronously, to its next park or its
        // end -- which is what makes a roster change BETWEEN two `SendSealed` calls a change
        // this session's first frame never sees and its second one does.
        [](SessionFixture* self) -> core::async::DetachedTask {
            auto const who = ProvenSessionPeer { .id = NodeId { self->sender }, .key = self->provenKey };
            self->ending = co_await ReadProvenSession(&self->byteReader,
                                                      &self->opener,
                                                      &who,
                                                      self->readerIdentity.get(),
                                                      &self->sink,
                                                      SessionReadLimits { .maxFrameBytes = MaxFrameBytes },
                                                      &self->observer);
        }(this);
    }
};

/// @param term A term, so two responses on one session are distinguishable.
/// @param followerId Who the response names as having answered.
/// @return An `AppendEntriesResponse` wrapped as a `RaftMessage`.
[[nodiscard]] RaftMessage Response(std::uint64_t term, std::string_view followerId)
{
    return RaftMessage { AppendEntriesResponse { .term = Term { .value = term }, .followerId = NodeId { followerId } } };
}

} // namespace

TEST_CASE("A proven session delivers sealed frames and stops at the first frame from another sender",
          "[consensus][raft][session][formation]")
{
    SessionFixture fix { "office", "laptop" }; // reader side proves "laptop"; sealer shares its key
    fix.SendSealed(Response(1, "laptop"));
    fix.SendSealed(Response(2, "mallory"));
    fix.SendSealed(Response(3, "laptop"));

    auto const ending = fix.ReadUntilEnd();
    CHECK(ending.end == SessionEnd::WrongSender);
    REQUIRE(fix.sink.received.size() == 1);
    CHECK(std::get<AppendEntriesResponse>(fix.sink.received[0]).term.value == 1);
    CHECK(fix.observer.delivered.load() == 1);
}

TEST_CASE("A session frame with a bad tag ends the session before anything is delivered",
          "[consensus][raft][session][formation]")
{
    SessionFixture fix { "office", "laptop" };
    fix.SendTampered(Response(1, "laptop"));

    CHECK(fix.ReadUntilEnd().end == SessionEnd::BadTag);
    CHECK(fix.sink.received.empty());
    CHECK(fix.observer.delivered.load() == 0);
}

TEST_CASE("A key the roster withdrew ends the session at its next frame", "[consensus][raft][session][formation]")
{
    SessionFixture fix { "office", "laptop" };
    fix.SendSealed(Response(1, "laptop"));
    fix.roster->Revoke("laptop");
    fix.SendSealed(Response(2, "laptop"));

    CHECK(fix.ReadUntilEnd().end == SessionEnd::KeyWithdrawn);
    REQUIRE(fix.sink.received.size() == 1);
    CHECK(fix.observer.delivered.load() == 1);
}

TEST_CASE("A tampered frame after a revoke still ends BadTag: the tag is checked before the roster",
          "[consensus][raft][session][formation]")
{
    // #178's ordering, pinned: a frame that fails its OWN tag is refused for that, whatever the
    // roster went on to say about the key that (honestly) sealed an earlier one. Swapping the
    // order in `ReadProvenSession` -- StillProves before the tag -- would make this KeyWithdrawn.
    SessionFixture fix { "office", "laptop" };
    fix.SendSealed(Response(1, "laptop"));
    fix.roster->Revoke("laptop");
    fix.SendTampered(Response(2, "laptop"));

    CHECK(fix.ReadUntilEnd().end == SessionEnd::BadTag);
    REQUIRE(fix.sink.received.size() == 1);
    CHECK(fix.observer.delivered.load() == 1);
}

TEST_CASE("The control: an honest session with nothing revoked and every frame named right delivers everything",
          "[consensus][raft][session][formation]")
{
    // The one case every refusal above needs beside it: the same fixture, and the only
    // difference is that nothing here is wrong.
    SessionFixture fix { "office", "laptop" };
    fix.SendSealed(Response(1, "laptop"));
    fix.SendSealed(Response(2, "laptop"));

    auto const ending = fix.ReadUntilEnd();
    CHECK(ending.end == SessionEnd::PeerClosed);
    REQUIRE(fix.sink.received.size() == 2);
    CHECK(std::get<AppendEntriesResponse>(fix.sink.received[0]).term.value == 1);
    CHECK(std::get<AppendEntriesResponse>(fix.sink.received[1]).term.value == 2);
    CHECK(fix.observer.delivered.load() == 2);
    CHECK(fix.observer.skipped.load() == 0);
}
