// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Core/Sha256.hpp>
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

/// The LAN discovery wire: what a node broadcasts about itself, and how a peer
/// proves it holds the cluster's pre-shared key before it is admitted.
namespace FastCache::Cluster::DiscoveryWire
{

/// Magic byte, distinct from the compile cache's `0xFC` and Raft's `0xFA`.
///
/// Discovery datagrams land on a broadcast or multicast address that anything on
/// the segment may also be using, so the first byte has to say "not for you" to
/// an unrelated listener as cheaply as possible.
inline constexpr std::byte Magic { 0xFD };

/// Lowest wire version this build still decodes.
inline constexpr std::uint8_t MinimumVersion = 1;

/// Wire version this build emits.
inline constexpr std::uint8_t CurrentVersion = 1;

/// What a datagram is.
enum class Kind : std::uint8_t
{
    Invalid = 0x00,   ///< Never sent; the zero value a default-constructed field would take.
    Beacon = 0x01,    ///< "I am here, this is my cluster, reach me at this endpoint."
    Challenge = 0x02, ///< A nonce a joiner must authenticate to be admitted.
    Proof = 0x03,     ///< HMAC over the challenge, proving possession of the key.
};

/// A node announcing itself on the segment.
///
/// What it carries is deliberately minimal, and what it does **not** carry is the
/// point: no key, no key hash, and nothing an eavesdropper could replay into a
/// membership change. A beacon is an invitation to *ask*, not a credential --
/// the plan's rule that the pre-shared key authenticates a handshake rather than
/// travelling in beacons.
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
    std::string clusterId;              ///< Which cluster is asking.
    std::array<std::byte, 32> nonce {}; ///< Fresh random bytes; never reused.
};

/// The joiner's answer to a Challenge.
struct Proof
{
    std::string nodeId;       ///< Who is answering.
    std::string raftEndpoint; ///< Where to reach them once admitted.
    Sha256::Digest tag {};    ///< HMAC over the challenge; see `ExpectedProofTag`.
};

namespace Detail
{

    /// The four fields a proof authenticates, in wire order.
    ///
    /// One function rather than one per direction, for `LeaseToken::PackClaims`'
    /// reason: a signer and a verifier that each spell this list are a signer and a
    /// verifier that will one day spell it differently, which presents as every
    /// node on the segment failing to prove a key they all hold.
    ///
    /// `Detail`, because the returned spans BORROW from @p challenge, @p nodeId and
    /// @p raftEndpoint -- this is the shape `.agent/rules/wire-and-protocol.md`
    /// names as having already been a use-after-free twice, and the mitigation here
    /// is that the only two callers are the two functions directly below, each of
    /// which consumes the result inside the full expression that built it.
    /// @param challenge What was asked.
    /// @param nodeId Who is answering.
    /// @param raftEndpoint Where they will answer Raft traffic.
    /// @return The fields, borrowing from the arguments.
    [[nodiscard]] inline std::array<std::span<std::byte const>, 4> ProofFields(Challenge const& challenge,
                                                                               std::string_view nodeId,
                                                                               std::string_view raftEndpoint)
    {
        return {
            WireFields::AsBytes(challenge.clusterId),
            std::span<std::byte const> { challenge.nonce },
            WireFields::AsBytes(nodeId),
            WireFields::AsBytes(raftEndpoint),
        };
    }

} // namespace Detail

/// The tag a holder of @p key must produce for @p challenge.
///
/// The joiner's identity and endpoint are inside the MAC, not merely alongside
/// it. Authenticating the nonce alone would let anyone who observed one valid
/// proof replay its tag with a *different* endpoint substituted -- admitting a
/// legitimate node id at an attacker's address, which is object injection into
/// every build the fleet serves. The fields are length-prefixed for the reason
/// the object key's are: a separator that can occur inside a value is not a
/// framing, so `{node="a", endpoint="b:1"}` and `{node="a:b", endpoint="1"}`
/// would otherwise authenticate identically.
///
/// Signed through `Cluster::SignFields`, which folds
/// `SigningDomain::DiscoveryProof`'s label in ahead of these four fields. The
/// same pre-shared key MACs lease tokens, and before #402 a proof carried no
/// label at all -- the two constructions were kept apart only by happening to
/// have different field arities, which is a coincidence and not a property.
/// @param key The cluster's pre-shared key.
/// @param challenge What was asked.
/// @param nodeId Who is answering.
/// @param raftEndpoint Where they will answer Raft traffic.
/// @return The expected tag.
[[nodiscard]] inline Sha256::Digest ExpectedProofTag(std::span<std::byte const> key,
                                                     Challenge const& challenge,
                                                     std::string_view nodeId,
                                                     std::string_view raftEndpoint)
{
    return SignFields(key, SigningDomain::DiscoveryProof, Detail::ProofFields(challenge, nodeId, raftEndpoint));
}

/// Whether @p presented is the tag a holder of @p key would have produced.
///
/// The verifier's half, and it exists so that no caller has to hold a tag and
/// choose a comparison for it. `Cluster::VerifyFields` compares in constant time;
/// `==` and `std::ranges::equal` stop at the first difference, and a peer that can
/// retry -- which anything on the segment can, by sending another beacon -- would
/// learn a tag one byte at a time from the timing.
///
/// `ExpectedProofTag` stays public because SIGNING is a separate act: a node
/// answering a challenge has to produce a tag, and the tests have to produce one
/// to send. What this removes is the reason for a *verifier* to call it.
/// @param key The cluster's pre-shared key.
/// @param challenge What was asked.
/// @param nodeId Who is answering.
/// @param raftEndpoint Where they claim they will answer Raft traffic.
/// @param presented The tag the proof carried.
/// @return True when it authenticates.
[[nodiscard]] inline bool VerifyProofTag(std::span<std::byte const> key,
                                         Challenge const& challenge,
                                         std::string_view nodeId,
                                         std::string_view raftEndpoint,
                                         Sha256::Digest const& presented)
{
    return VerifyFields(key, SigningDomain::DiscoveryProof, Detail::ProofFields(challenge, nodeId, raftEndpoint), presented);
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
        std::span<std::byte const> { proof.tag },
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

    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), 3);
    if (!fields.has_value())
        return std::nullopt;

    auto const& tag = (*fields)[2];
    if (tag.size() != Sha256::DigestSize)
        return std::nullopt;

    Proof out { .nodeId = std::string { WireFields::AsStringView((*fields)[0]) },
                .raftEndpoint = std::string { WireFields::AsStringView((*fields)[1]) },
                .tag = {} };
    std::ranges::copy(tag, out.tag.begin());
    return out;
}

} // namespace FastCache::Cluster::DiscoveryWire
