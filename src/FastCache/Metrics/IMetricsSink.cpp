// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Metrics/IMetricsSink.hpp>

namespace FastCache::CounterSkew
{

// The one definition, for the count this file saw. A unit compiled against any other count names a symbol nothing
// defines, and the link fails there (#1361). Dependency-free, so fastcache-cc, which does not link FastCache,
// compiles it in as well.
bool RequireExtent(Extent<CounterCount> /*extent*/) noexcept
{
    return true;
}

} // namespace FastCache::CounterSkew
