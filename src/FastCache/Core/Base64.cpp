// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>

#include <algorithm>
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
    /// The one alphabet, shared by both directions.
    ///
    /// Named once rather than spelled in each function: an encoder and a decoder
    /// holding two copies of these sixty-four characters is two chances to get one
    /// of them wrong, and the failure only shows up when a value crosses between
    /// two builds.
    constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    consteval std::array<std::uint8_t, 256> BuildDecodeTable()
    {
        std::array<std::uint8_t, 256> table {};
        for (auto& entry: table)
            entry = Invalid;
        for (std::size_t i = 0; i < Alphabet.size(); ++i)
            table[static_cast<unsigned char>(Alphabet[i])] = static_cast<std::uint8_t>(i);
        return table;
    }

    constexpr auto DecodeTable = BuildDecodeTable();
} // namespace

std::string Base64Encode(std::span<std::byte const> bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        // How many of this group's three input bytes actually exist. The final
        // group is the only short one, and the count drives both how many symbols
        // are emitted and how many `=` follow -- computed once rather than
        // branched on twice, which is where the two traditionally disagree.
        auto const present = std::min<std::size_t>(3, bytes.size() - i);

        std::uint32_t group = 0;
        for (std::size_t j = 0; j < 3; ++j)
            group = (group << 8) | (j < present ? std::to_integer<std::uint32_t>(bytes[i + j]) : 0);

        // Three input bytes are four symbols; two are three; one is two. The spare
        // low bits of a short group are zero because they were shifted in as zero
        // above, which is exactly the property `Base64Decode` refuses an input for
        // getting wrong.
        for (std::size_t j = 0; j < present + 1; ++j)
            out.push_back(Alphabet[(group >> (18 - (6 * j))) & 0x3F]);
        for (std::size_t j = present + 1; j < 4; ++j)
            out.push_back('=');
    }

    return out;
}

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

        // The bits a padded group cannot carry must be zero, or one value has
        // several spellings. Two symbols carry twelve bits and one byte consumes
        // eight; three carry eighteen and two consume sixteen -- so the spare
        // count is twice the padding, sitting directly below the bytes emitted
        // below. Dropping them unchecked is the traditional shape of this
        // function, and it is what turns `QQ==` and `QR==` into one secret.
        auto const spareBits = 2 * groupPadding;
        if (spareBits != 0 && ((group >> (6 * groupPadding)) & ((std::uint32_t { 1 } << spareBits) - 1)) != 0)
            return std::nullopt;

        out.push_back(static_cast<char>((group >> 16) & 0xFF));
        if (groupPadding < 2)
            out.push_back(static_cast<char>((group >> 8) & 0xFF));
        if (groupPadding < 1)
            out.push_back(static_cast<char>(group & 0xFF));
    }

    return out;
}

} // namespace FastCache
