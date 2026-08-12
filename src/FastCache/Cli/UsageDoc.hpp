// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Whether rendered usage text carries ANSI SGR color escapes.
enum class UsageColor : std::uint8_t
{
    Plain,   ///< Plain text — for files, pipes, NO_COLOR, stderr, and tests.
    Colored, ///< ANSI SGR escapes — interactive terminals only; see StdoutSupportsColor.
};

/// The escapes used to colorize usage text.
///
/// Every field is empty in the plain palette, so one renderer drives both
/// forms. Escapes are emitted *outside* the padding computation, which is what
/// makes `StripAnsi(colored) == plain` hold byte for byte and keeps color from
/// ever disturbing column alignment.
struct UsagePalette
{
    std::string_view reset;   ///< Reset all attributes.
    std::string_view heading; ///< Section titles.
    std::string_view term;    ///< The left column of an aligned row.
};

/// Bold-cyan headings, bold-green terms.
inline constexpr UsagePalette ColoredUsagePalette { .reset = "\x1b[0m", .heading = "\x1b[1;36m", .term = "\x1b[1;32m" };

/// No escapes — identical layout, just no color.
inline constexpr UsagePalette PlainUsagePalette { .reset = "", .heading = "", .term = "" };

/// Select the palette for a color mode.
/// @param color Which form to render.
/// @return The matching palette.
[[nodiscard]] constexpr UsagePalette const& PaletteFor(UsageColor color) noexcept
{
    return color == UsageColor::Colored ? ColoredUsagePalette : PlainUsagePalette;
}

/// One aligned row: a term in the left column, its description in the right.
///
/// Every string is a *view*. The tables that hold these are usually
/// `constexpr`, but a document assembled from runtime-formatted strings must
/// keep those strings alive across the RenderUsage call.
struct UsageEntry
{
    std::string_view term {};        ///< Left column: a flag, an invocation form, an environment variable.
    std::string_view description {}; ///< Right column; '\n' starts a continuation line re-indented
                                     ///< to the same column. May be empty.
};

/// One piece of a section: either a run of aligned rows, or free-form prose.
///
/// A section is a *sequence* of these rather than a single list because prose
/// often sits between two runs of rows that must still share one column — the
/// launcher's ENVIRONMENT section is exactly that shape. Modelling those as two
/// sections would let the halves drift apart the moment one gains a longer term.
struct UsageBlock
{
    std::span<UsageEntry const> entries {}; ///< Aligned rows; empty means this is a text block.
    std::string_view text {};               ///< Free-form body emitted verbatim (it carries its own
                                            ///< indentation); used iff `entries` is empty.
};

/// One part of a usage document.
struct UsageSection
{
    std::string_view title {};             ///< Heading, colorized; empty for an untitled section.
    std::string_view subject {};           ///< Printed after `title` on the same line and never
                                           ///< colorized, e.g. " fastcached [options]".
    std::span<UsageBlock const> blocks {}; ///< The body; may be empty for a bare title line.
};

/// A complete usage/help text, as data.
///
/// Layout rules, applied uniformly so every binary lays out identically:
///   - a blank line precedes every section but the first, and every block but
///     the first of its section; none follows a title line;
///   - the description column is `leftIndent + widest term + columnGap`,
///     computed per SECTION across all of its entry blocks, so rows separated
///     by prose still line up with each other;
///   - a row with an empty description emits its term alone, with no trailing
///     whitespace.
struct UsageDocument
{
    std::span<UsageSection const> sections; ///< The sections, in print order.
    std::size_t leftIndent { 2 };           ///< Spaces before each term.
    std::size_t columnGap { 2 };            ///< Spaces between the widest term and the descriptions.
};

/// A value quoted in the text and spliced in at render time.
///
/// Without this a default lives twice — once where it is compiled in, once
/// re-typed as a literal in the help — and the two drift apart silently. Writing
/// `{token}` in any field keeps the compiled value the only place it is spelled.
struct UsageSubstitution
{
    std::string_view token; ///< Placeholder as written, e.g. "{port}".
    std::string value;      ///< Value spliced in its place; may span several lines.
};

/// Render `document` into its final text.
///
/// Substitution runs over titles, subjects, terms, descriptions and text blocks
/// alike, and *before* the column is measured — so a token appearing in a term
/// still lines up. Unknown braces are left verbatim, so ordinary prose needs no
/// escaping.
///
/// @param document The document; every view it holds must outlive this call.
/// @param color Whether to emit ANSI SGR escapes.
/// @param substitutions Tokens to splice; may be empty.
/// @return The rendered text, ending in exactly one '\n' (empty for an empty document).
[[nodiscard]] std::string RenderUsage(UsageDocument const& document,
                                      UsageColor color,
                                      std::span<UsageSubstitution const> substitutions = {});

/// Splice every substitution token in `text` with its value.
///
/// Exposed because an assembler sometimes needs an expanded string before it
/// can build the document (a term whose width depends on a substituted value).
/// @param text Text possibly containing `{token}` placeholders.
/// @param substitutions Tokens to splice.
/// @return `text` with every known token replaced.
[[nodiscard]] std::string ExpandUsageTokens(std::string_view text, std::span<UsageSubstitution const> substitutions);

} // namespace FastCache
