// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Turning a preprocess probe's output into a cache-key input.
///
/// A cache hit reproduces two artefacts from one key: the object file and the
/// build system's dependency record. Until now the key was a function of only the
/// first. Preprocessing suppresses line markers (`-E -P` / `/EP`, see CmdLine.cpp)
/// so a checkout path never reaches the key — which is what makes a key portable,
/// and equally what makes it **invariant under a header move**. Move a header
/// without changing a byte and the token stream is identical: the object is still
/// correct and is served, while the depfile, which is nothing but paths, names a
/// file that is gone. That is worse than a miss, because the build system records
/// the dependency, cannot stat it, rebuilds, hits the same value, and never
/// converges — with a successful exit code every time (issue #53).
///
/// ReplayGuard detects that at replay time. Folding the dependency set into the
/// key removes the class instead: a moved header is a different key by
/// construction, and existence never has to stand in for identity (issue #56).

/// A preprocess probe's output with the two things it carries separated.
///
/// They arrive interleaved on ONE stream for clang-cl, whose `/showIncludes`
/// notes go to stdout — the same stream the preprocessed text uses. A note line
/// that survived into `preprocessed` would be hashed into the cache key as though
/// it were source, and it names an absolute path, which is exactly what
/// suppressing line markers exists to keep out of a key.
struct ProbeText
{
    std::string preprocessed;           ///< The remaining bytes: what the key hashes.
    std::vector<std::string> notePaths; ///< The paths the removed notes named.
};

/// Split `/showIncludes` note lines out of a captured stream.
///
/// Lines that are not notes are preserved byte-for-byte, line endings included,
/// exactly as PathCanon's region walkers preserve non-matching lines: the result
/// is hashed, so a normalization here would be a silent re-keying.
///
/// Recognises a note exactly as ParseIncludePaths does, because both call
/// `IncludeNotePath` — the *rule*, not merely the marker, is what the two have
/// to share. It is anchored after leading blanks (`cl` indents by nesting depth)
/// and nowhere else: a rule matching the marker mid-line would delete an ordinary
/// source line that merely contains the text from the hashed bytes, so two
/// revisions differing only in such a string literal would key identically.
///
/// Pure: touches no filesystem.
///
/// @param text A captured stream that may carry include notes.
/// @return The stream without its note lines, and the paths those notes named,
///         in emission order with duplicates preserved.
[[nodiscard]] ProbeText SplitIncludeNotes(std::string_view text);

/// What became of one path the probe reported.
///
/// The filter below has one way to keep a path and five ways to drop it, and for
/// a long time the launcher's note reported only how many of each there were in
/// total. `0 of M` was meant to be the fingerprint of the issue #66 short-name
/// root mismatch — a root spelled with an 8.3 component prefix-matches nothing
/// `cl` echoes back, so every path classifies as outside both roots — and that
/// mattered because #66 is silent from every other direction: the key is quietly
/// empty, the replay guard quietly skips, and the stored value quietly keeps the
/// producing machine's absolute paths. Issue #65 gave the same line a second
/// cause (a drive-relative path under no root also drops), after which it
/// fingerprinted neither. Naming the reason is what separates them again
/// (issue #105).
///
/// A DIAGNOSTIC vocabulary, not a decision: nothing here changes which paths
/// reach the key, and the enumerators exist so a drop has somewhere to be counted
/// rather than so a caller can branch on one.
enum class PathDisposition : std::uint8_t
{
    Keyed,         ///< Canonicalized to a token and hashed.
    Empty,         ///< The driver reported an empty path; there is nothing to classify.
    Unanchored,    ///< Relative, and the working directory could not place it.
    DriveRelative, ///< Anchored to a drive's own current directory AND under no root, so
                   ///< nothing here can place it. Both halves are required: a path under
                   ///< a drive-relative ROOT canonicalizes to a portable token, so it is
                   ///< Keyed — or, inside a vendored tree, the Toolchain it genuinely is.
    Toolchain,     ///< Under neither root, or matching a toolchain marker: content the
                   ///< compiler identity in the key already covers collectively.
    Uncanonical,   ///< Rooted by IsToolchainHeader's character-wise prefix test, but
                   ///< Canonicalize's segment-wise one declined — `/x/build-other/a.h`
                   ///< against a `/x/build` root. A root spelled almost right.
    Last,          ///< Not a disposition, and has no row: the table's length.
};

/// One row per disposition: the enumerator, and the word the note uses for it.
struct DispositionRow
{
    PathDisposition disposition; ///< The outcome this row names.
    std::string_view label;      ///< How the launcher's note spells it.
};

/// The disposition table, in enumerator order so a disposition indexes its own row.
///
/// A table rather than a `switch`, for the reason `Metrics/MetricsCatalog` is one:
/// a disposition that can be counted but not named is a drop that renders as
/// nothing, which is the defect this whole change exists to close.
///
/// Sized from `Last` rather than from a literal or from the final enumerator by
/// name, and that is the half that has to be right. An enumerator appended after
/// `Uncanonical` while `Last` still ended the enum would leave a table one row
/// short that a `size() == Uncanonical + 1` assert happily accepts — and
/// `KeyDependencySet` indexes `tally` by the enumerator, so the new disposition
/// writes one past the end. Deriving the length instead makes the missing row
/// value-initialize to `{ Keyed, "" }` at a non-zero index, which the coverage
/// check below rejects. `MetricsCatalog` sizes `CounterTable` from `Counter::Last`
/// for exactly this reason.
inline constexpr std::array<DispositionRow, static_cast<std::size_t>(PathDisposition::Last)> DispositionTable { {
    { .disposition = PathDisposition::Keyed, .label = "keyed" },
    { .disposition = PathDisposition::Empty, .label = "empty" },
    { .disposition = PathDisposition::Unanchored, .label = "unanchored" },
    { .disposition = PathDisposition::DriveRelative, .label = "drive-relative" },
    { .disposition = PathDisposition::Toolchain, .label = "toolchain" },
    { .disposition = PathDisposition::Uncanonical, .label = "no canonical form" },
} };

/// Whether the table has one row per enumerator, in order.
/// @return True when every `PathDisposition` has its own row.
[[nodiscard]] consteval bool CoversEveryDisposition() noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, DispositionTable.size()), [](std::size_t index) {
        return static_cast<std::size_t>(DispositionTable[index].disposition) == index;
    });
}

static_assert(CoversEveryDisposition(),
              "DispositionTable must hold one row per PathDisposition, in enumerator order -- the order is what lets "
              "a disposition index its own tally slot and its own row");

/// The portable dependency set, and why each path that is not in it is not.
struct DependencySet
{
    /// The tokens the key hashes: sorted, deduplicated.
    std::vector<std::string> keyed;

    /// One count per PathDisposition, indexed by the enumerator.
    ///
    /// Counted per reported OCCURRENCE, so the whole tally sums to what the probe
    /// reported — while `keyed` is the set after sort and deduplication, which is
    /// smaller whenever a header was reached from more than one inclusion site.
    /// `/showIncludes` repeats one per site, hundreds of notes for a few dozen
    /// files, so the two genuinely differ on every real translation unit. The
    /// launcher's note has always reported the deduplicated count against the raw
    /// one; this states the relationship rather than introducing it.
    std::array<std::size_t, DispositionTable.size()> tally {};

    /// How many reported paths reached one disposition.
    /// @param disposition The outcome to read.
    /// @return The count.
    [[nodiscard]] std::size_t Count(PathDisposition disposition) const noexcept
    {
        return tally[static_cast<std::size_t>(disposition)];
    }

    /// How many paths the probe reported, derived from the tally rather than
    /// carried beside it: two counters for one fact are two counters that drift.
    /// @return The sum of every row.
    [[nodiscard]] std::size_t Reported() const noexcept
    {
        return std::accumulate(tally.begin(), tally.end(), std::size_t { 0 });
    }
};

/// Render the reasons paths did not reach the key, for the launcher's note.
///
/// Walks DispositionTable in order, skipping `Keyed` — which the note's own
/// `N of M` prefix already reports — and every row that counted nothing, so a
/// healthy compile says only what happened. Empty when nothing was dropped.
///
/// Lives here rather than in main.cpp because main.cpp is in no test target, the
/// lesson `CacheProtocol.cpp` and `RootReconciler.cpp` are each recorded as having
/// been extracted for.
///
/// @param set A classified dependency set.
/// @return `"9 toolchain, 3 drive-relative"`, or an empty string.
[[nodiscard]] std::string DescribeDropped(DependencySet const& set);

/// Whether `path` is a Windows drive-relative path that no root can tokenize.
///
/// The one shape this launcher can neither key nor check, and therefore the one
/// that makes a whole compile uncacheable (issue #104). `C:foo` resolves against
/// drive C's *own* current directory, per-process state on the producing machine
/// that no cache entry records. Two filters meet on it and each is right on its
/// own terms (issue #65): `PortableForm` drops it, and `ReplayGuard`'s
/// `IsCheckable` skips it because there is no working directory it could
/// truthfully be stat'ed against. Nothing anywhere asked whether SOMETHING
/// covered the path, and under no root nothing does — move a header spelled that
/// way and the key does not change, so the stale entry is still found, and
/// nothing probes it, so nothing notices: a replayed depfile naming a file that
/// is gone, under a zero exit code.
///
/// So the compile is refused instead, the way a module interface unit is: it
/// costs those builds the cache and cannot mis-serve. Resolving the path at
/// capture time is the other direction and is rejected — it would record the
/// producing machine's per-drive current directory in the value, which is exactly
/// the machine-specific state a key exists to keep out.
///
/// **This is for a path no `PortableForm` classification covers**, which since
/// issue #105 means the COMMAND LINE and nothing else. A reported dependency path
/// already carries `PathDisposition::DriveRelative`, computed by the same rule one
/// layer down, so `RunCached` reads that tally rather than asking again — two
/// spellings of one classification are two places for it to drift, which is the
/// defect `DispositionTable` above exists to prevent. What the tally cannot answer
/// is a path the compiler never reported because the argument carrying it was
/// never split out, and that is what `Cc::UnkeyableArgument` uses this for.
///
/// The anchor is asked FIRST, which is what makes it free everywhere it cannot
/// fire: `AnchorForLayout` never reports `DriveRelative` under a POSIX layout, so
/// the whole rule is inert there without a single canonicalization. Under a
/// **drive-relative root** (`C:src\proj`) such a path canonicalizes to a token and
/// is *not* refused — that layout is portable precisely because the consumer
/// substitutes its own root, and refusing it would un-key a layout that works.
///
/// Inequality against the input is what says a token was produced, never a
/// sentinel spelling PathCanon keeps private — the same test `PortableForm`,
/// `ProjectToken` and `RootReconciler::Translate` each apply.
///
/// Pure: touches no filesystem. A drive-relative path is one of the few a
/// filesystem could not place anyway.
///
/// @param path   A path as a compiler or a build system spelled it.
/// @param layout This machine's roots, and the source of path conventions.
/// @return True when this compile must not be cached because of `path`.
[[nodiscard]] bool IsDriveRelativeUnderNoRoot(std::string_view path, PathCanon::Layout const& layout);

/// Reduce a probe's raw dependency paths to the portable set the key hashes.
///
/// **Which paths survive, and why the exclusion is load-bearing.** Every field of
/// a key must be free of machine-specific detail, or two checkouts of the same
/// content stop sharing entries:
///
/// Every path is put through DirectManifest's `NormalizePath` first, so a `..`
/// segment or a mixed separator cannot make one header into two entries — or, on
/// two machines whose generators spell an include directory differently, into two
/// keys for identical content. Then:
///
/// - A path under either root canonicalizes to a `<SRCROOT>/...` or
///   `<BUILDTREE>/...` token and is kept. These are the project's own headers —
///   the ones that move, and the whole reason this set exists.
/// - A *relative* path is **resolved** against the compile's working directory and
///   then classified exactly like the absolute paths above, rather than kept for its
///   spelling. That is issue #64, and the whole of it is argued below.
/// - A Windows **drive-relative** path (`C:foo`) is emphatically *not* treated as
///   relative, and so is never resolved that way. It resolves against drive C's own
///   current directory — per-process state on the producing machine that no cache
///   entry records — so resolving it against the compile's working directory would
///   name a file that was never read, and hashing its spelling alongside the
///   genuinely relative paths would let two machines whose C: cwd differs produce the
///   *same* key for *different* headers. `PathCanon::Anchor` is what separates the
///   two; before issue #65 the classifier stopped at the colon and called it
///   absolute, reaching a safe outcome by an answer that was not true. It is then
///   left to the root tests rather than dropped outright, because root membership is
///   the stronger question: under no root it cannot prefix-match a rooted root and is
///   dropped as toolchain regardless, while under a drive-relative *root* it
///   canonicalizes to a token that is portable exactly because the consumer
///   substitutes its own root.
/// - **Toolchain content is dropped**, judged by DirectManifest's
///   `IsToolchainHeader` so that this filter, the manifest's and the replay
///   guard's cannot disagree *about an absolute path*: one under neither root,
///   *and* a vcpkg tree nested under the build tree, which canonicalizes but is
///   still the producing machine's. (About a *relative* one the three part, and
///   each answer is deliberate: only this filter resolves before it decides;
///   `ReplayGuard`'s `IsCheckable` keeps every relative path, which is right for an
///   existence probe against this machine's own cwd; and `BuildManifest` refuses
///   the whole manifest rather than dropping the path, because it has no working
///   directory to resolve against and a manifest that silently lost its headers
///   revalidates the source alone. Anything moved into a shared helper has to
///   account for all three.) It is covered collectively by the compiler identity
///   already in the key, and hashing it would mean two machines with the same
///   compiler but
///   different install prefixes (`/usr/include/c++/16` against
///   `/opt/gcc-16/include`) share *nothing at all* — a full duplicate entry set
///   for every translation unit. This is the same split DirectManifest makes for
///   the same reason (see its header: 476 of a real TU's 635 headers are
///   toolchain, and "a manifest naming them would be machine-specific").
///
/// **A relative path is classified by what it resolves to, never by its
/// spelling** (issue #64), which is why this needs a working directory at all.
/// Both rules above ask what a path *names*; "is it absolute" answers a different
/// question, and the two invert for a vendored or relocatable toolchain reached
/// through a relative include path. `-I../../vendor/sdk/include` — the ordinary
/// way a vendored SDK is referenced from a build directory — makes the driver
/// report `../../vendor/sdk/include/foo.h`, which lies under no root, so a
/// spelling test *keeps and hashes* it ahead of the toolchain rule that drops the
/// very same file when the driver spells it absolutely. Two machines whose build
/// directory sits at a different depth then key every translation unit that
/// touches that SDK differently — the "same compiler, share nothing at all"
/// outcome the toolchain exclusion exists to prevent, arrived at from the other
/// side. Resolving first makes both branches one branch: a relative project
/// header becomes the same token its absolute spelling would, and a relative
/// toolchain header is dropped exactly as an absolute one is.
///
/// What that gives up is what ReplayGuard still covers: the key no longer
/// re-keys when a *vendored* header moves, but a hit's replayed dependency record
/// is probed for existence before it is written, so the moved path discards the
/// hit rather than being replayed. That is the same trade, and the same backstop,
/// the absolute toolchain exclusion has always made.
///
/// The result is sorted and deduplicated. `/showIncludes` repeats a header once
/// per inclusion site — hundreds of notes for a few dozen files — and sorting
/// makes the key insensitive to emission-order differences between driver
/// versions, which are not differences in what was compiled.
///
/// Pure: touches no filesystem. The resolution is lexical for that reason and for
/// a second one — `weakly_canonical` would rewrite an 8.3 short component to its
/// long form, which is precisely what DirectManifest's `NormalizePath` documents
/// itself as avoiding.
///
/// @param rawPaths         The dependency paths as the compiler spelled them.
/// @param layout           This machine's roots, and the source of path
///                         conventions.
/// @param workingDirectory What a relative path resolves against: the compile's
///                         working directory, which is also the launcher's. A
///                         parameter rather than the process working directory
///                         for the reason MissingReplayedDependency takes one —
///                         so tests can drive a synthetic directory without
///                         mutating global state. A *string* rather than a
///                         `std::filesystem::path` because the join is the
///                         LAYOUT's, not the host's: `path::operator/` would
///                         apply the running machine's rules to a path belonging
///                         to a cache shared with machines that do not share
///                         them. The replay guard's is a path because its join
///                         ends in a real filesystem probe, where the layout and
///                         the host *are* the same machine. A working directory
///                         that is itself empty or relative drops every relative
///                         path rather than guessing: a path this machine cannot
///                         classify is a path it cannot hash portably, and the
///                         launcher's `dependency set: N of M` note is what makes
///                         that visible instead of silent.
/// @return The portable dependency set — sorted, without duplicates — beside a
///         per-reason tally of everything that did not reach it. The tally is
///         what makes `0 of M` say WHY rather than merely how many, which two
///         separate faults had come to render identically (issue #105).
[[nodiscard]] DependencySet KeyDependencySet(std::span<std::string const> rawPaths,
                                             PathCanon::Layout const& layout,
                                             std::string_view workingDirectory);

} // namespace FastCache::Cc
