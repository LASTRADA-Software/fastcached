// SPDX-License-Identifier: Apache-2.0
#include "ToolchainDiscovery.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    /// The suffix a Windows compiler's filename carries.
    ///
    /// NOT conditioned on the host this code was built for, and that is the point:
    /// the table describes machines, and a Windows layout has to be probed from a
    /// Linux runner or four rows of it are untested. Stripping a suffix a POSIX
    /// bindir does not have costs nothing, while `#if`-ing it out would make the
    /// same table find different things depending on who compiled it.
    constexpr std::string_view WindowsExecutableSuffix = ".exe";

    /// The MSVC bindir for the architecture this process was built for.
    ///
    /// One row rather than every `Host<a>/<b>` combination a Visual Studio install
    /// contains, and the restraint is deliberate. Every target variant of one
    /// toolset -- x64, x86, arm64 -- shares an include tree AND, because `cl` has no
    /// `--version`, a banner of the normalized basename, so all of them fingerprint
    /// IDENTICALLY. Offering them all would register one machine several times under
    /// one identity, and hand the scheduler a worker that might compile for the
    /// wrong target. The native one is what a client on this machine invokes.
    constexpr std::string_view MsvcNativeBinPath =
#if defined(_M_ARM64) || defined(__aarch64__)
        "bin/Hostarm64/arm64";
#elif defined(_M_X64) || defined(__x86_64__)
        "bin/Hostx64/x64";
#else
        "bin/Hostx86/x86";
#endif

    /// The bindir a Visual Studio install keeps its BUNDLED LLVM in.
    ///
    /// Visual Studio ships clang-cl itself, under `VC/Tools/Llvm` rather than beside
    /// `cl` -- so the MSVC row above walks straight past it, and every one of the
    /// three LLVM rows below wants a STANDALONE install (a registry key, or
    /// `%ProgramFiles%/LLVM`). A machine whose only clang-cl came with Visual Studio
    /// therefore registered no clang-cl toolchain at all: builds using it were
    /// cached, because the launcher does not need a worker for that, and could never
    /// be dispatched, because no worker ever advertised the fingerprint. Nothing
    /// reported it -- half of what the fleet is for, off, silently.
    ///
    /// Native architecture only, for the reason `MsvcNativeBinPath` is: the other
    /// directory holds a compiler built for a different HOST, which this machine
    /// cannot run. `VC/Tools/Llvm/bin` is where a 32-bit host's copy sits, matching
    /// the layout Visual Studio used before it grew per-architecture directories.
    constexpr std::string_view VsLlvmNativeBinPath =
#if defined(_M_ARM64) || defined(__aarch64__)
        "VC/Tools/Llvm/ARM64/bin";
#elif defined(_M_X64) || defined(__x86_64__)
        "VC/Tools/Llvm/x64/bin";
#else
        "VC/Tools/Llvm/bin";
#endif

    /// The bindir every non-MSVC layout keeps its compilers in.
    constexpr std::array<std::string_view, 1> BinOnly { "bin" };

    /// MSVC's, for the architecture this build targets.
    constexpr std::array<std::string_view, 1> MsvcBin { MsvcNativeBinPath };

    /// The LLVM Visual Studio bundles, for the architecture this build targets.
    constexpr std::array<std::string_view, 1> VsLlvmBin { VsLlvmNativeBinPath };

    /// The three MSYS2 environments that ship a usable compiler.
    ///
    /// `ucrt64` first because it is the one MSYS2 itself recommends and the one a
    /// modern install defaults to; `mingw64` for older roots; `clang64` for the LLVM
    /// environment. `msys` is deliberately absent -- its `gcc` targets the Cygwin-like
    /// MSYS runtime rather than native Windows, so a job dispatched to it comes back
    /// linked against a DLL the client does not have.
    constexpr std::array<std::string_view, 3> Msys2Bins { "ucrt64/bin", "mingw64/bin", "clang64/bin" };

    /// What an MSVC layout is looked for.
    constexpr std::array<std::string_view, 1> MsvcBinaries { "cl" };

    /// What an LLVM layout is looked for, in order of PREFERENCE.
    ///
    /// `clang-cl` first, and the order is load-bearing rather than cosmetic. All
    /// three fingerprint identically -- clang's banner does not name its own
    /// `argv[0]` the way a GNU driver's does, and they own one include tree -- so a
    /// worker keeps the first and drops the rest as duplicates. The first must
    /// therefore be the driver a client on this machine actually invokes, and the
    /// arguments it will be handed are spelled in that driver's grammar: an MSVC-
    /// spelled job routed to `clang++` is read as a list of filenames.
    constexpr std::array<std::string_view, 4> LlvmBinaries { "clang-cl", "clang++", "clang", "cc" };

    /// What a GNU-shaped layout is looked for.
    ///
    /// `cc` and `c++` are here beside `gcc` and `g++` deliberately. They are usually
    /// the same binary under another name, and they fingerprint DIFFERENTLY, because
    /// a GNU driver prints its own `argv[0]` in the banner its clients hash -- so a
    /// worker registered only as `gcc` never matches a build that invokes `cc`.
    constexpr std::array<std::string_view, 6> PosixBinaries { "gcc", "g++", "clang", "clang++", "cc", "c++" };

    /// `vswhere`'s arguments.
    ///
    /// `-products *` so Build Tools and the Express editions are found and not only
    /// Community/Professional/Enterprise, and `-prerelease` so a Preview-channel
    /// install is too -- a developer machine on Preview would otherwise report "no
    /// Visual Studio" while a compiler sat on it.
    ///
    /// **No `-requires`.** Filtering on `...VC.Tools.x86.x64` looks like precision
    /// and is a false negative on an ARM64-only toolset -- on exactly the host
    /// `MsvcNativeBinPath` has an arm64 spelling for. The filesystem decides
    /// instead: an installation with no C++ toolset has nothing beneath its
    /// `versionRoot`, so the walk finds nothing and that install costs one directory
    /// listing.
    ///
    /// **No `-utf8`.** It would be the only UTF-8 thing in the pipeline: every path
    /// here reaches `CreateProcessA`, `RegQueryValueExA` and MSVC's
    /// `std::filesystem::path`, all of which decode narrow bytes in the ACP. Asking
    /// for UTF-8 would hand those APIs bytes they misread, which is worse than the
    /// ACP round trip everything else already does. A non-ASCII install path is a
    /// tree-wide question rather than a `vswhere` one.
    /// The trailing four ask for one bare installation path per line, unadorned,
    /// which is what `ParseVsWhereInstallations` is written against.
    constexpr std::array<std::string_view, 7> VsWhereArguments { "-products", "*",         "-prerelease",     "-format",
                                                                 "value",     "-property", "installationPath" };

    /// Where `vswhere.exe` always is.
    ///
    /// A fixed, VERSIONLESS location, which is the whole reason it is usable: it is
    /// how you find Visual Studio without already knowing where Visual Studio is.
    constexpr std::string_view VsWhereRelativePath = "Microsoft Visual Studio/Installer/vswhere.exe";

    /// The variable naming the 32-bit program-files directory.
    constexpr std::string_view ProgramFilesX86Variable = "ProgramFiles(x86)";

    /// The variable naming the native program-files directory.
    constexpr std::string_view ProgramFilesVariable = "ProgramFiles";

    /// The layout table itself.
    ///
    /// Ordered so the more specific and more likely layouts are searched first;
    /// duplicates collapse afterwards, so order decides which row gets NAMED in the
    /// log rather than whether a compiler is found at all.
    constexpr auto Layouts = std::to_array<ToolchainLayout>({
        { .name = "visual-studio",
          .root = LayoutRoot::VsWhere,
          .rootPath = {},
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = "VC/Tools/MSVC",
          .versionHintFile = "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt",
          .binPaths = MsvcBin,
          .binaries = MsvcBinaries,
          .match = NameMatch::Exact },
        // The same installation, one directory over. No `versionRoot`: the bundled
        // LLVM is not versioned side by side the way the MSVC toolsets are, so the
        // bindir hangs directly off the installation path.
        { .name = "visual-studio-llvm",
          .root = LayoutRoot::VsWhere,
          .rootPath = {},
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = VsLlvmBin,
          .binaries = LlvmBinaries,
          .match = NameMatch::Exact },
        { .name = "llvm-registry",
          .root = LayoutRoot::RegistryValue,
          .rootPath = {},
          .environmentVariable = {},
          .view = RegistryView::ThirtyTwoBit,
          .registryKey = R"(SOFTWARE\LLVM\LLVM)",
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = LlvmBinaries,
          .match = NameMatch::Exact },
        { .name = "llvm-registry-native",
          .root = LayoutRoot::RegistryValue,
          .rootPath = {},
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = R"(SOFTWARE\LLVM\LLVM)",
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = LlvmBinaries,
          .match = NameMatch::Exact },
        { .name = "llvm-program-files",
          .root = LayoutRoot::EnvironmentRelative,
          .rootPath = "LLVM",
          .environmentVariable = ProgramFilesVariable,
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = LlvmBinaries,
          .match = NameMatch::Exact },
        { .name = "msys2",
          .root = LayoutRoot::FixedPath,
          .rootPath = "C:/msys64",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = Msys2Bins,
          .binaries = PosixBinaries,
          .match = NameMatch::Exact },
        { .name = "mingw-w64",
          .root = LayoutRoot::FixedPath,
          .rootPath = "C:/mingw64",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::Exact },
        // POSIX. `/usr/local` before `/usr`, matching the order a shell's PATH
        // conventionally carries them, so a hand-built compiler is the one the log
        // names when both hold the same compiler name.
        { .name = "usr-local",
          .root = LayoutRoot::FixedPath,
          .rootPath = "/usr/local",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::ExactOrVersionSuffixed },
        { .name = "usr",
          .root = LayoutRoot::FixedPath,
          .rootPath = "/usr",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::ExactOrVersionSuffixed },
        // MacPorts, then Homebrew's Apple-silicon prefix. Homebrew's Intel prefix is
        // `/usr/local`, which the rows above already cover.
        { .name = "macports",
          .root = LayoutRoot::FixedPath,
          .rootPath = "/opt/local",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::ExactOrVersionSuffixed },
        { .name = "homebrew",
          .root = LayoutRoot::FixedPath,
          .rootPath = "/opt/homebrew",
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::ExactOrVersionSuffixed },
        // Last, and the only row that spawns anything on POSIX. On most Macs it
        // confirms what `/usr/bin` already yielded rather than adding to it, and the
        // duplicate check collapses the overlap.
        { .name = "xcode",
          .root = LayoutRoot::Xcrun,
          .rootPath = {},
          .environmentVariable = {},
          .view = RegistryView::Native,
          .registryKey = {},
          .versionRoot = {},
          .versionHintFile = {},
          .binPaths = BinOnly,
          .binaries = PosixBinaries,
          .match = NameMatch::Exact },
    });

    /// The key two spellings of one compiler path share, for deduplication.
    ///
    /// Separators are collapsed on EVERY host, because a Windows layout has to be
    /// describable from a Linux runner -- a `#if` here would leave the case that
    /// matters most untested on the two platforms most of the suite runs on.
    ///
    /// Case is folded on Windows only, where two spellings differing only in case
    /// name one file. Folding everywhere would collapse `/usr/bin/CC` onto
    /// `/usr/bin/cc` on a machine that genuinely has both.
    ///
    /// @param path A discovered compiler path.
    /// @return Its identity for the duplicate check.
    [[nodiscard]] std::string PathIdentity(std::string_view path)
    {
        std::string key { path };
        std::ranges::replace(key, '\\', '/');
#if defined(_WIN32)
        std::ranges::transform(key, key.begin(), [](char c) { return PathCanon::AsciiLower(c); });
#endif
        return key;
    }

    /// `vswhere`'s answer, held so it is asked once however many rows want it.
    ///
    /// Two rows describe one Visual Studio installation -- its MSVC toolsets and the
    /// LLVM it bundles -- and `vswhere` is a PROCESS. Asking per row would spawn it
    /// once for each, at every node start, forever, to be told the same thing; and a
    /// third row wanting the same installation would make it three. Filled only
    /// when a row actually reaches it, so the "no installer means no run at all"
    /// guard still holds.
    ///
    /// A flag beside the vector rather than an `optional<vector>`: an empty answer is
    /// a real answer here, so the two states are "asked" and "not asked" rather than
    /// "has a value" and "does not". It also keeps the read a plain member access --
    /// GCC's `-Wnull-dereference` cannot see through an inlined `optional` deref at
    /// `-O3` and rejects the copy out of one.
    struct VsWhereInstallations
    {
        std::vector<std::string> paths; ///< The installations; empty when there are none.
        bool asked { false };           ///< Whether the question has been put to `vswhere` yet.
    };

    /// Ask `vswhere` which Visual Studio installations this machine has.
    ///
    /// @param host The machine, for the installer directory.
    /// @param runner Process-spawning seam.
    /// @return The installation paths; empty when Visual Studio is not installed here.
    [[nodiscard]] std::vector<std::string> AskVsWhere(IToolchainHost& host, IProcessRunner& runner)
    {
        auto const installer = host.Environment(ProgramFilesX86Variable);
        if (!installer.has_value() || installer->empty())
            return {};
        auto const vswhere = JoinPath(*installer, VsWhereRelativePath);
        if (!host.ExecutableExists(vswhere))
            return {};

        std::vector<std::string> argv;
        argv.reserve(VsWhereArguments.size() + 1);
        argv.push_back(vswhere);
        for (auto const& argument: VsWhereArguments)
            argv.emplace_back(argument);

        // Combined, because `vswhere` writes its own diagnostics to stderr and a run
        // that reports nothing useful there is indistinguishable from one that
        // reports nothing at all. The exit code is not checked for the reason the GNU
        // include probe does not check its own: parsing decides whether the output is
        // usable.
        return ParseVsWhereInstallations(runner.RunCaptureCombined(argv).out);
    }

    /// The root directories a layout row names on this machine.
    ///
    /// @param layout The row.
    /// @param host The machine.
    /// @param runner Process-spawning seam, for `vswhere`.
    /// @param vsWhere Memo for `vswhere`'s answer, shared across rows.
    /// @return Its roots; empty when the layout is not installed here.
    [[nodiscard]] std::vector<std::string> RootsOf(ToolchainLayout const& layout,
                                                   IToolchainHost& host,
                                                   IProcessRunner& runner,
                                                   VsWhereInstallations& vsWhere)
    {
        // No `default:`, so a mechanism added to the enum is a compile error here
        // rather than a row that silently finds nothing.
        switch (layout.root)
        {
            case LayoutRoot::FixedPath:
                return { std::string { layout.rootPath } };

            case LayoutRoot::EnvironmentRelative: {
                auto const prefix = host.Environment(layout.environmentVariable);
                if (!prefix.has_value() || prefix->empty())
                    return {};
                return { JoinPath(*prefix, layout.rootPath) };
            }

            case LayoutRoot::RegistryValue: {
                // The key's DEFAULT value, under HKEY_LOCAL_MACHINE. That is where
                // every installer this table knows about records its prefix; a named
                // value or a per-user key is a column away and deliberately not
                // carried until something needs it, since an unused column is one
                // every row must spell and nobody can check.
                auto const value = host.RegistryString(RegistryHive::LocalMachine, layout.registryKey, {}, layout.view);
                if (!value.has_value() || value->empty())
                    return {};
                return { JoinPath(*value, layout.rootPath) };
            }

            case LayoutRoot::VsWhere: {
                // Memoized whatever the answer, the empty ones included: "not
                // installed here" is as much an answer as a list of installations,
                // and a second row re-deriving it would probe the filesystem again.
                if (!vsWhere.asked)
                {
                    vsWhere.asked = true;
                    vsWhere.paths = AskVsWhere(host, runner);
                }
                return vsWhere.paths;
            }

            case LayoutRoot::Xcrun:
                // Answers with compilers rather than directories, so it has no roots.
                // Handled by the caller.
                return {};
        }
        return {};
    }

    /// The directories beneath one root that a layout searches for binaries.
    ///
    /// Where a row has a `versionRoot`, that is one bindir per installed version --
    /// EVERY version, because a client pinned to an older toolset needs a worker
    /// matching it. The `versionHintFile` only decides which comes first.
    ///
    /// @param layout The row.
    /// @param host The machine.
    /// @param root One of the row's roots.
    /// @return The directories to list.
    [[nodiscard]] std::vector<std::string> BinDirectoriesOf(ToolchainLayout const& layout,
                                                            IToolchainHost& host,
                                                            std::string const& root)
    {
        std::vector<std::string> prefixes;
        if (layout.versionRoot.empty())
            prefixes.push_back(root);
        else
        {
            auto const versionRoot = JoinPath(root, layout.versionRoot);
            auto versions = host.ListDirectories(versionRoot);

            // Sorted for determinism -- a directory listing's order is a property of
            // the filesystem -- and then the hinted version moved to the front, so
            // the toolchain a plain `cl` resolves to is the first one an operator
            // watching a cold start sees register.
            std::ranges::sort(versions);
            if (!layout.versionHintFile.empty())
                if (auto const hint = host.ReadTextFile(JoinPath(root, layout.versionHintFile)); hint.has_value())
                {
                    auto const trimmed = std::string_view { *hint }.substr(0, hint->find_first_of("\r\n"));
                    if (auto const found = std::ranges::find(versions, trimmed); found != versions.end())
                        std::ranges::rotate(versions, found);
                }

            for (auto const& version: versions)
                prefixes.push_back(JoinPath(versionRoot, version));
        }

        std::vector<std::string> directories;
        for (auto const& prefix: prefixes)
            for (auto const& binPath: layout.binPaths)
                directories.push_back(JoinPath(prefix, binPath));
        return directories;
    }

    /// The tool `LayoutRoot::Xcrun` asks.
    constexpr std::string_view XcrunName = "xcrun";

    /// Ask `xcrun` where the active Xcode toolchain keeps each compiler.
    ///
    /// @param layout The row.
    /// @param host The machine, for finding `xcrun` itself.
    /// @param runner Process-spawning seam.
    /// @param report Called with each compiler found.
    void DiscoverThroughXcrun(ToolchainLayout const& layout,
                              IToolchainHost& host,
                              IProcessRunner& runner,
                              auto const& report)
    {
        // Looked for BEFORE anything is spawned. This row asks one question per
        // compiler name, so on a machine with no `xcrun` -- which is every Windows
        // and every Linux machine -- an unguarded row costs six failed process
        // creations at every single start, forever, to learn something the search
        // path already said.
        auto const xcrun = host.ResolveOnSearchPath(XcrunName);
        if (!xcrun.has_value())
            return;

        for (auto const& binary: layout.binaries)
        {
            // The RESOLVED path, not the bare name again: resolving it and then
            // spawning something else is two questions with one answer between them.
            std::array<std::string, 3> const argv { *xcrun, "--find", std::string { binary } };
            auto const run = runner.RunCaptureCombined(argv);
            if (run.exitCode != 0)
                continue;

            // One line, and `xcrun` writes its failures here too -- so a path is
            // only a path when it looks like one. An error message beginning with
            // `xcrun:` would otherwise be registered as a compiler.
            auto const line = std::string_view { run.out }.substr(0, run.out.find_first_of("\r\n"));
            if (line.empty() || line.front() != '/')
                continue;
            report(std::string { line });
        }
    }
} // namespace

std::span<ToolchainLayout const> ToolchainLayouts() noexcept
{
    return Layouts;
}

std::vector<std::string> ParseVsWhereInstallations(std::string_view output)
{
    std::vector<std::string> installations;
    while (!output.empty())
    {
        auto const newline = output.find('\n');
        auto line = output.substr(0, newline);
        output = newline == std::string_view::npos ? std::string_view {} : output.substr(newline + 1);

        // A `\r` survives `vswhere`'s own CRLF output and, left on, becomes part of
        // the path -- where every probe beneath it then finds nothing, silently.
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);

        // `vswhere` prints diagnostics on the same stream when something is wrong,
        // and an installation path is always absolute. A line that is not one is a
        // message, not a location.
        if (line.empty() || !line.contains(':'))
            continue;
        if (line.starts_with("vswhere"))
            continue;
        installations.emplace_back(line);
    }
    return installations;
}

bool MatchesCompilerName(std::string_view name, std::string_view stem, NameMatch match)
{
    if (name == stem)
        return true;
    if (match != NameMatch::ExactOrVersionSuffixed)
        return false;

    // `gcc-13` yes, `gcc-ar` no. The suffix must be a version and nothing else:
    // `gcc-ar`, `gcc-nm` and `gcc-ranlib` sit in the same bindir, and offering one
    // as a compiler registers a toolchain that fails every job it is sent.
    if (!name.starts_with(stem) || name.size() <= stem.size() + 1 || name[stem.size()] != '-')
        return false;
    return LooksLikeVersion(name.substr(stem.size() + 1));
}

std::vector<ToolchainCandidate> DiscoverToolchainCandidates(IToolchainHost& host, IProcessRunner& runner)
{
    std::vector<ToolchainCandidate> candidates;

    std::set<std::string> seen;
    VsWhereInstallations vsWhere;
    for (auto const& layout: ToolchainLayouts())
    {
        auto record = [&](std::string compiler) {
            if (!seen.insert(PathIdentity(compiler)).second)
                return;
            candidates.push_back(
                ToolchainCandidate { .compiler = std::move(compiler), .layout = std::string { layout.name } });
        };

        if (layout.root == LayoutRoot::Xcrun)
        {
            DiscoverThroughXcrun(layout, host, runner, record);
            continue;
        }

        for (auto const& root: RootsOf(layout, host, runner, vsWhere))
        {
            if (root.empty() || !host.DirectoryExists(root))
                continue;

            for (auto const& directory: BinDirectoriesOf(layout, host, root))
            {
                // Listed once and matched against every wanted name, rather than
                // probed name by name: a bindir holds hundreds of entries and the
                // version-suffixed rule cannot be expressed as a path to test for.
                auto entries = host.ListFiles(directory);

                // SORTED rather than copied into a set. The only question asked of
                // this is whether a sibling name is present, and `/usr/bin` holds
                // several thousand entries -- a set would copy every one of them and
                // allocate a node each, per bindir, to answer it.
                std::ranges::sort(entries);
                auto const present = [&entries](std::string_view name) {
                    return std::ranges::binary_search(entries, name);
                };

                // The ROW's order outside, the directory's inside. That order is a
                // preference and not decoration: several drivers in one bindir can
                // share a fingerprint -- `clang-cl`, `clang++` and `clang` do, since
                // clang's banner does not name its own `argv[0]` the way a GNU
                // driver's does -- and a worker serves the FIRST of them and drops
                // the rest as duplicates. Left to the listing, `clang++.exe` won on
                // alphabetical order alone, so a Windows node advertised the GNU
                // driver and every clang-cl job routed to it arrived spelled in a
                // grammar that driver does not read.
                for (auto const& sought: layout.binaries)
                    for (auto const& entry: entries)
                    {
                        // An extensionless entry loses to its `.exe` sibling, and the
                        // rule is about the DESCRIBED machine rather than the host
                        // running this, so a Windows bindir stays testable from
                        // anywhere. An MSYS2 or Cygwin bindir routinely holds a shell
                        // wrapper named `gcc` beside the launchable `gcc.exe`; Windows
                        // resolves a bare name through PATHEXT and never runs the
                        // first, and `ExecutableExists` cannot tell them apart because
                        // that filesystem has no execute bit. Registering the wrapper
                        // would hand the fleet a toolchain that cannot be spawned.
                        if (!entry.ends_with(WindowsExecutableSuffix)
                            && present(entry + std::string { WindowsExecutableSuffix }))
                            continue;

                        auto entryName = std::string_view { entry };
                        if (entryName.ends_with(WindowsExecutableSuffix))
                            entryName.remove_suffix(WindowsExecutableSuffix.size());

                        if (!MatchesCompilerName(entryName, sought, layout.match))
                            continue;

                        // Asked of the HOST rather than inferred from the name,
                        // because only the host knows: a POSIX bindir holds `gcc`
                        // beside `gcc.1` and the execute bit is what separates them,
                        // while on Windows the filesystem has no such bit and the name
                        // is all there is. A manual page offered as a compiler would
                        // register a toolchain that fails every job it is sent.
                        auto full = JoinPath(directory, entry);
                        if (host.ExecutableExists(full))
                            record(std::move(full));
                    }
            }
        }
    }

    return candidates;
}

namespace
{
    /// `IToolchainDiscovery` over the layout table.
    class HostToolchainDiscovery final: public IToolchainDiscovery
    {
      public:
        /// @param host The machine.
        /// @param runner Process-spawning seam.
        HostToolchainDiscovery(IToolchainHost& host, IProcessRunner& runner):
            _host { host },
            _runner { runner }
        {
        }

        std::vector<ToolchainCandidate> Discover() override
        {
            return DiscoverToolchainCandidates(_host, _runner);
        }

      private:
        IToolchainHost& _host;
        IProcessRunner& _runner;
    };
} // namespace

std::unique_ptr<IToolchainDiscovery> MakeToolchainDiscovery(IToolchainHost& host, IProcessRunner& runner)
{
    return std::make_unique<HostToolchainDiscovery>(host, runner);
}

} // namespace FastCache::Cc
