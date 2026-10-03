// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"
#include "SharedCacheUpstream.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <tests/SharedCacheFleet.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::SharedCacheFleet;

namespace
{

namespace Wire = FastCache::CompileCacheWire;

/// A stored object told apart by one byte, so a hit on the wrong key cannot pass for the right one.
/// @param tag The byte.
/// @return The encoded value.
[[nodiscard]] std::vector<std::byte> AValue(char tag)
{
    CompileValue compiled;
    compiled.objectBlob = std::vector<std::byte> { std::byte { static_cast<unsigned char>(tag) } };
    return EncodeCompileValue(compiled);
}

/// The tag `AValue` wrote into a fetched value, compared DECODED because the shared tier stores
/// the canonical form rather than the bytes it was sent.
/// @param fetched What a fetch answered.
/// @return The tag, or nullopt for a miss or a value that is not one `AValue` wrote.
[[nodiscard]] std::optional<char> TagOf(std::optional<std::vector<std::byte>> const& fetched)
{
    if (!fetched.has_value())
        return std::nullopt;
    auto const decoded = DecodeCompileValue(*fetched);
    if (!decoded.has_value() || decoded->objectBlob.size() != 1)
        return std::nullopt;
    return static_cast<char>(std::to_integer<unsigned char>(decoded->objectBlob.front()));
}

} // namespace

TEST_CASE("Every node reads through to the machine the fleet names, having proved its key", "[node][fleet][shared-cache]")
{
    SharedCacheFleet fleet;
    fleet.AddCacheMachine("cache-c");
    fleet.AddNode("pc-7");
    fleet.AddNode("pc-8");
    fleet.Announce("cache-c");
    fleet.Name("cache-c");
    fleet.Publish();

    CHECK(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("k", AValue('1'))) == UpstreamStore::Stored);
    // A second object on the same machine, so the fetch below is answered by KEY and not by presence.
    CHECK(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("j", AValue('2'))) == UpstreamStore::Stored);
    CHECK(TagOf(core::async::syncRun(fleet.NodeOf("pc-8").upstream.Fetch("k"))) == '1');
    CHECK(fleet.Machine("cache-c").opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
    CHECK(fleet.Machine("cache-c").metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 2);
    // One kept session per node: the store and the fetch each opened exactly one.
    CHECK(fleet.NodeOf("pc-7").metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(fleet.NodeOf("pc-8").metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
}

TEST_CASE("A machine answering at the named machine's address with another key is sent nothing",
          "[node][fleet][shared-cache]")
{
    // cache-c's record points at cache-d's port: an address reassigned between two fleet machines.
    SharedCacheFleet fleet;
    fleet.AddCacheMachine("cache-c");
    fleet.AddCacheMachine("cache-d");
    fleet.AddNode("pc-7");
    fleet.AnnounceAt("cache-c", "cache-d");
    fleet.Name("cache-c");
    fleet.Publish();

    auto& node = fleet.NodeOf("pc-7");
    CHECK(core::async::syncRun(node.upstream.Store("k", AValue('1'))) == UpstreamStore::Declined);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 1);
    constexpr auto challenge = static_cast<std::uint8_t>(Wire::Op::NodeChallenge);
    REQUIRE_FALSE(node.connector.Ops().empty());
    CHECK(std::ranges::all_of(node.connector.Ops(), [](std::uint8_t op) { return op == challenge; }));
    // cache-d is dormant and would have refused anyway -- but it was never ASKED, which is the property.
    CHECK(fleet.Machine("cache-d").metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotServing) == 0);
    CHECK(fleet.Machine("cache-d").metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 0);
}

TEST_CASE("Moving the setting moves every node at the next apply, with no restart", "[node][fleet][shared-cache]")
{
    SharedCacheFleet fleet;
    fleet.AddCacheMachine("cache-c");
    fleet.AddCacheMachine("cache-d");
    fleet.AddNode("pc-7");
    fleet.AddNode("pc-8");
    fleet.Announce("cache-c");
    fleet.Announce("cache-d");
    fleet.Name("cache-c");
    fleet.Publish();
    REQUIRE(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("before", AValue('1'))) == UpstreamStore::Stored);

    fleet.Name("cache-d");
    fleet.Publish();
    CHECK(fleet.Machine("cache-c").host.Current() == nullptr);
    CHECK(fleet.Machine("cache-d").host.Current() != nullptr);

    auto const cBefore = fleet.Machine("cache-c").metrics.Read(IMetricsSink::Counter::NodeProofsAccepted);
    CHECK(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("after", AValue('2'))) == UpstreamStore::Stored);
    CHECK(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("also", AValue('1'))) == UpstreamStore::Stored);
    CHECK(TagOf(core::async::syncRun(fleet.NodeOf("pc-8").upstream.Fetch("after"))) == '2');
    // The old machine is not dialled at all once the state moved -- not even for the handshake -- and
    // the session pc-7 kept to it was dropped rather than reused.
    CHECK(fleet.Machine("cache-c").metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == cBefore);
    // And what it held stays there: a move is not a migration.
    CHECK_FALSE(core::async::syncRun(fleet.NodeOf("pc-8").upstream.Fetch("before")).has_value());
}

TEST_CASE("Unsetting the setting stops every node using the shared cache", "[node][fleet][shared-cache]")
{
    SharedCacheFleet fleet;
    fleet.AddCacheMachine("cache-c");
    fleet.AddNode("pc-7");
    fleet.Announce("cache-c");
    fleet.Name("cache-c");
    fleet.Publish();
    REQUIRE(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("k", AValue('1'))) == UpstreamStore::Stored);
    auto const dialled = fleet.NodeOf("pc-7").connector.Ops().size();

    fleet.Name("");
    fleet.Publish();
    CHECK(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("k2", AValue('2'))) == UpstreamStore::NotConfigured);
    CHECK_FALSE(fleet.NodeOf("pc-7").upstream.Configured());
    CHECK(fleet.NodeOf("pc-7").connector.Ops().size() == dialled);
    CHECK(fleet.Machine("cache-c").host.Current() == nullptr);
}

TEST_CASE("A forgotten shared-cache machine is dropped by every node while the setting still names it",
          "[node][fleet][shared-cache][forget]")
{
    SharedCacheFleet fleet;
    fleet.AddCacheMachine("cache-c");
    fleet.AddNode("pc-7");
    fleet.Announce("cache-c");
    fleet.Name("cache-c");
    fleet.Publish();
    REQUIRE(core::async::syncRun(fleet.NodeOf("pc-7").upstream.Store("k", AValue('1'))) == UpstreamStore::Stored);
    auto const dialled = fleet.NodeOf("pc-7").connector.Ops().size();

    fleet.Forget("cache-c");
    fleet.Publish();
    auto& node = fleet.NodeOf("pc-7");
    // The setting still names it -- a forget does not rewrite configuration -- and every node refuses to use it.
    CHECK(fleet.State().SettingOf(Cluster::SharedCacheSetting) == std::optional<std::string> { "cache-c" });
    CHECK(core::async::syncRun(node.upstream.Store("k2", AValue('2'))) == UpstreamStore::Declined);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeSharedCacheUnresolved) == 1);
    CHECK(node.connector.Ops().size() == dialled);
    CHECK(node.upstream.Report().state == Wire::WireSharedCacheState::Unresolved);
}
