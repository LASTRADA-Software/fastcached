// SPDX-License-Identifier: Apache-2.0
//
// The Raft peer handshake and the session it agrees, driven directly: no socket, so every
// case states exactly which bytes each end saw. What the server and transport do with an
// outcome -- close, count, log -- is theirs and is tested there.
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/X25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::SharedRoster;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::TestPeerIdentity;
using FastCache::Testing::Unwrap;
using namespace FastCache::Consensus;

namespace
{

/// The direction every case dials in unless it is about the direction: the one every voter uses.
constexpr auto OneWay = RaftWire::SessionDirection::OneWay;

/// The direction a member nobody dials uses, so the acceptor writes to it on the same connection.
constexpr auto TwoWay = RaftWire::SessionDirection::TwoWay;

/// The members every case's roster names, each under its own key.
[[nodiscard]] std::shared_ptr<SharedRoster> Roster()
{
    return SharedRoster::Of({ "n1", "n2", "n3" });
}

/// One machine's side of a connection: who it proves itself as, with which key, and its
/// randomness.
///
/// The operating system's generator, as production draws a nonce (#1527), rather than a seed
/// per side: what these cases need is that the two sides' values DIFFER, which is the very
/// property a seeded engine was found not to guarantee. A case that needs to know the bytes
/// scripts them with `ScriptedSecureRandom` instead.
struct Side
{
    TestPeerIdentity identity;
    SystemSecureRandom random;

    /// The honest machine @p id.
    /// @param id Its id, and the machine whose key it holds.
    /// @param roster What it believes about everybody else.
    Side(NodeId const& id, std::shared_ptr<SharedRoster const> roster):
        identity { id, TestKeyPair(id), std::move(roster) }
    {
    }

    /// A machine claiming @p claimed with @p machine's key.
    /// @param claimed The id it proves itself as.
    /// @param machine Whose key it holds.
    /// @param roster What it believes about everybody else.
    Side(NodeId const& claimed, std::string const& machine, std::shared_ptr<SharedRoster const> roster):
        identity { claimed, TestKeyPair(machine), std::move(roster) }
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

/// Seal a whole encoded frame, split the way a writer sends it.
[[nodiscard]] SessionTag SealWhole(FrameSealer& sealer, std::vector<std::byte> const& frame)
{
    auto const whole = std::span<std::byte const> { frame };
    return sealer.Seal(whole.first(RaftWire::HeaderSize), whole.subspan(RaftWire::HeaderSize));
}

/// Open a whole encoded frame, split the way a reader receives it.
[[nodiscard]] bool OpenWhole(FrameOpener& opener, std::vector<std::byte> const& frame, SessionTag const& tag)
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

/// Run a whole handshake: @p dialler dials @p target at @p acceptor, asking for @p direction.
[[nodiscard]] Handshake Run(Side& acceptor,
                            Side& dialler,
                            NodeId const& target,
                            RaftWire::SessionDirection direction = OneWay)
{
    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, target, direction, dialler.random).value();

    auto judgement = accepting.Judge(ProofFor(dialling, accepting.Challenge()));

    auto conclusion =
        judgement.verdict.has_value() ? dialling.Conclude(*judgement.verdict) : DiallerHandshake::Conclusion {};
    return Handshake { .judgement = std::move(judgement), .conclusion = std::move(conclusion) };
}

/// The bytes of one of a handshake's session keys, for comparing two ends' copies.
/// @param keys What one end agreed; required to exist.
/// @param way Which direction's key: the dialler's unless a case says otherwise.
[[nodiscard]] std::vector<std::byte> BytesOf(std::optional<SessionKeys> const& keys,
                                             SessionKey SessionKeys::* way = &SessionKeys::diallerToAcceptor)
{
    REQUIRE(keys.has_value());
    auto const bytes = (Unwrap(keys).*way).Bytes();
    return { bytes.begin(), bytes.end() };
}

/// @p text as the bytes a transcript carries.
[[nodiscard]] std::span<std::byte const> Text(std::string_view text)
{
    return WireFields::AsBytes(text);
}

/// What a signer signs for @p label over @p fields: `[label][field]...`, written out the long
/// way so the construction is pinned to something other than the code that produces it.
[[nodiscard]] std::vector<std::byte> Signed(std::string_view label, std::vector<std::span<std::byte const>> fields)
{
    fields.insert(fields.begin(), Text(label));
    return WireFields::Encode(WireFields::FieldList { fields });
}

} // namespace

TEST_CASE("Two members complete a handshake and agree on one session key", "[consensus][raft][session]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto const result = Run(acceptor, dialler, "n2");

    CHECK(result.judgement.outcome == ProofOutcome::Accepted);
    CHECK(result.judgement.dialler == "n1");
    CHECK(result.judgement.provenKey == TestKeyPair("n1").PublicKey());
    CHECK(result.conclusion.outcome == VerdictOutcome::Accepted);
    CHECK(result.conclusion.acceptor == "n2");
    CHECK(result.conclusion.provenKey == TestKeyPair("n2").PublicKey());

    // Both ends hold the SAME key, which neither sent: it was agreed.
    CHECK(BytesOf(result.judgement.session) == BytesOf(result.conclusion.session));
}

TEST_CASE("Each end's nonce and ephemeral key are the bytes its random seam drew", "[consensus][raft][session]")
{
    // Scripted per end, with bytes that differ between the ends, so a value taken from
    // anywhere but the seam -- an engine, the other end's source, a zeroed array -- shows as
    // the wrong bytes rather than as a handshake that still happens to complete (#1527).
    auto const roster = Roster();
    TestPeerIdentity const acceptorIdentity { "n2", TestKeyPair("n2"), roster };
    TestPeerIdentity const diallerIdentity { "n1", TestKeyPair("n1"), roster };
    ScriptedSecureRandom acceptorRandom { ScriptedSecureRandom::Ascending(NonceBytes + X25519KeyBytes, 0) };
    ScriptedSecureRandom diallerRandom { ScriptedSecureRandom::Ascending(NonceBytes + X25519KeyBytes, 100) };

    auto accepting = AcceptorHandshake::Create(acceptorIdentity, acceptorRandom).value();
    auto dialling = DiallerHandshake::Create(diallerIdentity, "n2", OneWay, diallerRandom).value();
    auto const proof = ProofFor(dialling, accepting.Challenge());

    auto const scripted = [](std::uint8_t first, std::size_t skip, std::size_t count) {
        auto const bytes = ScriptedSecureRandom::Ascending(skip + count, first);
        return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(skip), bytes.end());
    };
    auto const bytesOf = [](auto const& array) {
        return std::vector<std::byte>(array.begin(), array.end());
    };
    CHECK(bytesOf(accepting.Challenge().nonce) == scripted(0, 0, NonceBytes));
    CHECK(accepting.Challenge().ephemeral == Unwrap(X25519PublicKeyFrom(scripted(0, NonceBytes, X25519KeyBytes))));
    CHECK(bytesOf(proof.nonce) == scripted(100, 0, NonceBytes));
    CHECK(proof.ephemeral == Unwrap(X25519PublicKeyFrom(scripted(100, NonceBytes, X25519KeyBytes))));
    CHECK(accepting.Judge(proof).outcome == ProofOutcome::Accepted);
}

TEST_CASE("A handshake that cannot draw is not begun, at either end", "[consensus][raft][session]")
{
    // No handshake object exists to run with a weak value: the refusal IS the result, and it
    // carries the seam's own failure, so nothing else can have supplied the bytes (#1527).
    auto const roster = Roster();
    TestPeerIdentity const identity { "n1", TestKeyPair("n1"), roster };

    SECTION("the nonce")
    {
        ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };

        auto const accepting = AcceptorHandshake::Create(identity, denied);
        REQUIRE_FALSE(accepting.has_value());
        CHECK(accepting.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);

        auto const dialling = DiallerHandshake::Create(identity, "n2", OneWay, denied);
        REQUIRE_FALSE(dialling.has_value());
        CHECK(dialling.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);

        // Both ends ASKED -- a factory that refused without drawing would pass the checks above.
        CHECK(denied.FillCount() == 2);
    }

    SECTION("the ephemeral key, after the nonce was drawn")
    {
        // The second draw, which a factory that checked only the first would run without: a
        // session key agreed from an ephemeral secret nobody drew is one anybody can compute.
        ScriptedSecureRandom stops { ScriptedSecureRandom::Ascending(NonceBytes) };

        stops.DenyAfter(1, ScriptedSecureRandom::DeniedFailure());
        auto const accepting = AcceptorHandshake::Create(identity, stops);
        REQUIRE_FALSE(accepting.has_value());
        CHECK(accepting.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);

        ScriptedSecureRandom alsoStops { ScriptedSecureRandom::Ascending(NonceBytes) };
        alsoStops.DenyAfter(1, ScriptedSecureRandom::DeniedFailure());
        auto const dialling = DiallerHandshake::Create(identity, "n2", OneWay, alsoStops);
        REQUIRE_FALSE(dialling.has_value());
        CHECK(alsoStops.FillCount() == 2);
    }
}

TEST_CASE("A dialler the roster holds no key for is refused, and told nothing", "[consensus][raft][session]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side stranger { "n9", roster };

    auto const result = Run(acceptor, stranger, "n2");
    CHECK(result.judgement.outcome == ProofOutcome::UnknownKey);
    CHECK_FALSE(result.judgement.verdict.has_value());
    CHECK_FALSE(result.judgement.session.has_value());

    // An id nobody proved is not one this node may name.
    CHECK(result.judgement.dialler.empty());
}

TEST_CASE("n3, holding every byte it ever held, cannot prove n2's id", "[consensus][raft][session]")
{
    // #178's case (a): under the pre-shared key n3 held the key every member proved with, so any
    // id it claimed was an id it proved. It still holds its own private key and the whole roster
    // -- every PUBLIC key -- and neither signs as n2.
    auto const roster = Roster();
    Side acceptor { "n1", roster };
    Side impostor { "n2", "n3", roster };

    auto const result = Run(acceptor, impostor, "n1");
    CHECK(result.judgement.outcome == ProofOutcome::Forged);
    CHECK_FALSE(result.judgement.verdict.has_value());
    CHECK(result.judgement.dialler.empty());

    // And a proof n2 really made, recorded when n2 dialled n3, answers nothing at n1: it answers
    // n3's challenge and names n3 as the member dialled.
    Side n3 { "n3", roster };
    Side n2 { "n2", roster };
    auto recordedAt = AcceptorHandshake::Create(n3.identity, n3.random).value();
    auto n2Dialling = DiallerHandshake::Create(n2.identity, "n3", OneWay, n2.random).value();
    auto const recorded = ProofFor(n2Dialling, recordedAt.Challenge());
    REQUIRE(recordedAt.Judge(recorded).outcome == ProofOutcome::Accepted);

    auto replayedAt = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    CHECK(replayedAt.Judge(recorded).outcome == ProofOutcome::Forged);
}

TEST_CASE("A dialler whose key the cluster revoked is told so, signed", "[consensus][raft][session]")
{
    // The removed machine itself, still dialling with the key it always had. Its signature
    // verifies -- under a REVOKED key -- so the refusal is signed, and it reports its own removal
    // rather than a key problem at n1.
    auto const roster = Roster();
    roster->Revoke("n3");
    Side acceptor { "n1", roster };
    Side removed { "n3", roster };

    auto const result = Run(acceptor, removed, "n1");

    CHECK(result.judgement.outcome == ProofOutcome::RevokedKey);
    REQUIRE(result.judgement.verdict.has_value());
    CHECK(Unwrap(result.judgement.verdict).verdict == RaftWire::HandshakeVerdict::KeyRevoked);
    CHECK(result.judgement.provenKey == TestKeyPair("n3").PublicKey());
    CHECK_FALSE(result.judgement.session.has_value());

    // Not an id this node may name as a member: the key that proved it is nobody's now.
    CHECK(result.judgement.dialler.empty());

    CHECK(result.conclusion.outcome == VerdictOutcome::OwnKeyRevoked);
    CHECK(result.conclusion.acceptor == "n1");
    CHECK_FALSE(result.conclusion.session.has_value());
}

TEST_CASE("A recorded proof cannot answer a later challenge", "[consensus][raft][session]")
{
    // The replay the acceptor's nonce exists to stop: a proof harvested from one connection,
    // presented on the next. Everything about it is genuine except the challenge it answers.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto first = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const recorded = ProofFor(dialling, first.Challenge());
    REQUIRE(first.Judge(recorded).outcome == ProofOutcome::Accepted);

    auto second = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    REQUIRE(second.Challenge().nonce != first.Challenge().nonce);
    CHECK(second.Judge(recorded).outcome == ProofOutcome::Forged);
}

TEST_CASE("A handshake judges one proof, even a genuine second one", "[consensus][raft][session]")
{
    // Spent whatever the outcome: a challenge that could answer twice is one a recorded
    // exchange could be replayed against on the same connection.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const proof = ProofFor(dialling, accepting.Challenge());

    CHECK(accepting.Judge(proof).outcome == ProofOutcome::Accepted);
    CHECK(accepting.Judge(proof).outcome == ProofOutcome::Forged);
}

TEST_CASE("The proof's signature covers every field of the transcript", "[consensus][raft][session]")
{
    // #178: a signature that left one field out would let whatever sits on the path change that
    // field and keep the signature. Each section changes ONE field in transit and requires the
    // acceptor to refuse -- so dropping any one field from what is signed turns exactly one
    // section red. The ephemeral keys are the ones that matter most: swapped for its own, a
    // man in the middle would share the session key with each end.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const challenge = accepting.Challenge();

    // A key pair nobody at either end holds: what an attacker substitutes.
    SystemSecureRandom attacker;
    auto const attackerNonce = DrawNonce(attacker).value();
    auto attackerSecret = SecureByteBuffer(X25519KeyBytes);
    REQUIRE(attacker.Fill(attackerSecret).has_value());
    auto const attackerEphemeral = Unwrap(X25519PublicKeyFrom(attackerSecret));

    SECTION("the acceptor's nonce, as the dialler saw it")
    {
        auto seen = challenge;
        seen.nonce = attackerNonce;
        CHECK(accepting.Judge(ProofFor(dialling, seen)).outcome == ProofOutcome::Forged);
    }

    SECTION("the acceptor's ephemeral key, as the dialler saw it")
    {
        auto seen = challenge;
        seen.ephemeral = attackerEphemeral;
        CHECK(accepting.Judge(ProofFor(dialling, seen)).outcome == ProofOutcome::Forged);
    }

    SECTION("the dialler's id")
    {
        // To another member's id the roster holds a key for, so the refusal is the signature's
        // rather than an unknown key's.
        auto proof = ProofFor(dialling, challenge);
        proof.dialler = "n3";
        CHECK(accepting.Judge(proof).outcome == ProofOutcome::Forged);
    }

    SECTION("the member it dialled")
    {
        // Uncovered, this would be read as WrongTarget: a signed claim the dialler never made.
        auto proof = ProofFor(dialling, challenge);
        proof.target = "n3";
        CHECK(accepting.Judge(proof).outcome == ProofOutcome::Forged);
    }

    SECTION("the session direction it asked for")
    {
        // Uncovered, a relay could make an acceptor write on a connection the dialler only ever
        // writes on -- or starve a dialler that asked to be written to.
        auto proof = ProofFor(dialling, challenge);
        REQUIRE(proof.direction == OneWay);
        proof.direction = TwoWay;
        auto const judgement = accepting.Judge(proof);
        CHECK(judgement.outcome == ProofOutcome::Forged);
        CHECK_FALSE(judgement.session.has_value());
    }

    SECTION("the dialler's nonce")
    {
        auto proof = ProofFor(dialling, challenge);
        proof.nonce = attackerNonce;
        CHECK(accepting.Judge(proof).outcome == ProofOutcome::Forged);
    }

    SECTION("the dialler's ephemeral key")
    {
        auto proof = ProofFor(dialling, challenge);
        proof.ephemeral = attackerEphemeral;
        auto const judgement = accepting.Judge(proof);
        CHECK(judgement.outcome == ProofOutcome::Forged);
        CHECK_FALSE(judgement.session.has_value());
    }

    SECTION("the control: nothing changed")
    {
        CHECK(accepting.Judge(ProofFor(dialling, challenge)).outcome == ProofOutcome::Accepted);
    }
}

TEST_CASE("The verdict's signature covers what it decided and the exchange it decided about", "[consensus][raft][session]")
{
    auto const roster = Roster();

    SECTION("a signed refusal turned into an acceptance")
    {
        // The removed n3's own refusal, flipped in transit. It came from the member n3 dialled,
        // so the byte is the only thing standing between it and a session.
        roster->Revoke("n3");
        Side acceptor { "n1", roster };
        Side removed { "n3", roster };

        auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
        auto dialling = DiallerHandshake::Create(removed.identity, "n1", OneWay, removed.random).value();
        auto const refusal = accepting.Judge(ProofFor(dialling, accepting.Challenge()));
        REQUIRE(refusal.verdict.has_value());
        REQUIRE(dialling.Conclude(Unwrap(refusal.verdict)).outcome == VerdictOutcome::OwnKeyRevoked);

        auto tampered = Unwrap(refusal.verdict);
        tampered.verdict = RaftWire::HandshakeVerdict::Accepted;
        CHECK(dialling.Conclude(tampered).outcome == VerdictOutcome::Forged);
    }

    SECTION("a recorded acceptance, replayed to a dialler answering the same replayed challenge")
    {
        // Everything the acceptor signed is genuine; the dialler's fresh values are not the ones
        // it was signed over. Covered three ways -- the dialler's nonce, its ephemeral key, and its
        // proof's signature over both -- so this is what dropping all of them would open.
        Side acceptor { "n2", roster };
        Side dialler { "n1", roster };

        auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
        auto first = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
        auto const recorded = accepting.Judge(ProofFor(first, accepting.Challenge()));
        REQUIRE(recorded.verdict.has_value());

        auto second = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
        std::ignore = ProofFor(second, accepting.Challenge());
        CHECK(second.Conclude(Unwrap(recorded.verdict)).outcome == VerdictOutcome::Forged);
    }
}

TEST_CASE("A signature from one half of the handshake does not verify as the other", "[consensus][raft][session]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const judgement = accepting.Judge(proof);
    REQUIRE(judgement.verdict.has_value());

    SECTION("the acceptor's verdict signature, reflected back as a proof")
    {
        // n2's own signature, presented as n2's proof to n2: verified under n2's own key, so only
        // the label keeps it from reading as a copied state directory.
        auto reflected = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
        auto echo = proof;
        echo.dialler = "n2";
        echo.signature = Unwrap(judgement.verdict).signature;
        CHECK(reflected.Judge(echo).outcome == ProofOutcome::Forged);
    }

    SECTION("the dialler's own proof signature, reflected back as a verdict")
    {
        auto echo = Unwrap(judgement.verdict);
        echo.acceptor = "n1";
        echo.signature = proof.signature;
        CHECK(dialling.Conclude(echo).outcome == VerdictOutcome::Forged);
    }
}

TEST_CASE("A proven dialler that dialled another member is told so, signed", "[consensus][raft][session]")
{
    // The dialler's address for n3 now answers as n2. Both prove their ids, so the refusal is
    // signed and the dialler can name the member that did answer rather than reporting a key
    // problem.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto const result = Run(acceptor, dialler, "n3");

    CHECK(result.judgement.outcome == ProofOutcome::WrongTarget);
    REQUIRE(result.judgement.verdict.has_value());
    CHECK(Unwrap(result.judgement.verdict).verdict == RaftWire::HandshakeVerdict::WrongTarget);
    CHECK(result.conclusion.outcome == VerdictOutcome::WrongTarget);
    CHECK(result.conclusion.acceptor == "n2");
}

TEST_CASE("A dialler proving the acceptor's own id is told so, signed", "[consensus][raft][session]")
{
    // A copied `--cluster-dir`: two machines holding n2's private key. Only that key proves n2,
    // so this is the one way the refusal can arise -- and OwnId comes before WrongTarget, so
    // what it dialled does not change the diagnosis.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side copy { "n2", roster };

    for (auto const* const target: { "n2", "n3" })
    {
        CAPTURE(target);
        auto const result = Run(acceptor, copy, target);

        CHECK(result.judgement.outcome == ProofOutcome::OwnId);
        REQUIRE(result.judgement.verdict.has_value());
        CHECK(Unwrap(result.judgement.verdict).verdict == RaftWire::HandshakeVerdict::OwnId);
        CHECK(result.conclusion.outcome == VerdictOutcome::OwnId);
    }
}

TEST_CASE("A verdict the dialler cannot authenticate is refused, and says why", "[consensus][raft][session]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const genuine = accepting.Judge(proof);
    REQUIRE(genuine.verdict.has_value());
    REQUIRE(dialling.Conclude(Unwrap(genuine.verdict)).outcome == VerdictOutcome::Accepted);

    SECTION("an acceptance signed with another machine's key: an impostor at the address")
    {
        TestPeerIdentity const impostor { "n2", TestKeyPair("n3"), roster };
        auto forged = Unwrap(genuine.verdict);
        forged.signature = impostor.Sign(RaftPeerSignature::AcceptorVerdict, {});
        CHECK(dialling.Conclude(forged).outcome == VerdictOutcome::Forged);
    }

    SECTION("an answer from a member this node holds no key for")
    {
        auto unknown = Unwrap(genuine.verdict);
        unknown.acceptor = "n9";
        CHECK(dialling.Conclude(unknown).outcome == VerdictOutcome::AcceptorKeyUnknown);
    }

    SECTION("an answer signed with a key the cluster revoked")
    {
        // n2's genuine signature, read by a dialler whose roster has since revoked n2's key.
        roster->Revoke("n2");
        auto const conclusion = dialling.Conclude(Unwrap(genuine.verdict));
        CHECK(conclusion.outcome == VerdictOutcome::AcceptorKeyRevoked);
        CHECK_FALSE(conclusion.session.has_value());
    }

    SECTION("a verdict for a challenge this dialler never answered")
    {
        auto unasked = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
        CHECK(unasked.Conclude(Unwrap(genuine.verdict)).outcome == VerdictOutcome::Forged);
    }
}

TEST_CASE("The handshake signatures and the session key are the documented construction", "[consensus][raft][session]")
{
    // Written out the long way against the primitives, so the construction is pinned to
    // something other than the code that produces it: "the tests still pass" cannot show a
    // field transposed on both ends at once, because the tests would move with the code.
    auto const roster = Roster();
    TestPeerIdentity const acceptorIdentity { "n2", TestKeyPair("n2"), roster };
    TestPeerIdentity const diallerIdentity { "n1", TestKeyPair("n1"), roster };
    auto const acceptorScript = ScriptedSecureRandom::Ascending(NonceBytes + X25519KeyBytes, 0);
    auto const diallerScript = ScriptedSecureRandom::Ascending(NonceBytes + X25519KeyBytes, 100);
    ScriptedSecureRandom acceptorRandom { acceptorScript };
    ScriptedSecureRandom diallerRandom { diallerScript };

    // Two-way, so the direction byte is the one that is NOT a zeroed buffer's: a construction that
    // dropped it, or signed a constant in its place, is told apart from one that signed it.
    auto accepting = AcceptorHandshake::Create(acceptorIdentity, acceptorRandom).value();
    auto dialling = DiallerHandshake::Create(diallerIdentity, "n2", TwoWay, diallerRandom).value();
    auto const proof = ProofFor(dialling, accepting.Challenge());
    auto const judgement = accepting.Judge(proof);
    REQUIRE(judgement.verdict.has_value());
    auto const& challenge = accepting.Challenge();

    auto const nonceA = std::span<std::byte const> { challenge.nonce };
    auto const ephA = std::span<std::byte const> { challenge.ephemeral };
    auto const nonceD = std::span<std::byte const> { proof.nonce };
    auto const ephD = std::span<std::byte const> { proof.ephemeral };
    // The direction's byte, spelled as the number it is rather than through the enumerator.
    auto const twoWayByte = std::array { std::byte { 0x01 } };
    auto const dir = std::span<std::byte const> { twoWayByte };

    CHECK(Ed25519Verify(TestKeyPair("n1").PublicKey(),
                        Signed("fastcache-raft-proof-v3", { nonceA, ephA, Text("n1"), Text("n2"), dir, nonceD, ephD }),
                        proof.signature));

    auto const accepted = std::array { std::byte { static_cast<std::uint8_t>(RaftWire::HandshakeVerdict::Accepted) } };
    CHECK(Ed25519Verify(TestKeyPair("n2").PublicKey(),
                        Signed("fastcache-raft-verdict-v3",
                               { nonceA,
                                 ephA,
                                 Text("n1"),
                                 Text("n2"),
                                 dir,
                                 nonceD,
                                 ephD,
                                 std::span<std::byte const> { proof.signature },
                                 std::span<std::byte const> { accepted },
                                 Text("n2") }),
                        Unwrap(judgement.verdict).signature));

    // The session keys: one HKDF call per direction over the X25519 output, salted with both
    // nonces, bound to the label, the way, the direction, both ephemeral keys and both ids. The
    // acceptor's ephemeral secret is the scripted bytes after its nonce.
    auto const acceptorSecret = std::span<std::byte const> { acceptorScript }.subspan(NonceBytes);
    auto const shared = Unwrap(X25519SharedSecret(acceptorSecret, proof.ephemeral));
    std::vector<std::byte> salt { nonceA.begin(), nonceA.end() };
    salt.insert(salt.end(), nonceD.begin(), nonceD.end());
    auto const expectedFor = [&](std::string_view way) {
        auto const info =
            WireFields::Encode({ Text("fastcache-raft-session-v3"), Text(way), dir, ephA, ephD, Text("n1"), Text("n2") });
        auto const key = DeriveSessionKey(shared, salt, info).value();
        return std::vector<std::byte>(key.Bytes().begin(), key.Bytes().end());
    };
    CHECK(BytesOf(judgement.session, &SessionKeys::diallerToAcceptor) == expectedFor("dialler-to-acceptor"));
    CHECK(BytesOf(judgement.session, &SessionKeys::acceptorToDialler) == expectedFor("acceptor-to-dialler"));
}

TEST_CASE("A signed acceptance naming a member other than the one dialled is not acted on", "[consensus][raft][session]")
{
    // No acceptor of this build sends one -- it accepts only as itself, and only when it is the
    // target -- so the verdict is signed by hand, the way n3 could sign one of its own.
    auto const roster = Roster();
    Side dialler { "n1", roster };
    Side n3 { "n3", roster };

    auto challenging = AcceptorHandshake::Create(n3.identity, n3.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const& challenge = challenging.Challenge();
    auto const proof = ProofFor(dialling, challenge);

    auto const accepted = std::array { std::byte { static_cast<std::uint8_t>(RaftWire::HandshakeVerdict::Accepted) } };
    auto const oneWay = std::array { std::byte { static_cast<std::uint8_t>(OneWay) } };
    auto const signature = n3.identity.Sign(RaftPeerSignature::AcceptorVerdict,
                                            WireFields::AsFields({ std::span<std::byte const> { challenge.nonce },
                                                                   std::span<std::byte const> { challenge.ephemeral },
                                                                   Text("n1"),
                                                                   Text("n2"),
                                                                   std::span<std::byte const> { oneWay },
                                                                   std::span<std::byte const> { proof.nonce },
                                                                   std::span<std::byte const> { proof.ephemeral },
                                                                   std::span<std::byte const> { proof.signature },
                                                                   std::span<std::byte const> { accepted },
                                                                   Text("n3") }));

    auto const conclusion =
        dialling.Conclude({ .verdict = RaftWire::HandshakeVerdict::Accepted, .acceptor = "n3", .signature = signature });
    CHECK(conclusion.outcome == VerdictOutcome::WrongTarget);
    CHECK(conclusion.acceptor == "n3");
    CHECK_FALSE(conclusion.session.has_value());
}

TEST_CASE("A proven dialler whose ephemeral key is low-order agrees no session", "[consensus][raft][session]")
{
    // An all-zero X25519 key fixes the "shared" secret to one everybody knows. Only a member can
    // send it -- the signature has to verify -- and no honest one does, so it is refused as a
    // proof that did not hold up, and NO verdict is signed for a session this end cannot open.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    TestPeerIdentity const member { "n1", TestKeyPair("n1"), roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto const& challenge = accepting.Challenge();
    auto proof = RaftWire::ProofFrame { .dialler = "n1",
                                        .target = "n2",
                                        .direction = OneWay,
                                        .nonce = DrawNonce(acceptor.random).value(),
                                        .ephemeral = {},
                                        .signature = {} };
    auto const oneWay = std::array { std::byte { static_cast<std::uint8_t>(OneWay) } };
    proof.signature = member.Sign(RaftPeerSignature::DiallerProof,
                                  WireFields::AsFields({ std::span<std::byte const> { challenge.nonce },
                                                         std::span<std::byte const> { challenge.ephemeral },
                                                         Text("n1"),
                                                         Text("n2"),
                                                         std::span<std::byte const> { oneWay },
                                                         std::span<std::byte const> { proof.nonce },
                                                         std::span<std::byte const> { proof.ephemeral } }));

    auto const judgement = accepting.Judge(proof);
    CHECK(judgement.outcome == ProofOutcome::Forged);
    CHECK_FALSE(judgement.verdict.has_value());
    CHECK_FALSE(judgement.session.has_value());
}

TEST_CASE("A dialler whose id no handshake can carry refuses to send a proof", "[consensus][raft][session]")
{
    auto const roster = Roster();
    SystemSecureRandom random;
    auto const challenge = RaftWire::ChallengeFrame {};

    TestPeerIdentity const tooLongIdentity { std::string(RaftWire::MaxHandshakeIdBytes + 1, 'x'),
                                             TestKeyPair("n1"),
                                             roster };
    auto tooLong = DiallerHandshake::Create(tooLongIdentity, "n2", OneWay, random).value();
    auto const refused = tooLong.Answer(challenge);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("this node's id"));

    TestPeerIdentity const atTheBoundIdentity { std::string(RaftWire::MaxHandshakeIdBytes, 'x'), TestKeyPair("n1"), roster };
    auto atTheBound = DiallerHandshake::Create(atTheBoundIdentity, "n2", OneWay, random).value();
    CHECK(atTheBound.Answer(challenge).has_value());
}

TEST_CASE("Session frames open in order, once, on their own session only", "[consensus][raft][session]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };
    auto session = Run(acceptor, dialler, "n2");
    REQUIRE(session.conclusion.session.has_value());
    REQUIRE(session.judgement.session.has_value());

    FrameSealer sealer { Unwrap(session.conclusion.session).diallerToAcceptor };
    auto const first = Heartbeat(1);
    auto const second = Heartbeat(2);
    auto const firstTag = SealWhole(sealer, first);
    auto const secondTag = SealWhole(sealer, second);

    SECTION("in order, every frame opens at the other end")
    {
        FrameOpener opener { Unwrap(session.judgement.session).diallerToAcceptor };
        CHECK(OpenWhole(opener, first, firstTag));
        CHECK(OpenWhole(opener, second, secondTag));
    }

    SECTION("a frame replayed on its own session is refused")
    {
        FrameOpener opener { Unwrap(session.judgement.session).diallerToAcceptor };
        REQUIRE(OpenWhole(opener, first, firstTag));
        CHECK_FALSE(OpenWhole(opener, first, firstTag));
    }

    SECTION("a frame out of order is refused: the one before it was dropped or delayed")
    {
        FrameOpener opener { Unwrap(session.judgement.session).diallerToAcceptor };
        CHECK_FALSE(OpenWhole(opener, second, secondTag));
    }

    SECTION("a frame whose bytes changed is refused")
    {
        FrameOpener opener { Unwrap(session.judgement.session).diallerToAcceptor };
        auto tampered = first;
        tampered.back() ^= std::byte { 0x01 };
        CHECK_FALSE(OpenWhole(opener, tampered, firstTag));
    }

    SECTION("a frame spliced in from another connection between the same two members is refused")
    {
        // Same two members, same keys, same position -- a different handshake, so a different
        // agreed key. What the pre-shared key could not tell apart without both nonces in every
        // tag is now apart by construction.
        auto const other = Run(acceptor, dialler, "n2");
        REQUIRE(BytesOf(other.judgement.session) != BytesOf(session.judgement.session));
        FrameOpener opener { Unwrap(other.judgement.session).diallerToAcceptor };
        CHECK_FALSE(OpenWhole(opener, first, firstTag));
    }
}

TEST_CASE("A two-way handshake agrees two different keys and both ends hold the same pair",
          "[consensus][raft][session][handshake][formation]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", TwoWay, dialler.random).value();

    auto const proof = ProofFor(dialling, accepting.Challenge());
    CHECK(proof.direction == TwoWay);
    auto judgement = accepting.Judge(proof);
    REQUIRE(judgement.outcome == ProofOutcome::Accepted);
    CHECK(judgement.direction == TwoWay);
    REQUIRE(judgement.verdict.has_value());
    auto conclusion = dialling.Conclude(Unwrap(judgement.verdict));
    REQUIRE(conclusion.outcome == VerdictOutcome::Accepted);

    CHECK(BytesOf(judgement.session, &SessionKeys::diallerToAcceptor)
          == BytesOf(conclusion.session, &SessionKeys::diallerToAcceptor));
    CHECK(BytesOf(judgement.session, &SessionKeys::acceptorToDialler)
          == BytesOf(conclusion.session, &SessionKeys::acceptorToDialler));
    // One key per direction.
    CHECK(BytesOf(judgement.session, &SessionKeys::diallerToAcceptor)
          != BytesOf(judgement.session, &SessionKeys::acceptorToDialler));
}

TEST_CASE("A one-way handshake reports the direction its dialler signed", "[consensus][raft][session][handshake][formation]")
{
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto const result = Run(acceptor, dialler, "n2", OneWay);
    REQUIRE(result.judgement.outcome == ProofOutcome::Accepted);
    CHECK(result.judgement.direction == OneWay);
    REQUIRE(result.conclusion.outcome == VerdictOutcome::Accepted);
}

TEST_CASE("A session direction changed in transit is refused as a forged proof, either way",
          "[consensus][raft][session][handshake][formation]")
{
    // The acceptor's reading of the direction is what the proof SAYS; what the dialler SIGNED is
    // the only direction that verifies. A relay turning one into the other is a forgery whichever
    // way it turns it, refused with no verdict and no session -- never an accepted connection of
    // the shape the relay chose.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto const refusedWhenTurned = [&](RaftWire::SessionDirection signedAs, RaftWire::SessionDirection arrivesAs) {
        auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
        auto dialling = DiallerHandshake::Create(dialler.identity, "n2", signedAs, dialler.random).value();
        auto proof = ProofFor(dialling, accepting.Challenge());
        REQUIRE(proof.direction == signedAs);
        proof.direction = arrivesAs;
        return accepting.Judge(proof);
    };

    SECTION("signed one-way, arriving as two-way")
    {
        auto const judgement = refusedWhenTurned(OneWay, TwoWay);
        CHECK(judgement.outcome == ProofOutcome::Forged);
        CHECK_FALSE(judgement.verdict.has_value());
        CHECK_FALSE(judgement.session.has_value());
    }

    SECTION("signed two-way, arriving as one-way")
    {
        auto const judgement = refusedWhenTurned(TwoWay, OneWay);
        CHECK(judgement.outcome == ProofOutcome::Forged);
        CHECK_FALSE(judgement.verdict.has_value());
        CHECK_FALSE(judgement.session.has_value());
    }

    SECTION("the control: each direction arriving as it was signed is accepted")
    {
        CHECK(refusedWhenTurned(OneWay, OneWay).outcome == ProofOutcome::Accepted);
        CHECK(refusedWhenTurned(TwoWay, TwoWay).outcome == ProofOutcome::Accepted);
    }
}

TEST_CASE("A verdict over another direction than the one the dialler signed is refused as forged",
          "[consensus][raft][session][handshake][formation]")
{
    // The dialler's side of the same property: its verdict check re-derives the transcript from
    // the proof IT sent, direction included. So an acceptor that accepted a two-way proof from
    // this member -- same nonce, same ephemeral key, genuinely signed -- has signed a verdict this
    // one-way dialler cannot verify. The two-way proof is signed by hand with the member's own
    // key, which is the only way one exists beside a one-way proof over the same fresh values.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };

    auto accepting = AcceptorHandshake::Create(acceptor.identity, acceptor.random).value();
    auto dialling = DiallerHandshake::Create(dialler.identity, "n2", OneWay, dialler.random).value();
    auto const& challenge = accepting.Challenge();
    auto turned = ProofFor(dialling, challenge);
    turned.direction = TwoWay;
    auto const twoWay = std::array { std::byte { static_cast<std::uint8_t>(TwoWay) } };
    turned.signature = dialler.identity.Sign(RaftPeerSignature::DiallerProof,
                                             WireFields::AsFields({ std::span<std::byte const> { challenge.nonce },
                                                                    std::span<std::byte const> { challenge.ephemeral },
                                                                    Text("n1"),
                                                                    Text("n2"),
                                                                    std::span<std::byte const> { twoWay },
                                                                    std::span<std::byte const> { turned.nonce },
                                                                    std::span<std::byte const> { turned.ephemeral } }));

    auto const judgement = accepting.Judge(turned);
    REQUIRE(judgement.outcome == ProofOutcome::Accepted);
    REQUIRE(judgement.direction == TwoWay);
    REQUIRE(judgement.verdict.has_value());

    auto const conclusion = dialling.Conclude(Unwrap(judgement.verdict));
    CHECK(conclusion.outcome == VerdictOutcome::Forged);
    CHECK_FALSE(conclusion.session.has_value());
}

TEST_CASE("A frame sealed for one direction does not open in the other", "[consensus][raft][session][handshake][formation]")
{
    // Both ends count positions from zero. Under one shared key the acceptor's first frame and the
    // dialler's first frame would be sealed at the same position under the same key, so a frame
    // reflected back at the end that sent it would open there as the other end's. Each direction
    // has its own key, so it does not.
    auto const roster = Roster();
    Side acceptor { "n2", roster };
    Side dialler { "n1", roster };
    auto const session = Run(acceptor, dialler, "n2", TwoWay);
    REQUIRE(session.judgement.outcome == ProofOutcome::Accepted);
    REQUIRE(session.conclusion.outcome == VerdictOutcome::Accepted);
    auto const& atAcceptor = Unwrap(session.judgement.session);
    auto const& atDialler = Unwrap(session.conclusion.session);

    auto const frame = Heartbeat(1, "n2");

    SECTION("the acceptor's frame, reflected back to the acceptor")
    {
        FrameSealer acceptorWrites { atAcceptor.acceptorToDialler };
        auto const tag = SealWhole(acceptorWrites, frame);
        FrameOpener acceptorReads { atAcceptor.diallerToAcceptor };
        CHECK_FALSE(OpenWhole(acceptorReads, frame, tag));
    }

    SECTION("the dialler's frame, reflected back to the dialler")
    {
        FrameSealer diallerWrites { atDialler.diallerToAcceptor };
        auto const tag = SealWhole(diallerWrites, frame);
        FrameOpener diallerReads { atDialler.acceptorToDialler };
        CHECK_FALSE(OpenWhole(diallerReads, frame, tag));
    }

    SECTION("the control: each direction's frame opens at the other end")
    {
        FrameSealer acceptorWrites { atAcceptor.acceptorToDialler };
        FrameOpener diallerReads { atDialler.acceptorToDialler };
        CHECK(OpenWhole(diallerReads, frame, SealWhole(acceptorWrites, frame)));

        FrameSealer diallerWrites { atDialler.diallerToAcceptor };
        FrameOpener acceptorReads { atAcceptor.diallerToAcceptor };
        CHECK(OpenWhole(acceptorReads, frame, SealWhole(diallerWrites, frame)));
    }
}

TEST_CASE("The Raft signature labels moved to v3 and no retired label is live",
          "[consensus][raft][session][handshake][formation]")
{
    // The transcript gained a field, which is another construction, and a label names one. So the
    // labels moved, and the ones they replaced are retired rather than free for a later reuse.
    // That no live label is a retired one is `IdentityKeyLabelsSeparate`'s, a build failure.
    for (auto const& row: RaftPeerSignatureLabels)
    {
        CAPTURE(LabelOf(row.construction));
        CHECK(LabelOf(row.construction).ends_with("-v3"));
    }
    CHECK(std::ranges::contains(RetiredIdentityKeyLabels, std::string_view { "fastcache-raft-proof-v2" }));
    CHECK(std::ranges::contains(RetiredIdentityKeyLabels, std::string_view { "fastcache-raft-verdict-v2" }));
}
