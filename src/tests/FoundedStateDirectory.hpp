// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <apps/fastcache-compile-node/NodeKey.hpp>
#include <tests/SecureRandomFakes.hpp>

/// @file FoundedStateDirectory.hpp
/// A state directory as a node that FOUNDED its cluster leaves it on disk (#1663).
///
/// Shared rather than written per file, because the defect it exists for lived in exactly the
/// convenience a hand-built fixture takes: the pin cases set `NodeConfig::identityPublicKey` on the
/// configuration they judged, which `main` has not resolved when it judges the record. This writes
/// the KEY FILE and the RECORD the way a start does, and leaves the key to be found where a start
/// finds it.
namespace FastCache::Testing
{

/// A cluster id spelled as a mint spells one, so `--fleet-id` parses it.
inline constexpr std::string_view FoundedClusterId = "0123456789abcdef0123456789abcdef";

/// The record a node keeps once it has founded a fleet others joined: a VOTER in its own cluster,
/// which commits it to that cluster under its own key (`Node::CommittedUnderOwnKey`).
/// @param clusterId Its own cluster's id.
/// @return The record.
[[nodiscard]] inline Cluster::FormationRecord FoundedVoterRecord(std::string_view clusterId = FoundedClusterId)
{
    return Cluster::FormationRecord { .mode = Cluster::NodeMode::Voter,
                                      .own = { .clusterId = std::string { clusterId }, .createdAtUnixSeconds = 100 },
                                      .joining = std::nullopt,
                                      .fleet = std::nullopt,
                                      .archivePending = std::nullopt,
                                      .rejectedBy = std::nullopt,
                                      .askedJoins = {} };
}

/// Write @p record into @p directory beside an identity key minted the way a start mints it
/// (`Node::ResolveNodeKey`).
/// @param directory The state directory.
/// @param record The record to keep there.
/// @return The public half of the key the directory now holds.
[[nodiscard]] inline Ed25519PublicKey WriteFoundedStateDirectory(
    std::filesystem::path const& directory, Cluster::FormationRecord const& record = FoundedVoterRecord())
{
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 7) };
    Node::FileTrustNodeKeyGuard guard;
    auto const key = Node::ResolveNodeKey(directory, random, guard);
    REQUIRE(key.has_value());
    REQUIRE(key->origin == Node::NodeKeyOrigin::Minted);
    Cluster::FileFormationStore store { directory };
    REQUIRE(store.Save(record).has_value());
    return key->pair.PublicKey();
}

} // namespace FastCache::Testing
