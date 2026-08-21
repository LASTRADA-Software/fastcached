// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Cc
{

/// Whether a cached compile's *dependency record* is still true on this machine.
///
/// A cache hit reproduces two artefacts from one key: the object file and the
/// build system's dependency record (a GNU depfile, or the `/showIncludes` notes
/// Ninja parses as `deps = msvc`). The key determines only the first. Preprocessing
/// suppresses line markers (`-E -P` / `/EP`, see CmdLine.cpp) precisely so that a
/// checkout path never reaches the key — which is what makes a key portable, and
/// also what makes it **invariant under a header move**. The object is a function of
/// the token stream and is still correct after such a move; the depfile is nothing
/// but paths and is not.
///
/// Replaying a dependency record that names a file this machine does not have is
/// worse than a miss: the build system records the dependency, cannot stat it, marks
/// the target dirty, rebuilds, hits the same cached value, and never converges — with
/// a successful exit code every time (issue #53). So a hit re-checks the paths it is
/// about to assert, exactly as ValidateManifest re-checks a direct-mode manifest
/// before trusting it, and a hit that cannot be honoured here is discarded rather
/// than served.

/// The paths a replayed hit asserts this machine has: first-seen order, deduplicated,
/// filtered to the ones this build is actually responsible for.
///
/// **Which paths are checked, and why each exclusion is load-bearing.** Every one of
/// these was a way to turn a perfectly good hit into a miss:
///
/// - The rule target of a depfile is not a dependency — and it is the *object file*,
///   which does not exist yet because the caller is about to write it. Probing it
///   would make every single hit a miss. ParseDepFilePaths already excludes it (and
///   `-MP` phony rules), which is why it is reused here rather than a whitespace
///   split written.
/// - A path that still carries a canonical sentinel (`<SRCROOT>/...`) is one the
///   region walker declined to localize — the extractors accept slightly more than
///   PathCanon's grammars do — so it names nothing on any machine and is skipped.
/// - An absolute path outside both roots is toolchain or system content, covered
///   collectively by the toolchain stamp. Checking it would break *convergence*, not
///   merely hit rate: machine A stores `/usr/include/c++/15/...`, machine B probes it,
///   misses, and re-stores `/opt/gcc/...`, whereupon A misses too — two machines
///   missing on every compile, forever.
/// - A *relative* path is kept. It resolves against the compile's working directory,
///   which is also the launcher's, so it is always this machine's path. Note this must
///   be decided before the toolchain test, which reports every relative path as
///   outside the roots.
///
/// Pure: touches no filesystem.
///
/// @param localizedRegions The decoded value's text regions, already localized to
///                         this machine (canonical tokens are not paths).
/// @param layout           This machine's roots, and the source of path conventions.
/// @return The paths to require, in first-seen order, without duplicates.
[[nodiscard]] std::vector<std::string> ReplayedDependencyPaths(std::span<TextRegion const> localizedRegions,
                                                               PathCanon::Layout const& layout);

/// The first replayed dependency this machine does not have.
///
/// Short-circuits: one missing path is enough to discard the hit, and a large
/// translation unit names hundreds. A path that cannot be examined at all (an
/// unclassifiable name, a permission error) counts as **present** — refusing a hit
/// over a question we could not answer would trade a real speed-up for a guess.
///
/// @param localizedRegions The decoded value's text regions, already localized.
/// @param layout           This machine's roots.
/// @param workingDirectory What relative paths resolve against. A parameter rather
///                         than the process working directory so tests can drive it
///                         without mutating global state; the launcher passes its own
///                         cwd, which is the compile's.
/// @return The missing path, or nullopt when every dependency resolves.
[[nodiscard]] std::optional<std::string> MissingReplayedDependency(std::span<TextRegion const> localizedRegions,
                                                                   PathCanon::Layout const& layout,
                                                                   std::filesystem::path const& workingDirectory);

} // namespace FastCache::Cc
