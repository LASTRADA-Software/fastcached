// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file MachineTicket.hpp
/// A machine ticket: a short-lived statement, signed by one machine's identity key, that the
/// connection presenting it speaks for that machine to ONE audience (spec §5).
///
/// Header-only so the node, the CLI and the tests share one layout. The launcher never reads a
/// ticket: it carries the bytes its own node minted into AUTH, opaque.
namespace FastCache::Distributed
{

/// The first signed FIELD of every ticket: what it is, and which layout. A field and not a
/// prefix, the rule every construction under an identity key keeps (#178), so a signature over a
/// ticket can never be replayed as a discovery proof or a lease. Spelled in ONE place,
/// `IdentityKeyLabels`, beside every other construction's; the decoder reads it by this name. A
/// layout change is a new label there.
inline constexpr std::string_view MachineTicketLabel = LabelOf(IdentityKeyPurpose::MachineTicket);

/// How many random bytes make two tickets for one audience different; drawn from `ISecureRandom`.
inline constexpr std::size_t MachineTicketNonceBytes = 16;

/// How long a minted ticket is good for. One exchange presents it once, at AUTH, so this bounds
/// how long a CAPTURED ticket is worth anything, never how long a compile may run.
inline constexpr std::chrono::seconds MachineTicketLifetime { 60 };

/// The largest encoded ticket a decoder reads. Five short fields and a signature; AUTH's own
/// ceiling (`MaxAuthPayload`) is larger, so this is the tighter of the two.
inline constexpr std::size_t MaxMachineTicketBytes = 1024;

/// The largest expiry this host's `system_clock` can represent, in seconds: refused at decode,
/// before any conversion could overflow a signed tick count.
inline constexpr std::uint64_t MaxMachineTicketExpirySeconds = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::duration::max()).count());

/// A ticket's nonce.
using MachineTicketNonce = std::array<std::byte, MachineTicketNonceBytes>;

/// What a ticket claims.
struct MachineTicketClaims
{
    std::string machineId;                 ///< The machine it speaks for: a member id.
    std::string audience;                  ///< The endpoint it may be presented to, as the presenter dialled it.
    std::uint64_t expiresAtUnixSeconds {}; ///< When it stops being good, in Unix seconds.
    MachineTicketNonce nonce {};           ///< Distinguishes two tickets with otherwise equal claims.

    [[nodiscard]] friend bool operator==(MachineTicketClaims const&, MachineTicketClaims const&) = default;
};

/// A ticket as decoded: its claims, the bytes its signature covers AS RECEIVED, and the signature.
/// Nothing here is verified; that is `TicketVerifier`'s.
///
/// **Plain storage, deliberately, and consistently with where it came from.** A decoded ticket is
/// read out of an AUTH frame the connection already holds in ordinary buffers, so a
/// `SecureByteBuffer` here would wipe one copy of bytes the frame still carries unwiped. The
/// `SecureString` rule governs the MINTED ticket, the bearer credential this node itself holds
/// and hands out (`MintMachineTicket`). Change the two together or neither: securing only this
/// side protects nothing, and weakening only the minting side hands a live credential to storage
/// no allocator wipes.
struct DecodedMachineTicket
{
    MachineTicketClaims claims;         ///< What it says.
    std::vector<std::byte> signedBytes; ///< The packed claims exactly as they arrived.
    Ed25519Signature signature {};      ///< Over `signedBytes`.
};

namespace Detail
{
    inline constexpr std::size_t MachineTicketClaimFields = 5;    ///< label, id, audience, expiry, nonce
    inline constexpr std::size_t MachineTicketEnvelopeFields = 2; ///< packed claims, signature
} // namespace Detail

/// What a ticket's signature covers: its label (`IdentityKeyPurpose::MachineTicket`), then every
/// claim, each length-prefixed. One function for the minter and the verifier, so the two cannot
/// spell the list apart -- and a `LabelledMessage`, so the label is first by construction.
/// @param claims What the ticket claims.
/// @return The message.
[[nodiscard]] inline LabelledMessage MachineTicketMessage(MachineTicketClaims const& claims)
{
    auto const expiry = WireFields::ToBigEndian<std::uint64_t>(claims.expiresAtUnixSeconds);
    return LabelledMessage::Of(IdentityKeyPurpose::MachineTicket,
                               { WireFields::AsBytes(claims.machineId),
                                 WireFields::AsBytes(claims.audience),
                                 std::span<std::byte const> { expiry },
                                 std::span<std::byte const> { claims.nonce } });
}

/// The bytes a ticket's signature covers, as a ticket carries them.
/// @param claims What the ticket claims.
/// @return `MachineTicketMessage(claims)`'s bytes.
[[nodiscard]] inline std::vector<std::byte> PackMachineTicketClaims(MachineTicketClaims const& claims)
{
    auto const message = MachineTicketMessage(claims);
    return { message.Bytes().begin(), message.Bytes().end() };
}

/// Sign a ticket with this machine's identity key.
/// @param key This machine's identity key.
/// @param claims What the ticket claims; `machineId` must be the id that key is recorded under.
/// @return The encoded ticket, held in wiping storage: it is a bearer credential until it expires.
[[nodiscard]] inline SecureString MintMachineTicket(Ed25519KeyPair const& key, MachineTicketClaims const& claims)
{
    auto const message = MachineTicketMessage(claims);
    auto signature = SignLabelled(key, message);
    auto envelope = WireFields::Encode({ message.Bytes(), std::span<std::byte const> { signature } });
    auto machineTicket = SecureString { WireFields::AsStringView(envelope) };
    // Both copies outside the `SecureString` are wiped. The claims are not secret, but they and
    // the signature together ARE the ticket, so the signature's copy is wiped with the envelope.
    SecureZero(envelope.data(), envelope.size());
    SecureZero(signature.data(), signature.size());
    return machineTicket;
}

/// Decode a ticket. Verifies NOTHING but its shape.
/// @param bytes The ticket as presented.
/// @return The ticket, or nullopt when the bytes are not exactly one: another label, an empty id
///         or audience, a nonce of the wrong width, an expiry the clock cannot hold, or more than
///         `MaxMachineTicketBytes`.
[[nodiscard]] inline std::optional<DecodedMachineTicket> DecodeMachineTicket(std::span<std::byte const> bytes)
{
    if (bytes.size() > MaxMachineTicketBytes)
        return std::nullopt;
    auto const outer = WireFields::SplitExactly(bytes, Detail::MachineTicketEnvelopeFields);
    if (!outer.has_value() || (*outer)[1].size() != Ed25519SignatureBytes)
        return std::nullopt;
    auto const packed = (*outer)[0];
    auto const fields = WireFields::SplitExactly(packed, Detail::MachineTicketClaimFields);
    if (!fields.has_value() || WireFields::AsStringView((*fields)[0]) != MachineTicketLabel || (*fields)[1].empty()
        || (*fields)[2].empty() || (*fields)[4].size() != MachineTicketNonceBytes)
        return std::nullopt;
    auto const expiry = WireFields::FromBigEndian<std::uint64_t>((*fields)[3]);
    if (!expiry.has_value() || *expiry > MaxMachineTicketExpirySeconds)
        return std::nullopt;

    auto ticket = DecodedMachineTicket {};
    ticket.claims.machineId = std::string { WireFields::AsStringView((*fields)[1]) };
    ticket.claims.audience = std::string { WireFields::AsStringView((*fields)[2]) };
    ticket.claims.expiresAtUnixSeconds = *expiry;
    std::ranges::copy((*fields)[4], ticket.claims.nonce.begin());
    ticket.signedBytes.assign(packed.begin(), packed.end());
    std::ranges::copy((*outer)[1], ticket.signature.begin());
    return ticket;
}

} // namespace FastCache::Distributed
