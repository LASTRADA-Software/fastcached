// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "IProcessRunner.hpp"
#include "LauncherCli.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Where the `/showIncludes` marker this build emits and matches came from.
///
/// **A PRIVATE enum: nothing transmits or persists it.** It reaches one status line
/// and the precedence table below, both inside this process. No explicit `= N`, for
/// the reason `AGENT.md` gives -- on a private enum the numbers are noise, and adding
/// them would tell a later reader the ordinals mean something.
///
/// The ORDER is the precedence, most authoritative first. That is a property of the
/// ENUM, not of the table below: the table carries labels, and `ResolveIncludeNoteMarker`
/// spells one arm per source. A fourth source is an enumerator, a row AND an arm --
/// stated rather than dressed up as "a fourth source is a row", because each source asks
/// a genuinely different question and a walk that pretended otherwise would be three
/// `if`s wrapped in a loop.
enum class MarkerSource : std::uint8_t
{
    /// `FASTCACHE_MSVC_DEPS_PREFIX`. First because it is the only source that can be
    /// RIGHT about a build this launcher cannot see: the value lives in `build.ninja`
    /// as `msvc_deps_prefix`, and an operator who copied it there is stating a fact,
    /// not expressing a preference.
    Operator,

    /// Asked of the compiler, over a probe translation unit this launcher wrote.
    Discovered,

    /// Asked, and the compiler did not say. The VALUE is the English default, and that
    /// is exactly why this is not `Default`: the two carry the same string and send an
    /// operator to opposite places. Reaching here means somebody wrote
    /// `FASTCACHE_MSVC_DEPS_PREFIX=auto`, so *override with FASTCACHE_MSVC_DEPS_PREFIX*
    /// is advice they have already taken -- a confident wrong remedy, told to the one
    /// operator who opted in. Their remedy is to name the prefix outright.
    ///
    /// Fourth state of the four this file has to keep apart: told, asked-and-answered,
    /// asked-and-declined, never-asked. A three-valued provenance collapsed the last
    /// two, which is the failure it was introduced to prevent, one state along.
    DiscoveryDeclined,

    /// `PathCanon::IncludeNoteMarker`, the literal English prefix. What a build with
    /// no operator setting and no probe ASKED gets, which is what it got before any
    /// of this existed. Never asked -- a probe that was asked and declined is
    /// `DiscoveryDeclined`, however identical the string.
    Default,

    Last,
};

/// One resolution source, as the precedence table spells it.
struct MarkerSourceRow
{
    MarkerSource source;    ///< The row this is.
    std::string_view label; ///< What a `--verbose` line and `--show-stats` call it.
};

/// How each source is named on a status line, in enumerator order so a source indexes
/// its own label.
///
/// A table rather than a ternary because there are FOUR answers and they send an
/// operator to four different places: they already set the variable, the compiler was
/// asked and answered, the compiler was asked and would not say, or nobody asked at all
/// and this is English on trust. A bool carried two; three collapsed the middle pair.
///
/// The last two rows carry the SAME marker and differ only in the remedy, which is the
/// whole reason they are separate rows.
inline constexpr EnumTable<MarkerSource, MarkerSourceRow> MarkerSourceTable { {
    { .source = MarkerSource::Operator, .label = "named by FASTCACHE_MSVC_DEPS_PREFIX" },
    { .source = MarkerSource::Discovered, .label = "discovered from the compiler" },
    { .source = MarkerSource::DiscoveryDeclined,
      .label = "the compiler was asked and did not say, so the default; name it with FASTCACHE_MSVC_DEPS_PREFIX" },
    { .source = MarkerSource::Default, .label = "the default; override with FASTCACHE_MSVC_DEPS_PREFIX" },
} };

static_assert(RowsInEnumeratorOrder(MarkerSourceTable, &MarkerSourceRow::source),
              "MarkerSourceTable must hold one row per MarkerSource, in enumerator order -- the order is what "
              "lets a source index its own label");

// The three rows that NAME the variable must name the one the launcher actually reads.
// The previous status line formatted `Cc::EnvName::MsvcDepsPrefix` into the sentence, so
// the spelling could not drift; a label carrying it as a literal can, and nothing else
// would notice -- `check-launcher-env-reference.cmake` compares the declarations in
// `LauncherCli.hpp` against the documentation page and never reads this table, so a
// rename would leave the one line an operator is sent to naming a variable no build
// reads. A `static_assert` rather than a runtime check: this is a *do something*
// obligation, so it belongs to the type system.
static_assert(MarkerSourceTable[static_cast<std::size_t>(MarkerSource::Operator)].label.contains(EnvName::MsvcDepsPrefix),
              "the Operator row's label must name FASTCACHE_MSVC_DEPS_PREFIX as EnvName spells it");
static_assert(MarkerSourceTable[static_cast<std::size_t>(MarkerSource::Default)].label.contains(EnvName::MsvcDepsPrefix),
              "the Default row's label must name FASTCACHE_MSVC_DEPS_PREFIX as EnvName spells it -- it is the remedy "
              "an operator is told to apply");
static_assert(
    MarkerSourceTable[static_cast<std::size_t>(MarkerSource::DiscoveryDeclined)].label.contains(EnvName::MsvcDepsPrefix),
    "the DiscoveryDeclined row's label must name FASTCACHE_MSVC_DEPS_PREFIX as EnvName spells it -- it is the "
    "remedy for the operator who asked for discovery and did not get it");

// The two English-default rows must not read alike. They carry the same MARKER, so the
// label is the only thing telling an operator which of the two they are in -- and the
// one that is easy to get wrong is the one that looks harmless: a copy-paste making
// DiscoveryDeclined read like Default restores exactly the collapse the enumerator was
// added to end, with every test still passing because the marker is equal either way.
static_assert(MarkerSourceTable[static_cast<std::size_t>(MarkerSource::DiscoveryDeclined)].label
                  != MarkerSourceTable[static_cast<std::size_t>(MarkerSource::Default)].label,
              "asked-and-declined must not be spelled the same as never-asked; they differ only in the remedy");

/// How a status line spells one source.
///
/// `Last` indexes past the end, being the table's length rather than a source, so it is
/// answered rather than dereferenced -- nothing constructs a `ResolvedMarker` carrying
/// it, which is exactly why the read would go unnoticed if anything did.
///
/// @param source The row to name.
/// @return Its label, or `"unknown"`.
[[nodiscard]] constexpr std::string_view MarkerSourceLabel(MarkerSource source) noexcept
{
    auto const index = static_cast<std::size_t>(source);
    return index < MarkerSourceTable.size() ? MarkerSourceTable[index].label : std::string_view { "unknown" };
}

/// A marker and where it came from.
struct ResolvedMarker
{
    std::string marker;  ///< The prefix this build's notes carry.
    MarkerSource source; ///< Which row answered.
};

/// Read a `/showIncludes` marker out of a probe's own output.
///
/// **The DECISION, kept apart from the spawn**, so every rule below is testable on a
/// host with no MSVC at all -- which is every host this project's CI runs on.
///
/// The rule is *the line that ends in the path we know the probe includes, minus that
/// path*. Three things make that safe here and unsafe anywhere else:
///
/// - **The stream is the launcher's own probe, never the user's compile output.**
///   Measured: `#pragma message("see C:\\ci\\deep\\src\\a.h")` emits exactly
///   `see C:\ci\deep\src\a.h` on the same stream -- a line ending in a real
///   dependency-shaped path carrying no marker at all, from which this rule would
///   learn the prefix `see `. No localization is needed for that to exist, so the
///   heuristic is a design constraint on WHERE it may run rather than an argument
///   against it. The probe TU includes one header and defines no macros, so nothing
///   but the note can produce such a line.
/// - **Trailing blanks are trimmed off what is learned.** `cl` renders inclusion
///   depth as blanks BETWEEN the marker and the path (see
///   `PathCanon::IncludeNoteMarkerEnd` for the measurement), so a derivation that kept
///   them would learn a prefix whose length depends on which note it happened to read.
///   `PathCanon::IncludeNoteMarker` is spelled without a trailing space for the same
///   reason.
/// - **Nothing may precede the marker**, which is the anchor every reader in this tree
///   shares since [#1270](https://github.com/LASTRADA-Software/fastcached/issues/1270).
///   A candidate line with leading blanks is refused rather than trimmed.
///
/// **Disagreement is an answer, and the answer is *nothing*.** Two candidate lines
/// yielding different prefixes means the rule matched something that is not a note, so
/// this returns `nullopt` rather than the first or the commonest. A marker that is
/// WRONG is worse than no marker: an unmatched prefix normalizes nothing and stores the
/// producing checkout's absolute paths, while a wrongly-matched one re-spells a line no
/// consumer will look for.
///
/// The path comparison is case-insensitive and treats `/` and `\` alike, which is
/// deliberately MORE permissive than a byte compare and is justified only because this
/// runs on the MSVC family, where a driver legitimately echoes back a spelling of the
/// path that differs from the one it was given.
///
/// @param probeOutput Everything the probe compiler printed, both streams.
/// @param knownPath The absolute path of the header the probe TU includes.
/// @return The prefix its notes carry, or nullopt when nothing was learned.
[[nodiscard]] std::optional<std::string> MarkerFromProbeOutput(std::string_view probeOutput, std::string_view knownPath);

/// Asks a compiler what prefix its `/showIncludes` notes carry.
///
/// A seam rather than a free function because the answer costs a compiler spawn and a
/// pair of files, and `ResolveIncludeNoteMarker` must be drivable with neither.
class IMarkerDiscovery
{
  public:
    IMarkerDiscovery() = default;
    virtual ~IMarkerDiscovery() = default;
    IMarkerDiscovery(IMarkerDiscovery const&) = delete;
    IMarkerDiscovery& operator=(IMarkerDiscovery const&) = delete;
    IMarkerDiscovery(IMarkerDiscovery&&) = delete;
    IMarkerDiscovery& operator=(IMarkerDiscovery&&) = delete;

    /// @return The prefix this compiler's notes carry, or nullopt when it would not say.
    [[nodiscard]] virtual std::optional<std::string> Discover() = 0;
};

/// What `FASTCACHE_MSVC_DEPS_PREFIX` is set to when it asks for discovery.
///
/// A sentinel VALUE rather than a second variable, because the two settings answer one
/// question -- *what prefix do this build's notes carry* -- and a build has one answer.
/// Safe as a sentinel by inspection: no driver emits `auto` as a note prefix, and an
/// operator who somehow needed that literal is the operator who can spell it with a
/// trailing colon as every real prefix has.
inline constexpr std::string_view MarkerDiscoveryRequest = "auto";

/// Resolve the one marker this invocation will emit with and match against.
///
/// The three sources in `MarkerSource`'s own order, first one that answers. The table is
/// the LABELS and the enumerator order is the precedence; the arms are here, written out,
/// because each source asks a different question and a loop over the table would only
/// wrap three `if`s in an indirection.
///
/// @param operatorNamed What `FASTCACHE_MSVC_DEPS_PREFIX` named; empty for unset, since
///        an empty prefix is not one Ninja could match a note against, and empty also for
///        `MarkerDiscoveryRequest`, which names no prefix but asks for one.
/// @param discovery The probe, or nullptr when this invocation must not spawn one.
/// @return The marker and the row that supplied it; never empty.
[[nodiscard]] ResolvedMarker ResolveIncludeNoteMarker(std::string_view operatorNamed, IMarkerDiscovery* discovery);

/// The compiler probe, over a translation unit this launcher writes.
///
/// **Not memoized, and not persisted anywhere.** The value decides how a stored value's
/// notes are normalized and how a replayed one is re-spelled, so a stale answer is a
/// wrong answer that looks right -- installing a language pack changes the notes'
/// language without moving anything a cache stamp covers. That is the shape `AGENT.md`
/// names as NOT cacheable however expensive the probe, and it is `DiscoverTargetTriple`'s
/// own argument one field over.
///
/// **That is exactly why it is OPT-IN**, and the cost is why the opt-in is not a default
/// somebody can leave on by accident. A launcher process serves ONE translation unit, and
/// under CMake + Ninja + MSVC `/showIncludes` is on every compile line -- so "probe
/// whenever nobody named a prefix" is "spawn a second compiler for every file in the
/// build", paid on CACHE HITS too, where the whole value of the hit is that no compiler
/// ran. On an English install it would pay that to rediscover
/// `PathCanon::IncludeNoteMarker`, which is what the default already says. So the
/// operator asks for it, by setting `FASTCACHE_MSVC_DEPS_PREFIX` to
/// `MarkerDiscoveryRequest`, and a build that does is one whose notes are not English.
///
/// **Spawned in the build's OWN environment**, unlike every other probe in this
/// launcher. `RunCaptureSplitInEnglish` exists because the launcher must be able to READ
/// what it asked for, and forcing `VSLANG=1033` there is right. It would be exactly
/// wrong here: the question is what the BUILD's own compiles emit, and those are not
/// English-forced. A probe that anglicized itself would discover English on every
/// machine and report it with confidence.
///
/// Fails open. A compiler that cannot be spawned, a probe that writes no note, and a
/// note whose prefix cannot be derived are all `nullopt`, which leaves the caller on the
/// English default -- where it was before, rather than worse.
///
/// @param runner Process-spawning seam.
/// @param compiler The compiler to interrogate.
/// @param driver That compiler's table row; its `preprocessFlags` and
///        `dependencyProbeFlags` are what the probe spawns with, so this function spells
///        no driver flag of its own. `/EP` ALONE is a rule with a ticket behind it
///        (`AGENT.md`), and a second literal would sit outside its reach.
/// @return Its note prefix, or nullopt.
[[nodiscard]] std::optional<std::string> ProbeIncludeNoteMarker(IProcessRunner& runner,
                                                                std::string const& compiler,
                                                                DriverSpec const& driver);

} // namespace FastCache::Cc
