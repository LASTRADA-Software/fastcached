// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheHost.hpp"
#include "SharedCacheStatus.hpp"

#include <FastCache/Cluster/SharedCacheTarget.hpp>

#include <format>
#include <string>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// `--upstream` wins; say what it won over.
    /// @param target The target; `resolved` is what the setting names, whatever overrides it.
    /// @return The record.
    [[nodiscard]] Wire::SharedCacheStatusFields OverrideStatus(SharedCacheTarget const& target)
    {
        auto const& named = target.resolved.machineId;
        return Wire::SharedCacheStatusFields {
            .source = Wire::WireSharedCacheSource::Override,
            .machineId = {},
            .endpoint = target.overrideEndpoint,
            .state = Wire::WireSharedCacheState::NotTried,
            .detail = named.empty()
                          ? std::string { "--upstream on this node is read through to; the fleet's shared-cache "
                                          "setting names no machine" }
                          : std::format("--upstream on this node overrides the fleet's shared-cache setting, which "
                                        "names {}",
                                        named),
        };
    }

    /// The setting names another machine and this node builds no fleet half to reach it.
    /// @param target The target.
    /// @return The record.
    [[nodiscard]] Wire::SharedCacheStatusFields UnreachedStatus(SharedCacheTarget const& target)
    {
        auto const& row = Cluster::RowOf(target.resolved.resolution);
        return Wire::SharedCacheStatusFields {
            .source = Wire::WireSharedCacheSource::Setting,
            .machineId = target.resolved.machineId,
            .endpoint = target.resolved.endpoint,
            // Unresolved is a fact about the state, not about an attempt: said whether or not
            // anything here would dial.
            .state = row.dials ? Wire::WireSharedCacheState::NotTried : Wire::WireSharedCacheState::Unresolved,
            .detail = row.dials ? std::string { "this node reads nothing through to it: it keeps no private tier, or "
                                                "holds no identity to prove itself with" }
                                : std::string { row.why },
        };
    }

    /// The setting names this machine.
    /// @param target The target.
    /// @param host The tier's host, or null.
    /// @param selfEndpoint Where this node answers.
    /// @return The record.
    [[nodiscard]] Wire::SharedCacheStatusFields ThisMachineStatus(SharedCacheTarget const& target,
                                                                  SharedCacheHost const* host,
                                                                  std::string_view selfEndpoint)
    {
        auto fields = Wire::SharedCacheStatusFields { .source = Wire::WireSharedCacheSource::ThisMachine,
                                                      .machineId = target.resolved.machineId,
                                                      .endpoint = std::string { selfEndpoint },
                                                      .state = Wire::WireSharedCacheState::Unavailable,
                                                      .detail = {} };
        if (host == nullptr)
        {
            fields.detail = "this process runs no shared-cache host";
            return fields;
        }
        auto const status = host->Status();
        if (status.serving)
            fields.state = Wire::WireSharedCacheState::Serving;
        else if (!status.named)
            // The directory is told of an apply before the host has reconciled it, and opening a
            // store is the host's own thread's work: a window, not a failure.
            fields.detail = "not open yet: the state naming this machine has not been reconciled";
        else
            fields.detail = status.unavailable;
        return fields;
    }
} // namespace

Wire::SharedCacheStatusFields SharedCacheStatusOf(SharedCacheTarget const& target,
                                                  ISharedCacheStatusSource const* fleet,
                                                  SharedCacheHost const* host,
                                                  std::string_view selfEndpoint)
{
    switch (target.source)
    {
        case Wire::WireSharedCacheSource::None:
            return Wire::SharedCacheStatusFields { .source = Wire::WireSharedCacheSource::None,
                                                   .machineId = {},
                                                   .endpoint = {},
                                                   .state = Wire::WireSharedCacheState::NotTried,
                                                   .detail = {} };
        case Wire::WireSharedCacheSource::Override:
            return OverrideStatus(target);
        case Wire::WireSharedCacheSource::Setting:
            // The target this report already read: a second read could straddle an apply.
            return fleet != nullptr ? fleet->ReportFor(target) : UnreachedStatus(target);
        case Wire::WireSharedCacheSource::ThisMachine:
            return ThisMachineStatus(target, host, selfEndpoint);
    }
    // Unreachable: the directory builds every target from these four. Closed anyway, as `None`,
    // because falling off the end of a function returning a value is undefined.
    return Wire::SharedCacheStatusFields {};
}

NodeSharedCacheStatus::NodeSharedCacheStatus(ISharedCacheTargetSource const& targets,
                                             ISharedCacheStatusSource const* fleet,
                                             SharedCacheHost const* host,
                                             Cc::IAdvertisedEndpointSource const& advertised) noexcept:
    _targets { targets },
    _fleet { fleet },
    _host { host },
    _advertised { advertised }
{
}

Wire::SharedCacheStatusFields NodeSharedCacheStatus::Report() const
{
    // The directory, read ONCE for the whole record; every part is judged against this read.
    return ReportFor(_targets.Current());
}

Wire::SharedCacheStatusFields NodeSharedCacheStatus::ReportFor(SharedCacheTarget const& target) const
{
    return SharedCacheStatusOf(target, _fleet, _host, _advertised.Current());
}

} // namespace FastCache::Node
