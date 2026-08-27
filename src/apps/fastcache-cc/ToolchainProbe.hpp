// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "IProcessRunner.hpp"
#include "ToolchainFingerprint.hpp"
#include "ToolchainHost.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Discovering a toolchain's include tree, and digesting it into a fingerprint.
///
/// Split the way `PathCanon` and `CompileValue` already are: the parsing is pure
/// and unit-tested against captured driver output, and the filesystem walk is a
/// separate function that the pure half never calls. That matters more here than
/// usual, because the inputs this has to get right — an Xcode SDK layout, a
/// Windows developer prompt's `INCLUDE`, a GCC install with a version-suffixed
/// directory — cannot all exist on whatever machine runs the tests.

/// Extract the system include search paths a GNU driver prints under `-v`.
///
/// The driver frames the list between two literal markers and indents each entry
/// by one space. Everything outside those markers is other verbose noise — the
/// target triple, the configure line, the assembler invocation — and must not be
/// read as a path.
///
/// A `(framework directory)` suffix is stripped and the path kept: on macOS the
/// SDK's framework directories are genuine search paths, and dropping them would
/// silently narrow the fingerprint on the one platform where they carry the
/// system headers for Objective-C++ translation units.
///
/// @param verboseOutput The driver's stderr (the list is not printed on stdout).
/// @return The search paths in the order the driver listed them, which is
///         significant to the compiler and preserved here even though the digest
///         sorts — this function answers "what does it search", not "what is the
///         fingerprint".
[[nodiscard]] std::vector<std::string> ParseGnuIncludeSearchPaths(std::string_view verboseOutput);

/// Split an MSVC `INCLUDE` environment variable into search paths.
///
/// Semicolon-separated, which is the separator the variable uses on Windows
/// regardless of what the host running this parser thinks a path list looks like.
/// Empty entries are dropped: a trailing or doubled separator is ordinary in a
/// value built up by several `vcvars` invocations, and an empty path is not a
/// directory anyone can walk.
///
/// @param value The raw variable value.
/// @return The search paths, in order.
[[nodiscard]] std::vector<std::string> ParseIncludeEnvironment(std::string_view value);

/// Walk include search roots and digest every file under them.
///
/// **This is the I/O half** — it opens files, and it is deliberately not
/// something the pure digest calls.
///
/// Each file is recorded by its path RELATIVE to the root it was found under,
/// which is what lets two machines running the same toolchain at different
/// install prefixes agree. A root that does not exist is skipped rather than
/// failing: a driver lists search paths it would use if they existed, and a
/// missing one is normal (`/usr/local/include` on a machine that has none).
///
/// Cost is the reason the caller must cache this. Measured on an Xcode
/// toolchain: 14,600 files and 288 MB, about 2 s warm — per launcher invocation
/// that would dwarf the compile it is trying to accelerate.
///
/// @param roots Include search paths, as a driver reported them.
/// @return One entry per readable file, unsorted (the digest sorts).
[[nodiscard]] std::vector<ToolchainFile> ProbeToolchainFiles(std::span<std::string const> roots);

/// The compiler's own version banner: its first `--version` line.
///
/// Shared rather than private to the launcher because the compile node needs the
/// identical string. The node derives its own fingerprint from the compiler it
/// was configured with, and a fingerprint is a digest OF this banner among other
/// things -- so two spellings of "what does this compiler call itself" would put
/// a worker and its clients permanently out of agreement, with no error anywhere,
/// just a scheduler that never finds a match.
///
/// Falls back to `NormalizedCompilerName` (in `CmdLine.hpp`) when the compiler
/// cannot be run or says nothing. A weak identity beats an empty one: an empty
/// banner would make every unrunnable compiler look like every other. Where that
/// fallback is ALSO the whole identity, `ToolchainIdentity::degenerate` says so.
///
/// @param runner Process-spawning seam.
/// @param compiler The compiler to ask.
/// @return The banner line, or the basename.
[[nodiscard]] std::string CompilerBanner(IProcessRunner& runner, std::string const& compiler);

/// Whether a name is made only of digits and dots, with at least one digit.
///
/// One predicate for two questions that must answer alike: which subdirectory of a
/// Windows SDK's `Include` is a kit version (`10.0.26100.0` yes, `KitsRoot10` and
/// the GUID-named values no), and which suffix on a compiler name is a version
/// (`gcc-13` yes, `gcc-ar` no). A second spelling is a second set of rules for what
/// counts as a version.
///
/// @param name The candidate.
/// @return True when it could be a version.
[[nodiscard]] bool LooksLikeVersion(std::string_view name) noexcept;

/// The VC half of an MSVC toolchain's include roots, from its own install layout.
///
/// Derived from the COMPILER'S PATH rather than from anything ambient, which is
/// what lets a service -- with no `INCLUDE` and no developer prompt -- reach the
/// same answer a build system does. A bare name is resolved on the search path
/// first, so `cl` and `C:\...\cl.exe` land on one set of roots.
///
/// The toolset root is found by walking up from the compiler until an ancestor's
/// PARENT is named `MSVC`, which is the `VC\Tools\MSVC\<version>` layout every
/// Visual Studio since 2017 uses. Counting a fixed number of levels would break on
/// a cross-targeting `bin\Hostx64\arm64` the moment somebody used one.
///
/// The roots and their order mirror what `vcvarsall` puts in `INCLUDE` --
/// `<toolset>\include`, `<toolset>\atlmfc\include`, `<vs>\VC\Auxiliary\VS\include`
/// -- so the two mechanisms agree on a machine where both can answer. A root that
/// is not present is skipped rather than emitted: `ProbeToolchainFiles` would skip
/// it anyway, and listing it would only make the roots harder to read in a log.
///
/// @param host The machine's filesystem and search path.
/// @param compiler The compiler being identified, bare name or path.
/// @return Its VC include roots; empty when the layout cannot be determined.
[[nodiscard]] std::vector<std::string> MsvcToolsetIncludeRoots(IToolchainHost& host, std::string const& compiler);

/// The Windows SDK half of an MSVC toolchain's include roots.
///
/// Separate from the VC half because it is a separate install with a separate
/// version, located through the registry rather than through the compiler: nothing
/// about a `cl.exe` says which SDK it will be used with.
///
/// The kit ROOT comes from the registry -- nothing else knows where it is -- and the
/// kit VERSION from the subdirectories of `<root>/Include`. Not from the value names
/// under `Installed Roots`: only a version with an `Include` directory is usable, so
/// the listing is already the complete set of answers and a registry name could add
/// none. (Telling an installed kit from a directory an uninstall left behind is a
/// real question the registry could answer, but it needs its names treated as
/// authoritative rather than unioned in, and that changes which kit is chosen.)
///
/// **The HIGHEST installed kit is chosen, not the one the build selected**, and the
/// imprecision is deliberate rather than overlooked. A build pins its kit through
/// `INCLUDE`, `vcvarsall <version>` or `WindowsTargetPlatformVersion`, none of which
/// a service can see -- so honouring it would put the launcher and the worker back
/// on different answers, which is the failure `MsvcLayout` exists to close. Picking
/// the highest is a rule both ends reach identically from the machine alone.
///
/// What that gives up is bounded, and bounded by the shape of a dispatched compile:
/// a worker compiles text that is ALREADY PREPROCESSED, so no SDK header is opened
/// on the far side and the kit a machine pinned cannot change the object that comes
/// back. Two machines pinned to different kits therefore look interchangeable here
/// and genuinely are, for this purpose.
///
/// @param host The machine's filesystem and registry.
/// @return The SDK include roots for the highest installed kit; empty when no kit
///         is installed, and always empty off Windows.
[[nodiscard]] std::vector<std::string> WindowsKitIncludeRoots(IToolchainHost& host);

/// Ask a driver where it searches for system headers.
///
/// Dispatches on `spec.includeDiscovery` with no `default:`, so a mechanism added
/// to the table is a compile error here rather than a silent empty result.
///
/// Every failure yields an empty list rather than an error: discovery is
/// best-effort by construction. A toolchain whose paths cannot be discovered
/// falls back to a banner-only fingerprint, which is weaker but still correct in
/// the direction that matters -- it can only cause two genuinely-identical
/// toolchains to be treated as identical, never two different ones.
///
/// @param runner Process-spawning seam.
/// @param host The machine's filesystem, registry and environment.
/// @param compiler The compiler to interrogate.
/// @param spec The driver's table row.
/// @return Search paths in the driver's own order; empty when undiscoverable.
[[nodiscard]] std::vector<std::string> DiscoverIncludePaths(IProcessRunner& runner,
                                                            IToolchainHost& host,
                                                            std::string const& compiler,
                                                            DriverSpec const& spec);

/// A cheap check that a cached fingerprint still describes this toolchain.
///
/// Digested rather than stored field-by-field, so validation is a string compare
/// and the cache file needs no parser -- a format with a parser is a format that
/// can be misparsed, and this one is written and read by short-lived processes
/// racing each other.
///
/// What it covers, and what it deliberately does not. The compiler binary's size
/// and mtime catch a toolchain UPGRADE, which is the case that actually happens.
/// Each search root's own mtime catches headers being added or removed. Neither
/// catches a header edited IN PLACE without changing any directory -- accepted,
/// because a system toolchain's headers are installed rather than edited, and the
/// alternative is the 2-second full walk this exists to avoid. `--print-toolchain
/// -fingerprint` recomputes unconditionally for when someone needs to be sure.
///
/// @param banner The compiler's version line.
/// @param compiler Path to the compiler binary.
/// @param roots Its include search roots.
/// @return A hex digest, or empty when the compiler cannot be stat'd at all.
[[nodiscard]] std::string ComputeToolchainStamp(std::string_view banner,
                                                std::string const& compiler,
                                                std::span<std::string const> roots);

/// A toolchain fingerprint, and whether it says anything about WHICH compiler it is.
struct ToolchainIdentity
{
    std::string fingerprint; ///< The digest a client must match. Never empty.

    /// True when the digest carries no information about which compiler this is.
    ///
    /// Reported HERE because this is the only place both halves are known, and
    /// reconstructing it outside meant guessing which branch `CompilerBanner` took
    /// and deriving the include roots a second time. The condition: the driver has
    /// a way to find its include roots, that way found none, AND the banner is
    /// itself the fallback name. All three, because each alone is ordinary --
    /// `IncludeDiscovery::None` has no roots by construction, a real version banner
    /// is an identity whatever its roots, and a fallback banner over a located
    /// include tree is exactly the MSVC case that works.
    ///
    /// Together they are not a weak identity but no identity:
    /// `KeyDigest("toolchain-v1").Field("cl")` is a value this repository could
    /// print with no compiler installed, and every MSVC toolset in existence
    /// produces it. The "weaker but still correct" argument above for a
    /// banner-only fingerprint has an unstated precondition -- that the banner is a
    /// real version string -- and this is that precondition, checked.
    bool degenerate { false };
};

/// A toolchain fingerprint, computed once per machine and remembered.
///
/// The full walk costs about 2 seconds over 288 MB on an ordinary Xcode
/// toolchain. The launcher runs once per translation unit, so without this cache
/// the fingerprint would cost far more than the compile it exists to distribute
/// -- which is why the cache is part of the design rather than an optimization.
///
/// Concurrency is handled by tolerating it rather than locking: a cold cache and
/// `-j16` means sixteen launchers all walk the tree and all write the answer.
/// They write the SAME answer, the write is atomic (temp file plus rename), and
/// duplicated work once per machine is cheaper than a lock protocol between
/// short-lived processes that must never deadlock a build.
///
/// @param runner Process-spawning seam.
/// @param host The machine's filesystem, registry and environment.
/// @param compiler Path to the compiler.
/// @param banner Its version line, already obtained by the caller.
/// @param spec The driver's table row.
/// @param forceRefresh Skip the cached value and rewrite it.
/// @return The fingerprint and whether it means anything; the digest is never
///         empty (it degrades to a banner-only one).
[[nodiscard]] ToolchainIdentity CachedToolchainFingerprint(IProcessRunner& runner,
                                                           IToolchainHost& host,
                                                           std::string const& compiler,
                                                           std::string_view banner,
                                                           DriverSpec const& spec,
                                                           bool forceRefresh = false);

} // namespace FastCache::Cc
