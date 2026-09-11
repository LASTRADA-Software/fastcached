// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CompressionValues.hpp>

#include <charconv>
#include <format>
#include <string>
#include <system_error>

namespace FastCache
{

namespace
{
    /// Build an error that names no field and no source; see the header.
    /// @param code Error category.
    /// @param context Free-form explanation.
    /// @return The populated error.
    [[nodiscard]] ConfigError ValueError(ConfigErrorCode code, std::string context)
    {
        return ConfigError { .code = code, .source = {}, .line = 0, .field = {}, .context = std::move(context) };
    }
} // namespace

std::expected<CompressionCodec, ConfigError> ParseCompressionCodec(std::string_view sv)
{
    auto const codec = Compression::CodecFromName(sv);
    if (!codec.has_value())
        return std::unexpected(ValueError(ConfigErrorCode::OutOfRange,
                                          std::format("unknown codec (expect {}): {}", Compression::NameList(), sv)));

    // A codec this build lacks is recognised by name and refused anyway: the name
    // is a real one, so "unknown codec" would send an operator looking for a typo
    // that is not there.
    if (!Compression::IsAvailable(*codec))
        return std::unexpected(ValueError(
            ConfigErrorCode::OutOfRange,
            std::format("codec '{}' is not available in this build (rebuild with FASTCACHED_ENABLE_COMPRESSION)", sv)));

    return *codec;
}

std::expected<int, ConfigError> ParseCompressionLevel(std::string_view sv)
{
    if (sv.empty())
        return std::unexpected(ValueError(ConfigErrorCode::TypeMismatch, "empty value"));

    std::size_t value = 0;
    auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc {} || ptr != sv.data() + sv.size())
        return std::unexpected(ValueError(ConfigErrorCode::TypeMismatch, std::format("not a number: {}", sv)));

    if (value == 0 || value > 22)
        return std::unexpected(ValueError(ConfigErrorCode::OutOfRange, std::format("out of range (1..22): {}", value)));

    return static_cast<int>(value);
}

std::expected<std::size_t, ConfigError> ParseCompressionMinBytes(std::string_view sv)
{
    return ParseByteSize(sv, {});
}

} // namespace FastCache
