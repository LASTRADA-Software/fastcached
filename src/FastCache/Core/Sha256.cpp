// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <algorithm>
#include <bit>
#include <ranges>

namespace FastCache
{

namespace
{
    /// The 64 round constants: the first 32 bits of the fractional parts of the
    /// cube roots of the first 64 primes (FIPS 180-4 §4.2.2).
    constexpr std::array<std::uint32_t, 64> RoundConstants { {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    } };

    /// FIPS 180-4 §4.1.2 functions. Spelled as the standard spells them, so a
    /// reader can check them against it line by line rather than reverse the
    /// algebra of a "simplified" form.
    [[nodiscard]] constexpr std::uint32_t Choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (~x & z);
    }

    [[nodiscard]] constexpr std::uint32_t Majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    [[nodiscard]] constexpr std::uint32_t BigSigma0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
    }

    [[nodiscard]] constexpr std::uint32_t BigSigma1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
    }

    [[nodiscard]] constexpr std::uint32_t SmallSigma0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
    }

    [[nodiscard]] constexpr std::uint32_t SmallSigma1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10U);
    }
} // namespace

void Sha256::Compress(std::span<std::byte const> block) noexcept
{
    std::array<std::uint32_t, 64> schedule {};

    // Big-endian by definition of the algorithm, and read through the helper
    // rather than a native-order load: a host-order read would make the digest
    // differ between machines, which for a MAC means two peers that can never
    // authenticate each other.
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 16 }))
        schedule[index] = ReadBigEndian<std::uint32_t>(block.subspan(index * 4, 4));

    for (auto const index: std::views::iota(std::size_t { 16 }, std::size_t { 64 }))
        schedule[index] = SmallSigma1(schedule[index - 2]) + schedule[index - 7] + SmallSigma0(schedule[index - 15])
                          + schedule[index - 16];

    auto working = _state;

    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 64 }))
    {
        auto const temp1 = working[7] + BigSigma1(working[4]) + Choose(working[4], working[5], working[6])
                           + RoundConstants[index] + schedule[index];
        auto const temp2 = BigSigma0(working[0]) + Majority(working[0], working[1], working[2]);

        working[7] = working[6];
        working[6] = working[5];
        working[5] = working[4];
        working[4] = working[3] + temp1;
        working[3] = working[2];
        working[2] = working[1];
        working[1] = working[0];
        working[0] = temp1 + temp2;
    }

    for (auto const index: std::views::iota(std::size_t { 0 }, _state.size()))
        _state[index] += working[index];
}

void Sha256::Update(std::span<std::byte const> input) noexcept
{
    _bytesAbsorbed += input.size();

    // Top up a partial block first, so the caller's chunking cannot change the
    // digest -- which is the whole contract of an incremental hash.
    if (_pendingSize != 0)
    {
        auto const take = std::min(BlockSize - _pendingSize, input.size());
        std::ranges::copy(input.first(take), std::next(_pending.begin(), static_cast<std::ptrdiff_t>(_pendingSize)));
        _pendingSize += take;
        input = input.subspan(take);

        if (_pendingSize < BlockSize)
            return;

        Compress(_pending);
        _pendingSize = 0;
    }

    while (input.size() >= BlockSize)
    {
        Compress(input.first(BlockSize));
        input = input.subspan(BlockSize);
    }

    std::ranges::copy(input, _pending.begin());
    _pendingSize = input.size();
}

Sha256::Digest Sha256::Finish() noexcept
{
    // FIPS 180-4 §5.1.1: append 0x80, then zeroes, then the length in BITS as a
    // big-endian 64-bit value, so the padded message is a whole number of blocks.
    auto const bitLength = _bytesAbsorbed * 8;

    _pending[_pendingSize] = std::byte { 0x80 };
    ++_pendingSize;

    // The length needs eight bytes at the end of a block; when they do not fit,
    // this block is zero-filled and compressed and the length goes in the next.
    if (_pendingSize > BlockSize - 8)
    {
        std::ranges::fill(std::span { _pending }.subspan(_pendingSize), std::byte { 0 });
        Compress(_pending);
        _pendingSize = 0;
    }

    std::ranges::fill(std::span { _pending }.subspan(_pendingSize, BlockSize - 8 - _pendingSize), std::byte { 0 });
    WriteBigEndian<std::uint64_t>(std::span { _pending }.subspan(BlockSize - 8, 8), bitLength);
    Compress(_pending);

    Digest digest {};
    for (auto const index: std::views::iota(std::size_t { 0 }, _state.size()))
        WriteBigEndian<std::uint32_t>(std::span { digest }.subspan(index * 4, 4), _state[index]);
    return digest;
}

Sha256::Digest Sha256::Hash(std::span<std::byte const> input) noexcept
{
    Sha256 hasher;
    hasher.Update(input);
    return hasher.Finish();
}

Sha256::Digest HmacSha256(std::span<std::byte const> key, std::span<std::byte const> message)
{
    // RFC 2104: a key longer than the block is replaced by its hash, and a
    // shorter one is zero-padded to the block.
    std::array<std::byte, Sha256::BlockSize> paddedKey {};
    if (key.size() > Sha256::BlockSize)
    {
        auto const hashed = Sha256::Hash(key);
        std::ranges::copy(hashed, paddedKey.begin());
    }
    else
        std::ranges::copy(key, paddedKey.begin());

    std::array<std::byte, Sha256::BlockSize> innerPad {};
    std::array<std::byte, Sha256::BlockSize> outerPad {};
    for (auto const index: std::views::iota(std::size_t { 0 }, Sha256::BlockSize))
    {
        innerPad[index] = paddedKey[index] ^ std::byte { 0x36 };
        outerPad[index] = paddedKey[index] ^ std::byte { 0x5c };
    }

    Sha256 inner;
    inner.Update(innerPad);
    inner.Update(message);
    auto const innerDigest = inner.Finish();

    Sha256 outer;
    outer.Update(outerPad);
    outer.Update(innerDigest);
    return outer.Finish();
}

bool ConstantTimeEquals(Sha256::Digest const& lhs, Sha256::Digest const& rhs) noexcept
{
    // Every byte is examined whatever the outcome, and the result is folded into
    // one accumulator rather than short-circuited. `volatile` on the accumulator
    // is what keeps a compiler from noticing it may stop early once a difference
    // is seen -- the optimisation is legal and would reintroduce the timing
    // channel this exists to remove.
    std::uint8_t volatile difference = 0;
    for (auto const index: std::views::iota(std::size_t { 0 }, Sha256::DigestSize))
        difference = static_cast<std::uint8_t>(difference | std::to_integer<std::uint8_t>(lhs[index] ^ rhs[index]));
    return difference == 0;
}

std::string HexDigest(Sha256::Digest const& digest)
{
    constexpr std::string_view Digits = "0123456789abcdef";

    std::string out;
    out.reserve(Sha256::DigestSize * 2);
    for (auto const byte: digest)
    {
        auto const value = std::to_integer<std::uint8_t>(byte);
        out.push_back(Digits[value >> 4U]);
        out.push_back(Digits[value & 0x0FU]);
    }
    return out;
}

} // namespace FastCache
