// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"

#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <cassert>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What a clamped detail ends with, so a reader can tell it was cut.
    constexpr std::string_view Clamped = "...";

    /// @p raw as TEXT: every byte that belongs to no UTF-8 sequence written `\xNN`.
    /// @param raw What a component observed; a path may be in any encoding its host chose.
    /// @return Well-formed UTF-8.
    [[nodiscard]] std::string AsText(std::string_view raw)
    {
        std::string text;
        text.reserve(raw.size());
        while (!raw.empty())
        {
            auto const length = Utf8SequenceLength(raw);
            if (length == 0)
            {
                text += std::format("\\x{:02X}", static_cast<unsigned>(static_cast<unsigned char>(raw.front())));
                raw.remove_prefix(1);
                continue;
            }
            text.append(raw.substr(0, length));
            raw.remove_prefix(length);
        }
        return text;
    }

    /// @p text cut to @p ceiling bytes on a code-point boundary, marked when it was cut.
    /// @param text Well-formed UTF-8.
    /// @param ceiling The most bytes it may take.
    /// @return The text, or a prefix of it ending in `Clamped`.
    [[nodiscard]] std::string ClampedTo(std::string text, std::size_t ceiling)
    {
        if (text.size() <= ceiling)
            return text;
        auto cut = ceiling - Clamped.size();
        // Back off a continuation byte at a time, so the cut never splits a sequence -- a split
        // one would be the very non-text this clamp runs after `AsText` to avoid.
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & Utf8ContinuationMask) == Utf8ContinuationMark)
            --cut;
        text.resize(cut);
        text += Clamped;
        return text;
    }

    /// A detail as a row carries it: text, and inside the ceiling.
    /// @param raw What was observed.
    /// @return The detail.
    [[nodiscard]] std::string DetailOf(std::string_view raw)
    {
        return ClampedTo(AsText(raw), Wire::MaxConditionDetailBytes);
    }
} // namespace

void NodeConditions::Raise(NodeCondition condition, std::string_view detail)
{
    std::scoped_lock const guard { _mutex };
    SetLocked(condition, Wire::ConditionState::Raised, DetailOf(detail));
}

void NodeConditions::Clear(NodeCondition condition)
{
    std::scoped_lock const guard { _mutex };
    SetLocked(condition, Wire::ConditionState::Clear, {});
}

void NodeConditions::NotEvaluated(NodeCondition condition, std::string_view reason)
{
    std::scoped_lock const guard { _mutex };
    SetLocked(condition, Wire::ConditionState::NotEvaluated, DetailOf(reason));
}

void NodeConditions::SetLocked(NodeCondition condition, Wire::ConditionState state, std::string detail)
{
    auto& reading = _readings[static_cast<std::size_t>(condition)];
    auto const latched = RowFor(condition).persistence == Wire::ConditionPersistence::Latched;
    if (latched && reading.state == Wire::ConditionState::Raised && state != Wire::ConditionState::Raised)
    {
        // The latched claim is that nothing this process does can clear the row; a clear arriving
        // anyway is a component that misread its own condition, and honouring it would report a
        // broken process as fixed.
        assert(false && "a latched condition was cleared after it was raised");
        return;
    }
    reading.state = state;
    reading.detail = std::move(detail);
}

std::vector<NodeCondition> NodeConditions::Settle(PresentComponents const& present)
{
    std::scoped_lock const guard { _mutex };
    std::vector<NodeCondition> undecided;
    for (auto const& row: NodeConditionTable)
    {
        auto const& scope = ConditionScopeTable[static_cast<std::size_t>(row.scope)];
        auto& reading = _readings[static_cast<std::size_t>(row.condition)];
        auto const runs = scope.present == nullptr || present.*scope.present;
        if (!runs)
        {
            // A component this node never built cannot have said anything, so a row of its scope
            // that is not `Undecided` here was set by something else -- a wiring defect of the
            // opposite kind, and asserted rather than overwritten in silence.
            assert(reading.state == Wire::ConditionState::Undecided
                   && "a condition was evaluated by a component this node does not run");
            reading.state = Wire::ConditionState::NotEvaluated;
            reading.detail = std::string { scope.notEvaluated };
            continue;
        }
        if (reading.state == Wire::ConditionState::Undecided)
            undecided.push_back(row.condition);
    }
    return undecided;
}

Wire::ConditionState NodeConditions::StateOf(NodeCondition condition) const
{
    std::scoped_lock const guard { _mutex };
    return _readings[static_cast<std::size_t>(condition)].state;
}

std::vector<Wire::NodeConditionFields> NodeConditions::Snapshot() const
{
    std::vector<Wire::NodeConditionFields> rows;
    rows.reserve(NodeConditionTable.size());
    std::scoped_lock const guard { _mutex };
    for (auto const& row: NodeConditionTable)
    {
        auto const& reading = _readings[static_cast<std::size_t>(row.condition)];
        rows.push_back(Wire::NodeConditionFields { .id = std::string { row.id },
                                                   .persistence = std::string { Wire::ConditionName(row.persistence) },
                                                   .severity = std::string { Wire::ConditionName(row.severity) },
                                                   .state = std::string { Wire::ConditionName(reading.state) },
                                                   .detail = reading.detail,
                                                   .remedy = std::string { row.remedy } });
    }
    return rows;
}

void EvaluateProcessConditions(NodeConditions& conditions, IMetricsSink const& metrics)
{
    auto const missing = CatalogueRowsWithoutASlot(metrics);
    if (missing.empty())
    {
        conditions.Clear(NodeCondition::CounterTableSkew);
        return;
    }
    auto names = std::vector<std::string> {};
    names.reserve(missing.size());
    for (auto const name: missing)
        names.emplace_back(name);
    conditions.Raise(
        NodeCondition::CounterTableSkew,
        ListDetail(std::format("{} catalogue row(s) this build's sink has no slot for:", missing.size()), names));
}

std::string ListDetail(std::string_view lead, std::vector<std::string> const& items)
{
    auto detail = std::string { lead };
    for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
    {
        auto const separator = index == 0 ? std::string_view { " " } : std::string_view { ", " };
        auto const rest = items.size() - index - 1;
        // Room for this item AND for the count of whatever follows it, so the list is never cut
        // with no room left to say how much was left out.
        auto const tail = rest == 0 ? std::string {} : std::format(" and {} more", rest);
        auto const candidate = std::format("{}{}", separator, AsText(items[index]));
        if (detail.size() + candidate.size() + tail.size() > Wire::MaxConditionDetailBytes)
            return detail + std::format(" and {} more", items.size() - index);
        detail += candidate;
    }
    return detail;
}

} // namespace FastCache::Node
