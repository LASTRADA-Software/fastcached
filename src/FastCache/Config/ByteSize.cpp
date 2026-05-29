// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace FastCache
{

namespace
{

    [[nodiscard]] ConfigError MakeError(ConfigErrorCode code, std::string_view field, std::string context)
    {
        return ConfigError {
            .code = code,
            .source = "",
            .line = 0,
            .field = std::string { field },
            .context = std::move(context),
        };
    }

    /// Decode a single trailing unit character into a 1024-based multiplier.
    /// Returns 0 to signal "unknown suffix"; 1 means "no suffix".
    [[nodiscard]] constexpr std::uint64_t SuffixMultiplier(char c) noexcept
    {
        switch (c)
        {
            case 'k':
            case 'K':
                return 1024ULL;
            case 'm':
            case 'M':
                return 1024ULL * 1024ULL;
            case 'g':
            case 'G':
                return 1024ULL * 1024ULL * 1024ULL;
            default:
                return 0;
        }
    }

} // namespace

std::expected<std::size_t, ConfigError> ParseByteSize(std::string_view sv,
                                                      std::string_view field,
                                                      std::size_t hostTotalBytes)
{
    if (sv.empty())
        return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, field, "empty value"));

    auto const tail = sv.back();

    // Percentage of host total memory.
    if (tail == '%')
    {
        auto const digits = sv.substr(0, sv.size() - 1);
        if (digits.empty())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, field, "percent without digits"));

        auto pct = std::uint64_t { 0 };
        auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), pct);
        if (ec != std::errc {} || ptr != digits.data() + digits.size())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, field, std::format("not a number: {}", sv)));

        if (pct > 100)
            return std::unexpected(
                MakeError(ConfigErrorCode::OutOfRange, field, std::format("percentage out of range 0..100: {}", sv)));

        if (hostTotalBytes == 0)
            return std::unexpected(
                MakeError(ConfigErrorCode::TypeMismatch, field, "percent-of-host requested but host memory is unknown"));

        return static_cast<std::size_t>((static_cast<std::uint64_t>(hostTotalBytes) * pct) / 100U);
    }

    auto multiplier = std::uint64_t { 1 };
    auto digits = sv;
    if (tail < '0' || tail > '9')
    {
        multiplier = SuffixMultiplier(tail);
        if (multiplier == 0)
            return std::unexpected(
                MakeError(ConfigErrorCode::TypeMismatch, field, std::format("unknown unit suffix: '{}'", tail)));
        digits = sv.substr(0, sv.size() - 1);
        if (digits.empty())
            return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, field, "suffix without digits"));
    }

    auto raw = std::uint64_t { 0 };
    auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), raw);
    if (ec != std::errc {} || ptr != digits.data() + digits.size())
        return std::unexpected(MakeError(ConfigErrorCode::TypeMismatch, field, std::format("not a number: {}", sv)));

    constexpr auto SizeMax = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    if (multiplier > 1 && raw > SizeMax / multiplier)
        return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, field, std::format("value overflows size_t: {}", sv)));

    auto const product = raw * multiplier;
    if (product > SizeMax)
        return std::unexpected(MakeError(ConfigErrorCode::OutOfRange, field, std::format("value overflows size_t: {}", sv)));

    return static_cast<std::size_t>(product);
}

std::string FormatByteSize(std::size_t bytes)
{
    constexpr std::size_t KiB = 1024;
    constexpr std::size_t MiB = 1024 * KiB;
    constexpr std::size_t GiB = 1024 * MiB;

    if (bytes >= GiB && bytes % GiB == 0)
        return std::format("{}G", bytes / GiB);
    if (bytes >= MiB && bytes % MiB == 0)
        return std::format("{}M", bytes / MiB);
    if (bytes >= KiB && bytes % KiB == 0)
        return std::format("{}K", bytes / KiB);
    return std::format("{}B", bytes);
}

} // namespace FastCache
