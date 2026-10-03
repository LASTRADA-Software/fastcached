// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/SharedCacheTarget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace
{
[[nodiscard]] Ed25519PublicKey KeyOf(std::uint8_t fill)
{
    auto key = Ed25519PublicKey {};
    key.fill(static_cast<std::byte>(fill));
    return key;
}

/// A learner's advertised 0xFC endpoint, recorded at join -- or none.
[[nodiscard]] Command Learner(std::string id, Ed25519PublicKey const& key, std::string nodeEndpoint)
{
    return Command { .kind = CommandKind::AddLearner,
                     .key = std::move(id),
                     .value = "10.0.0.3:6680",
                     .schedulerEndpoint = std::move(nodeEndpoint),
                     .publicKey = key,
                     .role = std::nullopt };
}

[[nodiscard]] Command Set(std::string value)
{
    return Command { .kind = CommandKind::SetSetting,
                     .key = std::string { SharedCacheSetting },
                     .value = std::move(value),
                     .schedulerEndpoint = {},
                     .publicKey = std::nullopt,
                     .role = std::nullopt };
}

/// A fleet whose shared cache is C, fully resolvable.
[[nodiscard]] ClusterState Named()
{
    ClusterState state;
    Apply(state, Learner("cache-c", KeyOf(0x11), "cache-c.office.example:6674"));
    Apply(state, Set("cache-c"));
    return state;
}
} // namespace

TEST_CASE("Every shared-cache resolution is one row, in order", "[cluster][shared-cache]")
{
    static_assert(RowsInEnumeratorOrder(SharedCacheResolutionTable, &SharedCacheResolutionRow::resolution));
    // Only a resolved target is dialled; every other answer fails closed.
    for (auto const& row: SharedCacheResolutionTable)
        CHECK(row.dials == (row.resolution == SharedCacheResolution::Resolved));
    // "Configured" is what separates a store nobody asked for from one that failed.
    CHECK_FALSE(RowOf(SharedCacheResolution::Unset).configured);
    CHECK_FALSE(RowOf(SharedCacheResolution::ThisMachine).configured);
    CHECK(RowOf(SharedCacheResolution::NoEndpoint).configured);
    CHECK(RowOf(SharedCacheResolution::KeyRevoked).configured);
}

TEST_CASE("A named machine with a key and an endpoint resolves to both", "[cluster][shared-cache]")
{
    auto const resolved = ResolveSharedCache(Named(), "pc-7");
    CHECK(resolved.resolution == SharedCacheResolution::Resolved);
    CHECK(resolved.machineId == "cache-c");
    CHECK(resolved.endpoint == "cache-c.office.example:6674");
    CHECK(resolved.key == std::optional { KeyOf(0x11) });
}

TEST_CASE("Each way the setting cannot be used resolves to its own answer", "[cluster][shared-cache]")
{
    CHECK(ResolveSharedCache(ClusterState {}, "pc-7").resolution == SharedCacheResolution::Unset);

    auto unset = Named();
    Apply(unset, Set(""));
    CHECK(ResolveSharedCache(unset, "pc-7").resolution == SharedCacheResolution::Unset);

    // The named machine asks about itself: it serves, and reads its tier in process, never by dialling.
    CHECK(ResolveSharedCache(Named(), "cache-c").resolution == SharedCacheResolution::ThisMachine);

    auto noEndpoint = ClusterState {};
    Apply(noEndpoint, Learner("cache-c", KeyOf(0x11), ""));
    Apply(noEndpoint, Set("cache-c"));
    CHECK(ResolveSharedCache(noEndpoint, "pc-7").resolution == SharedCacheResolution::NoEndpoint);

    // A setting that outlived its machine: the forget took the member and revoked the key, and the
    // setting is left alone -- every node resolves it to "unknown" rather than to an address.
    auto forgotten = Named();
    Apply(forgotten, Command { .kind = CommandKind::Forget, .key = "cache-c" });
    auto const gone = ResolveSharedCache(forgotten, "pc-7");
    CHECK(gone.resolution == SharedCacheResolution::UnknownMachine);
    CHECK(gone.machineId == "cache-c");
    CHECK(gone.endpoint.empty());
    CHECK_FALSE(gone.key.has_value());
}

TEST_CASE("A named machine whose live key is also revoked resolves to revoked", "[cluster][shared-cache]")
{
    // Unreachable through `Apply` today (a forget removes the record with the key), and asserted
    // anyway: a state DECODED from a peer is not guaranteed to have come through this build's Apply.
    auto state = Named();
    state.revokedKeys.push_back(RevokedKey { .id = "cache-c", .publicKey = KeyOf(0x11) });
    CHECK(ResolveSharedCache(state, "pc-7").resolution == SharedCacheResolution::KeyRevoked);
}
