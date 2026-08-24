// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// The inputs that determine a shared cache key. Two machines that produce the
/// same `KeyInputs` share a cache entry — so every field must be free of any
/// machine-specific detail (the args are pre-relativized, the preprocessed text
/// has its line markers normalized by the compiler's own preprocessor).
struct KeyInputs
{
    std::string compilerId;                   ///< Stable compiler identity (name + version banner).
    std::string preprocessed;                 ///< Preprocessor output for the TU.
    std::vector<std::string> relativizedArgs; ///< Compile args with checkout-rooted paths relativized.

    /// The dependency paths the compile resolves, in portable form (see
    /// DependencyProbe's KeyDependencySet, which produces this: canonical tokens,
    /// sorted and deduplicated, toolchain paths dropped — a relative path having
    /// been resolved against the compile's working directory first, so that what
    /// it names rather than how it was spelled decides which of the two it is).
    ///
    /// A hit reproduces TWO artefacts — the object and the build system's
    /// dependency record — and without this the key determines only the first.
    /// Suppressed line markers keep every path out of `preprocessed`, which is
    /// what makes a key portable and equally what makes it invariant under a
    /// header MOVE: the token stream is unchanged, so the object is still correct
    /// and served, while the replayed depfile names a file that is gone. The build
    /// system then records that dependency, cannot stat it, rebuilds, hits the same
    /// value, and never converges (issue #53). Naming the paths here makes a move a
    /// different key by construction (issue #56).
    std::vector<std::string> dependencyPaths;
};

/// Compute the shared cache key as a wide hex digest. Isolated here so the key
/// derivation can be tuned against real cross-machine hits without touching the
/// launcher flow.
///
/// A 128-bit MurmurHash3 x64_128 digest (`Core/MurmurHash3.hpp`) over the joined
/// inputs, reached through `KeyDigest`'s grammar: not cryptographic, but
/// collision-negligible for a compile cache and dependency-free. It genuinely is
/// 128 bits — the construction this replaced was four CRC32C runs over one blob
/// differing only by a salt byte, which carried 32 (issue #63).
///
/// The schema version deliberately does not appear here. It is spelled once, in
/// ComputeKey, next to the construction it versions; naming it in prose as well
/// gave two places to update and they had already drifted apart.
/// @param inputs The machine-independent key inputs.
/// @return A 32-hex-char key string.
[[nodiscard]] std::string ComputeKey(KeyInputs const& inputs);

/// Rewrite every argument whose path lies under `sourceRoot` or `buildTree` to
/// a checkout-independent token form (via PathCanon), leaving other arguments
/// untouched. This is what makes the key identical across checkouts at
/// different absolute roots. Handles bare source paths and every flag in
/// `CmdLine`'s PathValueFlags() table — the object output, the include dirs and
/// the depfile options — in both the fused and the separate spelling.
///
/// BOTH spellings, off one table, because the two used to be answered by two:
/// a separated value is an argument of its own and reached the bare-path branch,
/// while a fused one went through a table that listed the include-dir prefixes
/// and nothing else. So `/Fo<abs>` — what every build system driving MSVC
/// writes — kept the producing machine's object path, and two Windows checkouts
/// at different roots could never share an entry, while the separated form used
/// by the unit tests worked and hid it.
///
/// Both roots matter: a checkout commonly nests its build tree *inside* the
/// source root (e.g. `<src>/out/build/...`), and include args frequently point
/// into it (vcpkg headers, generated config). Relativizing only the source root
/// would leave those build-tree paths absolute — and machine-specific — so the
/// key would differ across machines even for identical content. `PathCanon`
/// emits `<BUILDTREE>` for the (longer-matching) build-tree paths and
/// `<SRCROOT>` for the rest.
/// `resolve` reconciles a spelling before the roots are consulted, and it is
/// applied to the path PORTION this function has already isolated rather than to
/// the whole argument — which is the reason it is injected here instead of in the
/// caller: `IncludePrefixes` is the only table that knows where an `-I` flag ends
/// and its path begins, and re-deriving that outside would be a second copy of it.
/// It matters because a build system may spell an include directory in a form the
/// compiler never echoes back (an 8.3 short component, a `subst` drive), and an
/// argument that does not reconcile against the roots leaves the checkout location
/// in the key — issue #66. Left empty, every path is used exactly as written.
/// @param args       The raw compile arguments (excluding the compiler).
/// @param sourceRoot The checkout source root to relativize against.
/// @param buildTree  The build output root to relativize against (may be empty).
/// @param resolve    Optional spelling reconciliation for each isolated path.
/// @return The arguments with source-/build-rooted paths tokenized.
[[nodiscard]] std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                                      std::string_view sourceRoot,
                                                      std::string_view buildTree,
                                                      PathCanon::PathTransform const& resolve = {});

/// The first argument carrying a path this launcher can neither key nor guard, or
/// nothing when the whole command line is safe to cache (issue #104).
///
/// The rule is `IsDriveRelativeUnderNoRoot`'s and is spelled once, there. What
/// this adds is *where* it is asked: on the command line, before the launcher has
/// done anything at all.
///
/// That placement is the substance rather than an optimization. The authoritative
/// answer comes from the paths the compiler actually reported, but that is only
/// known after the preprocess — and direct mode runs *before* it, validating a
/// manifest whose entries came through the same filter and therefore dropped the
/// very path in question. A manifest recorded by an older launcher would keep
/// direct-hitting a stale object forever. Asking here steps out before the
/// manifest is consulted at all, which retires those entries by making them
/// unreachable rather than by bumping a schema tag and invalidating every entry on
/// every platform for a clang-cl-only exposure.
///
/// What it cannot promise is completeness, which is why it is not the only ask:
/// isolating an argument's path depends on `PathValueFlags()` recognising the
/// flag, and `CouldNameAFile` records in as many words that such a list is what
/// cannot be kept complete (`-isystem<path>`, `--sysroot=`, `-B`, a partial
/// `@response-file`). `KeyDependencySet` asks the same question of what the
/// compiler reported, where there is no list to be incomplete.
///
/// Every `PathValueFlags()` role is examined, not only `IncludeDir`. Filtering by
/// role would be a second list to keep complete, and the only cost of examining
/// one that could not have seeded a dependency path is a lost cache on a layout
/// nothing common generates.
///
/// Which introducers may match, and therefore which arguments are bare paths, is
/// the LAYOUT's answer and not the host's — see RelativizeArgs, whose walk this
/// shares.
///
/// @param args   The raw compile arguments (excluding the compiler).
/// @param layout This machine's roots, and the source of path conventions.
/// @return The offending argument, verbatim, or nullopt when there is none.
[[nodiscard]] std::optional<std::string> UnkeyableArgument(std::span<std::string const> args,
                                                           PathCanon::Layout const& layout);

} // namespace FastCache::Cc
