// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardPanel.hpp"

namespace FastCache::Cli
{

/// @file DashboardPanels.hpp
/// The `cache`, `node` and `fleet` panels, as the tables `PanelView` draws.
///
/// **Names come from the catalogue where the catalogue has them.** A counter's name is
/// `DescriptorOf(...)->prometheusName`, which is also what a node's `NodeMetrics` reply calls it,
/// so no counter is spelled here. The storage, tier and host series are named in
/// `PrometheusFormatter.cpp`, file-local, so they are spelled here -- and a test renders a real
/// snapshot through that formatter and requires every such name to be one it emitted, which is
/// what stops a rename there from leaving a panel drawing absent markers in silence.

/// The `cache` panel (#134 §3): a cache daemon's rates, levels and tiers.
/// @return The spec; static storage.
[[nodiscard]] PanelSpec const& CachePanel() noexcept;

/// The `node` panel's counters and host figures (#134 §4).
///
/// The status block -- toolchains, registrars, consensus, slots and what limits them -- is not
/// here, because no stats reading carries it: those come from `NodeStatus`, and a panel can draw
/// only what the model holds.
/// @return The spec; static storage.
[[nodiscard]] PanelSpec const& NodePanel() noexcept;

/// The `fleet` panel (#134 §5): the leader's `/fleet.txt`, as the headline tiles, a strip naming its
/// sections, and the active section's table walked from its own header line.
///
/// No figure rows: everything it draws is the newest reading's document, which the fleet reader hands
/// to the model parsed. `DocumentSpec`'s defaults are this panel's drop order -- the strip first, then
/// the tiles, then the source line, and the table last of all, shrinking to `+N more` and never going.
/// @return The spec; static storage.
[[nodiscard]] PanelSpec const& FleetPanel() noexcept;

} // namespace FastCache::Cli
