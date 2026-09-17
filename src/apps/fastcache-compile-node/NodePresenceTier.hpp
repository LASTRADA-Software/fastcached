// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheTier.hpp"
#include "EndpointDialer.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// What the presence loop borrows from the node. All of it outlives the loop.
struct NodePresenceParts
{
    NodeConfig const& cfg;                          ///< Where the schedulers are.
    Distributed::NodeCapacity const& capacity;      ///< What this machine is.
    Cc::IAdvertisedEndpointSource const& announced; ///< Where it answers; the key its row is filed under.
    CacheTier const* cacheTier;                     ///< Null on a node with no cache.
    IMetricsSink const& metrics;                    ///< Where the cache figures are read.
    FleetSampler& sampler;                          ///< This machine's own series, and its history.
    ICredentialSource const& credential;            ///< What the announcement presents.
    ILogger& logger;                                ///< Where a refusal is named.
};

/// What one presence announcement is made of.
///
/// **A record, and the round over it is a free function, because that is the half worth
/// testing.** The loop below acquires a thread, a host sampler and a real dialler, none of
/// which a case can drive; what decides anything is *what gets said and whether the history
/// cursor moves*, and that is this. `.agent/rules/testing.md` states the split: take the
/// DECISION out as a function over a record and leave the acquisition alone.
struct PresenceRound
{
    IHostLoadSampler& loadSampler;                    ///< CPU, memory and scratch.
    CacheTier const* cacheTier;                       ///< Null on a node with no cache.
    IMetricsSink const& metrics;                      ///< Where the cache figures are read.
    FleetSampler& sampler;                            ///< This machine's series, and its history.
    ICredentialSource const& credential;              ///< What the announcement presents.
    Cc::CredentialNotice& notice;                     ///< Where an unwanted credential is reported.
    CompileCacheWire::CapacityFields const& capacity; ///< What this machine is.
    std::string_view endpoint;                        ///< Where it answers; the key its row is filed under.
    ILogger& logger;                                  ///< Where a refusal is named.
};

/// Announce this machine once, and hand over the history it owes.
///
/// **The cursor advances only when a scheduler ACCEPTED the batch**, which is the whole reason
/// this returns anything: a round where every endpoint refused has to leave the buckets to be
/// offered again, or a fleet that re-elects loses exactly the history the handover exists to
/// carry across an election.
/// @param round What to say and where the history comes from.
/// @param link Which scheduler to try, and what an answer teaches it.
/// @param dialer How a connection is made.
/// @return Whether a scheduler recorded this machine.
[[nodiscard]] bool AnnounceMachineOnce(PresenceRound const& round, SchedulerLink& link, IEndpointDialer& dialer);

/// The loop that tells a scheduler this MACHINE exists, running on EVERY node.
///
/// **The component `--slots=0` was missing** (#1440). `WorkerTier` is null on such a node, so
/// before this existed a machine with no worker reached the fleet through nothing at all: it
/// was a cluster member that never appeared in `/fleet`'s Machines table and handed over no
/// history, which made a scheduler-only leader invisible on the page it was itself serving.
///
/// **It runs unconditionally, on a worker node too, and that is deliberate.** A verb only the
/// workerless send is a verb exercised exactly where nobody is looking. It also settles the
/// history question: history is filed under the MACHINE, so it rides this verb -- the one
/// every node sends -- rather than `Register`, which a workerless node never sends at all. See
/// `SampleMachineLoad` for why there can be only one carrier.
///
/// Its own thread rather than a share of the worker's, because the worker's heartbeat loop is
/// worker-specific throughout -- the toolchain survey, the registrar list, the cordon wake --
/// and folding a node-level concern into it would put the presence rules behind
/// `RunsWorker(cfg)`. What the two DO share is `SampleMachineLoad`, because they describe one
/// host, and `DialAndAnnounce`, because the scheduler-dial rules may have only one answer.
class NodePresence
{
  public:
    /// Start announcing.
    ///
    /// **Null when this node has no `--scheduler`**, which is the honest off-switch rather than
    /// a flag of its own: `SchedulerLink::For` answers nothing for an empty list, and a node
    /// with nowhere to announce to has no loop to run. A node with `--slots=0` still gets one.
    /// @param parts What the node lends the loop.
    /// @return The running loop, or null when there is no scheduler to announce to.
    [[nodiscard]] static std::unique_ptr<NodePresence> Start(NodePresenceParts const& parts);

    ~NodePresence() = default;

    NodePresence(NodePresence const&) = delete;
    NodePresence& operator=(NodePresence const&) = delete;
    NodePresence(NodePresence&&) = delete;
    NodePresence& operator=(NodePresence&&) = delete;

  private:
    NodePresence(NodePresenceParts const& parts, SchedulerLink link);

    /// Begin the thread. Called by `Start` once construction has finished, never from the
    /// member-initialiser list: the loop touches every member, so it must not start while any
    /// of them is still being built.
    void Launch();

    /// One announcement per interval, until stopped.
    void Loop(std::stop_token const& stop);

    /// Wait out one interval, or return early when stopped.
    ///
    /// A `condition_variable_any` with the stop token rather than a sleep, because a wait
    /// nothing can cancel is a thread a `SIGTERM` has to sit through. One helper rather than
    /// the lock dance at each of the two exits, which is where the two come to differ.
    /// @param stop Requested when the node is shutting down.
    /// @return True when the loop should end.
    [[nodiscard]] bool WaitOutInterval(std::stop_token const& stop);

    // No `_cfg` and no `_capacity`: the scheduler list is read once in `Start` and the
    // capacity is converted once into `_capacityWire` below, so keeping either would be a
    // member nothing reads -- and a second route to a fact that already has one.
    Cc::IAdvertisedEndpointSource const& _announced;
    CacheTier const* _cacheTier;
    IMetricsSink const& _metrics;
    FleetSampler& _sampler;
    ICredentialSource const& _credential;
    ILogger& _logger;

    /// Where a credential the scheduler did not want is reported, once for this loop.
    Cc::CredentialNotice _notice;

    /// This machine's capacity record, converted once: it is compiled-in and configured
    /// state, and nothing about it changes between rounds.
    CompileCacheWire::CapacityFields _capacityWire;

    /// One sampler for the whole loop, never one per round: CPU utilization is a difference
    /// between two readings, so a sampler per round would report nothing, forever. Built with
    /// NO scratch root -- `MakeSystemCounterSource`'s argument is defaulted for exactly this
    /// caller, and a machine with no worker has no scratch directory to measure. Its
    /// `freeScratchBytes` therefore comes back absent, which the page's `scratch-free` column
    /// renders as its dash rather than as a full disk.
    std::unique_ptr<IHostLoadSampler> _loadSampler;

    BlockingEndpointDialer _dialer;
    SchedulerLink _link;

    /// The bounded, cancellable wait between rounds.
    std::mutex _wakeMutex;
    std::condition_variable_any _wake;

    /// **Declared LAST, and the order is load-bearing.** The thread's body touches every
    /// member above it, so they are all constructed before it can start and destroyed only
    /// after `~jthread` has requested a stop and joined. Moving this declaration up turns a
    /// clean shutdown into a use-after-free that no test asserting the loop's output can see.
    std::jthread _thread;
};

} // namespace FastCache::Node
