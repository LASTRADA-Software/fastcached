// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace FastCache::Cc
{
namespace
{

    /// The last path component of `path` (after the final `/` or `\`).
    [[nodiscard]] std::string_view Basename(std::string_view path)
    {
        auto const slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    /// True if `lowerName` ends with one of the recognised C/C++ source suffixes.
    [[nodiscard]] bool IsSourceSuffix(std::string_view lowerName)
    {
        // The module-interface spellings are here to be RECOGNISED, not supported:
        // a line naming one is then refused by `UncacheableBecause` with its reason
        // stated, where before it fell through as "no source file found" and was
        // passed through in silence -- indistinguishable, from outside, from the
        // cache being broken. See SourceExtensions for why they can never be cached.
        constexpr std::array<std::string_view, 12> Suffixes { ".cpp", ".cc",   ".cxx", ".c++",  ".c",    ".m",
                                                              ".ixx", ".cppm", ".ccm", ".cxxm", ".c++m", ".mxx" };
        return std::ranges::any_of(Suffixes, [&](std::string_view s) { return lowerName.ends_with(s); });
    }

    // --- driver descriptors -------------------------------------------------
    //
    // The single source of truth for how each compiler driver spells the things
    // the launcher cares about. A new driver is a new row here plus a name
    // pattern below; the parsing logic itself never grows a branch.

    // Both spellings must suppress line markers. `#line` / `# 1 "file"` markers
    // embed the ABSOLUTE source path, which would make the preprocessed text —
    // and therefore the cache key — differ between two checkouts of the same
    // content at different paths, defeating cross-machine sharing entirely.
    // MSVC: /EP writes to stdout without #line. GNU: -P suppresses markers.
    //
    // `/EP` ALONE, and the absence of `/P` here is the whole point. The two are
    // not additive: `/EP` preprocesses to stdout, `/P` preprocesses to a FILE, and
    // MSVC documents the pair as "write the file without #line directives". So
    // passing both sent the preprocessed text to `<base>.i` and left the launcher
    // hashing an essentially empty stdout — a key with no content in it at all,
    // under which an edited source re-fetches the object built from the OLD text.
    // Direct mode hides it (its manifest hashes the source's bytes), so it showed
    // up only with FASTCACHE_NO_DIRECT=1, and it dropped a stray `.i` in the
    // working directory on every probe besides.
    constexpr std::array<std::string_view, 1> MsvcPreprocess { "/EP" };
    constexpr std::array<std::string_view, 2> GnuPreprocess { "-E", "-P" };

    /// Preprocess flags for text that is going to be COMPILED elsewhere, which
    /// keep the `#line` markers the key's own probe suppresses.
    ///
    /// The markers are what tell the compiler which text came from a system header,
    /// and `#pragma GCC system_header` suppression rides on that. Without them every
    /// warning inside libc++ or the CRT resurfaces in the remote compile -- and under
    /// this project's own `-pedantic -Werror` they are not noise, they are errors, so
    /// every dispatched translation unit would fail and be retried locally. Measured:
    /// a trivial TU including <string> produced two, and a real one produces many.
    ///
    /// They carry absolute paths, which is exactly why the KEY's probe suppresses
    /// them and why this text must never reach `ComputeKey`. The two runs answer two
    /// questions and only one of them has to be portable.
    constexpr std::array<std::string_view, 1> MsvcDispatchPreprocess { "/E" };
    constexpr std::array<std::string_view, 1> GnuDispatchPreprocess { "-E" };

    /// Tell a driver its input is already preprocessed, per language. See
    /// `DriverSpec::preprocessedInput`: without this, `-pedantic` reports the
    /// `#line` markers themselves as a GNU extension and `-Werror` fails the
    /// compile -- and nothing states the LANGUAGE, which the worker's scratch file
    /// name would otherwise decide.
    constexpr std::array<std::string_view, 2> GnuPreprocessedC { "-x", "cpp-output" };
    constexpr std::array<std::string_view, 2> GnuPreprocessedCxx { "-x", "c++-cpp-output" };
    constexpr std::array<std::string_view, 2> GnuPreprocessedObjC { "-x", "objective-c-cpp-output" };
    constexpr std::array<std::string_view, 2> GnuPreprocessedObjCxx { "-x", "objective-c++-cpp-output" };

    /// MSVC's own spelling of the same thing. `/TC` and `/TP` say "treat every
    /// input as C / as C++" and are documented as overriding the extension, which
    /// is exactly what is needed for a file the worker had to name itself.
    ///
    /// There is deliberately no Objective-C row: the MSVC drivers compile no such
    /// language, so a job in it is refused before it is sent rather than being
    /// handed over with its language unstated.
    constexpr std::array<std::string_view, 1> MsvcPreprocessedC { "/TC" };
    constexpr std::array<std::string_view, 1> MsvcPreprocessedCxx { "/TP" };

    constexpr std::array<PreprocessedInputSpelling, 4> GnuPreprocessedInput { {
        { .language = SourceLanguage::C, .flags = GnuPreprocessedC },
        { .language = SourceLanguage::Cxx, .flags = GnuPreprocessedCxx },
        { .language = SourceLanguage::ObjectiveC, .flags = GnuPreprocessedObjC },
        { .language = SourceLanguage::ObjectiveCxx, .flags = GnuPreprocessedObjCxx },
    } };

    constexpr std::array<PreprocessedInputSpelling, 2> MsvcPreprocessedInput { {
        { .language = SourceLanguage::C, .flags = MsvcPreprocessedC },
        { .language = SourceLanguage::Cxx, .flags = MsvcPreprocessedCxx },
    } };

    /// Which extension names which language.
    ///
    /// Matched case-insensitively so a `.CPP` on Windows still dispatches, with the
    /// two genuinely ambiguous spellings excluded below rather than guessed at.
    ///
    /// The module-interface rows carry every extension the two toolchains that have
    /// a convention for one use -- `.ixx` is MSVC's, the `*m` family is clang's --
    /// and they are here to be REFUSED rather than to be supported. A module
    /// interface unit writes a BMI beside its object, which is a second artefact
    /// neither a cache hit nor a dispatched compile can carry (the same rule
    /// `cmake/portable/CompileCache.cmake` applies when it turns module scanning off
    /// while a launcher is active). Naming them is what makes that a stated rule
    /// instead of the accident it was: they simply were not in `IsSourceSuffix`, so
    /// nothing recognised them, and adding one there would have quietly started
    /// replaying objects whose BMI nobody reproduced.
    constexpr std::array<std::pair<std::string_view, SourceLanguage>, 14> SourceExtensions { {
        { ".c", SourceLanguage::C },
        { ".cc", SourceLanguage::Cxx },
        { ".cp", SourceLanguage::Cxx },
        { ".cpp", SourceLanguage::Cxx },
        { ".cxx", SourceLanguage::Cxx },
        { ".c++", SourceLanguage::Cxx },
        { ".m", SourceLanguage::ObjectiveC },
        { ".mm", SourceLanguage::ObjectiveCxx },
        { ".ixx", SourceLanguage::CxxModuleInterface },
        { ".cppm", SourceLanguage::CxxModuleInterface },
        { ".ccm", SourceLanguage::CxxModuleInterface },
        { ".cxxm", SourceLanguage::CxxModuleInterface },
        { ".c++m", SourceLanguage::CxxModuleInterface },
        { ".mxx", SourceLanguage::CxxModuleInterface },
    } };

    /// Extensions whose language depends on the DRIVER and not on the extension:
    /// a GNU driver reads `.C` as C++ and `.M` as Objective-C++, while an MSVC one
    /// reads both case-insensitively and so calls them C and Objective-C. Compared
    /// case-SENSITIVELY, which is the whole point of the row.
    constexpr std::array<std::string_view, 2> AmbiguousSourceExtensions { ".C", ".M" };

    /// What each language is called, and whether it may be dispatched at all.
    ///
    /// `refused` is a REASON rather than a flag, because the two ways a job is
    /// turned away are not the same fact and must not read as though they were: a
    /// driver with no spelling for a language (Objective-C on MSVC) is a property of
    /// that driver, while a module interface unit is refused on every driver, by
    /// both gates, for a reason no spelling could fix.
    struct LanguageSpec
    {
        SourceLanguage language;
        std::string_view name;
        /// Why a translation unit in this language is neither cached nor dispatched.
        /// Empty when it may be.
        std::string_view refused;
    };

    constexpr std::array<LanguageSpec, 5> LanguageSpecs { {
        { .language = SourceLanguage::C, .name = "C", .refused = {} },
        { .language = SourceLanguage::Cxx, .name = "C++", .refused = {} },
        { .language = SourceLanguage::ObjectiveC, .name = "Objective-C", .refused = {} },
        { .language = SourceLanguage::ObjectiveCxx, .name = "Objective-C++", .refused = {} },
        { .language = SourceLanguage::CxxModuleInterface,
          .name = "C++ module interface",
          .refused = "it writes a BMI beside the object, and neither a cache hit nor a dispatched compile "
                     "reproduces anything but the object" },
    } };

    /// Flags dropped when preprocessing that carry no path value of their own:
    /// the compile-only marker (we want text on stdout, not an object) and the
    /// dependency-reporting switches, which would otherwise make the probe
    /// overwrite the build's real depfile.
    ///
    /// The object-output and depfile flags are NOT here. They are dropped by
    /// role, off PathValueFlags(), so `/Fo` and `-MF` are spelled once for the
    /// whole launcher rather than once per table — see DroppedFromPreprocess.
    ///
    /// Every spelling of `/showIncludes` the drivers accept has to be here, not
    /// just the one the parser looks for: the probe appends its own, the drivers
    /// resolve repeats last-wins, and `/showIncludes:user` suppresses the system
    /// headers. A surviving one would therefore make the key's dependency set
    /// depend on which spelling the build happened to use — the exact invariant
    /// MsvcDependencyProbe exists to establish.
    ///
    /// `-c` is here beside `/c` for the same reason: clang-cl is an MSVC-family
    /// driver that also accepts the GNU spelling, and CMake's Ninja generator
    /// emits exactly that spelling for CMAKE_CXX_COMPILER=clang-cl. A build
    /// spelling it `-c` left the marker on the /EP probe line, where clang-cl
    /// reports it as an unused argument (-Wunused-command-line-argument) --
    /// silent under an ordinary build, and a hard preprocess failure under one
    /// that also builds with /WX, which takes the cache out of every compile
    /// with no build failure to notice it by (#688).
    constexpr std::array<std::string_view, 6> MsvcDrop {
        "/c", "-c", "/showIncludes", "/showIncludes:user", "-showIncludes", "-showIncludes:user",
    };
    constexpr std::array<std::string_view, 4> GnuDrop { "-c", "-MD", "-MMD", "-MP" };

    /// How each driver is asked to report dependencies during the preprocess
    /// probe. `-MD` rather than `-MMD` on purpose: the key's dependency set must
    /// not depend on which of the two the build happened to ask for, or on whether
    /// it asked at all. The GNU spelling is followed by the probe's depfile path.
    constexpr std::array<std::string_view, 1> MsvcDependencyProbe { "/showIncludes" };
    constexpr std::array<std::string_view, 2> GnuDependencyProbe { "-MD", "-MF" };

    /// Asks a GNU driver to print its include search list.
    ///
    /// `-E -v` over an empty C++ input: `-v` is what makes it print the list, `-E`
    /// stops it before it tries to assemble anything, and `-x c++` names the
    /// language because the search list DIFFERS between C and C++ — the C++ one
    /// carries the standard library headers, which are most of what a fingerprint
    /// is trying to identify. The input path is appended by the caller.
    constexpr std::array<std::string_view, 4> GnuIncludeProbe { "-E", "-v", "-x", "c++" };

    /// `-###` over an empty input: print the frontend invocation, run nothing.
    ///
    /// The language is named explicitly (`/TP`, `-x c++`) because the input has no
    /// extension to read it from -- `NUL` and `/dev/null` alike -- and a driver that
    /// cannot tell which language it is being handed prints NO frontend invocation at
    /// all. That failure is silent and looks exactly like "this driver has no target
    /// to report", so the flag that prevents it belongs in the table beside the probe
    /// rather than in a comment at the call site.
    ///
    /// `/c` and `-c` keep it to a compile step: without them the driver also prints a
    /// link line, which carries no `-triple` and only makes the output longer -- and
    /// on a driver read through the `Target:` header, a link line is all there is.
    constexpr std::array<std::string_view, 3> MsvcTargetProbe { "-###", "/TP", "/c" };
    constexpr std::array<std::string_view, 4> GnuTargetProbe { "-###", "-x", "c++", "-c" };

    /// What every clang vendor's version line carries and no GCC's does.
    ///
    /// The marker, not a prefix match: a distribution puts its own name first
    /// (`Ubuntu clang version 20.1.2`, `Apple clang version 17.0.0`), which is the
    /// same vendor-prefixing that makes a banner comparison across two distributions
    /// fail. Here only the presence of the product name matters.
    constexpr std::string_view ClangBannerMarker = "clang version";

    /// Asks a driver that HAS a version option to state its version.
    ///
    /// Every driver but `cl` accepts this and exits 0, `clang-cl` included — it is
    /// clang's own option, not a GNU-family one, so this is not `GnuVersion`.
    /// MSVC's row is empty; see `DriverSpec::versionFlags`.
    ///
    /// No input path is appended, unlike `MsvcTargetProbe` and `GnuIncludeProbe`:
    /// those ask a driver a question ABOUT a compile and need something to pretend
    /// to compile, while this one asks who the driver is. Giving `cl` a `NUL` to go
    /// with an empty flag list would be handing it a source file.
    constexpr std::array<std::string_view, 1> VersionProbe { "--version" };

    constexpr std::array<DriverSpec, 5> Drivers { {
        { .flavor = Flavor::Unknown,
          .family = DriverFamily::None,
          .preprocessFlags = {},
          .dispatchPreprocessFlags = {},
          .preprocessedInput = {},
          .preprocessDropFlags = {},
          .dependencyProbeFlags = {},
          .usesDepfile = false,
          .includeDiscovery = IncludeDiscovery::None,
          .targetDiscovery = TargetDiscovery::None,
          .includeProbeFlags = {},
          .targetProbeFlags = {},
          // `--version` even here. An unrecognised driver is not cached, so this
          // value reaches nothing but a diagnostic -- and asking the one option
          // almost every tool answers beats asking nothing.
          .versionFlags = VersionProbe },
        { .flavor = Flavor::Cl,
          .family = DriverFamily::Msvc,
          .preprocessFlags = MsvcPreprocess,
          .dispatchPreprocessFlags = MsvcDispatchPreprocess,
          .preprocessedInput = MsvcPreprocessedInput,
          .preprocessDropFlags = MsvcDrop,
          .dependencyProbeFlags = MsvcDependencyProbe,
          .usesDepfile = false,
          // Layout rather than environment, because a Windows SERVICE inherits no
          // `INCLUDE`, and a worker started that way would otherwise carry no
          // toolchain content in its fingerprint at all. See the enumerator.
          .includeDiscovery = IncludeDiscovery::MsvcLayout,
          // Nothing to ask and nothing to state: `cl` has no `-###` and no
          // `--target`. Which code generator runs is decided by WHICH `cl.exe` is
          // invoked, and a launcher cannot restate that on a command line.
          .targetDiscovery = TargetDiscovery::None,
          .includeProbeFlags = {},
          .targetProbeFlags = {},
          // EMPTY, and that is the probe: `cl` prints its banner on every
          // invocation, so it needs no option to be asked -- and it has none, so
          // asking `--version` made it exit 2 and identify as the string `cl` on
          // every MSVC toolset alike. See `DriverSpec::versionFlags`.
          .versionFlags = {} },
        { .flavor = Flavor::ClangCl,
          .family = DriverFamily::Msvc,
          .preprocessFlags = MsvcPreprocess,
          .dispatchPreprocessFlags = MsvcDispatchPreprocess,
          .preprocessedInput = MsvcPreprocessedInput,
          .preprocessDropFlags = MsvcDrop,
          .dependencyProbeFlags = MsvcDependencyProbe,
          .usesDepfile = false,
          // Asked for its OWN resource directory, not handed `INCLUDE` and not
          // modelled off the VC layout: a service inherits no `INCLUDE` either, and
          // clang-cl lives outside the toolset it borrows. See the enumerator.
          .includeDiscovery = IncludeDiscovery::ClangResourceLayout,
          // The other half of what `ClangResourceLayout` left out. Dropping the VC
          // toolset and the SDK from the digest was right -- a worker opens neither,
          // compiling text the client already preprocessed -- but it also left the
          // fingerprint blind to `-fms-compatibility-version`, which clang derives
          // from that same borrowed install and DOES bake into code generation.
          // Stated on the dispatch line instead of digested into an identity.
          .targetDiscovery = TargetDiscovery::ClangDriverLine,
          .includeProbeFlags = {},
          .targetProbeFlags = MsvcTargetProbe,
          .versionFlags = VersionProbe },
        { .flavor = Flavor::Gcc,
          .family = DriverFamily::Gnu,
          .preprocessFlags = GnuPreprocess,
          .dispatchPreprocessFlags = GnuDispatchPreprocess,
          .preprocessedInput = GnuPreprocessedInput,
          .preprocessDropFlags = GnuDrop,
          .dependencyProbeFlags = GnuDependencyProbe,
          .usesDepfile = true,
          .includeDiscovery = IncludeDiscovery::GnuVerbose,
          // Asked, but never told. GCC prints no `-cc1` line -- its frontend takes no
          // `-triple` -- so the `Target:` header is all there is, and for this driver
          // that header is the whole answer rather than the unversioned half one.
          // It cannot be pinned (no `--target=`), so it identifies the code generator
          // for the KEY and states nothing on a dispatched line. Without it one g++
          // version string covers x86_64 and aarch64 alike, and an arch-independent
          // translation unit keys the same on both.
          .targetDiscovery = TargetDiscovery::GnuTargetLine,
          .includeProbeFlags = GnuIncludeProbe,
          .targetProbeFlags = GnuTargetProbe,
          .versionFlags = VersionProbe },
        { .flavor = Flavor::Clang,
          .family = DriverFamily::Gnu,
          .preprocessFlags = GnuPreprocess,
          .dispatchPreprocessFlags = GnuDispatchPreprocess,
          .preprocessedInput = GnuPreprocessedInput,
          .preprocessDropFlags = GnuDrop,
          .dependencyProbeFlags = GnuDependencyProbe,
          .usesDepfile = true,
          .includeDiscovery = IncludeDiscovery::GnuVerbose,
          .targetDiscovery = TargetDiscovery::ClangDriverLine,
          .includeProbeFlags = GnuIncludeProbe,
          .targetProbeFlags = GnuTargetProbe,
          .versionFlags = VersionProbe },
    } };

    // --- path-valued flags --------------------------------------------------
    //
    // The one table behind three consumers: the parser (which captures the
    // object output and the depfile), the preprocess line (which drops every
    // role but IncludeDir), and the cache key (which rewrites every value it can
    // to a canonical token). See PathValueFlags() in the header for why they must
    // not be three tables — while they were, the object output was relativized in
    // its separated spelling and not in its fused one, so a `/Fo<abs>` build baked
    // the producing machine's object path into every Windows key and two checkouts
    // at different roots could never share an entry.

    constexpr std::array<PathValueFlag, 13> PathValues { {
        // The prefix-map row first, being the longest spelling. GNU-only: `cl` has
        // no path-map switch at all, and clang-cl accepts `-ffile-prefix-map`
        // while ignoring it for the records that matter -- measured, an object
        // built with it still differs cross-root by the same 23 bytes in
        // `S_OBJNAME` and the embedded `-cc1` line.
        //
        // ONLY the debug spelling, and the omission of `-fmacro-prefix-map` and
        // `-ffile-prefix-map` is deliberate rather than an oversight. Every row
        // here is dropped from the preprocess line unless its role is
        // `IncludeDir`, and that is safe exactly when the flag cannot change
        // preprocessed text. Measured on gcc 14 and clang 20, source named
        // absolutely, `-E -P`: `-fdebug-prefix-map` leaves the text byte-identical,
        // while the other two rewrite `__FILE__` in it. A row for either would
        // therefore hash text the real compile does not produce -- and worse, a
        // dispatched compile would bake the UNMAPPED `__FILE__` into an object
        // stored under the same key a locally-mapped one uses. Unrecognised, they
        // reach the key verbatim and two checkouts simply miss, which is the safe
        // direction and is the behaviour that was already there.
        { .spelling = "-fdebug-prefix-map",
          .role = PathValueRole::PrefixMap,
          .families = DriverFamily::Gnu,
          .valueTailSeparator = '=' },
        { .spelling = "/external:I", .role = PathValueRole::IncludeDir, .families = DriverFamily::Msvc },
        { .spelling = "-external:I", .role = PathValueRole::IncludeDir, .families = DriverFamily::Msvc },
        { .spelling = "/Fo", .role = PathValueRole::ObjectOutput, .families = DriverFamily::Msvc },
        { .spelling = "-Fo", .role = PathValueRole::ObjectOutput, .families = DriverFamily::Msvc },
        { .spelling = "/Fd", .role = PathValueRole::DebugOutput, .families = DriverFamily::Msvc },
        { .spelling = "-Fd", .role = PathValueRole::DebugOutput, .families = DriverFamily::Msvc },
        { .spelling = "-MF", .role = PathValueRole::DepFile, .families = DriverFamily::Gnu },
        { .spelling = "-MT", .role = PathValueRole::DepTarget, .families = DriverFamily::Gnu },
        { .spelling = "-MQ", .role = PathValueRole::DepTarget, .families = DriverFamily::Gnu },
        { .spelling = "/I", .role = PathValueRole::IncludeDir, .families = DriverFamily::Msvc },
        { .spelling = "-I", .role = PathValueRole::IncludeDir, .families = DriverFamily::Any },
        { .spelling = "-o", .role = PathValueRole::ObjectOutput, .families = DriverFamily::Any },
    } };

    /// One flag that makes a compile write a second artefact, and which families
    /// spell it that way.
    struct SideArtefactFlag
    {
        std::string_view spelling;
        DriverFamily families;
    };

    /// Flags that make a compile write something BESIDES its object file.
    ///
    /// One row per spelling, exactly as PathValues does it and for the same reason:
    /// an MSVC driver accepts `-` for every option, and a row matched on introducer
    /// alone would let a GNU `-interface` through. See `ProducesSideArtefact` for
    /// why a line carrying one of these is not cacheable.
    ///
    /// The residual, recorded deliberately: clang can be told to emit a module
    /// interface through `-Xclang -emit-module-interface`, which is two arguments
    /// and matches nothing here. It is left out rather than half-matched, because a
    /// row that fires on `-Xclang` alone would un-cache every build that passes any
    /// `-Xclang` flag at all.
    constexpr std::array<SideArtefactFlag, 13> SideArtefacts { {
        { .spelling = "/interface", .families = DriverFamily::Msvc },
        { .spelling = "-interface", .families = DriverFamily::Msvc },
        { .spelling = "/internalPartition", .families = DriverFamily::Msvc },
        { .spelling = "-internalPartition", .families = DriverFamily::Msvc },
        { .spelling = "/exportHeader", .families = DriverFamily::Msvc },
        { .spelling = "-exportHeader", .families = DriverFamily::Msvc },
        { .spelling = "/ifcOutput", .families = DriverFamily::Msvc },
        { .spelling = "-ifcOutput", .families = DriverFamily::Msvc },
        { .spelling = "/Yc", .families = DriverFamily::Msvc },
        { .spelling = "-Yc", .families = DriverFamily::Msvc },
        { .spelling = "-fmodule-output", .families = DriverFamily::Gnu },
        { .spelling = "-fmodule-mapper", .families = DriverFamily::Gnu },
        { .spelling = "--precompile", .families = DriverFamily::Gnu },
    } };

    /// Which introducer characters each family's options start with.
    constexpr std::array<std::pair<DriverFamily, std::string_view>, 4> FamilyIntroducers { {
        { DriverFamily::None, "" },
        { DriverFamily::Msvc, "/-" },
        { DriverFamily::Gnu, "-" },
        { DriverFamily::Any, "/-" },
    } };

    /// True when a path-valued flag of this role must not reach the preprocess
    /// line. An object output would make the probe write an object instead of
    /// text on stdout, and a dependency flag would overwrite the build's own
    /// depfile with the probe's; an include directory is exactly what the
    /// preprocessor needs and stays.
    /// @param role The role to test.
    /// @return True when the flag (and its value) is dropped.
    [[nodiscard]] constexpr bool DroppedFromPreprocess(PathValueRole role) noexcept
    {
        return role != PathValueRole::IncludeDir;
    }

    /// Whether a driver reads its input's language off the file name, or compiles
    /// everything as C++ whatever the name says.
    ///
    /// The `++` drivers do the latter, in as many words: "g++ treats .c, .h and .i
    /// files as C++ source files instead of C source files". It is not a detail --
    /// the launcher has to state the language when it hands preprocessed text to a
    /// worker, and taking that from the extension alone would tell a worker to
    /// compile as C what this machine compiles as C++.
    enum class LanguageDefault : std::uint8_t
    {
        FromExtension,
        AlwaysCxx,
    };

    /// How a driver's basename is recognised. Order matters: the first match
    /// wins, so longer, more specific stems precede their prefixes
    /// ("clang-cl" before "clang", "c++" before "cc").
    struct NamePattern
    {
        std::string_view stem;
        Flavor flavor;
        LanguageDefault languageDefault;
    };

    constexpr std::array<NamePattern, 8> NamePatterns { {
        { .stem = "clang-cl", .flavor = Flavor::ClangCl, .languageDefault = LanguageDefault::FromExtension },
        { .stem = "clang++", .flavor = Flavor::Clang, .languageDefault = LanguageDefault::AlwaysCxx },
        { .stem = "clang", .flavor = Flavor::Clang, .languageDefault = LanguageDefault::FromExtension },
        { .stem = "g++", .flavor = Flavor::Gcc, .languageDefault = LanguageDefault::AlwaysCxx },
        { .stem = "gcc", .flavor = Flavor::Gcc, .languageDefault = LanguageDefault::FromExtension },
        { .stem = "c++", .flavor = Flavor::Gcc, .languageDefault = LanguageDefault::AlwaysCxx },
        { .stem = "cc", .flavor = Flavor::Gcc, .languageDefault = LanguageDefault::FromExtension },
        { .stem = "cl", .flavor = Flavor::Cl, .languageDefault = LanguageDefault::FromExtension },
    } };

    /// Flags that state the input's language ON THE COMMAND LINE, overriding both
    /// the driver's default and the file's extension.
    ///
    /// A build reaches these more often than it looks: CMake's
    /// `set_source_files_properties(x.c PROPERTIES LANGUAGE CXX)` emits `/TP` or
    /// `-x c++`. A dispatched compile appends its own spelling of "this input is
    /// preprocessed <language>" LAST, so it would silently override the build's --
    /// compiling as C what the build asked to be compiled as C++, and storing that
    /// under the key. The command line is not re-derivable from anything the worker
    /// sees, so such a compile is refused rather than guessed at.
    ///
    /// `/Tc` and `/Tp` are here for a second reason as well: they name a FILE, and
    /// with a bare file name they carry no separator, so the path filter lets them
    /// past.
    ///
    /// Each row carries the language it names, because refusing was only ever the
    /// best answer available to a table that could not say WHICH language (#232).
    /// The launcher appends its own "this text is preprocessed <language>" last, and
    /// for `/TP` that appended flag is `/TP` -- the very argument being refused. A
    /// row whose language is known is therefore dropped rather than refused: the
    /// appended spelling states the same language and additionally states that the
    /// text is preprocessed, which is strictly more than the build said.
    ///
    /// CMake emits `/TP` on every C++ source it compiles with MSVC, so this table
    /// refusing it meant **no CMake + MSVC translation unit was ever dispatchable**.
    /// The fleet cached normally and distributed nothing, with every scheduler
    /// counter at zero because no lease was ever requested.
    struct LanguageSelector
    {
        std::string_view spelling; ///< As it appears on the command line.
        DriverFamily families;     ///< Which family's spellings may match.
        /// The language this spelling names, or nullopt when the spelling does not
        /// carry one by itself -- `-x` takes it in a value, and `/Tp` names a file.
        std::optional<SourceLanguage> language;
        /// Whether the spelling attaches a FILE. Those can never be dropped: the
        /// path is the build's own source, which must not reach a worker, and the
        /// worker compiles a scratch file of its own name regardless.
        bool namesAFile { false };
    };

    constexpr std::array<LanguageSelector, 9> LanguageSelectors { {
        { .spelling = "/TC", .families = DriverFamily::Msvc, .language = SourceLanguage::C },
        { .spelling = "-TC", .families = DriverFamily::Msvc, .language = SourceLanguage::C },
        { .spelling = "/TP", .families = DriverFamily::Msvc, .language = SourceLanguage::Cxx },
        { .spelling = "-TP", .families = DriverFamily::Msvc, .language = SourceLanguage::Cxx },
        { .spelling = "/Tc", .families = DriverFamily::Msvc, .language = std::nullopt, .namesAFile = true },
        { .spelling = "-Tc", .families = DriverFamily::Msvc, .language = std::nullopt, .namesAFile = true },
        { .spelling = "/Tp", .families = DriverFamily::Msvc, .language = std::nullopt, .namesAFile = true },
        { .spelling = "-Tp", .families = DriverFamily::Msvc, .language = std::nullopt, .namesAFile = true },
        { .spelling = "-x", .families = DriverFamily::Gnu, .language = std::nullopt },
    } };

    /// Debug-info flags that make MSVC write a PDB BESIDE the object.
    ///
    /// `/Fd` only says WHERE one would go; these decide whether one is written at
    /// all. With `/Z7` the debug info lives in the object itself and a dispatched
    /// compile carries back everything the build asked for -- so `/Fd` names a file
    /// nothing produces and is dropped. With these, a worker would have to send back
    /// a second artefact nothing on the wire carries.
    ///
    /// `cmake/portable/CompileCache.cmake` rewrites `/Zi` to `/Z7` whenever a
    /// launcher is active, for this same reason, so this fires only for a build that
    /// reached the launcher another way.
    constexpr std::array<std::string_view, 4> MsvcSharedPdb { "/Zi", "-Zi", "/ZI", "-ZI" };

    /// What a GNU `-x` value names. Only the four plain languages: anything else
    /// (`assembler`, `c-header`, a `-cpp-output` form) has no exact `SourceLanguage`
    /// and keeps being refused, because guessing at it is how a worker comes to
    /// compile something other than what the build asked for.
    constexpr std::array<std::pair<std::string_view, SourceLanguage>, 4> GnuLanguageNames { {
        { "c", SourceLanguage::C },
        { "c++", SourceLanguage::Cxx },
        { "objective-c", SourceLanguage::ObjectiveC },
        { "objective-c++", SourceLanguage::ObjectiveCxx },
    } };

    /// Classify the compiler flavor from its basename.
    ///
    /// Tolerates version suffixes (`g++-14`, `clang-18`) and the `.exe`
    /// extension, both of which are ordinary on real build systems.
    ///
    /// @param compiler argv[0] as invoked.
    /// @return The matching flavor, or Unknown.
    [[nodiscard]] Flavor ClassifyCompilerImpl(std::string_view compiler)
    {
        std::string const base = NormalizedCompilerName(compiler);

        // The match is consumed through a view rather than bound to a named
        // iterator. An iterator variable cannot be spelled portably here:
        // `auto const*` compiles only where std::array's iterator is a raw
        // pointer (libstdc++, libc++) and fails on MSVC's class-type iterator,
        // plain `auto const` trips readability-qualified-auto where it *is* a
        // pointer, and naming the type trips modernize-use-auto.
        for (NamePattern const& pattern: NamePatterns)
        {
            if (!base.starts_with(pattern.stem))
                continue;
            // Anything after the stem must be a version suffix ("-14", "-18"),
            // never more name — so "clanger" does not read as clang.
            auto const rest = std::string_view { base }.substr(pattern.stem.size());
            if (rest.empty() || rest.front() == '-')
                return pattern.flavor;
        }
        return Flavor::Unknown;
    }

    /// Whether this driver compiles every input as C++ regardless of its name.
    ///
    /// The same walk as ClassifyCompilerImpl over the same table, because it is the
    /// same question asked of a different column -- `g++-14` and `clang++.exe` have
    /// to be recognised here exactly as they are there, and a second spelling of
    /// "which driver is this" is a second thing to keep in step.
    /// @param compiler argv[0] as invoked.
    /// @return True for a `++` driver.
    [[nodiscard]] bool CompilesEverythingAsCxx(std::string_view compiler)
    {
        std::string const base = NormalizedCompilerName(compiler);

        for (NamePattern const& pattern: NamePatterns)
        {
            if (!base.starts_with(pattern.stem))
                continue;
            auto const rest = std::string_view { base }.substr(pattern.stem.size());
            if (rest.empty() || rest.front() == '-')
                return pattern.languageDefault == LanguageDefault::AlwaysCxx;
        }
        return false;
    }

    /// True if `a` is an option (starts with one of the driver's introducers).
    [[nodiscard]] bool IsOption(std::string_view a, DriverSpec const& driver)
    {
        return !a.empty() && IntroducersOf(driver.family).contains(a.front());
    }

    /// True if `flag`, given bare, consumes the following argument as its value.
    ///
    /// Every path-valued flag does, and nothing else the launcher knows about
    /// does — which is why this is a lookup in the shared table rather than a
    /// list of its own. It used to be one, and the object output ended up
    /// relativized in the spelling that table happened to cover.
    ///
    /// @param flag The flag as it appeared on the command line.
    /// @return True when the next argument belongs to it.
    [[nodiscard]] bool TakesValue(std::string_view flag)
    {
        return std::ranges::any_of(PathValues, [flag](PathValueFlag const& row) { return row.spelling == flag; });
    }

    /// Characters that may separate a flag from a value fused onto it.
    ///
    /// Only a value-taking flag may appear in joined form at all, and the join
    /// must be a value — never more flag name. `-MFdep.d` and `-o=x.o` are the
    /// joined forms; `-MP` merely starts with `-M`, and `-coverage` with `-c`.
    constexpr std::string_view JoinSeparators = "=:";

    /// True if `arg` is `flag` carrying a fused value, rather than a different
    /// flag that merely begins with the same characters.
    ///
    /// Getting this wrong is expensive and silent: matching on `starts_with`
    /// alone makes `-c` swallow `-coverage` and `/c` swallow `/clr`, dropping a
    /// real flag from the preprocess line (and stranding its value as a stray
    /// input file), so the probe fails and every such TU compiles uncached
    /// forever while all unit tests still pass.
    ///
    /// A value-taking flag joins directly (`-MFdep.d`, `/Fox.obj`) or through a
    /// separator (`-MF=dep.d`). A flag that takes no value has no joined form,
    /// so anything longer than it is a different flag.
    ///
    /// @param arg  The argument as it appeared on the command line.
    /// @param flag The candidate flag.
    /// @return True when `arg` is `flag` with a value fused onto it.
    [[nodiscard]] bool IsJoinedValue(std::string_view arg, std::string_view flag)
    {
        if (!arg.starts_with(flag) || arg.size() <= flag.size())
            return false;
        if (!TakesValue(flag))
            return false;
        auto const tail = arg.substr(flag.size());
        // `/Fo` and `-o` fuse their value directly; a separator is also accepted
        // so `-MF=dep.d` is not mistaken for an unrelated flag.
        return !JoinSeparators.contains(tail.front()) || tail.size() > 1;
    }

    /// Drop a leading join separator from a fused value, so `-MF=dep.d` and
    /// `-MFdep.d` both yield `dep.d`.
    /// @param tail The text following the flag in its joined form.
    /// @return The value proper.
    [[nodiscard]] std::string_view StripJoinSeparator(std::string_view tail)
    {
        if (!tail.empty() && JoinSeparators.contains(tail.front()))
            tail.remove_prefix(1);
        return tail;
    }

    /// True if `arg` is `flag` exactly, or `flag` with a fused value.
    /// @param arg  The argument as it appeared on the command line.
    /// @param flag The candidate flag.
    /// @return True on either the bare or the joined form.
    [[nodiscard]] bool MatchesFlag(std::string_view arg, std::string_view flag)
    {
        return arg == flag || IsJoinedValue(arg, flag);
    }

    /// Whether `arg` states the input language explicitly.
    /// @param arg    The argument as it appeared on the command line.
    /// @param family Which family's spellings may match.
    /// @return True when the build has named the language itself.
    [[nodiscard]] LanguageSelector const* MatchLanguageSelector(std::string_view arg, DriverFamily family)
    {
        auto const introducers = IntroducersOf(family);
        if (arg.empty() || introducers.empty() || !introducers.contains(arg.front()))
            return nullptr;
        // A plain prefix test, and NOT MatchesFlag: that one only recognises a fused
        // value for a flag the path-value table knows takes one, so it reads
        // `-xc++` and `/Tcother.c` as ordinary arguments -- which is exactly how
        // they would have reached a worker.
        // A loop rather than a named `find_if` iterator, for the reason
        // `ClassifyCompilerImpl` records: `std::array`'s iterator is a raw pointer on
        // libstdc++ and libc++ and a class type on MSVC, so no single spelling of the
        // variable satisfies `readability-qualified-auto` everywhere.
        for (LanguageSelector const& row: LanguageSelectors)
            if (introducers.contains(row.spelling.front()) && Overlaps(row.families, family)
                && arg.starts_with(row.spelling))
                return &row;
        return nullptr;
    }

    /// The language a matched selector names, given the rest of the command line.
    ///
    /// @param row The matched row.
    /// @param arg The argument as it appeared, so a fused `-xc++` is readable.
    /// @param next The following argument, for the separated `-x c++` form.
    /// @return The language, or nullopt when this occurrence cannot be reduced to
    ///         one -- which is a refusal rather than a guess.
    [[nodiscard]] std::optional<SourceLanguage> LanguageNamedBy(LanguageSelector const& row,
                                                                std::string_view arg,
                                                                std::string_view next)
    {
        if (row.namesAFile)
            return std::nullopt;
        if (row.language.has_value())
            return row.language;

        // `-x`: the language is the value, fused (`-xc++`) or separate (`-x c++`).
        auto const fused = arg.substr(row.spelling.size());
        auto const value = fused.empty() ? next : fused;
        for (auto const& [name, language]: GnuLanguageNames)
            if (name == value)
                return language;
        return std::nullopt;
    }

    /// The ParsedCommand field a path-valued flag's value belongs in.
    ///
    /// A pointer-to-member rather than a branch per role, so the fused and
    /// separated forms are unpacked once for every flag the parser captures.
    /// @param role The matched flag's role.
    /// @return The field to write, or nullptr for a role the parser does not capture.
    [[nodiscard]] constexpr std::string ParsedCommand::* DestinationFor(PathValueRole role) noexcept
    {
        switch (role)
        {
            case PathValueRole::ObjectOutput:
                return &ParsedCommand::objPath;
            case PathValueRole::DepFile:
                return &ParsedCommand::depPath;
            case PathValueRole::IncludeDir:
            case PathValueRole::DepTarget:
            // Nothing captures it: the launcher never reads or writes the PDB, it
            // only has to keep the path out of the key and off a worker's line.
            case PathValueRole::DebugOutput:
            // Nor this: the compiler applies the rewrite itself, and the launcher
            // only has to relativize the root it names so the key stays portable.
            case PathValueRole::PrefixMap:
                break;
        }
        return nullptr;
    }

    /// Find the flag `arg` must be dropped as, if any: one of the driver's own
    /// drop flags, or a path-valued flag whose role has no business on a
    /// preprocess line.
    /// @param arg    The argument as it appeared on the command line.
    /// @param driver The driver whose line is being built.
    /// @return The matched flag spelling, or nullopt to keep the argument.
    [[nodiscard]] std::optional<std::string_view> MatchDroppedFlag(std::string_view arg, DriverSpec const& driver)
    {
        for (std::string_view const flag: driver.preprocessDropFlags)
            if (MatchesFlag(arg, flag))
                return flag;

        if (auto const match = MatchPathValueFlag(arg, IntroducersOf(driver.family), driver.family);
            match.has_value() && DroppedFromPreprocess(match->flag.role))
            return match->flag.spelling;

        return std::nullopt;
    }

} // namespace

std::span<PathValueFlag const> PathValueFlags()
{
    return PathValues;
}

std::string_view TargetPinPrefixFor(TargetDiscovery discovery) noexcept
{
    // No `default:`, so a mechanism added to the table fails to compile here rather
    // than silently pinning nothing -- which would present as a worker quietly going
    // back to choosing its own target.
    switch (discovery)
    {
        case TargetDiscovery::None:
            return {};
        case TargetDiscovery::ClangDriverLine:
            return "--target=";
        case TargetDiscovery::GnuTargetLine:
            // Identified, never stated. `gcc` is a fixed-target driver: it has no
            // `--target=`, so putting one on a dispatched line would fail the compile
            // rather than direct it. Its target still belongs in the cache key.
            return {};
    }
    return {};
}

std::string_view IntroducersOf(DriverFamily family) noexcept
{
    for (auto const& [candidate, introducers]: FamilyIntroducers)
        if (candidate == family)
            return introducers;
    return {};
}

std::optional<PathValueMatch> MatchPathValueFlag(std::string_view arg, std::string_view introducers, DriverFamily families)
{
    if (arg.empty() || !introducers.contains(arg.front()))
        return std::nullopt;

    for (PathValueFlag const& row: PathValues)
    {
        // A row whose introducer this context does not recognise cannot match:
        // under a POSIX layout `/I` is the head of an absolute path, not a flag.
        if (!introducers.contains(row.spelling.front()) || !Overlaps(row.families, families))
            continue;
        if (!MatchesFlag(arg, row.spelling))
            continue;

        // A fused value is never empty — IsJoinedValue rejects a bare separator —
        // so an empty `value` unambiguously means "the value is the next argument".
        if (arg.size() == row.spelling.size())
            return PathValueMatch { .flag = row, .prefix = {}, .value = {}, .valueTail = {} };

        auto const tail = arg.substr(row.spelling.size());
        auto const value = StripJoinSeparator(tail);
        auto const prefix = arg.substr(0, arg.size() - value.size());

        // A row that says its value carries a tail is split HERE, so no consumer
        // re-derives where the path ends.
        //
        // At the LAST separator, which follows GCC. The two drivers in this row's
        // family DISAGREE and there is no answer that satisfies both -- measured
        // with a directory literally named `a=b`:
        // `-fdebug-prefix-map=<dir>/a=b=ZZZ` remaps to `ZZZ` under gcc and to
        // `b=ZZZ=b` under clang, so gcc cuts at the last separator and clang at
        // the first. The disagreement is unreachable unless a mapped root
        // CONTAINS a separator, and it costs a MISS rather than a mis-serve: the
        // head the launcher isolates then lies under no root, so the argument
        // comes back verbatim and the two checkouts key apart.
        //
        // A value with no separator at all is malformed and the driver will say
        // so -- it is reported as a bare path rather than refused, because the
        // diagnostic belongs to the compiler and relativizing a root it is about
        // to reject changes nothing.
        if (row.valueTailSeparator != '\0')
            if (auto const at = value.rfind(row.valueTailSeparator); at != std::string_view::npos)
                return PathValueMatch {
                    .flag = row, .prefix = prefix, .value = value.substr(0, at), .valueTail = value.substr(at)
                };

        return PathValueMatch { .flag = row, .prefix = prefix, .value = value, .valueTail = {} };
    }
    return std::nullopt;
}

bool ProducesSideArtefact(std::string_view arg, DriverFamily family)
{
    auto const introducers = IntroducersOf(family);
    if (arg.empty() || introducers.empty() || !introducers.contains(arg.front()))
        return false;

    // A prefix test, and NOT MatchesFlag, for the reason IsLanguageSelector gives:
    // MatchesFlag only recognises a fused value for a flag the path-value table
    // knows takes one, and every row here takes its value in the spelling a build
    // actually writes -- `/Yc"pch.h"`, `-fmodule-output=x.pcm`. Matching only the
    // bare form would have caught the shape nobody writes and missed the one
    // everybody does.
    return std::ranges::any_of(SideArtefacts, [&](SideArtefactFlag const& row) {
        return introducers.contains(row.spelling.front()) && Overlaps(row.families, family) && arg.starts_with(row.spelling);
    });
}

std::string_view ObjectOutputPrefixFor(DriverFamily family)
{
    // The MOST SPECIFIC row wins: `-o` is DriverFamily::Any and would match an
    // MSVC driver too, so a plain "first row that overlaps" scan would hand `cl`
    // the very flag it does not accept. Preferring a row whose families are not
    // Any is what makes the table answer this question correctly.
    std::string_view fallback;
    for (auto const& row: PathValueFlags())
    {
        if (row.role != PathValueRole::ObjectOutput || !Overlaps(row.families, family))
            continue;
        if (row.families != DriverFamily::Any)
            return row.spelling;
        if (fallback.empty())
            fallback = row.spelling;
    }
    return fallback.empty() ? std::string_view { "-o" } : fallback;
}

std::string NormalizedCompilerName(std::string_view compiler)
{
    std::string base = PathCanon::AsciiLower(Basename(compiler));
    if (base.ends_with(".exe"))
        base.resize(base.size() - 4);
    return base;
}

Flavor ClassifyCompiler(std::string_view compiler)
{
    return ClassifyCompilerImpl(compiler);
}

Flavor ClassifyCompilerFromBanner(Flavor named, std::string_view banner) noexcept
{
    // The GNU-driver pair only. `clang-cl` prints `clang version ...` exactly as
    // plain clang does, so the name is the only thing separating those two drivers
    // and a banner test here would collapse them into one.
    if (named != Flavor::Gcc && named != Flavor::Clang)
        return named;

    // ONE direction, on POSITIVE evidence only. Every vendor's spelling carries the
    // marker -- `clang version 22.1.3`, `Apple clang version 17.0.0`,
    // `Ubuntu clang version 20.1.2` -- so seeing it is proof. Not seeing it is not
    // proof of the opposite: GCC has no equally reliable marker of its own (vanilla
    // prints `(GCC)`, Ubuntu prints `(Ubuntu 14.2.0-...)`), and more to the point a
    // driver that could not be RUN falls back to its own basename, so `clang++`
    // whose `--version` failed would arrive here carrying the string `clang++` and
    // be demoted into the gcc row -- unversioned target, no pin, on a driver whose
    // name said exactly what it was.
    //
    // So an unrecognised banner leaves the name-based guess standing, which is both
    // the safe direction and what this did before there was a correction at all.
    return banner.contains(ClangBannerMarker) ? Flavor::Clang : named;
}

DriverSpec const& DriverOf(Flavor flavor)
{
    // Iterated rather than searched via a named iterator — see the note in
    // ClassifyCompiler for why an iterator variable is not portable here.
    for (DriverSpec const& spec: Drivers)
        if (spec.flavor == flavor)
            return spec;
    return Drivers.front();
}

std::optional<SourceLanguage> LanguageOfSource(std::string_view path)
{
    // The final component first: a directory is free to carry a dot
    // (`../build.release/src/a.cpp`), and the extension of a path is a property of
    // its last component only.
    auto const slash = path.find_last_of("/\\");
    auto const name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    auto const dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == name.size())
        return std::nullopt;
    auto const extension = name.substr(dot);

    // Asked BEFORE the case-insensitive lookup, because the ambiguity is exactly a
    // difference of case: folding first is what would turn `.C` into the C row.
    if (std::ranges::contains(AmbiguousSourceExtensions, extension))
        return std::nullopt;

    auto const folded = PathCanon::AsciiLower(extension);
    for (auto const& [spelling, language]: SourceExtensions)
        if (folded == spelling)
            return language;
    return std::nullopt;
}

std::string_view DescribeLanguage(SourceLanguage language) noexcept
{
    for (auto const& spec: LanguageSpecs)
        if (spec.language == language)
            return spec.name;
    return "unknown";
}

std::string_view UncacheableBecause(SourceLanguage language) noexcept
{
    for (auto const& spec: LanguageSpecs)
        if (spec.language == language)
            return spec.refused;
    return {};
}

std::optional<std::span<std::string_view const>> PreprocessedInputFlagsFor(DriverSpec const& driver, SourceLanguage language)
{
    for (auto const& spelling: driver.preprocessedInput)
        if (spelling.language == language)
            return spelling.flags;
    return std::nullopt;
}

ParsedCommand ParseCommand(std::span<std::string const> argv)
{
    ParsedCommand out;
    if (argv.empty())
        return out;

    out.compiler = argv.front();
    out.flavor = ClassifyCompilerImpl(out.compiler);
    auto const& driver = DriverOf(out.flavor);
    if (out.flavor == Flavor::Unknown)
        return out;

    auto const args = argv.subspan(1);
    bool sawCompileOnly = false;
    bool preprocessOnly = false;
    std::size_t skipUntil = 0; // index the next iteration must not re-process

    for (auto const i: std::views::iota(std::size_t { 0 }, args.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = args[i];

        // Inline dependency reporting (MSVC drivers only).
        if (a == "/showIncludes" || a == "-showIncludes")
        {
            out.wantShowIncludes = true;
            continue;
        }

        // Compile-only / preprocess-only markers decide cacheability below.
        if (a == "/c" || a == "-c")
        {
            sawCompileOnly = true;
            continue;
        }
        if (a == "-E" || a == "/EP" || a == "/P")
        {
            preprocessOnly = true;
            continue;
        }

        // Path-valued flags, in any spelling this driver accepts, joined
        // (`/Fo<path>`, `-MFdep.d`) or separated (`/Fo <path>`, `-o <path>`).
        // Which of them the parser captures is decided by the row's role, so a
        // driver that spells the object output differently is a table row rather
        // than a branch here — as `-o`, which every MSVC driver takes alongside
        // `/Fo`, used to be.
        if (auto const match = MatchPathValueFlag(a, IntroducersOf(driver.family), driver.family))
        {
            auto const destination = DestinationFor(match->flag.role);
            // An include directory or a dependency target goes nowhere, and its
            // separated value is deliberately left to be scanned rather than
            // consumed: it is not a source path, so nothing downstream reads it.
            // What a stored depfile must not have respelled is read from the
            // depfile itself, structurally — see ParseDepFileTargets.
            if (destination == nullptr)
                continue;

            if (!match->value.empty())
            {
                out.*destination = std::string { match->value };
                continue;
            }
            if (i + 1 < args.size())
            {
                out.*destination = args[i + 1];
                skipUntil = i + 2; // consume the value argument
            }
            continue;
        }

        // Asked of every argument, and of the SOURCE too by way of its extension
        // below: a compile that also writes a BMI or a precompiled header is not
        // one a cache hit can reproduce.
        if (ProducesSideArtefact(a, driver.family))
        {
            out.sideArtefact = true;
            continue;
        }

        // A bare argument ending in a source suffix is the translation unit.
        if (!IsOption(a, driver) && IsSourceSuffix(PathCanon::AsciiLower(a)))
        {
            // First source wins; a second source means a multi-TU line we do
            // not cache (leave parsedOk false).
            if (out.source.empty())
                out.source = std::string { a };
            else
                out.source.clear(); // ambiguous — force fallback
        }
    }

    // A cacheable line compiles exactly one TU to an object. A preprocess-only
    // run produces text, not an object, so it is never cached; a line with no
    // -c/​/c is a link (or a compile-and-link) and is likewise left alone.
    //
    // The object path must be known, too. `g++ -c a.cpp` (no -o) is a perfectly
    // ordinary compile that defaults its output to ./a.o, but the launcher has
    // no path to read the object back from or write it to — treating it as
    // cacheable makes every such compile report a MISS and then fail to store,
    // forever, and would hand an empty path to the file writer on a hit.
    //
    // A module interface unit reaches a compiler two ways -- as `foo.ixx`, and as
    // `cl /interface foo.cpp` -- and both must be stepped over for one reason: what
    // a hit reproduces is the object and the dependency record, so the BMI beside it
    // would afterwards be missing (which fails loudly) or left from a previous build
    // (which does not). The extension is asked of the LANGUAGE table so that the
    // rule has one home, and the flag of `ProducesSideArtefact` above, which also
    // covers a precompiled header.
    if (auto const language = LanguageOfSource(out.source); language.has_value() && !UncacheableBecause(*language).empty())
        out.sideArtefact = true;

    out.parsedOk = !out.source.empty() && !out.objPath.empty() && sawCompileOnly && !preprocessOnly && !out.sideArtefact;
    return out;
}

/// Whether `arg` could still make a compiler reach a file, after the path-valued
/// flags this launcher knows about have already been removed.
///
/// A separator is the test, not a list of spellings, precisely because the list is
/// what cannot be kept complete: `-isystem`, `--sysroot`, `-B`, `-specs=`,
/// `-fplugin=` and `@file` all reach a file and none of them is a `PathValueFlags()`
/// row. `@` is called out on its own because a response file names a path with no
/// separator at all when it sits in the working directory.
///
/// The **introducer is skipped before the separator is looked for**, and which
/// characters introduce is the driver's answer rather than this function's. `/`
/// starts an option for an MSVC driver and an absolute path everywhere else -- the
/// rule this launcher already lives by -- so testing the raw argument would refuse
/// `/std:c++20` and `/O2`, i.e. every MSVC compile, while a `\` inside
/// `/DCONFIG=C:\x` is still exactly the signal being looked for.
/// @param arg One surviving argument.
/// @param family The driver family whose spellings apply.
/// @return True when the argument must not be sent to a worker.
[[nodiscard]] bool CouldNameAFile(std::string_view arg, DriverFamily family)
{
    if (arg.starts_with('@'))
        return true;
    // One introducer only: a second `/` is a path separator even on Windows.
    auto const body = !arg.empty() && IntroducersOf(family).contains(arg.front()) ? arg.substr(1) : arg;
    return body.contains('/') || body.contains('\\');
}

std::expected<std::vector<std::string>, std::string> RemoteCompileArgs(ParsedCommand const& cmd,
                                                                       std::span<std::string const> argv,
                                                                       std::string_view targetTriple)
{
    auto const& driver = DriverOf(cmd.flavor);

    // Asked FIRST, before a single argument is examined: what this decides is
    // whether the job can be dispatched at all, and a refusal that costs nothing is
    // worth reaching before one that costs a scan.
    auto language = LanguageOfSource(cmd.source);
    if (!language.has_value())
        return std::unexpected(std::format("no language is named unambiguously by the extension of {}", cmd.source));
    if (auto const because = UncacheableBecause(*language); !because.empty())
        return std::unexpected(std::format("{} is never dispatched: {}", DescribeLanguage(*language), because));

    // The DRIVER overrides the extension where it has a default of its own: `g++
    // -c a.c` compiles C++, so telling a worker `-x cpp-output` would have it
    // compile as C what this machine compiles as C++, and store that under the key.
    // Only the C row moves -- a driver that always compiles C++ says nothing about
    // an Objective-C source, whose own driver default is unchanged.
    if (*language == SourceLanguage::C && CompilesEverythingAsCxx(cmd.compiler))
        language = SourceLanguage::Cxx;

    // And the BUILD's own selector outranks both, which is the order the comment on
    // `LanguageSelectors` describes and nothing implemented: `/TP` on a `.c` file
    // means C++, whatever the extension or the driver would have said. Resolved
    // here, before the language is used, so the flags appended below state what the
    // build asked for rather than overriding it.
    //
    // A selector that cannot be reduced to a language -- one naming a FILE, or an
    // `-x` value with no exact `SourceLanguage` -- is still refused. Refusing costs
    // one local compile; guessing hands back an object nobody asked for.
    for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
    {
        auto const* row = MatchLanguageSelector(argv[i], driver.family);
        if (row == nullptr)
            continue;
        auto const named = LanguageNamedBy(*row, argv[i], i + 1 < argv.size() ? argv[i + 1] : std::string_view {});
        if (!named.has_value())
            return std::unexpected(
                std::format("the command line names the input language itself ({}) in a form this launcher cannot "
                            "restate to a worker",
                            argv[i]));
        language = named;
    }

    // A shared PDB is a second artefact, and only the object comes back. Refused
    // before anything is sent, on the same reasoning as a module interface unit.
    if (driver.family == DriverFamily::Msvc)
        for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
            if (std::ranges::contains(MsvcSharedPdb, argv[i]))
                return std::unexpected(
                    std::format("{} writes debug info to a shared PDB, which a dispatched compile cannot bring back; "
                                "build with /Z7 to distribute this",
                                argv[i]));

    auto const preprocessedInput = PreprocessedInputFlagsFor(driver, *language);
    if (!preprocessedInput.has_value())
        return std::unexpected(
            std::format("this driver has no way to be handed preprocessed {}", DescribeLanguage(*language)));

    std::vector<std::string> out;
    out.reserve(argv.size() + 1);

    // FIRST, before a single one of the build's own arguments. This states the
    // default the client's driver would have used; the build's own `--target=` or
    // `-m32` comes later and still wins, which is what happens locally and is why
    // probing the ambient default is enough. The prefix comes from the mechanism
    // that produced the triple, so a driver with nothing to state pins nothing even
    // if a caller hands it a value.
    if (auto const prefix = TargetPinPrefixFor(driver.targetDiscovery); !prefix.empty() && !targetTriple.empty())
        out.emplace_back(std::format("{}{}", prefix, targetTriple));

    std::size_t skipUntil = 1; // argv[0] is the compiler; the worker picks its own
    for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = argv[i];

        // The source itself never travels: the worker compiles preprocessed text
        // from a file of its own. Matched by value against the parsed source rather
        // than by position, because a build system is free to put it anywhere.
        if (a == cmd.source)
            continue;

        // The compile-only marker and the dependency switches, off the driver's own
        // list. Same rule PreprocessCommand applies, and the same reason for reading
        // it here rather than restating it: a spelling added to that table has to be
        // dropped by every consumer or the one that missed it silently diverges.
        if (auto const dropped = MatchDroppedFlag(a, driver))
        {
            if (a == *dropped && TakesValue(*dropped) && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }

        // EVERY path-valued flag, not merely the ones the preprocess line drops.
        // An include directory is the difference between the two lists: the probe
        // needs it (it is still resolving headers), and the worker must not have it
        // (the headers are already inlined, and the path names nothing there).
        if (auto const match = MatchPathValueFlag(a, IntroducersOf(driver.family), driver.family))
        {
            // Only a bare occurrence consumes the next argument; a fused one already
            // carries its value, and eating the successor would drop a real flag.
            if (match->value.empty() && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }

        // A language the BUILD named itself, already folded into `language` above.
        // Dropped rather than forwarded: the flags appended at the end of this
        // function say the same thing and say more -- "this text is preprocessed
        // <language>" rather than "this source is <language>" -- and forwarding both
        // would leave two spellings of the input's language on one command line.
        //
        // The unreducible forms never reach here; the loop above refused them.
        if (auto const* row = MatchLanguageSelector(a, driver.family))
        {
            // The separated `-x c++` form owns the next argument too, and leaving
            // its value behind would hand the worker a bare `c++` to open as a file.
            if (a == row->spelling && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }

        // The positive check, and the last word. See the header: refusing the whole
        // command line is the only safe answer, because stripping an argument this
        // function does not recognise would change the generated code.
        if (CouldNameAFile(a, driver.family))
            return std::unexpected(std::format("argument {} could name a file this launcher cannot account for", a));

        out.emplace_back(a);
    }

    // Appended last, so a build's own `-x` (if any) is overridden rather than
    // overriding: the input genuinely IS preprocessed output whatever the build
    // thought it was handing over.
    for (auto const& flag: *preprocessedInput)
        out.emplace_back(flag);
    return out;
}

std::string CompilerWorkingDirectory(std::string_view physicalDirectory, std::string_view environmentPwd)
{
    if (environmentPwd.empty() || physicalDirectory.empty())
        return std::string { physicalDirectory };

    // Both sides through `PathFromNarrowText`: `PWD` is text some other process wrote,
    // so on a host where a `char` is not UTF-8 it can be bytes `std::filesystem::path`'s
    // narrow constructor throws on, before any `error_code` overload runs. A spelling
    // this host cannot read is one no comparison can be made about, which is the
    // fallback answer anyway.
    auto const logical = PathFromNarrowText(environmentPwd);
    auto const physical = PathFromNarrowText(physicalDirectory);
    if (!logical.has_value() || !physical.has_value())
        return std::string { physicalDirectory };

    // POSIX-ROOTED first, and not merely as an optimisation: `equivalent` resolves a
    // relative path against this process's own working directory, so `PWD=build` inside
    // `.../build` compares EQUAL and would be returned as a compilation directory that
    // is not absolute. Measured: `PWD=relative/bits` fell back on both drivers.
    //
    // A leading `/` and NOT `path::is_absolute()`, which is the same test on POSIX and
    // strictly more permissive on a Windows layout -- it admits `D:/work` and
    // `\\host\share`, and NEITHER Windows driver consults `PWD` for either. libiberty's
    // `getpwd()`, which is where gcc's `DW_AT_comp_dir` comes from, gates on
    // `*p == '/'` before it stats anything; LLVM does the `PWD` dance only in
    // `Unix/Path.inc`, its Windows path being `GetCurrentDirectoryW` with no `PWD` in
    // it. So modelling this as `is_absolute()` would make a MinGW client predict a
    // spelling its own compiler never uses, send a mapping the local compile did not
    // apply, and rebuild #506's asymmetry under a correct key.
    //
    // **Read rather than measured** -- the five-row table on the declaration is Linux,
    // and no Windows GNU-layout driver was available to run it against. Stated
    // separately because a reader cannot recover the difference and would otherwise
    // inherit it as measurement. An MSYS-style `/d/work` still reaches the `stat` here
    // and still falls back, because that spelling resolves for the shell and not for
    // the driver -- so the two agree by the check below rather than by this one.
    if (!logical->has_root_directory() || logical->has_root_name())
        return std::string { physicalDirectory };

    // `equivalent`, not a string compare: the question is whether `PWD` names the SAME
    // directory, which is what the driver asks and what a symlink makes different from
    // spelling the same. It returns false on error and it is the only filesystem call
    // here; a directory that cannot be stat'd falls back like everything else.
    std::error_code ec;
    if (!std::filesystem::equivalent(*logical, *physical, ec) || ec)
        return std::string { physicalDirectory };

    // `PWD` VERBATIM rather than `logical->string()`, because the driver's comparison is
    // over the bytes it was handed and `path` may re-spell separators on a Windows
    // layout. What this returns is compared byte-for-byte one call later.
    return std::string { environmentPwd };
}

std::string CompilerWorkingDirectory(std::string_view physicalDirectory)
{
    auto const pwd = ReadEnvironmentVariable("PWD");
    return CompilerWorkingDirectory(physicalDirectory, pwd.has_value() ? std::string_view { *pwd } : std::string_view {});
}

std::optional<MappedCompileDir> MappedCompileDirectory(std::span<std::string const> argv,
                                                       DriverFamily family,
                                                       std::string_view workingDirectory)
{
    auto const introducers = IntroducersOf(family);

    // The LAST match wins, so the loop records rather than returns. Measured on gcc
    // 14 and clang 20, two rules over one directory: the second replacement is what
    // `DW_AT_comp_dir` holds. It is also what `_fc_debug_prefix_map_rules` relies on
    // -- it emits the source rule first and the build-tree rule last precisely so the
    // build tree wins -- so a first-match model here would predict the source rule's
    // replacement and disagree with every object this project builds.
    std::optional<MappedCompileDir> mapped;
    for (auto const& arg: argv)
    {
        auto const match = MatchPathValueFlag(arg, introducers, family);
        // A rule whose value carries no tail is `-fdebug-prefix-map=/abs` with no
        // replacement, which the driver rejects; it maps nothing, so it is skipped
        // here rather than treated as a mapping to the empty string.
        if (!match.has_value() || match->flag.role != PathValueRole::PrefixMap || match->valueTail.empty())
            continue;

        // A BYTE prefix, deliberately, because that is what both drivers implement:
        // `-fdebug-prefix-map=/tmp/work=X` rewrites a working directory of
        // `/tmp/worker` to `Xer` on gcc and on clang alike. Requiring a separator
        // boundary would read better and would make this client predict a replacement
        // its own compiler does not write.
        if (!workingDirectory.starts_with(match->value))
            continue;

        // `valueTail` carries the separator, so the replacement is what follows it. The
        // directory travels beside it because a worker needs BOTH left-hand sides: gcc
        // under `-g` puts this directory into the preprocessed text and the worker's
        // object then adopts it, while clang leaves the worker's own showing.
        mapped = MappedCompileDir { .directory = std::string { workingDirectory },
                                    .replacement = std::string { match->valueTail.substr(1) }
                                                   + std::string { workingDirectory.substr(match->value.size()) } };
    }
    return mapped;
}

std::vector<std::string> DispatchPreprocessCommand(ParsedCommand const& cmd, std::span<std::string const> argv)
{
    auto const& driver = DriverOf(cmd.flavor);

    std::vector<std::string> out;
    out.reserve(argv.size() + driver.dispatchPreprocessFlags.size() + 1);
    out.emplace_back(cmd.compiler);
    for (auto const& flag: driver.dispatchPreprocessFlags)
        out.emplace_back(flag);

    // No dependency probe: the key's run already reported them, and asking again
    // would make this run write a depfile the caller has no use for.
    std::size_t skipUntil = 1;
    for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = argv[i];
        if (auto const dropped = MatchDroppedFlag(a, driver))
        {
            if (a == *dropped && TakesValue(*dropped) && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }
        out.emplace_back(a);
    }
    return out;
}

std::vector<std::string> PreprocessCommand(ParsedCommand const& cmd,
                                           std::span<std::string const> argv,
                                           std::string_view dependencyProbePath)
{
    auto const& driver = DriverOf(cmd.flavor);

    std::vector<std::string> out;
    out.reserve(argv.size() + driver.preprocessFlags.size() + driver.dependencyProbeFlags.size() + 1);
    out.emplace_back(cmd.compiler);
    for (auto const& flag: driver.preprocessFlags)
        out.emplace_back(flag);

    // The dependency probe rides on this same run, and the path is what requests
    // it. A depfile driver needs the destination anyway: writing to the build's
    // own `-MF` would leave a probe's depfile behind for a hit that is then
    // discarded, and letting `-MD` default its name would drop a stray `.d` in
    // the working directory. A stream driver reports inline and reads nothing but
    // the request out of it.
    if (!dependencyProbePath.empty())
    {
        for (auto const& flag: driver.dependencyProbeFlags)
            out.emplace_back(flag);
        if (driver.usesDepfile)
            out.emplace_back(dependencyProbePath);
    }

    std::size_t skipUntil = 1; // argv[0] is the compiler, already emitted
    for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = argv[i];

        // Drop a dropped flag together with its separated value, so a `-MF dep.d`
        // pair never leaves a stray "dep.d" argument behind. Matching is exact or
        // joined-with-a-value only — see IsJoinedValue: a prefix match would drop
        // `-coverage` for `-c` and `/clr` for `/c`.
        if (auto const dropped = MatchDroppedFlag(a, driver))
        {
            // Only the bare form takes the NEXT argument; a joined form already
            // carries its value, so consuming the successor would eat a real flag.
            if (a == *dropped && TakesValue(*dropped) && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }
        out.emplace_back(a);
    }
    return out;
}

} // namespace FastCache::Cc
