// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftPeerCredential.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

/// How one Raft peer connection proves the cluster key, and how every frame after that
/// stays bound to the proof (#1308).
///
/// **Pure: no socket, no clock, no thread.** `RaftPeerServer` and `RaftPeerTransport`
/// drive these over real connections and `RaftClusterHarness` drives the same objects
/// over its in-memory network, so what the harness shows about a cluster is shown about
/// this code rather than about a copy of it.
///
/// ## The exchange
///
/// ```
/// acceptor                                   dialler
///   Challenge{nonceA}                  ---->
///                                      <----  Proof{d, a, nonceD, tag}
///   verify tag, then a == self, d != self
///   Verdict{verdict, self, tag}        ---->
///                                             verify tag, then read verdict
///                                      <----  frame[tag] frame[tag] ...
/// ```
///
/// Proof tag:    `Dialler`  MAC over `[nonceA, nonceD, d, a]`.
/// Verdict tag:  `Verdict`  MAC over `[nonceA, nonceD, d, acceptor id, verdict]`.
/// Frame tag:    `Frame`    MAC over `[nonceA, nonceD, seq, header, payload]`, where `seq`
///               counts this connection's frames from zero and never travels. The
///               header and payload are two fields so the receiver, which reads them
///               apart, MACs what it read without first copying them together.
///
/// ## Why each part is what it is
///
/// - **The acceptor challenges FIRST, before it has read a byte.** Its port is the
///   surface anything on the network can reach, so it signs nothing until the other
///   end has proved the key. A dialler gives a tag only to an address it chose to dial.
/// - **Both nonces in every MAC.** The acceptor's makes a proof unreplayable; the
///   dialler's makes the verdict fresh as well, so a recorded Accepted cannot answer a
///   dialler that is talking to something without the key.
/// - **Both ids in the handshake MACs, and NOT the endpoint.** Discovery binds a
///   `(node, endpoint)` pair because what it produces is an address somebody records.
///   What this produces is "the frames on this connection come from d, for a", and the
///   endpoint is routing that neither side can state identically: an acceptor binds the
///   wildcard, a dialler reaches it through `--raft-self` or NAT. A relay at some other
///   address can only forward frames whose MACs it cannot make, which the network
///   already can.
/// - **A verdict is SIGNED, including the refusals.** A dialler told "you dialled the
///   wrong member" by a bare close could not tell that from a wrong key, and would
///   report a stale address book or a cloned state directory as a key problem -- a
///   confident wrong signal. There is no oracle in it: a verdict exists only once the
///   proof's MAC has verified.
/// - **The sequence number is implicit.** Both ends count, so a frame dropped, replayed
///   or reordered inside a connection fails its MAC, and there is no field an attacker
///   could set.
///
/// What a pre-shared key cannot do, stated so nobody reads more into it: it proves "holds
/// the cluster key", not WHICH holder. A holder can claim any id. Telling holders apart
/// is #178's, and `IRaftPeerCredential` is where a per-node key would go.
namespace FastCache::Consensus
{

/// The two nonces one connection's handshake exchanged.
struct SessionNonces
{
    Nonce acceptor {}; ///< The acceptor's, from its Challenge.
    Nonce dialler {};  ///< The dialler's, from its Proof.

    /// Value equality, for tests.
    [[nodiscard]] bool operator==(SessionNonces const&) const = default;
};

/// What an acceptor decided about a proof.
///
/// Private: never transmitted. The verdict that travels is `RaftWire::HandshakeVerdict`,
/// which has no value for `Refused` -- a proof whose MAC failed is answered with nothing.
enum class ProofOutcome : std::uint8_t
{
    Accepted,    ///< Proved the key, for this node, as another member.
    Refused,     ///< The MAC did not verify, or this handshake has already judged a proof.
    WrongTarget, ///< Proved the key, but dialled another member.
    OwnId,       ///< Proved the key, but claims this node's own id.
};

/// The acceptor's half of one connection's handshake.
///
/// One per connection, and judges ONE proof: the nonce is spent whatever the outcome, so
/// a second proof against it is refused even when it would verify -- a nonce that could
/// answer twice is one that can be replayed.
class AcceptorHandshake
{
  public:
    /// What the acceptor decided, and what it sends back.
    struct Judgement
    {
        ProofOutcome outcome { ProofOutcome::Refused }; ///< The decision.

        /// The signed answer to send, for every outcome but `Refused`.
        std::optional<RaftWire::VerdictFrame> verdict;

        /// The connection's nonces; the session's, when `Accepted`.
        SessionNonces nonces;

        /// The id the dialler proved. Empty when `Refused`, because an id nobody proved
        /// is not one this node may name.
        NodeId dialler;
    };

    /// Draws the challenge nonce.
    /// @param credential What MACs are made and checked with; must outlive this.
    /// @param self This node's id.
    /// @param random Where the nonce comes from.
    AcceptorHandshake(IRaftPeerCredential const& credential, NodeId self, IRandomSource& random);

    /// @return The challenge to send, before reading anything.
    [[nodiscard]] RaftWire::ChallengeFrame const& Challenge() const noexcept;

    /// Judge the dialler's proof. The MAC first, then the ids it claimed.
    /// @param proof The decoded proof; nothing about it is trusted yet.
    /// @return The decision and, unless refused, the signed verdict to send.
    [[nodiscard]] Judgement Judge(RaftWire::ProofFrame const& proof);

  private:
    IRaftPeerCredential const& _credential;
    NodeId _self;
    RaftWire::ChallengeFrame _challenge;
    bool _spent { false };
};

/// What a dialler concluded from the acceptor's verdict.
///
/// Private: never transmitted.
enum class VerdictOutcome : std::uint8_t
{
    Accepted,    ///< The acceptor proved the key and took this node, as the member it dialled.
    Forged,      ///< The verdict's MAC did not verify: something without the key answered.
    WrongTarget, ///< The member that answered is not the one this node dialled.
    OwnId,       ///< The acceptor says this node's id is its own.
};

/// The dialler's half of one connection's handshake.
class DiallerHandshake
{
  public:
    /// What the dialler concluded.
    struct Conclusion
    {
        VerdictOutcome outcome { VerdictOutcome::Forged }; ///< The decision.

        /// Which member answered, as its signed verdict says. Empty when `Forged`.
        NodeId acceptor;

        /// The connection's nonces; the session's, when `Accepted`.
        SessionNonces nonces;
    };

    /// Draws this end's nonce.
    /// @param credential What MACs are made and checked with; must outlive this.
    /// @param self This node's id.
    /// @param target The member this node believes it dialled.
    /// @param random Where the nonce comes from.
    DiallerHandshake(IRaftPeerCredential const& credential, NodeId self, NodeId target, IRandomSource& random);

    /// Answer the acceptor's challenge.
    /// @param challenge The decoded challenge.
    /// @return The proof to send, or why this node cannot send one: an id no acceptor
    ///         could accept, which is refused here rather than by every peer.
    [[nodiscard]] std::expected<RaftWire::ProofFrame, std::string> Answer(RaftWire::ChallengeFrame const& challenge);

    /// Read the acceptor's verdict. The MAC first, then what it says.
    /// @param verdict The decoded verdict; nothing about it is trusted yet.
    /// @return The conclusion. `Forged` when no challenge was answered first.
    [[nodiscard]] Conclusion Conclude(RaftWire::VerdictFrame const& verdict) const;

  private:
    IRaftPeerCredential const& _credential;
    NodeId _self;
    NodeId _target;
    Nonce _nonce;
    std::optional<Nonce> _answered;
};

/// Seals the frames one end of a session sends.
///
/// Holds the session's position, so it is one per connection and never shared between
/// two: a second sealer over the same nonces would restart the count and produce tags
/// the opener refuses.
class FrameSealer
{
  public:
    /// @param credential What frames are MACed with; must outlive this.
    /// @param nonces The session's nonces.
    FrameSealer(IRaftPeerCredential const& credential, SessionNonces nonces) noexcept;

    /// The tag for the next frame, which moves the position on.
    /// @param frame The whole frame as `RaftWire::Encode` produced it: at least a header.
    /// @return The tag that goes after it.
    [[nodiscard]] Sha256::Digest Seal(std::span<std::byte const> frame);

  private:
    IRaftPeerCredential const& _credential;
    SessionNonces _nonces;
    std::uint64_t _next { 0 };
};

/// Checks the frames one end of a session receives.
class FrameOpener
{
  public:
    /// @param credential What frames are MACed with; must outlive this.
    /// @param nonces The session's nonces.
    FrameOpener(IRaftPeerCredential const& credential, SessionNonces nonces) noexcept;

    /// Whether @p tag is the next frame's, which moves the position on when it is.
    ///
    /// A refusal does not move it, and the caller ends the connection anyway: a session
    /// that has seen one frame it cannot account for has no position to resume from.
    /// @param header The frame's `RaftWire::HeaderSize` header bytes, as read.
    /// @param payload The frame's payload, as read.
    /// @param tag The tag that followed it.
    /// @return True when the frame is this session's next one.
    [[nodiscard]] bool Open(std::span<std::byte const> header,
                            std::span<std::byte const> payload,
                            Sha256::Digest const& tag);

  private:
    IRaftPeerCredential const& _credential;
    SessionNonces _nonces;
    std::uint64_t _next { 0 };
};

} // namespace FastCache::Consensus
