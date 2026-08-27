// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

namespace FastCache
{

/// Bits of a continuation byte that say it is one.
inline constexpr std::uint8_t Utf8ContinuationMask = 0xC0;

/// What those bits must hold: `10xxxxxx`.
inline constexpr std::uint8_t Utf8ContinuationMark = 0x80;

/// Bits of a continuation byte that carry code-point data.
inline constexpr std::uint8_t Utf8ContinuationPayload = 0x3F;

/// How many they are.
inline constexpr unsigned Utf8ContinuationBits = 6;

/// The largest code point Unicode defines, and so the largest UTF-8 may encode.
///
/// Four-byte sequences can express values above it — `F4 90 80 80` is `U+110000`,
/// which assembles perfectly and is not a character. RFC 3629 removed that range
/// from UTF-8 when it capped the encoding at the UTF-16 ceiling, so a decoder that
/// only checks the sequence's shape accepts text no other decoder will.
inline constexpr char32_t Utf8MaxCodePoint = 0x10FFFF;

/// First code point reserved for UTF-16 surrogate pairs.
inline constexpr char32_t Utf8FirstSurrogate = 0xD800;

/// Last code point reserved for UTF-16 surrogate pairs.
///
/// A surrogate is half of a UTF-16 pair and never stands alone, so UTF-8 may not
/// encode one. `ED A0 80` is the CESU-8 spelling of `U+D800`: valid in shape,
/// rejected by every strict decoder, and the byte sequence a lenient encoder is
/// most likely to emit by accident.
inline constexpr char32_t Utf8LastSurrogate = 0xDFFF;

/// One class of UTF-8 lead byte: what follows it, and what it may encode.
struct Utf8LeadClass
{
    std::uint8_t first;        ///< First lead byte in this class.
    std::uint8_t last;         ///< Last lead byte in this class.
    std::uint8_t payloadMask;  ///< Bits of the lead byte that carry code-point data.
    std::size_t continuations; ///< Continuation bytes that must follow it.
    /// Smallest code point a sequence of this length may legally encode.
    ///
    /// The overlong check, and the reason it is a column rather than a comment:
    /// every code point has exactly one UTF-8 spelling, and `C0 80` — a
    /// two-byte `U+0000` — is the classic way to smuggle a NUL past a scanner
    /// that only looks for the one-byte form.
    char32_t lowest;
};

/// Every lead byte that starts a sequence, and what must follow it.
///
/// A table rather than a ladder of `if`s, and the bytes it OMITS carry as much of
/// the rule as the rows do: `0x80`–`0xBF` are continuation bytes and start
/// nothing, `0xC0`–`0xC1` can only ever spell an overlong ASCII character, and
/// `0xF5`–`0xFF` lead sequences that would exceed `Utf8MaxCodePoint` before a
/// single continuation byte is read. A byte matching no row starts no sequence,
/// which is one answer rather than three special cases.
inline constexpr std::array Utf8LeadClasses {
    Utf8LeadClass { .first = 0x00, .last = 0x7F, .payloadMask = 0x7F, .continuations = 0, .lowest = 0x0000 },
    Utf8LeadClass { .first = 0xC2, .last = 0xDF, .payloadMask = 0x1F, .continuations = 1, .lowest = 0x0080 },
    Utf8LeadClass { .first = 0xE0, .last = 0xEF, .payloadMask = 0x0F, .continuations = 2, .lowest = 0x0800 },
    Utf8LeadClass { .first = 0xF0, .last = 0xF4, .payloadMask = 0x07, .continuations = 3, .lowest = 0x10000 },
};

/// One decoded code point, and how many bytes it occupied.
struct Utf8CodePoint
{
    char32_t value;     ///< The code point.
    std::size_t length; ///< Bytes it was encoded in; never 0.
};

/// Decode the UTF-8 sequence at the front of `text`.
///
/// The one primitive every reader of this header shares, so "what is a valid
/// sequence" is decided once. It answers with the code point rather than only its
/// length because that is a different question from "may this format carry it":
/// `U+FFFF` is perfectly good UTF-8 and XML forbids it outright, so a caller that
/// could only ask about bytes would have no way to tell.
/// @param text Bytes to inspect; only the front is read.
/// @return The code point and its length, or nothing when `text` does not start
///         with a valid sequence — empty, a byte that leads nothing, a truncated
///         tail, a bad continuation byte, an overlong form, a surrogate, or above
///         `Utf8MaxCodePoint`.
[[nodiscard]] constexpr std::optional<Utf8CodePoint> DecodeUtf8(std::string_view text) noexcept
{
    if (text.empty())
        return std::nullopt;

    auto const lead = static_cast<std::uint8_t>(text.front());
    for (auto const& row: Utf8LeadClasses)
    {
        if (lead < row.first || lead > row.last)
            continue;

        // Truncated, not merely short: a sequence cut off by the end of the text is
        // as invalid as one cut off by a bad byte. Answering "valid so far" would
        // let a caller append the rest later, which is exactly the assumption that
        // makes a chunked writer emit a document no decoder accepts.
        if (text.size() <= row.continuations)
            return std::nullopt;

        auto value = static_cast<char32_t>(lead & row.payloadMask);
        for (auto const index: std::views::iota(std::size_t { 1 }, row.continuations + 1))
        {
            auto const byte = static_cast<std::uint8_t>(text[index]);
            if ((byte & Utf8ContinuationMask) != Utf8ContinuationMark)
                return std::nullopt;
            value = static_cast<char32_t>((value << Utf8ContinuationBits) | (byte & Utf8ContinuationPayload));
        }

        if (value < row.lowest || value > Utf8MaxCodePoint)
            return std::nullopt;
        if (value >= Utf8FirstSurrogate && value <= Utf8LastSurrogate)
            return std::nullopt;
        return Utf8CodePoint { .value = value, .length = row.continuations + 1 };
    }

    return std::nullopt;
}

/// Length of the valid UTF-8 sequence at the front of `text`.
///
/// The length half of `DecodeUtf8`, for the callers that only need to know how far
/// to advance.
/// @param text Bytes to inspect; only the front is read.
/// @return The sequence's length in bytes, or 0 when there is no valid sequence.
[[nodiscard]] constexpr std::size_t Utf8SequenceLength(std::string_view text) noexcept
{
    auto const decoded = DecodeUtf8(text);
    return decoded.has_value() ? decoded->length : 0;
}

/// Whether `text` is well-formed UTF-8 from end to end.
///
/// Strict in the sense RFC 3629 is strict, which is also the sense every
/// language's own decoder is: an overlong form, a lone surrogate and a value above
/// `U+10FFFF` are all rejected, not repaired. Anything this accepts, a strict
/// parser on the other end accepts too — which is the whole point of asking.
/// @param text The bytes.
/// @return True when every byte belongs to a valid sequence.
[[nodiscard]] constexpr bool IsValidUtf8(std::string_view text) noexcept
{
    while (!text.empty())
    {
        auto const length = Utf8SequenceLength(text);
        if (length == 0)
            return false;
        text.remove_prefix(length);
    }
    return true;
}

} // namespace FastCache
