// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/UdpSocket.hpp>

#include <cstdio>
#include <format>
#include <optional>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// The shortest key this node will accept.
    ///
    /// An HMAC key shorter than its own block is not wrong, but a key an operator
    /// can type in a hurry is one an attacker can guess -- and what a guessed key
    /// buys is admission to the fleet, which is object injection into everybody's
    /// build. Sixteen bytes is a low bar that any generated key clears without
    /// anybody thinking about it, and the refusal says how to produce one.
    constexpr std::size_t MinimumKeyBytes = 16;

    /// Whether a byte is trailing noise a key file did not mean to carry.
    /// @param byte The candidate.
    /// @return True when it may be trimmed.
    [[nodiscard]] constexpr bool IsTrailingNoise(std::byte byte) noexcept
    {
        auto const raw = static_cast<unsigned char>(byte);
        return raw == '\n' || raw == '\r' || raw == ' ' || raw == '\t';
    }
} // namespace

std::expected<std::vector<std::byte>, std::string> ReadClusterKey(std::filesystem::path const& path)
{
    auto error = std::error_code {};
    auto const size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected { std::format("cannot read {}: {}", path.string(), error.message()) };

    // A `unique_ptr` over the handle for the reason every other `fopen` in this tree
    // has one: there are failure paths below, and a bare `fclose` at each is the one
    // that eventually gets forgotten.
    auto file = std::unique_ptr<std::FILE, int (*)(std::FILE*)> { std::fopen(path.string().c_str(), "rb"), &std::fclose };
    if (file == nullptr)
        return std::unexpected { std::format("cannot open {}", path.string()) };

    auto key = std::vector<std::byte>(static_cast<std::size_t>(size));
    if (!key.empty() && std::fread(key.data(), 1, key.size(), file.get()) != key.size())
        return std::unexpected { std::format("cannot read {} in full", path.string()) };

    while (!key.empty() && IsTrailingNoise(key.back()))
        key.pop_back();

    if (key.size() < MinimumKeyBytes)
        return std::unexpected { std::format("{} holds {} byte(s) of key and at least {} are required. Generate one "
                                             "with `head -c 32 /dev/urandom | base64 > {}` and give every node in the "
                                             "cluster that same file",
                                             path.string(),
                                             key.size(),
                                             MinimumKeyBytes,
                                             path.string()) };

    return key;
}

DiscoveryTier::DiscoveryTier(std::unique_ptr<IDatagramSocket> socket,
                             Cluster::DiscoveryConfig config,
                             PeerObserver onPeers,
                             ILogger& logger):
    _logger { logger },
    _onPeers { std::move(onPeers) },
    _socket { std::move(socket) },
    _random { std::make_unique<SystemRandomSource>() },
    _directory { _clock, config.clusterId, config.nodeId },
    _beaconInterval { config.beaconInterval },
    // Due immediately rather than one interval from now: a node that waited would be
    // invisible to a segment that is already up for as long as its own interval, and
    // the first beacon is the cheapest thing it will ever send.
    _nextBeacon { _clock.Now() },
    _service { *_socket, _clock, *_random, _directory, std::move(config), logger }
{
}

std::unique_ptr<DiscoveryTier> DiscoveryTier::Over(std::unique_ptr<IDatagramSocket> socket,
                                                   Cluster::DiscoveryConfig config,
                                                   PeerObserver onPeers,
                                                   ILogger& logger)
{
    return std::unique_ptr<DiscoveryTier> { new DiscoveryTier {
        std::move(socket), std::move(config), std::move(onPeers), logger } };
}

std::expected<std::unique_ptr<DiscoveryTier>, std::string> DiscoveryTier::Start(NodeConfig const& cfg,
                                                                                std::string_view raftEndpoint,
                                                                                PeerObserver onPeers,
                                                                                ILogger& logger)
{
    auto const beacon = SplitHostPort(cfg.discoveryAddress);
    if (!beacon.has_value())
        return std::unexpected { std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress) };

    auto const port = ParseTcpPort(beacon->second);
    if (!port.has_value())
        return std::unexpected { std::format("--discovery={} names no usable port", cfg.discoveryAddress) };

    auto key = ReadClusterKey(cfg.clusterKeyFile);
    if (!key.has_value())
        return std::unexpected { key.error() };

    // Bound on the SAME port the beacons go to, and on the wildcard. A beacon is a
    // broadcast, so every node has to be listening where the others shout -- an
    // ephemeral local port would send perfectly and hear nothing, which presents as a
    // segment of nodes that each believe they are alone.
    auto socket = OpenUdpSocket("0.0.0.0", *port, BroadcastMode::On, PortSharing::Shared);
    if (socket == nullptr)
        return std::unexpected { std::format("cannot bind UDP 0.0.0.0:{} for discovery", *port) };

    auto config = Cluster::DiscoveryConfig { .clusterId = cfg.clusterId,
                                             .nodeId = cfg.nodeId,
                                             .raftEndpoint = std::string { raftEndpoint },
                                             .beaconAddress = DatagramAddress { .host = beacon->first, .port = *port },
                                             .presharedKey = *std::move(key),
                                             .beaconInterval = Cluster::DiscoveryConfig {}.beaconInterval,
                                             .challengeLifetime = Cluster::DiscoveryConfig {}.challengeLifetime };

    auto tier = Over(std::move(socket), std::move(config), std::move(onPeers), logger);

    tier->_thread = std::jthread { [tier = tier.get()](std::stop_token const& stop) {
        while (!stop.stop_requested() && tier->Step(PollTimeout))
            ;
    } };

    logger.Logf(
        LogLevel::Info, "discovery on {} for cluster {}, announcing {}", tier->BoundEndpoint(), cfg.clusterId, raftEndpoint);
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
        // No scheduler endpoint, and `nullopt` rather than an empty string is what
        // says so. Discovery learns where a peer answers CONSENSUS, because that is
        // what the proof covered; the port clients speak to is one nobody dials, so
        // only that node can announce it. An empty string here would read as "I know
        // it has none" and clear whatever the peer had announced about itself.
        members.push_back(Cluster::DesiredMember {
            .id = peer.nodeId, .raftEndpoint = peer.raftEndpoint, .schedulerEndpoint = std::nullopt });

    if (_onPeers && !members.empty())
        _onPeers(members);
}

std::expected<std::unique_ptr<DiscoveryTier>, std::string> StartDiscoveryOrExplain(
    NodeConfig const& cfg, std::unique_ptr<ConsensusTier> const& consensus, ILogger& logger)
{
    if (cfg.discoveryAddress.empty())
        return std::unique_ptr<DiscoveryTier> {};

    // Refused rather than ignored. `StartupPolicyRejection` already turns
    // `--discovery` without `--node-id` away before this runs, so this is the belt to
    // that braces -- but a null consensus tier here would mean discovering peers for
    // a cluster this node is not in, and proposing them nowhere.
    if (consensus == nullptr)
        return std::unexpected { std::string { "--discovery needs --node-id: there is no cluster to admit anybody to" } };

    return DiscoveryTier::Start(
        cfg,
        consensus->Self().raftEndpoint,
        [&consensus](std::span<Cluster::DesiredMember const> peers) {
            // Desired, not proposed. Whether this node is the one that may act on it
            // is the reconciler's question, and asking it here would have every node
            // on the segment proposing the same change at once.
            consensus->Desire(peers);
        },
        logger);
}

} // namespace FastCache::Node
