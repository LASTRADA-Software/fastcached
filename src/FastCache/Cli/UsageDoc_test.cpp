// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/UsageDoc.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// Remove every ANSI SGR escape, so colored output can be compared against
/// plain output character for character.
/// @param text Possibly-colored text.
/// @return `text` with all escape sequences removed.
[[nodiscard]] std::string StripAnsi(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\x1b')
        {
            out += text[i];
            continue;
        }
        while (i < text.size() && text[i] != 'm')
            ++i;
    }
    return out;
}

/// Split rendered output into lines, dropping the trailing empty segment the
/// final newline produces.
/// @param text The rendered text.
/// @return One entry per visual line.
[[nodiscard]] std::vector<std::string> Lines(std::string_view text)
{
    std::vector<std::string> lines;
    while (!text.empty())
    {
        auto const newline = text.find('\n');
        if (newline == std::string_view::npos)
        {
            lines.emplace_back(text);
            break;
        }
        lines.emplace_back(text.substr(0, newline));
        text.remove_prefix(newline + 1);
    }
    return lines;
}

} // namespace

TEST_CASE("an empty document renders as nothing", "[cli][usage]")
{
    CHECK(RenderUsage({}, UsageColor::Plain).empty());
}

TEST_CASE("descriptions align to one column per section", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({
        { .term = "--a", .description = "short flag" },
        { .term = "--a-much-longer-flag", .description = "long flag" },
    });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .title = "OPTIONS", .blocks = blocks } });

    auto const lines = Lines(RenderUsage({ .sections = sections }, UsageColor::Plain));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "OPTIONS");
    // 2 indent + 20 (the widest term) + 2 gap == column 24.
    CHECK(lines[1] == std::string("  --a") + std::string(19, ' ') + "short flag");
    CHECK(lines[2] == "  --a-much-longer-flag  long flag");
}

TEST_CASE("rows separated by prose still share one column", "[cli][usage]")
{
    // The reason a section holds a sequence of blocks: splitting these into two
    // sections would let the halves drift apart the moment one gained a longer
    // term.
    static constexpr auto first = std::to_array<UsageEntry>({ { .term = "--short", .description = "one" } });
    static constexpr auto second = std::to_array<UsageEntry>({ { .term = "--a-very-long-flag", .description = "two" } });
    static constexpr auto blocks = std::to_array<UsageBlock>({
        { .entries = first },
        { .text = "  some prose" },
        { .entries = second },
    });
    static constexpr auto sections = std::to_array<UsageSection>({ { .title = "ENV", .blocks = blocks } });

    auto const lines = Lines(RenderUsage({ .sections = sections }, UsageColor::Plain));
    // Where the description actually starts: past the run of padding that
    // follows the term, not merely where that run begins.
    auto const columnOf = [](std::string const& line) {
        return line.find_first_not_of(' ', line.find("  ", 2));
    };
    REQUIRE(lines.size() == 6);
    CHECK(columnOf(lines[1]) == columnOf(lines[5]));
}

TEST_CASE("continuation lines re-indent to the description column", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({ { .term = "--f", .description = "first\nsecond" } });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .blocks = blocks } });

    auto const lines = Lines(RenderUsage({ .sections = sections }, UsageColor::Plain));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "  --f  first");
    CHECK(lines[1] == "       second");
}

TEST_CASE("a row with no description carries no trailing whitespace", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({
        { .term = "--bare" },
        { .term = "--other", .description = "has one" },
    });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .blocks = blocks } });

    auto const lines = Lines(RenderUsage({ .sections = sections }, UsageColor::Plain));
    CHECK(lines[0] == "  --bare");
}

TEST_CASE("blank lines separate sections and blocks but never follow a title", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({ { .term = "--f", .description = "d" } });
    static constexpr auto firstBlocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto secondBlocks = std::to_array<UsageBlock>({ { .text = "one" }, { .text = "two" } });
    static constexpr auto sections = std::to_array<UsageSection>({
        { .title = "FIRST", .blocks = firstBlocks },
        { .title = "SECOND", .blocks = secondBlocks },
    });

    auto const rendered = RenderUsage({ .sections = sections }, UsageColor::Plain);
    CHECK(rendered == "FIRST\n  --f  d\n\nSECOND\none\n\ntwo\n");
}

TEST_CASE("a title prints its subject uncolored on the same line", "[cli][usage]")
{
    static constexpr auto sections =
        std::to_array<UsageSection>({ { .title = "usage:", .subject = " fastcached [options]" } });

    CHECK(RenderUsage({ .sections = sections }, UsageColor::Plain) == "usage: fastcached [options]\n");

    auto const colored = RenderUsage({ .sections = sections }, UsageColor::Colored);
    CHECK(colored.starts_with("\x1b[1;36musage:\x1b[0m fastcached [options]"));
}

TEST_CASE("substitution runs before the column is measured", "[cli][usage]")
{
    // A token in a *term* changes its width, so expanding after measuring would
    // misalign the row.
    static constexpr auto entries = std::to_array<UsageEntry>({
        { .term = "--port={port}", .description = "listens on {port}" },
        { .term = "--x", .description = "other" },
    });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .blocks = blocks } });
    auto const substitutions = std::to_array<UsageSubstitution>({ { .token = "{port}", .value = "6674" } });

    auto const lines = Lines(RenderUsage({ .sections = sections }, UsageColor::Plain, substitutions));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "  --port=6674  listens on 6674");
    CHECK(lines[1] == "  --x          other");
}

TEST_CASE("unknown braces survive verbatim", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({ { .term = "--f", .description = "use {braces} freely" } });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .blocks = blocks } });

    CHECK(RenderUsage({ .sections = sections }, UsageColor::Plain).contains("{braces}"));
}

TEST_CASE("color adds escapes without changing a single character", "[cli][usage][color]")
{
    // The invariant the whole palette design exists to preserve: escapes are
    // emitted outside the padding computation, so stripping them reproduces the
    // plain rendering exactly.
    static constexpr auto entries = std::to_array<UsageEntry>({
        { .term = "--alpha", .description = "first\nwrapped" },
        { .term = "--b", .description = "second" },
        { .term = "--bare" },
    });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries }, { .text = "  prose" } });
    static constexpr auto sections = std::to_array<UsageSection>({
        { .title = "usage:", .subject = " tool [options]" },
        { .title = "OPTIONS", .blocks = blocks },
    });
    auto const substitutions = std::to_array<UsageSubstitution>({ { .token = "{t}", .value = "v" } });

    auto const plain = RenderUsage({ .sections = sections }, UsageColor::Plain, substitutions);
    auto const colored = RenderUsage({ .sections = sections }, UsageColor::Colored, substitutions);

    CHECK_FALSE(plain.contains('\x1b'));
    CHECK(colored.contains('\x1b'));
    CHECK(StripAnsi(colored) == plain);
}

TEST_CASE("rendered text ends in exactly one newline", "[cli][usage]")
{
    static constexpr auto entries = std::to_array<UsageEntry>({ { .term = "--f", .description = "d" } });
    static constexpr auto blocks = std::to_array<UsageBlock>({ { .entries = entries } });
    static constexpr auto sections = std::to_array<UsageSection>({ { .title = "T", .blocks = blocks } });

    auto const rendered = RenderUsage({ .sections = sections }, UsageColor::Plain);
    REQUIRE(rendered.size() >= 2);
    CHECK(rendered.back() == '\n');
    CHECK(rendered[rendered.size() - 2] != '\n');
}
