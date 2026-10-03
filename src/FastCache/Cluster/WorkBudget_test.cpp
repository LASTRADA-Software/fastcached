// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Cluster/WorkBudget.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>

#include <core/platform/Clock.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using namespace std::chrono_literals;

// Every case below walks every kind of work, so a budget added to the table is tested here without
// being named; the rows' shape is what each case relies on.
static_assert(std::ranges::all_of(WorkBudgets, [](WorkBudgetRow const& row) { return row.burst % row.sourceBurst == 0; }),
              "the shared-budget case spends each burst exactly with whole sources");

namespace
{
/// The name of the @p index-th of many sources.
/// @param index Which.
/// @return Its host.
[[nodiscard]] std::string SourceHost(std::size_t index)
{
    return std::format("10.1.{}.{}", index / 200, (index % 200) + 1);
}
} // namespace

TEST_CASE("One source host's burst is its own, and the rest of the budget is everybody else's",
          "[cluster][discovery][budget]")
{
    for (auto const work: Enumerators<DiscoveryWork>())
    {
        auto const& row = BudgetOf(work);
        INFO(row.spends);
        auto const start = core::platform::SteadyTimePoint {};
        auto budget = WorkBudget { work, start };

        for ([[maybe_unused]] auto const unit: std::views::iota(std::size_t { 0 }, row.sourceBurst))
            REQUIRE(budget.TryTake("10.0.0.66", start) == WorkGrant::Granted);
        CHECK(budget.TryTake("10.0.0.66", start) == WorkGrant::SourceSpent);

        // Another host is served from a budget the first did not drain.
        CHECK(budget.TryTake("10.0.0.2", start) == WorkGrant::Granted);

        // And the first's own refill brings back exactly one.
        CHECK(budget.TryTake("10.0.0.66", start + row.sourceRefillEvery) == WorkGrant::Granted);
        CHECK(budget.TryTake("10.0.0.66", start + row.sourceRefillEvery) == WorkGrant::SourceSpent);
    }
}

TEST_CASE("The shared work budget bounds every source together", "[cluster][discovery][budget]")
{
    // Enough sources, each within its own burst, to spend the shared one: the bound on WORK that a
    // flood varying its source address meets.
    for (auto const work: Enumerators<DiscoveryWork>())
    {
        auto const& row = BudgetOf(work);
        INFO(row.spends);
        auto const start = core::platform::SteadyTimePoint {};
        auto budget = WorkBudget { work, start };
        for (auto const source: std::views::iota(std::size_t { 0 }, row.burst / row.sourceBurst))
            for ([[maybe_unused]] auto const unit: std::views::iota(std::size_t { 0 }, row.sourceBurst))
                REQUIRE(budget.TryTake(SourceHost(source), start) == WorkGrant::Granted);

        CHECK(budget.TryTake("10.9.9.9", start) == WorkGrant::BudgetSpent);

        // One refill interval on, exactly one more, to a source that still has its own token.
        CHECK(budget.TryTake("10.9.9.8", start + row.refillEvery) == WorkGrant::Granted);
        CHECK(budget.TryTake("10.9.9.7", start + row.refillEvery) == WorkGrant::BudgetSpent);
    }
}

TEST_CASE("The source table is bounded, and a source that keeps sending keeps its drained bucket",
          "[cluster][discovery][budget]")
{
    for (auto const work: Enumerators<DiscoveryWork>())
    {
        auto const& row = BudgetOf(work);
        INFO(row.spends);
        auto const start = core::platform::SteadyTimePoint {};
        auto budget = WorkBudget { work, start };
        for ([[maybe_unused]] auto const unit: std::views::iota(std::size_t { 0 }, row.sourceBurst))
            REQUIRE(budget.TryTake("10.0.0.66", start) == WorkGrant::Granted);

        // More quiet hosts than the table holds pass through, one each, while the flood keeps
        // sending between them: it is never the stalest, so it is never displaced, and stays
        // refused. Each step is shorter than any refill, so what refuses it is its own bucket.
        auto now = start;
        for (auto const source: std::views::iota(std::size_t { 0 }, MaxBudgetedSources + 10))
        {
            now += 1us;
            CHECK(budget.TryTake(SourceHost(source), now) == WorkGrant::Granted);
            CHECK(budget.TryTake("10.0.0.66", now) == WorkGrant::SourceSpent);
            CHECK(budget.Sources() <= MaxBudgetedSources);
        }
        CHECK(budget.Sources() == MaxBudgetedSources);

        // The control: a flood that STOPS sending is displaced like any quiet host, and comes back
        // to a full bucket -- a displacement only ever makes this node more willing to work. What
        // refuses it then, if anything, is the shared budget the quiet hosts spent.
        for (auto const source: std::views::iota(std::size_t { 1000 }, 1000 + MaxBudgetedSources))
        {
            now += 1us;
            REQUIRE(budget.TryTake(SourceHost(source), now) != WorkGrant::SourceSpent);
        }
        CHECK(budget.TryTake("10.0.0.66", now) != WorkGrant::SourceSpent);
    }
}
