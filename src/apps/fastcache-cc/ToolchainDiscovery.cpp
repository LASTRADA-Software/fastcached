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

    /// The bindir every non-MSVC layout keeps its compilers in.
    constexpr std::array<std::string_view, 1> BinOnly { "bin" };

    /// MSVC's, for the architecture this build targets.
    constexpr std::array<std::string_view, 1> MsvcBin { MsvcNativeBinPath };

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

    /// What an LLVM layout is looked for.
    ///
    /// `clang-cl` first so the more specific stem is reported before the prefix it
    /// contains, matching the order `ClassifyCompiler`'s own stem table uses.
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

    /// The root directories a layout row names on this machine.
    ///
    /// @param layout The row.
    /// @param host The machine.
    /// @param runner Process-spawning seam, for `vswhere`.
    /// @return Its roots; empty when the layout is not installed here.
    [[nodiscard]] std::vector<std::string> RootsOf(ToolchainLayout const& layout,
                                                   IToolchainHost& host,
                                                   IProcessRunner& runner)
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

                // Combined, because `vswhere` writes its own diagnostics to stderr
                // and a run that reports nothing useful there is indistinguishable
                // from one that reports nothing at all. The exit code is not checked
                // for the reason the GNU include probe does not check its own:
                // parsing decides whether the output is usable.
                return ParseVsWhereInstallations(runner.RunCaptureCombined(argv).out);
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

        for (auto const& root: RootsOf(layout, host, runner))
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

                for (auto const& entry: entries)
                {
                    // An extensionless entry loses to its `.exe` sibling, and the rule
                    // is about the DESCRIBED machine rather than the host running this,
                    // so a Windows bindir stays testable from anywhere. An MSYS2 or
                    // Cygwin bindir routinely holds a shell wrapper named `gcc` beside
                    // the launchable `gcc.exe`; Windows resolves a bare name through
                    // PATHEXT and never runs the first, and `ExecutableExists` cannot
                    // tell them apart because that filesystem has no execute bit.
                    // Registering the wrapper would hand the fleet a toolchain that
                    // cannot be spawned.
                    if (!entry.ends_with(WindowsExecutableSuffix)
                        && present(entry + std::string { WindowsExecutableSuffix }))
                        continue;

                    auto entryName = std::string_view { entry };
                    if (entryName.ends_with(WindowsExecutableSuffix))
                        entryName.remove_suffix(WindowsExecutableSuffix.size());

                    auto const wanted = std::ranges::any_of(layout.binaries, [&](std::string_view sought) {
                        return MatchesCompilerName(entryName, sought, layout.match);
                    });
                    if (!wanted)
                        continue;

                    // Asked of the HOST rather than inferred from the name, because
                    // only the host knows: a POSIX bindir holds `gcc` beside `gcc.1`
                    // and the execute bit is what separates them, while on Windows the
                    // filesystem has no such bit and the name is all there is. A
                    // manual page offered as a compiler would register a toolchain
                    // that fails every job it is sent.
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
