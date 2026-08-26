// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ConsensusTier.hpp"
#include "NodeConfig.hpp"

#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IDatagramSocket.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace FastCache::Node
{

/// Read the cluster's pre-shared key from a file.
///
/// **A file rather than a flag, and that is the whole reason this function
/// exists.** A command line is world-readable through `ps` on every POSIX system
/// and through the process list on Windows, and a service's arguments end up in a
/// unit file or a registry key that more accounts can read than can read a
/// mode-0600 file. A key that leaks admits a node to the fleet, and an admitted
/// node is assigned compile jobs and returns objects cached fleet-wide -- so it is
/// object injection into everybody's build, which is why the key never travels
/// anywhere it does not have to.
///
/// Trailing whitespace is stripped, because the overwhelmingly common way to
/// produce one of these is `... > key` or an editor that ends files with a newline
/// -- and a key that differs from its peers' by one byte fails to authenticate with
/// a message about a bad proof rather than about a newline.
/// @param path Where the key is.
/// @return The key bytes, or why the file cannot serve as one.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> ReadClusterKey(std::filesystem::path const& path);

/// LAN discovery, running.
///
/// The loop that turns `Cluster::DiscoveryService` -- which is synchronous, and
/// pure enough that a whole segment forming a cluster is a unit test -- into a
/// thread that announces this node and listens for its peers. Everything it decides
/// lives below it; what it adds is a socket, an interval and somewhere to put the
/// answer.
///
/// **It proposes nothing.** Discovery answers "who has proved they hold the key and
/// where do they answer"; admitting a node is a Raft decision only a leader may
/// make, and a discovery layer that proposed directly would have every node on the
/// segment proposing the same change at once. So the authenticated set goes to
/// `ConsensusTier::Desire`, and the reconciler there decides whether this node is
/// the one that may act on it.
class DiscoveryTier
{
  public:
    /// How long a receive parks before the loop re-checks its stop flag.
    ///
    /// The same rule as every other loop here: POSIX does not unblock a parked
    /// receive when another thread closes the socket, so a poll timeout is the only
    /// portable way a stop is ever observed.
    static constexpr std::chrono::milliseconds PollTimeout { 250 };

    /// Told which peers have proved they hold the cluster key.
    using PeerObserver = std::function<void(std::span<Cluster::DesiredMember const>)>;

    /// Start discovery over a real socket, or explain why it cannot run.
    /// @param cfg The parsed configuration.
    /// @param raftEndpoint This node's consensus endpoint, as its peers dial it.
    /// @param onPeers Told the authenticated set; must outlive the tier.
    /// @param logger Where beacons, joins and rejections are reported.
    /// @return The running tier, or the fatal reason.
    [[nodiscard]] static std::expected<std::unique_ptr<DiscoveryTier>, std::string> Start(NodeConfig const& cfg,
                                                                                          std::string_view raftEndpoint,
                                                                                          PeerObserver onPeers,
                                                                                          ILogger& logger);

    /// Build a tier over a socket somebody else chose, without starting its thread.
    ///
    /// The injection seam, and it exists because the alternative is untestable: the
    /// conditions this loop is for -- two nodes finding each other, one of them
    /// holding the wrong key -- need a segment, and `Net/InMemoryDatagram` is the
    /// segment this repository already has. A caller that takes this door drives
    /// `Step` itself, so a whole cluster forming is a scripted sequence with no
    /// threads and no sleeps in it.
    /// @param socket Where datagrams come from and go; owned.
    /// @param config What this node announces and accepts.
    /// @param onPeers Told the authenticated set; must outlive the tier.
    /// @param logger Where beacons, joins and rejections are reported.
    /// @return The tier, not yet running.
    [[nodiscard]] static std::unique_ptr<DiscoveryTier> Over(std::unique_ptr<IDatagramSocket> socket,
                                                             Cluster::DiscoveryConfig config,
                                                             PeerObserver onPeers,
                                                             ILogger& logger);

    DiscoveryTier(DiscoveryTier const&) = delete;
    DiscoveryTier& operator=(DiscoveryTier const&) = delete;
    DiscoveryTier(DiscoveryTier&&) = delete;
    DiscoveryTier& operator=(DiscoveryTier&&) = delete;

    /// Closes the socket and asks the loop to stop, in that order.
    ~DiscoveryTier();

    /// Announce if due, handle at most one datagram, expire what has gone stale.
    ///
    /// One pass of the loop, public so a test can drive it. Bounded by @p timeout
    /// rather than parking indefinitely, which is the same rule the socket's own
    /// contract states: nothing else can wake a receive.
    /// @param timeout How long to wait for a datagram.
    /// @return False when the socket has closed and the loop should end.
    bool Step(std::chrono::milliseconds timeout);

    /// Where a peer's challenge or proof reaches this node, as text for a log line.
    ///
    /// **Not the beacon port.** A node listens for beacons where every other node
    /// on the segment does and answers from an address of its own, because only
    /// one socket on a shared port is handed a unicast -- see
    /// `Net/SharedPortDatagram`. This is that own address, which is what the
    /// socket reports and what a peer replies to.
    ///
    /// The join lives above the socket for the reason `Cc::DialEndpoint` exists
    /// -- see `DatagramAddress`. Out of line so that reason does not make
    /// `Core/HostPort.hpp` a dependency of everything including this header.
    /// @return `host:port`, bracketed when the host is an IPv6 literal.
    [[nodiscard]] std::string BoundEndpoint() const;

    /// How many peers have proved they hold the cluster key.
    /// @return The count.
    [[nodiscard]] std::size_t AuthenticatedCount() const
    {
        return _directory.AuthenticatedPeers().size();
    }

  private:
    DiscoveryTier(std::unique_ptr<IDatagramSocket> socket,
                  Cluster::DiscoveryConfig config,
                  PeerObserver onPeers,
                  ILogger& logger);

    /// Hand the authenticated set to the observer.
    void PublishAuthenticated();

    ILogger& _logger;
    PeerObserver _onPeers;

    // Declaration order IS construction order, and each is referenced by the one
    // below it -- the reference chain the other tiers own for the same reason.
    std::unique_ptr<IDatagramSocket> _socket;
    SteadyClock _clock;
    std::unique_ptr<IRandomSource> _random;
    Cluster::PeerDirectory _directory;
    std::chrono::seconds _beaconInterval;
    TimePoint _nextBeacon;
    Cluster::DiscoveryService _service;

    /// Started last and joined first, which the member order gives for free.
    std::jthread _thread;
};

/// Start discovery when the operator configured it, wiring it to consensus.
///
/// A function rather than a block in `WorkerBody`, for the reason
/// `StartConsensusOrExplain` is one: it is a coherent decision with one answer, and
/// `main.cpp` is in no test target.
///
/// A null result is success and means no `--discovery` was given, which is the
/// ordinary deployment: a cluster whose members an operator typed. Discovery is what
/// makes a *changing* fleet possible, not what makes a fleet possible.
/// @param cfg The parsed configuration.
/// @param consensus The running consensus tier; null when none was configured.
/// @param logger Where progress and refusals are reported.
/// @return The tier, a null tier meaning "not configured", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<DiscoveryTier>, std::string> StartDiscoveryOrExplain(
    NodeConfig const& cfg, std::unique_ptr<ConsensusTier> const& consensus, ILogger& logger);

} // namespace FastCache::Node
