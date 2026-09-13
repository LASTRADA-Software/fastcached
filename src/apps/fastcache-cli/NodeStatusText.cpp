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

} // namespace FastCache::Cli
