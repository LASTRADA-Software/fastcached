// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "IProcessRunner.hpp"
#include "ToolchainFingerprint.hpp"
#include "ToolchainHost.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
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

/// The files found under a toolchain's include roots, and whether that is all of them.
///
/// The second field draws the same line `IncludeSearchRoots::answered` draws one
/// layer up, for the same reason and against a worse failure. A root that is not
/// THERE is ordinary -- a driver lists search paths it would use if they existed --
/// and so is an entry that is not a regular file. A root that is there and could not
/// be read, or a walk that stopped partway, is neither: the digest then covers less
/// of the toolchain than the toolchain has, and says so to nobody.
///
/// Worse than the unrun probe of `IncludeSearchRoots::answered`, because a short walk
/// moves no stamp. `ComputeToolchainStamp` folds each root's path and mtime, not its
/// contents, so an I/O failure inside a root leaves the stamp identical -- the wrong
/// fingerprint is written under a stamp that still validates, and every later run
/// hits it without walking anything. There is no self-correcting run to wait for.
struct ToolchainFileScan
{
    /// One entry per readable file, unsorted (the digest sorts).
    std::vector<ToolchainFile> files;

    /// False when content was omitted for a reason other than not being there.
    ///
    /// The signals that clear it are a root that could not be opened or stat'd, a walk
    /// that ended early, a file with no relative spelling, and a regular file whose
    /// bytes could not be read -- the last being what an antivirus holding a header
    /// looks like. What they share is that they are accidents of one moment on one
    /// machine, so two ends running this same code disagree.
    ///
    /// Two omissions deliberately do NOT clear it, and both would refuse healthy
    /// toolchains. A failed `is_regular_file` is one: that query fails on a dangling
    /// symlink, which a real toolchain tree contains. A root whose bytes this process
    /// cannot decode into a path is the other, and it is the subtler of the two --
    /// whether those bytes decode is a property of the bytes and of the narrow
    /// encoding every executable here pins to UTF-8, so a launcher and a worker reach
    /// the same answer and digest the same narrower tree, which still matches.
    bool complete { true };
};

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
/// @return The files, and whether every root was walked to the end.
[[nodiscard]] ToolchainFileScan ProbeToolchainFiles(std::span<std::string const> roots);

/// How this compiler is invoked to learn what it is.
///
/// `argv[0]` plus its driver's `DriverSpec::versionFlags`, which for MSVC is
/// nothing at all -- `cl` has no version option and prints its banner regardless.
///
/// Exposed because TWO questions are asked with this one command and they must be
/// asked identically: what the compiler calls itself (`CompilerBanner`), and
/// whether it can be spawned at all (`NodeToolchains`' `CanSpawn`, which reads only
/// the "could not spawn" answer). Built here once, so the day a driver's row moves
/// it moves for both -- a second spelling would make a node judge spawnability from
/// an invocation it then never uses.
///
/// @param compiler argv[0] as invoked.
/// @return The full argv for the probe.
[[nodiscard]] std::vector<std::string> VersionProbeCommand(std::string const& compiler);

/// The compiler's own version banner: the first line it prints when asked the way
/// `DriverSpec::versionFlags` says to ask it.
///
/// Shared rather than private to the launcher because the compile node needs the
/// identical string. The node derives its own fingerprint from the compiler it
/// was configured with, and a fingerprint is a digest OF this banner among other
/// things -- so two spellings of "what does this compiler call itself" would put
/// a worker and its clients permanently out of agreement, with no error anywhere,
/// just a scheduler that never finds a match.
///
/// It is also the cache key's compiler identity -- `main.cpp`'s `toolchainStamp`,
/// which reaches `ComputeKey`, `ComputeManifestKey` and `ValidateManifest` -- and
/// that is the harsher of the two contracts. A banner that fails to tell two
/// compilers apart is not a fleet that matches nothing; it is one compiler's
/// object replayed for another compiler's compile, under a zero exit code. Which
/// is why every driver's probe is one it answers with a zero exit rather than one
/// spelling assumed to work everywhere (issue #195).
///
/// Falls back to `NormalizedCompilerName` (in `CmdLine.hpp`) when the compiler
/// cannot be run or says nothing. A weak identity beats an empty one: an empty
/// banner would make every unrunnable compiler look like every other. Where that
/// fallback is ALSO the whole identity, `IdentityDefect::NoEvidence` says so.
///
/// @param runner Process-spawning seam.
/// @param compiler The compiler to ask.
/// @return The banner line, or the basename.
[[nodiscard]] std::string CompilerBanner(IProcessRunner& runner, std::string const& compiler);

/// A short, readable name for a compiler: what it is, and which version.
///
/// The fingerprint is the right IDENTITY and the wrong label. It stopped being
/// something a person can derive -- `NodeToolchains`' own comment records that it used
/// to be the `--version` line an operator could read -- so `/fleet` showed two opaque
/// hashes for the two MSVC toolsets an ordinary Visual Studio update leaves on a
/// machine, with no way to tell which was which (#194).
///
/// Derived ONCE, here, rather than formatted per surface. A renderer that shortened a
/// banner would be a second place the value is decided, and this string travels to a
/// leader that renders it in two formats -- so the page and the JSON would then be two
/// spellings of one fact.
///
/// **Display only, and never an identity.** Nothing matches on it, keys on it, or
/// compares it: the fingerprint decides a match, and both are shown side by side
/// because they answer different questions. `ToolchainDiscovery`'s warning is the
/// reason -- a worker that derived its identity differently from its clients would
/// register successfully, heartbeat happily, and never be matched, with nothing
/// anywhere reporting why.
///
/// The version is the banner's first token that is only digits and dots, found with
/// the same `LooksLikeVersion` predicate that decides which SDK subdirectory is a kit
/// version and which `gcc-` suffix is a version -- a third question that must answer
/// alike, rather than a fourth set of rules. That is what makes this need no
/// per-driver table: `Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207 for
/// x64` yields `19.44.35207`, and `g++ (Ubuntu 14.2.0-4ubuntu2) 14.2.0` yields
/// `14.2.0`, because the Ubuntu revision carries letters and is passed over.
///
/// @param compiler The compiler as invoked; names the tool half.
/// @param banner What `CompilerBanner` returned for it.
/// @return e.g. `cl 19.44.35207`, or just the tool when the banner states no version.
///         Empty only when the compiler is.
[[nodiscard]] std::string ToolchainLabel(std::string_view compiler, std::string_view banner);

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

/// Where a driver searches for system headers, and whether it could be asked.
///
/// The second field exists because the first cannot carry it. An empty root list is
/// an ORDINARY answer -- `IncludeDiscovery::None` has none by construction, an MSVC
/// install whose layout is not derivable has none to give, and a wrapper that does
/// not understand `-print-resource-dir` names none -- and every one of those is
/// deliberately served on a banner-only fingerprint (see `ToolchainIdentity`). A
/// driver that could not be SPAWNED is none of them: nothing was measured, so the
/// digest that follows describes no toolchain at all while looking exactly like the
/// digest of one whose include tree is genuinely empty.
///
/// Conflating the two is issue #225. A transient spawn failure at fingerprint time
/// yielded a well-formed hex string no other machine agrees with; `NoEvidence` did
/// not fire, because the banner was a real version line; and the value was written to
/// the fingerprint cache, so a machine that keeps failing settles on it permanently.
/// Both ends stay silent -- the worker registers and is never matched, the client
/// sees `NoWorker` and compiles locally.
struct IncludeSearchRoots
{
    /// The search paths, in the driver's own order. Empty is an ordinary answer.
    std::vector<std::string> roots;

    /// False ONLY when a mechanism that has to run the driver could not run it.
    ///
    /// True where nothing is spawned at all -- `IncludeDiscovery::None`, and
    /// `MsvcLayout`, which reads the filesystem, the registry and the environment.
    /// "Nothing was asked" and "the answer is missing" are different states, and a
    /// mechanism that asks no process has genuinely answered.
    ///
    /// A non-zero exit does NOT clear this, deliberately: a driver prints its search
    /// list before anything that could fail and exits non-zero for reasons that leave
    /// the list perfectly good. Only `CompileRun::exitCode == NotSpawned`, which
    /// `IProcessRunner` defines as "could not be spawned at all", clears it.
    bool answered { true };
};

/// A clang driver's own resource directory: the headers that ship WITH it.
///
/// `<prefix>/lib/clang/<version>/include` -- `stddef.h`, `stdarg.h`, the intrinsics
/// headers -- which belong to this exact clang build rather than to whatever else
/// the machine has installed. **The driver is asked** (`-print-resource-dir`)
/// rather than having it derived from its path, and the difference is correctness
/// rather than taste: `/usr/bin/clang-cl-20` has `/usr` for a prefix, whose
/// `lib/clang` holds `20`, `20.1.2`, `22` and `22.1.8` on an ordinary Debian, and
/// no rule over those names picks the right one. `cl` is modelled from its layout
/// only because `cl` cannot be asked anything; this driver can.
///
/// Nothing ambient reaches the answer, which is the point. `clang-cl` took its
/// search list from `INCLUDE`, which `vcvarsall` sets per shell and a Windows
/// service never inherits, so a launcher in a developer prompt and a worker under
/// the SCM fingerprinted one compiler two ways and the scheduler matched nothing.
/// A resource directory is a property of the binary, so both ends reach it.
///
/// **What is deliberately NOT here is the VC toolset and the Windows SDK.**
/// `MsvcToolsetIncludeRoots` folds them into `cl`'s identity because `cl` LIVES
/// inside the toolset and because its banner is the constant `cl`, so those headers
/// are the only identity it has. Neither holds for `clang-cl`: it announces a
/// genuine version, and it borrows whichever MSVC the machine happens to have.
/// Folding a borrowed tree in would buy no discrimination -- a worker compiles text
/// the client already preprocessed, so it opens no MSVC header -- while costing
/// real matches, since the newest-kit rule would split two boxes running one
/// clang-cl with different SDKs installed.
///
/// @param runner Process-spawning seam.
/// @param host The machine's filesystem.
/// @param compiler The compiler being identified, bare name or path.
/// @return Its resource include root, and whether the driver could be asked at all.
[[nodiscard]] IncludeSearchRoots ClangResourceIncludeRoots(IProcessRunner& runner,
                                                           IToolchainHost& host,
                                                           std::string const& compiler);

/// Extract the target triple from a clang driver's `-###` output.
///
/// Pure, like `ParseGnuIncludeSearchPaths` and for the same reason: the outputs this
/// has to get right come from drivers that cannot all be installed on whatever
/// machine runs the tests, so the parsing is exercised against captured text.
///
/// Read from the `-cc1` line, and **not** from the `Target:` header three lines
/// above it. That distinction is the entire value of this function. Both name a
/// triple, but the header's is unversioned -- `x86_64-pc-windows-msvc` -- while the
/// frontend is really run with `x86_64-pc-windows-msvc19.51.36252`. The suffix is
/// where `-fms-compatibility-version` lives, so a parser that read the easier line
/// would return a triple that looks right, pins the architecture, and silently drops
/// the very thing it was written to pin.
///
/// The answer is validated rather than trusted, because it becomes both a cache key
/// input and a command-line argument. A triple is letters, digits, dots, underscores
/// and dashes; anything carrying a separator or a space is a parse that went wrong,
/// and returning it would poison a key with a path and hand a worker an argument it
/// must refuse.
///
/// @param driverOutput The driver's `-###` output (it writes to STDERR).
/// @return The triple, or empty when the output names none it can trust.
[[nodiscard]] std::string ParseDriverTargetTriple(std::string_view driverOutput);

/// Extract the target from the `Target:` header a GNU driver prints under `-###`.
///
/// The companion to `ParseDriverTargetTriple`, and deliberately a separate function
/// rather than a fallback inside it, because the two disagree about the same line.
/// For `gcc` this header is the ANSWER: it prints no `-cc1` invocation and its
/// frontend takes no `-triple`, so there is nothing more precise to read. For a
/// clang driver the very same header is the WRONG answer -- unversioned, dropping
/// the `-fms-compatibility-version` that the whole probe exists to carry. Which one
/// is authoritative is a property of the driver, so it is decided by the table and
/// not by whichever line a parser happened to find first.
///
/// @param driverOutput The driver's `-###` output (it writes to STDERR).
/// @return The triple, or empty when the output names none it can trust.
[[nodiscard]] std::string ParseDriverTargetHeader(std::string_view driverOutput);

/// Ask a driver which target it will generate for.
///
/// Dispatches on `spec.targetDiscovery` with no `default:`, so a mechanism added to
/// the table is a compile error here rather than a silent empty result.
///
/// Fails open, exactly as `DiscoverIncludePaths` does, and the error direction is
/// safe as far as it goes -- but only that far, so it is worth stating exactly. An
/// empty answer on ONE end leaves that end keying as it did before, so the two
/// machines key differently and stop sharing: a MISS. An empty answer on BOTH ends
/// is the original defect returning in silence, since both fall back to the banner
/// alone. Failing open therefore never turns a working match into a wrong one, and
/// never repairs a pair that was already wrong. The caller says so when a driver
/// that has a mechanism declines to use it.
///
/// One spawn, and it is a real cost: this runs per launcher invocation rather than
/// per machine, because the value it returns is a cache key input and a cache key is
/// needed on hits too. Caching it under the fingerprint's stamp was considered and
/// rejected -- that stamp covers the compiler binary and its include roots, none of
/// which move when the MSVC install beside `clang-cl` is upgraded, so a cached triple
/// would go stale in the one direction that produces a WRONG HIT rather than a miss.
///
/// @param runner Process-spawning seam.
/// @param compiler The compiler to interrogate.
/// @param spec The driver's table row.
/// @return Its target triple; empty when this driver has none to state or would not
///         say.
[[nodiscard]] std::string DiscoverTargetTriple(IProcessRunner& runner, std::string const& compiler, DriverSpec const& spec);

/// Ask a driver where it searches for system headers.
///
/// Dispatches on `spec.includeDiscovery` with no `default:`, so a mechanism added
/// to the table is a compile error here rather than a silent empty result.
///
/// A driver that ANSWERS badly yields an empty list rather than an error:
/// discovery is best-effort by construction. A toolchain whose paths cannot be
/// discovered falls back to a banner-only fingerprint, which is weaker but still
/// correct in the direction that matters -- it can only cause two
/// genuinely-identical toolchains to be treated as identical, never two different
/// ones.
///
/// A driver that could not be RUN is the one case that argument does not cover, and
/// it is reported rather than folded into the empty list: see
/// `IncludeSearchRoots::answered`. Best-effort is a statement about how much of a
/// toolchain was measured, and it needs something to have been measured.
///
/// @param runner Process-spawning seam.
/// @param host The machine's filesystem, registry and environment.
/// @param compiler The compiler to interrogate.
/// @param spec The driver's table row.
/// @return Search paths in the driver's own order, and whether it answered at all.
[[nodiscard]] IncludeSearchRoots DiscoverIncludePaths(IProcessRunner& runner,
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

/// Why a computed fingerprint must not be served as a toolchain's identity.
///
/// Both members name a digest that is a perfectly well-formed hex string and means
/// nothing -- which is why neither can be spotted by looking at the value, and why
/// this travels beside it.
enum class IdentityDefect : std::uint8_t
{
    /// Usable: the digest tells this toolchain from a different one.
    None,

    /// The digest carries no information about WHICH compiler this is.
    ///
    /// Decided where both halves are known, because reconstructing it outside meant
    /// guessing which branch `CompilerBanner` took and deriving the include roots a
    /// second time. The condition: the driver has a way to find its include roots,
    /// that way found none, AND the banner is itself the fallback name. All three,
    /// because each alone is ordinary -- `IncludeDiscovery::None` has no roots by
    /// construction, a real version banner is an identity whatever its roots, and a
    /// fallback banner over a located include tree is exactly the MSVC case that
    /// works.
    ///
    /// Together they are not a weak identity but no identity:
    /// `KeyDigest("toolchain-v1").Field("cl")` is a value this repository could
    /// print with no compiler installed. The "weaker but still correct" argument for
    /// a banner-only fingerprint has an unstated precondition -- that the banner is
    /// a real version string -- and this is that precondition, checked.
    ///
    /// `cl` used to fail it on every machine, which is what made a digest of the
    /// string `cl` the identity of every MSVC toolset in existence (issue #195). It
    /// answers now, so an MSVC toolchain whose layout could not be derived is served
    /// on a banner-only fingerprint rather than refused -- correctly, and on the same
    /// argument `ClangResourceLayout` already rests on: a worker compiles text the
    /// client preprocessed, so it opens no header from the roots that are missing.
    NoEvidence,

    /// The include probe could not be RUN, so nothing about the tree was measured.
    ///
    /// Distinct from `NoEvidence` in the direction that matters: the banner here is
    /// usually a real version line, so the digest looks like a strong identity and
    /// is simply a different one from what every machine that probed successfully
    /// computes. Issue #225 -- observed as a `clang++` that fingerprinted one value,
    /// then another, then the first again across five node startups.
    ///
    /// Transient by nature, which is exactly why it must be reported rather than
    /// absorbed: the cheap remedy is to probe again, and nothing can choose that
    /// remedy if the failure is indistinguishable from success.
    UnrunProbe,

    /// Part of the include tree could not be read, so the digest covers less of the
    /// toolchain than the toolchain has.
    ///
    /// The same shape as `UnrunProbe` one layer down, and the more dangerous of the
    /// two on its own: an unrun probe empties the root list and so moves the stamp,
    /// while a short walk inside a root that IS there leaves every stamped input
    /// identical. Cached, it would validate forever against a walk that never
    /// happens again. See `ToolchainFileScan::complete` for what does and does not
    /// count as short.
    PartialTree,

    Last, ///< Not a defect, and has no row: the table's length.
};

/// One row per defect: the enumerator, what an operator is told, and what to do.
///
/// A table rather than prose at each surface, because there are two surfaces --
/// `NodeToolchains`' refusal and `--print-toolchain-fingerprint`'s warning -- and
/// they were already spelling one defect's reason two ways by hand. One fact, two
/// audiences.
struct IdentityDefectRow
{
    IdentityDefect defect;   ///< The defect this row names.
    std::string_view reason; ///< Why the fingerprint cannot be trusted, as a clause.
    std::string_view remedy; ///< What the operator can do about it, as a sentence.
};

/// The defect table, in enumerator order so a defect indexes its own row.
inline constexpr EnumTable<IdentityDefect, IdentityDefectRow> IdentityDefectTable { {
    { .defect = IdentityDefect::None, .reason = {}, .remedy = {} },
    { .defect = IdentityDefect::NoEvidence,
      .reason = "it could not be asked its version and no include roots were found, so its fingerprint carries "
                "nothing about which compiler it is -- every install of it digests the same",
      .remedy = "Pin one with --toolchain=<fingerprint>=<compiler> if that is deliberate." },
    { .defect = IdentityDefect::UnrunProbe,
      .reason = "its include search paths could not be discovered, because the driver could not be run at all -- so "
                "its fingerprint describes an empty include tree instead of this toolchain's, and no machine that "
                "probed it successfully computes the same value",
      .remedy = "Nothing was cached, so probing again is the remedy; a spawn that fails only sometimes is usually "
                "a scanner or a resource limit rather than the toolchain." },
    { .defect = IdentityDefect::PartialTree,
      .reason = "part of its include tree could not be read, so its fingerprint covers less of the toolchain than "
                "the toolchain has -- and a root that is merely absent does not count, so this is an I/O failure "
                "rather than a layout this machine does not have",
      .remedy = "Nothing was cached, so probing again is the remedy; check whether a scanner or a permission is "
                "holding files under the compiler's include roots." },
} };

static_assert(RowsInEnumeratorOrder(IdentityDefectTable, &IdentityDefectRow::defect),
              "IdentityDefectTable must hold one row per IdentityDefect, in enumerator order -- the order is what "
              "lets a defect index its own row");

/// The row describing @p defect.
/// @param defect The defect to explain.
/// @return Its row; `IdentityDefect::None`'s carries empty text.
[[nodiscard]] constexpr IdentityDefectRow const& ExplainDefect(IdentityDefect defect) noexcept
{
    return IdentityDefectTable[static_cast<std::size_t>(defect)];
}

/// A toolchain fingerprint, and whether it says anything about WHICH compiler it is.
struct ToolchainIdentity
{
    std::string fingerprint; ///< The digest a client must match. Never empty.

    /// Why this digest must not be served, or `None` when it may be.
    IdentityDefect defect { IdentityDefect::None };

    /// Whether this identity may be registered, matched on, or dispatched with.
    ///
    /// Asked rather than compared, so the two consumers cannot drift on what counts
    /// as usable the day a third defect is added.
    /// @return True when the digest identifies a toolchain.
    [[nodiscard]] constexpr bool Usable() const noexcept
    {
        return defect == IdentityDefect::None;
    }
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
