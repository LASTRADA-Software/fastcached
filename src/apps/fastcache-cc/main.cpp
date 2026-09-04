// SPDX-License-Identifier: Apache-2.0
//
// fastcache-cc — an sccache-style compiler launcher over the fastcached 0xFC
// compile-cache protocol.
//
// Invoked as `fastcache-cc <compiler> <args...>` (e.g. via
// CMAKE_CXX_COMPILER_LAUNCHER). On a cache HIT it reproduces the object file
// and replays the compiler's stdout/stderr (localizing /showIncludes header
// paths to this machine's layout) so the build behaves as if it compiled. On a
// MISS it has a worker compile it when a scheduler is configured and otherwise
// runs the real compiler, stores the canonicalized result, and passes the output
// through. A cache error does not end the invocation — the compile is still
// dispatched, because a cache and a compile fleet are two services and an answer
// about one is not an answer about the other — and (when FASTCACHE_VERBOSE is
// set) it prints a one-line diagnostic. The build never breaks because the cache
// is unavailable, and it does not stop distributing either.
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
//   FASTCACHE_TIMEOUT_MS deadline in ms for one WHOLE cache exchange, request to
//                        last byte of the reply (default 10000; 0 = unbounded)
//   FASTCACHE_DISPATCH_TIMEOUT_MS
//                        deadline in ms for one whole COMPILE exchange with a
//                        worker (default 600000; 0 = unbounded). Separate from the
//                        one above because a compile is bounded by how long a
//                        compiler runs, not by a round trip (#223).
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
#include "FileBytes.hpp"
#include "HitVerification.hpp"
#include "IProcessRunner.hpp"
#include "LauncherCli.hpp"
#include "ParallelFor.hpp"
#include "PathResolve.hpp"
#include "ReactorExchange.hpp"
#include "ReplayGuard.hpp"
#include "RootReconciler.hpp"
#include "Stats.hpp"
#include "ToolchainHost.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Net/TcpClient.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/NarrowText.hpp>
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

/// Default deadline for one whole CACHE exchange, overridable with
/// FASTCACHE_TIMEOUT_MS.
///
/// Impatient on purpose: a daemon answers a FETCH or a STORE out of memory, so one
/// that has not finished in ten seconds is one this build is better off without.
/// That is what keeps a miss cheap. It is deliberately NOT what bounds a remote
/// compile -- see `Cc::DefaultDispatchTotal` and issue #223.
///
/// Taken FROM `ExchangeBudget`'s own default rather than restated beside it: that
/// default is what `DispatchBudgets::control` and every test that constructs a
/// budget run under, so a number changed here and not there would leave the whole
/// suite asserting the old policy with nothing failing.
constexpr std::chrono::milliseconds DefaultIoTimeout = Cc::ExchangeBudget {}.total;

/// Default ceiling on OPENING a connection, name resolution included.
///
/// A second rather than the I/O timeout's ten, because they bound different
/// things: a cache that has not accepted within a second is one this build is
/// better off without, and a name lookup that hangs would otherwise stall every
/// translation unit with nothing to say why. They used to be one value passed
/// twice.
/// Taken from `ExchangeBudget`'s own default, for the reason above.
constexpr std::chrono::milliseconds DefaultConnectTimeout = Cc::ExchangeBudget {}.connect;

struct Config
{
    std::string addr;
    std::string srcRoot;
    std::string buildTree;
    std::string prefetchGroup { "default" };
    bool verbose { false };
    bool stats { true };  ///< Record each invocation to the per-user log.
    bool direct { true }; ///< Try the manifest shortcut before preprocessing.

    /// Verify one hit in every this many, or `VerificationOff` to verify none.
    ///
    /// Off by default and deliberately so: a verified hit costs a whole compile, so
    /// this is for CI, a nightly, or somebody reproducing a report -- never for every
    /// build (#423).
    unsigned verifyRate { Cc::VerificationOff };
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
    /// Deadline for one whole exchange with the daemon or the scheduler. The
    /// default keeps a wedged peer from hanging a build while staying far
    /// above any healthy round-trip, including multi-megabyte objects.
    std::chrono::milliseconds ioTimeout { DefaultIoTimeout };
    /// Deadline for one whole COMPILE exchange with a worker, which is bounded by
    /// how long a compiler runs rather than by a round trip. Separate from
    /// `ioTimeout` because the two are different shapes of conversation and one
    /// number served neither (#223).
    std::chrono::milliseconds dispatchTimeout { Cc::DefaultDispatchTotal };

    /// How long a dispatched compile may go SILENT before this client gives up.
    ///
    /// Separate from `dispatchTimeout` because they bound different things: that one
    /// is how slow a compile may legitimately be, this one is how long a worker may
    /// say nothing. Only the second can be short, and only since the worker pulses
    /// (#245).
    std::chrono::milliseconds dispatchIdle { Cc::DefaultDispatchIdle };
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
    // `--listen-node` defaults to the same address for exactly that reason. SET
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
    c.verifyRate = Cc::ParseVerificationRate(EnvOr(Cc::EnvName::Verify, ""));
    c.ioTimeout = EnvMillis(Cc::EnvName::TimeoutMs, DefaultIoTimeout);
    c.dispatchTimeout = EnvMillis(Cc::EnvName::DispatchTimeoutMs, Cc::DefaultDispatchTotal);
    c.dispatchIdle = EnvMillis(Cc::EnvName::DispatchIdleMs, Cc::DefaultDispatchIdle);
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

/// Whether this launcher has a fleet to ask at all.
///
/// One predicate with one spelling, because two call sites ask it and they must
/// agree: the miss path uses it to decide whether to attempt a dispatch, and the
/// statistics seed uses it to decide between "no scheduler" and "not attempted".
/// Answered differently, a launcher would either report a fleet it never had or
/// stay silent about one it did.
/// @param cfg The launcher configuration.
/// @return True when a scheduler endpoint is configured.
[[nodiscard]] bool DispatchConfigured(Config const& cfg) noexcept
{
    return !cfg.schedulerAddr.empty();
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
    // The three byte-wide members lead, in one run. Not reading order: a byte-wide
    // member between two 8-aligned ones costs seven bytes of padding, which is this
    // tree's layout rule and the one `DispatchRow` states for itself a file away.
    bool verbose = false; ///< FASTCACHE_VERBOSE; gates every diagnostic here.

    Cc::Outcome outcome = Cc::Outcome::Unavailable; ///< Hit / Miss / Uncacheable / Unavailable.

    /// What this invocation did about DISTRIBUTION — a second axis, never a
    /// refinement of `outcome` above. A dispatch failure and a cache failure are two
    /// facts about one compile and an operator fixes them in two different places;
    /// see `Cc::DispatchOutcome`, and #427 for what recording only the first cost.
    ///
    /// Seeded in `main()` from whether a scheduler is configured at all, so a
    /// launcher that never distributes says *no fleet* rather than defaulting into
    /// a claim about one. `TryRemoteCompile` overwrites it on every path out.
    Cc::DispatchOutcome dispatch = Cc::DispatchOutcome::Unknown;

    /// Fall-back reason. Empty on a hit; usually empty on a miss, but see
    /// `ReportCrossedReply` for the one miss that carries one.
    std::string outcomeDetail;
    /// Why distribution did not help, under `RecordFallback`'s fixed-string rule.
    /// Empty when there is nothing to explain.
    std::string dispatchDetail;
    std::uint64_t valueBytes = 0; ///< Cached payload size; 0 when nothing moved.

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

/// The fixed `--show-stats` reason for a stored value this build could not decode.
///
/// Two reasons rather than one, for the same argument `DescribeOutcome` makes about
/// a daemon's refusal words: a value written under another generation is not a
/// damaged one, and an operator does different things about them. A mixed-version
/// fleet is a rolling upgrade in progress and ends by itself, while a malformed
/// value is a defect somebody has to look at — and a fleet is permanently
/// mid-upgrade, so this is the ordinary case rather than the exotic one (#483).
/// Lumping them together makes the tally say "the cache is broken" for a cache that
/// is merely being upgraded.
///
/// Both are FIXED strings under `RecordFallback`'s rule. Which generations were
/// involved varies per compile, so it goes in a `Note` beside the call rather than
/// into a tally that would then hold a row per invocation.
///
/// The classification is `IsForeignGeneration`, never a comparison against an error
/// code spelled out here: the decoder owns which refusal means what, and a launcher
/// restating it is the second place for that rule to drift.
///
/// @param error What `DecodeCompileValue` refused with.
/// @return The reason to record.
[[nodiscard]] std::string_view DecodeFailureReason(ProtocolError const& error) noexcept
{
    return IsForeignGeneration(error) ? "fetch decoded another generation's value" : "fetch decoded malformed";
}

/// Say, under `FASTCACHE_VERBOSE`, that a stored value would not decode.
///
/// A rolling upgrade otherwise presents as an endlessly cold cache with no
/// diagnostic, which is the shape this wire has already recorded paying for once.
/// The decode error's own context names both generations, so nothing here has to
/// restate them.
///
/// @param what  Which fetch this was, so two sites are told apart in a build log.
/// @param error What `DecodeCompileValue` refused with.
void NoteUndecodableValue(std::string_view what, ProtocolError const& error)
{
    Note(std::format("{} could not be decoded ({})", what, error.context));
}

/// The ways the cache can fail to serve a compile, as far as anybody outside this
/// file can tell them apart.
///
/// A row per kind BESIDE the three reporting functions, not instead of them, and
/// the split is deliberate: the row carries the three facts that have to move
/// together and used to be paired by hand, while the wrapper carries the one thing
/// a table cannot -- its RETURN TYPE, which is what stops a call site announcing
/// one continuation and taking the other. A fall-back reason distinguishes "deliberately not cacheable" from "the
/// cache let us down" — an operator acts on the second and cannot act on the first,
/// so `--show-stats` separates them — and the sentence has to say the same thing
/// the outcome does at both ends of it. "cache unavailable" is a claim about the
/// cache and is simply untrue of a refusal, which is the "a refusal reported under
/// the wrong reason sends an operator to fix something that was never wrong" rule
/// this tree records elsewhere; and "running real compiler" is a claim about what
/// happens next, which stopped being true of a cache failure at issue #236.
///
/// The classification used to be derived by sniffing the reason text for a
/// `uses __TIME__` prefix, which cannot survive a second deliberate reason: the
/// next one is counted as a cache failure, inflating the very bucket somebody
/// investigates.
enum class Fallback : std::uint8_t
{
    /// The cache let us down and the cache flow ends here.
    Unavailable,
    /// The cache let us down and the invocation carries on — a worker may still
    /// compile this, and the local compiler is only what happens if none does.
    UnavailableCarryOn,
    /// This translation unit is one the launcher deliberately will not cache.
    Uncacheable,
    Last
};

/// How one fall-back kind is reported and recorded.
///
/// The two byte-wide members sit together at the end rather than in reading order,
/// which is this tree's layout rule wherever a struct mixes them with pointers.
struct FallbackRow
{
    std::string_view leadIn;       ///< What kind of non-answer this is.
    std::string_view continuation; ///< What happens next, ending the same line.
    Fallback kind {};              ///< Which case this row describes.
    Cc::Outcome outcome {};        ///< How `--show-stats` buckets it.
};

constexpr EnumTable<Fallback, FallbackRow> FallbackTable { {
    FallbackRow { .leadIn = "cache unavailable",
                  .continuation = "running real compiler",
                  .kind = Fallback::Unavailable,
                  .outcome = Cc::Outcome::Unavailable },
    FallbackRow { .leadIn = "cache unavailable",
                  .continuation = "compiling this translation unit anyway",
                  .kind = Fallback::UnavailableCarryOn,
                  .outcome = Cc::Outcome::Unavailable },
    FallbackRow { .leadIn = "not caching",
                  .continuation = "running real compiler",
                  .kind = Fallback::Uncacheable,
                  .outcome = Cc::Outcome::Uncacheable },
} };
static_assert(RowsInEnumeratorOrder(FallbackTable, &FallbackRow::kind),
              "FallbackTable must hold exactly one row per Fallback kind, in enumerator order");

/// @param kind The fall-back kind. @return Its row.
[[nodiscard]] constexpr FallbackRow const& RowFor(Fallback kind) noexcept
{
    return FallbackTable[static_cast<std::size_t>(kind)];
}

/// Record that the cache did not serve this compile, and say why.
///
/// OVERWRITES the recorded outcome, so it is only for a compile the cache has not
/// answered for. Once a HIT or MISS has been traced, use Note() instead.
///
/// @param kind   Which fall-back this is; its row supplies everything but the reason.
/// @param reason The fall-back reason, recorded as the statistics detail. A FIXED
///               string wherever one will do: `--show-stats` tallies these, so one
///               carrying a path or a key produces a row per compile instead of a
///               row per cause, and variable detail goes in a Note() beside the
///               call. A daemon's own refusal words are the admitted exception,
///               for the reason `DescribeOutcome` gives: they are bounded by
///               cause, and lumping a version mismatch in with an unreachable
///               daemon is what makes the tally useless.
void RecordFallback(Fallback kind, std::string_view reason)
{
    auto const& row = RowFor(kind);
    invocation.outcome = row.outcome;
    invocation.outcomeDetail = reason;
    if (invocation.verbose)
        std::cerr << "fastcache-cc: " << row.leadIn << " (" << reason << "); " << row.continuation << '\n';
}

/// Report that the cache could not serve this compile, and end the cache flow.
///
/// **The return value is the continuation, so the line cannot lie about it.** The
/// row above promises the operator that the real compiler runs next; returning
/// `std::nullopt` is how that happens, and making it this function's result means
/// the only spelling at a call site is `return Warn(...)`. Announcing one
/// continuation and taking the other was previously a thing a caller could do by
/// accident, and issue #236 is what it costs.
/// @param reason The fall-back reason, under `RecordFallback`'s fixed-string rule.
/// @return Always `std::nullopt` — "fall back to a plain real compile".
[[nodiscard]] std::optional<int> Warn(std::string_view reason)
{
    RecordFallback(Fallback::Unavailable, reason);
    return std::nullopt;
}

/// Report that the cache is out of the picture for the rest of this invocation,
/// which continues without it.
///
/// The same bucket `Warn` records — an operator still has to see that the cache is
/// broken, and `--show-stats` still ranks the reason — with a different line,
/// because the two facts came apart. A daemon that refused, and one that was never
/// reached, both leave this launcher holding a key and a fleet that knows nothing
/// about either, so the compile goes on being attempted, remotely first. It
/// returns nothing for the same reason `Warn` returns `std::nullopt`.
/// @param reason The fall-back reason, under `RecordFallback`'s fixed-string rule.
void WarnAndCarryOn(std::string_view reason)
{
    RecordFallback(Fallback::UnavailableCarryOn, reason);
}

/// Report that this compile is deliberately not cacheable.
///
/// The refusal half: the cache is working, and this translation unit is one the
/// launcher will not cache — because it could never hit (a time macro), or
/// because caching it could not be made truthful (a path that is neither keyed
/// nor guarded, issue #104).
/// @param reason The refusal reason, under `RecordFallback`'s fixed-string rule.
/// @return Always `std::nullopt`, as `Warn` does and for the same reason.
[[nodiscard]] std::optional<int> Decline(std::string_view reason)
{
    RecordFallback(Fallback::Uncacheable, reason);
    return std::nullopt;
}

/// Record what this invocation did about DISTRIBUTION.
///
/// A separate recorder from `RecordFallback` rather than a parameter on it, because
/// the two axes are independent: a compile that missed the cache and then failed to
/// dispatch has to say BOTH, and a single recorder would make each call overwrite
/// the other's half. Before this axis existed, every non-answer from `Dispatch`
/// reached only a verbose-gated `Note` and the record said nothing at all -- so a
/// fleet in which every dispatch failed produced an ordinary miss rate and total
/// silence ([#427](https://github.com/LASTRADA-Software/fastcached/issues/427)).
/// @param recording The state and its FIXED tally reason; see `Cc::RecordingFor`.
void ApplyDispatchRecording(Cc::DispatchRecording const& recording)
{
    invocation.dispatch = recording.outcome;
    invocation.dispatchDetail = recording.reason;
}

/// Record that distribution did not serve this compile, and end the attempt.
///
/// **The return value is the continuation, so a call site cannot record one thing
/// and do another.** The cache axis learnt this: `Warn` returns `std::nullopt`
/// precisely so the only spelling is `return Warn(...)`, and issue #236 is what the
/// loose shape cost. Recording and returning as separate statements is the same trap
/// one axis over — a branch added later that forgets the record does not leave a
/// blank, it leaves the `main()` seed, so a refusal THIS launcher made is reported
/// as `NotAttempted`. That is a false claim rather than a gap, and an invisible one:
/// `NotAttempted` neither prints a line of its own nor moves the dispatch rate.
///
/// @param recording What to record.
/// @param note The verbose line, or `std::nullopt` when the caller has already
///        announced this itself. The line's variable half — an offending flag, a
///        worker's endpoint, an exit code — lives HERE and never in the tally
///        reason, which is what keeps the tally one row per cause rather than one
///        row per machine. The exact wordings are asserted by `dist-compile-e2e`.
/// @return Always `std::nullopt` — "compile this translation unit locally".
[[nodiscard]] std::optional<Cc::CompileRun> DeclineDispatch(Cc::DispatchRecording const& recording,
                                                            std::optional<std::string> note)
{
    ApplyDispatchRecording(recording);
    if (note.has_value())
        Note(*note);
    return std::nullopt;
}

/// The dispatch recording for a refusal this LAUNCHER made, before the fleet was
/// ever asked.
///
/// `Refused`, never `Declined`: the fleet was not involved, so this says nothing
/// about it and is fixed on this machine. Reported as a fleet refusal it would send
/// an operator to look at capacity for a command line that was never sent.
/// @param reason The FIXED tally reason. @return The recording.
[[nodiscard]] constexpr Cc::DispatchRecording RefusedHere(std::string_view reason) noexcept
{
    return Cc::DispatchRecording { .reason = reason, .outcome = Cc::DispatchOutcome::Refused };
}

/// The dispatch recording for a compile a worker RAN and this client threw away.
///
/// `Discarded` rather than `Dispatched` with a caveat in the reason: the reports
/// rate `Dispatched` against what was asked of the fleet, so a fleet whose every
/// result was discarded would headline 100% dispatched. See `Cc::DispatchOutcome`.
/// @param reason The FIXED tally reason. @return The recording.
[[nodiscard]] constexpr Cc::DispatchRecording Discarded(std::string_view reason) noexcept
{
    return Cc::DispatchRecording { .reason = reason, .outcome = Cc::DispatchOutcome::Discarded };
}

/// The fixed reason `--show-stats` tallies a crossed worker reply under.
///
/// Fixed, under `RecordFallback`'s rule: the endpoint and the two digests are what
/// an operator acts on but they differ per compile, so tallying them would produce
/// a row per translation unit instead of a row per cause. They ride the announced
/// line instead.
constexpr std::string_view CrossedReplyReason = "a worker answered about a different compile";

/// Report that a worker's reply did not belong to the request that asked for it,
/// and that this client refused it (#280).
///
/// **Announced unconditionally, unlike every other fall-back here.** The rest are a
/// fleet declining to help — a scheduler with nobody free, a worker that went away —
/// and an operator has nothing to fix; those are `--verbose` material. This one says
/// a machine produced an object for work nobody asked it to do, which is a defect,
/// and it is the failure class this project fears most: accepted, it would be a
/// wrong object under a correct key, stored, and then served to every other machine
/// that fetches that key. A correctness alarm and a "distribution did not help
/// today" note must not share a verbosity level.
///
/// **This line and `--show-stats` are the complete set of places the fact can live,
/// and no fleet-wide aggregate of it exists or can.** Only the client can detect a
/// crossed reply — a worker that knew its reply was crossed would not have sent it,
/// so the server has nothing to count — and the client is `fastcache-cc`, one
/// short-lived process per translation unit with no metrics sink. A
/// `MetricsCatalog` row would therefore be a `/metrics` series reading a permanent
/// zero *while the defect fires*, which misinforms an operator rather than merely
/// failing to inform one. Do not add one; add a way for a client to report it, or
/// leave it here.
///
/// **The outcome is deliberately not touched.** The cache answered honestly and
/// this translation unit is still a MISS that the local compiler will serve and the
/// daemon will store; what failed was a worker. Recording it as `Unavailable` would
/// blame the cache and file the source under "never cached", both untrue.
/// @param detail What the dispatch saw — the worker and the two correlations.
void ReportCrossedReply(std::string_view detail)
{
    invocation.outcomeDetail = CrossedReplyReason;
    std::cerr << "fastcache-cc: " << CrossedReplyReason << " (" << detail
              << "); refusing that object and compiling this translation unit locally\n";
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

// --- process exec -----------------------------------------------------------

/// The one place a "your credential went unchecked" line comes from.
///
/// Constructed here because this file is the composition root, and reached by the
/// same accessor idiom as `ProcessRunner()` and `ToolchainHost()` below. What
/// changed in [#363](https://github.com/LASTRADA-Software/fastcached/issues/363) is
/// not that the guard exists but WHERE: it used to be a `static bool` inside a
/// function only the cache path called, so the six dispatch consumers -- three in
/// other translation units, one in another executable -- had nothing to report
/// through. The notice now travels with the exchange, so every path reports and none
/// of them has to remember to.
/// @return The process's credential notice.
[[nodiscard]] Cc::CredentialNotice& Notice()
{
    static Cc::CredentialNotice notice { [](std::string_view text) { Note(text); } };
    return notice;
}

/// The process runner used by every spawn in this file. Created once; the
/// concrete implementation (CreateProcess on Windows, posix_spawn elsewhere)
/// is chosen behind the IProcessRunner seam.
[[nodiscard]] Cc::IProcessRunner& ProcessRunner()
{
    static std::unique_ptr<Cc::IProcessRunner> const runner = Cc::MakeProcessRunner();
    return *runner;
}

/// The machine's filesystem, registry and environment, as the toolchain probe
/// reaches them. Created once, alongside the process runner and for the same
/// reason: this file is the composition root, so the seams are constructed here
/// and everything below takes them as parameters.
[[nodiscard]] Cc::IToolchainHost& ToolchainHost()
{
    static std::unique_ptr<Cc::IToolchainHost> const host = Cc::MakeToolchainHost();
    return *host;
}

using Cc::CompileRun;

/// Run `argv` with stdout and stderr captured separately.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return Exit code plus both captured streams.
[[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv)
{
    return ProcessRunner().RunCaptureSplit(argv);
}

/// Run `argv` with the streams captured separately, asking the compiler to speak
/// English.
///
/// **Only for a spawn whose output nobody but this process reads.** `VSLANG` selects
/// `cl`'s diagnostic language, so this is what makes a compiler's `/showIncludes`
/// notes parseable by a launcher that matches them against an English literal -- and
/// it would just as happily anglicize the warnings a developer reads. A separate
/// entry point rather than a flag on the one above, so that distinction is made at
/// the call site and can be seen there (issue #692).
///
/// `1033` is `en-US`. Best effort by nature: `cl` falls back to whichever language
/// pack IS installed when the requested one is absent, so a caller must still be
/// able to tell from the OUTPUT that the request did not take --
/// `Cc::CarriesUnreadableIncludeNotes` is how.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return Exit code plus both captured streams.
[[nodiscard]] CompileRun RunCaptureSplitInEnglish(std::span<std::string const> argv)
{
    std::array<Cc::EnvironmentAssignment, 1> const english { {
        { .name = "VSLANG", .value = "1033" },
    } };
    return ProcessRunner().RunCaptureSplit(argv, english);
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
    return run.exitCode == Cc::NotSpawned ? 1 : run.exitCode;
}

/// Compile the translation unit again and compare it against the object a hit put
/// on disk.
///
/// **The mechanism that turns a wrong object from invisible into loud**
/// ([#423](https://github.com/LASTRADA-Software/fastcached/issues/423)). Off unless
/// `FASTCACHE_VERIFY` names a rate, because a verified hit costs a whole compile.
///
/// It compiles to the SAME output path rather than a temporary one, having first
/// copied the served object aside. Two things fall out of that and neither is an
/// accident: no `-o` / `/Fo` has to be rewritten, which would be a second parser for
/// the one flag whose spelling differs most between drivers -- and on a mismatch the
/// freshly compiled object is already where the build expects it, so "use the fresh
/// one, never the cache's" needs no separate step. A build that has just proved its
/// cache wrong must not go on to link the wrong thing.
///
/// @param cmd The parsed command line; `objPath` is what a hit wrote.
/// @param argv What to run to compile for real.
/// @param key The object key this hit was served under.
/// @param rate One hit in this many is checked; `VerificationOff` checks none.
/// @return What the comparison found, and what it turned on.
[[nodiscard]] Cc::HitComparison VerifyServedObject(Cc::ParsedCommand const& cmd,
                                                   std::span<std::string const> argv,
                                                   std::string const& key,
                                                   unsigned rate)
{
    if (!Cc::ShouldVerifyHit(key, rate))
        return { .verdict = Cc::HitVerdict::NotChecked, .comparison = std::nullopt, .detail = {} };

    auto const served = FastCache::PathFromNarrowText(cmd.objPath);
    if (!served.has_value())
        return { .verdict = Cc::HitVerdict::Inconclusive, .comparison = std::nullopt, .detail = {} };

    // Beside the object rather than in the system temp directory: the build already
    // writes here, so it is writable and on the same filesystem, and a rename or a
    // copy cannot cross a device.
    auto const aside = std::filesystem::path { *served }.concat(".fastcache-verify");

    std::error_code ec;
    std::filesystem::copy_file(*served, aside, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return { .verdict = Cc::HitVerdict::Inconclusive, .comparison = std::nullopt, .detail = {} };

    // The compiler's own streams are DISCARDED. It has already been served a hit, so
    // its diagnostics were replayed; printing them a second time would make a verified
    // build look like it compiled everything twice, which it did -- but the reader is
    // being asked about the object, not the warnings.
    auto const run = RunCaptureSplit(argv);
    if (run.exitCode != 0)
    {
        // The fresh compile failed, which says nothing about the cached object -- and
        // may well have left no output at all. Put back what the hit served, so a
        // failed verification never costs the build its object.
        std::filesystem::copy_file(aside, *served, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(aside, ec);
        return { .verdict = Cc::HitVerdict::Inconclusive, .comparison = std::nullopt, .detail = {} };
    }

    auto comparison = Cc::CompareObjectFiles(aside, *served);
    std::filesystem::remove(aside, ec);
    return comparison;
}

/// Report a verification, and say nothing when there is nothing to say.
///
/// **Not verbose-gated**, unlike every other launcher diagnostic. This is the one an
/// operator must not have to have opted into: a wrong object that was detected and
/// then only counted is a number somebody has to come back and ask about, and the
/// whole reason #368 went unnoticed is that nothing said anything.
/// @param comparison What the comparison found, and what it turned on.
/// @param key The key, so the entry can be looked at rather than only counted.
void ReportVerification(Cc::HitComparison const& comparison, std::string const& key)
{
    // A clean verification stays SILENT by default and still says what it saw when
    // asked. On Windows every hit is "identical apart from the clock" (#493), so
    // printing that unconditionally would put a line on every hit of every build and
    // teach a reader to filter exactly the stream the loud case arrives on.
    if (comparison.comparison == Cc::ObjectComparison::EquivalentApartFromVolatile)
        Note(std::format("verified the hit for key {}: identical apart from {}", key, comparison.detail));

    auto const line = Cc::DescribeVerdict(comparison, key);
    if (line.empty())
        return;
    std::cerr << line << '\n';
}

// --- file helpers -----------------------------------------------------------

[[nodiscard]] bool WriteFileBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream out { path, std::ios::binary };
    if (!out)
        return false;
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

/// The deadlines this invocation runs a dispatch under.
///
/// The scheduler's control verbs take the cache's budget, because they are the same
/// shape of conversation -- a short reply out of the scheduler's own tables. The
/// worker's COMPILE takes its own, because the worker writes nothing until the
/// compiler has finished, so the client sits in one read for the whole compile and
/// the cache's ten seconds abandoned every translation unit worth distributing
/// (#223).
///
/// The derivation itself is `Cc::DispatchBudgetsFor`, and it is there rather than
/// here for one reason: this file is in no test target, so a budget built here is
/// unreadable by anything. That is how the compile leg's keepalive was dropped for
/// every shipped launcher -- see that function's note.
/// @param cfg The launcher's configuration.
/// @return The budgets.
[[nodiscard]] Cc::DispatchBudgets DispatchBudgetsOf(Config const& cfg)
{
    return Cc::DispatchBudgetsFor(Cc::DispatchBudgetKnobs {
        .connect = cfg.connectTimeout,
        .controlTotal = cfg.ioTimeout,
        .compileTotal = cfg.dispatchTimeout,
        .compileIdle = cfg.dispatchIdle });
}

/// The deadlines this invocation runs every cache exchange under.
///
/// The dispatch's CONTROL budget, taken rather than built a second time. The two are
/// the same conversation -- a short reply out of a daemon's or a scheduler's own
/// tables -- and saying so once is what the comment this replaced was reaching for
/// when it derived the control leg from this call instead. Either direction is one
/// producer; this one is the direction that leaves `Cc::DispatchBudgetsFor` the only
/// place a budget is assembled, so a field added to `ExchangeBudget` cannot reach the
/// cache legs and miss the dispatch legs or the reverse.
/// @param cfg The launcher's configuration.
/// @return The budget.
[[nodiscard]] Cc::ExchangeBudget BudgetOf(Config const& cfg)
{
    return DispatchBudgetsOf(cfg).control;
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
    auto const bytes = Cc::ReadFileBytes(std::filesystem::path { cmd.depPath });
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
    auto const bytes = Cc::ReadFileBytes(std::filesystem::path { path });
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

    /// Whether the probe's dependency record was READ, as opposed to reporting
    /// nothing.
    ///
    /// `dependencyPaths` being empty answers two opposite questions at once -- this
    /// build has no dependency mechanism, or this machine's compiler answered in a
    /// language the launcher does not read (issue #692) -- and the manifest built
    /// downstream is correct for the first and a claim about files nobody looked at
    /// for the second. `Cc::ReportedDependencies` is where that distinction is
    /// carried; the field is here because the probe is the only thing that can see
    /// which it was.
    bool dependenciesUnreadable { false };
};

/// What a probe learned about a compile's dependencies, without the text.
///
/// `SourceProbe` carries several megabytes of preprocessed source and is
/// deliberately destroyed as soon as the key is computed. The two manifest writers
/// run long after that, so this is the part that outlives it -- the paths, and
/// whether the record they came from was READ at all.
struct ProbedDependencies
{
    std::vector<std::string> paths; ///< Dependencies as the compiler spelled them.

    /// The compiler reported dependencies and the launcher could not read them --
    /// a localized `cl` whose notes do not begin with `Cc::IncludeNoteMarker`
    /// (issue #692). Distinct from `paths` being empty, which is also what a build
    /// with no dependency flags at all produces, because the two send an operator
    /// to opposite places.
    bool unreadable { false };
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
    // English, because this run's output is read by nobody but this process: the
    // `/EP` text goes into the key and the notes are parsed and dropped. `cl` prints
    // `/showIncludes` notes in the language of whatever pack is installed, and the
    // launcher matches them against a literal English marker -- so on a localized
    // Visual Studio every note is invisible here, the key loses its dependency set,
    // and no manifest is ever recorded (issue #692). The REAL compile is deliberately
    // not run this way; its diagnostics are the developer's and stay in their
    // language.
    auto run = RunCaptureSplitInEnglish(pp);
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
        if (auto const bytes = Cc::ReadFileBytes(std::filesystem::path { probeRequest });
            bytes.has_value() && !bytes->empty())
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

    // Asked only when nothing was extracted, and only to explain THAT. The spawn
    // above requested English and `cl` honours `VSLANG` unless the pack is missing,
    // so reaching this with an empty set on a machine whose compiler is still
    // localized is the residue the request cannot cover -- and the difference
    // between "this compile has no dependencies" and "its dependencies are in a
    // language I do not read" is the whole of what an operator needs (issue #692).
    //
    // Only stderr is examined. Stdout carries the preprocessed SOURCE on a stream
    // driver, and this predicate is deliberately loose enough that a source line
    // ending in a path would trip it -- harmless for a sentence, misleading as a
    // diagnosis of the compiler.
    auto const unreadable = split.notePaths.empty() && Cc::CarriesUnreadableIncludeNotes(run.err);

    reconciler.All(split.notePaths);
    return SourceProbe { .preprocessed = std::move(split.preprocessed),
                         .dependencyPaths = std::move(split.notePaths),
                         .dependenciesUnreadable = unreadable };
}

/// This compiler's version banner.
///
/// A thin call through to `Cc::CompilerBanner`, which the compile node also uses.
/// One definition, because the node derives a fingerprint from this exact string
/// and two spellings would put a worker permanently out of agreement with its
/// clients -- silently, as a scheduler that simply never matches.
///
/// This is the FINGERPRINT's input and only half of the cache key's: what a key is
/// built on is this banner plus the target the driver generates for, joined by
/// `Cc::CacheCompilerId`. Passing this string where a key is expected would key two
/// materially different code generators alike.
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
    // Constructed here because this is a composition root: `--print-toolchain
    // -fingerprint` is a one-shot command and the walk it forces is the whole point
    // of it, so the width is not something a caller further in should be choosing.
    Cc::ThreadedParallelFor parallel;
    auto const identity = Cc::CachedToolchainFingerprint(
        ProcessRunner(), ToolchainHost(), compiler, banner, Cc::DriverOf(flavor), parallel, /*forceRefresh=*/true);

    std::cout << identity.fingerprint << '\n';

    // Said out loud, on stderr so it cannot corrupt a digest somebody is piping.
    // This command exists to answer "why did no worker match", and a defective
    // identity is the one answer the digest itself cannot give: it is a well-formed
    // hex string either way. Without this line an operator compares two
    // identical-looking values and concludes the two machines agree -- or compares
    // two different ones and goes looking for a toolchain difference that is really
    // a probe that did not run.
    if (!identity.Usable())
    {
        auto const& explanation = Cc::ExplainDefect(identity.defect);
        std::cerr << "warning: " << compiler << ": " << explanation.reason << ". " << explanation.remedy << '\n';
    }
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
    auto outcome = Cc::RunOneExchange(cfg.addr, Notice(), Wire::EncodeFetch(key), cfg.credential, BudgetOf(cfg));
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
                                            Notice(),
                                            Wire::EncodeStore(Wire::StoreRequest { .key = key,
                                                                                   .prefetchGroup = cfg.prefetchGroup,
                                                                                   .srcRoot = cfg.srcRoot,
                                                                                   .buildTree = cfg.buildTree,
                                                                                   .value = Wire::AsBytes(body) }),
                                            cfg.credential,
                                            BudgetOf(cfg));
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
///
/// The localized streams are deliberately NOT a member. They were, and their only
/// reader outside this type was the manifest backfill, which parsed them for the
/// headers this object depends on -- a reading with the PRODUCER's locale in it, so
/// a value written by a German `cl` was unparseable on an English machine and the
/// other way round (issue #692). The backfill takes the probe's own dependency set
/// now, and returning several megabytes of replayed text to nobody is not a
/// harmless leftover: it is a claim about who reads it.
struct MaterializedHit
{
    HitDisposition disposition { HitDisposition::Unusable };
};

/// A hit that was not materialized, so carries no streams. A named factory rather
/// than a brace-init at each site: the streams then have exactly one place that
/// says they are deliberately empty.
/// @param disposition Why it was not materialized.
/// @return The outcome, with empty replay text.
[[nodiscard]] MaterializedHit NotMaterialized(HitDisposition disposition)
{
    return { .disposition = disposition };
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
        localized.push_back(
            { .grammar = region.grammar, .bytes = PathCanon::LocalizeRegion(region.bytes, region.grammar, layout) });
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
    //
    // Local to this function now, because replaying them is the whole of what they
    // are for -- `MaterializedHit` records what used to read them afterwards and why
    // it no longer does.
    std::array<std::string, ReplayRegionCount> replayed;
    for (std::size_t idx = 0; idx < localized.size() && idx < replayed.size(); ++idx)
        replayed[idx] = localized[idx].bytes;
    ReplayStreams(replayed[0], replayed[1]);
    return NotMaterialized(HitDisposition::Served);
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
                                                   std::span<std::string const> argv,
                                                   std::string const& key,
                                                   PathCanon::Layout const& layout,
                                                   std::filesystem::path const& workingDirectory)
{
    auto const payload = FetchRaw(cfg, key);
    if (!payload.has_value())
        return std::nullopt;

    auto decoded = DecodeCompileValue(*payload);
    if (!decoded.has_value())
    {
        // Said rather than swallowed: direct mode falls through to the preprocessed
        // path here, so an undecodable value costs a whole preprocess and shows up
        // as nothing but a slower build. During a rolling upgrade that is every
        // compile on the machine.
        NoteUndecodableValue("the direct-mode object", decoded.error());
        return std::nullopt;
    }

    // Anything short of Served falls back to preprocessing, which re-runs the same
    // check and, if it also finds the value stale, recompiles and re-stores it.
    // Direct mode only ever declines to shortcut; repairing the entry is not its
    // job, and doing it here would duplicate the miss path.
    if (MaterializeHit(cmd, *decoded, layout, workingDirectory).disposition != HitDisposition::Served)
        return std::nullopt;

    invocation.valueBytes = decoded->objectBlob.size();
    // Before the trace, so a build log reads in the order the events happened: the
    // hit, then what verifying it found. Off unless `FASTCACHE_VERIFY` names a rate,
    // in which case this costs a whole compile (#423).
    ReportVerification(VerifyServedObject(cmd, argv, key, cfg.verifyRate), key);
    TraceOutcome("HIT", key);
    return 0;
}

/// Report that this compile gets no direct-mode manifest, and why.
///
/// One spelling for all three refusals, so a build log shows the same sentence
/// whichever of them fired and only the parenthesised reason differs — which is
/// what makes the reason the thing a reader's eye lands on. Verbose-gated like
/// every other launcher diagnostic; NoteIfRootsDoNotDescribeCompile carries why
/// that call was made, unmade and deliberately made again.
/// @param reason Why no manifest is being recorded.
void NoteNoManifest(std::string_view reason)
{
    Note(std::format("not recording a direct-mode manifest ({})", reason));
}

/// Record the direct-mode manifest for a compile whose object bytes are already
/// stored, so the next compile of this TU can skip preprocessing.
///
/// Called from BOTH the hit and the miss path, and from the same source on each:
/// the dependency set the preprocess probe already produced for the cache key. The
/// probe ran this very translation unit with these very flags moments earlier, so
/// no extra compiler invocation is needed and no console text has to be re-parsed.
///
/// It used to parse the compiler's captured streams instead -- the real compile's on
/// a miss, the cached value's REPLAYED ones on a hit -- and both readings had a
/// locale in them. The miss path matched `cl`'s notes against a literal English
/// marker, so a localized Visual Studio recorded no manifest ever (issue #692); the
/// hit path parsed text the PRODUCER's compiler wrote, so a value produced on a
/// German machine was unreadable on an English one and vice versa. Taking the
/// probe's set closes both, because the probe is asked in English on a spawn whose
/// output nobody reads.
///
/// The manifest records the object's ordinary key
/// rather than causing a second copy of the object to be stored: see
/// DirectManifest::objectKey for why duplicating it is not affordable.
/// @param cfg             Launcher config.
/// @param cmd             The parsed compile command.
/// @param layout              This machine's roots.
/// @param workingDirectory    The directory this compile runs in; every relative
///                            path the driver reported resolves against it.
/// @param relativizedArgs     The relativized compile arguments.
/// @param toolchainStamp      The toolchain identity.
/// @param probed              The preprocess probe's own dependency answer.
/// @param objectKeyForPointer Key the object is already stored under; recorded in
///                            the manifest so the object is never stored twice.
/// @param reconciler      Translates a driver's spelling into this build's.
void RecordManifest(Config const& cfg,
                    Cc::ParsedCommand const& cmd,
                    PathCanon::Layout const& layout,
                    std::filesystem::path const& workingDirectory,
                    std::vector<std::string> const& relativizedArgs,
                    std::string const& toolchainStamp,
                    ProbedDependencies const& probed,
                    std::string_view objectKeyForPointer,
                    Cc::RootReconciler& reconciler)
{
    auto const workingDirectoryText = workingDirectory.string();
    auto const resolvedSource = reconciler.Directory(cmd.source);
    auto const canonicalSource = Cc::CanonicalSourceToken(resolvedSource, layout, workingDirectoryText);
    if (!canonicalSource.has_value())
    {
        // Said out loud, because this is the refusal a real build meets and it used
        // to return in complete silence -- not even under FASTCACHE_VERBOSE. A
        // source with no canonical token can never key a manifest, so direct mode is
        // off for this translation unit permanently while the compile goes on
        // succeeding: nothing else in the log so much as mentions it (issue #68).
        NoteNoManifest(Cc::DescribeManifestFailure(canonicalSource.error()));
        return;
    }

    // The probe's set, taken as it stands. It is already reconciled -- `Preprocess`
    // does that at its own boundary -- and it covers both driver families, which is
    // why the two stream parses that used to be here are gone rather than moved.
    auto includes = probed.paths;

    // The build's OWN depfile, when the probe came back with nothing and the reason
    // was not that its notes were unreadable.
    //
    // Kept rather than dropped with the parses, and the reason is asymmetric.
    // `Preprocess` has two documented ways to yield an empty set on a depfile driver
    // -- its scratch destination was not writable, or the driver wrote the file and
    // put nothing in it -- and it DEGRADES rather than failing, because an empty
    // dependency set costs the KEY only its moved-header protection. It costs the
    // MANIFEST everything: direct mode would be off in that tree permanently and
    // silently, on every compile. The build's own `-MF` names the same headers.
    //
    // Guarded on `unreadable` so it cannot paper over the localized case: an MSVC
    // build has no depfile to read there, and the operator needs the sentence below
    // rather than a second empty answer arrived at by a different route.
    if (includes.empty() && !probed.unreadable)
        if (auto const depText = ReadDepFile(cmd))
        {
            includes = Cc::ParseDepFilePaths(*depText);
            // Reconciled here because this set came straight off disk, unlike the
            // probe's. Resolution is memoized and idempotent, so it costs a hash
            // lookup per path.
            reconciler.All(includes);
        }

    // Asked here as well as on the key path, because a manifest is the direct-mode
    // half of the same promise: an entry naming a path this host could not read is
    // revalidated against nothing, so a header edited inside it direct-hits forever.
    // The key path declines the whole compile for this; here there is nothing to
    // decline but the record itself.
    if (reconciler.UnreadablePaths() != 0)
    {
        NoteNoManifest("a reported dependency path is not text this host can read");
        return;
    }

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
    //
    // Whether a dependency record reached us AT ALL is stated rather than left to be
    // inferred from the vector being empty. Nothing came back means we DID NOT
    // OBSERVE the dependencies, not that there are none: the two are different states
    // and an empty vector renders them identically (issue #512). The probe cannot
    // tell them apart -- a GNU compile with no `-MD`/`-MF` produces no depfile and no
    // notes, and so does an MSVC compile whose notes carry a prefix
    // `Cc::IncludeNoteMarker` does not match, which is every note a localized `cl`
    // prints when `VSLANG` could not be honoured. Reading either as "this TU includes
    // nothing" would record a manifest asserting the TU alone on every compile on
    // that machine, and such a manifest revalidates forever and serves its object
    // into any checkout that computes the key (issue #368).
    //
    // Said with a VALUE rather than by returning early, which is what this used to
    // do. That left `BuildManifest` -- a library entry point -- correct only because
    // of a guard one translation unit away that nothing in `DirectManifest.cpp`
    // could see. Now the caller states what it observed and the seam decides, so the
    // refusal is named (`DepsNotObserved`), names the source path, and a second
    // caller cannot omit it: `ReportedDependencies` has no default constructor, so
    // there is no way to leave the question unanswered.
    auto const reported = includes.size();
    auto const manifest = Cc::BuildManifest(
        { .sourcePath = resolvedSource,
          .reportedDependencies = includes.empty() ? Cc::ReportedDependencies::NotObserved()
                                                   : Cc::ReportedDependencies::Observed(std::move(includes)),
          .workingDirectory = workingDirectoryText,
          .toolchainStamp = toolchainStamp,
          .objectKey = std::string { objectKeyForPointer } },
        layout);
    if (!manifest.has_value())
    {
        // One path and one reason, the shape the replay guard's STALE HIT note uses
        // and for the same reason: "why does this TU never cache" is otherwise a
        // whole investigation, and the answer is one path. This said
        // "uncanonicalizable source or include" for every refusal alike, which named
        // neither the file nor which of them it was (issue #68).
        //
        // One state gets a sharper sentence than the fault table can carry, and it
        // is the one an operator is least able to guess: the compiler DID report its
        // dependencies and this launcher could not read them. `DepsNotObserved`'s own
        // label -- "the compile produced no dependency record" -- is true of what was
        // observed and false about the world there, which is the string issue #692
        // exists to stop printing for this case. The refusal still comes from the
        // seam; only the wording is the caller's, because the provenance is.
        NoteNoManifest(probed.unreadable
                           ? std::format("the compiler reported dependencies in a language this launcher does not read "
                                         "(its notes do not begin with \"{}\"), so direct mode cannot populate: {}",
                                         Cc::IncludeNoteMarker,
                                         resolvedSource)
                           : Cc::DescribeManifestFailure(manifest.error()));
        return;
    }

    // Both counts, for the reason the key's dependency-set note gives them: the pair
    // is what distinguishes "this TU only includes toolchain headers" (fine, the
    // stamp covers them) from "most of what the driver reported was filtered out",
    // which is the shape of a misconfigured root.
    //
    // It no longer has to carry the WORST case. "Every reported path filtered out"
    // used to reach here and record a manifest that still validated, which is what
    // made the pair the only way to see it; `BuildManifest` now refuses that outright
    // (`NoProjectDeps`, #319) and this line is not reached for it. What the pair is
    // still for is the partial version -- nine paths dropped of ten -- which is a
    // misconfigured root in every way but the one that trips the refusal.
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
                                               std::span<std::string const> argv,
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
        // Deliberately silent, unlike RecordManifest's identical guard. Being the
        // same derivation from the same inputs, this refuses exactly when the
        // recording side does and for exactly the same reason -- so a note here
        // would print every such fact twice per translation unit, which is the
        // per-TU noise the #66 gating reversal was about. The recording side is
        // where a refusal gets said, because that is the side that had something to
        // record.
        return giveUp();

    auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);
    auto const manifestBytes = FetchRaw(cfg, manifestKey);
    if (!manifestBytes.has_value())
        return giveUp();

    // Unwrap the compile-value envelope the manifest was stored in.
    auto const envelope = DecodeCompileValue(*manifestBytes);
    if (!envelope.has_value())
        NoteUndecodableValue("the direct-mode manifest", envelope.error());
    auto const manifestSpan =
        envelope.has_value() ? std::span<std::byte const> { envelope->objectBlob } : std::span<std::byte const> {};
    auto const manifest =
        Cc::DecodeManifest(std::string_view { reinterpret_cast<char const*>(manifestSpan.data()), manifestSpan.size() });
    if (!manifest.has_value() || manifest->objectKey.empty())
        return giveUp();

    // Said by name, because this one is not an ordinary direct-mode miss. An empty
    // manifest validates on nothing at all -- `all_of` over no entries is true --
    // so it would serve its object however the sources move. `BuildManifest` cannot
    // write one, which makes it a decode artifact or an older format, and an
    // operator watching a miss rate move needs to see which of those it is rather
    // than a cache that merely looks cold.
    //
    // `Note`, not `Warn`: this is a cache fact, and the compile carries on to its
    // ordinary preprocessed key rather than falling back to the real compiler.
    if (Cc::ManifestAssertsNothing(*manifest))
    {
        Note("direct-mode manifest has no entries; refusing it rather than validating on nothing");
        return giveUp();
    }

    if (!Cc::ValidateManifest(*manifest, layout, toolchainStamp))
        return giveUp();

    invocation.directMs = MsSince(directStarted);
    // Follow the manifest's pointer to the object, which is stored exactly once
    // under its ordinary preprocessed key.
    auto served = TryServeFromCache(cfg, cmd, argv, manifest->objectKey, layout, workingDirectory);
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
/// @param compilerBanner The compiler's version line ALONE -- the fingerprint's
///        input, and deliberately not the cache key's, which also folds the target.
/// @param targetTriple The target this client generates for, stated on the wire.
/// @param dependencyPaths What the key's probe reported this TU depends on.
/// @return A run to continue with, or nullopt to compile locally.
[[nodiscard]] std::optional<Cc::CompileRun> TryRemoteCompile(Config const& cfg,
                                                             Cc::ParsedCommand const& cmd,
                                                             std::span<std::string const> argv,
                                                             std::string_view key,
                                                             std::string_view compilerBanner,
                                                             std::string_view targetTriple,
                                                             std::vector<std::string> const& dependencyPaths)
{
    // Refused before anything is sent when the command line carries something this
    // launcher cannot account for. See RemoteCompileArgs: refusing costs one local
    // compile, where stripping an unrecognised argument would change the generated
    // code and hand back an object nobody asked for.
    //
    // The target is stated in the SAME call rather than spliced in afterwards. It is
    // the caller's, probed once above the cache lookup because the key needs it, so
    // there is nothing here to defer and no second pass to justify -- and the rule
    // that the pin goes FIRST, so the build's own `--target=` or `-m32` still wins,
    // stays inside the function that owns the argument order, where it is tested.
    auto const args = Cc::RemoteCompileArgs(cmd, argv, targetTriple);
    if (!args.has_value())
        // The offending flag varies per compile, so it rides the verbose line and
        // never the tally -- otherwise one row per command line instead of per cause.
        return DeclineDispatch(RefusedHere("the command line is not dispatchable"),
                               std::format("not dispatchable ({}); compiling locally", args.error()));

    // Preprocessed again, with `#line` markers this time. The key's text has them
    // suppressed so no checkout path reaches the key; a worker needs them, because
    // they are what marks system-header lines as system-header lines. Without that
    // the remote compile re-reports every warning inside libc++ or the CRT, which
    // under `-Werror` is a failed compile rather than noise.
    auto const preprocessRun = RunCaptureSplit(Cc::DispatchPreprocessCommand(cmd, argv));
    if (preprocessRun.exitCode != 0)
        return DeclineDispatch(RefusedHere("the dispatch preprocess failed"),
                               "dispatch preprocess failed; compiling locally");

    // The DISPATCH identity, which is not the cache key's -- and it is built on the
    // BANNER ALONE, deliberately, where the key also folds the target.
    //
    // The two answer different questions and the same string cannot serve both. A
    // fingerprint decides which WORKER may serve this client: folding the target in
    // would split a developer-prompt launcher from a service-run worker, which is
    // the mismatch #145 removed, reintroduced for a fact the dispatch line now
    // states outright. A key decides which OBJECT may be served, and there the
    // target is load-bearing -- two machines with one clang-cl and two MSVC installs
    // print the same banner and need different objects.
    //
    // What the banner alone still cannot do is stand in for the include tree: two
    // machines can print an identical version line while resolving different
    // libstdc++ headers, and for distribution that is a wrong object rather than a
    // miss. So a worker is matched on a digest of the whole tree.
    //
    // Computed HERE rather than beside the stamp, so it stays on the miss-and-
    // dispatch-configured path only: it is a cache read in the steady state, but
    // several seconds the first time a machine sees a toolchain, and a build that
    // never dispatches must not pay that at all.
    Cc::ThreadedParallelFor parallel;
    auto const identity = Cc::CachedToolchainFingerprint(
        ProcessRunner(), ToolchainHost(), cmd.compiler, compilerBanner, Cc::DriverOf(cmd.flavor), parallel);

    // A digest that does not identify this toolchain is not dispatched with. The
    // outcome either way is a local compile -- a scheduler cannot match a value no
    // worker computes -- but sending it costs a round trip and, far worse, reports
    // as `NoWorker`, which reads as "the fleet has nobody on your toolchain" and
    // sends an operator to look at the fleet. This says which end is wrong.
    if (!identity.Usable())
        // `Refused` for the reason the comment above gives: sending it would report
        // as `NoWorker`, which reads as "the fleet has nobody on your toolchain".
        // The tally must not repeat that misdirection.
        return DeclineDispatch(
            RefusedHere("this toolchain has no usable fingerprint"),
            std::format("not dispatched ({}); compiling locally", Cc::ExplainDefect(identity.defect).reason));

    // What THIS compile records as its compilation directory, and the directory itself,
    // so a worker can make its object record the same thing. It is the launcher's own
    // working directory -- the local compile below runs with no directory of its own --
    // put through whatever `-fdebug-prefix-map` rules the build put on the line. Empty
    // when the build maps nothing, which is what tells the worker to map nothing
    // either (#506).
    //
    // `current_path()` rather than a seam: this is the one fact about the environment
    // that decides the answer, the function that uses it is pure and tested, and an
    // unreadable working directory is simply no mapping.
    //
    // **Read again here, and deliberately NOT the anchored value `RunCached` already
    // holds.** That one is put through `AnchorWorkingDirectory`, which re-spells the
    // resolved cwd in the vocabulary the layout's ROOTS use -- the right answer for
    // every prefix test in `DirectManifest`, and the wrong one here, where the only
    // vocabulary that decides anything is the DRIVER's. `main.cpp`'s read-once note
    // twelve hundred lines below governs the three consumers that need the layout's;
    // this is a fourth question.
    //
    // **And `current_path()` is not the driver's spelling either**, which is what
    // `CompilerWorkingDirectory` is for. `current_path()` is `getcwd(3)`, which resolves
    // every symlink, while both drivers report and compare `$PWD` when it names the same
    // directory -- so on any build reached through a link (macOS's `$TMPDIR`, under
    // `/var -> private/var`; a symlinked `/home`; a symlinked build directory) the rule
    // on the line spells the link and the resolved cwd does not match it. This predicted
    // "no mapping is in force" for a build that maps perfectly well, sent nothing, and
    // left the dispatched object recording the WORKER's directory while the local one
    // recorded `.` -- #506 unfixed, silently, by the fix for #506. The measurements are
    // on `CompilerWorkingDirectory`.
    //
    // A named value and not a `value_or` temporary: `DispatchRequest` holds views, and
    // this repository has been bitten three times by a value that borrows from
    // something it outlives.
    std::error_code cwdError;
    auto const workingDirectory = std::filesystem::current_path(cwdError);
    auto const compileDir = cwdError ? std::optional<Cc::MappedCompileDir> {}
                                     : Cc::MappedCompileDirectory(argv,
                                                                  Cc::DriverOf(cmd.flavor).family,
                                                                  Cc::CompilerWorkingDirectory(workingDirectory.string()));
    auto const compileDirPath = compileDir.has_value() ? compileDir->directory : std::string {};
    auto const compileDirReplacement = compileDir.has_value() ? compileDir->replacement : std::string {};

    auto const exchange = Cc::MakeTcpExchange(Notice());
    auto const outcome = Cc::Dispatch(*exchange,
                                      Cc::DispatchRequest { .schedulerEndpoint = cfg.schedulerAddr,
                                                            .fingerprint = identity.fingerprint,
                                                            .objectKey = key,
                                                            .args = *args,
                                                            .preprocessed = preprocessRun.out,
                                                            .sourceName = cmd.source,
                                                            .compileDir = compileDirPath,
                                                            .compileDirReplacement = compileDirReplacement },
                                      DispatchBudgetsOf(cfg),
                                      cfg.credential);
    // What the fleet's own answer means on the statistics axis, decided once and in
    // `Stats`, which is where it can be asserted -- `main.cpp` is in no test target.
    auto const fleetAnswer = Cc::RecordingFor(outcome.status);

    if (outcome.status == Cc::DispatchStatus::Mismatched)
    {
        // Not one of the two below, and not reported like them: a worker answering
        // about somebody else's compile is a defect rather than a fleet declining,
        // so it is announced unconditionally and the sentence goes on the CACHE
        // axis. That is why this is the one decline with no verbose line of its own.
        ReportCrossedReply(outcome.detail);
        return DeclineDispatch(fleetAnswer, std::nullopt);
    }
    if (!outcome.Ran())
        // Declined and Unavailable are both ordinary and both end the same way, but
        // they are fixed in different places, which is why they are two states. The
        // reason is named because "distribution stopped working" is otherwise a
        // whole investigation, and the answer is one line.
        return DeclineDispatch(fleetAnswer, std::format("not dispatched ({}); compiling locally", outcome.detail));
    if (outcome.exitCode != 0)
        // A worker RAN the compiler and this client is about to throw the result
        // away, which is `Discarded` -- see `Cc::DispatchOutcome`. It is the only
        // place a node failing compiles that are fine is visible at all: the build
        // stays green because the local retry succeeds, so a rising count is the
        // signal. Which of the two it was -- broken code or a broken node -- needs
        // the local retry's verdict, which this function never sees, so the reason
        // claims neither.
        return DeclineDispatch(Discarded("a worker compile failed and was retried locally"),
                               std::format("worker {} reported exit {}; recompiling locally to confirm",
                                           outcome.workerEndpoint,
                                           outcome.exitCode));

    // The object first: everything after it is a record ABOUT this object, and
    // writing those first would leave a dependency record describing a file that is
    // not there if the write fails.
    if (!WriteFileBytes(cmd.objPath, outcome.object))
        return DeclineDispatch(Discarded("the dispatched object could not be written"),
                               "could not write the dispatched object; compiling locally");

    // The dependency record, in whichever form this build asked for. Both can be
    // wanted at once, and neither is inferred from the other.
    Cc::CompileRun run { .exitCode = 0, .out = outcome.stdoutText, .err = outcome.stderrText };
    if (!cmd.depPath.empty() && !WriteDepFile(cmd.depPath, Cc::RenderDepFile(cmd.objPath, dependencyPaths)))
        return DeclineDispatch(Discarded("the depfile for a dispatched compile could not be written"),
                               "could not write the depfile for a dispatched compile; compiling locally");
    if (cmd.wantShowIncludes)
        // Prepended, not appended: `cl` emits its notes before its diagnostics, and
        // the stored value's region ordering is what a later hit replays verbatim.
        run.out = Cc::RenderShowIncludes(dependencyPaths) + run.out;

    // The object is kept, so this is a plain `Dispatched`. Recorded HERE rather than
    // beside the call above, because every branch between the two returns through
    // `DeclineDispatch` and a state written up front would be overwritten by each of
    // them -- an ordering nothing would enforce.
    ApplyDispatchRecording(fleetAnswer);
    Note(std::format("DISPATCHED to {} key={}", outcome.workerEndpoint, key));
    return run;
}

/// Try to serve `cmd` from the cache; returns the process exit code if handled
/// (hit, or miss-then-compiled — locally or on a worker), or std::nullopt to
/// signal "fall back to a plain real compile".
///
/// A cache that refused or could not be reached is NOT one of those signals — see
/// `Cc::CacheIsServing`, and issue #236 for what returning here cost.
/// @param cmd Taken BY VALUE so the driver flavour can be corrected in place once
///        the banner is known -- see `ClassifyCompilerFromBanner`. A copy of a few
///        strings, once per invocation, against a correction every consumer below
///        has to see the same way.
[[nodiscard]] std::optional<int> RunCached(Config const& cfg, Cc::ParsedCommand cmd, std::span<std::string const> argv)
{
    if (cfg.addr.empty() || cfg.srcRoot.empty() || cfg.buildTree.empty())
        return Warn("missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR");

    // One layout, and it is the build system's own spelling — every consumer of it
    // below either tokenizes against it or emits from it, and both want that form.
    // The reconciler holds the resolved spellings and is the only thing that sees
    // them; nothing downstream needs to know they exist. Taken FROM the reconciler
    // rather than built beside it, so the two cannot come to disagree about a
    // trailing separator or anything else the constructor normalizes.
    // The host's narrow-text policy goes in here and nowhere else, because this is
    // the one object every path a compiler emitted passes through. It is read from
    // the platform at exactly this call site so everything it reaches stays pure.
    Cc::RootReconciler reconciler { cfg.srcRoot, cfg.buildTree, PathResolver(), FastCache::HostNarrowTextPolicy() };
    PathCanon::Layout const& layout = reconciler.Layout();

    // Asked before ANYTHING else the cache does, direct mode included, and that
    // ordering is the point rather than the cost. A drive-relative path under no
    // root is dropped from the key and skipped by the replay guard, so a header
    // moved inside it is neither re-keyed nor noticed; the manifest a direct hit
    // validates came through the same filter, so it dropped that path too. Asking
    // here means such a build never consults an entry written under the old rules
    // — which is what closes this without re-keying every cache on every platform
    // (issue #104). See Cc::UnkeyableArgument for why this is not the only ask.
    if (auto const unkeyable = Cc::UnkeyableArgument(argv.subspan(1), layout))
    {
        // The argument goes in a Note and not in the reason: the reason is what
        // `--show-stats` tallies, and one carrying a path is a row per compile.
        Note(std::format("drive-relative path under no root, in argument {}", *unkeyable));
        return Decline("a command-line path is drive-relative under no root; not caching "
                       "(neither keyed nor guarded)");
    }

    // Directory-flavoured, because an argument's own last component can be the
    // aliased one — an `-I` pointing at a symlinked include directory is the
    // ordinary case — and there are few enough arguments that resolving each
    // completely costs nothing measurable.
    auto const relativizedArgs =
        Cc::RelativizeArgs(argv.subspan(1), cfg.srcRoot, cfg.buildTree, [&reconciler](std::string_view path) {
            return reconciler.Directory(path);
        });
    auto const compilerBanner = CompilerId(cmd.compiler);

    // `cc` and `c++` name a policy rather than a product, and on macOS that policy
    // resolves to Apple clang -- CMake's default C++ compiler there. Classified by
    // name they landed in the `gcc` row, which since this change is the row deciding
    // how a target is discovered and whether it can be stated. The banner settles it
    // and is already in hand, so the correction costs a string test rather than a
    // spawn. Only the target columns can move: the two rows are otherwise identical,
    // which `CmdLine_test` pins so that stays true.
    //
    // Corrected IN PLACE rather than threaded to the one call that discovers the
    // target, and the difference is a wrong object. `RemoteCompileArgs` re-derives
    // its own row from `cmd`, so a correction that stopped at the probe left a clang
    // invoked as `cc` with its versioned triple in the KEY and no `--target=` on the
    // dispatched line -- the worker generating for its own target, and the result
    // stored under the client's key. Everything below this line now reads one
    // flavour.
    cmd.flavor = Cc::ClassifyCompilerFromBanner(cmd.flavor, compilerBanner);
    auto const& driver = Cc::DriverOf(cmd.flavor);

    // Asked HERE, above the cache lookup, because the answer is a cache key input and
    // a key is needed on a hit too. That is a second spawn per invocation on a clang
    // driver, beside the `--version` one already paid, and it is the price of the key
    // meaning what it says. Caching it under the fingerprint's stamp was considered
    // and rejected: that stamp covers the compiler binary and its include roots, none
    // of which move when the MSVC install beside `clang-cl` is upgraded, so a cached
    // triple goes stale in the one direction that yields a WRONG HIT rather than a
    // miss. Only a driver with no target at all (`cl`) is left unspawned; `gcc` pays
    // it too, because its target is keyed even though it can never be stated.
    auto const targetTriple = Cc::DiscoverTargetTriple(ProcessRunner(), cmd.compiler, driver);

    // Said out loud, because the failing-open story has a hole and this is the only
    // place it is visible. An empty answer on ONE end is a miss -- the two sides key
    // differently and simply stop sharing. An empty answer on BOTH ends is the
    // ORIGINAL defect, silently: both fall back to the banner alone and two code
    // generators share a key again. Nothing downstream can tell the two cases apart,
    // so a driver that has a way to name its target and did not is worth a line.
    if (targetTriple.empty() && driver.targetDiscovery != Cc::TargetDiscovery::None)
        Note("the compiler did not report a target; keying on its banner alone");

    // The banner and the target, joined once. Everything that decides which OBJECT
    // may be served keys on this; the fingerprint, which decides which WORKER may
    // serve, keeps the banner alone. See CacheCompilerId.
    auto const toolchainStamp = Cc::CacheCompilerId(compilerBanner, targetTriple);

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
        if (auto served =
                TryDirectMode(cfg, cmd, argv, layout, workingDirectory, relativizedArgs, toolchainStamp, reconciler))
            return served;

    auto const preprocessStarted = std::chrono::steady_clock::now();
    // Scoped, and the text MOVED through it rather than copied: the preprocessed
    // form of a real translation unit runs to several megabytes, and everything
    // below — two round trips and, on a miss, the real compiler as a child
    // process — has no use for it once the key exists. Leaving it live held that
    // much dead memory resident for exactly as long as the machine is busiest.
    std::string key;
    // Kept alive past the block below ONLY when a scheduler is configured, and it
    // is the dependency PATHS rather than the preprocessed text. A worker gets its
    // own preprocess, with `#line` markers the key's copy deliberately suppresses,
    // so the text this block produced has no reader after the key -- the launcher
    // once held several megabytes of it alive across the whole compile for a
    // consumer that no longer existed. The paths it cannot re-derive: a worker sees
    // no `#include` at all, so the client writes the dependency record.
    std::vector<std::string> dispatchDependencies;
    bool const dispatchConfigured = DispatchConfigured(cfg);

    // What the probe learned about this compile's dependencies, carried past the
    // block the probe itself dies in.
    //
    // Both readers of it are outside that block: the manifest recorded on a MISS
    // after the compile, and the one backfilled on a HIT. Both used to parse the
    // compiler's captured console text instead, and both readings had a locale in
    // them -- see `RecordManifest` (issue #692).
    ProbedDependencies probed;
    {
        auto probe = Preprocess(cmd, argv, reconciler);
        if (!probe.has_value())
            return Warn("preprocess failed");

        // Skip translation units that reference a time/date macro. `__TIME__` /
        // `__DATE__` / `__TIMESTAMP__` expand to a run-varying (second-granular)
        // string, so such a TU re-keys on every compile and can never hit —
        // caching it only churns the store. Preprocessing has already *expanded*
        // the macro (its name is gone from the output), so we scan the source file
        // text itself, matching sccache's refusal to cache these. Direct use in the
        // TU is the overwhelmingly common case; header-introduced use is rare and
        // its only cost is a permanent miss, never incorrectness.
        if (SourceReferencesVolatileMacro(cmd.source))
            return Decline("uses __TIME__/__DATE__/__TIMESTAMP__; not caching (non-deterministic)");

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

        // The one disposition that is not merely a drop. A drive-relative path under
        // no root is keyed by nothing here and stat'ed by nothing in ReplayGuard, so
        // a header moved inside it replays a depfile naming a file that is gone
        // (issue #104). The note above has already said how many there were; this
        // says what the launcher does about them.
        //
        // This is the AUTHORITATIVE ask: it reads what the compiler actually opened,
        // where the command-line ask near the top of this function depends on
        // `PathValueFlags()` recognising the flag that carried the path. Read off the
        // tally rather than re-derived from the paths — `PortableForm` already
        // classified every one of them, and a second walk asking the same question
        // is a second place for the answer to drift.
        if (dependencies.Count(Cc::PathDisposition::DriveRelative) != 0)
        {
            return Decline("a reported dependency path is drive-relative under no root; not caching "
                           "(neither keyed nor guarded)");
        }

        // The same hazard reached by a different road, and it has to be its own ask
        // because `PortableForm` cannot see it: whether bytes are text is a property
        // of the HOST, and that function is pure. A path the reconciler could not
        // read went through untranslated, so it prefix-matches no root, keys as
        // toolchain and is stat'ed by nothing -- a moved header inside it would
        // replay a stored object under a zero exit code.
        //
        // Reachable only on Windows, and there only when a tool wrote a path in
        // neither UTF-8 nor the console output code page. The recovery is the
        // console: `chcp 65001` makes `cl` emit UTF-8 and this stops firing.
        if (reconciler.UnreadablePaths() != 0)
        {
            return Decline("a reported dependency path is not text this host can read; not caching "
                           "(neither keyed nor guarded) -- try a UTF-8 console code page");
        }

        // The preprocessed text is MOVED in, not copied: it runs to several
        // megabytes on a real translation unit, on the hot path of a parallel
        // build. Nothing moves back out again -- a worker preprocesses a second
        // time, with the `#line` markers this copy deliberately suppresses -- so
        // `inputs` dies with the block and the text with it, which is the whole
        // point of the block.
        Cc::KeyInputs const inputs {
            .compilerId = toolchainStamp,
            .preprocessed = std::move(probe->preprocessed),
            .relativizedArgs = relativizedArgs,
            .dependencyPaths = std::move(dependencies.keyed),
        };
        key = Cc::ComputeKey(inputs);

        // Moved out AFTER the key is computed, so the key path pays nothing: the
        // KEYED subset went into `inputs` above as its own copy, and what is left on
        // the probe is the full list, which only a dispatch reads and which would
        // otherwise be dropped with the block.
        if (dispatchConfigured)
            dispatchDependencies = probe->dependencyPaths;

        // Copied just above rather than moved when a scheduler is configured, and
        // moved here in every case: two consumers want the same full list, and the
        // manifest's is the one that runs on every build rather than only on a
        // distributed one.
        probed = { .paths = std::move(probe->dependencyPaths), .unreadable = probe->dependenciesUnreadable };
    }
    invocation.preprocessMs = MsSince(preprocessStarted);

    auto const cacheStarted = std::chrono::steady_clock::now();

    // How the fetch ended, for the two questions below: whether a MISS may be
    // traced, and whether the STORE is worth the transfer. Carried out of the block
    // rather than a `bool` derived from it, so both put the same question to
    // `Cc::CacheIsServing` instead of reading a summary somebody has to keep in
    // step with it. `Transport` is what `CacheOutcome` itself starts on and means
    // "we never got an answer", so it is the safe thing to hold for the two
    // statements before the exchange overwrites it -- not a guard for a path that
    // skips the assignment, because there is none.
    auto fetchKind = Cc::CacheOutcomeKind::Transport;

    // FETCH.
    {
        auto const outcome = Cc::RunOneExchange(cfg.addr, Notice(), Wire::EncodeFetch(key), cfg.credential, BudgetOf(cfg));
        fetchKind = outcome.kind;
        if (!Cc::CacheIsServing(fetchKind))
        {
            // Told apart, because an operator fixes them in different places. A
            // refusal is the daemon answering, so its own code and words become the
            // reason -- `--show-stats` then names a version mismatch instead of
            // lumping it in with an unreachable daemon. A transport failure has no
            // answer to quote: an endpoint that refused the connection, a peer that
            // broke mid-reply, or the budget running out, all under one FIXED
            // string so the tally gets a row per cause rather than one per compile.
            if (fetchKind == Cc::CacheOutcomeKind::Rejected)
                WarnAndCarryOn(Cc::DescribeOutcome(outcome));
            else
                WarnAndCarryOn("fetch exchange failed");
        }
        else if (outcome.IsHit())
        {
            auto decoded = DecodeCompileValue(outcome.value);
            if (!decoded.has_value())
            {
                NoteUndecodableValue("the fetched object", decoded.error());
                return Warn(DecodeFailureReason(decoded.error()));
            }

            // HIT: check what the value asserts, then write the object, reproduce
            // the depfile, and replay the streams (all with paths localized).
            //
            // Reproducing the depfile is not optional: skipping it silently breaks
            // incremental builds, because Ninja/Make would see no header
            // dependencies for this TU and stop rebuilding it when they change.
            auto const materialized = MaterializeHit(cmd, *decoded, layout, workingDirectory);
            if (materialized.disposition == HitDisposition::Unusable)
                return Warn("could not write object on hit");
            if (materialized.disposition == HitDisposition::Served)
            {
                invocation.valueBytes = decoded->objectBlob.size();
                invocation.cacheMs = MsSince(cacheStarted);

                // Backfill the direct-mode manifest from the hit we just served.
                //
                // Without this, direct mode could never populate on a cache that already
                // holds preprocessed-key entries: manifests would only ever be written by
                // the miss path, so a warm cache would preprocess forever.
                //
                // From THIS machine's probe rather than from the value's replayed
                // /showIncludes text, which is what it used to read. Both name the same
                // headers, but the replayed text was written by the producer's compiler
                // in the producer's language, so parsing it made the backfill work only
                // between machines that happened to share a locale (issue #692). The
                // probe already ran, so this still costs no compiler invocation.
                if (cfg.direct)
                    RecordManifest(
                        cfg, cmd, layout, workingDirectory, relativizedArgs, toolchainStamp, probed, key, reconciler);

                // The preprocessed-key hit, verified exactly as the direct-mode one
                // is. Both paths, because a wrong object served through either is the
                // same defect and a feature that covered one would be a feature an
                // operator could not rely on (#423).
                ReportVerification(VerifyServedObject(cmd, argv, key, cfg.verifyRate), key);
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
    // Only a daemon that answered produces a MISS. A refusal and a transport
    // failure have already recorded themselves as fall-backs, and tracing a MISS
    // over the top would clear the reason `--show-stats` ranks and report a broken
    // cache as a cold one -- the exact conflation `CacheOutcomeKind` was split to
    // end, arriving by a different road.
    if (Cc::CacheIsServing(fetchKind))
        TraceOutcome("MISS", key);

    // MISS: try a worker first when one is configured, then the real compiler.
    //
    // A dispatched compile is shaped to look exactly like a local one -- object on
    // disk at cmd.objPath, dependency record written, streams in hand -- so
    // everything below this point is unchanged and cannot tell the difference. That
    // is deliberate: the STORE, the manifest and the statistics all have one path,
    // and a second one would be a second place for them to diverge.
    auto run = dispatchConfigured ? TryRemoteCompile(cfg, cmd, argv, key, compilerBanner, targetTriple, dispatchDependencies)
                                  : std::optional<Cc::CompileRun> {};
    if (!run.has_value())
        run = RunCaptureSplit(argv);
    // Always surface the compiler's output on its true streams and its exit code.
    ReplayStreams(run->out, run->err);
    // A spawn failure reports `NotSpawned`, which a POSIX exit status truncates to 255 —
    // an arbitrary code no build system can interpret. Normalize it the same way
    // the fall-back path does.
    int const code = run->exitCode == Cc::NotSpawned ? 1 : run->exitCode;
    if (code != 0)
        return code; // do not cache a failed compile

    // Nothing more is offered to a daemon that refused this launcher, or that was
    // never reached: the fetch above established it, and everything below exists
    // only to store -- reading the object back, reconciling the captured regions,
    // encoding the value and pushing it, megabytes per translation unit, spent to
    // be told the same thing again.
    //
    // It is also what keeps the fall-through above from costing an extra connect
    // per translation unit against a wrong address. The connect BUDGET, and why
    // this is not a claim that one connect is all a dead cache costs, are in
    // .agent/rules/distributed-compilation.md.
    if (!Cc::CacheIsServing(fetchKind))
    {
        // Worded so it is true of BOTH kinds. A refusal *is* an answer -- the daemon
        // spoke and declined -- and saying it "did not answer" would send an
        // operator to look for an unreachable daemon that is in fact running and
        // rejecting, which is the rule this tree records about reporting a refusal
        // under the wrong reason. The fall-back reason above already names which.
        Note("the cache did not serve the fetch; not offering this object to it");
        return code;
    }

    auto const objectBytes = Cc::ReadFileBytes(cmd.objPath);
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

    // Asked AGAIN, after the regions above, and not only on the key path. Those
    // calls run the reconciler over every path span the grammars find in the
    // captured streams -- a wider set than the dependency list, and one the earlier
    // ask therefore cannot have covered. A span this host could not read went into
    // these regions untranslated, so the value would carry a spelling no consumer
    // can canonicalize, localize or check.
    if (reconciler.UnreadablePaths() != 0)
    {
        Note("a captured region names a path that is not text this host can read; not caching "
             "-- try a UTF-8 console code page");
        return code;
    }

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
                           Notice(),
                           Wire::EncodeStore(Wire::StoreRequest { .key = key,
                                                                  .prefetchGroup = cfg.prefetchGroup,
                                                                  .srcRoot = cfg.srcRoot,
                                                                  .buildTree = cfg.buildTree,
                                                                  .value = std::span<std::byte const> { encoded } }),
                           cfg.credential,
                           BudgetOf(cfg));
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
        RecordManifest(cfg, cmd, layout, workingDirectory, relativizedArgs, toolchainStamp, probed, key, reconciler);
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
    // Seeded before anything can dispatch, so the axis always says something true.
    // `NotConfigured` is an ABSENCE, not a failure: a launcher with no scheduler
    // reported as a failed dispatch would read as a 100% dispatch failure rate
    // forever -- which is precisely what `NoUpstream`'s honest `false` did to the
    // node's upstream-store figure. Every path that gets as far as asking overwrites
    // this; every path that does not leaves "a fleet was configured and this compile
    // never reached it", which is a different fact again.
    invocation.dispatch = DispatchConfigured(cfg) ? Cc::DispatchOutcome::NotAttempted : Cc::DispatchOutcome::NotConfigured;

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
            .dispatch = invocation.dispatch,
            .prefetchGroup = cfg.prefetchGroup,
            .source = cmd.source,
            .valueBytes = invocation.valueBytes,
            .elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
            .detail = invocation.outcomeDetail,
            .dispatchDetail = invocation.dispatchDetail,
            .preprocessMs = invocation.preprocessMs,
            .cacheMs = invocation.cacheMs,
            .directMs = invocation.directMs,
            .directHit = invocation.directHit,
        });
    }
    return code;
}
