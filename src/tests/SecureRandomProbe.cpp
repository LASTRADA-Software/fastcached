// SPDX-License-Identifier: Apache-2.0
//
// Draws handshake nonces and prints them, so a gate can ask whether two PROCESSES ever draw
// the same one.
//
// [#1527](https://github.com/LASTRADA-Software/fastcached/issues/1527) is a defect no
// in-process test can see. Nonces were drawn from an engine seeded ONCE per process from
// `std::random_device`, and on the host #1507 was measured on that device answers zero for 57%
// of draws -- so a process there seeds the engine with 0 about a third of the time (inferred:
// 0.57 squared), and every such process draws the SAME nonce stream. Inside one process the
// stream never repeats, so every in-process "two nonces differ" check passes on the broken
// build. The property that failed is ACROSS processes, and asking it needs processes.
//
// `scripts/secure-random-gate.cmake` runs this several times and refuses a repeat. It also runs
// the `seeded-engine` control, which draws through the pre-#1527 construction with the seed
// the broken host produced -- and requires the SAME repeat detector to fire on it, because a
// detector nobody has watched refuse is not known to work.
//
// Usage:
//   secure-random-probe --source=os|seeded-engine --count=<n>
// Prints one `nonce <64 hex>` line per draw. Exits 0 when every draw succeeded, 2 when a draw
// failed (naming the failure), 64 for a usage error.

#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace
{

/// The control: nonce bytes from an engine seeded ZERO, laid out as they were before #1527.
///
/// An `ISecureRandom` in name only, and deliberately so: it is the construction #1527 removed
/// -- `SystemRandomSource` draws written big-endian -- given the seed a host whose
/// `std::random_device` answers zero hands it. Routed through the real `DrawNonce` so the only
/// thing the control changes is the SOURCE. It exists here and nowhere else.
class SeededEngineSource final: public FastCache::ISecureRandom
{
  public:
    /// @copydoc FastCache::ISecureRandom::Fill
    [[nodiscard]] std::expected<void, FastCache::SecureRandomError> Fill(std::span<std::byte> out) override
    {
        auto draw = std::uint64_t { 0 };
        auto remaining = std::size_t { 0 };
        for (auto& byte: out)
        {
            if (remaining == 0)
            {
                draw = _engine.UniformInRange(0, std::numeric_limits<std::uint64_t>::max());
                remaining = sizeof(std::uint64_t);
            }
            --remaining;
            byte = static_cast<std::byte>((draw >> (remaining * 8U)) & 0xFFU);
        }
        return {};
    }

  private:
    FastCache::SystemRandomSource _engine { 0 };
};

/// Lowercase hex.
constexpr std::string_view HexDigits = "0123456789abcdef";

} // namespace

int main(int argc, char** argv)
{
    auto source = std::string_view {};
    auto count = std::size_t { 0 };
    for (std::string_view const argument: std::span { argv, static_cast<std::size_t>(argc) }.subspan(1))
    {
        if (argument.starts_with("--source="))
            source = argument.substr(std::string_view { "--source=" }.size());
        else if (argument.starts_with("--count="))
        {
            for (auto const digit: argument.substr(std::string_view { "--count=" }.size()))
            {
                if (digit < '0' || digit > '9')
                {
                    std::println(stderr, "secure-random-probe: --count takes a number, not {}", argument);
                    return 64;
                }
                count = (count * 10) + static_cast<std::size_t>(digit - '0');
            }
        }
        else
        {
            std::println(stderr, "secure-random-probe: unknown argument {}", argument);
            return 64;
        }
    }

    std::unique_ptr<FastCache::ISecureRandom> random;
    if (source == "os")
        random = std::make_unique<FastCache::SystemSecureRandom>();
    else if (source == "seeded-engine")
        random = std::make_unique<SeededEngineSource>();
    else
    {
        std::println(stderr, "secure-random-probe: --source is os or seeded-engine, not '{}'", source);
        return 64;
    }

    for ([[maybe_unused]] auto const draw: std::views::iota(std::size_t { 0 }, count))
    {
        auto const nonce = FastCache::DrawNonce(*random);
        if (!nonce.has_value())
        {
            std::println(stderr, "secure-random-probe: a draw failed: {}", nonce.error().ToString());
            return 2;
        }

        std::string hex;
        hex.reserve(nonce->size() * 2);
        for (auto const byte: *nonce)
        {
            auto const value = std::to_integer<unsigned>(byte);
            hex.push_back(HexDigits[value >> 4U]);
            hex.push_back(HexDigits[value & 0xFU]);
        }
        std::println("nonce {}", hex);
    }
    return 0;
}
