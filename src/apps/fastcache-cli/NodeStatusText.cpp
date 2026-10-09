// SPDX-License-Identifier: Apache-2.0
#include "NodeStatusText.hpp"

#include <array>
#include <format>

namespace FastCache::Cli
{

namespace
{
    /// Which `NodeComponentBit` each reported component name stands for.
    ///
    /// A table because the set grows: a bit this build does not name is reported under
    /// its NUMBER rather than dropped, so an older client meeting a newer node says
    /// *there is something here I do not know about* instead of quietly under-reporting
    /// what the node runs.
    struct ComponentBit
    {
        std::uint32_t bit;     ///< The mask bit.
        std::string_view name; ///< What to call it.
    };

    constexpr std::array<ComponentBit, 4> ComponentBits { {
        { .bit = CompileCacheWire::NodeComponentBit::CacheTier, .name = "cache-tier" },
        { .bit = CompileCacheWire::NodeComponentBit::Worker, .name = "worker" },
        { .bit = CompileCacheWire::NodeComponentBit::Scheduler, .name = "scheduler" },
        { .bit = CompileCacheWire::NodeComponentBit::Consensus, .name = "consensus" },
    } };
} // namespace

std::string DescribeComponents(std::uint32_t mask)
{
    std::string out;
    std::uint32_t named = 0;
    for (auto const& row: ComponentBits)
        if ((mask & row.bit) != 0)
        {
            named |= row.bit;
            if (!out.empty())
                out += ", ";
            out += row.name;
        }

    // Whatever is left is a component this build has no name for. Reported as the
    // residual mask, because *some bits I do not understand* is a fact an operator
    // can act on -- upgrade the client -- and silence is not.
    if (auto const unknown = mask & ~named; unknown != 0)
    {
        if (!out.empty())
            out += ", ";
        out += std::format("unknown(0x{:x})", unknown);
    }

    return out.empty() ? std::string { "none" } : out;
}

std::string_view NameOfToolchainState(CompileCacheWire::ToolchainState state) noexcept
{
    switch (state)
    {
        case CompileCacheWire::ToolchainState::Surveying:
            return "surveying";
        case CompileCacheWire::ToolchainState::Serving:
            return "serving";
        case CompileCacheWire::ToolchainState::NothingToServe:
            return "nothing-to-serve";
    }
    // Unreachable for the reason CliVerbs.cpp's `NameOfSurface` tail is: `DecodeNodeRuntime`
    // leaves a state this build has no name for DISENGAGED rather than passing it
    // through, so nothing but a named one arrives. Closed anyway -- falling off the
    // end of a function returning a view is a dangling one.
    return "unknown";
}

std::string_view NameOfSchedulerRole(CompileCacheWire::WireSchedulerRole role) noexcept
{
    switch (role)
    {
        case CompileCacheWire::WireSchedulerRole::Follower:
            return "follower";
        case CompileCacheWire::WireSchedulerRole::Undecided:
            return "undecided";
        case CompileCacheWire::WireSchedulerRole::Leader:
            return "leader";
    }
    // Unreachable for `NameOfToolchainState`'s reason: `DecodeNodeRuntime` leaves a role
    // this build has no name for disengaged rather than passing it through.
    return "unknown";
}

std::string_view NameOfSharedCacheSource(CompileCacheWire::WireSharedCacheSource source) noexcept
{
    // A `switch` for `NameOfToolchainState`'s reason and one more: the enum is transmitted with
    // explicit values and no `Last`, so no `EnumTable` can hold it. A source added and not spelled
    // here fails the GCC and Clang legs (`-Wswitch`, fatal under `PEDANTIC_COMPILER_WERROR`) and the
    // clang-tidy sweep; the MSVC and clang-cl legs do not fail, having no `/WX` or `-Werror` and cl's
    // C4062 being off even at `/W4` (`SharedCacheStatusOf` says the same about its own switch).
    switch (source)
    {
        case CompileCacheWire::WireSharedCacheSource::None:
            return "none";
        case CompileCacheWire::WireSharedCacheSource::Setting:
            return "setting";
        case CompileCacheWire::WireSharedCacheSource::Override:
            return "override";
        case CompileCacheWire::WireSharedCacheSource::ThisMachine:
            return "this-machine";
    }
    // Unreachable: `ReadSharedCacheStatus` leaves a record carrying a source this build has no
    // name for ABSENT rather than passing the byte through.
    return "unknown";
}

std::string_view NameOfSharedCacheState(CompileCacheWire::WireSharedCacheState state) noexcept
{
    switch (state)
    {
        case CompileCacheWire::WireSharedCacheState::NotTried:
            return "not-tried";
        case CompileCacheWire::WireSharedCacheState::Proven:
            return "proven";
        case CompileCacheWire::WireSharedCacheState::Unresolved:
            return "unresolved";
        case CompileCacheWire::WireSharedCacheState::WrongKey:
            return "wrong-key";
        case CompileCacheWire::WireSharedCacheState::Unreachable:
            return "unreachable";
        case CompileCacheWire::WireSharedCacheState::Serving:
            return "serving";
        case CompileCacheWire::WireSharedCacheState::Unavailable:
            return "unavailable";
        case CompileCacheWire::WireSharedCacheState::ProofRefused:
            return "proof-refused";
    }
    // Unreachable for `NameOfSharedCacheSource`'s reason.
    return "unknown";
}

std::optional<std::vector<ConditionMention>> ConditionsAskingForAttention(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions)
{
    if (!conditions.has_value())
        return std::nullopt;

    std::vector<ConditionMention> mentions;
    for (auto const& row: *conditions)
    {
        if (!CompileCacheWire::AsksForAttention(row))
            continue;
        // The state rides along only where it is not the plain `raised`, so the ordinary line stays
        // short and the unusual one cannot be mistaken for it.
        auto const plain = CompileCacheWire::ConditionStateNamed(row.state) == CompileCacheWire::ConditionState::Raised;
        mentions.push_back(
            ConditionMention { .id = row.id,
                               .persistence = row.persistence,
                               .unusualState = plain ? std::nullopt : std::optional<std::string> { row.state },
                               .severity = CompileCacheWire::ConditionSeverityNamed(row.severity) });
    }
    return mentions;
}

std::optional<std::string> DescribeConditions(
    std::optional<std::vector<CompileCacheWire::NodeConditionFields>> const& conditions)
{
    auto const mentions = ConditionsAskingForAttention(conditions);
    if (!mentions.has_value())
        return std::nullopt;
    if (mentions->empty())
        return std::string { NoConditionsRaised };

    std::string raised;
    for (auto const& mention: *mentions)
        raised += std::format("{}{} ({}{}{})",
                              raised.empty() ? "" : ", ",
                              mention.id,
                              mention.persistence,
                              mention.unusualState.has_value() ? ", " : "",
                              mention.unusualState.value_or(std::string {}));
    return std::format("raised: {}", raised);
}

bool ReportsConsensus(CompileCacheWire::NodeStatusFields const* status) noexcept
{
    return status != nullptr
           && ((status->components & CompileCacheWire::NodeComponentBit::Consensus) != 0
               || status->runtime.schedulerRole.has_value());
}

std::optional<std::string> NamedLeader(CompileCacheWire::NodeStatusFields const& status)
{
    auto const& runtime = status.runtime;
    if (!runtime.leaderEndpoint.empty())
        return runtime.leaderEndpoint;
    if (runtime.schedulerRole == CompileCacheWire::WireSchedulerRole::Leader)
        return std::string { SelfLeader };
    return std::nullopt;
}

} // namespace FastCache::Cli
