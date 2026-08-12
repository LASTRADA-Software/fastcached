// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/UsageDoc.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{
    /// Invoke `fn(line)` for each '\n'-separated segment of `text`. A trailing
    /// segment with no newline is still delivered, so a non-terminated string
    /// yields exactly its visual lines.
    /// @param text The text to split.
    /// @param fn Callable invoked once per line.
    template <typename Fn>
    void ForEachLine(std::string_view text, Fn fn)
    {
        while (true)
        {
            auto const newline = text.find('\n');
            if (newline == std::string_view::npos)
            {
                fn(text);
                return;
            }
            fn(text.substr(0, newline));
            text.remove_prefix(newline + 1);
        }
    }

    /// One block with every token already spliced, so the column can be
    /// measured from the text that will actually be printed.
    struct ExpandedBlock
    {
        std::vector<std::pair<std::string, std::string>> rows; ///< Term/description pairs; empty for a text block.
        std::string text;                                      ///< Free-form body; used iff `rows` is empty.
    };

    /// Expand every string in `section` up front.
    /// @param section The section to expand.
    /// @param substitutions Tokens to splice.
    /// @return One ExpandedBlock per block of the section, in order.
    [[nodiscard]] std::vector<ExpandedBlock> ExpandBlocks(UsageSection const& section,
                                                          std::span<UsageSubstitution const> substitutions)
    {
        std::vector<ExpandedBlock> expanded;
        expanded.reserve(section.blocks.size());
        for (auto const& block: section.blocks)
        {
            ExpandedBlock out;
            if (block.entries.empty())
                out.text = ExpandUsageTokens(block.text, substitutions);
            else
            {
                out.rows.reserve(block.entries.size());
                for (auto const& entry: block.entries)
                    out.rows.emplace_back(ExpandUsageTokens(entry.term, substitutions),
                                          ExpandUsageTokens(entry.description, substitutions));
            }
            expanded.push_back(std::move(out));
        }
        return expanded;
    }

    /// The description column for a section: wide enough for its widest term
    /// across *every* entry block, so rows separated by prose still line up.
    /// @param blocks The section's expanded blocks.
    /// @param document Supplies the indent and gap widths.
    /// @return The column at which descriptions start.
    [[nodiscard]] std::size_t DescriptionColumn(std::span<ExpandedBlock const> blocks, UsageDocument const& document)
    {
        std::size_t widest = 0;
        for (auto const& block: blocks)
            for (auto const& [term, description]: block.rows)
                widest = std::max(widest, term.size());
        return document.leftIndent + widest + document.columnGap;
    }

    /// Append one aligned row, re-indenting continuation lines to `column`.
    /// @param out Destination.
    /// @param row The term/description pair.
    /// @param column Where descriptions start.
    /// @param document Supplies the indent width.
    /// @param palette Escapes to wrap the term in.
    void EmitRow(std::string& out,
                 std::pair<std::string, std::string> const& row,
                 std::size_t column,
                 UsageDocument const& document,
                 UsagePalette const& palette)
    {
        auto const& [term, description] = row;
        std::string const indent(document.leftIndent, ' ');

        // A row with no description is its term alone: padding it would leave
        // trailing whitespace on the line.
        if (description.empty())
        {
            out += std::format("{}{}{}{}\n", indent, palette.term, term, palette.reset);
            return;
        }

        auto firstLine = true;
        ForEachLine(description, [&](std::string_view line) {
            if (firstLine)
            {
                firstLine = false;
                // The pad is derived from the term's own width, never from the
                // rendered string: escapes must not shift the column.
                out += std::format("{}{}{}{}{}{}\n",
                                   indent,
                                   palette.term,
                                   term,
                                   palette.reset,
                                   std::string(column - document.leftIndent - term.size(), ' '),
                                   line);
            }
            else
                out += std::format("{}{}\n", std::string(column, ' '), line);
        });
    }
} // namespace

std::string ExpandUsageTokens(std::string_view text, std::span<UsageSubstitution const> substitutions)
{
    std::string out { text };
    for (auto const& [token, value]: substitutions)
        for (auto at = out.find(token); at != std::string::npos; at = out.find(token, at + value.size()))
            out.replace(at, token.size(), value);
    return out;
}

std::string RenderUsage(UsageDocument const& document, UsageColor color, std::span<UsageSubstitution const> substitutions)
{
    auto const& palette = PaletteFor(color);

    std::string out;
    auto firstSection = true;
    for (auto const& section: document.sections)
    {
        if (!std::exchange(firstSection, false))
            out += '\n';

        auto const title = ExpandUsageTokens(section.title, substitutions);
        auto const subject = ExpandUsageTokens(section.subject, substitutions);
        if (!title.empty())
            out += std::format("{}{}{}{}\n", palette.heading, title, palette.reset, subject);
        else if (!subject.empty())
            out += std::format("{}\n", subject);

        auto const blocks = ExpandBlocks(section, substitutions);
        auto const column = DescriptionColumn(blocks, document);

        auto firstBlock = true;
        for (auto const& block: blocks)
        {
            if (!std::exchange(firstBlock, false))
                out += '\n';

            if (block.rows.empty())
            {
                if (!block.text.empty())
                    ForEachLine(block.text, [&](std::string_view line) { out += std::format("{}\n", line); });
                continue;
            }

            for (auto const& row: block.rows)
                EmitRow(out, row, column, document, palette);
        }
    }
    return out;
}

} // namespace FastCache
