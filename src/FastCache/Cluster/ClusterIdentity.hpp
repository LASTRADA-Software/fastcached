// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/IRandomSource.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace FastCache::Cluster
{

/// Which fleet this is, as a value a lease token can be signed over.
///
/// ## Why it is minted rather than configured
///
/// The failure this exists to close is two fleets provisioned from one
/// `--cluster-key-file` — the ordinary result of copying a working configuration to
/// a second site, or cloning staging from production. Cluster A's grant then
/// verifies perfectly on cluster B's worker: the MAC is valid, the endpoint matches,
/// the fingerprint matches, the expiry is in the future
/// ([#322](https://github.com/LASTRADA-Software/fastcached/issues/322)).
///
/// So the identity may not come from anywhere the copy would carry:
///
/// - **Not a config flag.** The act that causes the bug is copying a config file,
///   and a `--cluster-id` in it is copied by the same `cp`. An identity an operator
///   has to remember to change is an identity that is not changed.
/// - **Not derived from the key.** Two fleets sharing a key would then be identical
///   by construction, in precisely the case the field exists to tell apart.
/// - **Not derived from the member set.** Membership is replicated and *changes* —
///   adding a node would silently re-identify the fleet and invalidate every
///   outstanding grant — and a bootstrap set can legally be empty (`--raft-join`).
///
/// What is left is a value drawn from `IRandomSource` on first start and kept.
///
/// ## Why it is persisted
///
/// Because a worker pins the first identity it authenticates and refuses every
/// other one. An identity that changed per process would mean a scheduler restart
/// stops the fleet: the scheduler mints a new id, signs new grants with it, and
/// every worker refuses them with a security-flavoured error it cannot tell from a
/// genuine foreign grant — which is exactly the distinction the field exists to make
/// and therefore cannot be relaxed. Persisted, a restart is ordinary.
///
/// ## Where it lives, and the honest residual
///
/// With consensus running the identity is a **replicated setting**, so every member
/// of one cluster agrees on one value and leadership moving does not re-identify the
/// fleet. Without consensus — a node with no `--node-id`, which `SchedulerTier` calls
/// "what most people run" — there is no replicated state, and a lone scheduler simply
/// owns a file.
///
/// **The residual, stated rather than papered over: cloning a whole machine image,
/// state directory included, still copies the identity.** What this closes is copying
/// a *configuration*, which is the act the ticket describes and the one an operator
/// performs deliberately and often. Copying a state directory is a different act, and
/// a different ticket if anyone wants it closed.

/// How many characters an identity has: two 64-bit draws, hex encoded.
///
/// 128 bits, so two fleets colliding by accident is not a thing that happens —
/// which matters because a collision here is silent and reintroduces the exact bug.
inline constexpr std::size_t ClusterIdLength = 32;

/// The replicated setting an identity is stored under, when there is a cluster.
inline constexpr std::string_view ClusterIdSetting = "cluster-id";

/// The file a lone scheduler keeps its identity in, inside its state directory.
inline constexpr std::string_view ClusterIdFileName = "cluster-id";

/// Draw a fresh identity.
/// @param random Where the bits come from; injected, so a test can seed it.
/// @return 32 lowercase hex characters.
[[nodiscard]] std::string MintClusterId(IRandomSource& random);

/// Whether @p id is shaped like one of ours.
///
/// Checked because the value reaches here from a file an operator can edit and from
/// replicated state a peer wrote, and a malformed one must be refused where it
/// enters rather than signed into every grant.
/// @param id The candidate.
/// @return True when it is exactly `ClusterIdLength` lowercase hex characters.
[[nodiscard]] bool IsWellFormedClusterId(std::string_view id) noexcept;

/// Read this machine's identity, minting and storing one the first time.
///
/// The no-consensus path. Creating the file is the minting event, so two schedulers
/// pointed at one directory would race — which cannot happen, because a state
/// directory is one node's and `ScratchClaim` already refuses the sharing that would
/// make it possible.
///
/// @param path The file to read or create.
/// @param random Where a fresh identity comes from, when there is none to read.
/// @return The identity, or why this machine cannot establish one. A caller that
///         gets an error must not fall back to signing without an id: that is the
///         bug, reached by a different route.
[[nodiscard]] std::expected<std::string, std::string> LoadOrMintClusterId(std::filesystem::path const& path,
                                                                          IRandomSource& random);

} // namespace FastCache::Cluster
