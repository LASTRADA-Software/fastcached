// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliVerbs.hpp"
#include "DashboardLoop.hpp"
#include "DashboardPanels.hpp"
#include "FleetReading.hpp"
#include "LivePipedView.hpp"
#include "NodeClient.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file LiveStats.hpp
/// What `fastcache-cli live-stats` watches, and whether it may start.
///
/// **Admission is pure and decided before anything is rendered.** The subject, the
/// interval and the bound are settled here over injected seams -- the operands, the
/// modifiers, and an `IEndpointIdentity` -- so every refusal §9 names is a unit test
/// with no socket and no terminal. The session that runs once admission says yes lives
/// elsewhere; nothing in this file draws a frame.

/// What a `live-stats` session watches.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted, and
/// a fourth subject is a row of `LiveSubjectTable`.
enum class LiveSubject : std::uint8_t
{
    Cache, ///< A cache daemon's own counters.
    Node,  ///< One compile node's counters.
    Fleet, ///< The fleet as its leader sees it.
    Last,
};

/// A set of endpoint kinds: every kind that serves one subject.
class RemoteKinds
{
  public:
    /// @param kinds The kinds the set holds.
    /// @return The set holding exactly @p kinds.
    [[nodiscard]] static constexpr RemoteKinds Of(std::initializer_list<RemoteKind> kinds) noexcept
    {
        auto set = RemoteKinds {};
        for (auto const kind: kinds)
            set._bits = static_cast<std::uint8_t>(set._bits | Bit(kind));
        return set;
    }

    /// @param kind The kind asked about.
    /// @return Whether the set holds @p kind.
    [[nodiscard]] constexpr bool Contains(RemoteKind kind) const noexcept
    {
        return (_bits & Bit(kind)) != 0;
    }

  private:
    /// @param kind A kind.
    /// @return Its bit, at the enumerator's value.
    [[nodiscard]] static constexpr unsigned Bit(RemoteKind kind) noexcept
    {
        return 1U << static_cast<unsigned>(kind);
    }

    std::uint8_t _bits { 0 }; ///< One bit per `RemoteKind`.
};

static_assert(EnumeratorCount<RemoteKind> <= 8, "RemoteKinds keeps one bit per RemoteKind in eight");

/// The panel a subject's interactive session draws, as `CachePanel` and `NodePanel` hand it out.
using PanelOf = PanelSpec const& (*) () noexcept;

/// One subject's fixed properties.
struct LiveSubjectSpec
{
    LiveSubject subject;  ///< The enumerator this row describes.
    std::string_view key; ///< As an operator types it, and as the operand list spells it.

    /// The kind of endpoint an operator who names no subject is given this one at, or none.
    ///
    /// **None for `fleet`, and that is the reason the operand exists at all.** A node
    /// that leads a scheduler is both a machine and the fleet's leader; both readings of
    /// `live-stats <that node>` are true and nothing in the endpoint can pick between
    /// them. Naming the subject is how the operator says which question they are asking.
    ///
    /// **Its own column, not derived from `servedBy`**, because serving is not inferring: a
    /// compile node serves `cache` as well as `node`, and an unnamed session there is still
    /// the node. An unnamed subject is the one row whose `inferredAt` is what the endpoint
    /// is -- unique by `EveryKindInfersAtMostOneSubject`, and servable by
    /// `EveryInferredSubjectIsServed` -- so no ladder of `if kind == ...` says it twice.
    std::optional<RemoteKind> inferredAt;

    /// Every kind of endpoint that serves this subject.
    ///
    /// **A set** (#1399): `cache` is streamed by the cache daemon and by every compile node,
    /// whose `0xFC` surface captures its own cache tier in the same grammar. One kind here
    /// refused `live-stats cache` against a node that would have granted the stream.
    RemoteKinds servedBy;

    /// What an endpoint must be to serve this subject, in words for a refusal.
    ///
    /// On the subject's row rather than derived from `servedBy`, because it says what
    /// the SUBJECT watches: `node` and `fleet` are served by the same kind and need
    /// different things said about them.
    std::string_view needs;

    /// What this subject is on the wire: what a `SUBSCRIBE` names, and whose floor applies.
    ///
    /// **The floor is the wire's, never a second number here** (#1399). A server clamps a cadence
    /// below its floor, so a client floor of its own could only ever disagree with it: higher, and an
    /// operator is refused a cadence the server would grant; lower, and a typed interval is silently
    /// replaced. Admission reads `CompileCacheWire::LiveSubjectTable` through this column instead.
    CompileCacheWire::LiveSubject wire;

    /// The interval used when the operator gives none.
    std::chrono::milliseconds defaultInterval;

    /// Who pays for one snapshot, as the tail of the floor refusal.
    ///
    /// A column rather than a comment so the reason travels with the number: `fleet`'s floor is
    /// higher because each tick renders the whole fleet on the leader -- once, however many watch,
    /// but that cost still lands where the operator is not looking.
    std::string_view costsWhom;

    /// What one of this subject's samples reads as: the reader its session's loop is given.
    ///
    /// Never null (`EverySubjectHasAReader`): a subject a session could be admitted for and then not
    /// read would be a refusal written as a missing row. `fleet`'s is `ReadFleetSample`, over the
    /// leader's whole document.
    SampleReader reader;

    /// The panel an interactive session of this subject draws.
    ///
    /// **A column, so the rung's view never branches on a subject**: the composition asks the row
    /// and draws whatever panel it names. Never null (`EverySubjectHasAPanel`): a subject an operator
    /// can name at a terminal is one the terminal can draw, so no session is refused for want of one.
    PanelOf panel;

    /// What a piped session of this subject streams per sample, or null where this build has nothing.
    ///
    /// The panel's own figures for `cache` and `node` (`PanelFigures`), so a piped header names what
    /// an interactive run draws, and the leader's KPI section for `fleet` (`FleetKpiFigures`). A null
    /// projection refuses a piped session by name.
    FigureProjection figures;
};

/// The subjects, one row per enumerator, in enumerator order.
inline constexpr EnumTable<LiveSubject, LiveSubjectSpec> LiveSubjectTable { {
    { .subject = LiveSubject::Cache,
      .key = "cache",
      .inferredAt = RemoteKind::FastcacheWireOnly,
      .servedBy = RemoteKinds::Of({ RemoteKind::FastcacheWireOnly, RemoteKind::CompileNode }),
      .needs = "a fastcached or a fastcache-compile-node",
      .wire = CompileCacheWire::LiveSubject::Cache,
      .defaultInterval = std::chrono::milliseconds { 2000 },
      .costsWhom = "each tick is captured by the cache daemon itself",
      .reader = &ReadStatsSample,
      .panel = &CachePanel,
      .figures = &CacheFigures },
    { .subject = LiveSubject::Node,
      .key = "node",
      .inferredAt = RemoteKind::CompileNode,
      .servedBy = RemoteKinds::Of({ RemoteKind::CompileNode }),
      .needs = "a fastcache-compile-node",
      .wire = CompileCacheWire::LiveSubject::Node,
      .defaultInterval = std::chrono::milliseconds { 2000 },
      .costsWhom = "each tick is captured by the node itself",
      .reader = &ReadStatsSample,
      .panel = &NodePanel,
      .figures = &NodeFigures },
    { .subject = LiveSubject::Fleet,
      .key = "fleet",
      .inferredAt = std::nullopt,
      .servedBy = RemoteKinds::Of({ RemoteKind::CompileNode }),
      .needs = "a fastcache-compile-node (the fleet page is served by the one that leads)",
      .wire = CompileCacheWire::LiveSubject::Fleet,
      .defaultInterval = std::chrono::milliseconds { 5000 },
      .costsWhom = "each tick makes the LEADER render every fleet section for the whole fleet",
      .reader = &ReadFleetSample,
      .panel = &FleetPanel,
      .figures = &FleetKpiFigures },
} };

static_assert(RowsInEnumeratorOrder(LiveSubjectTable, &LiveSubjectSpec::subject),
              "LiveSubjectTable must hold one row per LiveSubject, in enumerator order");

/// Whether inference can pick at most one subject for any endpoint.
///
/// Two rows inferred at one kind would make an unnamed `live-stats` against that endpoint
/// ambiguous, and a first-match lookup would resolve it by table order in silence. All
/// three subjects are served by `CompileNode` and only `node` is inferred there -- and this
/// is the check that notices the day somebody gives `cache` or `fleet` that kind too.
/// @param table The subject table.
/// @return True when no kind is the `inferredAt` of two rows.
[[nodiscard]] consteval bool EveryKindInfersAtMostOneSubject(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::none_of(table, [&table](LiveSubjectSpec const& row) {
        return row.inferredAt.has_value() && std::ranges::count(table, row.inferredAt, &LiveSubjectSpec::inferredAt) > 1;
    });
}

static_assert(EveryKindInfersAtMostOneSubject(LiveSubjectTable),
              "two live-stats subjects are inferred at one RemoteKind, so inference would be ambiguous");

/// Whether every subject is inferred only at a kind that serves it.
///
/// An inference outside `servedBy` would admit an unnamed session the admission then refuses
/// as a mismatch, naming a subject the operator never typed.
/// @param table The subject table.
/// @return True when each engaged `inferredAt` is in its row's `servedBy`.
[[nodiscard]] consteval bool EveryInferredSubjectIsServed(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::all_of(table, [](LiveSubjectSpec const& row) {
        return !row.inferredAt.has_value() || row.servedBy.Contains(*row.inferredAt);
    });
}

static_assert(EveryInferredSubjectIsServed(LiveSubjectTable),
              "a live-stats subject is inferred at a RemoteKind that does not serve it");

/// Whether every subject names the reader its samples are read with.
/// @param table The subject table.
/// @return True when no row's reader is null.
[[nodiscard]] consteval bool EverySubjectHasAReader(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::all_of(table, [](LiveSubjectSpec const& row) { return row.reader != nullptr; });
}

static_assert(EverySubjectHasAReader(LiveSubjectTable), "every live-stats subject needs a reader for its samples");

/// Whether every subject names the panel an interactive session of it draws.
/// @param table The subject table.
/// @return True when no row's panel is null.
[[nodiscard]] consteval bool EverySubjectHasAPanel(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::all_of(table, [](LiveSubjectSpec const& row) { return row.panel != nullptr; });
}

static_assert(EverySubjectHasAPanel(LiveSubjectTable), "every live-stats subject needs a panel for an interactive session");

/// The shortest `--interval` a subject accepts: its wire floor. At the floor is accepted.
/// @param row The subject.
/// @return The server's floor for it.
[[nodiscard]] constexpr std::chrono::milliseconds FloorOf(LiveSubjectSpec const& row) noexcept
{
    auto const* const wire = CompileCacheWire::FindLiveSubject(static_cast<std::uint8_t>(row.wire));
    return wire != nullptr ? wire->floor : CompileCacheWire::MaxLiveCadence;
}

/// Whether every subject names a wire subject this build serves, and its default is a cadence the
/// server grants as asked.
///
/// A default below the floor or above the ceiling is a contradiction the operator would meet the
/// first time they typed back the interval they had been given.
/// @param table The subject table.
/// @return True when every row's default lies within its wire bounds.
[[nodiscard]] consteval bool EveryDefaultIsGrantedAsAsked(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::all_of(table, [](LiveSubjectSpec const& row) {
        return CompileCacheWire::FindLiveSubject(static_cast<std::uint8_t>(row.wire)) != nullptr
               && row.defaultInterval >= FloorOf(row) && row.defaultInterval <= CompileCacheWire::MaxLiveCadence;
    });
}

static_assert(EveryDefaultIsGrantedAsAsked(LiveSubjectTable),
              "a live-stats subject's default interval is outside what its wire subject grants");

namespace Detail
{
    /// How long the operand list is once spelled from the table.
    /// @return `" ["` + the keys joined by `|` + `"]"`.
    [[nodiscard]] consteval std::size_t LiveSubjectOperandsLength() noexcept
    {
        auto length = std::string_view { " ]" }.size();
        for (auto const& row: LiveSubjectTable)
            length += 1 + row.key.size(); // the `[` or `|` before the key, and the key
        return length;
    }

    /// The operand list, spelled from the table at compile time.
    /// @return The characters, unterminated.
    [[nodiscard]] consteval auto LiveSubjectOperandsText() noexcept
    {
        auto text = std::array<char, LiveSubjectOperandsLength()> {};
        auto at = std::size_t { 0 };
        auto const put = [&text, &at](std::string_view piece) {
            for (auto const ch: piece)
                text.at(at++) = ch;
        };
        put(" ");
        auto separator = std::string_view { "[" };
        for (auto const& row: LiveSubjectTable)
        {
            put(separator);
            put(row.key);
            separator = "|";
        }
        put("]");
        return text;
    }

    /// The characters `LiveSubjectOperands` views; static storage, so the view is valid in
    /// the verb table's constant expression.
    inline constexpr auto LiveSubjectOperandsStorage = LiveSubjectOperandsText();
} // namespace Detail

/// `live-stats`' operand list, as `--help` shows it: ` [cache|node|fleet]`.
///
/// **Derived from `LiveSubjectTable`, never written out** (#134 §1). A fourth subject is a
/// row, and a hand-written list is the one place it would be forgotten.
inline constexpr std::string_view LiveSubjectOperands { Detail::LiveSubjectOperandsStorage.data(),
                                                        Detail::LiveSubjectOperandsStorage.size() };

/// The row whose key @p key is.
/// @param key The operand as typed.
/// @return The row, or nullptr for a word that names no subject.
[[nodiscard]] LiveSubjectSpec const* FindLiveSubject(std::string_view key) noexcept;

/// What an admitted `live-stats` session will do.
struct LivePlan
{
    LiveSubject subject { LiveSubject::Cache }; ///< What it watches.
    /// What answered admission, which a panel titled by its server names: a node's cache is not `fastcached`.
    RemoteKind server { RemoteKind::FastcacheWireOnly };
    std::chrono::milliseconds interval {}; ///< Time between samples.
    std::size_t samples { 0 };             ///< Stop after this many; 0 means no bound.
    std::string endpoint {};               ///< What the endpoint turned out to be, in words.
};

/// Decide whether @p context may start a `live-stats` session, and what it watches.
///
/// **Everything that can be refused without asking the endpoint is refused first**: an
/// operand naming no subject, and a named subject's interval below its floor, cost no
/// identification. Only then is the endpoint identified -- to infer an unnamed subject, or
/// to check a named one is something this endpoint can serve.
///
/// The refusals, and why each is the outcome it is:
///   - an operand naming no subject, or an interval below the subject's floor: `Usage`,
///     naming the floor, the subject it belongs to and who pays for a sample;
///   - nobody could ask what the endpoint is: `Unreachable`, naming why and pointing at the
///     address -- **never** silently `cache`, and never *name a subject*, which leads back
///     here;
///   - the endpoint framed nothing recognisable: `Protocol`, because the port answered in
///     a protocol this client does not speak;
///   - a named subject the endpoint cannot serve, or no subject to infer: the outcome the
///     endpoint's kind establishes (`RemoteKindTable`), naming what it turned out to be,
///     which is the half a bare "refused" leaves out.
///
/// @param context The invocation; its `identity` is what the endpoint is asked through.
/// @return The plan, or the answer that refuses it.
[[nodiscard]] std::expected<LivePlan, Answer> AdmitLiveStats(VerbContext const& context);

/// `live-stats` through the verb table's synchronous door: the admission, answered.
///
/// **Not how the view runs.** A session is a stream of samples rather than one answer,
/// so it cannot be a `VerbHandler` -- it is dispatched on its own column, and that column
/// arrives with the session it names. This is the verb table's door onto the SAME
/// admission, so `RunVerb` stays total over every row and the refusals are reachable the
/// way every other verb's are: a refusal is answered as one, and an admitted plan as a
/// record of what would be watched.
/// @param context The invocation.
/// @return The refusal, or the admitted plan as a record.
[[nodiscard]] Answer LiveStatsVerb(VerbContext const& context);

} // namespace FastCache::Cli
