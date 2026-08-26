// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>

#include <array>
#include <cstdint>

namespace FastCache
{

namespace
{
    /// Sentinel for a byte the alphabet does not contain.
    constexpr std::uint8_t Invalid = 0xFF;

    /// Value per input byte, built once at compile time.
    ///
    /// A table rather than four range comparisons per character: the ladder form
    /// has to spell the alphabet's boundaries four times, and the offsets are
    /// exactly the kind of arithmetic that is wrong by one in only one branch.
    consteval std::array<std::uint8_t, 256> BuildDecodeTable()
    {
        std::array<std::uint8_t, 256> table {};
        for (auto& entry: table)
            entry = Invalid;
        constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < Alphabet.size(); ++i)
            table[static_cast<unsigned char>(Alphabet[i])] = static_cast<std::uint8_t>(i);
        return table;
    }

    constexpr auto DecodeTable = BuildDecodeTable();
} // namespace

std::optional<std::string> Base64Decode(std::string_view text)
{
    // An empty input decodes to nothing, which is different from an error: it is
    // what `Basic ` with no payload carries, and the caller rejects the empty
    // credential rather than this function guessing on its behalf.
    if (text.empty())
        return std::string {};
    if (text.size() % 4 != 0)
        return std::nullopt;

    // Padding is only ever the last one or two bytes. Counting it up front means
    // the loop below never has to special-case a group, and a `=` anywhere else
    // falls through to the alphabet check and is refused.
    std::size_t padding = 0;
    if (text.back() == '=')
    {
        ++padding;
        if (text.size() >= 2 && text[text.size() - 2] == '=')
            ++padding;
    }

    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4)
    {
        // Padding belongs to the LAST group only. Applying the count to every
        // group would drop a byte from each, which decodes short inputs correctly
        // and silently truncates every longer one -- the shape of bug that passes
        // a round-trip test written with one short string.
        auto const isLastGroup = i + 4 == text.size();
        auto const groupPadding = isLastGroup ? padding : 0;

        std::uint32_t group = 0;
        for (std::size_t j = 0; j < 4; ++j)
        {
            auto const ch = text[i + j];
            if (ch == '=' && isLastGroup && j >= 4 - groupPadding)
            {
                group <<= 6;
                continue;
            }
            auto const value = DecodeTable[static_cast<unsigned char>(ch)];
            if (value == Invalid)
                return std::nullopt;
            group = (group << 6) | value;
        }

        out.push_back(static_cast<char>((group >> 16) & 0xFF));
        if (groupPadding < 2)
            out.push_back(static_cast<char>((group >> 8) & 0xFF));
        if (groupPadding < 1)
            out.push_back(static_cast<char>(group & 0xFF));
    }

    return out;
}

} // namespace FastCache
