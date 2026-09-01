// SPDX-License-Identifier: Apache-2.0
#include "NodeLogging.hpp"

#include <memory>
#include <ostream>

namespace FastCache::Node
{

std::unique_ptr<ConsoleLogger> MakeNodeConsoleLogger(std::ostream& sink, NodeConfig const& cfg)
{
    // Every setting the logger takes, named here and nowhere else. A second call site
    // constructing its own is the defect this file exists to have closed, so there is
    // deliberately nothing to copy.
    return std::make_unique<ConsoleLogger>(
        sink, cfg.logLevel, cfg.logTimestamps ? LogTimestamps::Yes : LogTimestamps::No);
}

} // namespace FastCache::Node
