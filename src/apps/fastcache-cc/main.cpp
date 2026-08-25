// SPDX-License-Identifier: Apache-2.0
//
// fastcache-cc — an sccache-style compiler launcher over the fastcached 0xFC
// compile-cache protocol.
//
// Invoked as `fastcache-cc <compiler> <args...>` (e.g. via
// CMAKE_CXX_COMPILER_LAUNCHER). On a cache HIT it reproduces the object file
// and replays the compiler's stdout/stderr (localizing /showIncludes header
// paths to this machine's layout) so the build behaves as if it compiled. On a
// MISS it runs the real compiler, stores the canonicalized result, and passes
// the output through. On ANY cache error it falls back to a plain real compile
// and (when FASTCACHE_VERBOSE is set) prints a one-line diagnostic — the build
// never breaks because the cache is unavailable.
//
// Config (environment):
//   FASTCACHE_ADDR       host:port of the cache; defaults to 127.0.0.1:6674,
//                        empty disables caching
//   FASTCACHE_SOURCE_DIR checkout source root (for keying + canonicalization)
//   FASTCACHE_BINARY_DIR build output root
//   FASTCACHE_PREFETCH_GROUP     optional prefetch group id (default "default")
//   FASTCACHE_VERBOSE    if set, print fall-back diagnostics to stderr
//   FASTCACHE_NO_STATS   if set, do not record invocations to the statistics log
//   FASTCACHE_NO_DIRECT  if set, disable direct mode (always preprocess)
//   FASTCACHE_TIMEOUT_MS per-call socket deadline in ms (default 10000; 0 = none)
//
// The statistics log is located from the usual per-user state variables rather
// than one of our own: LOCALAPPDATA on Windows, else XDG_STATE_HOME or HOME.
//
// Run `fastcache-cc --help` for the flag and environment reference, and
// `--show-stats` for the recorded per-machine cache statistics. The accepted
// flags live in one table in LauncherCli.cpp, which also renders that help.
//
// Contains no project-specific data; it compiles whatever it is pointed at.

#include "CacheKey.hpp"
#include "CacheProtocol.hpp"
#include "CmdLine.hpp"
#include "DependencyOutput.hpp"
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"
#include "Dispatch.hpp"
#include "EndpointDial.hpp"
#include "IProcessRunner.hpp"
#include "LauncherCli.hpp"
#include "PathResolve.hpp"
#include "ReactorExchange.hpp"
#include "ReplayGuard.hpp"
#include "RootReconciler.hpp"
#include "Stats.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Net/TcpClient.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <fcntl.h>
    #include <io.h>
    #include <windows.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{
using namespace FastCache;

// Supplied by the build; defaulted so the tool still compiles standalone.
#if !defined(FASTCACHE_CC_VERSION)
    #define FASTCACHE_CC_VERSION "unknown"
#endif

namespace Wire = FastCache::CompileCacheWire;

// --- config ----------------------------------------------------------------

/// Default per-call socket deadline, overridable with FASTCACHE_TIMEOUT_MS.
constexpr std::chrono::milliseconds DefaultIoTimeout { 10000 };

/// Default ceiling on OPENING a connection, name resolution included.
///
/// A second rather than the I/O timeout's ten, because they bound different
/// things: a cache that has not accepted within a second is one this build is
/// better off without, and a name lookup that hangs would otherwise stall every
/// translation unit with nothing to say why. They used to be one value passed
/// twice.
constexpr std::chrono::milliseconds DefaultConnectTimeout { 1000 };

struct Config
{
    std::string addr;
    std::string srcRoot;
    std::string buildTree;
    std::string prefetchGroup { "default" };
    bool verbose { false };
    bool stats { true };  ///< Record each invocation to the per-user log.
    bool direct { true }; ///< Try the manifest shortcut before preprocessing.
    /// Scheduler endpoint for distributed compilation, empty when not configured.
    ///
    /// Distribution is OFF unless this is set. That is the whole switch: a launcher
    /// with no scheduler behaves exactly as it did before, and a scheduler that
    /// cannot be reached costs one failed connect on a miss and then a local
    /// compile, which is the same shape every other cache failure has here.
    std::string schedulerAddr;

    /// Credential presented to the daemon, empty when none is configured. Held
    /// here rather than read at each exchange so every round trip on one
    /// invocation presents the same thing — and so there is exactly one place
    /// that decides whether this build authenticates at all.
    Cc::Credential credential;
    /// Per-call deadline for every blocking send/recv against the daemon. The
    /// default keeps a wedged daemon from hanging a build while staying far
    /// above any healthy round-trip, including multi-megabyte objects.
    std::chrono::milliseconds ioTimeout { DefaultIoTimeout };
    std::chrono::milliseconds connectTimeout { DefaultConnectTimeout };
    /// Largest encoded value the launcher will offer to the daemon; 0 = no
    /// limit. See Cc::IsStorableSize for why this is a client-side policy.
    std::size_t maxStoreBytes { Cc::DefaultMaxStoreBytes };
};

/// Read an environment variable, or a fallback when unset/empty.
///
/// A set-but-empty variable is treated as unset here: every setting below wants
/// "the user gave me something usable", and an empty FASTCACHE_ADDR is not an
/// address.
/// @param name The variable to read.
/// @param fallback Returned when the variable is unset or empty.
/// @return The value, or `fallback`.
[[nodiscard]] std::string EnvOr(std::string_view name, std::string_view fallback)
{
    auto value = FastCache::ReadEnvironmentVariable(name);
    return value.has_value() && !value->empty() ? std::move(*value) : std::string { fallback };
}

/// Whether an environment variable is set (to any non-empty value).
[[nodiscard]] bool EnvSet(std::string_view name)
{
    return !EnvOr(name, "").empty();
}

/// Read a non-negative integer from the environment.
///
/// Anything unparseable, negative, or carrying trailing junk falls back to
/// `fallback` rather than failing the compile: a typo in a build-system variable
/// must not break the build, which is the same principle as every other cache
/// fall-back. An explicit `0` is honoured, and each caller documents what it
/// means for that setting.
/// @param name Variable to read.
/// @param fallback Value to use when unset or malformed.
/// @return The parsed number, or `fallback`.
[[nodiscard]] std::uint64_t EnvUnsigned(std::string_view name, std::uint64_t fallback)
{
    auto const raw = EnvOr(name, "");
    if (raw.empty())
        return fallback;
    auto value = std::int64_t { 0 };
    auto const* const begin = raw.data();
    auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(raw.size()));
    auto const [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {} || ptr != end || value < 0)
        return fallback;
    return static_cast<std::uint64_t>(value);
}

/// Read a non-negative millisecond count from the environment. `0` means "no
/// timeout".
/// @param name Variable to read.
/// @param fallback Value to use when unset or malformed.
/// @return The parsed duration, or `fallback`.
[[nodiscard]] std::chrono::milliseconds EnvMillis(std::string_view name, std::chrono::milliseconds fallback)
{
    auto const count = static_cast<std::uint64_t>(fallback.count());
    return std::chrono::milliseconds { static_cast<std::int64_t>(EnvUnsigned(name, count)) };
}

/// The path-identity seam every root comparison in this file depends on.
///
/// Created once and memoized, so the per-directory cost the resolver documents is
/// paid once per compile rather than once per lookup.
[[nodiscard]] Cc::IPathResolver& PathResolver()
{
    static std::unique_ptr<Cc::IPathResolver> const resolver = Cc::MakePathResolver();
    return *resolver;
}

[[nodiscard]] Config LoadConfig()
{
    Config c;
    // Three-valued on purpose, unlike every other setting here. UNSET means "use
    // the default", which is localhost -- so the launcher caches with no
    // configuration at all against whichever of `fastcached` or
    // `fastcache-compile-node` a developer is running, and the node's
    // `--listen-cache` defaults to the same address for exactly that reason. SET
    // BUT EMPTY still means *off*, which is the documented opt-out
    // `cmake/portable/CompileCache.cmake` exports, so `EnvOr` -- which collapses the
    // two -- cannot be used here without turning that opt-out into the default.
    //
    // Defaulting to a REMOTE address would be indefensible: every translation unit
    // on a machine with nothing listening would pay a connect timeout in silence.
    // Loopback is not that -- a closed port refuses immediately, no timeout and no
    // round trip -- which is the whole argument for this default and why it could
    // not be any other address.
    auto const configuredAddr = FastCache::ReadEnvironmentVariable(Cc::EnvName::Addr);
    c.addr = configuredAddr.has_value() ? *configuredAddr : std::string { Cc::DefaultAddr };
    // Deliberately NOT resolved here. These are the spelling the build system
    // exported, and they stay that way: they are the roots that go on the wire,
    // that the key tokenizes against, and that a hit's replayed paths are built
    // from. RootReconciler holds the resolved forms and uses them only to
    // translate INTO these — see the class comment for why emitting anything else
    // breaks the depfile a build system reads back.
    c.srcRoot = Cc::WithoutTrailingSeparator(EnvOr(Cc::EnvName::SourceDir, ""));
    c.buildTree = Cc::WithoutTrailingSeparator(EnvOr(Cc::EnvName::BinaryDir, ""));
    c.prefetchGroup = EnvOr(Cc::EnvName::PrefetchGroup, "default");
    c.verbose = EnvSet(Cc::EnvName::Verbose);
    c.stats = !EnvSet(Cc::EnvName::NoStats);
    c.direct = !EnvSet(Cc::EnvName::NoDirect);
    c.ioTimeout = EnvMillis(Cc::EnvName::TimeoutMs, DefaultIoTimeout);
    c.connectTimeout = EnvMillis(Cc::EnvName::ConnectTimeoutMs, DefaultConnectTimeout);
    // A username without a token is not a credential, and `Credential::Configured`
    // keys on the secret alone — so an operator who sets only FASTCACHE_USER gets
    // the same unauthenticated behaviour they had before, rather than an AUTH
    // frame carrying an empty secret that every server would refuse.
    c.schedulerAddr = EnvOr(Cc::EnvName::Scheduler, "");
    c.credential.username = EnvOr(Cc::EnvName::User, "");
    c.credential.secret = EnvOr(Cc::EnvName::Token, "");
    // Clamped, not merely cast: the reader is 64-bit and `std::size_t` need not
    // be, and a truncating cast turns a ceiling somebody raised into a tiny one
    // that silently stops caching almost everything.
    //
    // `max` is parenthesized to defeat windows.h's function-style max() macro,
    // for the same reason Stats.cpp is: this target deliberately does not link
    // the FastCache library, and NOMINMAX is defined on that library's target.
    // `std::min<...>` needs no such guard -- the explicit template argument puts
    // a `<` where the macro would need a `(` -- but it is spelled the same way
    // so that dropping the argument later cannot quietly reintroduce this.
    constexpr auto SizeMax = (std::numeric_limits<std::size_t>::max)();
    c.maxStoreBytes = static_cast<std::size_t>(
        (std::min<std::uint64_t>) (EnvUnsigned(Cc::EnvName::MaxStoreBytes, Cc::DefaultMaxStoreBytes), SizeMax));
    return c;
}

/// What this invocation ended up doing, for the statistics log.
///
/// Accumulated as the flow proceeds rather than returned through the call chain,
/// so the cache flow keeps its "exit code or fall back" shape; main() writes the
/// record once, at the end. One process handles exactly one compile, so this is
/// per-invocation state rather than shared state — but it is still reached by
/// name instead of passed, so it lives in a single object where every write to
/// the eventual record is visible in one declaration.
struct InvocationRecord
{
    bool verbose = false; ///< FASTCACHE_VERBOSE; gates every diagnostic here.

    Cc::Outcome outcome = Cc::Outcome::Unavailable; ///< Hit / Miss / Uncacheable / Unavailable.
    std::string outcomeDetail;                      ///< Fall-back reason; empty on hit and miss.
    std::uint64_t valueBytes = 0;                   ///< Cached payload size; 0 when nothing moved.

    std::uint64_t preprocessMs = 0; ///< Deriving the key (preprocess + compiler id).
    std::uint64_t cacheMs = 0;      ///< Talking to the daemon (connect + transfer).

    /// Direct-mode accounting: how long the manifest shortcut took, and whether it
    /// succeeded (so the report can separate a direct hit from a preprocessed one).
    std::uint64_t directMs = 0;
    bool directHit = false;
};

InvocationRecord invocation;

/// Milliseconds elapsed since `start`, for the phase counters above.
[[nodiscard]] std::uint64_t MsSince(std::chrono::steady_clock::time_point start)
{
    auto const delta = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

/// Print a one-line fall-back diagnostic when FASTCACHE_VERBOSE is set, without
/// touching the recorded outcome. For diagnostics issued AFTER the outcome is
/// already decided — a miss that compiled successfully but could not be stored
/// is still a miss, and recording it as a cache failure would inflate the
/// "unavailable" bucket in `--show-stats` and hide the real cause.
/// @param reason The diagnostic text.
void Note(std::string_view reason)
{
    if (invocation.verbose)
        std::cerr << "fastcache-cc: " << reason << '\n';
}

/// Report that the cache could not serve this compile, and record why.
///
/// Only for the fall-back path, where the launcher gives up on the cache and
/// runs the real compiler: it OVERWRITES the recorded outcome. Once a HIT or
/// MISS has been traced, use Note() instead.
/// @param reason The fall-back reason, recorded as the statistics detail.
void Warn(std::string_view reason)
{
    // A fall-back reason distinguishes "deliberately not cacheable" from "the
    // cache let us down" — the two need different responses, so the statistics
    // report separates them.
    bool const deliberate = reason.starts_with("uses __TIME__");
    invocation.outcome = deliberate ? Cc::Outcome::Uncacheable : Cc::Outcome::Unavailable;
    invocation.outcomeDetail = reason;
    if (invocation.verbose)
        std::cerr << "fastcache-cc: cache unavailable (" << reason << "); running real compiler\n";
}

/// Emit a HIT/MISS trace line (stderr) when FASTCACHE_VERBOSE is set. Useful in
/// real use to see the cache working, and the signal the E2E harness asserts on.
void TraceOutcome(std::string_view outcome, std::string_view key)
{
    invocation.outcome = (outcome == "HIT") ? Cc::Outcome::Hit : Cc::Outcome::Miss;
    invocation.outcomeDetail.clear();
    if (invocation.verbose)
        std::cerr << "fastcache-cc: " << outcome << " key=" << key << '\n';
}

/// Report a daemon refusal on a best-effort side-channel operation.
///
/// Only a refusal is worth a line. A miss is the normal case, and a transport
/// failure is already covered by the caller's own fall-back reason; a refusal is
/// the one outcome meaning the daemon is reachable, answered, and declined —
/// including the version mismatch that would otherwise leave the cache silently
/// useless for an entire build with nothing at all to show for it.
/// @param outcome The completed exchange.
/// @param what Short label for the operation, e.g. "STORE (raw)".
/// @param key The key involved.
void WarnIfRejected(Cc::CacheOutcome const& outcome, std::string_view what, std::string_view key)
{
    if (outcome.kind != Cc::CacheOutcomeKind::Rejected)
        return;
    Note(std::format("{} key={} {}", what, key, Cc::DescribeOutcome(outcome)));
}

/// Report a compile whose roots do not describe it at all.
///
/// Gated on FASTCACHE_VERBOSE like every other launcher diagnostic, and that is a
/// deliberate reversal: this started ungated, on the reasoning that issue #66's
/// defect is invisible and a diagnostic nobody enables is as silent as none. Two
/// rounds of narrowing failed to find a condition that means "broken" reliably
/// enough to justify four unsilenceable lines on the compiler's stderr for every
/// translation unit. A source outside both roots is an ordinary CMake layout —
/// `add_subdirectory(../shared shared)`, a superbuild, ExternalProject — not a
/// misconfiguration, and a message that cries wolf on a healthy build is the one
/// that gets ignored when it is right. What tipped it: `RootReconciler` now
/// REPAIRS the mismatch rather than merely detecting it, so this is a backstop for
/// a spelling the resolver could not reconcile, not the mechanism.
///
/// The condition is still narrower than "nothing was keyed", and the difference is
/// the SOURCE. `/showIncludes` never names the primary source, so on MSVC a
/// translation unit including only third-party headers outside the roots reports
/// paths and keys none of them while being perfectly healthy; `0 of 0` is a driver
/// that reported nothing on the preprocess line, a different fault this message
/// would misdescribe.
///
/// @param cfg          Launcher config, for the roots to name.
/// @param sourcePath   The translation unit, as the build system spelled it.
/// @param dependencies The classified dependency set, which carries both counts
///                     this condition needs. Passed whole rather than as a keyed
///                     count beside the raw path vector, so the two cannot be
///                     derived twice and come to disagree — the reason
///                     `DependencySet::Reported` exists at all.
/// @param reconciler   Decides whether the source has a portable form at all.
void NoteIfRootsDoNotDescribeCompile(Config const& cfg,
                                     std::string_view sourcePath,
                                     Cc::DependencySet const& dependencies,
                                     Cc::RootReconciler& reconciler)
{
    if (!dependencies.keyed.empty() || dependencies.Reported() == 0 || reconciler.IsInTree(sourcePath))
        return;

    // The roots next to the source they fail to contain: the mismatch is only
    // visible as a pair, and a list of every reported path would bury it.
    Note(std::format("the configured roots do not contain this translation unit, so nothing about this compile is"
                     " portable -- the checkout path stays in the key, a moved header cannot re-key, and the replay"
                     " guard is checking nothing (source root: {}, build tree: {}, source: {})",
                     cfg.srcRoot,
                     cfg.buildTree,
                     sourcePath));
}

/// Report, once, that a configured credential was not understood by the daemon.
///
/// This is not a failure — the exchange succeeded and the cache is working — so
/// it is deliberately not a fall-back reason and does not touch the recorded
/// outcome. It is said out loud anyway because the operator asked for something
/// that did not happen: a daemon predating the AUTH verb steps over it and serves
/// the traffic unauthenticated. Believing a shared cache is authenticated when it
/// is not is worth a line in a build log.
///
/// Guarded so a build of thousands of translation units says it once rather than
/// thousands of times, which is the difference between a diagnostic and noise.
/// @param outcome The completed exchange.
void NoteIfCredentialIgnored(Cc::CacheOutcome const& outcome)
{
    static bool reported = false;
    if (!outcome.credentialIgnored || reported)
        return;
    reported = true;
    Note("daemon does not support authentication; the configured credential was ignored");
}

// --- process exec -----------------------------------------------------------

/// The process runner used by every spawn in this file. Created once; the
/// concrete implementation (CreateProcess on Windows, posix_spawn elsewhere)
/// is chosen behind the IProcessRunner seam.
[[nodiscard]] Cc::IProcessRunner& ProcessRunner()
{
    static std::unique_ptr<Cc::IProcessRunner> const runner = Cc::MakeProcessRunner();
    return *runner;
}

using Cc::CompileRun;

/// Run `argv` with stdout and stderr captured separately.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return Exit code plus both captured streams.
[[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv)
{
    return ProcessRunner().RunCaptureSplit(argv);
}

/// Replay captured child bytes on one of our own streams verbatim.
///
/// Writes through the C `FILE*` in binary mode rather than `std::cout`/`std::cerr`:
/// the captured bytes already contain the compiler's own CRLF line endings, and a
/// text-mode stream would translate each '\n' again, emitting CR CR LF. That shows
/// up as a blank line between every real line and corrupts the /showIncludes bytes
/// Ninja parses for dependencies.
void ReplayVerbatim(std::string_view bytes, std::FILE* stream)
{
    if (bytes.empty())
        return;
#if defined(_WIN32)
    int const previousMode = _setmode(_fileno(stream), _O_BINARY);
#endif
    std::fwrite(bytes.data(), 1, bytes.size(), stream);
    std::fflush(stream);
#if defined(_WIN32)
    if (previousMode != -1)
        _setmode(_fileno(stream), previousMode);
#endif
}

/// Replay a captured stdout/stderr pair verbatim on our own streams.
void ReplayStreams(std::string_view out, std::string_view err)
{
    // Flush the iostreams first: our own diagnostics go through std::cerr, and
    // mixing them with these raw writes would otherwise reorder the output.
    std::cout.flush();
    std::cerr.flush();
    ReplayVerbatim(out, stdout);
    ReplayVerbatim(err, stderr);
}

/// Run the real compiler with the given argv, streaming its combined output to
/// our stdout, and return its exit code. Used for both fallback and miss when
/// we do NOT need to capture (fallback) — the miss path uses RunCapture.
[[nodiscard]] int RunPassthrough(std::span<std::string const> argv)
{
    auto const run = RunCaptureSplit(argv);
    ReplayStreams(run.out, run.err);
    return run.exitCode == -1 ? 1 : run.exitCode;
}

// --- file helpers -----------------------------------------------------------

[[nodiscard]] std::optional<std::vector<std::byte>> ReadFileBytes(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        return std::nullopt;
    std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (char const c: raw)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] bool WriteFileBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream out { path, std::ios::binary };
    if (!out)
        return false;
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

/// The deadlines this invocation runs every cache exchange under.
/// @param cfg The launcher's configuration.
/// @return The budget.
[[nodiscard]] Cc::ExchangeBudget BudgetOf(Config const& cfg)
{
    return Cc::ExchangeBudget { .connect = cfg.connectTimeout, .total = cfg.ioTimeout };
}

/// The grammar to tag the include-bearing stream with, per compiler flavor.
[[nodiscard]] PathCanon::Grammar IncludeGrammar()
{
    return PathCanon::Grammar::ShowIncludes;
}

/// Index of the depfile inside a stored value's text regions.
///
/// Regions are positional: 0 = stdout, 1 = stderr, 2 = the GNU depfile. A value
/// stored before depfile support simply has no region 2, so it decodes and
/// replays exactly as before — the addition is backward compatible.
constexpr std::size_t DepFileRegionIndex = 2;

/// Number of captured streams replayed to our own stdout/stderr. Regions beyond
/// these are files, not streams, and must never be replayed.
constexpr std::size_t ReplayRegionCount = 2;

/// Read the depfile a GNU-driver compile just wrote, for storing alongside the
/// object.
///
/// A depfile is the build system's record of which headers a translation unit
/// depends on. Ninja and Make read it to decide whether to recompile; without
/// it the TU appears to depend on nothing and stops rebuilding when its headers
/// change. It therefore has to be reproduced on a hit exactly like the object.
///
/// @param cmd The parsed compile command.
/// @return The depfile text, or nullopt when the line requested none (or it is
///         unreadable, in which case there is simply nothing to cache).
[[nodiscard]] std::optional<std::string> ReadDepFile(Cc::ParsedCommand const& cmd)
{
    if (cmd.depPath.empty())
        return std::nullopt;
    auto const bytes = ReadFileBytes(std::filesystem::path { cmd.depPath });
    if (!bytes.has_value())
        return std::nullopt;
    return std::string { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
}

/// Write the cached depfile back.
///
/// Called on every hit. A miss wrote its own depfile as a side effect of running
/// the real compiler; a hit runs no compiler, so without this the file the build
/// system depends on would simply be absent (or, worse, left stale from an
/// earlier build).
///
/// Takes text that is already localized rather than localizing here: the hit path
/// localizes every region once up front, because the same localized text is what
/// the replay guard examines before any of it is written.
///
/// @param depPath The destination, from the compile command's -MF.
/// @param text    The localized depfile bytes.
/// @return True when the write succeeded.
[[nodiscard]] bool WriteDepFile(std::string const& depPath, std::string_view text)
{
    std::ofstream out { std::filesystem::path { depPath }, std::ios::binary };
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

/// True if the source file at `path` references a compile-time-varying macro
/// (`__TIME__` / `__DATE__` / `__TIMESTAMP__`). Such a TU cannot be cached
/// because it re-keys every second. Reads the file text and looks for the
/// macro token as a substring — a superset check: a false positive only costs
/// a (correct) cache skip, never incorrectness.
/// @param path Source file path.
/// @return True if any volatile macro token appears in the file.
[[nodiscard]] bool SourceReferencesVolatileMacro(std::string const& path)
{
    auto const bytes = ReadFileBytes(std::filesystem::path { path });
    if (!bytes.has_value())
        return false; // cannot read → let the normal flow handle it
    std::string_view const text { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
    constexpr auto VolatileMacros = std::to_array<std::string_view>({ "__TIME__", "__DATE__", "__TIMESTAMP__" });
    return std::ranges::any_of(VolatileMacros, [text](std::string_view const macro) { return text.contains(macro); });
}

// --- preprocess + compiler identity ----------------------------------------

/// Remove a file when the enclosing scope ends.
///
/// The probe's depfile is an artefact of asking a question, not something the
/// build asked for, so it must not outlive the question — including on the paths
/// that return early. RAII rather than a delete at each exit, for the reason
/// every other resource here is owned by a wrapper.
class ScopedFile
{
  public:
    /// @param path The file to remove on destruction.
    explicit ScopedFile(std::filesystem::path path):
        _path { std::move(path) }
    {
    }
    ScopedFile(ScopedFile const&) = delete;
    ScopedFile(ScopedFile&&) = delete;
    ScopedFile& operator=(ScopedFile const&) = delete;
    ScopedFile& operator=(ScopedFile&&) = delete;

    ~ScopedFile()
    {
        if (_path.empty())
            return;
        // The error_code overload, never the throwing one: a launcher whose whole
        // contract is that a cache problem cannot break a build must not throw out
        // of a destructor because a temporary file was already gone.
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

  private:
    std::filesystem::path _path;
};

/// What one preprocess probe yields: the text the key hashes, and the paths the
/// compile depends on.
struct SourceProbe
{
    std::string preprocessed;                 ///< Preprocessor output, no line markers.
    std::vector<std::string> dependencyPaths; ///< Dependencies as the compiler spelled them.
};

/// Preprocess the source (compiler-native, no line markers) AND collect the
/// dependency paths, in one compiler run, for the cache key.
///
/// The per-driver spelling lives in the CmdLine driver table: MSVC drivers use
/// `/EP` (preprocess to stdout with no `#line` directives) and report dependencies
/// as `/showIncludes` notes on a stream; GNU drivers use `-E` and write a depfile.
/// Either way the compile action and the build's own dependency flags are dropped
/// and the probe's own appended in their place.
///
/// One run rather than two: the compiler has already opened every one of those
/// files, so naming them measured at +1.5% on a 45 ms preprocess. The probe's
/// depfile goes to a path of its own — writing the build's `-MF` would leave a
/// probe's depfile behind for a hit that is then discarded, which is the very
/// state the hit path exists to avoid.
///
/// Failing to obtain dependencies is not a failure of the probe. The key is then
/// computed over an empty set, which costs this translation unit the protection
/// against a moved header and nothing else; refusing to cache would be the larger
/// harm.
///
/// @param cmd The parsed compile command.
/// @param originalArgs The original full invocation.
/// @return The preprocessed text and dependency paths, or nullopt when the probe
///         itself failed.
[[nodiscard]] std::optional<SourceProbe> Preprocess(Cc::ParsedCommand const& cmd,
                                                    std::span<std::string const> originalArgs,
                                                    Cc::RootReconciler& reconciler)
{
    auto const& driver = Cc::DriverOf(cmd.flavor);

    // One string with two jobs, as PreprocessCommand documents: it is the
    // destination for a depfile driver and merely the REQUEST for a stream one.
    // Derived from the object path, which is unique per translation unit, so
    // parallel compiles in one build tree cannot collide — and it sits in a
    // directory the compile already writes to.
    std::string probeRequest = cmd.objPath + ".fcdep";

    // A depfile destination is PROVEN writable before it is asked for, because a
    // driver treats an unopenable `-MF` as a FATAL error rather than a warning: a
    // build tree that will not take the scratch depfile would otherwise turn every
    // compile in it into "preprocess failed" and an uncached build, forever and
    // silently. Withdrawing the request is the degradation this function's contract
    // describes — an empty set costs this TU its moved-header protection and
    // nothing else. Only a depfile driver has a file to remove afterwards; a stream
    // driver never creates one, so it is given no guard rather than a no-op guard.
    std::filesystem::path probeDepFilePath;
    if (driver.usesDepfile)
    {
        if (std::ofstream { probeRequest, std::ios::binary })
            probeDepFilePath = probeRequest;
        else
        {
            Note("dependency probe destination is not writable; keying without the dependency set");
            probeRequest.clear();
        }
    }
    ScopedFile const probeDepFile { std::move(probeDepFilePath) };

    auto const pp = Cc::PreprocessCommand(cmd, originalArgs, probeRequest);
    // Capture stdout and stderr SEPARATELY. Merging them (2>&1) would fold the
    // compiler's diagnostic lines into the hashed text, and the interleave point
    // of two independently-buffered streams is not stable run-to-run — which would
    // make the key nondeterministic and defeat all caching.
    auto run = RunCaptureSplit(pp);
    if (run.exitCode != 0)
        return std::nullopt;

    if (driver.usesDepfile)
    {
        SourceProbe probe { .preprocessed = std::move(run.out), .dependencyPaths = {} };
        if (probeRequest.empty())
            return probe;
        // Non-empty, not merely present: the writability check above created the
        // file, so a driver that wrote nothing into it would otherwise read as a
        // successful probe that found no dependencies.
        if (auto const bytes = ReadFileBytes(std::filesystem::path { probeRequest }); bytes.has_value() && !bytes->empty())
            probe.dependencyPaths =
                Cc::ParseDepFilePaths(std::string_view { reinterpret_cast<char const*>(bytes->data()), bytes->size() });
        else
            Note("dependency probe wrote no depfile; keying without the dependency set");
        // Reconciled at the boundary, so KeyDependencySet and everything after it
        // stay pure string work over paths spelled the way this host spells them.
        reconciler.All(probe.dependencyPaths);
        return probe;
    }

    // A stream driver's notes are taken from BOTH streams, and stdout is split
    // unconditionally — `DriverSpec::includeStream` describes the COMPILE run and
    // must not be trusted here. Measured: `clang-cl /c /showIncludes` puts notes
    // on stdout, but `clang-cl /EP /showIncludes` — the probe's own line — puts
    // them on STDERR, because clang deliberately moves them off the stream the
    // preprocessed text is using (LLVM D46394). Routing by the table therefore
    // read an empty set on clang-cl and made this whole key input a no-op there.
    // Guessing is what RecordManifest already refuses to do for the same question,
    // and the split is a byte-exact no-op on a stream that carries no notes.
    auto split = Cc::SplitIncludeNotes(run.out);
    auto errorNotes = Cc::ParseIncludePaths(run.err);
    split.notePaths.insert(
        split.notePaths.end(), std::make_move_iterator(errorNotes.begin()), std::make_move_iterator(errorNotes.end()));
    reconciler.All(split.notePaths);
    return SourceProbe { .preprocessed = std::move(split.preprocessed), .dependencyPaths = std::move(split.notePaths) };
}

/// This compiler's identity for keying: its version banner.
///
/// A thin call through to `Cc::CompilerBanner`, which the compile node also uses.
/// One definition, because the node derives a fingerprint from this exact string
/// and two spellings would put a worker permanently out of agreement with its
/// clients -- silently, as a scheduler that simply never matches.
/// @param compiler The compiler being fronted.
/// @return The banner line, or the compiler's basename.
[[nodiscard]] std::string CompilerId(std::string const& compiler)
{
    return Cc::CompilerBanner(ProcessRunner(), compiler);
}

/// Print the toolchain fingerprint a dispatched compile would send.
///
/// Recomputes unconditionally rather than reading the cache. This command exists
/// to answer "why did no worker match", and a cached answer cannot distinguish
/// "the two machines genuinely differ" from "one of them is holding a stale
/// entry" -- which is exactly what the cache's documented staleness window makes
/// possible. It rewrites the cache on the way past, so running it is the remedy
/// as well as the diagnosis.
/// @param compiler The compiler to interrogate.
/// @return Process exit code.
[[nodiscard]] int PrintToolchainFingerprint(std::string const& compiler)
{
    auto const banner = CompilerId(compiler);
    auto const flavor = Cc::ClassifyCompiler(compiler);
    auto const fingerprint =
        Cc::CachedToolchainFingerprint(ProcessRunner(), compiler, banner, Cc::DriverOf(flavor), /*forceRefresh=*/true);

    std::cout << fingerprint << '\n';
    return 0;
}

// --- the cache flow ---------------------------------------------------------

/// FETCH one key and return its raw stored bytes, or nullopt on miss or any
/// transport failure. Used for manifests, whose payload is not a compile-value.
/// @param cfg  Launcher config (daemon address and socket deadline).
/// @param key  The key to fetch.
/// @return The stored bytes on hit.
[[nodiscard]] std::optional<std::vector<std::byte>> FetchRaw(Config const& cfg, std::string const& key)
{
    auto outcome = Cc::RunOneExchange(cfg.addr, Wire::EncodeFetch(key), cfg.credential, BudgetOf(cfg));
    NoteIfCredentialIgnored(outcome);
    if (!outcome.IsHit())
    {
        WarnIfRejected(outcome, "manifest fetch", key);
        return std::nullopt;
    }
    return std::move(outcome.value);
}

/// STORE bytes under `key`, best-effort. A manifest that fails to store only
/// costs the next compile its shortcut, so failures are silent unless verbose.
///
/// The payload MUST be a compile-value frame: the daemon decodes every STORE as
/// one (to canonicalize its text regions) and rejects anything else outright. A
/// manifest therefore travels as a compile-value whose object blob is the manifest
/// and whose text-region list is empty — nothing to canonicalize, and no protocol
/// change needed.
/// @param addr Daemon address.
/// @param cfg  Launcher config (prefetch group and layout travel with the store).
/// @param key  The key to store under.
/// @param body The bytes to store.
void StoreRaw(std::string const& addr, Config const& cfg, std::string const& key, std::string_view body)
{
    // Check the outcome rather than discarding it: a rejected STORE is silent
    // otherwise, and a manifest that never lands makes direct mode look simply
    // ineffective.
    auto const outcome = Cc::RunOneExchange(addr,
                                            Wire::EncodeStore(Wire::StoreRequest { .key = key,
                                                                                   .prefetchGroup = cfg.prefetchGroup,
                                                                                   .srcRoot = cfg.srcRoot,
                                                                                   .buildTree = cfg.buildTree,
                                                                                   .value = Wire::AsBytes(body) }),
                                            cfg.credential,
                                            BudgetOf(cfg));
    NoteIfCredentialIgnored(outcome);
    WarnIfRejected(outcome, "STORE (raw)", key);
}

/// What became of a cache hit we tried to honour.
///
/// Three outcomes rather than a bool, because the two failures want opposite
/// responses: a value whose dependency record no longer holds must be RECOMPILED
/// AND RE-STORED (which repairs the entry), while a value we simply could not
/// write to disk means the cache is not usable here and the compile should run
/// plainly, uncached.
enum class HitDisposition : std::uint8_t
{
    Served,   ///< Object and depfile written, streams replayed.
    Stale,    ///< A replayed dependency is missing here; recompile and re-store.
    Unusable, ///< The object or depfile could not be written; abandon the cache.
};

/// A hit after it has been examined and, if it held up, materialized.
struct MaterializedHit
{
    HitDisposition disposition { HitDisposition::Unusable };
    std::string replayOut; ///< Localized region 0, kept for the manifest backfill.
    std::string replayErr; ///< Localized region 1, same.
};

/// A hit that was not materialized, so carries no streams. A named factory rather
/// than a brace-init at each site: the streams then have exactly one place that
/// says they are deliberately empty.
/// @param disposition Why it was not materialized.
/// @return The outcome, with empty replay text.
[[nodiscard]] MaterializedHit NotMaterialized(HitDisposition disposition)
{
    return { .disposition = disposition, .replayOut = {}, .replayErr = {} };
}

/// Localize a hit's regions, check that this machine can honour what they assert,
/// and only then write the object, the depfile, and the replayed streams.
///
/// The check comes before every write on purpose. A hit we are going to discard
/// must leave the tree exactly as an uncached build would, so that the compile
/// running next writes the object itself — and so that a compile which then fails
/// has not left a cached object and depfile behind for a build that never
/// succeeded.
///
/// Localizing every region once here, rather than per consumer, is what lets the
/// guard examine precisely the bytes that are about to be written; it also leaves
/// exactly one copy of a loop that used to exist twice.
///
/// @param cmd              The parsed compile command (object path, depfile path).
/// @param decoded          The decoded cached value.
/// @param layout           This machine's roots.
/// @param workingDirectory The directory this compile runs in, for resolving the
///                         relative dependency paths the value replays.
/// @return What happened, plus the localized streams for the manifest backfill.
[[nodiscard]] MaterializedHit MaterializeHit(Cc::ParsedCommand const& cmd,
                                             CompileValue const& decoded,
                                             PathCanon::Layout const& layout,
                                             std::filesystem::path const& workingDirectory)
{
    // Localize everything the value carries. Region 2 is the depfile and is a
    // file, not a stream; regions beyond ReplayRegionCount must never be replayed,
    // which is why the positional contract stays explicit here.
    std::vector<TextRegion> localized;
    localized.reserve(decoded.textRegions.size());
    for (auto const& region: decoded.textRegions)
    {
        auto rewritten = PathCanon::LocalizeRegion(region.bytes, region.grammar, layout);
        localized.push_back(
            { .grammar = region.grammar, .bytes = rewritten.has_value() ? *std::move(rewritten) : region.bytes });
    }

    // A depfile the compile did not ask for is not going to be written, so it must
    // not be able to veto the hit either.
    auto assertions = std::span<TextRegion const> { localized };
    if (cmd.depPath.empty() && assertions.size() > DepFileRegionIndex)
        assertions = assertions.first(DepFileRegionIndex);

    if (auto const missing = Cc::MissingReplayedDependency(assertions, layout, workingDirectory); missing.has_value())
    {
        // Named rather than merely counted: "why does this TU never cache" is
        // otherwise a whole investigation, and the answer is one path.
        Note(std::format("STALE HIT (replayed dependency missing: {}); recompiling", *missing));
        return NotMaterialized(HitDisposition::Stale);
    }

    if (!WriteFileBytes(cmd.objPath, decoded.objectBlob))
        return NotMaterialized(HitDisposition::Unusable);

    // The depfile is a file, not a stream: restore it before replaying, so a
    // failure to write it is not reported after the build has already seen the
    // compiler's output.
    if (!cmd.depPath.empty() && localized.size() > DepFileRegionIndex
        && !WriteDepFile(cmd.depPath, localized[DepFileRegionIndex].bytes))
        return NotMaterialized(HitDisposition::Unusable);

    // Only the first ReplayRegionCount regions are streams; the rest are files and
    // must never reach stdout or stderr.
    MaterializedHit result = NotMaterialized(HitDisposition::Served);
    std::array<std::string*, ReplayRegionCount> const streams { &result.replayOut, &result.replayErr };
    for (std::size_t idx = 0; idx < localized.size() && idx < streams.size(); ++idx)
        *streams[idx] = localized[idx].bytes;
    ReplayStreams(result.replayOut, result.replayErr);
    return result;
}

/// Fetch `key`, and if it holds a compile value, materialize it: write the object
/// and replay the captured streams with paths localized to this machine.
/// @param cfg              Launcher config.
/// @param cmd              The parsed compile command (object path, source).
/// @param key              The object key to serve.
/// @param layout           This machine's roots, for localizing the replayed text.
/// @param workingDirectory The directory this compile runs in.
/// @return The exit code to return on a hit, or nullopt when not served.
[[nodiscard]] std::optional<int> TryServeFromCache(Config const& cfg,
                                                   Cc::ParsedCommand const& cmd,
                                                   std::string const& key,
                                                   PathCanon::Layout const& layout,
                                                   std::filesystem::path const& workingDirectory)
{
    auto const payload = FetchRaw(cfg, key);
    if (!payload.has_value())
        return std::nullopt;

    auto decoded = DecodeCompileValue(*payload);
    if (!decoded.has_value())
        return std::nullopt;

    // Anything short of Served falls back to preprocessing, which re-runs the same
    // check and, if it also finds the value stale, recompiles and re-stores it.
    // Direct mode only ever declines to shortcut; repairing the entry is not its
    // job, and doing it here would duplicate the miss path.
    if (MaterializeHit(cmd, *decoded, layout, workingDirectory).disposition != HitDisposition::Served)
        return std::nullopt;

    invocation.valueBytes = decoded->objectBlob.size();
    TraceOutcome("HIT", key);
    return 0;
}

/// Record the direct-mode manifest for a compile whose object bytes are already
/// stored, so the next compile of this TU can skip preprocessing.
///
/// Called from BOTH the hit and the miss path. On a miss the include text comes
/// from the compiler that just ran; on a hit it comes from the cached value's
/// replayed streams — either way it names the same headers, and neither requires
/// an extra compiler invocation. The manifest records the object's ordinary key
/// rather than causing a second copy of the object to be stored: see
/// DirectManifest::objectKey for why duplicating it is not affordable.
/// @param cfg             Launcher config.
/// @param cmd             The parsed compile command.
/// @param layout              This machine's roots.
/// @param workingDirectory    The directory this compile runs in; every relative
///                            path the driver reported resolves against it.
/// @param relativizedArgs     The relativized compile arguments.
/// @param toolchainStamp      The toolchain identity.
/// @param includeTextOut      Captured stdout (may carry the include notes).
/// @param includeTextErr      Captured stderr (cl puts include notes here).
/// @param objectKeyForPointer Key the object is already stored under; recorded in
///                            the manifest so the object is never stored twice.
/// @param reconciler      Translates a driver's spelling into this build's.
void RecordManifest(Config const& cfg,
                    Cc::ParsedCommand const& cmd,
                    PathCanon::Layout const& layout,
                    std::filesystem::path const& workingDirectory,
                    std::vector<std::string> const& relativizedArgs,
                    std::string const& toolchainStamp,
                    std::string_view includeTextOut,
                    std::string_view includeTextErr,
                    std::string_view objectKeyForPointer,
                    Cc::RootReconciler& reconciler)
{
    auto const workingDirectoryText = workingDirectory.string();
    auto const resolvedSource = reconciler.Directory(cmd.source);
    auto const canonicalSource = Cc::CanonicalSourceToken(resolvedSource, layout, workingDirectoryText);
    if (!canonicalSource.has_value())
        return;

    // Either stream may carry the notes (clang-cl uses stdout, cl uses stderr);
    // parse both rather than guessing which compiler produced this value.
    auto includes = Cc::ParseIncludePaths(includeTextOut);
    auto const fromErr = Cc::ParseIncludePaths(includeTextErr);
    includes.insert(includes.end(), fromErr.begin(), fromErr.end());

    // GNU drivers report nothing on either stream — their dependencies go to the
    // depfile. Without this, direct mode could never populate on POSIX: every
    // compile would pay for a manifest lookup that always missed.
    if (includes.empty())
        if (auto const depText = ReadDepFile(cmd))
            includes = Cc::ParseDepFilePaths(*depText);

    // No dependency record at all means no manifest. A manifest built from the
    // source alone revalidates the source alone, so a hit against it replays an
    // object built from headers nobody re-checked: edit a header, leave the .cpp
    // untouched, and the stale object is served forever with a zero exit code.
    // That is what a GNU compile with no `-MD`/`-MF` produces — neither stream
    // carries notes and there is no depfile to read — and it is the one shape
    // where recording nothing (a permanent direct-mode miss, resolved by the
    // ordinary preprocessed key) is strictly better than recording something.
    if (includes.empty())
    {
        Note("compile reported no dependencies; not recording a direct-mode manifest");
        return;
    }

    // Reconciled here rather than trusted from the caller. The captured streams
    // arrive already reconciled, but the depfile read above comes straight off
    // disk — and resolution is memoized and idempotent, so guaranteeing this
    // function's own inputs costs a hash lookup per path.
    reconciler.All(includes);

    // The manifest points at the object's ordinary key rather than causing a second
    // copy to be stored: L1 keeps values uncompressed, so duplicating objects would
    // double RAM pressure (27 GB -> 54 GB measured) where compression cannot help.
    //
    // The TU's own source is a field of its own rather than another element of the
    // include list: /showIncludes never names the primary source, and a GNU
    // depfile's rule deliberately excludes its own target, so a manifest without it
    // revalidates every header and not the file being compiled (issue #49 /
    // issue #51). BuildManifest refuses outright when it cannot record the TU,
    // which is what makes that a precondition rather than a comment here.
    auto const reported = includes.size();
    auto const manifest = Cc::BuildManifest({ .sourcePath = resolvedSource,
                                              .includePaths = std::move(includes),
                                              .workingDirectory = workingDirectoryText,
                                              .toolchainStamp = toolchainStamp,
                                              .objectKey = std::string { objectKeyForPointer } },
                                            layout);
    if (!manifest.has_value())
    {
        if (invocation.verbose)
            std::cerr << "fastcache-cc: manifest not built (uncanonicalizable source or include)\n";
        return;
    }

    // Both counts, for the reason the key's dependency-set note gives them: the
    // pair is what distinguishes "this TU only includes toolchain headers" (fine,
    // the stamp covers them) from "every path the driver reported was filtered
    // out" — which is the shape of a misconfigured root, and which the recorded
    // manifest cannot report on its own because it still validates.
    Note(std::format(
        "manifest: {} entries from {} reported dependency path(s) plus the source", manifest->entries.size(), reported));

    auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);

    // Wrap the manifest as a text-region-free compile value so the daemon's STORE
    // (which decodes every payload as one) accepts it.
    auto const manifestEncoded = Cc::EncodeManifest(*manifest);
    CompileValue manifestValue;
    manifestValue.objectBlob.assign(reinterpret_cast<std::byte const*>(manifestEncoded.data()),
                                    reinterpret_cast<std::byte const*>(manifestEncoded.data()) + manifestEncoded.size());
    auto const manifestFrame = EncodeCompileValue(manifestValue);
    StoreRaw(cfg.addr,
             cfg,
             manifestKey,
             std::string_view { reinterpret_cast<char const*>(manifestFrame.data()), manifestFrame.size() });
    if (invocation.verbose)
        std::cerr << "fastcache-cc: MANIFEST stored key=" << manifestKey << " entries=" << manifest->entries.size() << '\n';
}

/// DIRECT MODE. Preprocessing a translation unit to derive its key costs ~1.4 s
/// on a large codebase (it expands ~25 MB of headers); re-hashing the project
/// headers a previous compile recorded costs ~18 ms. So before preprocessing,
/// try to reach the object through a stored manifest instead.
///
/// Any failure here just means we preprocess as before: direct mode never
/// decides a compile is uncacheable, only that it cannot shortcut.
///
/// @param cfg              Launcher config.
/// @param cmd              The parsed compile command.
/// @param layout           This machine's source-root / build-tree layout.
/// @param workingDirectory The directory this compile runs in; the source path is
///                         resolved against it before it becomes a key input.
/// @param relativizedArgs  The command line with checkout-rooted paths tokenized.
/// @param toolchainStamp   The compiler identity folded into the manifest key.
/// @param reconciler       Translates a driver's spelling into this build's.
/// @return The exit code if the object was served, nullopt to keep going.
[[nodiscard]] std::optional<int> TryDirectMode(Config const& cfg,
                                               Cc::ParsedCommand const& cmd,
                                               PathCanon::Layout const& layout,
                                               std::filesystem::path const& workingDirectory,
                                               std::vector<std::string> const& relativizedArgs,
                                               std::string const& toolchainStamp,
                                               Cc::RootReconciler& reconciler)
{
    auto const directStarted = std::chrono::steady_clock::now();
    // Every early return records how long the attempt took, so the statistics
    // show the cost of a direct-mode miss as well as a direct-mode hit.
    auto const giveUp = [directStarted]() -> std::optional<int> {
        invocation.directMs = MsSince(directStarted);
        return std::nullopt;
    };

    // The same derivation RecordManifest uses, so the lookup and the recording
    // sides cannot spell this token differently — which for a relatively-named
    // source they did, one resolving it and the other not. The reconciliation is
    // part of that derivation: a driver's spelling has to collapse to the build's
    // on both sides, or the two tokens differ again by another route.
    auto const canonicalSource =
        Cc::CanonicalSourceToken(reconciler.Directory(cmd.source), layout, workingDirectory.string());
    if (!canonicalSource.has_value())
        return giveUp();

    auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);
    auto const manifestBytes = FetchRaw(cfg, manifestKey);
    if (!manifestBytes.has_value())
        return giveUp();

    // Unwrap the compile-value envelope the manifest was stored in.
    auto const envelope = DecodeCompileValue(*manifestBytes);
    auto const manifestSpan =
        envelope.has_value() ? std::span<std::byte const> { envelope->objectBlob } : std::span<std::byte const> {};
    auto const manifest =
        Cc::DecodeManifest(std::string_view { reinterpret_cast<char const*>(manifestSpan.data()), manifestSpan.size() });
    if (!manifest.has_value() || manifest->objectKey.empty() || !Cc::ValidateManifest(*manifest, layout, toolchainStamp))
        return giveUp();

    invocation.directMs = MsSince(directStarted);
    // Follow the manifest's pointer to the object, which is stored exactly once
    // under its ordinary preprocessed key.
    auto served = TryServeFromCache(cfg, cmd, manifest->objectKey, layout, workingDirectory);
    if (served.has_value())
        invocation.directHit = true;
    return served;
}

/// Have a worker compile this translation unit, and leave the tree exactly as a
/// local compile would have left it.
///
/// Returns a `CompileRun` so the whole miss path below is unchanged and cannot tell
/// a dispatched compile from a local one: the object is on disk at `cmd.objPath`,
/// the dependency record is written, and the streams are in hand. One path to the
/// STORE, the manifest and the statistics, rather than two that can diverge.
///
/// ## The dependency record is written HERE, and that is not optional
///
/// A worker compiles preprocessed text, which has no `#include` left in it, so its
/// compiler reports no dependencies -- there are none to report. The build system
/// asked for them anyway, and an object with no dependency record makes Ninja stop
/// rebuilding this translation unit when its headers change: a wrong build with a
/// zero exit code that persists until someone cleans. It is the same defect the hit
/// path guards against in as many words.
///
/// The client already has the answer -- its own preprocess probe opened every one
/// of those headers to compute the key -- so it writes the record rather than the
/// worker inventing one. See DependencyOutput.hpp.
///
/// ## A remote failure is retried locally
///
/// When the remote compiler exits non-zero this returns nullopt, so the caller runs
/// the compile locally and reports THAT. A compile can fail because of the code, in
/// which case both runs agree and the user sees the same diagnostics; or because of
/// something about the worker, in which case the local run succeeds and the build
/// is right. Reporting the remote failure directly would make a bad node fail
/// builds that are fine, which is the failure mode that gets distribution turned
/// off and never turned back on. The cost is one wasted remote attempt.
///
/// @param cfg Launcher config (scheduler endpoint, credential, timeout).
/// @param cmd The parsed compile command.
/// @param argv The original full invocation.
/// @param key The object key, for duplicate suppression at the scheduler.
/// @param toolchainStamp The compiler's version banner, as the cache key uses it.
/// @param dependencyPaths What the key's probe reported this TU depends on.
/// @return A run to continue with, or nullopt to compile locally.
[[nodiscard]] std::optional<Cc::CompileRun> TryRemoteCompile(Config const& cfg,
                                                             Cc::ParsedCommand const& cmd,
                                                             std::span<std::string const> argv,
                                                             std::string_view key,
                                                             std::string_view toolchainStamp,
                                                             std::vector<std::string> const& dependencyPaths)
{
    // Refused before anything is sent when the command line carries something this
    // launcher cannot account for. See RemoteCompileArgs: refusing costs one local
    // compile, where stripping an unrecognised argument would change the generated
    // code and hand back an object nobody asked for.
    auto const args = Cc::RemoteCompileArgs(cmd, argv);
    if (!args.has_value())
    {
        Note(std::format("not dispatchable ({}); compiling locally", args.error()));
        return std::nullopt;
    }

    // Preprocessed again, with `#line` markers this time. The key's text has them
    // suppressed so no checkout path reaches the key; a worker needs them, because
    // they are what marks system-header lines as system-header lines. Without that
    // the remote compile re-reports every warning inside libc++ or the CRT, which
    // under `-Werror` is a failed compile rather than noise.
    auto const preprocessRun = RunCaptureSplit(Cc::DispatchPreprocessCommand(cmd, argv));
    if (preprocessRun.exitCode != 0)
    {
        Note("dispatch preprocess failed; compiling locally");
        return std::nullopt;
    }

    // The DISPATCH identity, which is not the cache key's. The key folds in the
    // compiler's `--version` banner, which is enough for a cache -- a wrong answer
    // there is a miss the replay guard can still backstop. Distribution has no such
    // backstop: two machines can print an identical banner while resolving
    // different libstdc++ headers, and the object that comes back would be wrong
    // rather than merely unhelpful. So a worker is matched on a digest of the whole
    // include tree instead.
    //
    // Computed HERE rather than beside the stamp, so it stays on the miss-and-
    // dispatch-configured path only: it is a cache read in the steady state, but
    // several seconds the first time a machine sees a toolchain, and a build that
    // never dispatches must not pay that at all.
    auto const fingerprint =
        Cc::CachedToolchainFingerprint(ProcessRunner(), cmd.compiler, toolchainStamp, Cc::DriverOf(cmd.flavor));

    auto const dialer = Cc::MakeTcpDialer(cfg.connectTimeout, cfg.ioTimeout);
    auto const outcome = Cc::Dispatch(*dialer,
                                      Cc::DispatchRequest { .schedulerEndpoint = cfg.schedulerAddr,
                                                            .fingerprint = fingerprint,
                                                            .objectKey = key,
                                                            .args = *args,
                                                            .preprocessed = preprocessRun.out,
                                                            .sourceName = cmd.source },
                                      cfg.credential);
    if (!outcome.Ran())
    {
        // Declined and Unavailable are both ordinary and both end the same way. The
        // reason is named because "distribution stopped working" is otherwise a
        // whole investigation, and the answer is one line.
        Note(std::format("not dispatched ({}); compiling locally", outcome.detail));
        return std::nullopt;
    }
    if (outcome.exitCode != 0)
    {
        Note(std::format(
            "worker {} reported exit {}; recompiling locally to confirm", outcome.workerEndpoint, outcome.exitCode));
        return std::nullopt;
    }

    // The object first: everything after it is a record ABOUT this object, and
    // writing those first would leave a dependency record describing a file that is
    // not there if the write fails.
    if (!WriteFileBytes(cmd.objPath, outcome.object))
    {
        Note("could not write the dispatched object; compiling locally");
        return std::nullopt;
    }

    // The dependency record, in whichever form this build asked for. Both can be
    // wanted at once, and neither is inferred from the other.
    Cc::CompileRun run { .exitCode = 0, .out = outcome.stdoutText, .err = outcome.stderrText };
    if (!cmd.depPath.empty() && !WriteDepFile(cmd.depPath, Cc::RenderDepFile(cmd.objPath, dependencyPaths)))
    {
        Note("could not write the depfile for a dispatched compile; compiling locally");
        return std::nullopt;
    }
    if (cmd.wantShowIncludes)
        // Prepended, not appended: `cl` emits its notes before its diagnostics, and
        // the stored value's region ordering is what a later hit replays verbatim.
        run.out = Cc::RenderShowIncludes(dependencyPaths) + run.out;

    Note(std::format("DISPATCHED to {} key={}", outcome.workerEndpoint, key));
    return run;
}

/// Try to serve `cmd` from the cache; returns the process exit code if handled
/// (hit or miss-then-stored), or std::nullopt to signal "fall back to a plain
/// real compile" (any cache error).
[[nodiscard]] std::optional<int> RunCached(Config const& cfg,
                                           Cc::ParsedCommand const& cmd,
                                           std::span<std::string const> argv)
{
    if (cfg.addr.empty() || cfg.srcRoot.empty() || cfg.buildTree.empty())
    {
        Warn("missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR");
        return std::nullopt;
    }

    // One layout, and it is the build system's own spelling — every consumer of it
    // below either tokenizes against it or emits from it, and both want that form.
    // The reconciler holds the resolved spellings and is the only thing that sees
    // them; nothing downstream needs to know they exist. Taken FROM the reconciler
    // rather than built beside it, so the two cannot come to disagree about a
    // trailing separator or anything else the constructor normalizes.
    Cc::RootReconciler reconciler { cfg.srcRoot, cfg.buildTree, PathResolver() };
    PathCanon::Layout const& layout = reconciler.Layout();

    // Directory-flavoured, because an argument's own last component can be the
    // aliased one — an `-I` pointing at a symlinked include directory is the
    // ordinary case — and there are few enough arguments that resolving each
    // completely costs nothing measurable.
    auto const relativizedArgs =
        Cc::RelativizeArgs(argv.subspan(1), cfg.srcRoot, cfg.buildTree, [&reconciler](std::string_view path) {
            return reconciler.Directory(path);
        });
    auto const toolchainStamp = CompilerId(cmd.compiler);

    // Read once here rather than wherever a relative path needs placing. Three
    // consumers need exactly this value — the replay guard, the manifest's own
    // recording, and the manifest key's source token — and reading it three times
    // is three chances for them to disagree about what a relative path means.
    //
    // current_path() through its error_code overload, and "." when even that
    // fails: the throwing overload would abort a launcher whose whole contract is
    // that a cache problem never breaks a build, and "." resolves a relative
    // dependency exactly as the compiler's own working directory would.
    //
    // Then re-spelled in the layout's vocabulary. getcwd(3) answers with the
    // kernel's RESOLVED path, so a build under a symlinked prefix (macOS `/tmp`,
    // any symlinked `/home`) reports a working directory that shares no string
    // prefix with the roots it is actually inside — and every root test here is a
    // prefix comparison. See AnchorWorkingDirectory.
    std::error_code cwdError;
    auto workingDirectory = std::filesystem::current_path(cwdError);
    if (cwdError)
        workingDirectory = ".";
    workingDirectory = Cc::AnchorWorkingDirectory(workingDirectory.string(), layout);

    if (cfg.direct && !SourceReferencesVolatileMacro(cmd.source))
        if (auto served = TryDirectMode(cfg, cmd, layout, workingDirectory, relativizedArgs, toolchainStamp, reconciler))
            return served;

    auto const preprocessStarted = std::chrono::steady_clock::now();
    // Scoped, and the text MOVED through it rather than copied: the preprocessed
    // form of a real translation unit runs to several megabytes, and everything
    // below — two round trips and, on a miss, the real compiler as a child
    // process — has no use for it once the key exists. Leaving it live held that
    // much dead memory resident for exactly as long as the machine is busiest.
    std::string key;
    // Kept alive past the block below ONLY when a scheduler is configured. The
    // preprocessed form of a real translation unit runs to several megabytes, and
    // the block exists to drop it the moment the key is computed -- see below.
    // Dispatch is the one caller that still needs it afterwards, because it is
    // exactly what a worker compiles, so it pays that cost and nobody else does.
    std::string dispatchSource;
    std::vector<std::string> dispatchDependencies;
    bool const dispatchConfigured = !cfg.schedulerAddr.empty();
    {
        auto probe = Preprocess(cmd, argv, reconciler);
        if (!probe.has_value())
        {
            Warn("preprocess failed");
            return std::nullopt;
        }

        // Skip translation units that reference a time/date macro. `__TIME__` /
        // `__DATE__` / `__TIMESTAMP__` expand to a run-varying (second-granular)
        // string, so such a TU re-keys on every compile and can never hit —
        // caching it only churns the store. Preprocessing has already *expanded*
        // the macro (its name is gone from the output), so we scan the source file
        // text itself, matching sccache's refusal to cache these. Direct use in the
        // TU is the overwhelmingly common case; header-introduced use is rare and
        // its only cost is a permanent miss, never incorrectness.
        if (SourceReferencesVolatileMacro(cmd.source))
        {
            Warn("uses __TIME__/__DATE__/__TIMESTAMP__; not caching (non-deterministic)");
            return std::nullopt;
        }

        // The dependency set is reduced to its portable form before it reaches the
        // key: canonical tokens only, sorted and deduplicated, with toolchain paths
        // dropped because the compiler identity above already covers them and
        // hashing them would end cross-machine sharing outright. A relative path is
        // resolved against the compile's working directory first, so it is
        // classified by the file it names rather than by how the driver spelled it.
        // See DependencyProbe.hpp, where each half of that filter is justified.
        auto dependencies = Cc::KeyDependencySet(probe->dependencyPaths, layout, workingDirectory.string());

        // Both counts, because they answer different questions and only the pair
        // is diagnostic. An empty set means a moved header cannot re-key — the
        // property this whole input exists for, failing silently — and "the probe
        // reported nothing" (a driver that does not report on the preprocess line)
        // is a different defect from "every reported path was filtered out" (paths
        // the layout does not recognise as its own). Named for the same reason the
        // STALE HIT note names its path: otherwise this is a whole investigation.
        //
        // And the pair alone stopped being enough. `0 of M` was the fingerprint of
        // one fault (issue #66's short-name root, where nothing prefix-matches what
        // the driver echoes back) until issue #65 gave it a second (a drive-relative
        // path, which is anchored to nothing this machine can place). The reasons
        // are what tell those apart now; the counts alone told neither (issue #105).
        // Rendered by DescribeDropped rather than here, because main.cpp is in no
        // test target. Semicolon-separated from the filesystem-call count, which is
        // not a path count and must not read as another entry in the list.
        auto const dropped = Cc::DescribeDropped(dependencies);
        Note(std::format("dependency set: {} of {} reported path(s) keyed ({}{}{} filesystem call(s))",
                         dependencies.keyed.size(),
                         dependencies.Reported(),
                         dropped,
                         dropped.empty() ? "" : "; ",
                         PathResolver().FilesystemCalls()));
        NoteIfRootsDoNotDescribeCompile(cfg, cmd.source, dependencies, reconciler);

        // Non-const so the preprocessed text can be MOVED out below rather than
        // copied. It was const, and `std::move` on a const member is a silent copy
        // -- of several megabytes, on the hot path of a parallel build, while the
        // comment below claimed the opposite. clang-tidy's performance-move-const-arg
        // is what caught it.
        Cc::KeyInputs inputs {
            .compilerId = toolchainStamp,
            .preprocessed = std::move(probe->preprocessed),
            .relativizedArgs = relativizedArgs,
            .dependencyPaths = std::move(dependencies.keyed),
        };
        key = Cc::ComputeKey(inputs);

        // Moved out AFTER the key is computed, so the key path pays nothing: by
        // here ComputeKey has finished with the text and would otherwise drop it.
        if (dispatchConfigured)
        {
            dispatchSource = std::move(inputs.preprocessed);
            dispatchDependencies = std::move(probe->dependencyPaths);
        }
    }
    invocation.preprocessMs = MsSince(preprocessStarted);

    auto const cacheStarted = std::chrono::steady_clock::now();

    // FETCH.
    {
        auto const outcome = Cc::RunOneExchange(cfg.addr, Wire::EncodeFetch(key), cfg.credential, BudgetOf(cfg));
        NoteIfCredentialIgnored(outcome);
        if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        {
            Warn("fetch exchange failed");
            return std::nullopt;
        }
        if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        {
            // The daemon answered and declined. Its own code and words go into
            // the fall-back reason, so `--show-stats` names the cause instead of
            // lumping a version mismatch in with an unreachable daemon.
            Warn(Cc::DescribeOutcome(outcome));
            return std::nullopt;
        }
        if (outcome.IsHit())
        {
            auto decoded = DecodeCompileValue(outcome.value);
            if (!decoded.has_value())
            {
                Warn("fetch decoded malformed");
                return std::nullopt;
            }

            // HIT: check what the value asserts, then write the object, reproduce
            // the depfile, and replay the streams (all with paths localized).
            //
            // Reproducing the depfile is not optional: skipping it silently breaks
            // incremental builds, because Ninja/Make would see no header
            // dependencies for this TU and stop rebuilding it when they change.
            auto const materialized = MaterializeHit(cmd, *decoded, layout, workingDirectory);
            if (materialized.disposition == HitDisposition::Unusable)
            {
                Warn("could not write object on hit");
                return std::nullopt;
            }
            if (materialized.disposition == HitDisposition::Served)
            {
                invocation.valueBytes = decoded->objectBlob.size();
                invocation.cacheMs = MsSince(cacheStarted);

                // Backfill the direct-mode manifest from the hit we just served.
                //
                // Without this, direct mode could never populate on a cache that already
                // holds preprocessed-key entries: manifests would only ever be written by
                // the miss path, so a warm cache would preprocess forever. The localized
                // /showIncludes text names exactly the headers this object depends on, so
                // no compiler run is needed to record them.
                if (cfg.direct)
                    RecordManifest(cfg,
                                   cmd,
                                   layout,
                                   workingDirectory,
                                   relativizedArgs,
                                   toolchainStamp,
                                   materialized.replayOut,
                                   materialized.replayErr,
                                   key,
                                   reconciler);

                TraceOutcome("HIT", key);
                return 0;
            }
            // Stale: the object is fine but the dependency record it carries is not
            // true here, so fall through and compile for real. The STORE that follows
            // overwrites this very key with a correct one, which is what repairs the
            // entry rather than leaving it to poison every later build.
            //
            // And if that STORE is refused for any reason, the build still converges:
            // the real compiler ran, so a correct depfile is on disk regardless, and
            // the cost degrades to a permanent miss for this key.
        }
        // MISS — fall through to compile.
    }
    invocation.cacheMs = MsSince(cacheStarted);
    TraceOutcome("MISS", key);

    // MISS: try a worker first when one is configured, then the real compiler.
    //
    // A dispatched compile is shaped to look exactly like a local one -- object on
    // disk at cmd.objPath, dependency record written, streams in hand -- so
    // everything below this point is unchanged and cannot tell the difference. That
    // is deliberate: the STORE, the manifest and the statistics all have one path,
    // and a second one would be a second place for them to diverge.
    auto run = dispatchConfigured ? TryRemoteCompile(cfg, cmd, argv, key, toolchainStamp, dispatchDependencies)
                                  : std::optional<Cc::CompileRun> {};
    if (!run.has_value())
        run = RunCaptureSplit(argv);
    // Always surface the compiler's output on its true streams and its exit code.
    ReplayStreams(run->out, run->err);
    // A spawn failure reports -1, which a POSIX exit status truncates to 255 —
    // an arbitrary code no build system can interpret. Normalize it the same way
    // the fall-back path does.
    int const code = run->exitCode == -1 ? 1 : run->exitCode;
    if (code != 0)
        return code; // do not cache a failed compile

    auto const objectBytes = ReadFileBytes(cmd.objPath);
    if (!objectBytes.has_value())
    {
        // The compile itself succeeded, so this stays a MISS: only the caching
        // of it failed. Note() rather than Warn() keeps the recorded outcome.
        Note("object missing after compile; not caching");
        return code;
    }

    CompileValue value;
    value.objectBlob = *objectBytes;
    // Two regions, one per stream, in a fixed order (0=stdout, 1=stderr) so the
    // hit path replays each on its correct channel. clang-cl emits
    // /showIncludes on stdout, cl on stderr — we tag BOTH with the ShowIncludes
    // grammar so whichever stream carries include notes gets canonicalized; a
    // non-matching line in either region is preserved verbatim.
    //
    // Both are reconciled first, and only the STORED copy is: ReplayStreams above
    // has already passed the compiler's own bytes through untouched. The daemon
    // canonicalizes a STORE against the roots this launcher sends, which are the
    // AS-GIVEN ones — so a region still carrying a spelling the driver resolved
    // some other way would match neither root and be stored with this machine's
    // absolute paths in it. Reconciling here is what puts the two in one spelling;
    // sending the resolved roots instead would be the other way to do it, and it
    // is the way that breaks the replayed depfile (see RootReconciler).
    auto const includeTextOut = reconciler.Region(run->out, IncludeGrammar());
    auto const includeTextErr = reconciler.Region(run->err, IncludeGrammar());
    value.textRegions.push_back({ .grammar = IncludeGrammar(), .bytes = includeTextOut });
    value.textRegions.push_back({ .grammar = IncludeGrammar(), .bytes = includeTextErr });

    // Region 2, when present, is the GNU depfile the compile just wrote. It is
    // tagged with the depfile grammar so the daemon canonicalizes the header
    // paths inside it — a depfile full of this machine's absolute paths would be
    // useless (and wrong) when replayed on another checkout.
    if (auto const depText = ReadDepFile(cmd))
        value.textRegions.push_back(
            { .grammar = PathCanon::Grammar::GccDepfile,
              .bytes = reconciler.Region(*depText, PathCanon::Grammar::GccDepfile, Cc::ParseDepFileTargets(*depText)) });

    auto const encoded = EncodeCompileValue(value);
    if (!Cc::IsStorableSize(encoded.size(), cfg.maxStoreBytes))
    {
        // Still a MISS, and still a successful compile: the object on disk is
        // the real one. Only the caching of it is declined, and declined here
        // rather than by the daemon so the build does not spend the transfer to
        // be refused on every rebuild of this translation unit.
        Note(std::format(
            "value {} bytes exceeds {}={}; not caching", encoded.size(), Cc::EnvName::MaxStoreBytes, cfg.maxStoreBytes));
        return code;
    }

    // Best-effort store: a failure here must never fail the build (the compile
    // already succeeded). Surface the outcome under verbose so store rejections
    // (e.g. a value over the server's --storage-max-value) are diagnosable — and
    // now with the daemon's own reason, which the bare acknowledgement byte the
    // old framing carried could never express. An unreachable daemon lands here as
    // a transport failure, which reads the same way it always did.
    auto const outcome =
        Cc::RunOneExchange(cfg.addr,
                           Wire::EncodeStore(Wire::StoreRequest { .key = key,
                                                                  .prefetchGroup = cfg.prefetchGroup,
                                                                  .srcRoot = cfg.srcRoot,
                                                                  .buildTree = cfg.buildTree,
                                                                  .value = std::span<std::byte const> { encoded } }),
                           cfg.credential,
                           BudgetOf(cfg));
    NoteIfCredentialIgnored(outcome);
    if (outcome.IsHit())
        Note(std::format("STORED key={} bytes={}", key, encoded.size()));
    else if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        Note(std::format("STORE key={} bytes={} {}", key, encoded.size(), Cc::DescribeOutcome(outcome)));
    else
        Note(std::format("STORE exchange failed key={}", key));

    // Record the direct-mode manifest so the NEXT compile of this TU can reach the
    // object without preprocessing. The include set comes from the /showIncludes
    // output the compile just produced, so this costs no extra compiler run.
    //
    // The manifest holds a POINTER to the object's ordinary key, not a second copy
    // of the object: the two keys answer different questions (one from preprocessed
    // text, one from header hashes), but storing the bytes under both would double
    // the cached volume, and because L1 keeps values uncompressed that doubling
    // lands on RAM where compression cannot help. A direct hit therefore costs one
    // extra fetch to follow the pointer — see DirectManifest::objectKey.
    if (cfg.direct)
        RecordManifest(cfg,
                       cmd,
                       layout,
                       workingDirectory,
                       relativizedArgs,
                       toolchainStamp,
                       includeTextOut,
                       includeTextErr,
                       key,
                       reconciler);
    return code;
}

} // namespace

/// Delete the statistics log, reporting the outcome. @return Process exit code.
[[nodiscard]] int ClearStats()
{
    auto const path = Cc::LogPath();
    if (Cc::ResetLog())
    {
        std::cout << "fastcache-cc: statistics cleared" << (path.empty() ? "" : " (") << path << (path.empty() ? "" : ")")
                  << '\n';
        return 0;
    }
    std::cerr << "fastcache-cc: could not clear statistics log";
    if (!path.empty())
        std::cerr << " (" << path << ')';
    std::cerr << '\n';
    return 1;
}

/// Print the statistics report (`--show-stats`) and return the process exit code.
/// @param groupFilter Restrict the report to this prefetch group; empty reports all.
[[nodiscard]] int RunStatsReport(std::string_view groupFilter)
{
    // The color decision is made here, not inside Stats.cpp, so that module
    // stays free of ambient probes -- the same split --help already uses.
    std::cout << Cc::FormatReport(
        groupFilter, FastCache::StdoutSupportsColor() ? FastCache::UsageColor::Colored : FastCache::UsageColor::Plain);
    return 0;
}

/// Default location for `--html-stats`'s dashboard when `--out` names none:
/// alongside the statistics log itself, so both live under the same per-user
/// state directory rather than the current working directory (which for a
/// launcher invoked from inside a build could be anywhere).
/// @return The default report path, or empty when there is no state directory.
[[nodiscard]] std::string DefaultHtmlReportPath()
{
    auto const logPath = Cc::LogPath();
    if (logPath.empty())
        return {};
    return (std::filesystem::path { logPath }.parent_path() / "report.html").string();
}

/// Render the HTML dashboard (`--html-stats`) and write it to disk.
/// @param groupFilter Restrict the report to this prefetch group; empty reports all.
/// @param outputPath Where to write it; empty means DefaultHtmlReportPath().
/// @return Process exit code.
[[nodiscard]] int RunHtmlStatsReport(std::string_view groupFilter, std::string_view outputPath)
{
    auto const report = Cc::FormatHtmlReport(groupFilter);

    // The empty-log/empty-prefetch group case returns the same short plain-text
    // message FormatReport does (see FormatHtmlReport's doc comment) --
    // printed rather than written to a file, matching --show-stats's own
    // behaviour for the same condition.
    if (!report.starts_with("<!doctype html>"))
    {
        std::cout << report;
        return 0;
    }

    std::string const destination { !outputPath.empty() ? std::string { outputPath } : DefaultHtmlReportPath() };
    if (destination.empty())
    {
        std::cerr << "fastcache-cc: no state directory available to write the dashboard to; pass --out.\n";
        return 1;
    }

    std::error_code ec;
    if (auto const parent = std::filesystem::path { destination }.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent, ec);

    std::ofstream out { destination, std::ios::binary | std::ios::trunc };
    if (!out)
    {
        std::cerr << "fastcache-cc: could not write dashboard to '" << destination << "'.\n";
        return 1;
    }
    out << report;
    if (!out)
    {
        std::cerr << "fastcache-cc: could not write dashboard to '" << destination << "'.\n";
        return 1;
    }

    std::cout << "fastcache-cc: dashboard written to " << destination << '\n';
    return 0;
}

/// Report a bad command line, then the usage text, on stderr.
/// @param diagnostic What was wrong with the arguments.
/// @return Process exit code.
[[nodiscard]] int ReportUsageError(std::string_view diagnostic)
{
    // Deliberately plain: StdoutSupportsColor() probes stdout, and help on
    // stderr is usually captured by a build system, so colorizing it from
    // stdout's state would write escapes into a log file.
    std::cerr << "fastcache-cc: " << diagnostic << "\n\n" << Cc::HelpText();
    return 2;
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) // argv[0] is fastcache-cc itself; drop it
        args.emplace_back(argv[i]);

    // Dispatch is driven by the flag table in LauncherCli.cpp, which also renders
    // the help text, so an accepted flag is necessarily a documented one.
    auto const command = Cc::ParseTopLevel(std::span<std::string const> { args });
    switch (command.action)
    {
        // No arguments is a usage error (exit 2); an explicit --help is a
        // successful query and prints to stdout so it can be paged or redirected.
        case Cc::Action::NoArguments:
            std::cerr << Cc::HelpText();
            return 2;
        case Cc::Action::UsageError:
            return ReportUsageError(command.diagnostic);
        case Cc::Action::Help:
            // The color decision is made here rather than inside LauncherCli so
            // the module stays free of ambient probes. On Windows the call also
            // enables virtual-terminal processing as a side effect, so it has to
            // happen before anything is written.
            std::cout << Cc::HelpText(FastCache::StdoutSupportsColor() ? FastCache::UsageColor::Colored
                                                                       : FastCache::UsageColor::Plain);
            return 0;
        case Cc::Action::Version:
            std::cout << "fastcache-cc " << FASTCACHE_CC_VERSION << '\n';
            return 0;
        case Cc::Action::ShowStats:
            return RunStatsReport(command.groupFilter);
        case Cc::Action::HtmlStats:
            return RunHtmlStatsReport(command.groupFilter, command.outputPath);
        case Cc::Action::ZeroStats:
            return ClearStats();
        case Cc::Action::PrintFingerprint:
            return PrintToolchainFingerprint(command.compiler);
        // Stats sub-options, never returned as a top-level action. Handled
        // explicitly so the switch stays exhaustive without silently treating
        // "--prefetch-group"/"--out" as a compiler to spawn.
        case Cc::Action::PrefetchGroup:
            return ReportUsageError("--prefetch-group is only valid after --show-stats or --html-stats");
        case Cc::Action::OutputPath:
            return ReportUsageError("--out is only valid after --html-stats");
        case Cc::Action::Compile:
            break;
    }

    Config const cfg = LoadConfig();
    invocation.verbose = cfg.verbose;

    auto const cmd = Cc::ParseCommand(std::span<std::string const> { args });
    if (!cmd.parsedOk)
    {
        // Not a cacheable compile (a link or preprocess-only step). Recording it
        // would dilute the hit rate with lines that were never candidates.
        //
        // One of those reasons is worth naming, because it is the one that looks
        // like a defect from outside: a compile that also writes a BMI or a
        // precompiled header is stepped over deliberately, and an operator watching
        // a module-using build get no hits at all deserves to be told why rather
        // than left to conclude the cache is broken.
        if (cmd.sideArtefact)
            Note("the compile writes a second artefact (a BMI or a precompiled header) "
                 "that a cache hit cannot reproduce; not cached");
        return RunPassthrough(std::span<std::string const> { args });
    }

    auto const started = std::chrono::steady_clock::now();
    auto const handled = RunCached(cfg, cmd, std::span<std::string const> { args });
    int const code = handled.has_value() ? *handled : RunPassthrough(std::span<std::string const> { args });

    if (cfg.stats)
    {
        auto const elapsed = std::chrono::steady_clock::now() - started;
        Cc::AppendRecord({
            .outcome = invocation.outcome,
            .prefetchGroup = cfg.prefetchGroup,
            .source = cmd.source,
            .valueBytes = invocation.valueBytes,
            .elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
            .detail = invocation.outcomeDetail,
            .preprocessMs = invocation.preprocessMs,
            .cacheMs = invocation.cacheMs,
            .directMs = invocation.directMs,
            .directHit = invocation.directHit,
        });
    }
    return code;
}
