// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// A 128-bit MurmurHash3 digest, as the two 64-bit halves the algorithm produces.
struct Murmur128
{
    std::uint64_t high {}; ///< First output word (`h1` in the reference).
    std::uint64_t low {};  ///< Second output word (`h2` in the reference).

    /// Value equality, so a digest can be compared without unpacking it.
    [[nodiscard]] friend constexpr bool operator==(Murmur128 const&, Murmur128 const&) noexcept = default;
};

namespace detail
{

    /// Mixing constants of the x64_128 variant.
    inline constexpr std::uint64_t Murmur3C1 = 0x87C3'7B91'1142'53D5ULL;
    inline constexpr std::uint64_t Murmur3C2 = 0x4CF5'AD43'2745'937FULL;

    /// Size of one processed block, in bytes. Two 64-bit words.
    inline constexpr std::size_t Murmur3BlockSize = 16;

    /// The algorithm's 64-bit finalisation mix (`fmix64`).
    /// @param value Word to avalanche.
    /// @return The mixed word.
    [[nodiscard]] constexpr std::uint64_t Murmur3Mix(std::uint64_t value) noexcept
    {
        value ^= value >> 33;
        value *= 0xFF51'AFD7'ED55'8CCDULL;
        value ^= value >> 33;
        value *= 0xC4CE'B9FE'1A85'EC53ULL;
        value ^= value >> 33;
        return value;
    }

} // namespace detail

/// MurmurHash3, 128-bit output, x64 variant — the launcher's cache-key digest.
///
/// `x64` names the VARIANT, not the host. MurmurHash3 defines two 128-bit
/// variants: `x86_128`, built from four 32-bit lanes, and `x64_128`, built from
/// two 64-bit lanes. They produce different digests for the same input, so the
/// variant is part of the format and is spelled in this type's documentation
/// rather than left to the reader. Nothing here is architecture-specific: plain
/// unsigned 64-bit multiply, rotate and xor, with no intrinsic, SIMD or assembly,
/// so the digest is bit-identical on x86-64, aarch64 and anything else.
///
/// That portability is load-bearing rather than incidental. This value keys a
/// cache shared between machines, so a digest that differed by architecture would
/// not fail — it would silently split the cache in two, with every machine
/// missing on every entry the others wrote. Four things could cause that, and
/// each is closed here by construction:
///
///  - **`char` signedness differs** — signed on x86-64 Linux, unsigned on
///    aarch64 — so a byte widened through a plain `char` sign-extends differently
///    per architecture. Everything here is `std::byte` and `std::uint8_t`; a
///    `char` never reaches the arithmetic.
///  - **Block loads are explicitly little-endian** (`ReadLittleEndian`), not
///    native-order reads through a `std::uint64_t const*`.
///  - **That same cast would be unaligned** and therefore undefined behaviour
///    whatever the hardware tolerates; `ReadLittleEndian` copies instead, which
///    is also what satisfies the `clang-debug` preset's `-fsanitize=alignment`.
///  - **Rotation uses `std::rotl`**, not `(x << r) | (x >> (64 - r))`, which is
///    undefined when `r` is zero.
///
/// `MurmurHash3_test.cpp` pins the published SMHasher verification value, which
/// sweeps all 256 tail lengths, and CI runs it on arm64 macOS as well as x86-64
/// Linux and Windows — so architecture divergence is a red build, not a silent
/// halving of the hit rate.
///
/// Not collision-resistant against an adversary: MurmurHash3 is a
/// non-cryptographic hash and collisions in it can be constructed deliberately.
/// That is accepted here because the key is not a security boundary — anyone able
/// to STORE can already write a wrong object under a correct key — while the
/// accidental collisions this exists to prevent are closed by 128 bits of
/// avalanche.
///
/// There is no injected seam because there is nothing to inject: no clock,
/// socket, filesystem or environment is reachable from any of it, which is the
/// pure-leaf exception the dependency-injection rule states.
///
/// The class is the incremental form — default construction *is* seeding, so a
/// forgotten seed cannot be written. `MurmurHash3Compute` is the one-shot form.
class MurmurHash3
{
  public:
    /// Number of characters `ToHex` renders: two 64-bit words, 16 hex digits each.
    static constexpr std::size_t HexLength = 32;

    /// Start a digest.
    ///
    /// The seed is the reference algorithm's own parameter and is carried for one
    /// reason: SMHasher's published verification value — the anchor proving this
    /// is MurmurHash3 x64_128 and not a lookalike — hashes 256 messages under 256
    /// different seeds, so a seedless implementation could not be checked against
    /// it at all. Every caller in this repository uses the default 0 deliberately:
    /// key spaces are separated by their leading schema tag, which is inside the
    /// hashed bytes and prefix-free, not by a seed sitting outside them.
    /// @param seed Initial state for both words; 0 unless conformance-testing.
    explicit MurmurHash3(std::uint32_t seed = 0) noexcept:
        _h1 { seed },
        _h2 { seed }
    {
    }

    /// Fold a byte range into the digest.
    ///
    /// Whole 16-byte blocks are mixed immediately and a short remainder is
    /// carried until the next call completes it, so feeding a message in pieces
    /// gives the identical digest to hashing it whole. That equivalence is what
    /// lets the callers stream: `ComputeKey` no longer materializes a
    /// multi-megabyte copy of the preprocessed text purely to hash it, and
    /// `HashFileContents` hashes a file in fixed-size chunks. It is pinned by the
    /// split-offset test rather than assumed.
    /// @param bytes Bytes to fold in.
    void Update(BytesView bytes) noexcept
    {
        _totalLength += bytes.size();

        if (_carryLength != 0)
        {
            auto const wanted = std::min(detail::Murmur3BlockSize - _carryLength, bytes.size());
            std::ranges::copy(bytes.first(wanted), _carry.begin() + static_cast<std::ptrdiff_t>(_carryLength));
            _carryLength += wanted;
            bytes = bytes.subspan(wanted);
            if (_carryLength < detail::Murmur3BlockSize)
                return;
            MixBlock(BytesView { _carry });
            _carryLength = 0;
        }

        while (bytes.size() >= detail::Murmur3BlockSize)
        {
            MixBlock(bytes.first(detail::Murmur3BlockSize));
            bytes = bytes.subspan(detail::Murmur3BlockSize);
        }

        std::ranges::copy(bytes, _carry.begin());
        _carryLength = bytes.size();
    }

    /// Fold text into the digest.
    /// @param text Characters to fold in, reinterpreted as bytes.
    void Update(std::string_view text) noexcept
    {
        Update(AsBytes(text));
    }

    /// Fold a single byte into the digest — the separators of a key grammar.
    /// @param value Byte to fold in.
    void Update(std::byte value) noexcept
    {
        Update(BytesView { &value, 1 });
    }

    /// Close the digest without disturbing the state, so it can be read and then
    /// extended.
    /// @return The finalised 128-bit digest.
    [[nodiscard]] Murmur128 Finalise() const noexcept
    {
        auto h1 = _h1;
        auto h2 = _h2;

        // The tail: up to 15 bytes that never completed a block. Each byte is
        // shifted into place by its own index, so the two words are assembled
        // little-endian without a partial load.
        std::uint64_t k1 = 0;
        std::uint64_t k2 = 0;
        for (auto const index: std::views::iota(std::size_t { 0 }, _carryLength))
        {
            auto const octet = static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(_carry[index]));
            if (index >= 8)
                k2 ^= octet << (8 * (index - 8));
            else
                k1 ^= octet << (8 * index);
        }
        if (_carryLength > 8)
        {
            k2 *= detail::Murmur3C2;
            k2 = std::rotl(k2, 33);
            k2 *= detail::Murmur3C1;
            h2 ^= k2;
        }
        if (_carryLength > 0)
        {
            k1 *= detail::Murmur3C1;
            k1 = std::rotl(k1, 31);
            k1 *= detail::Murmur3C2;
            h1 ^= k1;
        }

        h1 ^= static_cast<std::uint64_t>(_totalLength);
        h2 ^= static_cast<std::uint64_t>(_totalLength);
        h1 += h2;
        h2 += h1;
        h1 = detail::Murmur3Mix(h1);
        h2 = detail::Murmur3Mix(h2);
        h1 += h2;
        h2 += h1;

        return Murmur128 { .high = h1, .low = h2 };
    }

    /// Render the finalised digest as `HexLength` lowercase hex characters, high
    /// half first. That order and case ARE the wire format of every cache key, so
    /// they are pinned by a test rather than left to a formatting default.
    /// @return The rendered digest.
    [[nodiscard]] std::string ToHex() const
    {
        return ToHex(Finalise());
    }

    /// Render an already-finalised digest.
    ///
    /// A nibble table rather than `std::format`: this header is included by five
    /// translation units and `<format>` is one of the heaviest in the standard
    /// library, which is a cost the launcher's build pays for no benefit. The
    /// output is byte-for-byte what `std::format("{:016x}{:016x}", high, low)`
    /// produces, which a test asserts.
    /// @param digest Digest to render.
    /// @return `HexLength` lowercase hex characters.
    [[nodiscard]] static std::string ToHex(Murmur128 digest)
    {
        constexpr std::string_view HexDigits = "0123456789abcdef";
        std::string out;
        out.reserve(HexLength);
        for (auto const word: { digest.high, digest.low })
            for (auto const shift: std::views::iota(0, 16) | std::views::reverse)
                out.push_back(HexDigits[(word >> (4 * shift)) & 0xFULL]);
        return out;
    }

  private:
    /// Mix exactly one 16-byte block into the running state.
    /// @param block Exactly `Murmur3BlockSize` bytes.
    void MixBlock(BytesView block) noexcept
    {
        auto k1 = ReadLittleEndian<std::uint64_t>(block.first(8));
        auto k2 = ReadLittleEndian<std::uint64_t>(block.subspan(8, 8));

        k1 *= detail::Murmur3C1;
        k1 = std::rotl(k1, 31);
        k1 *= detail::Murmur3C2;
        _h1 ^= k1;

        _h1 = std::rotl(_h1, 27);
        _h1 += _h2;
        _h1 = (_h1 * 5) + 0x52DC'E729ULL;

        k2 *= detail::Murmur3C2;
        k2 = std::rotl(k2, 33);
        k2 *= detail::Murmur3C1;
        _h2 ^= k2;

        _h2 = std::rotl(_h2, 31);
        _h2 += _h1;
        _h2 = (_h2 * 5) + 0x3849'5AB5ULL;
    }

    std::uint64_t _h1 { 0 };                                   ///< Running first word; seeded with 0.
    std::uint64_t _h2 { 0 };                                   ///< Running second word; seeded with 0.
    std::array<std::byte, detail::Murmur3BlockSize> _carry {}; ///< Bytes short of a whole block.
    std::size_t _carryLength { 0 };                            ///< How many of `_carry` are live.
    std::size_t _totalLength { 0 };                            ///< Message length so far, in bytes.
};

/// One-shot MurmurHash3 x64_128 over a byte range.
/// @param bytes Message to digest.
/// @param seed Initial state; 0 unless conformance-testing.
/// @return The 128-bit digest.
[[nodiscard]] inline Murmur128 MurmurHash3Compute(BytesView bytes, std::uint32_t seed = 0) noexcept
{
    MurmurHash3 digest { seed };
    digest.Update(bytes);
    return digest.Finalise();
}

/// One-shot MurmurHash3 x64_128 over text.
/// @param text Message to digest.
/// @param seed Initial state; 0 unless conformance-testing.
/// @return The 128-bit digest.
[[nodiscard]] inline Murmur128 MurmurHash3Compute(std::string_view text, std::uint32_t seed = 0) noexcept
{
    return MurmurHash3Compute(AsBytes(text), seed);
}

} // namespace FastCache
