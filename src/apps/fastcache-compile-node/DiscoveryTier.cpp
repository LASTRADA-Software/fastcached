// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/SharedPortDatagram.hpp>

#include <cstdio>
#include <format>
#include <optional>
#include <utility>

namespace FastCache::Node
{

DiscoveryTier::DiscoveryTier(std::unique_ptr<IDatagramSocket> socket,
                             Cluster::DiscoveryConfig config,
                             Consensus::IRaftPeerKeys const& keys,
                             PeerObserver onPeers,
                             IMetricsSink& metrics,
                             ILogger& logger):
    _logger { logger },
    _onPeers { std::move(onPeers) },
    _keys { keys },
    _socket { std::move(socket) },
    _random { std::make_unique<SystemSecureRandom>() },
    _directory { _clock, config.clusterId, config.nodeId },
    _beaconInterval { config.beaconInterval },
    // Due immediately rather than one interval from now: a node that waited would be
    // invisible to a segment that is already up for as long as its own interval, and
    // the first beacon is the cheapest thing it will ever send.
    _nextBeacon { _clock.Now() },
    _service { *_socket, _clock, *_random, _directory, std::move(config), keys, metrics, logger }
{
}

std::unique_ptr<DiscoveryTier> DiscoveryTier::Over(std::unique_ptr<IDatagramSocket> socket,
                                                   Cluster::DiscoveryConfig config,
                                                   Consensus::IRaftPeerKeys const& keys,
                                                   PeerObserver onPeers,
                                                   IMetricsSink& metrics,
                                                   ILogger& logger)
{
    return std::unique_ptr<DiscoveryTier> { new DiscoveryTier {
        std::move(socket), std::move(config), keys, std::move(onPeers), metrics, logger } };
}

std::expected<std::unique_ptr<DiscoveryTier>, std::string> DiscoveryTier::Start(NodeConfig const& cfg,
                                                                                std::string_view raftEndpoint,
                                                                                Consensus::IRaftPeerKeys const& keys,
                                                                                PeerObserver onPeers,
                                                                                IMetricsSink& metrics,
                                                                                ILogger& logger)
{
    // The ANNOUNCE address -- where beacons are sent. Only its host is read here; the
    // port and everything bound come from the row below, so the two cannot disagree.
    // Through `ParseDialEndpoint` because that is exactly what this is: an address
    // this node will send to, which must name a host and may not be a bare port.
    auto const announce = ParseDialEndpoint(cfg.discoveryAddress);
    if (!announce.has_value())
        return std::unexpected { std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress) };

    // Two sockets, and which does what is the whole of issue #126.
    //
    // A node LISTENS on the beacon port, on the wildcard, shared -- a beacon is a
    // broadcast, so every node has to be where the others shout, and an ephemeral
    // listening port would send perfectly and hear nothing. It ANSWERS somewhere
    // only it holds, because sharing a port buys hearing a broadcast and nothing
    // else: only one of the sockets sharing a port is handed a unicast, and both
    // the challenge and the proof are unicast to wherever the last datagram came
    // from. A node that answered on the shared port would be answering for its
    // machine, which is why two nodes on one host never finished proving the key.
    //
    // Which socket takes which option is `OpenSharedPortUdpSocket`'s to know, not
    // this function's: there are four of them, each easy to put on the wrong half,
    // and every wrong pairing still starts and still passes a test suite.
    // BOTH sockets come from this surface's row, not from a literal spelled here three
    // times and a reply port passed around it. The row resolves to exactly the two
    // endpoints this opens -- beacon first, reply second when one is pinned -- so the
    // addresses bound here and the ones `--print-surfaces` prints are one computation.
    // Taking only the host from the row and re-deriving the ports would have left the
    // resolver dead code for its only production consumer, on the surface with two
    // endpoints, the only UDP one, and the one operators get wrong most.
    //
    // `--discovery`'s host half is where beacons are SENT; both sockets bind the
    // wildcard whatever it says, and the resolver is where that is enforced.
    auto const endpoints = RowFor(NodeSurface::Discovery).Resolve(cfg);
    if (endpoints.empty())
        return std::unexpected { std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress) };

    auto const& beaconSocket = endpoints.front();
    auto const& bindHost = beaconSocket.host;
    auto const replyPort = endpoints.size() > 1 ? endpoints[1].port : std::uint16_t { 0 };

    auto socket = OpenSharedPortUdpSocket(bindHost, beaconSocket.port, replyPort);
    if (socket == nullptr)
    {
        // Through the row (#352). Both ports are named because either can be the one
        // that failed and this cannot tell which -- a message blaming the beacon port
        // alone sends an operator to look at a port that bound perfectly.
        auto judged =
            JudgeBindFailure(RowFor(NodeSurface::Discovery),
                             std::format("cannot bind the UDP sockets discovery needs: {} to listen on, and {} to answer on",
                                         FormatHostPort(bindHost, beaconSocket.port),
                                         cfg.discoveryReplyPort != 0 ? FormatHostPort(bindHost, cfg.discoveryReplyPort)
                                                                     : std::string { "a port of this node's own" }),
                             logger);
        if (!judged.has_value())
            return std::unexpected { std::move(judged).error() };

        // Refused rather than tolerated (#352). `main.cpp` does handle a null discovery
        // tier -- it is how "no --discovery" is spelled -- so this is the one opener
        // whose caller would survive it. It still refuses, because the two mean opposite
        // things: one is an operator who asked for nothing, the other an operator who
        // asked and silently did not get it, and only the row's reason distinguishes them.
        return std::unexpected { BindToleranceUnsupported(RowFor(NodeSurface::Discovery),
                                                          "a null tier already means \"--discovery was not asked for\"") };
    }

    // The port the operator configured, which is NOT the one the pair reports:
    // that is where this node is answered. Both go in the startup line.
    auto const listeningOn = FormatHostPort(bindHost, beaconSocket.port);

    auto config =
        Cluster::DiscoveryConfig { .clusterId = cfg.clusterId,
                                   .nodeId = cfg.nodeId,
                                   .raftEndpoint = std::string { raftEndpoint },
                                   .beaconAddress = DatagramAddress { .host = announce->first, .port = beaconSocket.port },
                                   .beaconInterval = Cluster::DiscoveryConfig {}.beaconInterval,
                                   .challengeLifetime = Cluster::DiscoveryConfig {}.challengeLifetime };

    auto tier = Over(std::move(socket), std::move(config), keys, std::move(onPeers), metrics, logger);

    tier->_thread = std::jthread { [tier = tier.get()](std::stop_token const& stop) {
        while (!stop.stop_requested() && tier->Step(PollTimeout))
            ;
    } };

    // Both addresses, because they answer different operator questions: the first
    // is the port that was configured and the one beacons are shouted to, and the
    // second is where this node's peers unicast their challenges and proofs --
    // which is what a host firewall has to let in, and is not the beacon port.
    logger.Logf(LogLevel::Info,
                "discovery listening on {}, answering from {}, for cluster {}, announcing {}",
                listeningOn,
                tier->BoundEndpoint(),
                cfg.clusterId,
                raftEndpoint);
    return tier;
}

std::string DiscoveryTier::BoundEndpoint() const
{
    auto const bound = _socket->BoundAddress();
    // An empty host is what `BoundAddress` reports for a socket it could not name,
    // and joining that yields `:0` -- which reads as an endpoint rather than as
    // the absence of one, in the log line an operator checks to see where
    // discovery came up.
    if (bound.host.empty())
        return {};
    return FormatHostPort(bound.host, bound.port);
}

DiscoveryTier::~DiscoveryTier()
{
    // The socket first, so a parked receive has a reason to return at its next poll,
    // and only then the stop -- which the loop observes on that same return. The
    // `jthread` joins in its own destructor after this, which the member order buys.
    _socket->Close();
    _thread.request_stop();
}

bool DiscoveryTier::Step(std::chrono::milliseconds timeout)
{
    if (_clock.Now() >= _nextBeacon)
    {
        if (!_service.SendBeacon())
            // Logged and carried on. A datagram the local stack refused is a
            // transient -- an interface coming up, a route that is not there yet --
            // and beacons repeat by design, so stopping here would turn a recoverable
            // moment into a node that never announces again.
            _logger.Log(LogLevel::Warn, "discovery: a beacon could not be sent");
        _nextBeacon = _clock.Now() + _beaconInterval;
    }

    auto const event = _service.PumpOnce(timeout);
    if (event == Cluster::DiscoveryEvent::Closed)
        return false;
    if (event == Cluster::DiscoveryEvent::PeerAuthenticated)
        PublishAuthenticated();

    // Expiry is driven from here rather than from a timer of its own, because it has
    // nothing to do that a pump does not already provoke: a peer stops being known
    // because its beacons stopped, and this loop is what would have seen them.
    _service.Maintain();
    return true;
}

void DiscoveryTier::PublishAuthenticated()
{
    auto members = std::vector<Cluster::DesiredMember> {};
    for (auto const& peer: _directory.AuthenticatedPeers())
    {
        // **Re-asked of the roster NOW, not taken from the moment of the proof** (#178). The
        // directory records that a key was proved; the roster may since have revoked it or
        // re-admitted the id under another, and this set is re-published whenever ANY peer
        // proves itself -- so a proof taken an hour ago would otherwise go on being handed to
        // `Desire` as if it were current. A peer whose proven key is no longer the one the
        // roster holds is left out until it proves the one it does.
        if (peer.provenKey != _keys.KeysOf(peer.nodeId).live)
            continue;

        // No scheduler endpoint, and `nullopt` rather than an empty string is what
        // says so. Discovery learns where a peer answers CONSENSUS, because that is
        // what the proof covered; the port clients speak to is one nobody dials, so
        // only that node can announce it. An empty string here would read as "I know
        // it has none" and clear whatever the peer had announced about itself.
        //
        // And no seat, because a desire carries none (#1535). The proof says this peer
        // holds the key, which is no reason to count it: a peer the cluster has placed
        // nowhere is recorded as a LEARNER and an operator promotes it, while one it
        // already records or counts keeps that seat. Decided by the leader against the
        // state it holds at every pass, which this tier cannot see.
        //
        // And no KEY, although the peer just proved one (#178). Discovery authenticates only the
        // key the roster already holds, so it has nothing to add -- and a desire OUTLIVES the
        // moment it was stated (`ConsensusTier::Desire` replaces per id and never prunes), so a
        // key stated here would still be desired after an operator re-admitted that id under
        // another key, and the reconciler would propose the old one straight back over the
        // operator's decision. No opinion keeps whatever is recorded, which is the reading that
        // cannot outlive its facts.
        members.push_back(Cluster::DesiredMember { .id = peer.nodeId,
                                                   .raftEndpoint = peer.raftEndpoint,
                                                   .schedulerEndpoint = std::nullopt,
                                                   .publicKey = std::nullopt });
    }

    if (_onPeers && !members.empty())
        _onPeers(members);
}

std::expected<std::unique_ptr<DiscoveryTier>, std::string> StartDiscoveryOrExplain(
    NodeConfig const& cfg, std::unique_ptr<ConsensusTier> const& consensus, IMetricsSink& metrics, ILogger& logger)
{
    if (cfg.discoveryAddress.empty())
        return std::unique_ptr<DiscoveryTier> {};

    // Refused rather than ignored. `StartupPolicyRejection` already turns
    // `--discovery` without `--node-id` away before this runs, so this is the belt to
    // that braces -- but a null consensus tier here would mean discovering peers for
    // a cluster this node is not in, and proposing them nowhere.
    if (consensus == nullptr)
        return std::unexpected { std::string { "--discovery needs --node-id: there is no cluster to admit anybody to" } };

    // The consensus tier's own keys (#178): discovery proves this node with the key the Raft
    // peer wire proves it with, and classifies a peer's key against the roster that wire reads.
    return DiscoveryTier::Start(
        cfg,
        consensus->Self().raftEndpoint,
        consensus->Keys(),
        [&consensus](std::span<Cluster::DesiredMember const> peers) {
            // Desired, not proposed. Whether this node is the one that may act on it
            // is the reconciler's question, and asking it here would have every node
            // on the segment proposing the same change at once.
            consensus->Desire(peers);
        },
        metrics,
        logger);
}

} // namespace FastCache::Node
