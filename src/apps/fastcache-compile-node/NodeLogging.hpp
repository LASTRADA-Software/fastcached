// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Logger.hpp>

#include <iosfwd>
#include <memory>

namespace FastCache::Node
{

/// Build the console logger this node's configuration asks for.
///
/// **A factory rather than two arguments at the call site, and that is the whole
/// point of the file** ([#485](https://github.com/LASTRADA-Software/fastcached/issues/485)).
/// The defect it closes was not a wrong logger: it was `main.cpp` constructing a
/// `ConsoleLogger` with two of its three arguments, so `logTimestamps` existed
/// nowhere, could not be asked for, and no amount of correct `ConsoleLogger`
/// behaviour would have shown it.
///
/// **A test of a settings struct would not have caught it either**, which is why this
/// returns the logger rather than the numbers to build one with: `main` could read
/// such a struct and still drop a field, and the test would pass. There is one
/// construction path, production takes it, and the test takes the same one and reads
/// the bytes that come out the other end.
///
/// Nothing here is a policy. What the defaults are and why they are what they are
/// lives on `NodeConfig::logLevel` and `NodeConfig::logTimestamps`; this only carries
/// them to the one object that spends them.
///
/// `unique_ptr` because `ConsoleLogger` holds a `std::mutex` and so is neither
/// movable nor copyable -- a by-value factory will not compile, and an out-parameter
/// would put the construction back at the call site this exists to empty.
///
/// @param sink Where lines are written; must outlive the returned logger.
/// @param cfg The resolved configuration, after the file and the command line.
/// @return The logger, never null.
[[nodiscard]] std::unique_ptr<ConsoleLogger> MakeNodeConsoleLogger(std::ostream& sink, NodeConfig const& cfg);

} // namespace FastCache::Node
