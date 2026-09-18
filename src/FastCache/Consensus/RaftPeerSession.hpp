// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/X25519.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

/// How one Raft peer connection proves WHICH member dials it, and how every frame after that
/// stays bound to the proof (#1308, #178).
///
/// **Pure: no socket, no clock, no thread.** `RaftPeerServer` and `RaftPeerTransport`
/// drive these over real connections and `RaftClusterHarness` drives the same objects
/// over its in-memory network, so what the harness shows about a cluster is shown about
/// this code rather than about a copy of it.
///
/// ## The exchange
///
/// ```
/// acceptor                                        dialler
///   Challenge{nonceA, ephA}                ---->
///                                          <----  Proof{d, a, nonceD, ephD, sig_d}
///   verify sig_d under d's key, then OwnId, then WrongTarget
///   Verdict{verdict, self, sig_a}          ---->
///                                                 verify sig_a under a's key, then read it
///                                          <----  frame[tag] frame[tag] ...
/// ```
///
/// `sig_d`: the dialler's Ed25519 signature over `[nonceA, ephA, d, a, nonceD, ephD]`.
/// `sig_a`: the acceptor's, over all of that, `sig_d`, the verdict and its own id.
/// The frame key: HKDF-SHA256 over X25519(ephA, ephD), salted with both nonces, bound to
///               both ephemeral keys and both ids. Each frame's tag is an HMAC under it, over
///               its implicit position and its bytes (`Core/SessionSeal.hpp`).
///
/// ## Why each part is what it is
///
/// - **Each signature covers the WHOLE transcript so far.** Every field a peer sent is one it
///   can be held to: a signature that left one out would let whatever sits on the path change
///   that field and keep the signature -- an ephemeral key swapped for its own is a session key
///   it shares. `RaftPeerSession_test` changes each field in turn and requires each change to
///   be refused.
/// - **Signed by each end's OWN key.** A pre-shared key proved only "holds the cluster key", so
///   any holder could claim any id and removing one machine meant rotating the key on all the
///   others. A key per machine proves WHICH machine, and revoking one is one roster entry.
/// - **The acceptor challenges FIRST, before it has read a byte.** Its port is the surface
///   anything on the network can reach, so it signs nothing until the other end has proved an
///   id. A dialler signs only for an address it chose to dial.
/// - **Fresh values from BOTH ends in every signature.** The acceptor's nonce makes a proof
///   unreplayable; the dialler's makes the verdict fresh as well, so a recorded Accepted cannot
///   answer a dialler talking to something else.
/// - **Ids in the transcript, and NOT the endpoint.** What this produces is "the frames on this
///   connection come from d, for a", and the endpoint is routing neither side can state
///   identically: an acceptor binds the wildcard, a dialler reaches it through `--raft-self` or
///   NAT.
/// - **A verdict is SIGNED, including the refusals it can make.** A dialler told "you dialled
///   the wrong member" by a bare close could not tell that from an unknown key, and would report
///   a stale address book or a copied state directory as a key problem -- a confident wrong
///   signal. There is no oracle in it: a verdict exists only once the proof's signature has
///   verified, so only the member that signed learns anything.
/// - **The frame key is agreed, never sent.** Only the two ends hold their ephemeral secrets,
///   so a machine that recorded every handshake it ever saw still cannot open or forge a frame
///   of a session it was not an end of.
/// - **The position is implicit.** Both ends count, so a frame dropped, replayed or reordered
///   inside a connection fails its tag, and there is no field an attacker could set.
namespace FastCache::Consensus
{

/// What an acceptor decided about a proof.
///
/// Private: never transmitted. The verdict that travels is `RaftWire::HandshakeVerdict`, which
/// has no value for the refusals made about an unproven claim -- those are answered with
/// nothing.
enum class ProofOutcome : std::uint8_t
{
    Accepted,    ///< Proved its id, dialled this node, and is another member.
    Forged,      ///< The signature did not verify under the key the roster holds for the id it claims.
    UnknownKey,  ///< The roster holds no key for the id it claims.
    RevokedKey,  ///< The signature verified under a key the roster has revoked.
    WrongTarget, ///< Proved its id, but dialled another member.
    OwnId,       ///< Proved this node's own id: it holds this node's private key.
};

/// The acceptor's half of one connection's handshake.
///
/// One per connection, and judges ONE proof: the challenge is spent whatever the outcome, so a
/// second proof against it is refused even when it would verify -- a challenge that could answer
/// twice is one that can be replayed.
class AcceptorHandshake
{
  public:
    /// What the acceptor decided, and what it sends back.
    struct Judgement
    {
        ProofOutcome outcome { ProofOutcome::Forged }; ///< The decision.

        /// The signed answer to send: for a proven id, whatever it proved, and for a revoked
        /// key. Disengaged for `Forged` and `UnknownKey`, whose claims nobody proved.
        std::optional<RaftWire::VerdictFrame> verdict;

        /// The id the dialler proved. Empty unless its signature verified, because an id
        /// nobody proved is not one this node may name.
        NodeId dialler;

        /// The key the signature verified under: when `Accepted`, the key the session is
        /// re-checked against on every frame, and when `RevokedKey`, the revoked key -- what an
        /// operator matches against the revocation they made. Meaningless otherwise.
        Ed25519PublicKey provenKey {};

        /// The frame key, when `Accepted`.
        std::optional<SessionKey> session;
    };

    /// Begin a handshake by drawing its challenge: a nonce and an ephemeral key.
    ///
    /// A factory rather than a constructor because the draw can FAIL, and a handshake that
    /// cannot draw must not exist at all: run with a weak nonce, a restarted acceptor re-issues
    /// challenges an earlier run already issued, and a recorded proof answers them (#1527); run
    /// with a weak ephemeral secret, its session keys are guessable. So the caller is handed the
    /// refusal and closes the connection.
    /// @param identity Who this node is, and how it tells who the dialler is; must outlive the
    ///        handshake.
    /// @param random Where the nonce and the ephemeral secret come from.
    /// @return The handshake, or why nothing could be drawn.
    [[nodiscard]] static std::expected<AcceptorHandshake, SecureRandomError> Create(IRaftPeerIdentity const& identity,
                                                                                    ISecureRandom& random);

    /// @return The challenge to send, before reading anything.
    [[nodiscard]] RaftWire::ChallengeFrame const& Challenge() const noexcept;

    /// Judge the dialler's proof. The signature first, then what it claimed.
    /// @param proof The decoded proof; nothing about it is trusted yet.
    /// @return The decision and, where one is owed, the signed verdict to send.
    [[nodiscard]] Judgement Judge(RaftWire::ProofFrame const& proof);

  private:
    /// @param identity Who this node is.
    /// @param challenge The challenge, freshly drawn by `Create`.
    /// @param ephemeralSecret The secret half of the challenge's ephemeral key.
    AcceptorHandshake(IRaftPeerIdentity const& identity,
                      RaftWire::ChallengeFrame const& challenge,
                      SecureByteBuffer ephemeralSecret);

    IRaftPeerIdentity const& _identity;
    RaftWire::ChallengeFrame _challenge;
    SecureByteBuffer _ephemeralSecret;
    bool _spent { false };
};

/// What a dialler concluded from the acceptor's verdict.
///
/// Private: never transmitted.
enum class VerdictOutcome : std::uint8_t
{
    Accepted,           ///< The member it dialled proved its id and took this node.
    Forged,             ///< The verdict's signature did not verify under the key the roster holds for the id that answered.
    AcceptorKeyUnknown, ///< The roster holds no key for the id that answered.
    AcceptorKeyRevoked, ///< The id that answered signed with a key the roster has revoked.
    WrongTarget,        ///< The member that answered is not the one this node dialled.
    OwnId,              ///< The acceptor says it proved this node's own id from this node.
    OwnKeyRevoked,      ///< The acceptor says this node's key has been revoked.
};

/// The dialler's half of one connection's handshake.
class DiallerHandshake
{
  public:
    /// What the dialler concluded.
    struct Conclusion
    {
        VerdictOutcome outcome { VerdictOutcome::Forged }; ///< The decision.

        /// Which member answered, as its signed verdict says. Empty unless the signature
        /// verified.
        NodeId acceptor;

        /// The key the acceptor proved itself with -- the key the session is re-checked against
        /// before every frame. Meaningless unless `Accepted`.
        Ed25519PublicKey provenKey {};

        /// The frame key, when `Accepted`.
        std::optional<SessionKey> session;
    };

    /// Begin a handshake by drawing this end's nonce and ephemeral key.
    ///
    /// A factory for `AcceptorHandshake::Create`'s reason: a dialler whose nonce is weak makes a
    /// recorded `Accepted` verdict answer it, so a draw that fails is a connection abandoned.
    /// @param identity Who this node is, and how it tells who answered; must outlive the
    ///        handshake.
    /// @param target The member this node believes it dialled.
    /// @param random Where the nonce and the ephemeral secret come from.
    /// @return The handshake, or why nothing could be drawn.
    [[nodiscard]] static std::expected<DiallerHandshake, SecureRandomError> Create(IRaftPeerIdentity const& identity,
                                                                                   NodeId target,
                                                                                   ISecureRandom& random);

    /// Answer the acceptor's challenge.
    /// @param challenge The decoded challenge.
    /// @return The proof to send, or why this node cannot send one: an id no acceptor
    ///         could accept, which is refused here rather than by every peer.
    [[nodiscard]] std::expected<RaftWire::ProofFrame, std::string> Answer(RaftWire::ChallengeFrame const& challenge);

    /// Read the acceptor's verdict. The signature first, then what it says.
    /// @param verdict The decoded verdict; nothing about it is trusted yet.
    /// @return The conclusion. `Forged` when no challenge was answered first.
    [[nodiscard]] Conclusion Conclude(RaftWire::VerdictFrame const& verdict) const;

  private:
    /// @param identity Who this node is.
    /// @param target The member this node believes it dialled.
    /// @param nonce This end's nonce, freshly drawn by `Create`.
    /// @param ephemeralSecret The secret half of this end's ephemeral key.
    /// @param ephemeral Its public half.
    DiallerHandshake(IRaftPeerIdentity const& identity,
                     NodeId target,
                     Nonce const& nonce,
                     SecureByteBuffer ephemeralSecret,
                     X25519PublicKey const& ephemeral);

    IRaftPeerIdentity const& _identity;
    NodeId _target;
    Nonce _nonce;
    SecureByteBuffer _ephemeralSecret;
    X25519PublicKey _ephemeral;

    /// The challenge answered, and the proof it was answered with.
    std::optional<RaftWire::ChallengeFrame> _challenge;
    std::optional<RaftWire::ProofFrame> _proof;
};

} // namespace FastCache::Consensus
