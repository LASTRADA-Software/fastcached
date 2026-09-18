// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/CryptoError.hpp>
#include <FastCache/Core/SecureBytes.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <span>

namespace FastCache
{

// X25519 Diffie-Hellman (RFC 7748 §5 and §6.1), over the vendored Monocypher (#178).
//
// **No production caller yet, by design.** PR 1 of #178's staged plan: PR 3 derives each Raft peer
// session's key from an ephemeral X25519 exchange, through `Core/Hkdf`. Do not remove these as
// unused before then; `Core/Ed25519.hpp` says the same and names the seam rule these share.
//
// No randomness is drawn here: an ephemeral secret is 32 bytes the CALLER supplies, from the
// operating system's generator (#1527's `ISecureRandom`).

/// Bytes in an X25519 secret key, a public key and a shared secret alike.
inline constexpr std::size_t X25519KeyBytes = 32;

/// An X25519 public key: the u-coordinate of a point on Curve25519. Not a secret.
using X25519PublicKey = std::array<std::byte, X25519KeyBytes>;

/// The public key belonging to @p secretKey: X25519(secretKey, 9), RFC 7748 §6.1.
///
/// Any 32 bytes are a usable secret key -- the function clamps the scalar itself (§5), so there
/// is no invalid secret to refuse beyond its length.
/// @param secretKey The secret key, exactly `X25519KeyBytes` long.
/// @return The public key, or `CryptoError::WrongKeyLength`.
[[nodiscard]] std::expected<X25519PublicKey, CryptoError> X25519PublicKeyFrom(std::span<std::byte const> secretKey);

/// The shared secret X25519(secretKey, peerPublicKey), RFC 7748 §6.1.
///
/// REFUSES an all-zero result, which is what a low-order peer key produces whatever this end's
/// secret: a peer that sends one has fixed the "shared" secret to a value everybody knows, and
/// RFC 7748 §6.1 names checking for it as the defence. Refused rather than returned, so no caller
/// can forget the check.
///
/// The result is RAW -- a curve coordinate, not uniformly random bytes -- and is not a key.
/// Derive one from it with `Core/Hkdf`, binding in both ends' public keys.
/// @param secretKey This end's secret key, exactly `X25519KeyBytes` long.
/// @param peerPublicKey The other end's public key.
/// @return The 32-byte shared secret, in storage zeroed at release; `CryptoError::WrongKeyLength`
///         for a secret of another length; `CryptoError::LowOrderPeerKey` for the all-zero result.
[[nodiscard]] std::expected<SecureByteBuffer, CryptoError> X25519SharedSecret(std::span<std::byte const> secretKey,
                                                                              X25519PublicKey const& peerPublicKey);

} // namespace FastCache
