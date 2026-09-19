// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache::Consensus
{

/// Which signature a Raft peer handshake is asking for (#178).
///
/// **Private: never transmitted, never persisted.** Each value is mapped to a LABEL, and it is
/// the label that goes into what is signed -- so the ordinals carry no explicit values and an
/// insertion shifts nothing anything outside this process has seen.
enum class RaftPeerSignature : std::uint8_t
{
    /// The dialler's proof: who it is, whom it dialled, and both ends' fresh values.
    DiallerProof,

    /// The acceptor's verdict on that proof, over everything the proof covered and the proof's
    /// own signature.
    AcceptorVerdict,

    Last, ///< Not a signature, and has no row: the length of a table keyed by one.
};

/// What one signature is labelled with.
struct RaftPeerSignatureLabel
{
    RaftPeerSignature purpose; ///< The signature this row describes.
    std::string_view label;    ///< The bytes signed ahead of the transcript.
};

/// One row per `RaftPeerSignature`, in enumerator order.
///
/// A label of its own per purpose, so a proof reflected back as a verdict -- or the reverse --
/// is a signature over a different message and verifies as nothing. Versioned because it is
/// signed: changing one retires every handshake in flight, which is a stated act. `v2` because
/// the `v1` names were the pre-shared key's MAC labels (#1308), retired with it.
inline constexpr EnumTable<RaftPeerSignature, RaftPeerSignatureLabel> RaftPeerSignatureLabels { {
    { .purpose = RaftPeerSignature::DiallerProof, .label = "fastcache-raft-proof-v2" },
    { .purpose = RaftPeerSignature::AcceptorVerdict, .label = "fastcache-raft-verdict-v2" },
} };

static_assert(RowsInEnumeratorOrder(RaftPeerSignatureLabels, &RaftPeerSignatureLabel::purpose),
              "RaftPeerSignatureLabels must hold one row per RaftPeerSignature, in enumerator order");

/// Whether every label is present and no two are the same.
///
/// The two halves the pre-shared key's `SigningDomainTable` asserted of its MAC labels until #178
/// retired it, for signatures: an empty label separates
/// nothing, and a copied row -- a new purpose added by duplicating the line above it -- would make
/// one signature verify as the other.
/// @return True when the labels separate every purpose.
[[nodiscard]] consteval bool RaftPeerSignatureLabelsSeparate() noexcept
{
    for (auto const& row: RaftPeerSignatureLabels)
    {
        if (row.label.empty())
            return false;
        for (auto const& other: RaftPeerSignatureLabels)
            if (other.purpose != row.purpose && other.label == row.label)
                return false;
    }
    return true;
}

static_assert(RaftPeerSignatureLabelsSeparate(), "each RaftPeerSignature needs a label of its own");

/// What a signature check concluded about the member that claims to have signed.
///
/// Private: never transmitted. Each answer names something different to go and fix, so a
/// refusal that reports it moves a counter of its own.
enum class SignerCheck : std::uint8_t
{
    Verified, ///< It verifies under the key the roster holds for that id: the id is proven.
    Forged,   ///< It does not verify under that key: whoever signed does not hold the id's key.
    Unknown,  ///< The roster holds no key for that id, so nothing could be verified.
    Revoked,  ///< It verifies under a key the roster has REVOKED: a machine that was removed.
};

/// A signature check's conclusion, and the key it was reached under.
struct SignerVerdict
{
    SignerCheck check { SignerCheck::Forged }; ///< What was concluded.

    /// The key the signature verified under: the proven key when `Verified`, the revoked key
    /// when `Revoked`, and meaningless otherwise.
    Ed25519PublicKey key {};
};

/// Who this node is on the Raft peer wire, and how it tells who everybody else is (#178).
///
/// Replaced `IRaftPeerCredential`, the pre-shared key's seam, and **the wire moved with it**
/// (`RaftWire` 3 -> 4). That header said a per-node credential could replace the shared one
/// "without the wire moving", because every field a handshake MACed already named its ids. It
/// was wrong: a signature is 64 bytes where a MAC was 32, a session key agreed by
/// Diffie-Hellman needs both ends' ephemeral keys on the wire, and what a verifier needs is no
/// longer one secret but the SIGNER's public key -- so every handshake frame changed shape.
///
/// ## What an implementation owes
///
/// - `Verify` reads the signature BEFORE anything the signer claimed is reported on. The one
///   exception is unavoidable and stated: the claimed id selects which key to verify under, so
///   `Unknown` names a claim that was never proved -- which is why it is a counted refusal
///   answered with nothing, never a signed verdict.
/// - A signature over a transcript under one purpose never verifies under the other.
/// - Every call is safe from any thread at once, `IRaftPeerKeys`' rule carried up.
class IRaftPeerIdentity
{
  public:
    IRaftPeerIdentity() = default;
    IRaftPeerIdentity(IRaftPeerIdentity const&) = delete;
    IRaftPeerIdentity(IRaftPeerIdentity&&) = delete;
    IRaftPeerIdentity& operator=(IRaftPeerIdentity const&) = delete;
    IRaftPeerIdentity& operator=(IRaftPeerIdentity&&) = delete;
    virtual ~IRaftPeerIdentity() = default;

    /// @return The id this node proves itself as.
    [[nodiscard]] virtual NodeId const& Self() const noexcept = 0;

    /// Sign @p transcript for @p purpose, as this node.
    /// @param purpose Which signature this is.
    /// @param transcript The handshake's fields so far, in wire order.
    /// @return The signature.
    [[nodiscard]] virtual Ed25519Signature Sign(RaftPeerSignature purpose, WireFields::FieldList transcript) const = 0;

    /// Whether @p presented is @p signer's signature of @p transcript for @p purpose.
    /// @param purpose Which signature this is.
    /// @param signer The id that claims to have signed; this node's own id is verified under
    ///        this node's own key.
    /// @param transcript The handshake's fields, in wire order.
    /// @param presented The signature the peer sent.
    /// @return What was concluded, and under which key.
    [[nodiscard]] virtual SignerVerdict Verify(RaftPeerSignature purpose,
                                               NodeId const& signer,
                                               WireFields::FieldList transcript,
                                               Ed25519Signature const& presented) const = 0;

    /// Whether @p key is still the key the roster holds for @p peer.
    ///
    /// Asked of every frame of an open session, which is how an applied forget closes the
    /// sessions the forgotten member's key proved: the next frame either way is refused, rather than a session
    /// outliving the decision that removed its member.
    /// @param peer The member the session proved.
    /// @param key The key it proved itself with.
    /// @return True when the roster still names that key as the member's own.
    [[nodiscard]] virtual bool StillProves(NodeId const& peer, Ed25519PublicKey const& key) const = 0;
};

/// The Raft peer identity every node runs: Ed25519 signatures over a labelled transcript, under
/// this node's own key and the roster's keys for everybody else.
///
/// What is signed is `[label][field]...` in this project's length-prefixed field grammar, so
/// the label is a field like any other and no choice of first field can shift bytes across it
/// -- the construction the pre-shared key's `SignFields` used, carried from a MAC to a signature.
class RaftPeerIdentity final: public IRaftPeerIdentity
{
  public:
    /// @param self The id this node proves itself as.
    /// @param keys Its key material and the roster; must outlive this.
    RaftPeerIdentity(NodeId self, IRaftPeerKeys const& keys);

    [[nodiscard]] NodeId const& Self() const noexcept override;

    [[nodiscard]] Ed25519Signature Sign(RaftPeerSignature purpose, WireFields::FieldList transcript) const override;

    [[nodiscard]] SignerVerdict Verify(RaftPeerSignature purpose,
                                       NodeId const& signer,
                                       WireFields::FieldList transcript,
                                       Ed25519Signature const& presented) const override;

    [[nodiscard]] bool StillProves(NodeId const& peer, Ed25519PublicKey const& key) const override;

  private:
    NodeId _self;
    IRaftPeerKeys const& _keys;
};

} // namespace FastCache::Consensus
