// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// Whether a supervisor handed this worker its listening socket.
///
/// An `enum class` rather than a `bool` because it is an API-surface argument and a
/// bare `true` at the call site says nothing about which way round it is.
enum class SocketActivation : std::uint8_t
{
    No = 0, ///< This process bound its own port, so `--bind` describes it.
    Yes,    ///< A socket unit owns the port, and `--bind` describes nothing.
};

/// Build the lease check this node's compile port applies, from its configuration.
///
/// **The node's trust decision, in one function.** It reads the cluster key, chooses
/// between the two validators `Cc::WorkerProtocol` accepts, and says which one it
/// chose.
///
/// A file of its own rather than a corner of `WorkerServer`, which is the idiom this
/// directory already follows -- `ScratchClaim`, `NodeToolchains` and `NodeMembership`
/// are each one small file for one node-level policy. It is emphatically not a
/// `WorkerServer` concern: that class never calls this, takes no validator and
/// returns none, and parking it there put the node's entire parsed configuration on
/// the interface of a header whose subject is a socket and an executor.
///
/// It is out of `main` for the reason this repository states as a rule: a check
/// nothing constructs is the bug it was written to fix, and `main` is the one
/// translation unit no test can reach. That is exactly where an accept-all lambda
/// survived a fully passing suite (#282).
///
/// An absent `--cluster-key-file` yields `Cc::UncheckedLeaseValidator()` and a
/// warning, and is legitimate for exactly one shape of node: one no other machine
/// can dial. `StartupPolicyRejection` refuses every other shape before this runs, so
/// the choice is made once, in front of the operator, rather than per request where
/// an open port and a zeroed counter look like a healthy fleet.
///
/// A key file that cannot be READ is an error, never a quiet fall back to checking
/// nothing -- a node told to verify and unable to must not serve.
///
/// @param cfg What this node was told to be; `clusterKeyFile` is the field read.
/// @param activation Whether the listener was inherited. **Load-bearing, and the
///        reason this refuses rather than only warning.** `StartupPolicyRejection`
///        decides reachability from `--bind`, which is the right answer for a node
///        that binds its own port and is worth nothing for one that does not: under
///        socket activation the unit chose the address -- the shipped
///        `fastcache-compile-node.socket` says `ListenStream=6676`, every interface --
///        and a leftover `--bind=127.0.0.1` in `FASTCACHE_NODE_ARGS` is ignored while
///        still telling the table this port is local. That combination passed the
///        table, built the unchecked validator and served an unauthenticated compile
///        port to the network, with all three refusal counters reading zero: the exact
///        defect #282 exists to close, surviving inside the fix for it. The table
///        keeps its rule because an install must be refused before any tier exists;
///        this is the backstop for the one fact the table cannot see.
/// @param advertise This worker's address as clients are told to dial it -- exactly
///        the string it registers under, because that is the string the scheduler
///        signs into the grant.
/// @param clock Where "now" comes from. A **wall** clock, not a steady one: the
///        expiry was stamped on another machine, and a steady instant means nothing
///        off the host that read it. Borrowed, so it must outlive the validator.
/// @param term What this node knows about the scheduler's term (#421). Borrowed by
///        the validator, so it must outlive it -- and it is the one object the
///        heartbeat thread and the compile threads share. Taken even on the paths
///        that build an unchecked validator, because whether a node has a cluster key
///        is not a reason for its caller to hold a different set of objects.
/// @param logger Where the chosen mode is announced.
/// @return The validator, or why the key file cannot serve as one.
[[nodiscard]] std::expected<Cc::LeaseValidator, std::string> MakeWorkerLeaseValidator(
    NodeConfig const& cfg,
    std::string_view advertise,
    SocketActivation activation,
    IWallClock const& clock,
    Distributed::KnownSchedulerTerm& term,
    ILogger& logger);

} // namespace FastCache::Node
