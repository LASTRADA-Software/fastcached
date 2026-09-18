// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/CpuFeatures.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/Sha256.hpp>

#if defined(_M_X64) || defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
#endif

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
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

    /// The eight working variables an engine compresses into.
    using State = std::array<std::uint32_t, 8>;

    /// FIPS 180-4 §6.2.2, one block at a time, spelled as the standard spells it.
    /// The `Scalar` engine, and the reference the others are checked against.
    /// @param state The running state.
    /// @param blocks A whole number of 64-byte blocks.
    void CompressBlocksScalar(State& state, std::span<std::byte const> blocks) noexcept
    {
        for (auto const blockIndex: std::views::iota(std::size_t { 0 }, blocks.size() / Sha256::BlockSize))
        {
            auto const block = blocks.subspan(blockIndex * Sha256::BlockSize, Sha256::BlockSize);
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

            auto working = state;

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

            for (auto const index: std::views::iota(std::size_t { 0 }, state.size()))
                state[index] += working[index];
        }
    }

    /// An engine's code: compress whole blocks into the running state.
    using CompressBlocksFunction = void (*)(State& state, std::span<std::byte const> blocks) noexcept;

#if defined(_M_X64) || defined(__x86_64__)
    // Asked for per function, never by a global -m flag, for the reason
    // .agent/rules/build-and-toolchain.md gives under instruction-set extensions. cl
    // needs no attribute.
    #if defined(__GNUC__) || defined(__clang__)
        #define FASTCACHED_SHA_NI_TARGET __attribute__((target("sha,ssse3,sse4.1")))
    #else
        #define FASTCACHED_SHA_NI_TARGET
    #endif

    /// One of a block's four message vectors, in the byte order SHA-256 reads.
    /// @param block The 64-byte block.
    /// @param index Which of its four 16-byte words.
    /// @return The words as one vector.
    [[nodiscard]] FASTCACHED_SHA_NI_TARGET __m128i LoadWordsX86(std::span<std::byte const> block, std::size_t index) noexcept
    {
        // SHA-256 reads its words big-endian; x86 loads them little-endian.
        __m128i const byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
        __m128i words {};
        std::memcpy(&words, block.subspan(index * 16, 16).data(), sizeof(words));
        return _mm_shuffle_epi8(words, byteSwap);
    }

    /// The x86 SHA extensions, in Intel's SHA-NI construction. `_mm_sha256rnds2_epu32`
    /// performs two rounds, the message expansion is `sha256msg1` then `sha256msg2`,
    /// and the state is held as the two lanes that instruction wants, ABEF and CDGH.
    /// @param state The running state.
    /// @param blocks A whole number of 64-byte blocks.
    FASTCACHED_SHA_NI_TARGET void CompressBlocksX86ShaNi(State& state, std::span<std::byte const> blocks) noexcept
    {
        __m128i high {};
        __m128i low {};
        std::memcpy(&high, state.data(), sizeof(high));
        std::memcpy(&low, std::span { state }.subspan(4).data(), sizeof(low));
        high = _mm_shuffle_epi32(high, 0xB1); // CDAB
        low = _mm_shuffle_epi32(low, 0x1B);   // HGFE
        __m128i abef = _mm_alignr_epi8(high, low, 8);
        __m128i cdgh = _mm_blend_epi16(low, high, 0xF0);

        for (auto const blockIndex: std::views::iota(std::size_t { 0 }, blocks.size() / Sha256::BlockSize))
        {
            auto const block = blocks.subspan(blockIndex * Sha256::BlockSize, Sha256::BlockSize);

            // The message schedule as a window of four vectors (four words each): the
            // block's own sixteen words first, then each vector expanded from the four
            // before it, as the window slides.
            auto w0 = LoadWordsX86(block, 0);
            auto w1 = LoadWordsX86(block, 1);
            auto w2 = LoadWordsX86(block, 2);
            auto w3 = LoadWordsX86(block, 3);

            auto const savedAbef = abef;
            auto const savedCdgh = cdgh;
            for (auto const index: std::views::iota(std::size_t { 0 }, RoundConstants.size() / 4))
            {
                __m128i constants {};
                std::memcpy(&constants, std::span { RoundConstants }.subspan(index * 4, 4).data(), sizeof(constants));
                auto const message = _mm_add_epi32(w0, constants);
                cdgh = _mm_sha256rnds2_epu32(cdgh, abef, message);
                abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(message, 0x0E));

                auto const next =
                    _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w0, w1), _mm_alignr_epi8(w3, w2, 4)), w3);
                w0 = w1;
                w1 = w2;
                w2 = w3;
                w3 = next;
            }
            abef = _mm_add_epi32(abef, savedAbef);
            cdgh = _mm_add_epi32(cdgh, savedCdgh);
        }

        auto const feba = _mm_shuffle_epi32(abef, 0x1B);
        auto const dchg = _mm_shuffle_epi32(cdgh, 0xB1);
        high = _mm_blend_epi16(feba, dchg, 0xF0); // DCBA
        low = _mm_alignr_epi8(dchg, feba, 8);     // HGFE
        std::memcpy(state.data(), &high, sizeof(high));
        std::memcpy(std::span { state }.subspan(4).data(), &low, sizeof(low));
    }

    #undef FASTCACHED_SHA_NI_TARGET

    constexpr CompressBlocksFunction X86ShaNiBlocks = &CompressBlocksX86ShaNi;
#else
    constexpr CompressBlocksFunction X86ShaNiBlocks = nullptr;
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    // Asked for per function, for the x86 engine's reason -- and asked for even where the
    // default target enables sha2, as Apple's does: an explicit -march=armv8-a refuses the
    // intrinsics otherwise (.agent/rules/build-and-toolchain.md, the same entry). The two
    // GNU-family spellings differ: GCC's aarch64 attribute takes an extension as `+sha2`,
    // and clang takes the feature name. cl needs no attribute, since its ARM64 intrinsics
    // are always declared and the instruction is chosen by this function's caller.
    #if defined(__clang__)
        #define FASTCACHED_ARM_SHA2_TARGET __attribute__((target("sha2")))
    #elif defined(__GNUC__)
        #define FASTCACHED_ARM_SHA2_TARGET __attribute__((target("+sha2")))
    #else
        #define FASTCACHED_ARM_SHA2_TARGET
    #endif

    /// One of a block's four message vectors, in the byte order SHA-256 reads.
    /// @param block The 64-byte block.
    /// @param index Which of its four 16-byte words.
    /// @return The words as one vector.
    [[nodiscard]] FASTCACHED_ARM_SHA2_TARGET uint32x4_t LoadWordsArm(std::span<std::byte const> block,
                                                                     std::size_t index) noexcept
    {
        std::array<std::uint8_t, 16> bytes {};
        std::memcpy(bytes.data(), block.subspan(index * 16, 16).data(), bytes.size());
        return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(bytes.data())));
    }

    /// The ARMv8 SHA-256 instructions. `vsha256hq_u32` and `vsha256h2q_u32` perform
    /// four rounds between them, and the message expansion is `sha256su0` then
    /// `sha256su1`. Compiled only where a CI leg builds it (see `CpuFeatures`).
    /// @param state The running state.
    /// @param blocks A whole number of 64-byte blocks.
    FASTCACHED_ARM_SHA2_TARGET void CompressBlocksArmSha2(State& state, std::span<std::byte const> blocks) noexcept
    {
        uint32x4_t abcd = vld1q_u32(state.data());
        uint32x4_t efgh = vld1q_u32(std::span { state }.subspan(4).data());

        for (auto const blockIndex: std::views::iota(std::size_t { 0 }, blocks.size() / Sha256::BlockSize))
        {
            auto const block = blocks.subspan(blockIndex * Sha256::BlockSize, Sha256::BlockSize);

            // The same window of four message vectors as the x86 engine's.
            auto w0 = LoadWordsArm(block, 0);
            auto w1 = LoadWordsArm(block, 1);
            auto w2 = LoadWordsArm(block, 2);
            auto w3 = LoadWordsArm(block, 3);

            auto const savedAbcd = abcd;
            auto const savedEfgh = efgh;
            for (auto const index: std::views::iota(std::size_t { 0 }, RoundConstants.size() / 4))
            {
                auto const message = vaddq_u32(w0, vld1q_u32(std::span { RoundConstants }.subspan(index * 4, 4).data()));
                auto const abcdBefore = abcd;
                abcd = vsha256hq_u32(abcd, efgh, message);
                efgh = vsha256h2q_u32(efgh, abcdBefore, message);

                auto const next = vsha256su1q_u32(vsha256su0q_u32(w0, w1), w2, w3);
                w0 = w1;
                w1 = w2;
                w2 = w3;
                w3 = next;
            }
            abcd = vaddq_u32(abcd, savedAbcd);
            efgh = vaddq_u32(efgh, savedEfgh);
        }

        vst1q_u32(state.data(), abcd);
        vst1q_u32(std::span { state }.subspan(4).data(), efgh);
    }

    #undef FASTCACHED_ARM_SHA2_TARGET

    constexpr CompressBlocksFunction ArmSha2Blocks = &CompressBlocksArmSha2;
#else
    constexpr CompressBlocksFunction ArmSha2Blocks = nullptr;
#endif

    /// One engine: what it needs of the CPU, and its code, which is null where this build
    /// does not carry it.
    struct EngineRow
    {
        Sha256Engine engine;
        std::string_view name;
        bool (*needs)(CpuFeatures const& features) noexcept;
        CompressBlocksFunction compressBlocks;
    };

    constexpr EnumTable<Sha256Engine, EngineRow> EngineRows { {
        { .engine = Sha256Engine::Scalar,
          .name = "scalar",
          .needs = [](CpuFeatures const&) noexcept { return true; },
          .compressBlocks = &CompressBlocksScalar },
        { .engine = Sha256Engine::X86ShaNi,
          .name = "x86-sha-ni",
          .needs =
              [](CpuFeatures const& features) noexcept { return features.x86Sha && features.x86Ssse3 && features.x86Sse41; },
          .compressBlocks = X86ShaNiBlocks },
        { .engine = Sha256Engine::ArmSha2,
          .name = "arm-sha2",
          .needs = [](CpuFeatures const& features) noexcept { return features.armSha2; },
          .compressBlocks = ArmSha2Blocks },
    } };
    static_assert(RowsInEnumeratorOrder(EngineRows, &EngineRow::engine));

    /// The row for @p engine, which every engine-taking function reaches the table through.
    /// @param engine The engine; `Last` is a count, not an engine, and passing it is a programmer error.
    /// @return Its row.
    [[nodiscard]] EngineRow const& RowOf(Sha256Engine engine) noexcept
    {
        assert(engine < Sha256Engine::Last);
        return EngineRows[static_cast<std::size_t>(engine)];
    }

    /// This process's CPU, read once, which `CpuFeatures` says is safe.
    /// @return What the CPU offers.
    [[nodiscard]] CpuFeatures const& ProcessCpuFeatures() noexcept
    {
        static CpuFeatures const features = DetectCpuFeatures();
        return features;
    }
} // namespace

std::string_view Sha256EngineName(Sha256Engine engine) noexcept
{
    return RowOf(engine).name;
}

bool Sha256EngineRunsOn(Sha256Engine engine, CpuFeatures const& features) noexcept
{
    auto const& row = RowOf(engine);
    return row.compressBlocks != nullptr && row.needs(features);
}

Sha256Engine SelectSha256Engine(CpuFeatures const& features) noexcept
{
    // Any hardware engine beats Scalar, and a build carries at most one.
    auto const* const hardware = FindIfOrNull(std::span { EngineRows }.subspan(1), [&features](EngineRow const& row) {
        return Sha256EngineRunsOn(row.engine, features);
    });
    return hardware != nullptr ? hardware->engine : Sha256Engine::Scalar;
}

Sha256Engine ActiveSha256Engine() noexcept
{
    static Sha256Engine const active = SelectSha256Engine(ProcessCpuFeatures());
    return active;
}

Sha256::Sha256(Sha256Engine engine) noexcept:
    _engine { engine }
{
}

std::optional<Sha256> Sha256::WithEngine(Sha256Engine engine) noexcept
{
    if (!Sha256EngineRunsOn(engine, ProcessCpuFeatures()))
        return std::nullopt;
    return Sha256 { engine };
}

void Sha256::CompressBlocks(std::span<std::byte const> blocks) noexcept
{
    RowOf(_engine).compressBlocks(_state, blocks);
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

        CompressBlocks(_pending);
        _pendingSize = 0;
    }

    // Every whole block in one call, so an engine keeps its state in its own form
    // across the run rather than converting it back per block.
    auto const whole = input.size() - (input.size() % BlockSize);
    if (whole != 0)
    {
        CompressBlocks(input.first(whole));
        input = input.subspan(whole);
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
        CompressBlocks(_pending);
        _pendingSize = 0;
    }

    std::ranges::fill(std::span { _pending }.subspan(_pendingSize, BlockSize - 8 - _pendingSize), std::byte { 0 });
    WriteBigEndian<std::uint64_t>(std::span { _pending }.subspan(BlockSize - 8, 8), bitLength);
    CompressBlocks(_pending);

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
        auto hashed = Sha256::Hash(key);
        std::ranges::copy(hashed, paddedKey.begin());
        SecureZero(hashed.data(), hashed.size());
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
    auto const tag = outer.Finish();

    // The three pads ARE the key, XORed with a public constant, so they are wiped with it in
    // mind: HKDF (`Core/Hkdf`) runs a derived secret through here as the key, and a copy left on
    // the stack is that secret, recoverable from the constant. `SecureZero` rather than a plain
    // store, which a compiler is entitled to delete from storage about to die.
    //
    // Not wiped, and why: the hashers need nothing. A whole-block pad is compressed straight from
    // this frame's array without being copied into a hasher's pending block, and the key-equivalent
    // state after that first block is overwritten by every block that follows it. What remains is
    // the compression function's own stack frame, its message schedule, which no caller reaches.
    SecureZero(paddedKey.data(), paddedKey.size());
    SecureZero(innerPad.data(), innerPad.size());
    SecureZero(outerPad.data(), outerPad.size());
    return tag;
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
