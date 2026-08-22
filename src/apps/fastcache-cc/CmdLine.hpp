// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

/// Which compiler driver the launcher is fronting.
///
/// The driver decides how the command line is spelled (`/Fo` vs `-o`), how a
/// preprocess-only run is requested, and how header dependencies are reported —
/// MSVC-style drivers print `/showIncludes` notes on a stream, GNU-style ones
/// write a Makefile depfile. See `DriverOf` for the per-flavor descriptor.
enum class Flavor : std::uint8_t
{
    Unknown,
    Cl,      ///< MSVC cl.exe — /showIncludes notes on stderr.
    ClangCl, ///< clang-cl.exe (MSVC-compatible driver) — notes on stdout.
    Gcc,     ///< gcc / g++ — GNU driver, depfile via -MD -MF.
    Clang,   ///< clang / clang++ — GNU driver, depfile via -MD -MF.
};

/// The families of command-line spelling the launcher understands, as a SET.
///
/// A set rather than a single value because a spelling can belong to both
/// families: every MSVC driver accepts `-o` alongside `/Fo`, and `-I` is
/// universal. It is carried per flag row and per driver, and it is NOT derivable
/// from a flag's introducer character — `-MT` names a dependency target on a GNU
/// driver and selects the static multithreaded runtime on an MSVC one, so a row
/// matched by introducer alone would make `cl -MT` consume the next argument,
/// which is usually the source file.
enum class DriverFamily : std::uint8_t
{
    None = 0, ///< No recognised spelling (an unknown driver).
    Msvc = 1, ///< cl, clang-cl.
    Gnu = 2,  ///< gcc, g++, clang, clang++.
    Any = 3,  ///< Msvc | Gnu — a spelling every driver accepts.
};

/// True when two family sets overlap.
///
/// A membership test when one side is a single family (a driver's), and a
/// genuine intersection when it is not — the cache key asks with
/// DriverFamily::Any, because it relativizes a command line without knowing
/// which driver produced it.
///
/// @param left  One family set (typically a flag row's).
/// @param right The other (typically a driver's).
/// @return True when they have a family in common.
[[nodiscard]] constexpr bool Overlaps(DriverFamily left, DriverFamily right) noexcept
{
    return (std::to_underlying(left) & std::to_underlying(right)) != 0;
}

/// What a path-valued flag's value names.
///
/// The role is what each consumer filters on, so a new flag never grows a
/// branch: the preprocess line drops every role but IncludeDir, the parser
/// captures ObjectOutput and DepFile, and the cache key relativizes them all.
enum class PathValueRole : std::uint8_t
{
    ObjectOutput, ///< The compiled object file (`/Fo`, `-o`).
    IncludeDir,   ///< A header search directory (`-I`, `/I`, `/external:I`).
    DepFile,      ///< The Makefile dependency file the compile writes (`-MF`).
    DepTarget,    ///< The rule target named inside that depfile (`-MT`, `-MQ`).
};

/// One spelling of a flag whose value is a filesystem path.
struct PathValueFlag
{
    std::string_view spelling;                        ///< The flag text, without its value.
    PathValueRole role { PathValueRole::IncludeDir }; ///< What the value names.
    DriverFamily families { DriverFamily::None };     ///< Which drivers accept this spelling.
};

/// Every flag whose value is a filesystem path, in one table.
///
/// The single source of truth for three questions that used to be answered by
/// three separate tables, which is how the object output came to be relativized
/// in its separated form and not in its fused one: whether a bare occurrence
/// consumes the NEXT argument (CmdLine's own `ValueFlags`), which flag names the
/// object output (`DriverSpec::objectFlag`), and whose fused value the cache key
/// must rewrite to a canonical token (CacheKey's `IncludePrefixes`). Adding a
/// flag is adding a row here.
///
/// Rows are ordered longest spelling first, so a scan cannot match a shorter row
/// that prefixes a longer one before reaching it.
///
/// @return The table, in match order.
[[nodiscard]] std::span<PathValueFlag const> PathValueFlags();

/// A path-valued flag recognised on a command line.
struct PathValueMatch
{
    PathValueFlag flag;      ///< The table row that matched.
    std::string_view prefix; ///< The argument up to the fused value (flag plus any `=`/`:` separator).
    std::string_view value;  ///< The fused value; EMPTY when the value is the next argument instead.
};

/// Recognise the path-valued flag an argument names, bare or with a fused value.
///
/// `introducers` is what makes this usable from both consumers: the parser passes
/// the driver's own option introducers, while the cache key passes the ones that
/// apply to the LAYOUT it is relativizing against — a leading `/` introduces an
/// option under a Windows layout, but on POSIX it starts an absolute path, and
/// matching `/I` there splits a checkout rooted at `/Infra` into a fragment that
/// lies under no root and keeps its absolute path in the key.
///
/// @param arg         The argument as it appeared on the command line.
/// @param introducers The characters that introduce an option in this context.
/// @param families    Which families' spellings may match.
/// @return The match, or nullopt when `arg` names no path-valued flag.
[[nodiscard]] std::optional<PathValueMatch> MatchPathValueFlag(std::string_view arg,
                                                               std::string_view introducers,
                                                               DriverFamily families);

/// The option-introducer characters a driver family uses.
/// @param family The family (or family set) to describe.
/// @return Its introducers; empty for DriverFamily::None.
[[nodiscard]] std::string_view IntroducersOf(DriverFamily family) noexcept;

/// How one compiler driver spells the options the launcher needs.
///
/// This is the data behind the parser: adding a driver is adding a row to the
/// table in CmdLine.cpp, not a new branch in the parsing logic.
struct DriverSpec
{
    Flavor flavor { Flavor::Unknown };
    /// Which family's spellings this driver accepts. Decides both its option
    /// introducers (via IntroducersOf — MSVC drivers take `/` and `-`, GNU
    /// drivers only `-`, so a bare `/usr/lib/x.c` stays a source path) and which
    /// rows of PathValueFlags() apply to it. One field rather than two, because a
    /// second spelling of "this is an MSVC driver" is a second thing to keep in
    /// step.
    DriverFamily family { DriverFamily::None };
    /// Flags that request preprocess-to-stdout, appended for the key probe.
    std::span<std::string_view const> preprocessFlags;
    /// The driver's own flags dropped when building the preprocess command line
    /// (the compile-only marker and the dependency-reporting switches).
    ///
    /// Path-valued flags are deliberately absent here: the preprocess line drops
    /// every PathValueFlags() row whose role is not IncludeDir, so the object
    /// output and the depfile options go without being spelled a second time.
    /// This list is only what has no path value to speak of.
    std::span<std::string_view const> preprocessDropFlags;
    /// Flags appended to the preprocess line so the same probe ALSO reports the
    /// translation unit's dependencies, which are folded into the cache key.
    ///
    /// The key must be a function of both artefacts a hit reproduces. Without the
    /// dependency set it determines only the object, and a header that moves
    /// without changing a byte keys identically while its depfile no longer does
    /// (issue #53 / issue #56). Capturing them here rather than in a second probe
    /// is what makes that affordable: measured at +1.5% on a 45 ms preprocess,
    /// because the compiler has already opened every one of those files.
    ///
    /// When `usesDepfile`, the probe's depfile path is appended after these; a
    /// stream driver reports inline and needs no path.
    std::span<std::string_view const> dependencyProbeFlags;
    /// True when dependencies are emitted into a depfile (`-MF <path>`).
    ///
    /// There is deliberately no companion field naming *which* stream an inline
    /// reporter uses. There was, and nothing could safely interpret it: a
    /// per-driver stream is a property of the COMPILE run, while the launcher's
    /// two consumers of notes read a different command line each — the key probe
    /// (`/EP`, where clang-cl moves its notes to stderr so they do not corrupt the
    /// preprocessed stdout) and the replay path (which tags both captured streams
    /// with the ShowIncludes grammar rather than choosing). Both therefore read
    /// both streams, and a descriptor row nothing interprets is how the two drifted
    /// apart in the first place.
    bool usesDepfile { false };
};

/// The pieces of a compile command line the launcher needs to key, cache, and
/// reproduce a compilation.
struct ParsedCommand
{
    Flavor flavor { Flavor::Unknown };
    std::string compiler;            ///< argv[0] — the real compiler to exec.
    std::string source;              ///< The translation-unit source path.
    std::string objPath;             ///< The requested object output (/Fo or -o).
    std::string depPath;             ///< The requested depfile (-MF), if any.
    bool wantShowIncludes { false }; ///< True if /showIncludes was requested.
    bool parsedOk { false };         ///< False if the line is not a cacheable compile.
};

/// Look up the descriptor for a compiler flavor.
/// @param flavor The flavor to describe.
/// @return Its driver spec; the Unknown spec for an unrecognised flavor.
[[nodiscard]] DriverSpec const& DriverOf(Flavor flavor);

/// Parse a compiler invocation (`argv[0]` = compiler, rest = its arguments).
///
/// Recognises MSVC-style (`cl`, `clang-cl`) and GNU-style (`gcc`, `g++`, `cc`,
/// `c++`, `clang`, `clang++`, including version-suffixed names like `g++-14`)
/// command lines, extracting the source file, the object output path, the
/// depfile path, and whether inline include reporting was requested.
///
/// `parsedOk` is false when the line has no single source file (a link step, a
/// multi-TU line), names no explicit object output (`g++ -c a.cpp`, which
/// defaults to `./a.o` — a path the launcher cannot reconstruct), or is
/// preprocess-only — the launcher then falls back to a plain exec.
///
/// @param argv The full invocation, argv[0] being the compiler.
/// @return The parsed command; `parsedOk` gates cacheability.
[[nodiscard]] ParsedCommand ParseCommand(std::span<std::string const> argv);

/// Build the argv that preprocesses `cmd`'s translation unit to stdout.
///
/// Drops the compile-only, object-output and dependency flags (which would
/// otherwise write stray files) and appends the driver's preprocess flags.
///
/// The caller's own dependency flags are dropped and the driver's
/// `dependencyProbeFlags` appended in their place, so the probe reports
/// dependencies in exactly one spelling regardless of what the build asked for —
/// `-MD` even when the compile said `-MMD`, and a `-MF` naming
/// `dependencyProbePath` rather than the build's own depfile, which a discarded
/// hit must not have touched.
///
/// @param cmd The parsed command.
/// @param argv The original full invocation.
/// @param dependencyProbePath Where a depfile driver writes the probe's
///                            dependencies, and the request for the probe itself:
///                            empty asks for no dependency reporting at all. A
///                            stream driver reports inline and reads nothing out
///                            of this but the request.
/// @return The preprocess invocation, argv[0] being the compiler.
[[nodiscard]] std::vector<std::string> PreprocessCommand(ParsedCommand const& cmd,
                                                         std::span<std::string const> argv,
                                                         std::string_view dependencyProbePath = {});

} // namespace FastCache::Cc
