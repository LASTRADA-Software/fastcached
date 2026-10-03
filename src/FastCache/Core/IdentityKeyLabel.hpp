// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

/// Every construction one node identity key signs (#178).
///
/// **PRIVATE: persisted and transmitted nowhere** -- what travels is the LABEL its row names, as
/// the first field of what is signed, and the enumerator only selects it. So the ordinals carry no
/// explicit values and an insertion shifts nothing anything outside this process has seen.
///
/// ONE enum for every construction rather than one per construction, because the question a label
/// answers is asked ACROSS them: one key signs a node's discovery proofs, its fleet summaries, its
/// leases, both halves of its Raft handshake, both halves of its node proof, its machine tickets, the
/// admissions it answers and the enrollment it asks for, so a label is the only thing keeping a
/// signature made for one from verifying as another.
enum class IdentityKeyPurpose : std::uint8_t
{
    DiscoveryProof,      ///< A discovery proof: the answer to a challenge this node was sent.
    FleetSummary,        ///< A FLEET-SUMMARY answer, over a nonce a STRANGER chose.
    Lease,               ///< A scheduler's grant.
    RaftDiallerProof,    ///< A Raft peer dialler's proof.
    RaftAcceptorVerdict, ///< A Raft peer acceptor's verdict on that proof.
    NodeServerChallenge, ///< A node-proof server's challenge.
    NodeProof,           ///< A node-proof caller's proof.
    MachineTicket,       ///< A machine ticket: this machine speaks, to ONE audience, for a minute.
    EnrollAdmission,     ///< An ENROLL answer to a joiner, of any outcome, over a nonce the JOINER chose.
    EnrollRequest,       ///< A joiner's ENROLL request, over every field it states, under the key it asks with.
    Last,                ///< Not a construction, and has no row: the length of a table keyed by one.
};

/// What one construction is labelled with.
struct IdentityKeyLabel
{
    IdentityKeyPurpose purpose; ///< The construction this row describes.
    std::string_view label;     ///< The bytes signed as the message's FIRST field.
};

/// One row per `IdentityKeyPurpose`, in enumerator order: the ONE place a label is spelled.
///
/// Each is versioned because it is signed, so changing one retires every signature made under it,
/// which is a stated act -- the old label joins `RetiredIdentityKeyLabels`. Why each is at the
/// version it is:
/// - `discovery-proof-v3`: the fields it covers became a whole fleet summary where an id and an
///   endpoint were, and a label names ONE construction.
/// - `fleet-summary-v1`: new with its construction.
/// - `lease-v3`: versioned with the token, so a signature over a version-2 claim list never
///   verifies as a version-3 one under the same key.
/// - `raft-proof-v3`, `raft-verdict-v3`: the transcript gained the session direction, and a
///   transcript one field longer is another construction whatever its arity, since a
///   length-prefixed grammar separates fields and not field LISTS.
/// - `node-challenge-v2`, `node-proof-v2`: `node-proof-v1` was the pre-shared key's MAC label.
/// - `ticket-v1`: new with its construction (spec §5), and the label tickets were first signed
///   under, so routing them through this table moved no byte on the wire.
/// - `enroll-admission-v1`, `enroll-request-v1`: new with their constructions.
inline constexpr EnumTable<IdentityKeyPurpose, IdentityKeyLabel> IdentityKeyLabels { {
    { .purpose = IdentityKeyPurpose::DiscoveryProof, .label = "fastcache-discovery-proof-v3" },
    { .purpose = IdentityKeyPurpose::FleetSummary, .label = "fastcache-fleet-summary-v1" },
    { .purpose = IdentityKeyPurpose::Lease, .label = "fastcache-lease-v3" },
    { .purpose = IdentityKeyPurpose::RaftDiallerProof, .label = "fastcache-raft-proof-v3" },
    { .purpose = IdentityKeyPurpose::RaftAcceptorVerdict, .label = "fastcache-raft-verdict-v3" },
    { .purpose = IdentityKeyPurpose::NodeServerChallenge, .label = "fastcache-node-challenge-v2" },
    { .purpose = IdentityKeyPurpose::NodeProof, .label = "fastcache-node-proof-v2" },
    { .purpose = IdentityKeyPurpose::MachineTicket, .label = "fastcache-ticket-v1" },
    { .purpose = IdentityKeyPurpose::EnrollAdmission, .label = "fastcache-enroll-admission-v1" },
    { .purpose = IdentityKeyPurpose::EnrollRequest, .label = "fastcache-enroll-request-v1" },
} };

static_assert(RowsInEnumeratorOrder(IdentityKeyLabels, &IdentityKeyLabel::purpose),
              "IdentityKeyLabels must hold one row per IdentityKeyPurpose, in enumerator order");

/// Every label a construction under a member's key was once signed or MACed under: retired, and
/// never reused, since a new construction under an old label would accept whatever the old one
/// signed. So a label that enters this list never leaves it.
///
/// The `-v1` discovery, lease, Raft and node-proof labels were the pre-shared key's MAC labels
/// (#1308, #178); `discovery-proof-v2` signed an id and an endpoint rather than a summary; the Raft
/// `-v2` pair signed a transcript with no session direction; `roster-endorsement-v1` signed a
/// voter's endorsement of a certified roster, which no node uses any more.
inline constexpr std::array<std::string_view, 9> RetiredIdentityKeyLabels {
    "fastcache-discovery-v1",    "fastcache-discovery-proof-v2", "fastcache-lease-v1",
    "fastcache-raft-proof-v1",   "fastcache-raft-verdict-v1",    "fastcache-raft-proof-v2",
    "fastcache-raft-verdict-v2", "fastcache-node-proof-v1",      "fastcache-roster-endorsement-v1",
};

/// Whether every label is present, no two are the same, and none is retired.
///
/// An empty label separates nothing; a copied row -- a new purpose added by duplicating the line
/// above it -- would make one construction's signature verify as the other's; and a retired label
/// would make a live signature one an old build's MAC or signature could be taken for. A BUILD
/// failure rather than a test, because the table is data the compiler can read whole.
/// @return True when the labels separate every construction from every other, past and present.
[[nodiscard]] consteval bool IdentityKeyLabelsSeparate() noexcept
{
    return std::ranges::all_of(IdentityKeyLabels, [](IdentityKeyLabel const& row) {
        return !row.label.empty() && std::ranges::count(IdentityKeyLabels, row.label, &IdentityKeyLabel::label) == 1
               && !std::ranges::contains(RetiredIdentityKeyLabels, row.label);
    });
}

static_assert(IdentityKeyLabelsSeparate(),
              "each identity-key construction needs a label of its own, and never a retired one");

/// The label @p purpose is signed under.
/// @param purpose The construction.
/// @return Its label.
[[nodiscard]] constexpr std::string_view LabelOf(IdentityKeyPurpose purpose) noexcept
{
    return IdentityKeyLabels[static_cast<std::size_t>(purpose)].label;
}

/// Whether a protocol's own table of signatures names a different construction on every row.
///
/// A handshake that signs twice -- the Raft peer's proof and verdict, the node proof's challenge
/// and proof -- keeps an enum of its own so its interface cannot be handed another protocol's
/// purpose, and maps each value to an `IdentityKeyPurpose`. That mapping is where a copied row
/// would make its two signatures one construction, so it is asserted at the table.
/// @param table The protocol's table.
/// @param construction The member naming each row's construction.
/// @return True when no two rows name the same construction.
template <typename Row, std::size_t N>
[[nodiscard]] consteval bool EachRowItsOwnConstruction(std::array<Row, N> const& table,
                                                       IdentityKeyPurpose Row::* construction) noexcept
{
    return std::ranges::all_of(table, [&table, construction](Row const& row) {
        return std::ranges::count(table, row.*construction, construction) == 1;
    });
}

/// A message an identity key may sign: its construction's label as the FIRST field, then the
/// construction's own fields, in this project's length-prefixed field grammar.
///
/// **The label is first BY CONSTRUCTION**: the only way to make one is `Of`, which takes the
/// purpose rather than the label and writes the label itself, so a builder cannot drop it, move
/// it or spell it. What an identity key signs is this type and nothing else -- `SignLabelled`,
/// `ILeaseSigner::Sign` and `IRaftPeerKeys::SignAsSelf` take it --
/// which is what makes "every construction carries its label first" a property of the types
/// rather than of each builder remembering to.
class LabelledMessage
{
  public:
    /// The message for @p purpose over @p fields.
    /// @param purpose Which construction; selects the label.
    /// @param fields The construction's fields after the label, in wire order.
    /// @return The message.
    [[nodiscard]] static LabelledMessage Of(IdentityKeyPurpose purpose, WireFields::FieldList fields)
    {
        std::vector<std::span<std::byte const>> labelled;
        labelled.reserve(fields.size() + 1);
        labelled.push_back(WireFields::AsBytes(LabelOf(purpose)));
        labelled.insert(labelled.end(), fields.begin(), fields.end());
        return LabelledMessage { purpose, WireFields::Encode(WireFields::FieldList { labelled }) };
    }

    /// As `Of(purpose, fields)`, for a field list spelled at the call site.
    /// @param purpose Which construction; selects the label.
    /// @param fields The construction's fields after the label, in wire order.
    /// @return The message.
    [[nodiscard]] static LabelledMessage Of(IdentityKeyPurpose purpose,
                                            std::initializer_list<std::span<std::byte const>> fields)
    {
        return Of(purpose, WireFields::AsFields(fields));
    }

    /// @return The construction this message is for.
    [[nodiscard]] IdentityKeyPurpose Purpose() const noexcept
    {
        return _purpose;
    }

    /// @return The encoded message: what is signed and what a signature is verified over.
    [[nodiscard]] std::span<std::byte const> Bytes() const noexcept
    {
        return _bytes;
    }

    [[nodiscard]] friend bool operator==(LabelledMessage const&, LabelledMessage const&) = default;

  private:
    LabelledMessage(IdentityKeyPurpose purpose, std::vector<std::byte> bytes):
        _purpose { purpose },
        _bytes { std::move(bytes) }
    {
    }

    IdentityKeyPurpose _purpose;
    std::vector<std::byte> _bytes;
};

/// Sign @p message with @p key: the ONE first-party call of `Ed25519KeyPair::Sign` outside tests.
///
/// Every holder of an identity key signs through here, so what the key signs is a
/// `LabelledMessage` and nothing else; `IdentityKeyLabel_test`'s census holds every other call of
/// `Sign(` in the tree to a row naming why it is not a raw one.
/// @param key The key pair.
/// @param message What is signed.
/// @return The signature.
[[nodiscard]] inline Ed25519Signature SignLabelled(Ed25519KeyPair const& key, LabelledMessage const& message) noexcept
{
    return key.Sign(message.Bytes());
}

/// Whether @p signature is @p key's signature of @p message.
/// @param key The public key the signer is held to.
/// @param message What should have been signed.
/// @param signature What was presented.
/// @return True when it verifies.
[[nodiscard]] inline bool VerifyLabelled(Ed25519PublicKey const& key,
                                         LabelledMessage const& message,
                                         Ed25519Signature const& signature)
{
    return Ed25519Verify(key, message.Bytes(), signature);
}

} // namespace FastCache
