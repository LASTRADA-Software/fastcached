// SPDX-License-Identifier: Apache-2.0
#include "MemcachedClient.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// The line terminator this protocol ends every line with.
    constexpr std::string_view Crlf = "\r\n";

    /// The words that open a reply and say what the rest of it is.
    ///
    /// **Spelled here as literals rather than derived from `McLeadToken`'s ordinals**,
    /// for the reason `RespClient.cpp`'s `TypeMarker` gives: the enum is private and may
    /// be reordered freely, and nothing about its order may reach the wire. Taken from
    /// the server that writes them -- `MemcachedText.cpp` and `MemcachedMeta.cpp` --
    /// rather than from memcached's documentation, because it is this daemon's replies
    /// this parser has to read.
    struct LeadWord
    {
        std::string_view word; ///< The token as the server writes it.
        McLeadToken token;     ///< What it means.
    };

    /// The words themselves, named where anything else needs to spell one.
    ///
    /// `ValueWord` and `StatWord` are constants rather than `LeadWords[n].word`,
    /// because the parser trims `StatWord.size()` bytes off a line: reaching that
    /// literal by POSITION in the table means reordering two rows silently trims the
    /// wrong count off every `STAT` line, with no build error and a plausible result.
    constexpr std::string_view ValueWord = "VALUE";
    constexpr std::string_view StatWord = "STAT";

    /// The end of a `VALUE` or `STAT` sequence.
    constexpr std::string_view EndWord = "END";

    /// `CLIENT_ERROR` and `SERVER_ERROR` carry a message after the word and `ERROR`
    /// alone does not; all three are one kind to a caller, so three words map onto one
    /// enumerator. Every word this daemon can write is asserted reachable by
    /// `MemcachedClient_test.cpp`'s *every word this daemon opens a reply with is
    /// recognised* -- from OUTSIDE this table, against words read out of the server,
    /// since a census walking the table would agree with it whatever it held.
    constexpr std::array<LeadWord, 6> LeadWords { {
        { .word = ValueWord, .token = McLeadToken::Value },
        { .word = StatWord, .token = McLeadToken::Stat },
        { .word = "ME", .token = McLeadToken::Meta },
        { .word = "ERROR", .token = McLeadToken::Error },
        { .word = "CLIENT_ERROR", .token = McLeadToken::Error },
        { .word = "SERVER_ERROR", .token = McLeadToken::Error },
    } };

    /// What @p line's first token says the reply is.
    /// @param line One line, CRLF already stripped.
    /// @return The kind.
    [[nodiscard]] McLeadToken LeadTokenOf(std::string_view line) noexcept
    {
        auto const space = line.find(' ');
        auto const first = space == std::string_view::npos ? line : line.substr(0, space);
        for (auto const& row: LeadWords)
            if (first == row.word)
                return row.token;
        return McLeadToken::Status;
    }

    /// A parse that needs more bytes.
    /// @return The result.
    [[nodiscard]] McParseResult Incomplete() noexcept
    {
        return McParseResult { .state = McParseState::Incomplete };
    }

    /// A parse that cannot succeed.
    /// @param why What is wrong, for the operator.
    /// @return The result.
    [[nodiscard]] McParseResult Malformed(std::string why)
    {
        return McParseResult { .state = McParseState::Malformed, .diagnostic = std::move(why) };
    }

    /// Read one CRLF-terminated line starting at @p offset.
    ///
    /// @param bytes Everything read so far.
    /// @param offset Where the line starts.
    /// @param limits Caps this side imposes.
    /// @param line Receives the line without its CRLF.
    /// @param next Receives the offset just past the CRLF.
    /// @return `Complete` when a whole line was read, else why not.
    [[nodiscard]] McParseState TakeLine(std::string_view bytes,
                                        std::size_t offset,
                                        McParseLimits const& limits,
                                        std::string_view& line,
                                        std::size_t& next) noexcept
    {
        auto const rest = bytes.substr(offset);
        auto const end = rest.find(Crlf);
        if (end == std::string_view::npos)
        {
            // A line already longer than the cap will never terminate acceptably, so
            // this is Malformed rather than Incomplete: waiting for more bytes cannot
            // help, and a peer that never sends a CRLF would otherwise buffer forever.
            return rest.size() > limits.maxLineBytes ? McParseState::Malformed : McParseState::Incomplete;
        }
        if (end > limits.maxLineBytes)
            return McParseState::Malformed;
        line = rest.substr(0, end);
        next = offset + end + Crlf.size();
        return McParseState::Complete;
    }

    /// Parse a non-negative integer that occupies the whole of @p text.
    ///
    /// Strict on purpose, for the reason `RespClient.cpp`'s `ParseCount` gives: a
    /// prefix parse would read `12x` as 12, and this number is a peer-controlled LENGTH.
    /// @param text The token.
    /// @param out Receives the value.
    /// @return True when the whole token is an integer.
    template <typename T>
    [[nodiscard]] bool WholeUnsigned(std::string_view text, T& out) noexcept
    {
        if (text.empty())
            return false;
        auto const* const first = text.data();
        auto const* const last = text.data() + text.size();
        auto const [ptr, ec] = std::from_chars(first, last, out);
        return ec == std::errc {} && ptr == last;
    }

    /// Split @p line on single spaces.
    /// @param line The line.
    /// @return The tokens, empty ones dropped.
    [[nodiscard]] std::vector<std::string_view> Tokens(std::string_view line)
    {
        std::vector<std::string_view> out;
        std::size_t at = 0;
        while (at < line.size())
        {
            auto const space = line.find(' ', at);
            auto const end = space == std::string_view::npos ? line.size() : space;
            if (end > at)
                out.push_back(line.substr(at, end - at));
            at = end + 1;
        }
        return out;
    }

    /// Read one `VALUE` header and the payload that follows it.
    ///
    /// @param bytes Everything read so far.
    /// @param line The header line, CRLF stripped.
    /// @param after Offset just past the header's CRLF; receives the offset past the
    ///              payload's CRLF on success.
    /// @param limits Caps this side imposes.
    /// @param value Receives the block.
    /// @param diagnostic Receives why, when the result is `Malformed`.
    /// @return How it ended.
    [[nodiscard]] McParseState TakeValueBlock(std::string_view bytes,
                                              std::string_view line,
                                              std::size_t& after,
                                              McParseLimits const& limits,
                                              McValue& value,
                                              std::string& diagnostic)
    {
        auto const parts = Tokens(line);
        // `VALUE <key> <flags> <bytes>` with an optional `<cas>`; anything else is a
        // header this daemon does not write.
        if (parts.size() < 4 || parts.size() > 5)
        {
            diagnostic = "VALUE header has " + std::to_string(parts.size()) + " tokens, expected 4 or 5";
            return McParseState::Malformed;
        }

        std::uint32_t flags = 0;
        std::size_t length = 0;
        if (!WholeUnsigned(parts[2], flags))
        {
            diagnostic = "VALUE header's flags field is not an integer";
            return McParseState::Malformed;
        }
        if (!WholeUnsigned(parts[3], length))
        {
            diagnostic = "VALUE header's byte count is not an integer";
            return McParseState::Malformed;
        }
        // The cap is checked BEFORE the length is used to size anything.
        if (length > limits.maxValueBytes)
        {
            diagnostic = "VALUE of " + std::to_string(length) + " bytes exceeds this client's cap of "
                         + std::to_string(limits.maxValueBytes);
            return McParseState::Malformed;
        }

        std::uint64_t cas = 0;
        bool hasCas = false;
        if (parts.size() == 5)
        {
            if (!WholeUnsigned(parts[4], cas))
            {
                diagnostic = "VALUE header's cas field is not an integer";
                return McParseState::Malformed;
            }
            hasCas = true;
        }

        if (bytes.size() < after + length + Crlf.size())
            return McParseState::Incomplete;
        if (bytes.substr(after + length, Crlf.size()) != Crlf)
        {
            diagnostic = "VALUE payload is not followed by CRLF";
            return McParseState::Malformed;
        }

        value = McValue { .key = std::string { parts[1] },
                          .flags = flags,
                          .cas = cas,
                          .hasCas = hasCas,
                          .data = std::string { bytes.substr(after, length) } };
        after += length + Crlf.size();
        return McParseState::Complete;
    }
} // namespace

McParseResult ParseMemcachedReply(std::string_view bytes, McParseLimits const& limits)
{
    std::string_view line;
    std::size_t at = 0;
    switch (TakeLine(bytes, 0, limits, line, at))
    {
        case McParseState::Incomplete:
            return Incomplete();
        case McParseState::Malformed:
            return Malformed("the first line exceeds this client's line cap");
        case McParseState::Complete:
            break;
        case McParseState::Last:
            break;
    }

    auto const kind = LeadTokenOf(line);

    // Every shape but `VALUE` and `STAT` is one line, and that line IS the answer.
    if (kind != McLeadToken::Value && kind != McLeadToken::Stat)
    {
        McReply reply {};
        reply.kind = kind;
        reply.status = std::string { line };
        if (kind == McLeadToken::Meta)
        {
            // `ME <key> <name>=<value>...`. A flag with no `=` is kept with an empty
            // value rather than dropped: it is something this daemon does not write
            // today, and discarding it would make a future one invisible.
            auto const parts = Tokens(line);
            if (parts.size() >= 2)
                reply.metaKey = std::string { parts[1] };
            for (std::size_t index = 2; index < parts.size(); ++index)
            {
                auto const equals = parts[index].find('=');
                if (equals == std::string_view::npos)
                    reply.metaFlags.push_back(McPair { .name = std::string { parts[index] }, .value = {} });
                else
                    reply.metaFlags.push_back(McPair { .name = std::string { parts[index].substr(0, equals) },
                                                       .value = std::string { parts[index].substr(equals + 1) } });
            }
        }
        return McParseResult { .state = McParseState::Complete, .reply = std::move(reply), .consumed = at };
    }

    // `VALUE` and `STAT` repeat until `END`.
    McReply reply {};
    reply.kind = kind;
    std::size_t cursor = 0;
    auto current = line;
    auto next = at;
    while (true)
    {
        if (current == EndWord)
        {
            reply.status = std::string { EndWord };
            return McParseResult { .state = McParseState::Complete, .reply = std::move(reply), .consumed = next };
        }

        auto const currentKind = LeadTokenOf(current);
        if (currentKind == McLeadToken::Error)
        {
            // A server that gives up midway says so, and the caller needs the sentence
            // rather than a truncated item list.
            McReply failed {};
            failed.kind = McLeadToken::Error;
            failed.status = std::string { current };
            return McParseResult { .state = McParseState::Complete, .reply = std::move(failed), .consumed = next };
        }
        if (currentKind != kind)
            return Malformed("a " + std::string { LeadTokenTable[static_cast<std::size_t>(kind)].name }
                             + " sequence is interrupted by: " + std::string { current });

        if (cursor >= limits.maxItems)
            return Malformed("more than " + std::to_string(limits.maxItems) + " items in one reply");
        ++cursor;

        if (kind == McLeadToken::Value)
        {
            McValue value;
            std::string diagnostic;
            auto const state = TakeValueBlock(bytes, current, next, limits, value, diagnostic);
            if (state == McParseState::Incomplete)
                return Incomplete();
            if (state != McParseState::Complete)
                return Malformed(std::move(diagnostic));
            reply.values.push_back(std::move(value));
        }
        else
        {
            // `STAT <name> <value...>`: the name is one token and the value is the
            // rest of the line, which may contain spaces.
            auto const body = current.substr(StatWord.size());
            auto const start = body.find_first_not_of(' ');
            if (start == std::string_view::npos)
                return Malformed("a STAT line names nothing");
            auto const rest = body.substr(start);
            auto const space = rest.find(' ');
            if (space == std::string_view::npos)
                reply.stats.push_back(McPair { .name = std::string { rest }, .value = {} });
            else
                reply.stats.push_back(McPair { .name = std::string { rest.substr(0, space) },
                                               .value = std::string { rest.substr(space + 1) } });
        }

        switch (TakeLine(bytes, next, limits, current, next))
        {
            case McParseState::Incomplete:
                return Incomplete();
            case McParseState::Malformed:
                return Malformed("a line exceeds this client's line cap");
            case McParseState::Complete:
                break;
            case McParseState::Last:
                break;
        }
    }
}

std::string EncodeMemcachedCommand(std::string_view verb, std::span<std::string const> args)
{
    std::string out { verb };
    for (auto const& arg: args)
    {
        out.push_back(' ');
        out.append(arg);
    }
    out.append(Crlf);
    return out;
}

std::string EncodeMemcachedStorage(std::string_view verb,
                                   std::string_view key,
                                   std::uint32_t flags,
                                   std::int64_t exptime,
                                   std::string_view value,
                                   std::uint64_t casToken)
{
    std::string out { verb };
    out.push_back(' ');
    out.append(key);
    out.push_back(' ');
    out.append(std::to_string(flags));
    out.push_back(' ');
    out.append(std::to_string(exptime));
    out.push_back(' ');
    out.append(std::to_string(value.size()));
    // `cas` carries a sixth field and nothing else does. Keyed on the verb rather than
    // on the token being non-zero: zero is a cas token a server can legitimately have
    // issued, so "non-zero" would silently drop the field for it.
    if (verb == "cas")
    {
        out.push_back(' ');
        out.append(std::to_string(casToken));
    }
    out.append(Crlf);
    out.append(value);
    out.append(Crlf);
    return out;
}

bool ParseWholeUnsigned(std::string_view text, std::uint64_t& out) noexcept
{
    return WholeUnsigned(text, out);
}

bool ValidTextToken(std::string_view text) noexcept
{
    if (text.empty())
        return false;
    return std::ranges::none_of(text, [](char ch) {
        auto const byte = static_cast<unsigned char>(ch);
        return byte <= 0x20U || byte == 0x7FU;
    });
}

} // namespace FastCache::Cli
