// SPDX-License-Identifier: Apache-2.0
/// SHA-256 throughput per engine, which is what
/// [#1420](https://github.com/LASTRADA-Software/fastcached/issues/1420) is a claim about.
///
/// Every HMAC in this tree goes through `Sha256`: discovery proofs, lease grants, cluster
/// signing, and on #1308's branch every Raft peer frame, sealed and opened on the reactor
/// thread. There the digest was measured at about 98% of a seal, so the engine is the
/// lever and this measures it directly.
///
/// **Each engine is named in its benchmark**, and one this CPU cannot run is reported as
/// skipped by name, never silently left out. A table that listed only the engines that
/// ran would read identically to one where the hardware engine was never tried.
///
/// A figure here is a quantity under conditions. Whoever quotes one states the build,
/// the host, its load and the sample count beside it.
///
/// `HmacSha256` is deliberately not measured here: outside `Core/Sha256` and the one
/// signing seam, naming it is a second signing construction
/// (scripts/check-psk-signing-seam.cmake). An HMAC is two hashes over the message's
/// length, so the per-engine hash rate is the figure that transfers.

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;

namespace
{

/// The payload sizes measured: a short control message, a page, and the two sizes the
/// Raft frame figures in #1420 were taken at (1 MiB, and the 8 MiB frame cap).
constexpr std::array<std::size_t, 4> PayloadSizes { 64, 4096, std::size_t { 1 } << 20U, std::size_t { 8 } << 20U };

/// @p payload's digest, on a copy of @p hasher, so every call starts from a fresh one.
/// @param hasher A fresh hasher on the engine to measure.
/// @param payload Bytes to hash.
/// @return The digest.
[[nodiscard]] Sha256::Digest DigestOf(Sha256 hasher, std::span<std::byte const> payload) noexcept
{
    hasher.Update(payload);
    return hasher.Finish();
}

} // namespace

TEST_CASE("bench: Sha256 digest per engine", "[!benchmark][sha256]")
{
    std::cout << std::format("sha256 bench: default engine {}\n", Sha256EngineName(ActiveSha256Engine()));

    for (auto const size: PayloadSizes)
    {
        // Not a repeated byte: an input that is all one value is the case an engine that
        // mishandled its message schedule could still get right.
        std::vector<std::byte> payload(size);
        for (auto const index: std::views::iota(std::size_t { 0 }, size))
            payload[index] = static_cast<std::byte>(((index * 131U) + 7U) & 0xFFU);

        // Scalar is the reference: it comes first and runs everywhere. Were it ever not
        // first, every engine's check below would fail rather than pass unchecked.
        std::optional<Sha256::Digest> reference;
        for (auto const engineIndex: std::views::iota(std::size_t { 0 }, EnumeratorCount<Sha256Engine>))
        {
            auto const engine = static_cast<Sha256Engine>(engineIndex);
            auto const name = std::format("{} {} bytes", Sha256EngineName(engine), size);
            auto const hasher = Sha256::WithEngine(engine);
            if (!hasher.has_value())
            {
                std::cout << std::format("sha256 bench: {} skipped, not supported by this CPU\n", name);
                continue;
            }

            // Chosen once, here, so the timing below is the hash and not the choice.
            Sha256 const prototype = *hasher;
            auto const digest = DigestOf(prototype, payload);
            if (engine == Sha256Engine::Scalar)
                reference = digest;

            // The positive control: an engine that digests wrongly would report a rate for
            // a result nobody can use.
            REQUIRE(reference == digest);

            BENCHMARK(std::string { name })
            {
                return DigestOf(prototype, payload);
            };
        }
    }
}
