// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "IProcessRunner.hpp"

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
/// The ORDER is the precedence, most authoritative first, and `ResolveIncludeNoteMarker`
/// walks the table rather than branching. A fourth source is a row.
enum class MarkerSource : std::uint8_t
{
    /// `FASTCACHE_MSVC_DEPS_PREFIX`. First because it is the only source that can be
    /// RIGHT about a build this launcher cannot see: the value lives in `build.ninja`
    /// as `msvc_deps_prefix`, and an operator who copied it there is stating a fact,
    /// not expressing a preference.
    Operator,

    /// Asked of the compiler, over a probe translation unit this launcher wrote.
    Discovered,

    /// `PathCanon::IncludeNoteMarker`, the literal English prefix. What a build with
    /// no operator setting and no usable probe gets, which is what it got before any
    /// of this existed.
    Default,

    Last,
};

/// One resolution source, as the precedence table spells it.
struct MarkerSourceRow
{
    MarkerSource source;    ///< The row this is.
    std::string_view label; ///< What a `--verbose` line and `--show-stats` call it.
};

/// The precedence table, in enumerator order -- which IS the precedence, most
/// authoritative first, so `ResolveIncludeNoteMarker` walks it instead of branching
/// and a source indexes its own label.
inline constexpr EnumTable<MarkerSource, MarkerSourceRow> MarkerSourceTable { {
    { .source = MarkerSource::Operator, .label = "named by FASTCACHE_MSVC_DEPS_PREFIX" },
    { .source = MarkerSource::Discovered, .label = "discovered from the compiler" },
    { .source = MarkerSource::Default, .label = "the default; override with FASTCACHE_MSVC_DEPS_PREFIX" },
} };

static_assert(RowsInEnumeratorOrder(MarkerSourceTable, &MarkerSourceRow::source),
              "MarkerSourceTable must hold one row per MarkerSource, in enumerator order -- the order is the "
              "PRECEDENCE as well as the index, so a row out of place silently reorders resolution");

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

/// Resolve the one marker this invocation will emit with and match against.
///
/// Walks `MarkerSourceTable` in order and takes the first row that answers, so the
/// value stays ONE value with ONE place that decides it.
///
/// @param operatorNamed What `FASTCACHE_MSVC_DEPS_PREFIX` named; empty for unset, since
///        an empty prefix is not one Ninja could match a note against.
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
/// own argument one field over. What keeps the cost down instead is asking rarely: the
/// caller probes only when the operator named nothing AND this compile actually deals in
/// `/showIncludes`.
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
/// @return Its note prefix, or nullopt.
[[nodiscard]] std::optional<std::string> ProbeIncludeNoteMarker(IProcessRunner& runner, std::string const& compiler);

} // namespace FastCache::Cc
