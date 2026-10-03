// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include <WorkerProtocol.hpp>
#include <core/platform/Clock.hpp>

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

/// Which lease check a worker built: none yet, one that verifies a grant's signature, or one that
/// verifies nothing.
///
/// **Private**: never transmitted or persisted.
enum class BuiltLeaseCheck : std::uint8_t
{
    None,      ///< No worker built a lease check in this process yet.
    Signed,    ///< `Cc::SignedLeaseValidator`: every grant is verified against the roster.
    Unchecked, ///< `Cc::UncheckedLeaseValidator`: no grant is verified.
};

/// What the running worker's lease check IS, as the factory that built it recorded it.
///
/// A fact about the process rather than about a configuration, and that is the point: whether a
/// worker checks its leases is decided ONCE, at startup, by `MakeWorkerLeaseValidator`, and a
/// reload that widens admission afterwards is safe exactly when what was built verifies grants.
/// Asking flag shapes instead was how a path the shapes did not foresee reached an unchecked
/// compile port (review I-2): this is read by `ReloadCheckWith`, so no future path can widen past
/// an unchecked worker unguarded. Written on the body's thread, read on the reload's.
class LeaseCheckInForce
{
  public:
    /// Record what was built.
    /// @param built The lease check the factory returned.
    void Record(BuiltLeaseCheck built) noexcept
    {
        _built.store(built, std::memory_order_release);
    }

    /// @return The lease check the running worker built, or `None` before any did.
    [[nodiscard]] BuiltLeaseCheck Current() const noexcept
    {
        return _built.load(std::memory_order_acquire);
    }

  private:
    std::atomic<BuiltLeaseCheck> _built { BuiltLeaseCheck::None };
};

/// Build the lease check this node's compile port applies, from its configuration.
///
/// **The node's trust decision, in one function.** It chooses between the two validators
/// `Cc::WorkerProtocol` accepts, from whether this node holds a roster to verify a grant
/// against (#178), and says which one it chose.
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
/// No roster yields `Cc::UncheckedLeaseValidator()` and a warning, and is legitimate for
/// exactly one shape of node: one no other machine can dial. `StartupPolicyRejection` and
/// `NodeRoster::Build` refuse every other shape before this runs, so the choice is made once,
/// in front of the operator, rather than per request where an open port and a zeroed counter
/// look like a healthy fleet.
///
/// Since #178 a grant is signed by the voter that issued it, with its own identity key, and
/// verified against the roster: the state a consensus member applies, or the roster a strict
/// majority of the voters certified on any other node.
///
/// @param cfg What this node was told to be.
/// @param roster What a grant is verified against, or null when this node holds none. Borrowed
///        by the validator, so it must outlive it.
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
/// @param advertise Where this worker's address is read, asked per request rather
///        than copied here -- exactly the seam the registration reads, because the
///        string the scheduler signs into a grant and the string this checks against
///        have to be one fact at every moment, not two readers of one file (#1279).
///        Borrowed by the validator, so it must outlive it.
/// @param clock Where "now" comes from. A **wall** clock, not a steady one: the
///        expiry was stamped on another machine, and a steady instant means nothing
///        off the host that read it. Borrowed, so it must outlive the validator.
/// @param lease What this node keeps between lease checks -- the grants it has already
///        run (#614), the scheduler term it last learned (#421), and where a term going
///        backwards is reported. Borrowed by the validator, so it must outlive it, and
///        shared by every compile thread. Taken even on the paths that build an
///        unchecked validator, because whether a node has a roster is not a reason for its
///        caller to hold a different set of objects.
/// @param metrics Where an adopted term reset is counted.
/// @param logger Where the chosen mode is announced.
/// @param inForce Where the check it built is recorded, before it is returned -- folded into the
///        operation, so no worker can build one without the reload guard knowing which.
/// @return The validator, or why this node must not serve.
[[nodiscard]] std::expected<Cc::LeaseValidator, std::string> MakeWorkerLeaseValidator(
    NodeConfig const& cfg,
    Distributed::ILeaseRoster const* roster,
    Cc::IAdvertisedEndpointSource const& advertise,
    SocketActivation activation,
    core::platform::WallClockRef clock,
    Distributed::WorkerLeaseState& lease,
    IMetricsSink& metrics,
    ILogger& logger,
    LeaseCheckInForce& inForce);

/// Why a reload that widens admission on a worker whose lease check verifies nothing is refused.
inline constexpr std::string_view ReloadWidensUncheckedWorkerRefusal =
    "a reload may not widen admission with --fleet-open while this worker compiles WITHOUT verifying lease "
    "signatures: it chose that lease check at startup, when no machine but this one could reach its compile verbs, and "
    "widening now would open an unauthenticated compile port with every refusal counter reading zero. Restart the "
    "node with the admission you want, so it chooses its lease check for it, or leave the admission policy as it is.";

/// Whether a reload from @p previous to @p candidate widens admission while the running worker's
/// lease check verifies nothing.
///
/// Asked as a WIDENING, never as a state: a worker already admitting remote peers passed its own
/// startup rules, and may reload freely, narrowing included -- the one edit that makes it safer.
/// @param previous The configuration in force.
/// @param candidate The one the reload would publish.
/// @param built What the running worker built (`LeaseCheckInForce::Current`).
/// @return True when the reload must be refused.
[[nodiscard]] bool ReloadWidensUncheckedWorker(NodeConfig const& previous,
                                               NodeConfig const& candidate,
                                               BuiltLeaseCheck built);

/// The check a running node's reloader applies: `ValidateNodeReloadable`, then the one rule about
/// what the running worker BUILT (`ReloadWidensUncheckedWorker`), asked of @p inForce at the moment
/// of each reload. LAST, so a save that both widens and moves an unreloadable setting is told about
/// every setting first.
/// @param inForce What the running worker recorded. Borrowed, so it must outlive the reloader.
/// @return The check.
[[nodiscard]] std::function<std::expected<void, ConfigError>(NodeConfig const&, NodeConfig const&)> ReloadCheckWith(
    LeaseCheckInForce const& inForce);

} // namespace FastCache::Node
