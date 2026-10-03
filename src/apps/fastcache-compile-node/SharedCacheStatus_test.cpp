// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheHost.hpp"
#include "SharedCacheStatus.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/SharedCacheTarget.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include <WorkerProtocol.hpp>
#include <tests/SharedTierFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::MemoryOpener;
using FastCache::Testing::NamingSharedCache;

namespace
{

namespace Wire = CompileCacheWire;

/// A fleet half that reports one fixed record.
class FixedShared final: public ISharedCacheStatusSource
{
  public:
    /// @param fields What `Report()` answers.
    explicit FixedShared(Wire::SharedCacheStatusFields fields):
        _fields { std::move(fields) }
    {
    }

    [[nodiscard]] Wire::SharedCacheStatusFields Report() const override
    {
        return _fields;
    }

    [[nodiscard]] Wire::SharedCacheStatusFields ReportFor(SharedCacheTarget const& /*target*/) const override
    {
        return _fields;
    }

  private:
    Wire::SharedCacheStatusFields _fields;
};

/// A directory whose answer the case sets.
class SettableTargets final: public ISharedCacheTargetSource
{
  public:
    [[nodiscard]] SharedCacheTarget Current() const override
    {
        return target;
    }

    SharedCacheTarget target; ///< What `Current()` answers.
};

/// An advertised endpoint the case sets.
class SettableAdvertised final: public Cc::IAdvertisedEndpointSource
{
  public:
    [[nodiscard]] std::string Current() const override
    {
        return endpoint;
    }

    std::string endpoint; ///< What `Current()` answers.
};

/// @param source Where the answer comes from.
/// @param machine What the setting names, or empty.
/// @param resolution How it resolved.
/// @return The target.
[[nodiscard]] SharedCacheTarget TargetOf(Wire::WireSharedCacheSource source,
                                         std::string machine = {},
                                         Cluster::SharedCacheResolution resolution = Cluster::SharedCacheResolution::Unset)
{
    SharedCacheTarget target;
    target.source = source;
    target.resolved.resolution = resolution;
    target.resolved.machineId = std::move(machine);
    return target;
}

/// A fleet half reporting a proof, to tell a delegated answer from one made up here.
[[nodiscard]] Wire::SharedCacheStatusFields AProvenReport()
{
    return Wire::SharedCacheStatusFields { .source = Wire::WireSharedCacheSource::Setting,
                                           .machineId = "cache-c",
                                           .endpoint = "10.0.0.3:6674",
                                           .state = Wire::WireSharedCacheState::Proven,
                                           .detail = {} };
}

} // namespace

TEST_CASE("A node with no shared cache says none rather than saying nothing", "[node][status][shared-cache]")
{
    // Everything else wired, so the answer is the source's and not a missing collaborator's.
    FixedShared const fleet { AProvenReport() };
    NullLogger logger;
    MemoryOpener opener;
    SharedCacheHost const host { "pc-7", opener, nullptr, logger, ReconcileOn::Caller };

    auto const report = SharedCacheStatusOf(SharedCacheTarget {}, &fleet, &host, "10.0.0.7:6674");
    CHECK(report.source == Wire::WireSharedCacheSource::None);
    CHECK(report.state == Wire::WireSharedCacheState::NotTried);
    CHECK(report.machineId.empty());
    CHECK(report.endpoint.empty());
}

TEST_CASE("A node with a local upstream reports it as the override and names what it overrides",
          "[node][status][shared-cache]")
{
    auto target = TargetOf(Wire::WireSharedCacheSource::Override, "cache-c", Cluster::SharedCacheResolution::Resolved);
    target.overrideEndpoint = "cache-old.office.example:6674";

    SECTION("the setting names a machine: the override says which")
    {
        auto const report = SharedCacheStatusOf(target, nullptr, nullptr, {});
        CHECK(report.source == Wire::WireSharedCacheSource::Override);
        CHECK(report.endpoint == "cache-old.office.example:6674");
        CHECK(report.state == Wire::WireSharedCacheState::NotTried);
        CHECK(report.detail.contains("overrides"));
        CHECK(report.detail.contains("cache-c"));
    }
    SECTION("the setting names nobody: nothing is claimed to be overridden")
    {
        target.resolved = {};
        auto const report = SharedCacheStatusOf(target, nullptr, nullptr, {});
        CHECK(report.endpoint == "cache-old.office.example:6674");
        CHECK_FALSE(report.detail.contains("overrides"));
        CHECK(report.detail.contains("names no machine"));
    }
}

TEST_CASE("Another machine named by the setting is reported by the fleet half that reaches it",
          "[node][status][shared-cache]")
{
    auto const target = TargetOf(Wire::WireSharedCacheSource::Setting, "cache-c", Cluster::SharedCacheResolution::Resolved);
    FixedShared const fleet { AProvenReport() };
    CHECK(SharedCacheStatusOf(target, &fleet, nullptr, {}) == AProvenReport());
}

TEST_CASE("A setting nothing here reads through to says so, and one no node could use is unresolved",
          "[node][status][shared-cache]")
{
    // No fleet half: a node keeping no private tier, or holding no identity.
    SECTION("resolved: named, reached by nothing here")
    {
        auto target = TargetOf(Wire::WireSharedCacheSource::Setting, "cache-c", Cluster::SharedCacheResolution::Resolved);
        target.resolved.endpoint = "10.0.0.3:6674";
        auto const report = SharedCacheStatusOf(target, nullptr, nullptr, {});
        CHECK(report.machineId == "cache-c");
        CHECK(report.endpoint == "10.0.0.3:6674");
        CHECK(report.state == Wire::WireSharedCacheState::NotTried);
        CHECK(report.detail.contains("reads nothing through"));
    }
    SECTION("unresolved: said with the resolution's own reason")
    {
        auto const target =
            TargetOf(Wire::WireSharedCacheSource::Setting, "cache-c", Cluster::SharedCacheResolution::NoEndpoint);
        auto const report = SharedCacheStatusOf(target, nullptr, nullptr, {});
        CHECK(report.state == Wire::WireSharedCacheState::Unresolved);
        CHECK(report.detail == Cluster::RowOf(Cluster::SharedCacheResolution::NoEndpoint).why);
    }
}

TEST_CASE("The named machine reports whether it is serving, at the endpoint it advertises", "[node][status][shared-cache]")
{
    NullLogger logger;
    auto const named = NamingSharedCache("cache-c");
    auto const target =
        TargetOf(Wire::WireSharedCacheSource::ThisMachine, "cache-c", Cluster::SharedCacheResolution::ThisMachine);
    // A fleet half too, which must NOT be consulted: this machine's tier is the host's to report.
    FixedShared const fleet { AProvenReport() };

    SECTION("serving")
    {
        MemoryOpener working;
        SharedCacheHost serving { "cache-c", working, nullptr, logger, ReconcileOn::Caller };
        serving.Applied(named);
        serving.Reconcile();
        auto const up = SharedCacheStatusOf(target, &fleet, &serving, "10.0.0.3:6674");
        CHECK(up.source == Wire::WireSharedCacheSource::ThisMachine);
        CHECK(up.machineId == "cache-c");
        CHECK(up.endpoint == "10.0.0.3:6674");
        CHECK(up.state == Wire::WireSharedCacheState::Serving);
        CHECK(up.detail.empty());
    }
    SECTION("unavailable, with the host's reason")
    {
        MemoryOpener failing;
        failing.fail = true;
        SharedCacheHost broken { "cache-c", failing, nullptr, logger, ReconcileOn::Caller };
        broken.Applied(named);
        broken.Reconcile();
        auto const down = SharedCacheStatusOf(target, &fleet, &broken, "10.0.0.3:6674");
        CHECK(down.state == Wire::WireSharedCacheState::Unavailable);
        CHECK(down.detail.contains("another process"));
    }
    SECTION("named, not reconciled yet: a window, and said as one")
    {
        MemoryOpener working;
        SharedCacheHost opening { "cache-c", working, nullptr, logger, ReconcileOn::Caller };
        opening.Applied(named);
        auto const pending = SharedCacheStatusOf(target, &fleet, &opening, "10.0.0.3:6674");
        CHECK(pending.state == Wire::WireSharedCacheState::Unavailable);
        CHECK(pending.detail.contains("not open yet"));
    }
}

TEST_CASE("The node's shared-cache status is asked of its sources at every report", "[node][status][shared-cache]")
{
    // Live, not captured: an apply that moves the setting and a re-advertise both show on the next
    // answer, which a status built once at startup would not.
    SettableTargets targets;
    SettableAdvertised advertised;
    advertised.endpoint = "10.0.0.7:6674";
    NullLogger logger;
    MemoryOpener opener;
    SharedCacheHost host { "pc-7", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("pc-7"));
    host.Reconcile();
    FixedShared const fleet { AProvenReport() };
    NodeSharedCacheStatus const status { targets, &fleet, &host, advertised };

    CHECK(status.Report().source == Wire::WireSharedCacheSource::None);

    targets.target = TargetOf(Wire::WireSharedCacheSource::Setting, "cache-c", Cluster::SharedCacheResolution::Resolved);
    CHECK(status.Report() == AProvenReport());

    targets.target = TargetOf(Wire::WireSharedCacheSource::ThisMachine, "pc-7", Cluster::SharedCacheResolution::ThisMachine);
    CHECK(status.Report().state == Wire::WireSharedCacheState::Serving);
    CHECK(status.Report().endpoint == "10.0.0.7:6674");
    advertised.endpoint = "10.0.0.8:6674";
    CHECK(status.Report().endpoint == "10.0.0.8:6674");
}
