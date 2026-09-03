// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/// Rewriting absolute paths to portable tokens, and back.
///
/// **Nothing here can fail, and that is a contract rather than an accident.** A
/// path under neither root is not an error: it is echoed verbatim, which is what
/// lets a toolchain path survive a round trip unchanged. So every entry point
/// returns a plain `std::string`, and the question a caller actually has — "was a
/// token produced?" — is answered by comparing the result against the input, never
/// by a sentinel this namespace keeps private. `DependencyProbe`'s `RootToken` is
/// the canonical spelling of that test.
///
/// These functions returned `std::expected<std::string, CanonError>` until issues
/// #59 and #69. No code path ever produced the error, so every `has_value()` check
/// on a result read as a handled failure mode that was not one, and the wire status
/// those checks fed (`CanonicalizationFailed`, `0x06`) was a documented part of the
/// 0xFC protocol that no server could ever send.
///
/// **There is deliberately no version here either.** Canonical text only ever
/// travels inside a CompileValue, whose container carries `CompileValueVersion` and
/// is rejected on mismatch, so a change to the canonicalization spec is expressed by
/// bumping that — and by the `objkey-v6` schema tag in the launcher's ComputeKey,
/// which re-keys the cache so stale entries miss rather than being localized under
/// rules they were not written by. A version here was declared once and never
/// referenced by anything, because it had no work to do; `header-state-v1` is the
/// same lesson, recorded in `.agent/rules/compile-cache.md`.
namespace FastCache::PathCanon
{

/// A build's absolute roots on one machine. Both are native-form absolute
/// paths (backslash separators on Windows).
struct Layout
{
    std::string sourceRoot; ///< Checkout root — becomes the `<SRCROOT>` sentinel.
    std::string buildTree;  ///< Build output root — becomes the `<BUILDTREE>` sentinel.
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

/// A copy of @p text with every ASCII letter folded down.
///
/// The whole-string form of the byte-level one above, here rather than re-spelled
/// per caller: there were five copies of this three-line `ranges::transform`, each
/// carrying its own note about `std::tolower` being locale-sensitive, and a rule
/// written down five times is a rule four of them can drift from.
///
/// @param text What to fold.
/// @return The folded copy.
[[nodiscard]] inline std::string AsciiLower(std::string_view text)
{
    std::string folded { text };
    std::ranges::transform(folded, folded.begin(), [](char c) { return AsciiLower(c); });
    return folded;
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

/// Where a path stands in relation to a root.
///
/// A reason rather than a `bool`, because the middle state is what the caller most
/// needs to say out loud: it is a root spelled almost right, repaired by editing a
/// root rather than by moving a file.
enum class RootRelation : std::uint8_t
{
    Outside,  ///< No relation: not even a character-wise prefix match.
    NearMiss, ///< A character-wise prefix of the root, but not on a segment
              ///< boundary — `/home/dev/project-x/a.hpp` against `/home/dev/proj`.
    Under,    ///< Under the root, on a segment boundary.
};

/// Where `path` stands in relation to `root`, judged on a **segment boundary**.
///
/// This is the single definition of "is this path under this root", and it exists
/// because there was briefly more than one. `Canonicalize` has always asked it
/// segment-wise, while the launcher's `IsToolchainHeader` asked it with a bare
/// `starts_with` — so under a source root `/home/dev/proj` the sibling directory
/// `/home/dev/project-x/a.hpp` was project content to the classifier and under no
/// root to the canonicalizer (issue #562). That failed safe, the classifier being
/// the more permissive of the two, but the key filter, the manifest and the replay
/// guard all judge by that one classifier *so that they cannot disagree* — and an
/// invariant that is false in one of the places it is reasoned from is not
/// available anywhere. The bug is the missing boundary check, so the fix is that
/// there is one place the check can be missing from.
///
/// `NearMiss` is that bug's only survivor and is deliberate: the state used to fall
/// out of the gap between the two predicates, and it is the one root fault an
/// operator repairs by editing a *root*, so closing the gap without naming it would
/// have sent them looking for a file that is exactly where they put it.
///
/// Both arguments are **native** forms — the spellings a compiler and a build
/// system emit, mixed separators and mixed case included. Folding them is this
/// function's job precisely so a caller holding native paths does not fold its own
/// comparison form and get the boundary byte wrong; `Canonicalize` needs the folded
/// roots for other reasons and so keeps its own internal spelling of the same test.
///
/// An empty root relates to nothing: a layout that names no build tree must not
/// make every path in the world lie under it, nor a near miss of it.
///
/// @param path A path in native form.
/// @param root A layout root in native form.
/// @return The relation.
[[nodiscard]] RootRelation RelateToRoot(std::string_view path, std::string_view root);

/// Where `path` stands in relation to a layout's two roots, strongest answer first:
/// `Under` if it is under either, else `NearMiss` if it near-misses either, else
/// `Outside`.
///
/// Not a loop over `RelateToRoot` at the call site, and the ORDER is why: a build
/// tree spelled as the source root's sibling (`/w/src` and `/w/src-other`) makes a
/// path both a near miss of one root and legitimately under the other, and that
/// path is project content rather than a misspelling. Folding the path once instead
/// of once per root is the other reason — this is on the launcher's per-dependency
/// path, which runs a few hundred times per translation unit.
///
/// @param path   A path in native form.
/// @param layout The roots to relate it to.
/// @return The strongest relation to either root.
[[nodiscard]] RootRelation RelateToLayout(std::string_view path, Layout const& layout);

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
/// caller could do anything different with, which is the mistake `header-state-v1`
/// records: a distinction with no work to do. `DriveRelative` earns its place by
/// the opposite test: both callers *do* treat it differently from both neighbours.
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
/// @return The canonical token (`<SRCROOT>/...`, `<BUILDTREE>/...`), or the input
///         verbatim when under neither root — which is how a caller tells the two
///         apart, there being nothing here that can fail.
[[nodiscard]] std::string Canonicalize(std::string_view absolutePath, Layout const& layout);

/// Rewrite a canonical token back to an absolute path for the given layout.
/// @param token  A token previously produced by Canonicalize.
/// @param layout The consuming machine's roots.
/// @return The localized native-form absolute path, or the token verbatim when it
///         names no sentinel this layout knows.
[[nodiscard]] std::string Localize(std::string_view token, Layout const& layout);

/// Canonicalize every path span in a captured text region, per its grammar.
/// Lines that do not match the grammar's shape are preserved byte-for-byte,
/// including their line endings.
/// @param text    The captured region bytes (e.g. `/showIncludes` stdout).
/// @param grammar The grammar identifying path spans within `text`.
/// @param layout  The producing machine's roots.
/// @return The region with matched path spans replaced by canonical tokens.
[[nodiscard]] std::string CanonicalizeRegion(std::string_view text, Grammar grammar, Layout const& layout);

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
[[nodiscard]] std::string RewritePaths(std::string_view text, Grammar grammar, PathTransform const& xform);

/// Localize every canonical token span in a captured text region, per its
/// grammar. The inverse of CanonicalizeRegion.
/// @param text    The canonical region bytes.
/// @param grammar The grammar identifying token spans within `text`.
/// @param layout  The consuming machine's roots.
/// @return The region with tokens replaced by localized native paths.
[[nodiscard]] std::string LocalizeRegion(std::string_view text, Grammar grammar, Layout const& layout);

} // namespace FastCache::PathCanon
