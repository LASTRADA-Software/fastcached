// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

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
/// - A *relative* path is kept, normalized to forward slashes. It resolves
///   against the compile's working directory, so it names the same file on any
///   machine that runs the same build. Note this must be decided before the
///   toolchain test rather than after it, since that test reports every path
///   outside the roots as toolchain.
/// - A Windows **drive-relative** path (`C:foo`) is emphatically *not* treated as
///   relative. It resolves against drive C's own current directory — per-process
///   state on the producing machine that no cache entry records — so hashing it
///   alongside the genuinely relative paths would let two machines whose C: cwd
///   differs produce the *same* key for *different* headers. `PathCanon::Anchor`
///   is what separates the two; before issue #65 the classifier stopped at the
///   colon and called it absolute, reaching the same outcome by an answer that
///   was not true. It is then left to the root tests rather than dropped outright,
///   because root membership is the stronger question: under no root it cannot
///   prefix-match a rooted root and is dropped as toolchain regardless, while
///   under a drive-relative *root* it canonicalizes to a token that is portable
///   exactly because the consumer substitutes its own root.
/// - **Toolchain content is dropped**, judged by DirectManifest's
///   `IsToolchainHeader` so that this filter, the manifest's and the replay
///   guard's cannot disagree: an absolute path under neither root, *and* a vcpkg
///   tree nested under the build tree, which canonicalizes but is still the
///   producing machine's. It is covered collectively by the compiler identity
///   already in the key, and hashing it would mean two machines with the same
///   compiler but different install prefixes (`/usr/include/c++/16` against
///   `/opt/gcc-16/include`) share *nothing at all* — a full duplicate entry set
///   for every translation unit. This is the same split DirectManifest makes for
///   the same reason (see its header: 476 of a real TU's 635 headers are
///   toolchain, and "a manifest naming them would be machine-specific").
///
/// The result is sorted and deduplicated. `/showIncludes` repeats a header once
/// per inclusion site — hundreds of notes for a few dozen files — and sorting
/// makes the key insensitive to emission-order differences between driver
/// versions, which are not differences in what was compiled.
///
/// Pure: touches no filesystem.
///
/// @param rawPaths The dependency paths as the compiler spelled them.
/// @param layout   This machine's roots, and the source of path conventions.
/// @return The portable dependency set, sorted, without duplicates.
[[nodiscard]] std::vector<std::string> KeyDependencySet(std::span<std::string const> rawPaths,
                                                        PathCanon::Layout const& layout);

} // namespace FastCache::Cc
