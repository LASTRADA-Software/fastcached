// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/CryptoError.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <cstddef>
#include <expected>
#include <span>

namespace FastCache
{

// HKDF over HMAC-SHA256 (RFC 5869), on this tree's own `HmacSha256` (`Core/Sha256`).
//
// **No production caller yet, by design.** PR 1 of #178's staged plan: PR 3 turns each Raft peer
// session's raw X25519 secret into its frame key with this. Do not remove it as unused before then.
//
// It is the sibling of `Core/Ed25519` and `Core/X25519` but needs no Monocypher: HMAC-SHA256 is
// already here, verified against RFC 4231, and a second SHA-256 would be a second implementation
// of one function to keep identical on every machine. So it is NOT one of the files allowed past
// the crypto seam, and `ctest -R crypto-seam` would refuse it for including a Monocypher header.
//
// Every intermediate that carries key material -- the PRK, each block T(i), the buffer T(i) is
// computed over -- lives in a `SecureByteBuffer` or is wiped with `SecureZero`, the tree's one
// zeroing primitive, before it goes out of scope.

/// Bytes in one SHA-256 digest: the size of a PRK, and of each block of HKDF-SHA256 output.
inline constexpr std::size_t HkdfSha256HashBytes = Sha256::DigestSize;

/// The most HKDF-SHA256 can expand to: 255 blocks, RFC 5869 §2.3's "L <= 255*HashLen".
inline constexpr std::size_t HkdfSha256MaxOutputBytes = 255 * HkdfSha256HashBytes;

/// HKDF-Extract (RFC 5869 §2.2): PRK = HMAC-SHA256(salt, IKM).
///
/// An empty @p salt is the RFC's "not provided", which it defines as HashLen zero bytes. HMAC
/// zero-pads its key to the block size, so an empty key and 32 zero bytes are the same key, and
/// no special case is written for it -- RFC 5869's test case 3 is what checks that.
/// @param salt A non-secret random value; may be empty.
/// @param inputKeyMaterial The secret to extract from -- a raw X25519 output, for instance.
/// @return The 32-byte pseudorandom key, in storage zeroed at release.
[[nodiscard]] SecureByteBuffer HkdfSha256Extract(std::span<std::byte const> salt,
                                                 std::span<std::byte const> inputKeyMaterial);

/// HKDF-Expand (RFC 5869 §2.3): @p outputBytes of output keying material from a PRK.
/// @param pseudoRandomKey A PRK of at least `HkdfSha256HashBytes`, usually `HkdfSha256Extract`'s.
/// @param info Context binding the output to its purpose; may be empty.
/// @param outputBytes How many bytes to produce, from 1 to `HkdfSha256MaxOutputBytes`.
/// @return The output keying material, in storage zeroed at release; `CryptoError::OutputTooLong`
///         above the RFC's bound, `CryptoError::EmptyOutput` for zero, and
///         `CryptoError::PseudoRandomKeyTooShort` for a PRK shorter than one digest.
[[nodiscard]] std::expected<SecureByteBuffer, CryptoError> HkdfSha256Expand(std::span<std::byte const> pseudoRandomKey,
                                                                            std::span<std::byte const> info,
                                                                            std::size_t outputBytes);

/// HKDF (RFC 5869 §2): extract, then expand.
/// @param salt A non-secret random value; may be empty.
/// @param inputKeyMaterial The secret to derive from.
/// @param info Context binding the output to its purpose; may be empty.
/// @param outputBytes How many bytes to produce, from 1 to `HkdfSha256MaxOutputBytes`.
/// @return The output keying material, or `HkdfSha256Expand`'s refusals.
[[nodiscard]] std::expected<SecureByteBuffer, CryptoError> HkdfSha256(std::span<std::byte const> salt,
                                                                      std::span<std::byte const> inputKeyMaterial,
                                                                      std::span<std::byte const> info,
                                                                      std::size_t outputBytes);

} // namespace FastCache
