// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
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

/// How a driver can be asked where it looks for system headers.
///
/// A mechanism rather than a flag list, because the two families do not merely
/// spell the same question differently — they answer it in different places. A
/// GNU driver PRINTS its search list when asked to be verbose; `cl` does not have
/// such a switch at all and takes its list from the `INCLUDE` environment
/// variable the developer command prompt sets. A flags-only column could not
/// express the second, and a `bool isMsvc` at the use site would be the
/// "behaviour in code rather than data" this table exists to avoid.
///
/// A third mechanism is a new enumerator plus one arm in the one switch that
/// interprets it, which carries no `default:` so adding one is a compile error at
/// the site that must handle it rather than a silent fall-through.
enum class IncludeDiscovery : std::uint8_t
{
    /// Unknown driver: no discovery, so no include tree contributes to the
    /// fingerprint. The compiler banner still does, so an unknown driver degrades
    /// to the weaker identity rather than to none at all.
    None = 0,
    /// Run the driver verbosely over an empty input and read the search list it
    /// prints between its "search starts here" and "End of search list." markers.
    GnuVerbose = 1,
    /// Derive an MSVC toolchain's search list from the machine's INSTALL LAYOUT --
    /// the compiler's own path for the VC headers, the registry for the Windows
    /// SDK's -- falling back to `INCLUDE` when the layout cannot be determined.
    ///
    /// This exists because `INCLUDE` is set per shell by `vcvarsall`, and a
    /// **Windows service does not inherit it**. Reading the variable therefore
    /// yielded nothing under the SCM, `ProbeToolchainFiles` walked nothing, and an
    /// MSVC worker started as a service fingerprinted as its banner and nothing
    /// else. That banner was `cl` for every MSVC toolset in existence until issue
    /// #195, so the digest was too: the false match `ToolchainFingerprint.hpp`
    /// exists to prevent, and the one that yields a silently wrong object rather
    /// than a stale path a replay guard can probe. The banner now names the toolset
    /// and the target, which does not retire this: a fingerprint carrying no
    /// toolchain CONTENT still cannot see a patched header.
    ///
    /// The layout is tried FIRST and the environment only as a fallback, which is
    /// the whole of why this is not merely "read INCLUDE, then guess". Preferring
    /// `INCLUDE` where it is set would make a developer prompt and a service
    /// disagree the moment the two root sets differ by one directory -- and a
    /// fingerprint disagreement is invisible from both ends, presenting only as a
    /// scheduler that never matches. Layout-first means both derive the same roots
    /// wherever the layout is derivable at all.
    ///
    /// The layout is modelled here because `cl` cannot be asked WHERE IT LOOKS --
    /// it prints no search list at any verbosity. What it can be asked is who it
    /// is, and it is (`DriverSpec::versionFlags`). `Flavor::ClangCl` answers both,
    /// so both are asked: see `ClangResourceLayout`.
    MsvcLayout = 2,
    /// Ask a clang driver for the resource directory that SHIPS WITH IT --
    /// `<prefix>/lib/clang/<version>/include`, via `-print-resource-dir`.
    ///
    /// This exists for `clang-cl`, which read `INCLUDE` like `cl` did and hit the
    /// same wall from the other side: a developer command prompt has the variable
    /// and a **Windows service does not**, so a launcher fingerprinted the compiler
    /// over the whole MSVC include tree while a worker fingerprinted it over the
    /// banner alone. The two never agreed, the scheduler answered `NoWorker`, and
    /// nothing at either end said why.
    ///
    /// Asked rather than derived, because the resource tree is not recoverable from
    /// the driver's path: `/usr/bin/clang-cl-20` has `/usr` for a prefix, whose
    /// `lib/clang` holds `20`, `20.1.2`, `22` and `22.1.8` on an ordinary Debian.
    /// One spawn, on the same path `GnuVerbose` already spawns on.
    ///
    /// Unlike `MsvcLayout` this reads `INCLUDE` **not at all**, not even as a
    /// fallback, which is what makes a service and a developer prompt agree
    /// unconditionally rather than merely where a layout is derivable. It can
    /// afford that because `clang-cl` announces a genuine version: a driver that
    /// does not answer degrades to a banner-only fingerprint, which still tells one
    /// clang from another. `cl` announces one too now (issue #195), but only after
    /// being asked the one way it answers -- and a banner alone still cannot see a
    /// patched header, which is why `MsvcLayout` keeps its fallback and this does
    /// not need one.
    ///
    /// The VC toolset and the Windows SDK stay OUT of the answer. `clang-cl`
    /// borrows them rather than owning them, and a worker compiles text the client
    /// already preprocessed, so it opens no header from either -- while the
    /// newest-kit rule that picks them would split two machines running one
    /// clang-cl with different SDKs installed. `ClangResourceIncludeRoots` carries
    /// the argument in full.
    ClangResourceLayout = 3,
};

/// How a driver can be asked which target it will actually generate for.
///
/// Separate from `IncludeDiscovery` because it answers a different question about a
/// different half of the machine. The include roots decide what the compiler READS;
/// the target decides what it EMITS -- and once `ClangResourceLayout` took the VC
/// toolset and the Windows SDK out of `clang-cl`'s fingerprint, nothing left in that
/// digest varies with the target at all. Two machines running one clang-cl therefore
/// match while generating differently, which is the hole this closes.
///
/// A mechanism added here is a new enumerator plus one arm in the single switch that
/// interprets it, which carries no `default:` -- so adding one is a compile error at
/// the site that must handle it rather than a silent "no target".
enum class TargetDiscovery : std::uint8_t
{
    /// Not askable, or nothing ambient worth stating. `cl` is both: it has no `-###`
    /// and no `--target`, because its target is decided by WHICH `cl.exe` is
    /// invoked -- a fact no command line can restate.
    None = 0,
    /// Ask a clang driver to print the frontend invocation it WOULD run (`-###`) and
    /// read the `-triple` out of it.
    ///
    /// The `-triple` from the `-cc1` line, and deliberately not the `Target:` line
    /// three lines above it. That is the trap this mechanism exists to walk around:
    /// `Target:` reports `x86_64-pc-windows-msvc`, while the frontend is really run
    /// with `x86_64-pc-windows-msvc19.51.36252`. The version suffix is the entire
    /// point -- it is where `-fms-compatibility-version` lives, and clang's Microsoft
    /// C++ ABI gates version-specific CODE GENERATION on it (a `noexcept` function
    /// type mangles as `P6AXXZ` below 19.12 and `P6AXX_E` from 19.12 on). Pinning the
    /// unversioned spelling would look exactly like a fix and change nothing.
    ClangDriverLine = 1,
    /// Read the `Target:` header a GNU driver prints under `-###`.
    ///
    /// For `gcc` this header IS the answer: it prints no `-cc1` line and its
    /// frontend takes no `-triple`, so there is nothing more precise to read. That
    /// is the opposite of `ClangDriverLine`, where the very same header is the trap
    /// -- unversioned, and three lines closer to the top than the real answer. Two
    /// mechanisms rather than one that tries both, so which line is authoritative is
    /// a property of the driver in the table rather than a guess at parse time.
    ///
    /// It DISCOVERS without pinning. `gcc` is a fixed-target driver and has no
    /// `--target=`, so this identifies the code generator for the cache key and
    /// states nothing on a dispatched line -- see `TargetPinPrefixFor`, which is
    /// where those two questions are kept apart.
    GnuTargetLine = 2,
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
    /// Where MSVC would put a PDB (`/Fd`). CMake emits it on every compile, and it
    /// names a path on THIS machine, so it neither travels to a worker nor belongs
    /// in a key. Whether a PDB is written at all is decided by `/Zi` / `/ZI`, which
    /// dispatch refuses outright -- with `/Z7`, and with no debug flag, this names a
    /// file nothing produces.
    DebugOutput,

    /// A path rewrite the compiler applies to what it EMBEDS (`-fdebug-prefix-map`
    /// and the two spellings beside it). Its value carries a tail; where that is
    /// cut is `PathValueFlag::valueTailSeparator`, not a fact about this role.
    ///
    /// It is here because the flag carries the producing checkout's absolute root
    /// by construction -- that is what it is for -- so an argument nothing
    /// relativized would put that root into every key and cost exactly the
    /// cross-checkout sharing the flag was added to preserve. Measured before the
    /// row existed: `-fdebug-prefix-map=/home/ci/checkout-aaa=/fastcache/src` came
    /// back from `RelativizeArgs` byte-for-byte.
    ///
    /// The replacement half is deliberately NOT rewritten. Two machines that map
    /// to different replacements produce different objects, so they must produce
    /// different keys; leaving that half literal is what makes the key enforce
    /// #203's "the mapping must be identical on every machine sharing the cache"
    /// rather than merely document it.
    PrefixMap,
};

/// One spelling of a flag whose value is a filesystem path.
struct PathValueFlag
{
    std::string_view spelling;                        ///< The flag text, without its value.
    PathValueRole role { PathValueRole::IncludeDir }; ///< What the value names.
    DriverFamily families { DriverFamily::None };     ///< Which drivers accept this spelling.

    /// For a flag whose value is `<path><sep><rest>`, the separator; `\0` when the
    /// whole value is the path, which is every row but the prefix-map family.
    ///
    /// A COLUMN and not a branch at a consumer, because the shape of a value is a
    /// fact about the FLAG. This header's own contract two paragraphs up is that
    /// "the role is what each consumer filters on, so a new flag never grows a
    /// branch" -- and a consumer asking `role == PrefixMap` to learn where to cut
    /// is that branch, spelled as a role test. With the column, the next flag
    /// whose value carries a tail (`-fprofile-prefix-map`, a `<path>:<something>`
    /// spelling) is a row.
    ///
    /// The LAST occurrence is the split point, which follows GCC. There is no
    /// answer that satisfies both drivers in the family: measured with a directory
    /// named `a=b`, gcc cuts `-fdebug-prefix-map=<dir>/a=b=ZZZ` at the last
    /// separator and clang at the first. Reachable only when a mapped root itself
    /// contains one, and it costs a MISS rather than a mis-serve -- the head the
    /// launcher isolates lies under no root, so the argument comes back verbatim.
    char valueTailSeparator { '\0' };
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

/// How `family` spells "write the object here", as a prefix to fuse a path onto.
///
/// Read out of `PathValueFlags()` rather than restated, so the flag a worker
/// EMITS and the flag the launcher PARSES cannot drift apart -- which is the
/// defect this exists to close: the compile worker hard-coded the GNU `-o`, and
/// `cl` does not take it. MSVC quietly wrote `tu.obj` beside the source instead,
/// exited 0, and the worker then found nothing at the path it had asked for and
/// refused the job. Distribution therefore never worked on Windows at all.
///
/// The value is meant to be FUSED onto the path (`/Fofoo.o`, `-ofoo.o`), which
/// both families accept and which is the only form MSVC documents for `/Fo`.
/// Returning a prefix rather than emitting two arguments is what keeps one rule
/// covering both.
///
/// @param family The driver family being invoked.
/// @return The flag text, or `-o` for a family with no more specific spelling.
[[nodiscard]] std::string_view ObjectOutputPrefixFor(DriverFamily family);

/// A path-valued flag recognised on a command line.
struct PathValueMatch
{
    PathValueFlag flag;      ///< The table row that matched.
    std::string_view prefix; ///< The argument up to the fused value (flag plus any `=`/`:` separator).
    /// The fused value's PATH portion; EMPTY when the value is the next argument
    /// instead. For a row with a `valueTailSeparator` this is the part before the
    /// last separator, and @ref valueTail carries the rest.
    std::string_view value;
    /// Whatever followed the path inside the same value, separator included;
    /// empty for every row without a `valueTailSeparator`.
    ///
    /// Split HERE rather than by each consumer, for the same reason `prefix` and
    /// `value` are: "where does a flag end and its value begin" is answered once,
    /// and a second copy of that question is exactly how the object output came to
    /// be relativized in its separated spelling and not in its fused one.
    /// `prefix + value + valueTail` reconstitutes the argument.
    std::string_view valueTail;
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

/// How a driver is TOLD which target to generate for.
///
/// A function over the DISCOVERY mechanism rather than a constant, so the two
/// cannot drift: a mechanism added to `TargetDiscovery` is a compile error here,
/// which is exactly where somebody has to decide whether `--target=` is still the
/// right way to say it.
///
/// **Discovering and stating are separate questions and this is the seam.** `gcc`
/// can be asked what it generates for and cannot be told -- it is a fixed-target
/// driver with no `--target=` -- so its target belongs in the cache key, which
/// decides which object may be served, and nowhere near a dispatched command line,
/// which would fail the compile outright. An empty prefix means exactly that:
/// identified, not pinned.
///
/// The value is meant to be FUSED onto the triple, for the reason
/// `ObjectOutputPrefixFor`'s is: `--target x` is rejected by clang-cl while
/// `--target=x` is accepted by both clang drivers, so one spelling covers both.
///
/// @param discovery The mechanism that produced the triple.
/// @return The flag prefix, or empty when this mechanism pins nothing.
[[nodiscard]] std::string_view TargetPinPrefixFor(TargetDiscovery discovery) noexcept;

/// The option-introducer characters a driver family uses.
/// @param family The family (or family set) to describe.
/// @return Its introducers; empty for DriverFamily::None.
[[nodiscard]] std::string_view IntroducersOf(DriverFamily family) noexcept;

/// The source language a translation unit is written in.
///
/// Needed only where a driver has to be TOLD, because the ordinary signal — the
/// file's extension — is lost the moment the text is preprocessed into a worker's
/// scratch file. See `DriverSpec::preprocessedInput`.
enum class SourceLanguage : std::uint8_t
{
    C,
    Cxx,
    ObjectiveC,
    ObjectiveCxx,
    /// A module interface unit (`.ixx`, `.cppm`, ...). Recognised so it can be
    /// REFUSED: it writes a BMI beside its object, and neither a cache hit nor a
    /// dispatched compile reproduces anything but the object.
    CxxModuleInterface,
};

/// One driver's spelling of "this input is preprocessed <language>".
struct PreprocessedInputSpelling
{
    SourceLanguage language;
    std::span<std::string_view const> flags;
};

/// The language a source path names, by its extension.
///
/// Case-insensitive, with `.C` and `.M` deliberately excluded: those two are read
/// as C++ / Objective-C++ by a GNU driver and as C / Objective-C by an MSVC one, so
/// the extension alone does not answer the question and a guess would hand a worker
/// the wrong language. An extension with no answer is not dispatched.
///
/// @param path The source path as the build system spelled it.
/// @return The language, or nullopt when the extension names none unambiguously.
[[nodiscard]] std::optional<SourceLanguage> LanguageOfSource(std::string_view path);

/// A language's English name, for a refusal a human has to act on.
/// @param language The language.
/// @return Its name.
[[nodiscard]] std::string_view DescribeLanguage(SourceLanguage language) noexcept;

/// Why a translation unit in this language is neither cached nor dispatched.
///
/// Both gates read it, and caching is the wider of the two: a line that is not
/// cacheable never reaches dispatch at all, so a language refused here is refused
/// once rather than in two places that can come to disagree.
///
/// @param language The language.
/// @return The reason, or empty when this language may be cached and dispatched.
[[nodiscard]] std::string_view UncacheableBecause(SourceLanguage language) noexcept;

/// Whether `arg` makes a compile write something BESIDES its object file.
///
/// A cache hit reproduces the object and the dependency record and nothing else, so
/// a line that also writes a BMI (`/ifcOutput`, `-fmodule-output`) or a precompiled
/// header (`/Yc`) must not be cached: replaying only the object leaves the second
/// artefact missing, which fails loudly, or stale, which does not. The module
/// EXTENSIONS are the other half of the same rule and are handled by
/// `LanguageOfSource` — this table is for an ordinary source promoted by a flag,
/// which is how a `.cpp` becomes a module interface unit (`cl /interface`).
///
/// @param arg    The argument as it appeared on the command line.
/// @param family Which family's spellings may match.
/// @return True when this argument means a second artefact is produced.
[[nodiscard]] bool ProducesSideArtefact(std::string_view arg, DriverFamily family);

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
    /// Flags that request preprocess-to-stdout WITH `#line` markers, for text a
    /// worker is going to compile.
    ///
    /// Separate from `preprocessFlags` because the two runs answer different
    /// questions. The key's text must carry no path, so it suppresses markers; a
    /// worker's text must carry them, because they are what tells the compiler
    /// which lines came from a system header — and without that every warning
    /// inside libc++ or the CRT resurfaces, which under `-Werror` fails the
    /// compile outright rather than merely being noisy.
    std::span<std::string_view const> dispatchPreprocessFlags;
    /// How this driver is told, per language, that its input is ALREADY
    /// preprocessed — appended to a remote compile's argument list.
    ///
    /// Keeping `#line` markers fixes system-header warnings and immediately creates
    /// a second problem: under `-pedantic` the markers themselves are a GNU
    /// extension, so clang reports `-Wgnu-line-marker` and `-Werror` turns that into
    /// a failed compile. Naming the input's language as preprocessed output is what
    /// makes the driver expect them — the same thing ccache and distcc do.
    ///
    /// A TABLE and not one span, because the language is the whole point of the
    /// flag and it was previously inferred from a FILE NAME on the far side. MSVC's
    /// entry used to be empty on the reasoning that `/E` emits standard `#line` and
    /// so there is nothing to tell it — true about the markers, and it left nothing
    /// stating the LANGUAGE. The worker writes its scratch file as `tu.cpp`, MSVC
    /// reads the language off that extension, and a dispatched `.c` translation unit
    /// therefore came back compiled as C++: a failed remote compile where C is not
    /// valid C++ (so C silently never distributed), and where it is valid C++, an
    /// object with C++ mangling stored under the C key. `/TC` and `/TP` are exactly
    /// the `-x` spelling MSVC does have, and `/TP` is a byte-for-byte no-op on a C++
    /// translation unit, so nothing that worked before moves.
    ///
    /// A language with no row is NOT DISPATCHABLE on this driver, which is the
    /// difference between an empty table and a missing row: MSVC has no way to be
    /// handed preprocessed Objective-C, and sending it anyway is how the defect
    /// above happened. See `PreprocessedInputFlagsFor`.
    std::span<PreprocessedInputSpelling const> preprocessedInput;
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
    /// How this driver reveals its system include search paths.
    IncludeDiscovery includeDiscovery { IncludeDiscovery::None };
    /// How this driver reveals the target it will generate for.
    ///
    /// Beside `includeDiscovery` rather than below the spans, so every byte-wide
    /// member stays in one run: one dropped between two 8-aligned members costs
    /// seven bytes of padding and fails clang-tidy's budget.
    TargetDiscovery targetDiscovery { TargetDiscovery::None };
    /// Flags that make the driver print its include search list, for
    /// `IncludeDiscovery::GnuVerbose`. Empty for every other mechanism.
    ///
    /// The input is a separate concern from these flags and is supplied by the
    /// caller, because "a file that is empty and always exists" has no portable
    /// spelling: `/dev/null` is not a path on Windows, and reading from stdin
    /// needs the caller to close it.
    std::span<std::string_view const> includeProbeFlags;
    /// Flags that make the driver print what it WOULD run without running it, for
    /// every `TargetDiscovery` mechanism that asks the driver a question -- both
    /// `ClangDriverLine` and `GnuTargetLine` read this. Empty only for
    /// `TargetDiscovery::None`.
    ///
    /// The two mechanisms differ in which LINE of that output is authoritative, not
    /// in how the driver is asked, so emptying this for one of them would not
    /// disable it -- it would spawn `g++ /dev/null`, a link step with no target on
    /// it, and drop the target from every GCC key in silence.
    ///
    /// Deliberately NOT the compile's own command line, which would make the answer a
    /// property of one translation unit rather than of the machine. It does not need
    /// to be: the pin these produce is placed FIRST among the dispatched arguments,
    /// so anything the build states for itself -- `--target=`, `-m32` -- comes later
    /// and still wins, which is precisely what happens locally.
    ///
    /// The input path is appended by the caller, for the reason `includeProbeFlags`
    /// gives: "a file that is empty and always exists" has no portable spelling.
    std::span<std::string_view const> targetProbeFlags;
    /// Flags that make the driver print its version banner as the FIRST line of
    /// its output, and exit ZERO having done so.
    ///
    /// Both halves are load-bearing, and MSVC is why this is a table row rather
    /// than one spelling for everybody. `cl` has no `--version`: handed it, it
    /// prints its banner (it always does), then warns D9002, then errors D8003 and
    /// exits 2. `CompilerBanner` requires a zero exit — deliberately, so that a
    /// driver which cannot answer falls back to a normalized name rather than
    /// fingerprinting on an error message — so `cl` took that fallback, and every
    /// MSVC compiler ever installed identified as the string `cl`. That string is
    /// the cache key's compiler identity, so one toolset's object was served to
    /// another toolset's compile under a zero exit code (issue #195).
    ///
    /// MSVC's row is therefore EMPTY, which is not "no probe" but "no flags
    /// needed": `cl` invoked with nothing at all prints the same banner, reads no
    /// stdin, writes no file, and exits 0. The gate stays as it was; the probe is
    /// what was wrong.
    std::span<std::string_view const> versionFlags;
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
    /// True when the line also writes a BMI or a precompiled header, which makes it
    /// uncacheable. Recorded rather than folded into `parsedOk` alone so the
    /// launcher can say WHY it stepped aside: a link step and a module interface
    /// unit are both passed through, and only one of them looks like a defect.
    bool sideArtefact { false };
    bool parsedOk { false }; ///< False if the line is not a cacheable compile.
};

/// Look up the descriptor for a compiler flavor.
/// @param flavor The flavor to describe.
/// @return Its driver spec; the Unknown spec for an unrecognised flavor.
/// Classify a compiler's driver flavor from its basename.
///
/// Tolerates version suffixes (`g++-14`, `clang-18`) and a `.exe` extension, both
/// ordinary on real build systems.
///
/// Exposed because the flavor is needed outside a parsed command line: asking a
/// toolchain where it searches for headers requires knowing which family it
/// belongs to, and that question is asked by `--print-toolchain-fingerprint`,
/// which has a compiler and no command line at all.
///
/// @param compiler argv[0] as invoked.
/// @return The matching flavor, or `Flavor::Unknown`.
[[nodiscard]] Flavor ClassifyCompiler(std::string_view compiler);

/// Correct a name-based classification against what the driver calls itself.
///
/// `cc` and `c++` name a POLICY -- "the system C compiler" -- rather than a
/// product. On most Linuxes they are GCC; on macOS `/usr/bin/c++` is Apple clang,
/// and it is CMake's default C++ compiler there, so the spelling a build system
/// uses most is precisely the one that says least. Classifying it by name put an
/// Apple clang in the `gcc` row, and once that row became the one that decides how
/// a target is discovered, a name-based guess started deciding whether two machines
/// share a cache entry.
///
/// The banner settles it and costs nothing: it is already in hand for the key, so
/// this is a string test rather than a second spawn.
///
/// Only the GNU-driver pair is refined, and only on POSITIVE evidence. A banner
/// carrying `clang version` is proof; a banner without it is not proof of the
/// opposite, because a driver that could not be RUN falls back to its own basename
/// -- so a real `clang++` whose `--version` failed would otherwise be demoted into
/// the gcc row, losing both the versioned target and the pin on a driver whose name
/// said exactly what it was. An unrecognised banner therefore leaves @p named alone.
///
/// `clang-cl` is deliberately untouchable here, because its banner is
/// `clang version ...` -- identical to plain clang's -- so the NAME is the only
/// thing that distinguishes the two drivers, and a banner test would collapse them.
/// `cl` is left alone for the same reason in reverse: its banner is a fallback, and
/// there is nothing to read.
///
/// @param named What the compiler's name said it was.
/// @param banner Its version line, as `CompilerBanner` returns it.
/// @return The corrected flavor; @p named unchanged for every driver but the pair.
[[nodiscard]] Flavor ClassifyCompilerFromBanner(Flavor named, std::string_view banner) noexcept;

/// A compiler's basename, lowered and stripped of `.exe`.
///
/// Here rather than beside its callers because THREE questions are answered from
/// it and they must not disagree: which driver this is (`ClassifyCompiler`),
/// whether it compiles everything as C++, and -- when a driver answers no
/// `--version` -- what to call it in a fingerprint. `CL.EXE`, `cl` and
/// `C:/.../cl.exe` are one compiler, and a second spelling of "what do we call it"
/// is a second chance for a launcher and a worker to part company silently.
///
/// @param compiler argv[0] as invoked, bare name or path.
/// @return Its normalized basename.
[[nodiscard]] std::string NormalizedCompilerName(std::string_view compiler);

[[nodiscard]] DriverSpec const& DriverOf(Flavor flavor);

/// How `driver` is told its input is preprocessed `language`.
///
/// @param driver   The driver descriptor.
/// @param language The translation unit's language.
/// @return The flags to append, or nullopt when this driver has no spelling for
///         that language — which means the job must not be dispatched to it.
[[nodiscard]] std::optional<std::span<std::string_view const>> PreprocessedInputFlagsFor(DriverSpec const& driver,
                                                                                         SourceLanguage language);

/// Parse a compiler invocation (`argv[0]` = compiler, rest = its arguments).
///
/// Recognises MSVC-style (`cl`, `clang-cl`) and GNU-style (`gcc`, `g++`, `cc`,
/// `c++`, `clang`, `clang++`, including version-suffixed names like `g++-14`)
/// command lines, extracting the source file, the object output path, the
/// depfile path, and whether inline include reporting was requested.
///
/// `parsedOk` is false when the line has no single source file (a link step, a
/// multi-TU line), names no explicit object output (`g++ -c a.cpp`, which
/// defaults to `./a.o` — a path the launcher cannot reconstruct), is
/// preprocess-only, or writes a second artefact besides the object (a BMI or a
/// precompiled header — see `ProducesSideArtefact`) — the launcher then falls back
/// to a plain exec.
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
/// Build the argument list a remote worker compiles this translation unit with.
///
/// The worker receives **preprocessed** text, so this is not the build's command
/// line minus a few things — it is the subset that still means something once the
/// headers are already inlined and the macros already expanded:
///
///   - The **source path** is dropped. The worker compiles a file of its own, in a
///     directory the client has never heard of.
///   - Every **path-valued** flag is dropped, whatever its role. An include
///     directory has already done its work (and naming a path that does not exist
///     on the worker is at best useless); an object output would make the worker
///     write where the client wanted it rather than where it can; a depfile or its
///     rule target would have the worker report dependencies for preprocessed text,
///     which has none. This is `PathValueFlags()` again, read for a fourth question,
///     rather than a fourth list of spellings to keep in step.
///   - The driver's own **compile-only and dependency switches** are dropped, for
///     the same reason and off the same table `PreprocessCommand` uses. The worker
///     adds its own `-c`, because only it knows its output path.
///
/// Everything else is kept, and deliberately: `-std=`, `-O`, `-g`, `-W`, `-f`, `-m`
/// and their kin all change the code the compiler generates and must reach it, or
/// the object the client gets back is not the object it asked for. `-D`/`-U` are
/// kept too — already expanded, so inert, but dropping them would mean a fifth
/// classification of flags and no benefit.
///
/// Anything left that could still name a file makes the whole command line
/// **undispatchable**, and this returns nothing.
///
/// Dropping the known path-valued flags is a deny-list, and a deny-list is the
/// losing game here: `PathValueFlags()` does not know `-isystem`, `-iquote`,
/// `--sysroot`, `-B`, `-specs=`, `-fplugin=` or `@response-file`, and it should not
/// have to — it exists to answer questions about the cache key. Several of those
/// point a compiler at an *executable* or at a file it will read, which is exactly
/// the surface a worker must not expose to a client.
///
/// So the last word is a positive check on what survives: an argument carrying a
/// path separator, or opening a response file, means this translation unit is not
/// dispatched at all. **Refused rather than stripped**, because the two failure
/// modes are not comparable — stripping an argument this function does not
/// recognise would change the code the compiler generates and hand back an object
/// that is quietly not the one that was asked for, while refusing costs one local
/// compile. A code-generation flag (`-std=c++23`, `-O2`, `-Wall`, `-fPIC`,
/// `/std:c++20`) has no path in it, so the ordinary case is unaffected.
///
/// The worker separately refuses to take its compiler from the client, so this is
/// the second of two independent barriers rather than the only one.
///
/// ## The target is stated, not left to the worker
///
/// `targetTriple` is emitted as `--target=<triple>` **first**, ahead of everything
/// the build said for itself. A worker otherwise re-derives the target from its own
/// machine, and for a Microsoft target that re-derivation includes
/// `-fms-compatibility-version`: clang-cl reads it from whatever MSVC it can find,
/// a Windows service can find none and falls back to clang's built-in default, and
/// the toolchain fingerprint sees neither. So two ends match and generate
/// differently, which is a wrong object with a zero exit code.
///
/// FIRST and not last, which is the opposite of the language flags below and for the
/// opposite reason. What this states is the DEFAULT the client's own driver would
/// have used, so anything the build names explicitly -- `--target=`, `-m32` -- comes
/// later on the line and still wins, exactly as it does locally. Appending it would
/// override the build instead, and compiling for a target the build did not ask for
/// is a wrong object rather than a failed one.
///
/// Passing it is not optional, and there is deliberately no default: a second
/// dispatch path that forgot it would silently go back to letting the worker choose.
/// Empty is the honest answer for a driver with no target to state (`cl`) or one
/// that would not say, and it leaves the line exactly as it was.
///
/// @param cmd The parsed compile command.
/// @param argv The original full invocation.
/// @param targetTriple The target the CLIENT's driver would generate for; empty to
///        state none.
/// @return The arguments to send (without the compiler and without the source), or
///         the reason this command line must not be dispatched. The reason travels
///         because every refusal here ends in a local compile, and "distribution
///         stopped helping" is otherwise a whole investigation.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> RemoteCompileArgs(ParsedCommand const& cmd,
                                                                                     std::span<std::string const> argv,
                                                                                     std::string_view targetTriple);

[[nodiscard]] std::vector<std::string> PreprocessCommand(ParsedCommand const& cmd,
                                                         std::span<std::string const> argv,
                                                         std::string_view dependencyProbePath = {});

/// Build the argv that preprocesses `cmd`'s translation unit for a REMOTE compile.
///
/// The same line `PreprocessCommand` builds, except that it keeps `#line` markers
/// and asks for no dependency reporting. See `DriverSpec::dispatchPreprocessFlags`
/// for why the markers are not optional.
///
/// This is a second preprocess run, and it is paid only on the path that is about
/// to spend seconds compiling remotely: roughly 45 ms against a compile that would
/// not have been dispatched at all if it were cheap. Reusing the key's text instead
/// would save that and break every `-Werror` build, which is not a trade.
///
/// @param cmd The parsed command.
/// @param argv The original full invocation.
/// @return The preprocess invocation, argv[0] being the compiler.
[[nodiscard]] std::vector<std::string> DispatchPreprocessCommand(ParsedCommand const& cmd,
                                                                 std::span<std::string const> argv);

} // namespace FastCache::Cc
