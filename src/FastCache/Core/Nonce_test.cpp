// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>

#include <tests/SecureRandomFakes.hpp>

using namespace FastCache;
using FastCache::Testing::ScriptedSecureRandom;

TEST_CASE("A nonce is the seam's bytes, in order, from one draw", "[core][nonce]")
{
    // Bytes whose every value differs, so a nonce filled from anywhere else -- a zeroed array,
    // an engine, the bytes out of order -- shows as a byte out of place. This is what proves
    // the nonce comes from `ISecureRandom` and from nothing beside it (#1527).
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(NonceBytes) };

    auto const nonce = DrawNonce(random);

    REQUIRE(nonce.has_value());
    CHECK(random.FillCount() == 1);
    for (std::size_t index = 0; auto const byte: nonce.value())
        CHECK(byte == static_cast<std::byte>(index++));
}

TEST_CASE("A nonce the seam cannot draw is a refusal carrying the seam's own failure", "[core][nonce]")
{
    // No fallback: the draw was ASKED for, it failed, and what comes back is that failure --
    // not a nonce from a clock, an engine or `std::random_device`. Asserting the fake's own
    // primitive name is what tells this refusal apart from one produced anywhere else.
    ScriptedSecureRandom random { ScriptedSecureRandom::DeniedFailure() };

    auto const nonce = DrawNonce(random);

    REQUIRE_FALSE(nonce.has_value());
    CHECK(random.FillCount() == 1);
    CHECK(nonce.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);
    CHECK(nonce.error().ToString().contains("Operation not permitted"));
}

TEST_CASE("Two nonces from the operating system's generator are two nonces", "[core][nonce]")
{
    // The property a nonce is FOR is that it does not repeat. Within one process only; the
    // property that failed in #1527 was ACROSS processes, which `secure-random-cross-process`
    // asks, because an engine seeded once per process passes this case whatever its seed was.
    SystemSecureRandom random;

    auto const first = DrawNonce(random);
    auto const second = DrawNonce(random);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value() != second.value());
    CHECK(std::ranges::any_of(first.value(), [](std::byte byte) { return byte != std::byte { 0 }; }));
}
