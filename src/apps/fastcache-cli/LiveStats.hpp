// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliVerbs.hpp"
#include "DashboardLoop.hpp"
#include "NodeClient.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
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

/// One subject's fixed properties.
struct LiveSubjectSpec
{
    LiveSubject subject;  ///< The enumerator this row describes.
    std::string_view key; ///< As an operator types it, and as the operand list spells it.

    /// Whether an operator who names no subject can be given this one.
    ///
    /// **False for `fleet`, and that is the reason the operand exists at all.** A node
    /// that leads a scheduler is both a machine and the fleet's leader; both readings of
    /// `live-stats <that node>` are true and nothing in the endpoint can pick between
    /// them. Naming the subject is how the operator says which question they are asking.
    bool inferrable;

    /// What an endpoint must turn out to be to serve this subject.
    ///
    /// **Inference is DERIVED from this column rather than written beside it**: an
    /// unnamed subject is the one inferrable row whose `servedBy` matches what the
    /// endpoint is, and `EveryKindInfersAtMostOneSubject` proves that choice is unique.
    /// A second ladder of `if kind == ... then subject = ...` would be a second place for
    /// the two to disagree.
    RemoteKind servedBy;

    /// What an endpoint must be to serve this subject, in words for a refusal.
    ///
    /// On the subject's row rather than looked up from `servedBy`, because it says what
    /// the SUBJECT watches: `node` and `fleet` share `servedBy` and need different things
    /// said about them.
    std::string_view needs;

    /// The shortest `--interval` accepted for this subject. At the floor is accepted.
    ///
    /// **A proposal, not a measurement** (#134 §1.4): nobody has timed a `/fleet.txt`
    /// render at 40 machines. What the ticket asks for is the SHAPE -- a floor per
    /// subject, stated as data -- and whoever measures corrects the row.
    std::chrono::milliseconds minInterval;

    /// The interval used when the operator gives none.
    std::chrono::milliseconds defaultInterval;

    /// Who pays for one sample, as the tail of the floor refusal.
    ///
    /// A column rather than a comment so the reason travels with the number: `fleet`'s
    /// floor is higher because every watcher's sample makes ONE process render a whole
    /// section for the whole fleet, and that cost lands where the operator is not
    /// looking.
    std::string_view costsWhom;

    /// What one of this subject's samples reads as: the reader its session's loop is given.
    ///
    /// **Null for `fleet` until its reader's shape is settled.** Its samples arrive as the leader's
    /// whole document, and a composition refuses a null reader by name rather than streaming a
    /// document nothing reads.
    SampleReader reader;

    /// The admin document one sample fetches, or empty for a subject sampled through the stats
    /// ladder.
    ///
    /// **Which door a sample goes through is this column**, so a fourth subject states its own
    /// rather than a composition branching on a key: empty asks `IStatsGatherer`, anything else
    /// asks `IAdminDocument` for exactly this path. `fleet` fetches the WHOLE document, every
    /// section behind its marker, because one sample is one moment and a strip read now beside
    /// a table read a second later would describe two fleets.
    std::string_view document;
};

/// The subjects, one row per enumerator, in enumerator order.
inline constexpr EnumTable<LiveSubject, LiveSubjectSpec> LiveSubjectTable { {
    { .subject = LiveSubject::Cache,
      .key = "cache",
      .inferrable = true,
      .servedBy = RemoteKind::FastcacheWireOnly,
      .needs = "a cache daemon",
      .minInterval = std::chrono::milliseconds { 1000 },
      .defaultInterval = std::chrono::milliseconds { 2000 },
      .costsWhom = "each sample is served by the cache daemon itself",
      .reader = &ReadStatsSample,
      .document = "" },
    { .subject = LiveSubject::Node,
      .key = "node",
      .inferrable = true,
      .servedBy = RemoteKind::CompileNode,
      .needs = "a fastcache-compile-node",
      .minInterval = std::chrono::milliseconds { 1000 },
      .defaultInterval = std::chrono::milliseconds { 2000 },
      .costsWhom = "each sample is served by the node itself",
      .reader = &ReadStatsSample,
      .document = "" },
    { .subject = LiveSubject::Fleet,
      .key = "fleet",
      .inferrable = false,
      .servedBy = RemoteKind::CompileNode,
      .needs = "a fastcache-compile-node (the fleet page is served by the one that leads)",
      .minInterval = std::chrono::milliseconds { 2000 },
      .defaultInterval = std::chrono::milliseconds { 5000 },
      .costsWhom = "each sample makes the LEADER render every fleet section for the whole fleet, "
                   "once per watcher",
      .reader = nullptr,
      .document = "/fleet.txt" },
} };

static_assert(RowsInEnumeratorOrder(LiveSubjectTable, &LiveSubjectSpec::subject),
              "LiveSubjectTable must hold one row per LiveSubject, in enumerator order");

/// Whether inference can pick at most one subject for any endpoint.
///
/// **What makes deriving inference from `servedBy` safe.** Two inferrable rows served by
/// one kind would make an unnamed `live-stats` against that endpoint ambiguous, and a
/// first-match lookup would resolve it by table order in silence. `fleet` and `node` share
/// `CompileNode`, which is legal only because `fleet` is not inferrable -- and this is
/// the check that notices the day somebody flips that.
/// @param table The subject table.
/// @return True when no kind is served by two inferrable rows.
[[nodiscard]] consteval bool EveryKindInfersAtMostOneSubject(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::none_of(table, [&table](LiveSubjectSpec const& row) {
        return row.inferrable && std::ranges::count_if(table, [&row](LiveSubjectSpec const& other) {
                                     return other.inferrable && other.servedBy == row.servedBy;
                                 }) > 1;
    });
}

static_assert(EveryKindInfersAtMostOneSubject(LiveSubjectTable),
              "two inferrable live-stats subjects are served by one RemoteKind, so inference would be ambiguous");

/// Whether every subject's default is at or above its own floor.
///
/// A default below the floor is a contradiction the operator would meet the first time
/// they typed back the interval they had been given.
/// @param table The subject table.
/// @return True when every default clears its floor.
[[nodiscard]] consteval bool EveryDefaultClearsItsFloor(EnumTable<LiveSubject, LiveSubjectSpec> const& table) noexcept
{
    return std::ranges::all_of(table, [](LiveSubjectSpec const& row) { return row.defaultInterval >= row.minInterval; });
}

static_assert(EveryDefaultClearsItsFloor(LiveSubjectTable),
              "a live-stats subject's default interval is below its own floor");

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
    std::chrono::milliseconds interval {};      ///< Time between samples.
    std::size_t samples { 0 };                  ///< Stop after this many; 0 means no bound.
    std::string endpoint {};                    ///< What the endpoint turned out to be, in words.
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
