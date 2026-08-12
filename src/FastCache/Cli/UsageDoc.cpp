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
    /// One block with every token already spliced, so the column can be
    /// measured from the text that will actually be printed.
    struct ExpandedBlock
    {
        std::vector<std::pair<std::string, std::string>> rows; ///< Term/description pairs; empty for a text block.
        std::string text;                                      ///< Free-form body; used iff `rows` is empty.
        std::size_t indent {};                                 ///< Spaces before each non-empty line of `text`.
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
            out.indent = block.indent;
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

        out.append(document.leftIndent, ' ');
        out += std::format("{}{}{}", palette.term, term, palette.reset);

        // A row with no description is its term alone: padding it would leave
        // trailing whitespace on the line.
        if (description.empty())
        {
            out += '\n';
            return;
        }

        auto firstLine = true;
        ForEachLine(description, [&](std::string_view line) {
            // The first line's pad is derived from the term's own width, never
            // from the rendered string: escapes must not shift the column.
            out.append(firstLine ? column - document.leftIndent - term.size() : column, ' ');
            firstLine = false;
            out += line;
            out += '\n';
        });
    }
} // namespace

void UsageRows::Add(std::string term, std::string_view description)
{
    _entries.push_back({ .term = _terms.emplace_back(std::move(term)), .description = description });
}

std::span<UsageEntry const> UsageRows::Rows() const noexcept
{
    return _entries;
}

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
                    ForEachLine(block.text, [&](std::string_view line) {
                        // An empty line stays empty: indenting it would be
                        // trailing whitespace and nothing else.
                        if (!line.empty())
                            out.append(block.indent, ' ');
                        out += line;
                        out += '\n';
                    });
                continue;
            }

            for (auto const& row: block.rows)
                EmitRow(out, row, column, document, palette);
        }
    }
    return out;
}

} // namespace FastCache
