// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliValue.hpp"

#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// How an answer is written to stdout.
///
/// Human is the default because the overwhelmingly common caller is a person at an
/// SSH session; the other four exist so the same command composes with `jq`, a
/// spreadsheet, and a shell loop without a second tool and without this one growing
/// a second data path.
///
/// The default is **not** switched on whether stdout is a TTY. That would make a
/// command's output shape change when it is piped, which is the kind of thing that
/// works interactively and breaks in the script somebody wrote by copying it. Colour
/// is TTY-conditional (that is conventional and cannot change a parse); the format is
/// not.
enum class OutputFormat : std::uint8_t
{
    Human, ///< Aligned columns, a dash for absent, optional colour.
    Json,  ///< One JSON document. Absent is `null`.
    Kv,    ///< `name=value` lines.
    Tsv,   ///< Tab-separated, with a header line.
    Csv,   ///< RFC 4180 comma-separated, with a header line.
    Last,
};

/// One output format's fixed properties.
struct FormatSpec
{
    OutputFormat format;         ///< The enumerator this row describes.
    std::string_view name;       ///< The `--format=` spelling.
    std::string_view absentText; ///< What an absent cell renders as, unless overridden.
    std::string_view summary;    ///< One-line help text.
};

/// The formats, one row per enumerator, in enumerator order.
///
/// `Json`'s `absentText` is empty and unused: JSON has a real null and that is what
/// an absent cell becomes there. It is the one format `--absent` deliberately does
/// not affect -- a consumer with `null` available does not need a sentinel, and
/// letting a flag turn a null into the string `"-"` would hand them a value that
/// parses and lies.
inline constexpr EnumTable<OutputFormat, FormatSpec> FormatTable { {
    { .format = OutputFormat::Human,
      .name = "human",
      .absentText = "-",
      .summary = "aligned columns for a person (the default)" },
    { .format = OutputFormat::Json, .name = "json", .absentText = "", .summary = "one JSON document; absent is null" },
    { .format = OutputFormat::Kv, .name = "kv", .absentText = "", .summary = "name=value lines" },
    { .format = OutputFormat::Tsv, .name = "tsv", .absentText = "", .summary = "tab-separated, with a header line" },
    { .format = OutputFormat::Csv, .name = "csv", .absentText = "", .summary = "RFC 4180 CSV, with a header line" },
} };

static_assert(RowsInEnumeratorOrder(FormatTable, &FormatSpec::format),
              "FormatTable must hold one row per OutputFormat, in enumerator order");

/// The row describing @p format.
/// @param format The format.
/// @return Its row; never null for a value below `Last`.
[[nodiscard]] FormatSpec const* DescriptorOf(OutputFormat format) noexcept;

/// The format @p name spells.
/// @param name The `--format=` value as typed.
/// @return The format, or nullopt when the name matches no row.
[[nodiscard]] std::optional<OutputFormat> FormatFromName(std::string_view name) noexcept;

/// Every accepted `--format=` spelling, comma-separated.
///
/// Derived from the table rather than restated, so a refusal cannot name a set that
/// has drifted from the set actually accepted.
/// @return The list, e.g. `human, json, kv, tsv, csv`.
[[nodiscard]] std::string FormatNames();

/// How the answer should be written.
struct RenderOptions
{
    OutputFormat format { OutputFormat::Human };  ///< Which format.
    UsageColor color { UsageColor::Plain };       ///< Colour, for Human only.
    std::optional<std::string> absentOverride {}; ///< From `--absent`; ignored by Json.
};

/// Render an answer.
///
/// Total: every shape and every format produce something, because a formatter that
/// can only handle the shapes it expected answers one of them anyway when it meets a
/// new one.
///
/// @param value The answer. Must satisfy `WellFormed`.
/// @param options How to write it.
/// @return The rendered text, ending in a newline unless it is empty.
[[nodiscard]] std::string RenderValue(Value const& value, RenderOptions const& options);

/// Quote one field for CSV per RFC 4180.
///
/// Exposed because it is the one piece of the CSV writer with a rule that is easy to
/// get subtly wrong (a quote is doubled, and a field containing a separator, a quote
/// or a newline must be quoted), so it is tested directly rather than only through a
/// whole document.
/// @param field The raw field text.
/// @return The field, quoted if it needs to be.
[[nodiscard]] std::string QuoteCsvField(std::string_view field);

/// Escape one field for TSV, by the rule `/fleet.txt` states.
///
/// `\t`, `\n`, `\r` and `\` are spelled out; nothing else is touched, and nothing is
/// quoted. TSV has no RFC 4180 to point at, so the reasoning for that rule -- and for
/// escaping rather than quoting -- is not restated here: it lives in the `/fleet.txt`
/// section of `docs/tools/fastcache-compile-node.md`, and this function takes the same
/// four in the same spelling. **Two products, one format, one spelling**: the node
/// serves those tables and this client renders `--format=tsv`, and an operator pipes
/// both into the same script, so a second convention here would be the one-spelling
/// rule broken across a process boundary -- where nothing in the build can catch it.
///
/// **The backslash is load-bearing rather than tidy.** Without it the escaping is not
/// injective: a value holding a literal `\t` and one holding an actual tab would both
/// render `\t`, and no reader could tell which it had. That is a different defect
/// wearing this one's fix as clothes. What carries the property is the row being
/// PRESENT, never where it sits: every byte is looked up once, in one pass, so row
/// order cannot matter -- and a reader who thinks it does will preserve an ordering
/// nothing depends on.
///
/// Exposed for the same reason `QuoteCsvField` is -- a rule that is easy to get subtly
/// wrong is tested directly rather than only through a whole document.
/// @param field The raw field text.
/// @return The field with those four characters spelled out.
[[nodiscard]] std::string EscapeTsvField(std::string_view field);

} // namespace FastCache::Cli
