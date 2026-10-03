// SPDX-License-Identifier: Apache-2.0
#include "UnservedToolchains.hpp"

#include <algorithm>
#include <tuple>

namespace FastCache::Distributed
{

void UnservedToolchains::Refused(std::string_view fingerprint, std::string_view label)
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.now();

    // Expired entries go first, so one that lapsed is re-recorded fresh -- its count restarts --
    // rather than resurrected with a tally from before the window.
    std::erase_if(_entries, [now](Entry const& entry) { return now - entry.lastAsked > Window; });

    auto const found = std::ranges::find(
        _entries, fingerprint, [](Entry const& entry) { return std::string_view { entry.toolchain.fingerprint }; });
    if (found != _entries.end())
    {
        ++found->toolchain.refusals;
        found->lastAsked = now;
        if (!label.empty())
            found->toolchain.label = std::string { label };
        return;
    }

    if (_entries.size() == Capacity)
        _entries.erase(std::ranges::min_element(_entries, {}, &Entry::lastAsked));
    _entries.push_back(Entry {
        .toolchain =
            UnservedToolchain { .fingerprint = std::string { fingerprint }, .label = std::string { label }, .refusals = 1 },
        .lastAsked = now });
}

std::vector<UnservedToolchain> UnservedToolchains::Recent() const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.now();
    auto recent = std::vector<UnservedToolchain> {};
    for (auto const& entry: _entries)
        if (now - entry.lastAsked <= Window)
            recent.push_back(entry.toolchain);
    std::ranges::sort(
        recent, {}, [](UnservedToolchain const& toolchain) { return std::tie(toolchain.label, toolchain.fingerprint); });
    return recent;
}

} // namespace FastCache::Distributed
