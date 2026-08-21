// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    /// DependencyProbe's KeyDependencySet, which produces this: canonical tokens
    /// and relative paths, sorted and deduplicated, toolchain absolutes dropped).
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
/// launcher flow. v1 uses a 128-bit multi-lane CRC32C digest over the joined
/// inputs — not cryptographic, but collision-negligible for a compile cache and
/// dependency-free.
/// @param inputs The machine-independent key inputs.
/// @return A 32-hex-char key string.
[[nodiscard]] std::string ComputeKey(KeyInputs const& inputs);

/// Rewrite every argument whose path lies under `sourceRoot` or `buildTree` to
/// a checkout-independent token form (via PathCanon), leaving other arguments
/// untouched. This is what makes the key identical across checkouts at
/// different absolute roots. Handles bare source paths and the `-I` / `/I` /
/// `/external:I` include-dir forms (fused or separate value).
///
/// Both roots matter: a checkout commonly nests its build tree *inside* the
/// source root (e.g. `<src>/out/build/...`), and include args frequently point
/// into it (vcpkg headers, generated config). Relativizing only the source root
/// would leave those build-tree paths absolute — and machine-specific — so the
/// key would differ across machines even for identical content. `PathCanon`
/// emits `<BUILDTREE>` for the (longer-matching) build-tree paths and
/// `<SRCROOT>` for the rest.
/// @param args       The raw compile arguments (excluding the compiler).
/// @param sourceRoot The checkout source root to relativize against.
/// @param buildTree  The build output root to relativize against (may be empty).
/// @return The arguments with source-/build-rooted paths tokenized.
[[nodiscard]] std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                                      std::string_view sourceRoot,
                                                      std::string_view buildTree);

} // namespace FastCache::Cc
