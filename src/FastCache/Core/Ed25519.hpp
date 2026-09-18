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

// Ed25519 signatures (RFC 8032 §5.1), over the vendored Monocypher (#178).
//
// **These have no production caller yet, and that is the plan rather than dead code.** They are
// PR 1 of #178's staged design: PR 2 mints a node's identity key with them and PR 3 signs the
// Raft handshake with it. Do not remove them as unused before those land -- the design comment on
// #178 is the argument, and this header is its seam.
//
// This file and its two siblings (`Core/X25519.hpp`, `Core/Hkdf.hpp`) are the ONLY way into the
// vendored implementation: no other first-party file may include a Monocypher header, and
// `ctest -R crypto-seam` refuses one that does. A caller that wants a curve operation this seam
// does not offer adds it HERE, where it is reviewed against the RFC vectors in `Ed25519_test.cpp`,
// never by reaching past it.
//
// Pure Ed25519, the RFC 8032 function: SHA-512 inside, no context, no pre-hash. NOT Monocypher's
// default `crypto_eddsa_*`, which is EdDSA over BLAKE2b -- a different signature scheme whose
// signatures no other Ed25519 implementation verifies, and which the RFC's vectors cannot test.
//
// No randomness is drawn here. Ed25519 signing is deterministic, and a key pair is derived from a
// seed the CALLER supplies -- from the operating system's generator (#1527's `ISecureRandom`) for a
// new identity, or from the file a node's identity was minted into. So these functions touch no
// ambient state and need no seam of their own.

/// Bytes in an Ed25519 seed: the 32-byte private key of RFC 8032 §5.1.5, from which the
/// signing scalar, the nonce prefix and the public key are all derived.
inline constexpr std::size_t Ed25519SeedBytes = 32;

/// Bytes in an encoded Ed25519 public key (RFC 8032 §5.1.2).
inline constexpr std::size_t Ed25519PublicKeyBytes = 32;

/// Bytes in an Ed25519 signature: the encoded point R, then the scalar S (RFC 8032 §5.1.6).
inline constexpr std::size_t Ed25519SignatureBytes = 64;

/// An Ed25519 public key. Not a secret, so a fixed-size value type.
using Ed25519PublicKey = std::array<std::byte, Ed25519PublicKeyBytes>;

/// An Ed25519 signature. Not a secret, so a fixed-size value type.
using Ed25519Signature = std::array<std::byte, Ed25519SignatureBytes>;

/// An Ed25519 key pair: the secret half that signs, and the public half that verifies.
///
/// The secret lives in a `SecureByteBuffer`, which zeroes its storage at every release, so a copy
/// of this object is safe without anybody thinking about it -- the credential rule in
/// `.agent/rules/distributed-compilation.md`. It is held in Monocypher's 64-byte layout, the seed
/// followed by the public key, because that is what signing reads; the expanded scalar is
/// re-derived per signature and never stored.
class Ed25519KeyPair
{
  public:
    /// Derive the key pair a 32-byte seed determines (RFC 8032 §5.1.5).
    ///
    /// The seed is only READ: Monocypher's own derivation wipes the buffer it is handed, so it is
    /// handed a copy, in a `SecureByteBuffer` that is zeroed when it is released.
    /// @param seed The private key, exactly `Ed25519SeedBytes` long.
    /// @return The key pair, or `CryptoError::WrongKeyLength` for a seed of any other length.
    [[nodiscard]] static std::expected<Ed25519KeyPair, CryptoError> FromSeed(std::span<std::byte const> seed);

    /// The public half, which is what a peer verifies against and what a roster records.
    /// @return The encoded public key.
    [[nodiscard]] Ed25519PublicKey const& PublicKey() const noexcept
    {
        return _publicKey;
    }

    /// Sign @p message (RFC 8032 §5.1.6). Deterministic: the same key and message always
    /// produce the same signature, so signing draws no randomness and cannot fail.
    /// @param message What is being signed; any length.
    /// @return The 64-byte signature.
    [[nodiscard]] Ed25519Signature Sign(std::span<std::byte const> message) const noexcept;

  private:
    /// Adopt an already derived pair. Private, so `FromSeed` is the only way one is made.
    /// @param secretKey Monocypher's 64-byte secret key: the seed, then the public key.
    /// @param publicKey The public key.
    Ed25519KeyPair(SecureByteBuffer secretKey, Ed25519PublicKey const& publicKey) noexcept;

    /// The seed followed by the public key, as Monocypher's signing function reads it.
    SecureByteBuffer _secretKey;

    /// The public key, kept apart so reading it does not touch the secret.
    Ed25519PublicKey _publicKey {};
};

/// Whether @p signature is a valid Ed25519 signature of @p message under @p publicKey
/// (RFC 8032 §5.1.7).
///
/// Strict where malleability lives: a signature whose scalar S is not below the group order
/// L is REFUSED, so a valid signature cannot be turned into a second valid signature of the
/// same message by adding L to it. An R or an A that is not a point on the curve is refused.
///
/// One leniency, stated rather than left to be found: Monocypher accepts a NON-CANONICAL
/// encoding of R or A -- a y coordinate at or above p, where RFC 8032 §5.1.3 has decoding
/// fail (`crypto_eddsa_check_equation`, "*Allow* non-cannonical encoding"). Only a point
/// whose y is below 19 has such a second spelling, so an honestly made signature or key has
/// none in practice, and this seam does not re-check it.
/// @param publicKey The signer's public key.
/// @param message What was signed.
/// @param signature The signature to check.
/// @return True only when the signature verifies.
[[nodiscard]] bool Ed25519Verify(Ed25519PublicKey const& publicKey,
                                 std::span<std::byte const> message,
                                 Ed25519Signature const& signature) noexcept;

} // namespace FastCache
