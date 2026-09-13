// SPDX-License-Identifier: Apache-2.0
#include "TerminalEvents.hpp"

namespace FastCache::Cli
{

/// @file TerminalEventsUnavailable.cpp
/// The terminal contract for a build without the vendored TUI (`FASTCACHED_BUILD_TUI=OFF`).
///
/// It refuses rather than yielding an empty stream, for the header's reason: no events would read
/// as a quiet operator, and the fact here is that this binary cannot open a terminal.

struct UnstartedTerminal::Parts
{
};

UnstartedTerminal::UnstartedTerminal(std::unique_ptr<Parts> parts) noexcept:
    _parts { std::move(parts) }
{
}

UnstartedTerminal::~UnstartedTerminal() = default;

namespace
{
    constexpr auto Unavailable = "this fastcache-cli was built without the terminal UI (FASTCACHED_BUILD_TUI=OFF)";
} // namespace

std::expected<std::unique_ptr<UnstartedTerminal>, std::string> MakeTerminalEvents(IExecutor* /*pool*/,
                                                                                  IExecutor* /*resumeOn*/,
                                                                                  UsageColor /*colour*/)
{
    return std::unexpected(std::string { Unavailable });
}

Task<std::expected<StartedTerminal, std::string>> StartTerminal(std::unique_ptr<UnstartedTerminal> /*terminal*/)
{
    co_return std::unexpected(std::string { Unavailable });
}

} // namespace FastCache::Cli
