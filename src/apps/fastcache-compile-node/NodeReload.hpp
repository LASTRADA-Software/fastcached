// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Config/ConfigReloader.hpp>

namespace FastCache::Node
{

/// The node's reloader: `NodeConfig`, read through the option table, guarded by its
/// `reloadable` column.
///
/// A name in a header rather than an alias inside `main.cpp`, because the live
/// snapshot is what several node-level policies read: `ReloadedCredential` presents
/// whatever `--requirepass` currently says, and anything else that has to answer
/// *now* rather than *at startup* reaches the same object. An alias private to the
/// one translation unit no test can reach would force each of them to spell
/// `ConfigReloaderOf<NodeConfig>` again, which is a second name for one concept.
using NodeReloader = ConfigReloaderOf<NodeConfig>;

} // namespace FastCache::Node
