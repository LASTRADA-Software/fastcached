// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>

namespace FastCache
{

namespace
{
    /// Sentinel for a byte the alphabet does not contain.
    constexpr std::uint8_t Invalid = 0xFF;

    /// The standard alphabet (RFC 4648 §4): HTTP `Basic`'s.
    ///
    /// Named once rather than spelled in each function: an encoder and a decoder
    /// holding two copies of these sixty-four characters is two chances to get one
    /// of them wrong, and the failure only shows up when a value crosses between
    /// two builds.
    constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// The URL-safe alphabet (RFC 4648 §5): a public key's (#178). The standard one with
    /// its last two symbols replaced and nothing else, so the two tables below differ in
    /// exactly the two entries that make a value typed in one unreadable in the other.
    constexpr std::string_view UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    /// Value per input byte, built once at compile time.
    ///
    /// A table rather than four range comparisons per character: the ladder form
    /// has to spell the alphabet's boundaries four times, and the offsets are
    /// exactly the kind of arithmetic that is wrong by one in only one branch.
    /// @param alphabet The sixty-four symbols, in value order.
    /// @return The decode table.
    consteval std::array<std::uint8_t, 256> BuildDecodeTable(std::string_view alphabet)
    {
        std::array<std::uint8_t, 256> table {};
        for (auto& entry: table)
            entry = Invalid;
        for (auto const i: std::views::iota(std::size_t { 0 }, alphabet.size()))
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<std::uint8_t>(i);
        return table;
    }

    constexpr auto DecodeTable = BuildDecodeTable(Alphabet);
    constexpr auto UrlDecodeTable = BuildDecodeTable(UrlAlphabet);

    /// Whether an encoding closes its final group with `=`.
    enum class Padding : std::uint8_t
    {
        Padded,   ///< Standard base64: every group four symbols, the short one filled with `=`.
        Unpadded, ///< base64url as a key is spelled: the short group simply ends.
    };

    /// Encode @p bytes in @p alphabet: the one walk both public encoders are.
    ///
    /// Walked by group rather than by byte: three input bytes per group, the last one short
    /// when the length is not a multiple of three. `+ 2` cannot overflow, since a span holds
    /// no more bytes than half the address space.
    /// @param bytes What to encode.
    /// @param alphabet The sixty-four symbols.
    /// @param padding Whether a short final group is filled out with `=`.
    /// @return The encoded text.
    [[nodiscard]] std::string EncodeWith(std::span<std::byte const> bytes, std::string_view alphabet, Padding padding)
    {
        auto const groups = (bytes.size() + 2) / 3;
        std::string out;
        out.reserve(groups * 4);

        for (auto const ordinal: std::views::iota(std::size_t { 0 }, groups))
        {
            auto const i = ordinal * 3;

            // How many of this group's three input bytes actually exist. The final
            // group is the only short one, and the count drives both how many symbols
            // are emitted and how many `=` follow -- computed once rather than
            // branched on twice, which is where the two traditionally disagree.
            auto const present = std::min<std::size_t>(3, bytes.size() - i);

            std::uint32_t group = 0;
            for (auto const j: std::views::iota(std::size_t { 0 }, std::size_t { 3 }))
                group = (group << 8) | (j < present ? std::to_integer<std::uint32_t>(bytes[i + j]) : 0);

            // Three input bytes are four symbols; two are three; one is two. The spare
            // low bits of a short group are zero because they were shifted in as zero
            // above, which is exactly the property both decoders refuse an input for
            // getting wrong.
            for (auto const j: std::views::iota(std::size_t { 0 }, present + 1))
                out.push_back(alphabet[(group >> (18 - (6 * j))) & 0x3F]);
            if (padding == Padding::Padded)
                for ([[maybe_unused]] auto const j: std::views::iota(present + 1, std::size_t { 4 }))
                    out.push_back('=');
        }

        return out;
    }
} // namespace

std::string Base64Encode(std::span<std::byte const> bytes)
{
    return EncodeWith(bytes, Alphabet, Padding::Padded);
}

std::string Base64UrlEncode(std::span<std::byte const> bytes)
{
    return EncodeWith(bytes, UrlAlphabet, Padding::Unpadded);
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

    // Walked by group, four symbols each -- exactly, because a length that is not a multiple of
    // four was refused above.
    auto const groups = text.size() / 4;
    std::string out;
    out.reserve(groups * 3);
    for (auto const ordinal: std::views::iota(std::size_t { 0 }, groups))
    {
        auto const i = ordinal * 4;

        // Padding belongs to the LAST group only. Applying the count to every
        // group would drop a byte from each, which decodes short inputs correctly
        // and silently truncates every longer one -- the shape of bug that passes
        // a round-trip test written with one short string.
        auto const isLastGroup = i + 4 == text.size();
        auto const groupPadding = isLastGroup ? padding : 0;

        std::uint32_t group = 0;
        for (auto const j: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
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

std::optional<std::string> Base64UrlDecode(std::string_view text)
{
    // A final group of ONE symbol carries six bits, which is no whole byte: no encoder
    // emits it, so it is refused rather than read as nothing. Two and three are the short
    // groups `EncodeWith` does emit, for one and two trailing bytes.
    auto const tail = text.size() % 4;
    if (tail == 1)
        return std::nullopt;

    std::string out;
    out.reserve(((text.size() / 4) * 3) + 2);

    // Whole groups first, then the short one -- `Base64Decode`'s walk with the padding
    // bookkeeping gone, because there is no `=` here for a group to carry: one anywhere is
    // outside the alphabet and refused by the table like any other stranger.
    auto const decodeGroup = [&out](std::string_view symbols) -> bool {
        std::uint32_t group = 0;
        for (auto const ch: symbols)
        {
            auto const value = UrlDecodeTable[static_cast<unsigned char>(ch)];
            if (value == Invalid)
                return false;
            group = (group << 6) | value;
        }

        // How many whole bytes these symbols carry, and the bits left over below them:
        // four symbols are three bytes exactly, three are two bytes and two spare bits,
        // two are one byte and four spare bits. The spare bits must be zero, or one key
        // has several spellings -- the property `Base64Decode` holds for padded input.
        auto const bytes = (symbols.size() * 6) / 8;
        auto const spareBits = (symbols.size() * 6) - (bytes * 8);
        if ((group & ((std::uint32_t { 1 } << spareBits) - 1)) != 0)
            return false;
        group >>= spareBits;
        for (auto const j: std::views::iota(std::size_t { 0 }, bytes))
            out.push_back(static_cast<char>((group >> (8 * (bytes - 1 - j))) & 0xFF));
        return true;
    };

    for (auto const ordinal: std::views::iota(std::size_t { 0 }, text.size() / 4))
        if (!decodeGroup(text.substr(ordinal * 4, 4)))
            return std::nullopt;
    if (tail != 0 && !decodeGroup(text.substr(text.size() - tail)))
        return std::nullopt;

    return out;
}

} // namespace FastCache
