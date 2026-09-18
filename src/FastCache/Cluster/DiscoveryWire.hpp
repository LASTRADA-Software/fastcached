// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// The LAN discovery wire: what a node broadcasts about itself, and how a peer proves which
/// identity key it holds before anything it claims is believed (#178).
namespace FastCache::Cluster::DiscoveryWire
{

/// Magic byte, distinct from the compile cache's `0xFC` and Raft's `0xFA`.
///
/// Discovery datagrams land on a broadcast or multicast address that anything on
/// the segment may also be using, so the first byte has to say "not for you" to
/// an unrelated listener as cheaply as possible.
inline constexpr std::byte Magic { 0xFD };

/// Lowest wire version this build still decodes.
///
/// Moved with `CurrentVersion`: a version-1 proof is a MAC under the shared key, which this
/// build has nothing to verify against, so accepting one would be accepting a claim nobody can
/// check.
inline constexpr std::uint8_t MinimumVersion = 2;

/// Wire version this build emits.
///
/// **2 is a GRAMMAR change, and that is why it moved** (#178) -- the opposite of #402, which
/// changed only what the MAC covered and rightly did not. A proof now carries the prover's
/// public key and a 64-byte Ed25519 signature where it carried a 32-byte HMAC, so its arity and
/// its field widths both changed: a version-1 reader would refuse the proof as malformed and
/// report a peer that failed to prove a key it holds. The question is always which of the two
/// changed, never whether a MAC did.
inline constexpr std::uint8_t CurrentVersion = 2;

/// What a datagram is.
///
/// **ORDINALS ARE A WIRE CONTRACT. Append only; never insert or reorder.** (#308) The
/// enumerator's value IS the byte `ClassifyDatagram` switches on, and this is a LAN
/// protocol between builds that upgrade at different times.
enum class Kind : std::uint8_t
{
    Invalid = 0x00,   ///< Never sent; the zero value a default-constructed field would take.
    Beacon = 0x01,    ///< "I am here, this is my cluster, reach me at this endpoint."
    Challenge = 0x02, ///< A nonce a peer must sign to be believed.
    Proof = 0x03,     ///< A signature over the challenge, by the key the proof names.
};

/// A node announcing itself on the segment.
///
/// What it carries is deliberately minimal, and what it does **not** carry is the
/// point: nothing an eavesdropper could replay into a membership change. A beacon is an
/// invitation to *ask*, not a credential -- the challenge that follows it is what proves
/// anything, and only to the node that chose its nonce.
struct Beacon
{
    /// Which cluster this node believes it is in.
    ///
    /// Plain text and not a secret: two unrelated fleets on one segment must be
    /// able to ignore each other, and that is a routing question rather than a
    /// security one. Treating it as a credential is the mistake -- it is on the
    /// wire in every datagram.
    std::string clusterId;

    /// The node's Raft id, which is also how membership names it.
    std::string nodeId;

    /// Where this node answers Raft peer traffic, as `host:port`.
    ///
    /// The whole reason discovery exists: `RaftMembership` names a member by id
    /// and carries no endpoint, so a node the cluster has agreed to admit is
    /// unreachable until something supplies one. This is that something.
    std::string raftEndpoint;
};

/// A nonce the joiner must authenticate.
struct Challenge
{
    std::string clusterId; ///< Which cluster is asking.
    Nonce nonce {};        ///< Fresh from `DrawNonce`; never reused.
};

/// A peer's answer to a Challenge.
///
/// **The key travels, and the signature is what makes it worth reading** (#178): a proof says
/// "the holder of THIS key, answering THIS nonce, is this id at this endpoint". Whether the
/// cluster knows that key is the verifier's question, asked of its roster AFTER the signature
/// verifies -- so a key nobody admitted is reported by name rather than trusted, and a key the
/// roster revoked is recognised for what it is.
struct Proof
{
    std::string nodeId;            ///< Who is answering.
    std::string raftEndpoint;      ///< Where to reach them.
    Ed25519PublicKey publicKey {}; ///< The key they sign with.
    Ed25519Signature signature {}; ///< Over `ProofMessage`, under `publicKey`.
};

/// The label a proof's signature is taken under.
///
/// A label of its own, so a discovery proof can never verify as a Raft handshake signature or
/// the reverse: those are labelled `fastcache-raft-*`, over different fields. `v2` because the
/// `v1` name was the shared key's MAC label (`fastcache-discovery-v1`), retired with it and
/// never reused -- a new construction under an old label would accept whatever the old one
/// signed.
inline constexpr std::string_view ProofSignatureLabel = "fastcache-discovery-proof-v2";

/// The bytes a proof signs: the label and five fields, in this project's length-prefixed
/// field grammar.
///
/// **The prover's identity, its endpoint and its KEY are inside the signature**, not merely
/// alongside it. Signing the nonce alone would let anyone who observed one valid proof replay it
/// with a different endpoint -- admitting a known id at an attacker's address, which is object
/// injection into every build the fleet serves -- and leaving the key out would let a proof be
/// re-attributed to any key the signature happened to verify under. Length-prefixed for the
/// reason the object key's fields are: a separator that can occur inside a value is not a
/// framing, so `{node="a", endpoint="b:1"}` and `{node="a:b", endpoint="1"}` would otherwise
/// sign identically.
///
/// One function for both ends, for `LeaseToken::PackClaims`' reason: a signer and a verifier
/// that each spell this list are a signer and a verifier that will one day spell it
/// differently, which presents as every node on the segment failing to prove the key it holds.
/// @param challenge What was asked.
/// @param nodeId Who is answering.
/// @param raftEndpoint Where they will answer Raft traffic.
/// @param publicKey The key they sign with.
/// @return The message, owned.
[[nodiscard]] inline std::vector<std::byte> ProofMessage(Challenge const& challenge,
                                                         std::string_view nodeId,
                                                         std::string_view raftEndpoint,
                                                         Ed25519PublicKey const& publicKey)
{
    return WireFields::Encode({ WireFields::AsBytes(ProofSignatureLabel),
                                WireFields::AsBytes(challenge.clusterId),
                                std::span<std::byte const> { challenge.nonce },
                                WireFields::AsBytes(nodeId),
                                WireFields::AsBytes(raftEndpoint),
                                std::span<std::byte const> { publicKey } });
}

/// Whether @p proof is a signature over @p challenge by the key it names.
///
/// Proof of POSSESSION and nothing more: it says the sender holds the private half of
/// `proof.publicKey`. Whether that key is anybody the cluster knows is the roster's question,
/// and asking it first would be reporting on a claim nothing has proved.
/// @param challenge What this node asked.
/// @param proof What came back.
/// @return True only when the signature verifies.
[[nodiscard]] inline bool VerifyProofSignature(Challenge const& challenge, Proof const& proof)
{
    auto const message = ProofMessage(challenge, proof.nodeId, proof.raftEndpoint, proof.publicKey);
    return Ed25519Verify(proof.publicKey, message, proof.signature);
}

/// Wrap an already-encoded payload in this protocol's frame header.
///
/// Shared by the three encoders because they differ only in their `Kind` and
/// their fields -- and because the length cast belongs in one place. `WireFields`
/// already refuses a payload above `MaxPayload` (2^32-1), so the narrowing here
/// cannot lose information; it is spelled out rather than implicit so a reader
/// can see that, and so `-Wshorten-64-to-32` is satisfied by an assertion instead
/// of a silence.
/// @param kind What this datagram is.
/// @param payload The already-encoded fields.
/// @return The complete datagram.
[[nodiscard]] inline std::vector<std::byte> Frame(Kind kind, std::span<std::byte const> payload)
{
    // The header is built in a fixed-size array and then appended, rather than
    // written into an already-sized vector. Both produce the same bytes, but GCC
    // cannot prove a heap buffer's storage is non-null once this is inlined, and
    // reports `-Wnull-dereference` on every `out[i]` inside `PutHeader` -- which
    // under this project's `-Werror` is a build failure rather than a remark. An
    // array's storage is provably there, so the question does not arise.
    std::array<std::byte, WireFrame::HeaderSize> header {};
    WireFrame::PutHeader(
        header, Magic, CurrentVersion, static_cast<std::uint8_t>(kind), static_cast<std::uint32_t>(payload.size()));

    std::vector<std::byte> out;
    out.reserve(header.size() + payload.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// Encode a beacon as a complete datagram.
/// @param beacon What to announce.
/// @return The bytes to send.
[[nodiscard]] inline std::vector<std::byte> EncodeBeacon(Beacon const& beacon)
{
    auto const payload = WireFields::Encode({
        WireFields::AsBytes(beacon.clusterId),
        WireFields::AsBytes(beacon.nodeId),
        WireFields::AsBytes(beacon.raftEndpoint),
    });

    return Frame(Kind::Beacon, payload);
}

/// Encode a challenge as a complete datagram.
/// @param challenge What to ask.
/// @return The bytes to send.
[[nodiscard]] inline std::vector<std::byte> EncodeChallenge(Challenge const& challenge)
{
    auto const payload = WireFields::Encode({
        WireFields::AsBytes(challenge.clusterId),
        std::span<std::byte const> { challenge.nonce },
    });

    return Frame(Kind::Challenge, payload);
}

/// Encode a proof as a complete datagram.
/// @param proof What to answer with.
/// @return The bytes to send.
[[nodiscard]] inline std::vector<std::byte> EncodeProof(Proof const& proof)
{
    auto const payload = WireFields::Encode({
        WireFields::AsBytes(proof.nodeId),
        WireFields::AsBytes(proof.raftEndpoint),
        std::span<std::byte const> { proof.publicKey },
        std::span<std::byte const> { proof.signature },
    });

    return Frame(Kind::Proof, payload);
}

/// What kind a datagram is, when it is one of ours at all.
///
/// Answered before the payload is looked at, so an unrelated broadcast on the
/// segment costs a magic-byte comparison rather than a parse. An unknown *kind*
/// is reported as such rather than refused outright: the framing exists so a
/// receiver can step over what it does not know, and a future kind must not make
/// an older node treat the whole datagram as corrupt.
/// @param datagram The bytes as they arrived.
/// @return The kind, or nullopt when this is not a discovery datagram this build
///         can read.
[[nodiscard]] inline std::optional<Kind> ClassifyDatagram(std::span<std::byte const> datagram)
{
    auto const header = WireFrame::DecodeHeader(datagram, Magic);
    if (!header.has_value())
        return std::nullopt;
    if (!WireFrame::IsSupported(header->version, MinimumVersion, CurrentVersion))
        return std::nullopt;

    // The declared length must match what actually arrived. A datagram is
    // all-or-nothing at the kernel, so a mismatch is a malformed sender rather
    // than a short read, and continuing would parse whatever followed.
    if (WireFrame::HeaderSize + header->payloadLength != datagram.size())
        return std::nullopt;

    switch (static_cast<Kind>(header->kindRaw))
    {
        case Kind::Beacon:
            return Kind::Beacon;
        case Kind::Challenge:
            return Kind::Challenge;
        case Kind::Proof:
            return Kind::Proof;
        case Kind::Invalid:
            break;
    }
    return std::nullopt;
}

/// Decode a beacon datagram.
/// @param datagram The bytes as they arrived.
/// @return The beacon, or nullopt when it is not a well-formed one.
[[nodiscard]] inline std::optional<Beacon> DecodeBeacon(std::span<std::byte const> datagram)
{
    if (ClassifyDatagram(datagram) != Kind::Beacon)
        return std::nullopt;

    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), 3);
    if (!fields.has_value())
        return std::nullopt;

    return Beacon { .clusterId = std::string { WireFields::AsStringView((*fields)[0]) },
                    .nodeId = std::string { WireFields::AsStringView((*fields)[1]) },
                    .raftEndpoint = std::string { WireFields::AsStringView((*fields)[2]) } };
}

/// Decode a challenge datagram.
/// @param datagram The bytes as they arrived.
/// @return The challenge, or nullopt when it is not a well-formed one.
[[nodiscard]] inline std::optional<Challenge> DecodeChallenge(std::span<std::byte const> datagram)
{
    if (ClassifyDatagram(datagram) != Kind::Challenge)
        return std::nullopt;

    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), 2);
    if (!fields.has_value())
        return std::nullopt;

    auto const& nonce = (*fields)[1];
    if (nonce.size() != std::tuple_size_v<decltype(Challenge::nonce)>)
        return std::nullopt;

    Challenge out { .clusterId = std::string { WireFields::AsStringView((*fields)[0]) }, .nonce = {} };
    std::ranges::copy(nonce, out.nonce.begin());
    return out;
}

/// Decode a proof datagram.
/// @param datagram The bytes as they arrived.
/// @return The proof, or nullopt when it is not a well-formed one.
[[nodiscard]] inline std::optional<Proof> DecodeProof(std::span<std::byte const> datagram)
{
    if (ClassifyDatagram(datagram) != Kind::Proof)
        return std::nullopt;

    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), 4);
    if (!fields.has_value())
        return std::nullopt;

    // Exactly one key and exactly one signature wide: a prefix of a key is a different key, and
    // a truncated signature verifies as nothing -- refused here so the verifier is never asked.
    auto const& key = (*fields)[2];
    auto const& signature = (*fields)[3];
    if (key.size() != Ed25519PublicKeyBytes || signature.size() != Ed25519SignatureBytes)
        return std::nullopt;

    Proof out { .nodeId = std::string { WireFields::AsStringView((*fields)[0]) },
                .raftEndpoint = std::string { WireFields::AsStringView((*fields)[1]) },
                .publicKey = {},
                .signature = {} };
    std::ranges::copy(key, out.publicKey.begin());
    std::ranges::copy(signature, out.signature.begin());
    return out;
}

} // namespace FastCache::Cluster::DiscoveryWire
