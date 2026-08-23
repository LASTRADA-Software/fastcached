// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// SHA-256 (FIPS 180-4).
///
/// Here rather than through OpenSSL because OpenSSL is **optional** in this
/// build (`FASTCACHED_ENABLE_TLS`, off by default), while peer authentication is
/// not: a cluster that can only authenticate its members when TLS happens to be
/// compiled in is a cluster that silently accepts anybody in the common
/// configuration. `Core/Compression` reaches for a library because a codec is
/// large and its output need only round-trip; a MAC is small and its output has
/// to be *identical* on every machine that checks it.
///
/// An existing, published algorithm rather than a construction assembled here,
/// for the reason `MurmurHash3` records: conformance is checkable against the
/// vectors in FIPS 180-4 and RFC 6234, and something invented locally would have
/// nothing to be checked against.
///
/// The portability hazards are the ones MurmurHash3 already documents, and are
/// closed the same way: everything is `std::byte`/`std::uint8_t` rather than
/// `char` (which is signed on x86-64 Linux and unsigned on aarch64), block loads
/// go through `ReadBigEndian` rather than a native-order read, and rotation uses
/// `std::rotr` rather than a shift pair that is UB at zero.
class Sha256
{
  public:
    /// Bytes in a SHA-256 digest.
    static constexpr std::size_t DigestSize = 32;

    /// Bytes in a SHA-256 compression block. HMAC's key padding depends on it.
    static constexpr std::size_t BlockSize = 64;

    using Digest = std::array<std::byte, DigestSize>;

    Sha256() = default;

    /// Absorb more input. May be called any number of times.
    /// @param input Bytes to hash.
    void Update(std::span<std::byte const> input) noexcept;

    /// Finish and return the digest. The object must not be updated afterwards.
    /// @return The 32-byte digest.
    [[nodiscard]] Digest Finish() noexcept;

    /// One-shot convenience.
    /// @param input Bytes to hash.
    /// @return The 32-byte digest.
    [[nodiscard]] static Digest Hash(std::span<std::byte const> input) noexcept;

  private:
    /// Compress one 64-byte block into the running state.
    /// @param block Exactly BlockSize bytes.
    void Compress(std::span<std::byte const> block) noexcept;

    /// The eight working variables, in FIPS 180-4's initial state.
    std::array<std::uint32_t, 8> _state { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };

    /// Bytes absorbed so far; the length is padded in as a bit count.
    std::uint64_t _bytesAbsorbed { 0 };

    /// Partial block awaiting BlockSize bytes.
    std::array<std::byte, BlockSize> _pending {};

    /// How much of `_pending` is filled.
    std::size_t _pendingSize { 0 };
};

/// HMAC-SHA256 (RFC 2104).
///
/// The keyed function a pre-shared-key handshake needs: a bare hash of
/// `key || challenge` is length-extendable and a hash of `challenge || key` is
/// weaker than HMAC against collisions in the underlying hash, so this is the
/// construction with published vectors (RFC 4231) rather than either of the
/// obvious hand-rolled ones.
/// @param key The shared secret; any length.
/// @param message What is being authenticated.
/// @return The 32-byte tag.
[[nodiscard]] Sha256::Digest HmacSha256(std::span<std::byte const> key, std::span<std::byte const> message);

/// Compare two digests without leaking where they first differ.
///
/// `std::ranges::equal` and `memcmp` both stop at the first difference, so the
/// time they take reveals how many leading bytes a guess got right -- which lets
/// an attacker who can retry recover a tag byte by byte instead of guessing all
/// 32 at once. Every comparison of a MAC against one an untrusted peer supplied
/// has to go through this.
/// @param lhs One digest.
/// @param rhs The other.
/// @return True when they are equal.
[[nodiscard]] bool ConstantTimeEquals(Sha256::Digest const& lhs, Sha256::Digest const& rhs) noexcept;

/// Lowercase hexadecimal spelling of a digest, for logs and wire text.
/// @param digest Digest to render.
/// @return 64 hex characters.
[[nodiscard]] std::string HexDigest(Sha256::Digest const& digest);

} // namespace FastCache
