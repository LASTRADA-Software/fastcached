// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/CryptoError.hpp>
#include <FastCache/Core/SecureBytes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

// Ed25519 signatures (RFC 8032 §5.1), over the vendored Monocypher (#178).
//
// **The key pair's first production caller is a node minting its identity key (#178 PR 2); signing
// and verifying have none yet, and that is the plan rather than dead code.** PR 3 signs the Raft
// handshake with that key. Do not remove `Sign` or `Ed25519Verify` as unused before it lands -- the
// design comment on #178 is the argument, and this header is its seam.
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

/// Characters in a public key's text form: its 32 bytes as unpadded base64url.
///
/// **Shown WHOLE, everywhere, and never abbreviated** (#178). A key an operator compares
/// between two machines is compared by eye, and an abbreviated identifier is a display form
/// that has already been compared as if it were the whole -- the node-id rule, one value along.
inline constexpr std::size_t Ed25519PublicKeyTextLength = 43;

/// Why a string is not a public key's text form.
///
/// **Private: never transmitted or persisted.** A key travels as its 32 bytes, and only the
/// text an operator types reaches this; the enumerators carry no values for that reason.
enum class PublicKeyTextFault : std::uint8_t
{
    WrongLength,  ///< Not 43 characters, which is the only length 32 bytes encode to.
    NotBase64Url, ///< A character outside `A-Z a-z 0-9 - _`, or a last one carrying bits no key has.
    Last,         ///< Not a fault, and has no row: the length of a table keyed by one.
};

/// One row per `PublicKeyTextFault`: what an operator is told.
struct PublicKeyTextFaultRow
{
    PublicKeyTextFault fault; ///< The fault.
    std::string_view why;     ///< The sentence, naming what a key looks like.
};

/// The sentence for every fault, in enumerator order.
inline constexpr EnumTable<PublicKeyTextFault, PublicKeyTextFaultRow> PublicKeyTextFaults { {
    { .fault = PublicKeyTextFault::WrongLength,
      .why = "a public key is 43 base64url characters, as --node-status and the startup log print it" },
    { .fault = PublicKeyTextFault::NotBase64Url,
      .why = "a public key is written in base64url (A-Z a-z 0-9 - _) with no padding, and its last character "
             "may carry no bits a 32-byte key does not have" },
} };

static_assert(RowsInEnumeratorOrder(PublicKeyTextFaults, &PublicKeyTextFaultRow::fault),
              "PublicKeyTextFaults must hold one row per PublicKeyTextFault, in enumerator order");

/// What an operator is told about @p fault.
/// @param fault Why a string was not a key.
/// @return The sentence.
[[nodiscard]] constexpr std::string_view DescribePublicKeyTextFault(PublicKeyTextFault fault) noexcept
{
    return PublicKeyTextFaults[static_cast<std::size_t>(fault)].why;
}

/// Spell @p key the way an operator reads and types it: 43 characters of unpadded base64url.
///
/// **The one encoder**, and `ParseEd25519PublicKey` is the one parser: the startup log,
/// `--node-status`, `--cluster-status`, a service registration and `--raft-peer` all go through
/// these two, so the string one prints is always one the other reads.
/// @param key The public key.
/// @return Its text form, exactly `Ed25519PublicKeyTextLength` characters.
[[nodiscard]] std::string FormatEd25519PublicKey(Ed25519PublicKey const& key);

/// Read a public key back out of its text form.
///
/// Strict, and the strictness is what makes the text a key's only spelling: exactly 43
/// characters, the URL-safe alphabet, and a canonical last character (`Base64UrlDecode`). Two
/// strings naming one key would be two things an operator could type into a roster and one key
/// a revocation could miss.
///
/// Whether the 32 bytes are a point on the curve is NOT asked. A key that is not one verifies no
/// signature, so admitting it grants nothing -- and this seam offers no point test, deliberately:
/// a check whose only effect is a nicer refusal for a string nobody printed is not worth a
/// second route into the curve arithmetic.
/// @param text What the operator typed.
/// @return The key, or why the text is not one.
[[nodiscard]] std::expected<Ed25519PublicKey, PublicKeyTextFault> ParseEd25519PublicKey(std::string_view text);

} // namespace FastCache
