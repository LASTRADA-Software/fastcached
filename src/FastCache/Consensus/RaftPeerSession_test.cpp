// SPDX-License-Identifier: Apache-2.0
//
// The Raft peer handshake and the frame MAC, driven directly: no socket, so every
// case states exactly which bytes each end saw. What the server and transport do with
// an outcome -- close, count, log -- is theirs and is tested there.
#include <FastCache/Cluster/PskRaftPeerCredential.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;
using namespace FastCache::Consensus;

namespace
{

/// A cluster key of thirty-two bytes of @p fill.
[[nodiscard]] SecureByteBuffer Key(unsigned char fill)
{
    return SecureByteBuffer(32, std::byte { fill });
}

/// The key every well-behaved member of these cases holds.
constexpr unsigned char ClusterKeyFill = 0x5A;

/// A key nobody in the cluster holds.
constexpr unsigned char StrangerKeyFill = 0x33;

/// One node's side of a connection: its credential and its randomness.
struct Side
{
    Cluster::PskRaftPeerCredential credential;
    SystemRandomSource random;

    /// @param fill The key's byte.
    /// @param seed The randomness seed, distinct per side so the two nonces differ.
    Side(unsigned char fill, std::uint64_t seed):
        credential { Key(fill) },
        random { seed }
    {
    }
};

/// A frame to seal: an AppendEntries heartbeat from @p leader at @p term.
[[nodiscard]] std::vector<std::byte> Heartbeat(std::uint64_t term, NodeId const& leader = "n1")
{
    return RaftWire::Encode(RaftMessage { AppendEntriesRequest { .term = Term { .value = term },
                                                                 .leaderId = leader,
                                                                 .prevLogIndex = LogIndex { .value = 0 },
                                                                 .prevLogTerm = Term { .value = 0 },
                                                                 .entries = {},
                                                                 .leaderCommit = LogIndex { .value = 0 } } });
}

/// The proof @p dialling answers @p challenge with, which these cases require to exist.
[[nodiscard]] RaftWire::ProofFrame ProofFor(DiallerHandshake& dialling, RaftWire::ChallengeFrame const& challenge)
{
    auto proof = dialling.Answer(challenge);
    REQUIRE(proof.has_value());
    return *std::move(proof);
}

/// Open a whole encoded frame, split the way a reader receives it.
/// @param opener The session's opener.
/// @param frame The frame as `RaftWire::Encode` produced it.
/// @param tag The tag that followed it.
/// @return Whether it opened.
[[nodiscard]] bool OpenWhole(FrameOpener& opener, std::vector<std::byte> const& frame, Sha256::Digest const& tag)
{
    auto const whole = std::span<std::byte const> { frame };
    return opener.Open(whole.first(RaftWire::HeaderSize), whole.subspan(RaftWire::HeaderSize), tag);
}

/// A handshake between two sides, run to the end.
struct Handshake
{
    AcceptorHandshake::Judgement judgement;
    DiallerHandshake::Conclusion conclusion;
};

/// Run a whole handshake: @p dialler, as @p diallerId, dials @p target at an acceptor
/// that is @p acceptorId.
[[nodiscard]] Handshake Run(
    Side& acceptor, NodeId const& acceptorId, Side& dialler, NodeId const& diallerId, NodeId const& target)
{
    AcceptorHandshake accepting { acceptor.credential, acceptorId, acceptor.random };
    DiallerHandshake dialling { dialler.credential, diallerId, target, dialler.random };

    auto judgement = accepting.Judge(ProofFor(dialling, accepting.Challenge()));

    auto conclusion =
        judgement.verdict.has_value() ? dialling.Conclude(*judgement.verdict) : DiallerHandshake::Conclusion {};
    return Handshake { .judgement = std::move(judgement), .conclusion = std::move(conclusion) };
}

} // namespace

TEST_CASE("Two holders of the key complete a handshake and agree on the session", "[consensus][raft][session]")
{
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    auto const result = Run(acceptor, "n2", dialler, "n1", "n2");

    CHECK(result.judgement.outcome == ProofOutcome::Accepted);
    CHECK(result.judgement.dialler == "n1");
    CHECK(result.conclusion.outcome == VerdictOutcome::Accepted);
    CHECK(result.conclusion.acceptor == "n2");

    // The session both ends will MAC frames under is the same one, and it is made of two
    // different nonces -- a session whose nonces agreed by being equal would make the
    // dialler's contribution to freshness decorative.
    CHECK(result.judgement.nonces == result.conclusion.nonces);
    CHECK(result.judgement.nonces.acceptor != result.judgement.nonces.dialler);
}

TEST_CASE("A dialler that does not hold the key is refused, and told nothing", "[consensus][raft][session]")
{
    Side acceptor { ClusterKeyFill, 1 };

    SECTION("a different key")
    {
        Side stranger { StrangerKeyFill, 2 };
        auto const result = Run(acceptor, "n2", stranger, "n1", "n2");
        CHECK(result.judgement.outcome == ProofOutcome::Refused);
        CHECK_FALSE(result.judgement.verdict.has_value());

        // An id nobody proved is not one this node may name.
        CHECK(result.judgement.dialler.empty());
    }

    SECTION("no key at all: a proof whose tag is whatever a peer without one can send")
    {
        AcceptorHandshake accepting { acceptor.credential, "n2", acceptor.random };
        auto const judgement = accepting.Judge(
            RaftWire::ProofFrame { .dialler = "n1", .target = "n2", .nonce = DrawNonce(acceptor.random), .tag = {} });
        CHECK(judgement.outcome == ProofOutcome::Refused);
        CHECK_FALSE(judgement.verdict.has_value());
    }
}

TEST_CASE("A recorded proof cannot answer a later challenge", "[consensus][raft][session]")
{
    // The replay the acceptor's nonce exists to stop: a proof harvested from one
    // connection, presented on the next. Everything about it is genuine except the
    // challenge it answers.
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    AcceptorHandshake first { acceptor.credential, "n2", acceptor.random };
    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const recorded = ProofFor(dialling, first.Challenge());
    REQUIRE(first.Judge(recorded).outcome == ProofOutcome::Accepted);

    AcceptorHandshake second { acceptor.credential, "n2", acceptor.random };
    REQUIRE(second.Challenge().nonce != first.Challenge().nonce);
    CHECK(second.Judge(recorded).outcome == ProofOutcome::Refused);
}

TEST_CASE("A handshake judges one proof, even a genuine second one", "[consensus][raft][session]")
{
    // Spent whatever the outcome: a nonce that could answer twice is one a recorded
    // exchange could be replayed against on the same connection.
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    AcceptorHandshake accepting { acceptor.credential, "n2", acceptor.random };
    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const proof = ProofFor(dialling, accepting.Challenge());

    CHECK(accepting.Judge(proof).outcome == ProofOutcome::Accepted);
    CHECK(accepting.Judge(proof).outcome == ProofOutcome::Refused);
}

TEST_CASE("A tag from one half of the handshake does not verify as the other", "[consensus][raft][session]")
{
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    AcceptorHandshake accepting { acceptor.credential, "n2", acceptor.random };
    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const judgement = accepting.Judge(proof);
    REQUIRE(judgement.verdict.has_value());

    SECTION("the acceptor's verdict tag, reflected back as a proof")
    {
        AcceptorHandshake reflected { acceptor.credential, "n2", acceptor.random };
        auto echo = proof;
        echo.tag = Unwrap(judgement.verdict).tag;
        CHECK(reflected.Judge(echo).outcome == ProofOutcome::Refused);
    }

    SECTION("the dialler's own proof tag, reflected back as a verdict")
    {
        auto echo = Unwrap(judgement.verdict);
        echo.tag = proof.tag;
        CHECK(dialling.Conclude(echo).outcome == VerdictOutcome::Forged);
    }
}

TEST_CASE("A proven dialler that dialled another member is told so, signed", "[consensus][raft][session]")
{
    // #178's shape: the dialler's address for n9 now answers as n2. Both hold the key, so
    // the refusal is signed and the dialler can name the member that did answer rather
    // than reporting a key it does not have wrong.
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    auto const result = Run(acceptor, "n2", dialler, "n1", "n9");

    CHECK(result.judgement.outcome == ProofOutcome::WrongTarget);
    REQUIRE(result.judgement.verdict.has_value());
    CHECK(Unwrap(result.judgement.verdict).verdict == RaftWire::HandshakeVerdict::WrongTarget);
    CHECK(result.conclusion.outcome == VerdictOutcome::WrongTarget);
    CHECK(result.conclusion.acceptor == "n2");
}

TEST_CASE("A proven dialler claiming the acceptor's own id is told so, signed", "[consensus][raft][session]")
{
    // A cloned `--cluster-dir`, or a duplicated `--node-id`: two machines answering to n2.
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    auto const result = Run(acceptor, "n2", dialler, "n2", "n2");

    CHECK(result.judgement.outcome == ProofOutcome::OwnId);
    REQUIRE(result.judgement.verdict.has_value());
    CHECK(Unwrap(result.judgement.verdict).verdict == RaftWire::HandshakeVerdict::OwnId);
    CHECK(result.conclusion.outcome == VerdictOutcome::OwnId);
}

TEST_CASE("A verdict the dialler cannot authenticate is forged, whatever it says", "[consensus][raft][session]")
{
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };

    AcceptorHandshake accepting { acceptor.credential, "n2", acceptor.random };
    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const genuine = accepting.Judge(proof);
    REQUIRE(genuine.verdict.has_value());
    REQUIRE(dialling.Conclude(Unwrap(genuine.verdict)).outcome == VerdictOutcome::Accepted);

    SECTION("an acceptance signed with another key: an impostor at the address")
    {
        // An impostor judging the proof under its own key refuses it, and so has no
        // honest verdict to send. What it CAN send is a verdict it made up.
        Side impostor { StrangerKeyFill, 3 };
        auto forged = Unwrap(genuine.verdict);
        forged.tag = impostor.credential.Sign(RaftPeerMac::AcceptorVerdict, {});
        CHECK(dialling.Conclude(forged).outcome == VerdictOutcome::Forged);
    }

    SECTION("a genuine refusal with its verdict byte turned into an acceptance")
    {
        // On ONE dialler, which answered the challenge the verdict is about: a fresh
        // dialler would call any verdict forged for having answered nothing, and the case
        // would pass with the MAC removed.
        AcceptorHandshake refusing { acceptor.credential, "n2", acceptor.random };
        DiallerHandshake misdialled { dialler.credential, "n1", "n2-as-it-used-to-be", dialler.random };
        auto const refusal = refusing.Judge(ProofFor(misdialled, refusing.Challenge()));
        REQUIRE(refusal.verdict.has_value());
        REQUIRE(misdialled.Conclude(Unwrap(refusal.verdict)).outcome == VerdictOutcome::WrongTarget);

        auto tampered = Unwrap(refusal.verdict);
        tampered.verdict = RaftWire::HandshakeVerdict::Accepted;
        tampered.acceptor = "n2-as-it-used-to-be";
        CHECK(misdialled.Conclude(tampered).outcome == VerdictOutcome::Forged);
    }

    SECTION("a verdict for a challenge this dialler never answered")
    {
        DiallerHandshake unasked { dialler.credential, "n1", "n2", dialler.random };
        CHECK(unasked.Conclude(Unwrap(genuine.verdict)).outcome == VerdictOutcome::Forged);
    }
}

TEST_CASE("The handshake MACs are the documented fields, in the documented order", "[consensus][raft][session]")
{
    // Written out the long way against the credential, so the construction is pinned to
    // something other than the code that produces it: "the tests still pass" cannot show
    // a field transposed on both ends at once, because the tests would move with the code.
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };
    Cluster::PskRaftPeerCredential const key { Key(ClusterKeyFill) };

    AcceptorHandshake accepting { acceptor.credential, "n2", acceptor.random };
    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const judgement = accepting.Judge(proof);
    REQUIRE(judgement.verdict.has_value());

    auto const nonceA = std::span<std::byte const> { accepting.Challenge().nonce };
    auto const nonceD = std::span<std::byte const> { proof.nonce };
    auto const bytes = [](std::string_view text) {
        return WireFields::AsBytes(text);
    };

    CHECK(proof.tag
          == key.Sign(RaftPeerMac::DiallerProof, WireFields::AsFields({ nonceA, nonceD, bytes("n1"), bytes("n2") })));

    auto const accepted = std::array { std::byte { static_cast<std::uint8_t>(RaftWire::HandshakeVerdict::Accepted) } };
    CHECK(Unwrap(judgement.verdict).tag
          == key.Sign(
              RaftPeerMac::AcceptorVerdict,
              WireFields::AsFields({ nonceA, nonceD, bytes("n1"), bytes("n2"), std::span<std::byte const> { accepted } })));

    auto const frame = Heartbeat(7);
    auto const zero = WireFields::ToBigEndian<std::uint64_t>(0);
    FrameSealer sealer { dialler.credential, judgement.nonces };
    auto const whole = std::span<std::byte const> { frame };
    CHECK(sealer.Seal(frame)
          == key.Sign(RaftPeerMac::Frame,
                      WireFields::AsFields({ nonceA,
                                             nonceD,
                                             std::span<std::byte const> { zero },
                                             whole.first(RaftWire::HeaderSize),
                                             whole.subspan(RaftWire::HeaderSize) })));
}

TEST_CASE("A signed acceptance naming a member other than the one dialled is not acted on", "[consensus][raft][session]")
{
    // No acceptor of this build sends one -- it accepts only as itself, and only when it
    // is the target -- so the tag is built by hand, the way a holder of the key could.
    Side dialler { ClusterKeyFill, 2 };
    Cluster::PskRaftPeerCredential const key { Key(ClusterKeyFill) };
    ScriptedRandomSource fixed { { 7, 7, 7, 7 } };

    DiallerHandshake dialling { dialler.credential, "n1", "n2", dialler.random };
    auto const challenge = RaftWire::ChallengeFrame { .nonce = DrawNonce(fixed) };
    auto const proof = ProofFor(dialling, challenge);

    auto const accepted = std::array { std::byte { static_cast<std::uint8_t>(RaftWire::HandshakeVerdict::Accepted) } };
    auto const tag = key.Sign(RaftPeerMac::AcceptorVerdict,
                              WireFields::AsFields({ std::span<std::byte const> { challenge.nonce },
                                                     std::span<std::byte const> { proof.nonce },
                                                     WireFields::AsBytes(std::string_view { "n1" }),
                                                     WireFields::AsBytes(std::string_view { "n3" }),
                                                     std::span<std::byte const> { accepted } }));

    auto const conclusion =
        dialling.Conclude({ .verdict = RaftWire::HandshakeVerdict::Accepted, .acceptor = "n3", .tag = tag });
    CHECK(conclusion.outcome == VerdictOutcome::WrongTarget);
    CHECK(conclusion.acceptor == "n3");
}

TEST_CASE("A dialler whose id no handshake can carry refuses to send a proof", "[consensus][raft][session]")
{
    Side dialler { ClusterKeyFill, 2 };
    auto const challenge = RaftWire::ChallengeFrame {};

    DiallerHandshake tooLong {
        dialler.credential, std::string(RaftWire::MaxHandshakeIdBytes + 1, 'x'), "n2", dialler.random
    };
    auto const refused = tooLong.Answer(challenge);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("this node's id"));

    DiallerHandshake atTheBound {
        dialler.credential, std::string(RaftWire::MaxHandshakeIdBytes, 'x'), "n2", dialler.random
    };
    CHECK(atTheBound.Answer(challenge).has_value());
}

TEST_CASE("Session frames open in order, once, on their own session only", "[consensus][raft][session]")
{
    Side acceptor { ClusterKeyFill, 1 };
    Side dialler { ClusterKeyFill, 2 };
    auto const session = Run(acceptor, "n2", dialler, "n1", "n2").judgement.nonces;

    FrameSealer sealer { dialler.credential, session };
    auto const first = Heartbeat(1);
    auto const second = Heartbeat(2);
    auto const firstTag = sealer.Seal(first);
    auto const secondTag = sealer.Seal(second);

    SECTION("in order, every frame opens")
    {
        FrameOpener opener { acceptor.credential, session };
        CHECK(OpenWhole(opener, first, firstTag));
        CHECK(OpenWhole(opener, second, secondTag));
    }

    SECTION("a frame replayed on its own session is refused")
    {
        FrameOpener opener { acceptor.credential, session };
        REQUIRE(OpenWhole(opener, first, firstTag));
        CHECK_FALSE(OpenWhole(opener, first, firstTag));
    }

    SECTION("a frame out of order is refused: the one before it was dropped or delayed")
    {
        FrameOpener opener { acceptor.credential, session };
        CHECK_FALSE(OpenWhole(opener, second, secondTag));
    }

    SECTION("a frame whose bytes changed is refused")
    {
        FrameOpener opener { acceptor.credential, session };
        auto tampered = first;
        tampered.back() ^= std::byte { 0x01 };
        CHECK_FALSE(OpenWhole(opener, tampered, firstTag));
    }

    SECTION("a frame injected by something without the key is refused")
    {
        FrameOpener opener { acceptor.credential, session };
        Side stranger { StrangerKeyFill, 3 };
        FrameSealer forging { stranger.credential, session };
        CHECK_FALSE(OpenWhole(opener, first, forging.Seal(first)));
    }

    SECTION("a frame spliced in from another connection is refused")
    {
        // Same key, same two nodes, same position -- a different handshake.
        auto const other = Run(acceptor, "n2", dialler, "n1", "n2").judgement.nonces;
        REQUIRE(other != session);
        FrameOpener opener { acceptor.credential, other };
        CHECK_FALSE(OpenWhole(opener, first, firstTag));
    }
}
