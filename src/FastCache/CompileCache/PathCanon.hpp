// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace FastCache::PathCanon
{

/// A build's absolute roots on one machine. Both are native-form absolute
/// paths (backslash separators on Windows).
struct Layout
{
    std::string sourceRoot; ///< Checkout root — becomes the `<SRCROOT>` sentinel.
    std::string buildTree;  ///< Build output root — becomes the `<BUILDTREE>` sentinel.
};

/// Failure modes of a canonicalize/localize call.
///
/// There is deliberately no version here. Canonical text only ever travels
/// inside a CompileValue, whose container carries `CompileValueVersion` and is
/// rejected on mismatch, so a change to the canonicalization spec is expressed
/// by bumping that — and by the `objkey-v4` schema tag in the launcher's
/// ComputeKey, which re-keys the cache so stale entries miss rather than being
/// localized under rules they were not written by. A second version here was
/// declared once and never referenced by anything, because it had no work to do.
enum class CanonError : std::uint8_t
{
    Malformed, ///< Token/path is structurally invalid (e.g. an empty sentinel tail).
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

/// True for an ASCII letter — the only thing a Windows drive specifier may start
/// with, and the single definition of that rule.
///
/// Compared directly rather than via std::isalpha, which is locale-dependent and
/// would make every drive test environment-sensitive: a cache is shared across
/// machines, so a rule that varies with the running process's locale is a rule
/// two machines can disagree about. It lives in the header rather than beside the
/// first caller because there are four of them and they had drifted — three
/// hand-written letter tests plus this one, two of them through std::isalpha.
/// Each still adds whatever further test its own question needs (see
/// AnchorForLayout, which also asks what follows the colon, against the depfile
/// walkers, which deliberately do not).
///
/// @param c Input byte.
/// @return True for A-Z or a-z.
[[nodiscard]] constexpr bool IsDriveLetter(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// Lower-case an ASCII byte, for the case-insensitive half of Windows path
/// comparison.
///
/// Here for the same reason and with the same force as IsDriveLetter:
/// std::tolower is locale-dependent, and case-folding decides whether a header
/// lies under a root. Under a Turkish locale `std::tolower('I')` is not `i`, so a
/// root spelled `D:\PROJECT\Inc` folds differently there than on a C-locale
/// machine — one classifies a header as project content and canonicalizes it, the
/// other calls it toolchain and drops it. Two machines then derive different
/// manifests and different dependency sets from byte-identical content, which is
/// the one failure this whole layer exists to prevent.
///
/// @param c Input byte.
/// @return The lower-case form for A-Z, else the byte unchanged.
[[nodiscard]] constexpr char AsciiLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// True when `layout` describes a Windows build tree.
///
/// The answer comes from the layout's own roots, never from the host: a cache is
/// shared across machines, so path conventions are a property of the data rather
/// than of the running binary. A root is Windows-shaped when it uses backslash
/// separators or begins with a drive-letter prefix (`C:`); anything else is
/// POSIX. The drive-letter test requires an ASCII letter before the colon, so a
/// relative POSIX root like `a:b/proj` is not mistaken for a drive.
///
/// This is the single definition of "is this a Windows layout". SeparatorOf and
/// the launcher's option-prefix test both derive from it, so the two can never
/// disagree about a root such as `C:/src/proj`, which uses forward slashes but
/// is still Windows.
///
/// @param layout The layout whose path conventions apply.
/// @return True when either root is Windows-shaped.
[[nodiscard]] bool IsWindowsLayout(Layout const& layout) noexcept;

/// What a path a compiler emitted is anchored to — the property that decides
/// whether another machine can make sense of it.
///
/// Three states rather than the two "absolute or relative" suggests, because
/// Windows has a third: `C:foo` carries a drive specifier yet is *not* rooted.
/// It resolves against that drive's own current directory, which is per-process
/// state on the producing machine that no cache entry records — so it is neither
/// a fixed location nor something a consumer may resolve against its own working
/// directory. Naming it makes each caller decide what to do about it; a boolean
/// only let them inherit whichever mistake the predicate happened to make.
/// The states are exactly the distinctions the two callers act on, which is why
/// there is no fourth. A Windows path that is rooted but carries no drive
/// (`\Windows\x.h`) is *also* not a fixed location — it names that path on the
/// process's current drive — yet it is reported Absolute, because the only
/// question either caller puts to this is whether the path may be resolved
/// against the compile's working directory, and for `\Windows\x.h` the answer is
/// no, same as for `C:\Windows\x.h`. Splitting it out would add a state neither
/// caller could do anything different with, which is the mistake CanonError's
/// absent version field already records. `DriveRelative` earns its place by the
/// opposite test: both callers *do* treat it differently from both neighbours.
enum class Anchor : std::uint8_t
{
    WorkingDirectory, ///< Relative: resolves against the compile's working directory.
    Absolute,         ///< Not resolvable against a working directory: `/x`, `C:\x`,
                      ///< `C:/x`, `\\host\share\x`, and `\x` (current drive, see above).
    DriveRelative,    ///< `C:foo`, or a bare `C:`: resolves against that DRIVE's
                      ///< current directory. Portable to nothing, checkable by nobody.
};

/// Classify `path` under the conventions `layout` describes.
///
/// Derived from the LAYOUT, never from the host: a cache is shared across
/// machines, so `D:\src\a.hpp` must read as absolute even when this binary runs
/// on POSIX — std::filesystem::path::is_absolute() would say otherwise and send
/// every Windows path down the relative branch. IsWindowsLayout is the single
/// definition of "is this a Windows layout"; this derives from it for the same
/// reason the launcher's option-prefix test does.
///
/// The drive test requires a separator (or end of string) after `C:`, exactly as
/// IsWindowsRoot's does and for a related reason: without it every drive-relative
/// path reads as absolute. This used to be a `bool IsAbsoluteForLayout`, and it
/// did precisely that — the two callers of the day still reached a safe outcome, but
/// by way of an answer that was not true, which is not a property a later caller
/// inherits (issue #65; the manifest became that third caller immediately after).
///
/// Lives here rather than in a caller because several of them need exactly this
/// question answered exactly this way — the replay guard, deciding which replayed
/// dependencies this machine is answerable for; the launcher's key dependency
/// filter, deciding which of them are portable enough to hash; and the direct-mode
/// manifest, deciding which it may record and re-hash. Each switches on the result
/// without a `default:`, so a fourth state added here is a compile error at every
/// one of them rather than a silent fall-through.
///
/// @param path   A path as a compiler spelled it. Empty reads as
///               Anchor::WorkingDirectory; both callers reject empty before this.
/// @param layout The layout whose path conventions apply.
/// @return The path's anchor.
[[nodiscard]] Anchor AnchorForLayout(std::string_view path, Layout const& layout) noexcept;

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

/// The transform RewritePaths applies to one path span.
using PathTransform = std::function<std::string(std::string_view)>;

/// Apply a caller-supplied transform to every path span in a captured text
/// region, per its grammar, preserving everything else byte-for-byte.
///
/// The generalization Canonicalize/LocalizeRegion are special cases of, exposed
/// because a caller outside this file needs the SAME grammar to find the same
/// spans: the launcher must reconcile the spelling of each path a compiler
/// emitted (an 8.3 short component, a `subst` drive, a junction) before the value
/// is stored, or the daemon's canonicalization finds nothing under either root
/// and the value keeps the producing machine's absolute paths — issue #66.
///
/// The transform is deliberately opaque here, and it decides per span, which is
/// what lets a caller preserve one path while rewriting its neighbours — the
/// launcher does exactly that for a depfile's rule target, which is the `-o` path
/// the BUILD SYSTEM named rather than anything the driver reported.
///
/// Reconciling a spelling means asking the filesystem, and this file must never
/// do that: it also runs on the DAEMON, over a producing machine's roots that do
/// not exist there. So the grammar stays here and the filesystem stays in the
/// launcher.
///
/// @param text    The region bytes.
/// @param grammar The grammar identifying path spans within `text`.
/// @param xform   Applied once per identified span; returning the span unchanged
///                is a byte-exact no-op.
/// @return The rewritten region.
[[nodiscard]] std::expected<std::string, CanonError> RewritePaths(std::string_view text,
                                                                  Grammar grammar,
                                                                  PathTransform const& xform);

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
