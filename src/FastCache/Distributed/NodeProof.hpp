// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/X25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace FastCache::Distributed
{

/// Which machine a caller on the `0xFC` surface is, proved, and the key its frames are sealed
/// under from then on (#178).
///
/// ## What this is for
///
/// Admission on that surface used to be decided by the caller's SOURCE ADDRESS, and then by a MAC
/// under the cluster's pre-shared key. An address stops standing in for *this is one of our nodes*
/// the moment it is not stable; a shared key never said WHICH node, so removing one machine meant
/// rotating the key on all the others, and a removed machine kept every byte it needed to prove
/// itself. This is the replacement: each machine signs with its own identity key, the one the
/// cluster admitted, and a revocation is one roster entry.
///
/// ## The handshake
///
/// 1. `NodeChallenge{nonceC, ephC}`: the caller's nonce and ephemeral X25519 key.
/// 2. `{serverId, serverKey, nonceS, ephS, sigS}`: the server says who it is and signs everything
///    so far under `NodeProofSignature::ServerChallenge`. A caller that holds a roster checks that
///    `serverKey` is a live voter's before it proves anything -- so a revoked ex-scheduler still
///    named in a worker's `--scheduler` is refused by the WORKER.
/// 3. `ProveNode{clientId, clientKey, sigC}`: the caller signs everything so far, `sigS` included,
///    under `NodeProofSignature::NodeProof`. The server verifies under `clientKey` first and asks
///    its roster second.
///
/// ## Why every frame after it is SEALED
///
/// A signature proves who signed THIS handshake; it says nothing about the frames that follow on
/// the connection. Without a seal, a machine the worker dials -- a revoked ex-scheduler still in
/// its `--scheduler` list, or anything on the path -- relays the handshake to the real scheduler,
/// lets the genuine proof through, and then injects verbs of its own on the connection the proof
/// admitted. So both ends derive a key per DIRECTION from the X25519 exchange, which the relay
/// cannot compute, and every later frame carries an HMAC under it (`Core/SessionSeal`): an
/// injected frame fails its tag and the connection closes. That is the property the seal is for,
/// and `FrameEndpoint_test` holds the endpoint to it with a relayed handshake over a real socket.
///
/// ## Pure
///
/// No socket, no clock, no randomness: the ephemeral secret and the nonce arrive from a caller
/// that drew them through `ISecureRandom`, and the identity key from one that read it once.

// The wire's widths against the types that fill them. `CompileCacheWire.hpp` spells its own
// numbers because the launcher compiles it in and it may include nothing from `Core/` beyond
// three leaf headers; this is the one file where both are visible, so it is the only place the
// two can be held equal. A BUILD failure rather than a test, because a field silently truncated
// has both ends sign different inputs and report a forgery for a version mismatch.
static_assert(NonceBytes == CompileCacheWire::NodeChallengeBytes,
              "the nonce field on the wire must be exactly as wide as a Nonce");
static_assert(X25519KeyBytes == CompileCacheWire::NodeEphemeralKeyBytes,
              "the ephemeral field on the wire must be exactly as wide as an X25519 public key");
static_assert(Ed25519PublicKeyBytes == CompileCacheWire::IdentityPublicKeyBytes,
              "the identity key field on the wire must be exactly as wide as an Ed25519 public key");
static_assert(Ed25519SignatureBytes == CompileCacheWire::NodeSignatureBytes,
              "the signature field on the wire must be exactly as wide as an Ed25519 signature");
static_assert(SessionTagBytes == CompileCacheWire::SealedFrameTagBytes,
              "the tag after a sealed frame must be exactly as wide as a session tag");

/// The two signatures the handshake carries.
///
/// **PRIVATE: persisted and transmitted nowhere** -- what travels is the LABEL each row names, and
/// the enumerator only selects it.
enum class NodeProofSignature : std::uint8_t
{
    ServerChallenge, ///< The server's, over the caller's half and its own.
    NodeProof,       ///< The caller's, over the whole handshake including the server's signature.
    Last,            ///< Not a signature, and has no row: the length of a table keyed by one.
};

/// One row of `NodeProofSignatureLabels`.
struct NodeProofSignatureLabel
{
    NodeProofSignature purpose; ///< The signature this row describes.
    std::string_view label;     ///< The bytes signed ahead of the transcript.
};

/// What each signature signs ahead of its transcript.
///
/// Versioned and distinct, so a signature made for one purpose -- or for the Raft handshake, the
/// lease or the roster -- verifies as nothing else. `v2` because `fastcache-node-proof-v1` was the
/// pre-shared key's MAC label; a retired label is never reused, so a MAC input and a signed message
/// can never be the same bytes.
inline constexpr EnumTable<NodeProofSignature, NodeProofSignatureLabel> NodeProofSignatureLabels { {
    { .purpose = NodeProofSignature::ServerChallenge, .label = "fastcache-node-challenge-v2" },
    { .purpose = NodeProofSignature::NodeProof, .label = "fastcache-node-proof-v2" },
} };

static_assert(RowsInEnumeratorOrder(NodeProofSignatureLabels, &NodeProofSignatureLabel::purpose),
              "NodeProofSignatureLabels must hold one row per NodeProofSignature, in enumerator order");

/// The labels this wire has retired: never signed again, so never reused for a different purpose.
inline constexpr std::array<std::string_view, 1> RetiredNodeProofLabels { "fastcache-node-proof-v1" };

/// A fresh ephemeral X25519 key pair, whose secret half lives in `SecureByteBuffer`.
struct NodeEphemeral
{
    SecureByteBuffer secret;   ///< Never transmitted; wiped at every release.
    X25519PublicKey publicKey; ///< What travels.
};

/// Draw a fresh ephemeral key pair.
/// @param random Where the secret comes from.
/// @return The pair, or why nothing could be drawn -- which a caller answers by refusing the
///         handshake, never by drawing from anywhere else (#1527).
[[nodiscard]] std::expected<NodeEphemeral, SecureRandomError> DrawNodeEphemeral(ISecureRandom& random);

/// The server's half of the handshake, before it is signed. Its identity key is the signing
/// pair's own public half, never a second field that could disagree with it.
struct ServerHello
{
    std::string_view serverId; ///< The node id the server claims.
    Nonce nonce;               ///< Its nonce: the challenge a proof answers.
    X25519PublicKey ephemeral; ///< Its ephemeral key.
};

/// Sign and frame the server's reply to @p request.
/// @param identity The server's identity key pair; its public half is the key the reply names.
/// @param request The caller's opening, as received.
/// @param hello The server's half.
/// @return The reply, signature included.
[[nodiscard]] CompileCacheWire::NodeChallengeReply AnswerNodeChallenge(Ed25519KeyPair const& identity,
                                                                       CompileCacheWire::NodeChallengeRequest const& request,
                                                                       ServerHello const& hello);

/// Whether @p reply's signature verifies under the key it names, over @p request as this caller sent it.
///
/// Says nothing about whether that key is one the caller should TRUST -- a caller holding a roster
/// asks that separately, of the key this has shown signed.
/// @param request What this caller sent.
/// @param reply What came back.
/// @return True when the server holds the key it named.
[[nodiscard]] bool VerifyNodeChallengeReply(CompileCacheWire::NodeChallengeRequest const& request,
                                            CompileCacheWire::NodeChallengeReply const& reply);

/// Sign the caller's proof over the whole handshake.
/// @param identity The caller's identity key pair.
/// @param clientId The id it claims.
/// @param request What it sent.
/// @param reply What the server answered, signature included.
/// @return The proof.
[[nodiscard]] CompileCacheWire::ProveNodeRequest MintNodeProof(Ed25519KeyPair const& identity,
                                                               std::string_view clientId,
                                                               CompileCacheWire::NodeChallengeRequest const& request,
                                                               CompileCacheWire::NodeChallengeReply const& reply);

/// Whether @p proof's signature verifies under the key it presents, over this connection's handshake.
///
/// The first question a server asks, and the only one before the roster: a caller who cannot sign
/// learns nothing about which ids and keys the cluster holds.
/// @param request What the caller opened with.
/// @param reply What this server answered, signature included.
/// @param proof What the caller presented.
/// @return True when the caller holds the key it presented.
[[nodiscard]] bool VerifyNodeProof(CompileCacheWire::NodeChallengeRequest const& request,
                                   CompileCacheWire::NodeChallengeReply const& reply,
                                   CompileCacheWire::ProveNodeRequest const& proof);

/// The two keys a proven connection is sealed under, one per direction.
///
/// Two because `Core/SessionSeal` holds one POSITION per key: a request/reply wire that sealed
/// both directions under one key would have two senders claiming the same positions, and each end
/// would refuse the other's frames.
struct NodeSessionKeys
{
    SessionKey callerToServer; ///< What the caller seals its requests under.
    SessionKey serverToCaller; ///< What the server seals its replies under.
};

/// Derive a proven connection's session keys.
///
/// HKDF over the X25519 output, salted with both nonces and bound to both ephemeral keys and both
/// ids, so a key agreed in one exchange is the key of no other.
/// @param ownSecret This end's ephemeral secret.
/// @param request The caller's opening.
/// @param reply The server's answer.
/// @param clientId The id the caller proved.
/// @param callerSide True on the caller, false on the server: which ephemeral key is the PEER's.
/// @return The keys, or nothing when no secret could be agreed -- a low-order peer key fixes the
///         "shared" secret to a value everybody knows, and `X25519SharedSecret` refuses it.
[[nodiscard]] std::optional<NodeSessionKeys> DeriveNodeSessionKeys(SecureByteBuffer const& ownSecret,
                                                                   CompileCacheWire::NodeChallengeRequest const& request,
                                                                   CompileCacheWire::NodeChallengeReply const& reply,
                                                                   std::string_view clientId,
                                                                   bool callerSide);

} // namespace FastCache::Distributed
