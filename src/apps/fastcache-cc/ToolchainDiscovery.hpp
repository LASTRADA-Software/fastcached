// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "IProcessRunner.hpp"
#include "ToolchainHost.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// One compiler a machine turned out to have.
struct ToolchainCandidate
{
    /// Where it lives, as an absolute path.
    ///
    /// A PATH and a provenance, and deliberately nothing else. Discovery adds
    /// CANDIDATES; it must never add a second way to decide what a compiler IS.
    /// Carrying a flavour here would be exactly that -- a second answer, computed at
    /// survey time, for a question `ClassifyCompiler` answers at fingerprint time
    /// from the path alone. A worker that derived its flavour, and through it its
    /// fingerprint, differently from its clients would register successfully,
    /// heartbeat happily, and never be matched, with nothing anywhere reporting why.
    std::string compiler;

    /// The layout row that found it, for the startup log.
    ///
    /// An operator whose compiler was NOT found needs to know where this looked,
    /// and an operator surprised by one that was needs to know why it is there.
    /// Naming the layout answers both.
    std::string layout;
};

/// How a layout's root directories are located.
///
/// A mechanism per enumerator rather than a path per row, because the four differ
/// in kind: one is a fixed location, one hangs off an environment variable whose
/// value differs per machine, one is a registry value, and two are answers only a
/// program can give.
enum class LayoutRoot : std::uint8_t
{
    /// `rootPath` names the directory outright.
    FixedPath,
    /// `environmentVariable` names a prefix; `rootPath` hangs beneath it.
    ///
    /// For `%ProgramFiles%`, which is relocatable and localized -- hard-coding
    /// `C:\Program Files` works on most machines and silently finds nothing on the
    /// rest, which is the failure shape this whole feature exists to avoid.
    EnvironmentRelative,
    /// A registry value names the directory.
    RegistryValue,
    /// `vswhere.exe` enumerates the Visual Studio installations.
    ///
    /// The documented way to find them, and the only one that copes with several
    /// side-by-side editions. It lives at a fixed, VERSIONLESS location under
    /// `%ProgramFiles(x86)%`, which is what makes it findable without already
    /// knowing where Visual Studio is.
    VsWhere,
    /// `xcrun --find` names the active Xcode toolchain's binaries.
    ///
    /// Unlike every other row this yields the COMPILER rather than a directory to
    /// search: `xcrun` answers for whichever toolchain `xcode-select` points at,
    /// and reconstructing that from a path would be a second, worse copy of a
    /// question the platform already answers.
    Xcrun,
};

/// How a layout's binaries are recognised among a directory's entries.
enum class NameMatch : std::uint8_t
{
    /// Only the exact names, plus this host's executable suffix.
    Exact,
    /// Also `<name>-<digits>`: `gcc-13`, `clang++-18`.
    ///
    /// Every mainstream Linux distribution installs versioned compilers this way,
    /// side by side, and a fleet that served only the unsuffixed one would miss the
    /// toolchain most CI images actually build with.
    ExactOrVersionSuffixed,
};

/// One place a machine keeps compilers, and how to look there.
///
/// The table is the design. Adding an SDK, a distribution's layout or a new
/// Visual Studio arrangement is a ROW -- never another `if (exists(...))` threaded
/// through the walk -- which is the same reason `ClassifyCompiler`'s stem table is
/// a table.
struct ToolchainLayout
{
    /// What this layout is called, in the startup log.
    std::string_view name;
    /// How its root directories are found.
    LayoutRoot root { LayoutRoot::FixedPath };
    /// The directory, or the part of it below `environmentVariable`.
    std::string_view rootPath;
    /// The environment variable holding the prefix, for `EnvironmentRelative`.
    std::string_view environmentVariable;
    /// Which of a 64-bit host's two registry views holds `registryKey`.
    RegistryView view { RegistryView::Native };
    /// The key under `HKEY_LOCAL_MACHINE`, for `RegistryValue`. Its DEFAULT value
    /// is what is read -- which is where every installer this table knows about
    /// records its prefix. A named value, or a per-user install under `HKCU`, is a
    /// field away and deliberately not carried until something needs it: an unused
    /// column is a column every row has to spell and nobody can check.
    std::string_view registryKey;
    /// A directory beneath the root whose SUBDIRECTORIES are versions, each of
    /// which is searched in turn. Empty when the layout has no version level.
    std::string_view versionRoot;
    /// A file beneath the root naming the version to look at FIRST.
    ///
    /// Ordering only, never filtering: every installed version is served, because
    /// a client pinned to an older toolset needs a worker that matches it. What
    /// this buys is that the version a plain `cl` resolves to registers first, so
    /// an operator watching a cold start sees the toolchain they care about come up
    /// before the others -- which on a machine with several is the difference
    /// between "it is working" and "it has hung".
    std::string_view versionHintFile;
    /// Directories beneath the root (or beneath `versionRoot/<version>`) holding
    /// the binaries.
    std::span<std::string_view const> binPaths;
    /// The compiler names sought there, without an executable suffix.
    std::span<std::string_view const> binaries;
    /// How those names are matched.
    NameMatch match { NameMatch::Exact };
};

/// The layouts this build knows about, in the order they are searched.
///
/// Exposed so a test can assert the table's own shape -- that every row names a
/// binary, that no two rows share a name -- rather than only its results.
///
/// @return The table.
[[nodiscard]] std::span<ToolchainLayout const> ToolchainLayouts() noexcept;

/// Parse `vswhere -property installationPath` output into install roots.
///
/// Pure, and tested against captured output, for the reason every parser in
/// `ToolchainProbe` is: a Visual Studio installation cannot exist on the Linux and
/// macOS runners that make up most of this project's CI.
///
/// @param output What `vswhere` printed.
/// @return One path per installation, in the order printed.
[[nodiscard]] std::vector<std::string> ParseVsWhereInstallations(std::string_view output);

/// Whether @p name is @p stem, or @p stem with a numeric version suffix.
///
/// Exposed because the rule is subtle enough to deserve cases of its own: `gcc-13`
/// is a compiler and `gcc-ar` is not, and a discovery that offered the second would
/// register a toolchain that fails every job with a spawn error.
///
/// @param name A directory entry's name, suffix already stripped.
/// @param stem The compiler name sought.
/// @param match Whether version suffixes count.
/// @return True when @p name names @p stem.
[[nodiscard]] bool MatchesCompilerName(std::string_view name, std::string_view stem, NameMatch match);

/// Walk the layout table and report every compiler this machine holds.
///
/// Candidates only. Each one still goes through the existing identity path
/// unchanged -- `ClassifyCompiler`, `CompilerBanner`, `CachedToolchainFingerprint`
/// -- because a worker that derived its identity differently from its clients would
/// register, heartbeat, and never be matched.
///
/// Duplicates are collapsed by path, since one Visual Studio install is reported by
/// `vswhere` and by a fixed-location row alike, and `WorkerRegistry` keys on
/// `(fingerprint, endpoint)`: one machine registering under two near-identical
/// identities is exactly the double-counting a fleet view then has to render.
/// Two DIFFERENT spellings of one compiler -- `cc` beside `gcc` -- are deliberately
/// kept, because a client invoking `cc` computes the `cc` banner and a worker
/// registered only as `gcc` would never match it.
///
/// @param host The machine's filesystem, registry and environment.
/// @param runner Process-spawning seam, for `vswhere` and `xcrun`.
/// @return Every compiler found, deduplicated, in table order.
[[nodiscard]] std::vector<ToolchainCandidate> DiscoverToolchainCandidates(IToolchainHost& host, IProcessRunner& runner);

/// What a worker asks when it was given no `--toolchain`.
///
/// An interface rather than a call to the function above, so the node's own tests
/// can present a fleet's worth of toolchains without scripting a filesystem for
/// each -- and so a future discovery (a config file, a peer's answer) is a second
/// implementation rather than a branch inside this one.
class IToolchainDiscovery
{
  public:
    IToolchainDiscovery() = default;
    virtual ~IToolchainDiscovery() = default;
    IToolchainDiscovery(IToolchainDiscovery const&) = delete;
    IToolchainDiscovery& operator=(IToolchainDiscovery const&) = delete;
    IToolchainDiscovery(IToolchainDiscovery&&) = delete;
    IToolchainDiscovery& operator=(IToolchainDiscovery&&) = delete;

    /// @return Every compiler this machine holds, deduplicated.
    [[nodiscard]] virtual std::vector<ToolchainCandidate> Discover() = 0;
};

/// Discovery over the real machine.
///
/// @param host The machine's filesystem, registry and environment.
/// @param runner Process-spawning seam.
/// @return A discovery walking the layout table.
[[nodiscard]] std::unique_ptr<IToolchainDiscovery> MakeToolchainDiscovery(IToolchainHost& host, IProcessRunner& runner);

} // namespace FastCache::Cc
