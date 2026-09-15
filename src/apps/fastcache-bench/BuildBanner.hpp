// SPDX-License-Identifier: Apache-2.0
#pragma once

/// What build produced the figures this binary prints
/// ([#1439](https://github.com/LASTRADA-Software/fastcached/issues/1439)).
///
/// A timing is a quantity UNDER CONDITIONS, and the condition that decides whether it is a
/// cost at all is the build. Nothing in this binary's output used to say which build that
/// was, and a Debug bench prints a plausible number -- so an MSVC `cl-release` log was read
/// as "a Debug-build signature" while reviewing #1420, and the only tell was the build
/// directory's name.
///
/// ## The label is not the measurement
///
/// `CMAKE_BUILD_TYPE` arrives here as a compile definition and is printed as a LABEL,
/// deliberately never as evidence. A CMake variable says what the build system was ASKED
/// for: `CMakeCache.txt` has read `ENABLE_SANITIZER_ADDRESS:BOOL=ON` on a tree where not one
/// object was instrumented, and the configure log agreed with the cache. So every fact this
/// banner draws a VERDICT from is one the compiler itself states, as a predefined macro in
/// the translation unit that asks -- which is why `CurrentBuildConfiguration` is defined in
/// `BuildBanner.cpp`, compiled into this executable with this executable's flags, and not
/// inline in a header some other target could instantiate with its own.
///
/// ## Three answers, because "optimised" is not a fact `NDEBUG` carries
///
/// `RaftPeerFrameBench.cpp` used to spell its own `BuildKind` as `optimised (NDEBUG)`, which
/// is a claim `NDEBUG` cannot support: `-O0 -DNDEBUG` defines it and optimises nothing. gcc,
/// clang and clang-cl all state `__OPTIMIZE__` (measured; clang-cl defines it at `/O1` and
/// `/O2` and not at `/Od`), and `cl` states no optimiser macro at all. So a figure's standing
/// has THREE values -- a cost, not a cost, or unconfirmed -- and an `cl` build that cannot be
/// confirmed says so rather than claiming the better answer.
///
/// ## What is NOT covered
///
/// libc++'s `_LIBCPP_HARDENING_MODE` and MSVC's `_ITERATOR_DEBUG_LEVEL` are both set by their
/// standard library rather than predefined by the compiler, so reading them would mean
/// including a library header and comparing against its own level macros. They are absent
/// from `BuildFactTable` on purpose; `_DEBUG` covers the MSVC debug runtime, which is what a
/// CMake `Debug` configuration actually switches, and no row claims to answer for libc++.

#include <FastCache/Core/EnumTable.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Bench
{

/// What kind of thing a compile-time fact says about a build.
///
/// PRIVATE: nothing transmits or persists these, so the enumerators carry no explicit values
/// and may be reordered. `Last` is the count, per `Core/EnumTable.hpp`.
///
/// The kind is what makes the standing table below extensible: a new sanitizer is one
/// `BuildFact` row with `kind = Sanitizer`, and the rule that a sanitized figure is not a
/// cost keeps working without a second edit.
enum class FactKind : std::uint8_t
{
    Optimisation,   ///< What the optimiser did.
    CheckedRuntime, ///< Checking the shipped binary does not do, added by a library or by the compiler.
    Sanitizer,      ///< Instrumentation that rewrites every access.
    Informational,  ///< Worth printing, decides nothing.
    Last,
};

/// One compile-time fact about a build, named by the macro that states it.
///
/// PRIVATE, as `FactKind` is.
///
/// A fact is a row of `BuildFactTable` AND a row of `BuildBanner.cpp`'s `StatedHereTable`,
/// where the `#if` that reads its macro lives. Writing only one of the two is refused at
/// compile time, because both tables are checked by `RowsInEnumeratorOrder`.
enum class BuildFact : std::uint8_t
{
    AssertsCompiledOut,         ///< `NDEBUG`
    OptimiserRan,               ///< `__OPTIMIZE__`
    OptimisedForSize,           ///< `__OPTIMIZE_SIZE__`
    InliningOff,                ///< `__NO_INLINE__`
    MsvcDebugRuntime,           ///< `_DEBUG`
    MsvcRuntimeChecks,          ///< `__MSVC_RUNTIME_CHECKS`
    LibstdcxxAssertions,        ///< `_GLIBCXX_ASSERTIONS`
    LibstdcxxDebugMode,         ///< `_GLIBCXX_DEBUG`
    AddressSanitizer,           ///< `__SANITIZE_ADDRESS__` or `__has_feature(address_sanitizer)`
    ThreadSanitizer,            ///< `__SANITIZE_THREAD__` or `__has_feature(thread_sanitizer)`
    MemorySanitizer,            ///< `__has_feature(memory_sanitizer)`
    UndefinedBehaviorSanitizer, ///< `__has_feature(undefined_behavior_sanitizer)`
    Last,
};

/// Whether this compiler says anything about the optimiser at all.
///
/// PRIVATE. Measured per driver rather than assumed: gcc, clang and clang-cl define
/// `__OPTIMIZE__` from `-O1`/`/O1` upwards, and `cl` defines no optimiser macro, so its
/// absence there means nothing.
enum class OptimiserEvidence : std::uint8_t
{
    Stated,                  ///< This compiler defines `__OPTIMIZE__` when it optimises, so absence means OFF.
    NotStatedByThisCompiler, ///< This compiler never defines it, so absence says nothing.
    Last,
};

/// What the optimiser did, as far as the compiler is willing to say.
///
/// PRIVATE.
enum class Optimiser : std::uint8_t
{
    Ran,       ///< `__OPTIMIZE__` is defined.
    DidNotRun, ///< This compiler states the optimiser and did not define `__OPTIMIZE__`.
    Unstated,  ///< This compiler states nothing, so nothing is known.
    Last,
};

/// Whether `assert()` runs inside the measured bodies.
///
/// PRIVATE.
enum class Asserts : std::uint8_t
{
    Live,        ///< `NDEBUG` is not defined.
    CompiledOut, ///< `NDEBUG` is defined.
    Last,
};

/// What a timing from a build may be read as.
///
/// PRIVATE. Three values rather than two: *unconfirmed* is not *a cost*, and reporting it as
/// one would be exactly the confident wrong signal this banner exists to stop.
enum class Standing : std::uint8_t
{
    ACost,       ///< Every fact needed to read a figure as a cost is confirmed.
    NotACost,    ///< A stated fact says the measured body is not the one that ships.
    Unconfirmed, ///< Nothing contradicts the figures, and the optimiser could not be confirmed either.
    Last,
};

/// Why a build's figures stand as they do.
///
/// PRIVATE, and the **enumerator order is the precedence**: `ReasonFor` answers with the
/// first row whose predicate holds, so a build that is both unoptimised and sanitized is
/// reported as unoptimised. `Confirmed` is the terminal row and its predicate is always true.
enum class StandingReason : std::uint8_t
{
    AssertsLive,           ///< `NDEBUG` is not defined.
    OptimiserDidNotRun,    ///< The compiler states the optimiser and says it did not run.
    CheckedRuntime,        ///< A `FactKind::CheckedRuntime` fact is stated.
    SanitizerInstrumented, ///< A `FactKind::Sanitizer` fact is stated.
    OptimiserUnstated,     ///< Nothing above holds and this compiler does not state the optimiser.
    Confirmed,             ///< Nothing above holds.
    Last,
};

/// What this executable's own translation unit saw of the build that produced it.
struct BuildConfiguration
{
    /// What the build system was ASKED for, from `FASTCACHE_BENCH_BUILD_TYPE`. A label; empty
    /// where the generator resolves the configuration at build time rather than at configure
    /// time. No verdict is drawn from it.
    std::string_view buildTypeLabel;

    /// The compiler's own identification of itself, from its own version macros.
    std::string_view compiler;

    /// Every fact whose macro the compiler stated, in `BuildFact` enumerator order.
    std::vector<BuildFact> stated;

    /// Whether the absence of `__OPTIMIZE__` means anything on this compiler.
    OptimiserEvidence optimiserEvidence;
};

/// One row of `BuildFactTable`.
struct BuildFactRow
{
    BuildFact fact;           ///< The fact this row describes.
    FactKind kind;            ///< What kind of thing it says.
    std::string_view macro;   ///< How the compiler spells it.
    std::string_view meaning; ///< What its presence means, as the banner prints it.
};

/// One row of `StandingTable`.
struct StandingRow
{
    StandingReason reason;                            ///< The reason this row describes.
    Standing standing;                                ///< What it makes of a figure.
    bool (*holds)(BuildConfiguration const& reading); ///< Whether this reason applies to @p reading.
    std::string_view sentence;                        ///< Why, as the banner prints it.
};

/// One row of `MarkerTable`.
struct MarkerRow
{
    Standing standing;       ///< The standing this row describes.
    char emphasis;           ///< The character a banner rule around this verdict is made of.
    std::string_view marker; ///< The tag every figure from such a build carries.
};

/// One line of the banner: what it is called, and how it reads off a configuration.
struct BannerLine
{
    std::string_view label;                                 ///< The line's name.
    std::string (*read)(BuildConfiguration const& reading); ///< How it reads off @p reading.
};

/// One figure, with both of the numbers Catch2 prints for it.
///
/// The two are here together because confusing them is half of #1439: `est run time` sits on
/// the same line as the case name and is the larger number, so it is the one a reader reaches
/// for, and it is samples times iterations rather than a per-operation cost.
struct Figure
{
    std::string_view name;       ///< The benchmark's name, as Catch2 reports it.
    double meanNanoseconds;      ///< The per-iteration MEAN. This is the per-operation cost.
    double estimatedNanoseconds; ///< Catch2's `est run time`: the whole run, and not a cost.
    std::size_t samples;         ///< How many samples the mean is over.
};

/// One row per `BuildFact`, in enumerator order.
inline constexpr EnumTable<BuildFact, BuildFactRow> BuildFactTable { {
    { .fact = BuildFact::AssertsCompiledOut,
      .kind = FactKind::Informational,
      .macro = "NDEBUG",
      .meaning = "assert() is compiled out" },
    { .fact = BuildFact::OptimiserRan,
      .kind = FactKind::Optimisation,
      .macro = "__OPTIMIZE__",
      .meaning = "the optimiser ran" },
    { .fact = BuildFact::OptimisedForSize,
      .kind = FactKind::Informational,
      .macro = "__OPTIMIZE_SIZE__",
      .meaning = "optimised for size rather than for speed" },
    { .fact = BuildFact::InliningOff,
      .kind = FactKind::Informational,
      .macro = "__NO_INLINE__",
      .meaning = "inlining is off" },
    { .fact = BuildFact::MsvcDebugRuntime,
      .kind = FactKind::CheckedRuntime,
      .macro = "_DEBUG",
      .meaning = "the MSVC debug runtime is linked, so the standard library checks every container operation" },
    { .fact = BuildFact::MsvcRuntimeChecks,
      .kind = FactKind::CheckedRuntime,
      .macro = "__MSVC_RUNTIME_CHECKS",
      .meaning = "MSVC's /RTC instrumentation is on, which /O is incompatible with" },
    { .fact = BuildFact::LibstdcxxAssertions,
      .kind = FactKind::CheckedRuntime,
      .macro = "_GLIBCXX_ASSERTIONS",
      .meaning = "libstdc++ checks its own preconditions" },
    { .fact = BuildFact::LibstdcxxDebugMode,
      .kind = FactKind::CheckedRuntime,
      .macro = "_GLIBCXX_DEBUG",
      .meaning = "libstdc++ debug mode replaces every container" },
    { .fact = BuildFact::AddressSanitizer,
      .kind = FactKind::Sanitizer,
      .macro = "__SANITIZE_ADDRESS__",
      .meaning = "AddressSanitizer instruments every access" },
    { .fact = BuildFact::ThreadSanitizer,
      .kind = FactKind::Sanitizer,
      .macro = "__SANITIZE_THREAD__",
      .meaning = "ThreadSanitizer instruments every access" },
    { .fact = BuildFact::MemorySanitizer,
      .kind = FactKind::Sanitizer,
      .macro = "__has_feature(memory_sanitizer)",
      .meaning = "MemorySanitizer instruments every read" },
    { .fact = BuildFact::UndefinedBehaviorSanitizer,
      .kind = FactKind::Sanitizer,
      .macro = "__has_feature(undefined_behavior_sanitizer)",
      .meaning = "UndefinedBehaviorSanitizer adds a check per operation" },
} };

static_assert(RowsInEnumeratorOrder(BuildFactTable, &BuildFactRow::fact),
              "every BuildFact needs a row of BuildFactTable, at its own index");

/// Whether @p reading states @p fact.
/// @param reading The configuration to ask.
/// @param fact The fact to look for.
/// @return True when the compiler stated it.
[[nodiscard]] bool Stated(BuildConfiguration const& reading, BuildFact fact);

/// Whether @p reading states any fact of @p kind.
/// @param reading The configuration to ask.
/// @param kind The kind to look for.
/// @return True when at least one stated fact carries that kind.
[[nodiscard]] bool StatedAnyOfKind(BuildConfiguration const& reading, FactKind kind);

/// Whether `assert()` runs inside the measured bodies of @p reading.
/// @param reading The configuration to ask.
/// @return `Live` unless `NDEBUG` was stated.
[[nodiscard]] Asserts AssertsOf(BuildConfiguration const& reading);

/// What @p reading's compiler said about the optimiser.
/// @param reading The configuration to ask.
/// @return `Ran`, `DidNotRun`, or `Unstated` where absence means nothing.
[[nodiscard]] Optimiser OptimiserOf(BuildConfiguration const& reading);

/// One row per `StandingReason`, in enumerator order, which is precedence order.
inline constexpr EnumTable<StandingReason, StandingRow> StandingTable { {
    { .reason = StandingReason::AssertsLive,
      .standing = Standing::NotACost,
      .holds = +[](BuildConfiguration const& reading) { return AssertsOf(reading) == Asserts::Live; },
      .sentence = "asserts are LIVE (NDEBUG is not defined), so every measured body runs checks the shipped "
                  "binary does not" },
    { .reason = StandingReason::OptimiserDidNotRun,
      .standing = Standing::NotACost,
      .holds = +[](BuildConfiguration const& reading) { return OptimiserOf(reading) == Optimiser::DidNotRun; },
      .sentence = "the optimiser did NOT run (this compiler states __OPTIMIZE__ and did not define it)" },
    { .reason = StandingReason::CheckedRuntime,
      .standing = Standing::NotACost,
      .holds = +[](BuildConfiguration const& reading) { return StatedAnyOfKind(reading, FactKind::CheckedRuntime); },
      .sentence = "a checked runtime or standard library is in the measured path" },
    { .reason = StandingReason::SanitizerInstrumented,
      .standing = Standing::NotACost,
      .holds = +[](BuildConfiguration const& reading) { return StatedAnyOfKind(reading, FactKind::Sanitizer); },
      .sentence = "a sanitizer is instrumenting this binary" },
    { .reason = StandingReason::OptimiserUnstated,
      .standing = Standing::Unconfirmed,
      .holds = +[](BuildConfiguration const& reading) { return OptimiserOf(reading) == Optimiser::Unstated; },
      .sentence = "this compiler states no optimiser macro, so whether the optimiser ran could not be confirmed "
                  "from inside the binary -- the build type printed above is a label and is not evidence" },
    { .reason = StandingReason::Confirmed,
      .standing = Standing::ACost,
      .holds = +[](BuildConfiguration const&) { return true; },
      .sentence = "asserts are compiled out, the optimiser ran, and no sanitizer or checked runtime is in the "
                  "measured path" },
} };

static_assert(RowsInEnumeratorOrder(StandingTable, &StandingRow::reason),
              "every StandingReason needs a row of StandingTable, at its own index");

/// One row per `Standing`, in enumerator order.
inline constexpr EnumTable<Standing, MarkerRow> MarkerTable { {
    { .standing = Standing::ACost, .emphasis = '-', .marker = "a cost" },
    { .standing = Standing::NotACost, .emphasis = '!', .marker = "NOT A COST" },
    { .standing = Standing::Unconfirmed, .emphasis = '?', .marker = "UNCONFIRMED" },
} };

static_assert(RowsInEnumeratorOrder(MarkerTable, &MarkerRow::standing),
              "every Standing needs a row of MarkerTable, at its own index");

/// Why @p reading's figures stand as they do.
/// @param reading The configuration to judge.
/// @return The first `StandingTable` row whose predicate holds; `Confirmed` when none above does.
[[nodiscard]] StandingReason ReasonFor(BuildConfiguration const& reading);

/// What @p reason makes of a figure.
/// @param reason The reason to read.
/// @return The standing its row carries.
[[nodiscard]] Standing StandingFor(StandingReason reason);

/// The banner's lines, in the order it prints them.
/// @return One entry per line, each naming how it reads off a configuration.
[[nodiscard]] std::span<BannerLine const> BannerLines();

/// The verdict block for @p reading: a rule, one sentence, a rule.
///
/// Its own function because a long run scrolls the banner away, so the block is printed again
/// when the run ends -- and printing it twice from one renderer is what keeps the two
/// identical.
///
/// @param reading The configuration to judge.
/// @return Three newline-terminated lines.
[[nodiscard]] std::string RenderVerdict(BuildConfiguration const& reading);

/// The whole banner for @p reading, newline-terminated and ending in its verdict block.
/// @param reading The configuration to describe.
/// @return The text to write, which is several lines.
[[nodiscard]] std::string RenderBuildBanner(BuildConfiguration const& reading);

/// One figure's line, naming `mean` and carrying @p reason's marker.
/// @param figure The numbers Catch2 reported.
/// @param reason Why this build's figures stand as they do.
/// @return One newline-terminated line.
[[nodiscard]] std::string RenderFigure(Figure const& figure, StandingReason reason);

/// The build that produced THIS executable, as its own translation unit sees it.
///
/// Defined in `BuildBanner.cpp` and deliberately not inline: one definition, compiled with
/// this executable's flags, so the answer is about this binary rather than about whichever
/// translation unit a linker happened to keep.
///
/// @return The configuration, read from the compiler's own predefined macros.
[[nodiscard]] BuildConfiguration CurrentBuildConfiguration();

} // namespace FastCache::Bench
