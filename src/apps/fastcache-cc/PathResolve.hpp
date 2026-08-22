// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Collapses every spelling of one filesystem location to a single one.
///
/// This is the launcher's PATH-IDENTITY seam, and it exists because every root
/// test in the launcher is a string prefix comparison — `IsToolchainHeader`,
/// `PathCanon::CanonicalizeOne`, and through them the keyed dependency set and
/// the replay guard. A comparison form that lowercases and unifies separators
/// still cannot tell that `C:\Users\RUNNER~1\p\inc\h.h` and
/// `C:\Users\runneradmin\p\inc\h.h` are the same file, so a root spelled one way
/// matches nothing a driver spelling it the other way emits.
///
/// That is not hypothetical and not limited to 8.3. Measured on a GitHub
/// `windows-2025` runner, where `%TEMP%` is the short form, the two MSVC drivers
/// disagree **within one build**: `cl` resolves an include through the filesystem
/// and reports the LONG name, while `clang-cl` echoes the spelling it was handed
/// and reports the SHORT one. A `subst` drive, a junctioned build tree, a
/// symlinked checkout and a UNC share reached by drive letter all do the same
/// thing. The failure is silent three times over — the keyed dependency set is
/// empty, the replay guard skips every path it exists to probe, and the stored
/// value keeps the producing machine's absolute paths — so the launcher reports
/// ordinary hits while a moved header keys identically (issue #66).
///
/// The fix is to run BOTH sides through the same function: the layout roots once
/// at startup, and every path a compiler emits before it is compared against
/// them. Resolving only one side is strictly worse than resolving neither — it
/// breaks the driver that was previously working — which is why this is a seam of
/// its own rather than a call sprinkled at each comparison. It is also why
/// `DirectManifest`'s warning against `weakly_canonical` does not apply here: that
/// warning is about rewriting one side of a comparison, and this rewrites both.
///
/// It lives in the launcher, not in `PathCanon`, on purpose: `PathCanon` also
/// runs on the DAEMON, over a producing machine's roots that do not exist there,
/// so it must never touch a filesystem. See `CompileCacheHandler::HandleStore`.
class IPathResolver
{
  public:
    IPathResolver() = default;
    virtual ~IPathResolver() = default;
    IPathResolver(IPathResolver const&) = delete;
    IPathResolver& operator=(IPathResolver const&) = delete;
    IPathResolver(IPathResolver&&) = delete;
    IPathResolver& operator=(IPathResolver&&) = delete;

    /// The one spelling this host uses for whatever `path` names.
    ///
    /// Total, and never throwing: anything that cannot be resolved — a build tree
    /// that does not exist yet, an argument that was never a path at all — comes
    /// back verbatim, which is exactly today's behaviour for it. A RELATIVE path
    /// also comes back verbatim, deliberately: it resolves against the compile's
    /// working directory and is therefore already machine-independent, and
    /// absolutizing it here would either re-key it needlessly or push it outside
    /// both roots and have it dropped from the key altogether.
    ///
    /// @param path A path as a compiler, a build system, or the environment spelled it.
    /// @return The host's own spelling, or `path` unchanged.
    [[nodiscard]] virtual std::string Resolve(std::string_view path) = 0;

    /// The same, for a path the caller already knows names a DIRECTORY.
    ///
    /// The split is not cosmetic, it is what makes Resolve() affordable: Resolve()
    /// memoizes the parent and appends the leaf, so a directory passed to it would
    /// have its own final component left unresolved — and a layout root whose last
    /// component is the short-named one (`...\Temp\CC-L-~1`) is exactly the shape
    /// this module exists for. The caller always knows which it is holding: a
    /// layout root is a directory, an include note is a file.
    ///
    /// @param path A directory path.
    /// @return The host's own spelling, or `path` unchanged.
    [[nodiscard]] virtual std::string ResolveDirectory(std::string_view path) = 0;

    /// How many times the filesystem was actually consulted so far.
    ///
    /// Reported rather than assumed, because the cost of resolving every emitted
    /// path is the open question issue #66 raises: the count against the number of
    /// paths handled is what says whether the per-directory memo is doing its job
    /// (a real translation unit reports ~635 headers from a few dozen directories).
    /// @return The call count.
    [[nodiscard]] virtual std::size_t FilesystemCalls() const noexcept = 0;
};

/// Create the resolver for the host platform.
///
/// The returned resolver memoizes on the PARENT DIRECTORY, which is what makes
/// per-path resolution affordable: a real translation unit's 635 headers live in a
/// few dozen directories, so the filesystem is asked once per directory rather
/// than once per header. The leaf name is appended as spelled — it comes from an
/// `#include` directive, so it is already the long form, and resolving it would
/// cost one call per file to answer a question nothing asks. A leaf that IS
/// spelled short therefore still keys as itself: two machines would then derive
/// two keys for one header, which costs a miss and never a mis-serve.
///
/// @return A resolver backed by the real filesystem.
[[nodiscard]] std::unique_ptr<IPathResolver> MakePathResolver();

} // namespace FastCache::Cc
