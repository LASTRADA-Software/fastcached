// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheDirectory.hpp"

#include <FastCache/Cluster/ClusterState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using Source = CompileCacheWire::WireSharedCacheSource;

namespace
{

/// Two fleet machines, each with its key and its announced 0xFC endpoint, and the shared-cache
/// setting naming @p named.
/// @param named The machine the setting names; empty unsets it.
/// @return The state.
[[nodiscard]] Cluster::ClusterState Fleet(std::string const& named)
{
    Cluster::ClusterState state;
    for (auto const* machine: { "cache-c", "cache-d" })
    {
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                          .key = machine,
                                          .value = "10.0.0.3:6680",
                                          .schedulerEndpoint = std::string { machine } + ".office.example:6674",
                                          .publicKey = Testing::TestKeyPair(machine).PublicKey() });
    }
    state.settings.push_back(Cluster::Setting { .name = std::string { Cluster::SharedCacheSetting }, .value = named });
    return state;
}

} // namespace

TEST_CASE("The shared-cache directory follows every applied state", "[node][shared-cache][directory]")
{
    SharedCacheDirectory directory { "pc-7", {} };
    CHECK(directory.Current().source == Source::None);

    directory.Applied(Fleet("cache-c"));
    CHECK(directory.Current().source == Source::Setting);
    CHECK(directory.Current().resolved.endpoint == "cache-c.office.example:6674");

    // Moved: the very next apply moves this node, with no restart.
    directory.Applied(Fleet("cache-d"));
    CHECK(directory.Current().resolved.machineId == "cache-d");

    // Unset: back to none, and a store is then NotConfigured rather than a failure.
    directory.Applied(Fleet(""));
    CHECK(directory.Current().source == Source::None);

    // A machine the cluster does not hold is still the SETTING's answer: it named one.
    directory.Applied(Fleet("cache-z"));
    CHECK(directory.Current().source == Source::Setting);
    CHECK(directory.Current().resolved.resolution == Cluster::SharedCacheResolution::UnknownMachine);
}

TEST_CASE("A local upstream overrides the setting and is reported as the override", "[node][shared-cache][directory]")
{
    SharedCacheDirectory directory { "pc-7", "cache-old.office.example:6674" };
    // Before any state: the override is what this node reads, from its first operation.
    CHECK(directory.Current().source == Source::Override);

    directory.Applied(Fleet("cache-c"));
    auto const target = directory.Current();
    CHECK(target.source == Source::Override);
    CHECK(target.overrideEndpoint == "cache-old.office.example:6674");
    // The setting is still resolved, so --node-status can say what the override is overriding.
    CHECK(target.resolved.machineId == "cache-c");
}

TEST_CASE("The named machine's own directory says it serves rather than reads", "[node][shared-cache][directory]")
{
    SharedCacheDirectory directory { "cache-c", {} };
    directory.Applied(Fleet("cache-c"));
    CHECK(directory.Current().source == Source::ThisMachine);
}
