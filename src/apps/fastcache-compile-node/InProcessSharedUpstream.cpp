// SPDX-License-Identifier: Apache-2.0
#include "InProcessSharedUpstream.hpp"
#include "SharedCacheTier.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

InProcessSharedUpstream::InProcessSharedUpstream(SharedCacheHost const& host) noexcept:
    _host { host }
{
}

core::async::Task<std::optional<std::vector<std::byte>>> InProcessSharedUpstream::Fetch(std::string_view key)
{
    // Held for the whole call: a move of the setting mid-read retires the tier, never frees it.
    auto const tier = _host.Host().Current();
    if (tier == nullptr)
        co_return std::nullopt;
    co_return co_await tier->Fetch(key);
}

core::async::Task<UpstreamStore> InProcessSharedUpstream::Store(std::string_view key, std::span<std::byte const> value)
{
    auto const tier = _host.Host().Current();
    if (tier == nullptr)
        co_return UpstreamStore::NotConfigured;
    co_return co_await tier->Store(key, value) ? UpstreamStore::Stored : UpstreamStore::Declined;
}

bool InProcessSharedUpstream::Configured() const noexcept
{
    return _host.Host().Current() != nullptr;
}

} // namespace FastCache::Node
