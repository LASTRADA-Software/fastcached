// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ClusterKeySource.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <string>
#include <string_view>

namespace FastCache::Cc
{
struct Credential;
class CredentialNotice;
} // namespace FastCache::Cc

namespace FastCache::Node
{

/// @file NodeProofClient.hpp
/// The presenting half of the cluster-key proof: what a node sends before it registers.
///
/// A node's own registration is decided by the SCHEDULER's membership answer, which is a list of
/// addresses -- so a machine whose address is not stable is refused, however long it has held
/// this fleet's key ([#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428),
/// [#178](https://github.com/LASTRADA-Software/fastcached/issues/178) item 1). This is what a
/// node does about that: one exchange at the top of each heartbeat round's connection, before
/// the withdrawals and the registrations that connection carries.
///
/// ## Once per CONNECTION, never per verb
///
/// What a proof establishes is connection state, so a round that dials once proves once and
/// every registration and heartbeat on that connection is a member's. A node whose address IS
/// listed proves anyway and is admitted by both routes, which is what
/// `Distributed::ExplainConnection` unions rather than choosing between -- so nothing has to
/// decide in advance whether the proof was needed.
///
/// ## Every way of not being proved is an ORDINARY state during a rollout
///
/// A peer too old to know the verbs, a peer holding no cluster key, a peer that refused the tag:
/// the first two are what a mixed fleet looks like mid-upgrade and the third is a machine with
/// the wrong `--cluster-key-file`. Only the third is a diagnosis, so they are four outcomes and
/// not a `bool` -- and a caller carries on in every one of them, because a node that stopped
/// registering when a proof was unavailable would take itself out of a fleet that was admitting
/// it perfectly well by address.

/// What one attempt to prove the cluster key over one connection learned.
///
/// **Four outcomes, and the reason is the rulebook's**: *skipped*, *absent*, *refused* and
/// *proved* are four states, a `bool` can carry two of them, and the one an operator has to act
/// on is the one a `bool` would fold into the others.
enum class NodeProofResult : std::uint8_t
{
    /// The peer accepted the tag. Every verb after this on the connection is a member's.
    Proved,
    /// This node holds no cluster key, so there was nothing to present. Not an event.
    NothingToPresent,
    /// The peer serves no proof: a build too old to know the verbs, or a node holding no
    /// cluster key. What a mixed fleet looks like mid-upgrade, and not an event either.
    NotOffered,
    /// The peer answered and did not accept, or the exchange did not complete.
    ///
    /// The one outcome worth a log line: a wrong `--cluster-key-file` on THIS machine is the
    /// commonest rollout mistake there is, and it is invisible from here in every other way --
    /// the node goes on being admitted by address, or refused by it, with nothing saying which
    /// question was answered.
    Refused,
};

/// What the attempt learned, and what to say about it.
struct NodeProofAttempt
{
    NodeProofResult result { NodeProofResult::NothingToPresent }; ///< What happened.
    /// The peer's own words when it refused, or empty. Never this node's paraphrase of them: a
    /// wrong key and a node that has never heard of the verb call for opposite actions, and only
    /// the peer can tell them apart from its own side.
    std::string reason {};
};

/// Prove this node's cluster key over @p peer, before anything else is sent on it.
///
/// Two exchanges: `NodeChallenge` for the peer's nonce, then `ProveNode` with the tag. The
/// challenge is the PEER's and is per connection, so a captured proof is useless on a second
/// one -- and it is spent whatever the outcome, which is why a refusal here is final for this
/// connection rather than something to retry on it.
///
/// @param peer The connected socket, before any other verb has been sent on it.
/// @param notice Where a credential the peer did not want is reported, as every other exchange
///        on this wire reports it.
/// @param key Where this node's cluster key is read from, at the moment it is presented -- never
///        captured, which is this project's rule for a credential and is what lets a rotated key
///        file reach a running node.
/// @param nodeId The label to bind inside the tag. **Legitimately EMPTY**: an identity is minted
///        only by a node that runs consensus, and a keyed worker without one still has a key to
///        prove. What an empty label costs is that the peer's log names the address alone.
/// @param credential What this node presents to the peer, as every other verb presents it.
/// @return What the attempt learned.
[[nodiscard]] NodeProofAttempt ProveNodeOver(ISocket& peer,
                                             Cc::CredentialNotice& notice,
                                             IClusterKeySource const& key,
                                             std::string_view nodeId,
                                             Cc::Credential const& credential);

} // namespace FastCache::Node
