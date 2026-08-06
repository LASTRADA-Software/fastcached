// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace FastCache::PathCanon
{

/// The canonicalization spec version this implementation targets. A stored
/// canonical form is only interpretable by a localizer implementing the same
/// version.
inline constexpr std::uint32_t CanonSpecVersion = 1;

/// A build's absolute roots on one machine. Both are native-form absolute
/// paths (backslash separators on Windows).
struct Layout
{
    std::string sourceRoot; ///< Checkout root — becomes the `<SRCROOT>` sentinel.
    std::string buildTree;  ///< Build output root — becomes the `<BUILDTREE>` sentinel.
};

/// Failure modes of a canonicalize/localize call.
enum class CanonError : std::uint8_t
{
    UnknownVersion, ///< Token carries a spec version this implementation cannot interpret.
    Malformed,      ///< Token/path is structurally invalid (e.g. an empty sentinel tail).
};

/// The line grammars that locate path spans inside captured compiler text
/// output. Each captured region is tagged with the grammar the producer knows
/// applies to it; the canonicalizer only rewrites spans that grammar identifies.
enum class Grammar : std::uint8_t
{
    ShowIncludes,    ///< MSVC/clang-cl `/showIncludes`: `Note: including file: <path>`.
    MsvcDiagnostics, ///< Diagnostics: `<path>(line[,col]): ...` — the leading path.
    GccDepfile,      ///< GCC/Clang `-MF` depfile: `target: dep dep \` continuation.
};

/// Rewrite a single absolute path to its canonical token form.
/// @param absolutePath Native-form absolute path (as the compiler emitted it).
/// @param layout       The producing machine's roots.
/// @return The canonical token (`<SRCROOT>/...`, `<BUILDTREE>/...`, or the input
///         verbatim when under neither root), or CanonError on a malformed input.
[[nodiscard]] std::expected<std::string, CanonError> Canonicalize(std::string_view absolutePath, Layout const& layout);

/// Rewrite a canonical token back to an absolute path for the given layout.
/// @param token  A token previously produced by Canonicalize.
/// @param layout The consuming machine's roots.
/// @return The localized native-form absolute path, or CanonError.
[[nodiscard]] std::expected<std::string, CanonError> Localize(std::string_view token, Layout const& layout);

/// Canonicalize every path span in a captured text region, per its grammar.
/// Lines that do not match the grammar's shape are preserved byte-for-byte,
/// including their line endings.
/// @param text    The captured region bytes (e.g. `/showIncludes` stdout).
/// @param grammar The grammar identifying path spans within `text`.
/// @param layout  The producing machine's roots.
/// @return The region with matched path spans replaced by canonical tokens.
[[nodiscard]] std::expected<std::string, CanonError> CanonicalizeRegion(std::string_view text,
                                                                        Grammar grammar,
                                                                        Layout const& layout);

/// Localize every canonical token span in a captured text region, per its
/// grammar. The inverse of CanonicalizeRegion.
/// @param text    The canonical region bytes.
/// @param grammar The grammar identifying token spans within `text`.
/// @param layout  The consuming machine's roots.
/// @return The region with tokens replaced by localized native paths.
[[nodiscard]] std::expected<std::string, CanonError> LocalizeRegion(std::string_view text,
                                                                    Grammar grammar,
                                                                    Layout const& layout);

} // namespace FastCache::PathCanon
