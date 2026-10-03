// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

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

/// The LAN discovery wire: what a node broadcasts about itself and its fleet, and how a peer proves
/// which identity key it holds before anything it claims is believed (#178).
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
///
/// **The beacon and the proof below are a GRAMMAR change again, and this value has not moved
/// with them yet.** Both now carry the node's fleet summary as one nested field where they
/// carried an id and an endpoint -- a beacon is one field where it was three, a proof three where
/// it was four -- and the proof signs that summary under a new label, so the answer to "which of
/// the two changed" is both. The move is to 3, with `MinimumVersion` beside it. The challenge's
/// grammar did not change and is still decoded, but a version-2 challenge is useless on its own:
/// it answers a version-2 beacon and is answered by a version-2 proof, and this build reads
/// neither. Until the value moves, a reader of the old grammar refuses these datagrams by ARITY
/// rather than by version, and either service drops a datagram it cannot decode as `Ignored`,
/// with no counter and no log line -- so the two builds fail closed and never misread, but
/// silently never see each other on a shared segment.
///
/// **The same unmoved change pads the beacon and the challenge**, each by one trailing field,
/// so the challenge's grammar changed after all: a beacon is two fields and a challenge three.
/// It rides the one move to 3 rather than taking a version of its own -- neither grammar has
/// shipped in between. See `EncodeBeacon` and `EncodeChallenge` for why.
///
/// **And the proof ECHOES the nonce it answers**, one more field -- four where it was three --
/// because a challenger keeps no table of what it asked: the nonce is a cookie it recomputes from
/// what the proof claims (`ChallengeCookies`), so the proof has to carry it back. The same one
/// move to 3 carries that too.
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

/// How many fields each kind's payload is: a beacon's summary and padding; a challenge's cluster,
/// nonce and padding; a proof's summary, the nonce it answers, its key and its signature.
inline constexpr std::size_t BeaconFieldCount = 2;
inline constexpr std::size_t ChallengeFieldCount = 3;
inline constexpr std::size_t ProofFieldCount = 4;

/// A node announcing itself and its fleet on the segment.
///
/// What it does **not** carry is the point: nothing an eavesdropper could replay into a
/// membership change. A beacon is an invitation to *ask*, not a credential -- every field in it
/// is a HINT until a proof signs the same summary, and then only to the node that chose that
/// proof's nonce. Anything decided on a fleet summary, a yield above all, is decided on a proof's.
struct Beacon
{
    /// What the node says: which fleet it is in, whether that fleet is established, when it was
    /// created, where its leader takes enrollment, and the node's own id and Raft endpoint.
    ///
    /// Plain text and not a secret. The cluster id is how two unrelated fleets on one segment
    /// tell each other apart, which is a routing question rather than a security one; the id and
    /// the endpoint are what `RaftMembership` needs and does not carry, since it names a member by
    /// id alone. The same codec as the `0xFC` port's `FleetSummary` reply, so a seed and a beacon
    /// cannot describe one node in two grammars.
    CompileCacheWire::FleetSummary summary;
};

/// A nonce the joiner must authenticate.
struct Challenge
{
    std::string clusterId; ///< The CHALLENGER's cluster: routing only, and signed so an answer names its asker.
    Nonce nonce {};        ///< A cookie the challenger can recompute (`ChallengeCookies`); never reused.
};

/// A peer's answer to a Challenge.
///
/// **The key travels, and the signature is what makes it worth reading** (#178): a proof says
/// "the holder of THIS key, answering THIS nonce, says THIS about itself and its fleet". Whether
/// the cluster knows that key is the verifier's question, asked of its roster AFTER the signature
/// verifies -- so a key nobody admitted is reported by name rather than trusted, and a key the
/// roster revoked is recognised for what it is.
struct Proof
{
    CompileCacheWire::FleetSummary summary; ///< What the prover says, every field of it signed.
    Nonce answers {};                       ///< The challenge's nonce, echoed; signed, as the challenge's.
    Ed25519PublicKey publicKey {};          ///< The key they sign with.
    Ed25519Signature signature {};          ///< Over `ProofMessage`, under `publicKey`.
};

/// The bytes a proof signs: its label (`IdentityKeyPurpose::DiscoveryProof`), the challenger's
/// cluster, the nonce, the prover's whole fleet summary as ONE nested field, and the key, in this
/// project's length-prefixed field grammar.
///
/// **Every summary field and the KEY are inside the signature**, not merely alongside it. The
/// summary is what formation decides on -- whether to yield, to whom, where to enroll -- so a
/// field outside the signature is one a relay can rewrite into a yield. Signing the nonce alone
/// would let anyone who observed one valid proof replay it with a different endpoint, admitting
/// a known id at an attacker's address, which is object injection into every build the fleet
/// serves; leaving the key out would let a proof be re-attributed to any key the signature
/// happened to verify under. Length-prefixed for the reason the object key's fields are: a
/// separator that can occur inside a value is not a framing, so `{node="a", endpoint="b:1"}` and
/// `{node="a:b", endpoint="1"}` would otherwise sign identically. The summary's bytes are
/// `EncodeFleetSummaryFields`', exactly what the beacon carries.
///
/// One function for both ends, for `LeaseToken::PackClaims`' reason: a signer and a verifier
/// that each spell this list are a signer and a verifier that will one day spell it
/// differently, which presents as every node on the segment failing to prove the key it holds.
/// @param challenge What was asked.
/// @param summary What the prover says about itself and its fleet.
/// @param publicKey The key they sign with.
/// @return The message, owned.
[[nodiscard]] inline LabelledMessage ProofMessage(Challenge const& challenge,
                                                  CompileCacheWire::FleetSummary const& summary,
                                                  Ed25519PublicKey const& publicKey)
{
    auto const fields = CompileCacheWire::EncodeFleetSummaryFields(summary);
    return LabelledMessage::Of(IdentityKeyPurpose::DiscoveryProof,
                               { WireFields::AsBytes(challenge.clusterId),
                                 std::span<std::byte const> { challenge.nonce },
                                 std::span<std::byte const> { fields },
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
    return VerifyLabelled(proof.publicKey, ProofMessage(challenge, proof.summary, proof.publicKey), proof.signature);
}

/// Whether a reply of @p reply bytes may answer a datagram of @p request bytes: never larger.
///
/// Discovery answers whatever address a datagram came FROM, and a source address on a datagram
/// is whatever its sender typed. So a reply larger than its request is an AMPLIFIER aimed at a
/// third party for the price of the smaller datagram, and every reply this protocol sends is
/// bounded by the one that provoked it: a challenge by the beacon, a proof by the challenge. A
/// reflector at 1:1 is left, which a spoofed source always buys; an amplifier is not.
/// @param reply The reply's size on the wire.
/// @param request The size of the datagram it answers.
/// @return True when the reply is no larger.
[[nodiscard]] constexpr bool AnswerFits(std::size_t reply, std::size_t request) noexcept
{
    return reply <= request;
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

/// Encode a proof as a complete datagram: the summary nested as one field, then the nonce it
/// answers, the key and the signature.
///
/// The grammar alone, unbounded: a node answers a challenge only when `AnswerFits` says the proof
/// is no larger than it, which `DiscoveryService` asks BEFORE signing -- see `ProofDatagramSize`.
/// @param proof What to answer with.
/// @return The bytes to send.
[[nodiscard]] inline std::vector<std::byte> EncodeProof(Proof const& proof)
{
    auto const fields = CompileCacheWire::EncodeFleetSummaryFields(proof.summary);
    return Frame(Kind::Proof,
                 WireFields::Encode({ std::span<std::byte const> { fields },
                                      std::span<std::byte const> { proof.answers },
                                      std::span<std::byte const> { proof.publicKey },
                                      std::span<std::byte const> { proof.signature } }));
}

/// The smallest proof a node of this build can send, and so the smallest honest beacon: a summary
/// naming a one-byte cluster and nothing else, then the nonce, the key and the signature.
/// @return Its datagram's size in bytes.
[[nodiscard]] consteval std::size_t SmallestProofDatagram() noexcept
{
    auto const summary = (CompileCacheWire::FleetSummaryFieldCount * WireFields::FieldPrefixSize) + 1 /* cluster */
                         + 1 /* state */ + sizeof(std::uint64_t) /* created */;
    return WireFrame::HeaderSize + (ProofFieldCount * WireFields::FieldPrefixSize) + summary + NonceBytes
           + Ed25519PublicKeyBytes + Ed25519SignatureBytes;
}

/// The largest challenge a node of this build sends before it is padded: a cluster id at
/// `CompileCacheWire::MaxIdBytes`, the nonce, and an empty padding field.
/// @return Its datagram's size in bytes.
[[nodiscard]] consteval std::size_t LargestUnpaddedChallenge() noexcept
{
    return WireFrame::HeaderSize + (ChallengeFieldCount * WireFields::FieldPrefixSize) + CompileCacheWire::MaxIdBytes
           + NonceBytes;
}

// An honest beacon is always challenged: no cluster id this build accepts makes a challenge larger
// than the smallest beacon a node of this build sends. Without the bound, a long enough id was a
// node that challenged nobody, in silence, since `EncodeChallenge` refuses to amplify.
static_assert(LargestUnpaddedChallenge() <= SmallestProofDatagram(),
              "a challenge naming the longest cluster id must fit the smallest honest beacon");

/// How large a proof carrying @p summary is on the wire.
///
/// Known before anything is signed, because the nonce, the key and the signature are fixed width: so a node
/// can refuse to answer a challenge too small for its proof without spending a signature on it.
/// @param summary What the proof would say.
/// @return Its datagram's size in bytes.
[[nodiscard]] inline std::size_t ProofDatagramSize(CompileCacheWire::FleetSummary const& summary)
{
    return EncodeProof(Proof { .summary = summary, .answers = {}, .publicKey = {}, .signature = {} }).size();
}

/// Encode a beacon as a complete datagram: the summary's own fields nested as one field, then
/// padding.
///
/// **Padded to exactly the size of the proof its sender answers with**, because that is what lets
/// neither reply amplify (`AnswerFits`). A challenger pads its challenge to the beacon it answers,
/// so the challenge is no larger than the beacon; and the challenged node's proof carries the
/// summary its beacon did, so it is no larger than the challenge. Unpadded, one of the two would
/// have to be larger than what provoked it. The padding is zeroes and is never read.
/// @param beacon What to announce.
/// @return The bytes to send.
[[nodiscard]] inline std::vector<std::byte> EncodeBeacon(Beacon const& beacon)
{
    auto const fields = CompileCacheWire::EncodeFleetSummaryFields(beacon.summary);
    auto const unpadded =
        WireFrame::HeaderSize
        + WireFields::Encode({ std::span<std::byte const> { fields }, std::span<std::byte const> {} }).size();
    auto const padding = std::vector<std::byte>(ProofDatagramSize(beacon.summary) - unpadded);
    return Frame(Kind::Beacon,
                 WireFields::Encode({ std::span<std::byte const> { fields }, std::span<std::byte const> { padding } }));
}

/// Encode a challenge as a complete datagram, padded to EXACTLY the size of the beacon it answers.
///
/// Exactly rather than at least, for `AnswerFits`' reason in both directions: no larger, so a
/// spoofed beacon buys no more than its own size aimed at a third party; and no smaller, so the
/// proof the beacon's sender answers with -- as large as its beacon, see `EncodeBeacon` -- fits.
/// The padding is zeroes and is never read.
/// @param challenge What to ask.
/// @param answering The size of the beacon datagram this challenge answers.
/// @return The bytes to send, or nullopt when even an unpadded challenge would be larger than
///         @p answering, which is not sent.
[[nodiscard]] inline std::optional<std::vector<std::byte>> EncodeChallenge(Challenge const& challenge, std::size_t answering)
{
    auto const fields = [&challenge](std::span<std::byte const> padding) {
        return WireFields::Encode(
            { WireFields::AsBytes(challenge.clusterId), std::span<std::byte const> { challenge.nonce }, padding });
    };
    auto const unpadded = WireFrame::HeaderSize + fields({}).size();
    if (!AnswerFits(unpadded, answering))
        return std::nullopt;
    auto const padding = std::vector<std::byte>(answering - unpadded);
    return Frame(Kind::Challenge, fields(padding));
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
///
/// The summary is read by its own codec and nothing else, so a state byte this build has no name
/// for refuses the beacon rather than reading as whatever a default says.
/// @param datagram The bytes as they arrived.
/// @return The beacon, owning every field, or nullopt when it is not a well-formed one.
[[nodiscard]] inline std::optional<Beacon> DecodeBeacon(std::span<std::byte const> datagram)
{
    if (ClassifyDatagram(datagram) != Kind::Beacon)
        return std::nullopt;

    // The second field is `EncodeBeacon`'s padding, whatever it holds.
    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), BeaconFieldCount);
    if (!fields.has_value())
        return std::nullopt;

    auto summary = CompileCacheWire::DecodeFleetSummaryFields((*fields)[0]);
    if (!summary.has_value())
        return std::nullopt;
    return Beacon { .summary = *std::move(summary) };
}

/// Decode a challenge datagram.
/// @param datagram The bytes as they arrived.
/// @return The challenge, or nullopt when it is not a well-formed one.
[[nodiscard]] inline std::optional<Challenge> DecodeChallenge(std::span<std::byte const> datagram)
{
    if (ClassifyDatagram(datagram) != Kind::Challenge)
        return std::nullopt;

    // The third field is `EncodeChallenge`'s padding, whatever it holds.
    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), ChallengeFieldCount);
    if (!fields.has_value())
        return std::nullopt;

    // The challenger's cluster within the bound every summary's is held to, so what this node
    // signs its proof over is never larger than an honest challenge can be.
    auto const& nonce = (*fields)[1];
    if ((*fields)[0].size() > CompileCacheWire::MaxIdBytes || nonce.size() != std::tuple_size_v<decltype(Challenge::nonce)>)
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

    auto const fields = WireFields::SplitExactly(datagram.subspan(WireFrame::HeaderSize), ProofFieldCount);
    if (!fields.has_value())
        return std::nullopt;

    // Exactly one nonce, one key and one signature wide: a prefix of a key is a different key, and
    // a truncated signature verifies as nothing -- refused here so the verifier is never asked.
    auto const& answers = (*fields)[1];
    auto const& key = (*fields)[2];
    auto const& signature = (*fields)[3];
    if (answers.size() != NonceBytes || key.size() != Ed25519PublicKeyBytes || signature.size() != Ed25519SignatureBytes)
        return std::nullopt;

    auto summary = CompileCacheWire::DecodeFleetSummaryFields((*fields)[0]);
    if (!summary.has_value())
        return std::nullopt;

    Proof out { .summary = *std::move(summary), .answers = {}, .publicKey = {}, .signature = {} };
    std::ranges::copy(answers, out.answers.begin());
    std::ranges::copy(key, out.publicKey.begin());
    std::ranges::copy(signature, out.signature.begin());
    return out;
}

} // namespace FastCache::Cluster::DiscoveryWire
