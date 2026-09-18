// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file NodeConditionWire.hpp
/// A node's CONDITIONS as they travel (#1364): what a node detected that an operator must act on,
/// carried by `NodeStatus` to whoever asks and by `NodeAnnounce` to the leader's fleet page.
///
/// **Text is sent, never looked up.** Every field of a row is a string -- the id, the three
/// vocabulary words, the observed detail and the remedy -- so a client or a leader older than the
/// node renders a row it has never heard of exactly as the node wrote it. A row carried as an
/// enumerator would be unrenderable by the one reader that most needs it: whoever is running the
/// older build while the newer one is rolled out.
///
/// Header-only and dependency-free for the reason `CompileCacheWire.hpp` is, which includes it:
/// `fastcache-cc` compiles that header in without linking `FastCache`. Its own file so the leader's
/// registry can name a row without taking the whole wire vocabulary with it.

namespace FastCache::CompileCacheWire
{

/// Whether a condition can CLEAR while the process that raised it runs.
///
/// **The distinction an operator cannot draw without being told** (#1364): a counter-table skew is
/// fixed before the process served anything and is wrong for its whole life, while an enrollment
/// window closes on a verb. Rendered alike, *still broken* and *was broken and is fixed* read the
/// same.
///
/// Transmitted by NAME only (`ConditionPersistenceWords`), so the enumerator values bind nothing:
/// the explicit `= 0` is `EnumTable`'s anchor rather than a wire contract, and a word may be added
/// anywhere. The WORD is the contract, pinned by a test.
enum class ConditionPersistence : std::uint8_t
{
    Latched = 0, ///< Fixed for the life of the process: only a restart on a different build or configuration clears it.
    Live,        ///< Can clear while the process runs, and raise again.
    Last,        ///< Not a persistence.
};

/// How loudly a raised condition asks for attention. Transmitted by NAME only, as
/// `ConditionPersistence` is.
enum class ConditionSeverity : std::uint8_t
{
    Notice = 0, ///< Something an operator should know or compare; nothing is wrong.
    Warning,    ///< Something is degraded or misconfigured, and the node serves anyway.
    Alert,      ///< Something is exposed or failing that an operator should act on now.
    Last,       ///< Not a severity.
};

/// What one process found when it asked about one condition. Transmitted by NAME only, as
/// `ConditionPersistence` is.
///
/// **Four answers, because two of the obvious three are one spelling apart from a lie.** *Checked
/// and benign* must not read as *nobody decided*: the first is a finding and the second is a node
/// whose evaluator was never wired, which is the forgotten-refusal shape this tree already spells
/// three ways for its counters. `Undecided` is that second answer said out loud -- a startup that
/// leaves a row in it is a programmer error the node reports, and a wiring test exists to keep it
/// unreachable -- rather than folded into whichever neighbour reads least alarming.
///
/// A renderer adds a FIFTH, *absent*, for a node that did not answer or is too old to carry
/// conditions at all; it is never on the wire, because an absent list is a zero-length field.
enum class ConditionState : std::uint8_t
{
    Raised = 0,   ///< The condition holds now; the row's detail says what was observed.
    Clear,        ///< Checked, and found benign.
    NotEvaluated, ///< This node runs no component that could raise it; the detail says which.
    Undecided,    ///< Nothing has evaluated it: a node whose wiring missed a row.
    Last,         ///< Not a state.
};

/// One word of the persistence or severity vocabulary.
template <typename Enum>
struct ConditionWord
{
    Enum value;            ///< The enumerator this row spells.
    std::string_view name; ///< What travels, and what every renderer prints.
};

/// One word of the state vocabulary, and whether a row in that state asks for an operator's eye.
struct ConditionStateWord
{
    ConditionState state;  ///< The enumerator this row spells.
    std::string_view name; ///< What travels, and what every renderer prints.
    /// Whether a row in this state is one a renderer lists as needing attention.
    ///
    /// **A column, so no renderer restates which states are quiet.** `Clear` and `NotEvaluated` are
    /// the two that are not; `Undecided` is, because a row nobody evaluated is a node that cannot
    /// say whether the condition holds -- and folding it into the quiet ones would report *nothing
    /// raised* for the node least able to claim it.
    bool asksForAttention;
};

/// Every persistence, spelled.
inline constexpr EnumTable<ConditionPersistence, ConditionWord<ConditionPersistence>> ConditionPersistenceWords { {
    { .value = ConditionPersistence::Latched, .name = "latched" },
    { .value = ConditionPersistence::Live, .name = "live" },
} };
static_assert(RowsInEnumeratorOrder(ConditionPersistenceWords, &ConditionWord<ConditionPersistence>::value),
              "ConditionPersistenceWords must hold one row per ConditionPersistence, in enumerator order");

/// Every severity, spelled.
inline constexpr EnumTable<ConditionSeverity, ConditionWord<ConditionSeverity>> ConditionSeverityWords { {
    { .value = ConditionSeverity::Notice, .name = "notice" },
    { .value = ConditionSeverity::Warning, .name = "warning" },
    { .value = ConditionSeverity::Alert, .name = "alert" },
} };
static_assert(RowsInEnumeratorOrder(ConditionSeverityWords, &ConditionWord<ConditionSeverity>::value),
              "ConditionSeverityWords must hold one row per ConditionSeverity, in enumerator order");

/// Every state, spelled.
inline constexpr EnumTable<ConditionState, ConditionStateWord> ConditionStateWords { {
    { .state = ConditionState::Raised, .name = "raised", .asksForAttention = true },
    { .state = ConditionState::Clear, .name = "clear", .asksForAttention = false },
    { .state = ConditionState::NotEvaluated, .name = "not-evaluated", .asksForAttention = false },
    { .state = ConditionState::Undecided, .name = "undecided", .asksForAttention = true },
} };
static_assert(RowsInEnumeratorOrder(ConditionStateWords, &ConditionStateWord::state),
              "ConditionStateWords must hold one row per ConditionState, in enumerator order");

/// @param value A persistence.
/// @return Its word.
[[nodiscard]] constexpr std::string_view ConditionName(ConditionPersistence value) noexcept
{
    return ConditionPersistenceWords[static_cast<std::size_t>(value)].name;
}

/// @param value A severity.
/// @return Its word.
[[nodiscard]] constexpr std::string_view ConditionName(ConditionSeverity value) noexcept
{
    return ConditionSeverityWords[static_cast<std::size_t>(value)].name;
}

/// @param value A state.
/// @return Its word.
[[nodiscard]] constexpr std::string_view ConditionName(ConditionState value) noexcept
{
    return ConditionStateWords[static_cast<std::size_t>(value)].name;
}

/// The persistence @p name spells, or nothing for a word this build does not have.
/// @param name The word as it travelled.
/// @return The persistence.
[[nodiscard]] constexpr std::optional<ConditionPersistence> ConditionPersistenceNamed(std::string_view name) noexcept
{
    for (auto const& word: ConditionPersistenceWords)
        if (word.name == name)
            return word.value;
    return std::nullopt;
}

/// The severity @p name spells, or nothing for a word this build does not have.
/// @param name The word as it travelled.
/// @return The severity.
[[nodiscard]] constexpr std::optional<ConditionSeverity> ConditionSeverityNamed(std::string_view name) noexcept
{
    for (auto const& word: ConditionSeverityWords)
        if (word.name == name)
            return word.value;
    return std::nullopt;
}

/// The state @p name spells, or nothing for a word this build does not have.
/// @param name The word as it travelled.
/// @return The state.
[[nodiscard]] constexpr std::optional<ConditionState> ConditionStateNamed(std::string_view name) noexcept
{
    for (auto const& word: ConditionStateWords)
        if (word.name == name)
            return word.state;
    return std::nullopt;
}

/// One condition as a node reports it.
///
/// Six strings and nothing a receiver could recompute: a count of raised rows, a summary line or a
/// worst severity would each be a second answer beside the rows, and the one a reader trusted would
/// be the one nothing kept correct.
struct NodeConditionFields
{
    std::string id;          ///< Stable, kebab-case; what a person greps for and a script keys on.
    std::string persistence; ///< A `ConditionPersistenceWords` name.
    std::string severity;    ///< A `ConditionSeverityWords` name.
    std::string state;       ///< A `ConditionStateWords` name.
    /// What was observed when `Raised`, why nothing could be when `NotEvaluated`; empty otherwise.
    std::string detail;
    std::string remedy; ///< What to do about it, in the node's own words.

    [[nodiscard]] bool operator==(NodeConditionFields const&) const = default;
};

/// Whether a row in the state @p state is one a renderer lists as needing an operator's eye.
///
/// **A state this build cannot name asks for attention too.** A newer node's word for a state is
/// one this build has never seen, and treating it as quiet would render *nothing raised* over a row
/// that may well be raised -- the reassuring reading, which is the wrong one to default to.
/// @param state The state's word, as it travelled.
/// @return True when it is raised, undecided, or a word this build does not know.
[[nodiscard]] constexpr bool StateAsksForAttention(std::string_view state) noexcept
{
    auto const named = ConditionStateNamed(state);
    return !named.has_value() || ConditionStateWords[static_cast<std::size_t>(*named)].asksForAttention;
}

/// Whether @p row is one a renderer lists as needing an operator's eye; see `StateAsksForAttention`.
/// @param row The row.
/// @return True when its state asks for attention.
[[nodiscard]] constexpr bool AsksForAttention(NodeConditionFields const& row) noexcept
{
    return StateAsksForAttention(row.state);
}

/// One field of a condition row: where it lives, the most bytes it may carry, and what a refusal
/// calls it.
struct ConditionFieldRow
{
    std::string NodeConditionFields::* member; ///< The field.
    std::size_t maxBytes;                      ///< Its ceiling; a longer one is refused by the decoder.
    std::string_view name;                     ///< What a refusal or a column calls it.
};

/// Every field of a row, **in wire order**: the encoder and the decoder both walk this, so the
/// order cannot be written twice. Append only -- an insertion shifts every later field and every
/// peer reads one fact as the next.
///
/// The ceilings are what a node honours and a receiver enforces. The vocabulary words are short by
/// construction; the id is a kebab-case name; the detail is clamped by the node at the moment it
/// is raised, and every remedy in the node's table is `static_assert`ed below its ceiling.
inline constexpr std::array ConditionFieldTable {
    ConditionFieldRow { .member = &NodeConditionFields::id, .maxBytes = 64, .name = "id" },
    ConditionFieldRow { .member = &NodeConditionFields::persistence, .maxBytes = 32, .name = "persistence" },
    ConditionFieldRow { .member = &NodeConditionFields::severity, .maxBytes = 32, .name = "severity" },
    ConditionFieldRow { .member = &NodeConditionFields::state, .maxBytes = 32, .name = "state" },
    ConditionFieldRow { .member = &NodeConditionFields::detail, .maxBytes = 512, .name = "detail" },
    ConditionFieldRow { .member = &NodeConditionFields::remedy, .maxBytes = 512, .name = "remedy" },
};

/// The ceiling @p member carries, found by the member rather than by its position in the table.
/// @param member A field of a row.
/// @return Its ceiling; zero for a member the table does not list, which no caller names.
[[nodiscard]] consteval std::size_t ConditionFieldCeiling(std::string NodeConditionFields::* member) noexcept
{
    for (auto const& field: ConditionFieldTable)
        if (field.member == member)
            return field.maxBytes;
    return 0;
}

/// The ceiling on the detail a node attaches to a row; read by the node where it clamps one.
inline constexpr std::size_t MaxConditionDetailBytes = ConditionFieldCeiling(&NodeConditionFields::detail);

/// The ceiling on a remedy; read by the node's table, which asserts every remedy fits.
inline constexpr std::size_t MaxConditionRemedyBytes = ConditionFieldCeiling(&NodeConditionFields::remedy);

/// The ceiling on an id; read by the node's table, which asserts every id fits.
inline constexpr std::size_t MaxConditionIdBytes = ConditionFieldCeiling(&NodeConditionFields::id);

/// The most rows one list may carry.
///
/// A node's table is a handful of rows and every one travels, so this is headroom for tables to
/// come rather than a limit anything meets today -- and the node asserts its table fits, so a list
/// is never cut short to honour it.
inline constexpr std::size_t MaxNodeConditions = 12;

/// What one encoded row costs at most, framing included: every field at its ceiling behind its
/// length prefix, plus the prefix on the row itself.
[[nodiscard]] consteval std::size_t MaxNodeConditionRowBytes() noexcept
{
    auto bytes = WireFields::FieldPrefixSize;
    for (auto const& field: ConditionFieldTable)
        bytes += WireFields::FieldPrefixSize + field.maxBytes;
    return bytes;
}

/// What a whole list costs at most. `CompileCacheWire.hpp` asserts it fits beside the history a
/// `NodeAnnounce` also carries.
inline constexpr std::size_t MaxNodeConditionListBytes = MaxNodeConditions * MaxNodeConditionRowBytes();

/// Frame a list of conditions as one nested field.
///
/// **An EMPTY list encodes as nothing, which is ABSENT, and no node ever sends one.** Every row of a
/// node's table travels whatever its state, and the table is not empty -- so a zero-length field
/// can only mean a sender that says nothing about conditions, which is exactly how an older build
/// reads. The same rule `NodeRuntimeFields::consensusEndpoint` states for an engaged empty string.
///
/// @param rows What to encode; at most `MaxNodeConditions` are taken, which a node's asserted table
///             never exceeds.
/// @return The nested record's bytes, to be carried as a single field.
[[nodiscard]] inline std::vector<std::byte> EncodeNodeConditions(std::span<NodeConditionFields const> rows)
{
    auto const count = std::min(rows.size(), MaxNodeConditions);
    std::vector<std::vector<std::byte>> encoded;
    encoded.reserve(count);
    for (auto const& row: rows.subspan(0, count))
    {
        std::vector<std::span<std::byte const>> parts;
        parts.reserve(ConditionFieldTable.size());
        for (auto const& field: ConditionFieldTable)
            parts.emplace_back(WireFields::AsBytes(row.*field.member));
        encoded.push_back(WireFields::Encode(WireFields::FieldList { parts }));
    }

    std::vector<std::span<std::byte const>> views;
    views.reserve(encoded.size());
    for (auto const& row: encoded)
        views.emplace_back(row);
    return WireFields::Encode(WireFields::FieldList { views });
}

/// Read a list of conditions back, leaving @p out ABSENT for an empty field.
///
/// Holds the whole tolerance rule once, the way `Detail::ReadOptionalBigEndian` holds it for an
/// integer: an EMPTY field is *did not say* and leaves @p out alone; a row carrying MORE fields than
/// this build knows has the surplus skipped, so a newer node's appended fact costs nothing; and what
/// is refused is a shape this build cannot read -- a row short of the six fields, a field above its
/// ceiling, or more rows than a list may carry. Refused rather than clamped, because keeping the
/// first rows of an oversized list would leave the rest looking reported.
///
/// A vocabulary word this build has no name for is KEPT as sent: the row renders as the node wrote
/// it, and `AsksForAttention` treats an unknown state as worth a look.
/// @param field The nested record's bytes, possibly empty.
/// @param out Set only when the field carried a list.
/// @return False when the field was present and malformed.
[[nodiscard]] inline bool ReadNodeConditions(std::span<std::byte const> field,
                                             std::optional<std::vector<NodeConditionFields>>& out)
{
    if (field.empty())
        return true;

    auto const rows = WireFields::SplitAll(field);
    if (!rows.has_value() || rows->size() > MaxNodeConditions)
        return false;

    std::vector<NodeConditionFields> decoded;
    decoded.reserve(rows->size());
    for (auto const& row: *rows)
    {
        auto const parts = WireFields::SplitAll(row);
        if (!parts.has_value() || parts->size() < ConditionFieldTable.size())
            return false;

        NodeConditionFields fields;
        for (auto const index: std::views::iota(std::size_t { 0 }, ConditionFieldTable.size()))
        {
            auto const& column = ConditionFieldTable[index];
            auto const bytes = (*parts)[index];
            if (bytes.size() > column.maxBytes)
                return false;
            fields.*column.member = std::string { WireFields::AsStringView(bytes) };
        }
        decoded.push_back(std::move(fields));
    }
    out = std::move(decoded);
    return true;
}

} // namespace FastCache::CompileCacheWire
