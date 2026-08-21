// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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

/// Which stream a driver reports header dependencies on, when it reports them
/// inline rather than into a depfile.
enum class IncludeStream : std::uint8_t
{
    None,   ///< Dependencies go to a depfile, not a stream.
    Stdout, ///< Notes are printed on stdout (clang-cl).
    Stderr, ///< Notes are printed on stderr (cl).
};

/// How one compiler driver spells the options the launcher needs.
///
/// This is the data behind the parser: adding a driver is adding a row to the
/// table in CmdLine.cpp, not a new branch in the parsing logic.
struct DriverSpec
{
    Flavor flavor { Flavor::Unknown };
    /// Option introducer characters. MSVC drivers accept both `/` and `-`;
    /// GNU drivers only `-`, so a bare `/usr/lib/x.c` stays a source path.
    std::string_view optionPrefixes;
    /// Flag that names the object output, in its joined form (`/Fo<path>` or
    /// `-o <path>`). Matched both joined and separated.
    std::string_view objectFlag;
    /// Flags that request preprocess-to-stdout, appended for the key probe.
    std::span<std::string_view const> preprocessFlags;
    /// Flags dropped when building the preprocess command line (compile-only,
    /// object output, and dependency options that would write stray files).
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
    /// When `usesDepfile`, the probe's depfile path is appended after these; the
    /// stream drivers report on `includeStream` instead and need no path.
    std::span<std::string_view const> dependencyProbeFlags;
    /// Where inline dependency notes appear, if anywhere.
    IncludeStream includeStream { IncludeStream::None };
    /// True when dependencies are emitted into a depfile (`-MF <path>`).
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
///                            stream driver reports on `includeStream` and reads
///                            nothing out of this but the request.
/// @return The preprocess invocation, argv[0] being the compiler.
[[nodiscard]] std::vector<std::string> PreprocessCommand(ParsedCommand const& cmd,
                                                         std::span<std::string const> argv,
                                                         std::string_view dependencyProbePath = {});

} // namespace FastCache::Cc
