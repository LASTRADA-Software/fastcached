// SPDX-License-Identifier: Apache-2.0
#include "RespClient.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The line terminator every RESP token ends with.
    constexpr std::string_view Crlf = "\r\n";

    /// The leading byte for each type this parser accepts.
    ///
    /// **These bytes are the wire contract**, which is why they are spelled here as
    /// literals rather than derived from `RespType`'s ordinals: the enum is private and
    /// may be reordered freely, and nothing about its order may reach the wire.
    namespace TypeMarker
    {
        constexpr char SimpleString = '+';
        constexpr char Error = '-';
        constexpr char Integer = ':';
        constexpr char BulkString = '$';
        constexpr char Array = '*';
        constexpr char Null = '_';
        constexpr char Boolean = '#';
        constexpr char Double = ',';
        constexpr char BigNumber = '(';
        constexpr char Verbatim = '=';
        constexpr char Map = '%';
        constexpr char Set = '~';
        constexpr char Push = '>';
    } // namespace TypeMarker

    /// A parse that needs more bytes.
    /// @return The result.
    [[nodiscard]] ParseResult Incomplete() noexcept
    {
        return ParseResult { .state = ParseState::Incomplete };
    }

    /// A parse that cannot succeed.
    /// @param why What is wrong, for the operator.
    /// @return The result.
    [[nodiscard]] ParseResult Malformed(std::string why)
    {
        return ParseResult { .state = ParseState::Malformed, .diagnostic = std::move(why) };
    }

    /// A completed parse.
    /// @param value The reply.
    /// @param consumed How many bytes it occupied.
    /// @return The result.
    [[nodiscard]] ParseResult Complete(RespValue value, std::size_t consumed)
    {
        return ParseResult { .state = ParseState::Complete, .value = std::move(value), .consumed = consumed };
    }

    /// Parse a RESP length or count: an optional `-` then digits, and nothing else.
    ///
    /// Strict on purpose. `std::from_chars` would accept a leading digit run and stop
    /// at the first non-digit, so `12x` would read as 12 -- a peer-controlled length
    /// silently truncated to something plausible is exactly the shape that turns a
    /// malformed frame into a wrong answer.
    /// @param text The header text, CRLF already stripped.
    /// @return The value, or nullopt when @p text is not exactly an integer.
    [[nodiscard]] std::optional<std::int64_t> ParseCount(std::string_view text) noexcept
    {
        if (text.empty())
            return std::nullopt;
        auto const digits = text.front() == '-' ? text.substr(1) : text;
        if (digits.empty() || !std::ranges::all_of(digits, [](char ch) { return ch >= '0' && ch <= '9'; }))
            return std::nullopt;

        std::int64_t value = 0;
        auto const* const first = text.data();
        auto const* const last = text.data() + text.size();
        auto const [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc {} || ptr != last)
            return std::nullopt;
        return value;
    }

    /// Parse a RESP3 double, including the three names RESP gives non-finite values.
    /// @param text The header text.
    /// @return The value, or nullopt when @p text is not a double.
    [[nodiscard]] std::optional<double> ParseDouble(std::string_view text) noexcept
    {
        if (text == "inf")
            return std::numeric_limits<double>::infinity();
        if (text == "-inf")
            return -std::numeric_limits<double>::infinity();
        if (text == "nan")
            return std::numeric_limits<double>::quiet_NaN();

        double value = 0.0;
        auto const* const first = text.data();
        auto const* const last = text.data() + text.size();
        auto const [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc {} || ptr != last)
            return std::nullopt;
        return value;
    }

    /// Parse one reply beginning at @p offset.
    ///
    /// @param bytes Everything read so far.
    /// @param offset Where this reply starts.
    /// @param limits Caps this side imposes.
    /// @param depth How many aggregates enclose this one.
    /// @return The result, whose `consumed` counts bytes from @p offset.
    [[nodiscard]] ParseResult ParseOne(std::string_view bytes,
                                       std::size_t offset,
                                       ParseLimits const& limits,
                                       std::size_t depth)
    {
        if (depth > limits.maxDepth)
            return Malformed(std::format("reply nests deeper than {} levels", limits.maxDepth));
        if (offset >= bytes.size())
            return Incomplete();

        auto const marker = bytes[offset];
        auto const headerStart = offset + 1;
        auto const eol = bytes.find(Crlf, headerStart);
        if (eol == std::string_view::npos)
            return Incomplete();

        auto const header = bytes.substr(headerStart, eol - headerStart);
        auto const afterHeader = eol + Crlf.size();

        // A payload-carrying type: the header is a byte count and the bytes follow.
        auto const readBlob = [&](RespType type) -> ParseResult {
            auto const declared = ParseCount(header);
            if (!declared.has_value())
                return Malformed(std::format("'{}' is not a length", header));
            if (*declared == -1)
                return Complete(RespValue { .type = RespType::Null }, afterHeader - offset);
            if (*declared < 0)
                return Malformed(std::format("negative length {}", *declared));

            auto const length = static_cast<std::size_t>(*declared);
            if (length > limits.maxBulkBytes)
                return Malformed(
                    std::format("declared payload of {} bytes exceeds the {}-byte cap", length, limits.maxBulkBytes));
            if (bytes.size() < afterHeader + length + Crlf.size())
                return Incomplete();
            if (bytes.substr(afterHeader + length, Crlf.size()) != Crlf)
                return Malformed("payload is not CRLF-terminated");

            return Complete(RespValue { .type = type, .text = std::string { bytes.substr(afterHeader, length) } },
                            afterHeader + length + Crlf.size() - offset);
        };

        // An aggregate: the header is an element count and the elements follow.
        auto const readAggregate = [&](RespType type, std::size_t perElement) -> ParseResult {
            auto const declared = ParseCount(header);
            if (!declared.has_value())
                return Malformed(std::format("'{}' is not an element count", header));
            if (*declared == -1)
                return Complete(RespValue { .type = RespType::Null }, afterHeader - offset);
            if (*declared < 0)
                return Malformed(std::format("negative element count {}", *declared));

            auto const declaredCount = static_cast<std::size_t>(*declared);
            if (declaredCount > limits.maxElements / perElement)
                return Malformed(std::format("declared {} elements, above the {} cap", declaredCount, limits.maxElements));

            auto const total = declaredCount * perElement;
            RespValue aggregate { .type = type };
            aggregate.items.reserve(total);

            auto at = afterHeader;
            for (std::size_t index = 0; index < total; ++index)
            {
                auto element = ParseOne(bytes, at, limits, depth + 1);
                if (element.state != ParseState::Complete)
                    return element;
                at += element.consumed;
                aggregate.items.push_back(std::move(element.value));
            }
            return Complete(std::move(aggregate), at - offset);
        };

        switch (marker)
        {
            case TypeMarker::SimpleString:
                return Complete(RespValue { .type = RespType::SimpleString, .text = std::string { header } },
                                afterHeader - offset);
            case TypeMarker::Error:
                return Complete(RespValue { .type = RespType::Error, .text = std::string { header } }, afterHeader - offset);
            case TypeMarker::BigNumber:
                return Complete(RespValue { .type = RespType::BigNumber, .text = std::string { header } },
                                afterHeader - offset);
            case TypeMarker::Integer: {
                auto const parsed = ParseCount(header);
                if (!parsed.has_value())
                    return Malformed(std::format("'{}' is not an integer", header));
                return Complete(RespValue { .type = RespType::Integer, .integer = *parsed }, afterHeader - offset);
            }
            case TypeMarker::Null:
                if (!header.empty())
                    return Malformed("a null carries no payload");
                return Complete(RespValue { .type = RespType::Null }, afterHeader - offset);
            case TypeMarker::Boolean:
                if (header != "t" && header != "f")
                    return Malformed(std::format("'{}' is not a boolean", header));
                return Complete(RespValue { .type = RespType::Boolean, .boolean = header == "t" }, afterHeader - offset);
            case TypeMarker::Double: {
                auto const parsed = ParseDouble(header);
                if (!parsed.has_value())
                    return Malformed(std::format("'{}' is not a double", header));
                return Complete(RespValue { .type = RespType::Double, .real = *parsed }, afterHeader - offset);
            }
            case TypeMarker::BulkString:
                return readBlob(RespType::BulkString);
            case TypeMarker::Verbatim:
                return readBlob(RespType::Verbatim);
            case TypeMarker::Array:
                return readAggregate(RespType::Array, 1);
            case TypeMarker::Set:
                return readAggregate(RespType::Set, 1);
            case TypeMarker::Push:
                return readAggregate(RespType::Push, 1);
            case TypeMarker::Map:
                // Two elements per declared pair: `%1` is one key and one value, and a
                // parser that read it as one element would leave the value in the
                // buffer to be mistaken for the next reply.
                return readAggregate(RespType::Map, 2);
            default:
                break;
        }
        return Malformed(std::format("'{}' does not begin a reply", marker));
    }
} // namespace

std::string_view RespTypeName(RespType type) noexcept
{
    auto const index = static_cast<std::size_t>(type);
    if (index >= RespTypeTable.size())
        return "unknown";
    return RespTypeTable[index].name;
}

std::string_view ErrorCodeWord(std::string_view text) noexcept
{
    auto const space = text.find(' ');
    return space == std::string_view::npos ? text : text.substr(0, space);
}

bool IsError(RespValue const& value) noexcept
{
    return value.type == RespType::Error;
}

bool IsNull(RespValue const& value) noexcept
{
    return value.type == RespType::Null;
}

ParseResult ParseReply(std::string_view bytes, ParseLimits const& limits)
{
    return ParseOne(bytes, 0, limits, 0);
}

std::vector<std::byte> EncodeCommand(std::span<std::string const> argv)
{
    std::string out = std::format("{}{}{}", TypeMarker::Array, argv.size(), Crlf);
    for (auto const& arg: argv)
        out += std::format("{}{}{}{}{}", TypeMarker::BulkString, arg.size(), Crlf, arg, Crlf);

    std::vector<std::byte> bytes;
    bytes.reserve(out.size());
    for (auto const ch: out)
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    return bytes;
}

std::expected<RespValue, ExchangeError> Call(IExchange& exchange, std::initializer_list<std::string_view> argv)
{
    std::vector<std::string> owned;
    owned.reserve(argv.size());
    for (auto const arg: argv)
        owned.emplace_back(arg);
    return exchange.Call(owned);
}

} // namespace FastCache::Cli
