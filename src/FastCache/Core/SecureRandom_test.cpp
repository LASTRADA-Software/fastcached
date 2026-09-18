// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ISecureRandom.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <vector>

#include <tests/SecureRandomFakes.hpp>

using namespace FastCache;
using FastCache::Testing::ScriptedSecureRandom;

namespace
{

/// Whether any byte of @p bytes is not zero.
/// @param bytes The bytes.
/// @return True when at least one is non-zero.
[[nodiscard]] bool AnyNonZero(std::span<std::byte const> bytes)
{
    return std::ranges::any_of(bytes, [](std::byte byte) { return byte != std::byte { 0 }; });
}

} // namespace

TEST_CASE("The operating system's generator answers two draws that differ and neither is zero", "[core][securerandom]")
{
    // 32 bytes each. A generator answering a constant -- which is what an engine seeded from
    // a `std::random_device` answering zero amounts to -- fails the first check; one that
    // filled nothing fails the second. Either by chance is 2^-256.
    SystemSecureRandom random;
    std::array<std::byte, 32> first {};
    std::array<std::byte, 32> second {};

    REQUIRE(random.Fill(first).has_value());
    REQUIRE(random.Fill(second).has_value());

    CHECK(first != second);
    CHECK(AnyNonZero(first));
    CHECK(AnyNonZero(second));
}

TEST_CASE("A draw longer than one call of the primitive fills every part of it", "[core][securerandom]")
{
    // `getentropy` fills at most 256 bytes a call, so a longer draw is several calls -- and a
    // loop that filled the first chunk and returned would leave the rest as the caller left it.
    // Each 256-byte window is asked separately; any one of them all-zero by chance is 2^-2048.
    SystemSecureRandom random;
    std::vector<std::byte> bytes(1024, std::byte { 0 });

    REQUIRE(random.Fill(bytes).has_value());

    for (auto const window: std::views::iota(std::size_t { 0 }, bytes.size() / 256))
        CHECK(AnyNonZero(std::span<std::byte const> { bytes }.subspan(window * 256, 256)));
}

TEST_CASE("An empty draw succeeds", "[core][securerandom]")
{
    // A zero-length request is not a failure. Its other half -- that the loop does not spin
    // asking a primitive for zero bytes -- is this case finishing at all.
    SystemSecureRandom random;
    CHECK(random.Fill(std::span<std::byte> {}).has_value());
}

TEST_CASE("The scripted generator serves its script in order across draws, and cycles", "[core][securerandom]")
{
    // The fake is a shared helper, and a wrong one makes its cases pass: every nonce and id
    // case reads its bytes through this, so the order it serves them in is asserted here.
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(6) };
    std::array<std::byte, 4> first {};
    std::array<std::byte, 4> second {};

    REQUIRE(random.Fill(first).has_value());
    REQUIRE(random.Fill(second).has_value());

    CHECK(first == std::array { std::byte { 0 }, std::byte { 1 }, std::byte { 2 }, std::byte { 3 } });
    CHECK(second == std::array { std::byte { 4 }, std::byte { 5 }, std::byte { 0 }, std::byte { 1 } });
    CHECK(random.FillCount() == 2);
}

TEST_CASE("The scripted generator's failure mode refuses every draw and writes nothing", "[core][securerandom]")
{
    ScriptedSecureRandom random { ScriptedSecureRandom::DeniedFailure() };
    std::array<std::byte, 4> out { std::byte { 0xAA }, std::byte { 0xAA }, std::byte { 0xAA }, std::byte { 0xAA } };

    auto const drawn = random.Fill(out);

    REQUIRE_FALSE(drawn.has_value());
    CHECK(drawn.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);
    CHECK(out == std::array { std::byte { 0xAA }, std::byte { 0xAA }, std::byte { 0xAA }, std::byte { 0xAA } });
    CHECK(random.FillCount() == 1);
}
