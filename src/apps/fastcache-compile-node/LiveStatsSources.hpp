// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdminEndpoint.hpp"
#include "LiveStatsResponder.hpp"
#include "NodeStatusResponder.hpp"

#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <optional>
#include <string>

namespace FastCache::Node
{

/// What a node's live stats are read from.
///
/// The same objects the admin surface reads, which is the point: a live panel and `/metrics`,
/// `/fleet.txt` or `NodeStatus` disagreeing about one machine would be two sources of truth for
/// one fact. Pointers because two of them are legitimately absent -- a node with no scheduler has
/// no fleet, and a build that keeps no history has none to draw.
struct NodeLiveStatsParts
{
    /// The counters `/metrics` renders. Never null.
    IMetricsSink const* metrics { nullptr };

    /// The snapshot `/metrics` renders beside the counters. Never empty.
    AdminHttpServer::SnapshotProvider snapshot {};

    /// What `NodeStatus` answers. Never null.
    INodeStatusSource const* identity { nullptr };

    /// The fleet `/fleet.txt` renders, or nullopt when this node runs no scheduler.
    std::optional<Distributed::FleetSources> fleet {};

    /// The history `/fleet.txt`'s figures draw on, or null to draw none.
    IFleetHistoryView const* history { nullptr };

    /// Where this node's `0xFC` surface answers.
    std::string endpoint {};
};

/// The production `ILiveStatsSources`: each subject captured with the functions its one-shot
/// surface already calls.
///
/// - **Cache:** one `EncodeStatsReading` of `CaptureStatsReading(metrics, snapshot())` -- the
///   reading `RenderPrometheus` renders `/metrics` from, so the binary and the text are one read.
/// - **Node:** that reading, then `EncodeNodeStatus(identity->Describe())`, as two fields.
/// - **Fleet:** `RenderFleetText(CollectFleet(...), <the Day view>, all sections)`, byte for byte
///   what `/fleet.txt` serves with no parameters.
class NodeLiveStatsSources final: public ILiveStatsSources
{
  public:
    /// @param parts What to read; every pointer must outlive this.
    explicit NodeLiveStatsSources(NodeLiveStatsParts parts) noexcept;

    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(CompileCacheWire::LiveSubject subject) const override;

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override;

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return _parts.endpoint;
    }

  private:
    NodeLiveStatsParts _parts;
};

/// What a node status says for the events a node subject reports.
///
/// Pure, so the event rows are testable over a literal status.
/// @param status What `NodeStatus` answers.
/// @return The probe: leadership, the survey and the enrollment window, each absent when the
///         status carries none.
[[nodiscard]] LiveEventProbe ProbeNodeStatus(CompileCacheWire::NodeStatusFields const& status);

/// What a fleet snapshot says for the events a fleet subject reports.
/// @param snapshot What `CollectFleet` gathered.
/// @return The probe: the cluster's members, the registry and leadership.
[[nodiscard]] LiveEventProbe ProbeFleet(Distributed::FleetSnapshot const& snapshot);

} // namespace FastCache::Node
