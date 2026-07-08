// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Compression.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// Turn a string literal into an owning byte vector for round-trip tests.
std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto const c: text)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

/// Deterministic pseudo-random (incompressible) bytes.
std::vector<std::byte> RandomBytes(std::size_t size, std::uint64_t seed)
{
    std::mt19937_64 rng { seed };
    std::vector<std::byte> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(rng() & 0xFFU)));
    return out;
}

/// A highly compressible buffer (long repeated run).
std::vector<std::byte> CompressibleBytes(std::size_t size)
{
    return std::vector<std::byte>(size, std::byte { 0x41 });
}

/// Every codec the test exercises, so the round-trip cases stay data-driven.
constexpr std::array<CompressionCodec, 3> AllCodecs { CompressionCodec::Identity,
                                                      CompressionCodec::Lz4,
                                                      CompressionCodec::Zstd };

} // namespace

TEST_CASE("Compression: names map to ids and back", "[compression]")
{
    CHECK(Compression::NameOf(CompressionCodec::Identity) == "none");
    CHECK(Compression::NameOf(CompressionCodec::Lz4) == "lz4");
    CHECK(Compression::NameOf(CompressionCodec::Zstd) == "zstd");

    CHECK(Compression::CodecFromName("none") == CompressionCodec::Identity);
    CHECK(Compression::CodecFromName("lz4") == CompressionCodec::Lz4);
    CHECK(Compression::CodecFromName("zstd") == CompressionCodec::Zstd);
    CHECK_FALSE(Compression::CodecFromName("brotli").has_value());
}

TEST_CASE("Compression: Identity is always available", "[compression]")
{
    CHECK(Compression::IsAvailable(CompressionCodec::Identity));
}

TEST_CASE("Compression: round-trips every available codec", "[compression]")
{
    std::vector<std::vector<std::byte>> const inputs {
        Bytes(""),
        Bytes("x"),
        Bytes("the quick brown fox jumps over the lazy dog, again and again and again"),
        CompressibleBytes(64 * 1024),
        RandomBytes(4096, 0xC0FFEE),
    };

    for (auto const codec: AllCodecs)
    {
        if (!Compression::IsAvailable(codec))
            continue;
        for (auto const& input: inputs)
        {
            auto const compressed = Compression::Compress(codec, input, 3);
            auto const restored = Compression::Decompress(codec, compressed, input.size());
            REQUIRE(restored.has_value());
            CHECK(*restored == input);
        }
    }
}

TEST_CASE("Compression: Identity stores bytes verbatim", "[compression]")
{
    auto const input = Bytes("verbatim payload");
    auto const compressed = Compression::Compress(CompressionCodec::Identity, input, 3);
    CHECK(compressed == input);
}

TEST_CASE("Compression: compressible data shrinks under zstd/lz4", "[compression]")
{
    auto const input = CompressibleBytes(64 * 1024);
    for (auto const codec: { CompressionCodec::Lz4, CompressionCodec::Zstd })
    {
        if (!Compression::IsAvailable(codec))
            continue;
        auto const compressed = Compression::Compress(codec, input, 3);
        CHECK(compressed.size() < input.size());
    }
}

TEST_CASE("Compression: decompress rejects a wrong original length", "[compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        return;
    auto const input = Bytes("some payload that will be zstd-framed");
    auto const compressed = Compression::Compress(CompressionCodec::Zstd, input, 3);

    // Claiming a length larger than the true one must fail rather than
    // over-allocate or return a short buffer.
    auto const wrong = Compression::Decompress(CompressionCodec::Zstd, compressed, input.size() + 100);
    CHECK_FALSE(wrong.has_value());
    if (!wrong.has_value())
        CHECK(wrong.error().code == StorageErrorCode::Corrupt);
}

TEST_CASE("Compression: decompress rejects corrupt input", "[compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        return;
    auto const input = CompressibleBytes(1024);
    auto compressed = Compression::Compress(CompressionCodec::Zstd, input, 3);
    REQUIRE(compressed.size() > 4);
    // Flip bytes in the middle of the frame to corrupt it.
    compressed[compressed.size() / 2] ^= std::byte { 0xFF };
    compressed[(compressed.size() / 2) + 1] ^= std::byte { 0xFF };

    auto const restored = Compression::Decompress(CompressionCodec::Zstd, compressed, input.size());
    // Either the frame fails to decode, or (unlikely) it decodes to the wrong
    // length — both are reported as Corrupt, never a silent wrong result.
    if (restored.has_value())
        CHECK(*restored == input); // decoded cleanly despite the flip (rare)
    else
        CHECK(restored.error().code == StorageErrorCode::Corrupt);
}
