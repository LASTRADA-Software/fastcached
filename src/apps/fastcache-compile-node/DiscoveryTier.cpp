// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <cstdio>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <core/net/SharedPortDatagram.hpp>

namespace FastCache::Node
{

namespace
{
    /// Where a proven fleet goes on a node with nothing that acts on one beyond the watch.
    ///
    /// The formation controller is that something; until it is built, a solitary node sees
    /// another fleet, says nothing, and yields to nobody -- which is what it did before discovery
    /// proved other fleets at all.
    class NoFormation final: public Cluster::IFleetObserver
    {
      public:
        void OnFleetProven(Cluster::ProvenFleet const& /*fleet*/) override {}
    };

    /// What a node with no formation controller can say about another fleet: nothing proves a split,
    /// since the evidence is read from a formation record and the fleet's state, which the controller
    /// holds. So every established fleet seen is foreign, as it was before split healing existed.
    class NoSplitEvidence final: public Cluster::ISplitEvidenceSource
    {
      public:
        [[nodiscard]] Cluster::SplitReading ReadSplit(Cluster::ProvenFleetSummary const& /*seen*/) const override
        {
            return Cluster::SplitReading {};
        }
    };

    /// @param addresses Destinations.
    /// @return Them as `host:port`, joined by `, `, for a log line.
    [[nodiscard]] std::string Listed(std::span<core::net::DatagramAddress const> addresses)
    {
        auto text = std::string {};
        for (auto const& address: addresses)
        {
            if (!text.empty())
                text += ", ";
            text += FormatHostPort(address.host, address.port);
        }
        return text;
    }

    /// @param refused Refused destinations.
    /// @return Which links they are, as `host:port`, in order: what a change of refusal is measured by,
    ///         leaving out the errors, which may differ from one beat to the next on the same link.
    [[nodiscard]] std::vector<std::string> LinksOf(std::span<Cluster::BeaconRefusal const> refused)
    {
        auto links = std::vector<std::string> {};
        for (auto const& refusal: refused)
            links.push_back(FormatHostPort(refusal.destination.host, refusal.destination.port));
        return links;
    }
} // namespace

/// What a production tier owns that a case injects.
struct DiscoveryTier::Production
{
    core::platform::SteadyClock clock;
    SystemSecureRandom random;
    NoSplitEvidence evidence;
    NoFormation fleets;
    std::unique_ptr<IInterfaceAddressSource> interfaces = MakeSystemInterfaceAddresses();
};

std::expected<std::shared_ptr<Cluster::IBeaconDestinations const>, std::string> BeaconDestinationsFor(
    NodeConfig const& cfg,
    std::uint16_t port,
    IInterfaceAddressSource const& interfaces,
    core::platform::IClock& clock,
    ILogger& logger)
{
    if (!cfg.discoveryAddressExplicit)
        return std::make_shared<Cluster::DirectedBroadcastDestinations const>(interfaces, clock, port, logger);

    // Named: where beacons go is exactly its host, on the row's port. Through `ParseDialEndpoint`
    // because that is exactly what this is: an address this node will send to, which must name a
    // host and may not be a bare port.
    auto const named = ParseDialEndpoint(cfg.discoveryAddress);
    if (!named.has_value())
        return std::unexpected { std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress) };
    return std::make_shared<Cluster::FixedBeaconDestination const>(
        core::net::DatagramAddress { .host = named->first, .port = port });
}

CompileCacheWire::FleetSummary ConfiguredFleetSummary(NodeConfig const& cfg, std::string_view raftEndpoint)
{
    // The recorded mode says what a node announces (`NodeModeRow::announces`); a configuration no
    // record shaped announces what a node that has minted nothing yet is: solitary.
    auto const mode = cfg.formation.has_value() ? cfg.formation->mode : Cluster::NodeMode::Solitary;
    return CompileCacheWire::FleetSummary {
        .clusterId = cfg.clusterId,
        .state = Cluster::NodeModeRowFor(mode).announces,
        .createdAtUnixSeconds = cfg.formation.has_value() ? cfg.formation->createdAtUnixSeconds : 0,
        .leaderId = {},
        .leaderNodeEndpoint = {},
        .nodeId = cfg.nodeId,
        .raftEndpoint = std::string { raftEndpoint },
    };
}

CompileCacheWire::FleetSummary AnsweredFleetSummary(NodeConfig const& cfg)
{
    if (!RunsConsensus(cfg))
        return CompileCacheWire::FleetSummary {};
    return ConfiguredFleetSummary(cfg, ConsensusDialAddressOf(cfg).value_or(std::string {}));
}

DiscoveryTier::DiscoveryTier(Parts parts):
    _logger { parts.logger },
    _onPeers { std::move(parts.onPeers) },
    _keys { parts.keys },
    _socket { std::move(parts.socket) },
    _clock { parts.clock },
    _directory { _clock, parts.self, parts.keys },
    _watch { parts.self, parts.evidence, _clock, parts.conditions, parts.fleets, parts.config.beaconInterval, parts.pin },
    _beaconInterval { parts.config.beaconInterval },
    // Due immediately rather than one interval from now: a node that waited would be
    // invisible to a segment that is already up for as long as its own interval, and
    // the first beacon is the cheapest thing it will ever send.
    _nextBeacon { _clock.now() },
    _service { *_socket,   _clock, parts.random, _directory,    std::move(parts.config),
               parts.self, _watch, parts.keys,   parts.metrics, parts.logger }
{
}

std::unique_ptr<DiscoveryTier> DiscoveryTier::Over(Parts parts)
{
    return std::unique_ptr<DiscoveryTier> { new DiscoveryTier { std::move(parts) } };
}

std::expected<std::unique_ptr<DiscoveryTier>, NodeRefusal> DiscoveryTier::Start(NodeConfig const& cfg,
                                                                                Cluster::IFleetSummarySource const& self,
                                                                                Consensus::IRaftPeerKeys const& keys,
                                                                                NodeConditions& conditions,
                                                                                PeerObserver onPeers,
                                                                                IMetricsSink& metrics,
                                                                                ILogger& logger,
                                                                                DiscoveryFormation formation)
{
    // The ANNOUNCE address -- where beacons are sent. Only its host is read here; the
    // port and everything bound come from the row below, so the two cannot disagree.
    // Through `ParseDialEndpoint` because that is exactly what this is: an address
    // this node will send to, which must name a host and may not be a bare port.
    auto const announce = ParseDialEndpoint(cfg.discoveryAddress);
    if (!announce.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule,
                                         std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress)) };

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
    // Which socket takes which option is `core::net::openSharedPortUdpSocket`'s to know, not
    // this function's: there are four of them, each easy to put on the wrong half,
    // and every wrong pairing still starts and still passes a test suite.
    // BOTH sockets come from this surface's row, not from a literal spelled here three
    // times and a reply port passed around it. The row resolves to exactly the two
    // endpoints this opens -- beacon first, reply second, whose port is 0 when its kind
    // says the kernel chooses it, which is also how the opener asks the kernel for one --
    // so the addresses bound here and the ones `--print-surfaces` prints are one computation.
    // Taking only the host from the row and re-deriving the ports would have left the
    // resolver dead code for its only production consumer, on the surface with two
    // endpoints, the only UDP one, and the one operators get wrong most.
    //
    // `--discovery`'s host half is where beacons are SENT; both sockets bind the
    // wildcard whatever it says, and the resolver is where that is enforced.
    auto const endpoints = RowFor(NodeSurface::Discovery).Resolve(cfg);
    if (endpoints.empty())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule,
                                         std::format("--discovery={} is not <address>:<port>", cfg.discoveryAddress)) };

    // Beacon first, reply second, always: the row resolves the reply socket whether or not
    // a port is pinned, and says which by its port kind. Anything else is this binary's defect.
    if (endpoints.size() != 2)
        // A defect in this build's own table, which the next start resolves the same way: refused,
        // as every build defect is, since a supervisor's restart would only meet it again.
        return std::unexpected { Refusal(
            NodeRefusalCause::BuildDefect,
            std::format("the discovery surface resolved {} endpoint(s) where it always resolves a beacon and a reply "
                        "socket; this is a defect in this binary rather than in the configuration",
                        endpoints.size())) };
    auto const& beaconSocket = endpoints.front();
    auto const& replySocket = endpoints.back();
    auto const& bindHost = beaconSocket.host;

    auto opened = core::net::openSharedPortUdpSocket(bindHost, beaconSocket.port, replySocket.port);
    if (!opened.has_value())
    {
        // Through the row (#352), naming both sockets (`DiscoveryBindFailure`).
        auto judged = JudgeBindFailure(RowFor(NodeSurface::Discovery),
                                       DiscoveryBindFailure(beaconSocket, replySocket, opened.error().toString()),
                                       logger);
        if (!judged.has_value())
            return std::unexpected { Refusal(NodeRefusalCause::Listener, std::move(judged).error()) };

        // Refused rather than tolerated (#352). `main.cpp` does handle a null discovery
        // tier -- it is how "no --discovery" is spelled -- so this is the one opener
        // whose caller would survive it. It still refuses, because the two mean opposite
        // things: one is an operator who asked for nothing, the other an operator who
        // asked and silently did not get it, and only the row's reason distinguishes them.
        return std::unexpected { Refusal(
            NodeRefusalCause::Listener,
            BindToleranceUnsupported(RowFor(NodeSurface::Discovery),
                                     "a null tier already means \"--discovery was not asked for\"")) };
    }

    auto socket = std::move(*opened);

    // The port the operator configured, which is NOT the one the pair reports:
    // that is where this node is answered. Both go in the startup line.
    auto const listeningOn = FormatHostPort(bindHost, beaconSocket.port);

    // The production parts are the tier's to keep: nothing else outlives it -- the interface
    // enumerator and the clock the default destinations re-enumerate by included.
    auto const announcing = self.Current().raftEndpoint;
    auto production = std::make_unique<Production>();

    auto destinations = BeaconDestinationsFor(cfg, beaconSocket.port, *production->interfaces, production->clock, logger);
    if (!destinations.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, std::move(destinations).error()) };
    auto config = Cluster::DiscoveryConfig { .beaconDestinations = *std::move(destinations),
                                             .beaconInterval = Cluster::DiscoveryConfig {}.beaconInterval,
                                             .challengeLifetime = Cluster::DiscoveryConfig {}.challengeLifetime };
    auto tier = Over(Parts { .socket = std::move(socket),
                             .clock = production->clock,
                             .random = production->random,
                             .config = std::move(config),
                             .keys = keys,
                             .self = self,
                             .conditions = conditions,
                             .evidence = formation.evidence != nullptr ? *formation.evidence : production->evidence,
                             .fleets = formation.fleets != nullptr ? *formation.fleets : production->fleets,
                             .onPeers = std::move(onPeers),
                             .metrics = metrics,
                             .logger = logger,
                             .pin = FleetPinOf(cfg) });
    tier->_production = std::move(production);

    tier->_thread = std::jthread { [tier = tier.get()](std::stop_token const& stop) {
        while (!stop.stop_requested() && tier->Step(PollTimeout))
            ;
    } };

    // Both addresses, because they answer different operator questions: the first
    // is the port that was configured and the one beacons are shouted to, and the
    // second is where this node's peers unicast their challenges and proofs --
    // which is what a host firewall has to let in, and is not the beacon port.
    logger.Logf(LogLevel::Info,
                "discovery listening on {}, answering from {}, for cluster {}, announcing {}, beaconing to {}",
                listeningOn,
                tier->BoundEndpoint(),
                cfg.clusterId,
                announcing,
                cfg.discoveryAddressExplicit ? FormatHostPort(announce->first, beaconSocket.port)
                                             : std::string { "every up interface's directed broadcast" });
    return tier;
}

std::string DiscoveryTier::BoundEndpoint() const
{
    auto const bound = _socket->boundAddress();
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
    _socket->close();
    _thread.request_stop();
}

bool DiscoveryTier::Step(std::chrono::milliseconds timeout)
{
    if (_clock.now() >= _nextBeacon)
    {
        // Only a REFUSAL is this loop's to say. No destination and a withheld summary are
        // said once per change by whoever knows them, and repeating either here at every beat
        // named a refusal that never happened -- about 5760 lines a day on a machine whose only
        // link is a point-to-point VPN. Every outcome is named and there is no `default`, so an
        // outcome added later is a build failure here rather than a silent "not a refusal".
        auto const report = _service.SendBeacon();
        switch (report.outcome)
        {
            case Cluster::BeaconSendOutcome::AllRefused:
                ReportRefusedBeacon(report.refused);
                break;
            case Cluster::BeaconSendOutcome::PartlyRefused:
                ReportPartlyRefusedBeacon(report);
                break;
            case Cluster::BeaconSendOutcome::Sent:
                ReportBeaconAcceptedEverywhere(report);
                break;
            case Cluster::BeaconSendOutcome::NoDestination:
            case Cluster::BeaconSendOutcome::Withheld:
                break;
        }
        _nextBeacon = _clock.now() + _beaconInterval;
    }

    auto const event = _service.PumpOnce(timeout);
    if (event == Cluster::DiscoveryEvent::Closed)
        return false;
    if (event == Cluster::DiscoveryEvent::PeerAuthenticated)
        PublishAuthenticated();

    // Expiry is driven from here rather than from a timer of its own, because it has
    // nothing to do that a pump does not already provoke: a peer stops being known
    // because its beacons stopped, and this loop is what would have seen them. The
    // same for a foreign fleet: its proofs stop arriving here.
    _service.Maintain();
    _watch.Tick();
    return true;
}

void DiscoveryTier::ReportRefusedBeacon(std::span<Cluster::BeaconRefusal const> refused)
{
    // Logged and carried on. A datagram the local stack refused is a transient -- an interface
    // coming up, a route that is not there yet -- and beacons repeat by design, so stopping here
    // would turn a recoverable moment into a node that never announces again. Throttled, because
    // a refusal that is not transient repeats at every beat; the count says how many a line
    // stands for. Not the partial refusal's set of links: a link that flaps between refusing
    // everywhere and refusing on one link would otherwise be a change of that set at every beat.
    auto const now = _clock.now();
    _lastRefusal = now;
    auto const count = _refusedEverywhere.Note(now);
    if (!count.has_value())
        return;
    // What the stack answered, per destination, is the cause: EACCES from a firewall and
    // ENETUNREACH from a route that is not there yet are different remedies.
    _logger.Logf(LogLevel::Warn,
                 "discovery: the local stack refused this node's beacon at every destination ({} refused since "
                 "the last such line; said at most every {}s): {}",
                 *count,
                 RefusedBeaconReportInterval.count(),
                 Cluster::DescribeRefusals(refused));
}

void DiscoveryTier::ReportPartlyRefusedBeacon(Cluster::BeaconSendReport const& report)
{
    // The machine this whole default exists for: the LAN refuses while a VPN adapter takes it,
    // and "beaconing to <both>" is all an operator would otherwise see. Counted once per change of
    // WHICH links refuse -- a refusal that is not transient repeats at every beat -- and said at
    // most once an interval, since links that keep changing would otherwise be a line a beat. A
    // change counted inside the interval is said at the first partly refused beat after it.
    auto const now = _clock.now();
    _lastRefusal = now;
    if (auto links = LinksOf(report.refused); links != _refusedLinks)
    {
        _refusedLinks = std::move(links);
        _refusedLinkChanges.Count();
    }
    auto const changes = _refusedLinkChanges.Due(now);
    if (!changes.has_value())
        return;
    _logger.Logf(LogLevel::Warn,
                 "discovery: the local stack refused this node's beacon at {}, while {} took it: no peer on a "
                 "refused link hears this node ({} change(s) of which links refuse since the last such line; said "
                 "at most every {}s)",
                 Cluster::DescribeRefusals(report.refused),
                 Listed(report.accepted),
                 *changes,
                 RefusedBeaconReportInterval.count());
}

void DiscoveryTier::ReportBeaconAcceptedEverywhere(Cluster::BeaconSendReport const& report)
{
    // The end of a refusal, said once -- and only after a whole interval with none, so a link that
    // refuses every other beat never reaches it: said at the first clean beat, each recovery would
    // re-arm the next refusal as a change, a Warn and an Info every two beats. Nothing at all while
    // nothing was refused.
    auto const now = _clock.now();
    if (!_lastRefusal.has_value() || now - *_lastRefusal < RefusedBeaconReportInterval)
        return;
    _lastRefusal.reset();
    _refusedLinks.clear();
    _refusedLinkChanges.Discard();
    _refusedEverywhere.Discard();
    _logger.Logf(LogLevel::Info,
                 "discovery: every destination takes this node's beacon again, with no refusal for {}s: {}",
                 RefusedBeaconReportInterval.count(),
                 Listed(report.accepted));
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

        // Nowhere to dial: a learner answers no consensus, it dials in. Desiring it with an
        // empty endpoint would ask the leader to record a member at no address, and the
        // learner's own record already carries where it is -- nothing, which is right.
        if (peer.raftEndpoint.empty())
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

std::string DiscoveryBindFailure(SurfaceEndpoint const& beacon, SurfaceEndpoint const& reply, std::string_view why)
{
    auto const& replyKind = SurfacePortKindRowOf(reply.portKind);
    return std::format("cannot bind the UDP sockets discovery needs: {} to listen on, and {} to answer on{} ({})",
                       FormatHostPort(beacon.host, beacon.port),
                       FormatHostPort(reply.host, replyKind.portText(reply.port)),
                       replyKind.trailer,
                       why);
}

std::expected<std::unique_ptr<DiscoveryTier>, NodeRefusal> StartDiscoveryOrExplain(
    NodeConfig const& cfg,
    std::unique_ptr<ConsensusTier> const& consensus,
    Cluster::IFleetSummarySource const& self,
    NodeConditions& conditions,
    IMetricsSink& metrics,
    ILogger& logger,
    DiscoveryFormation formation)
{
    // A node that runs consensus and no discovery can see no other fleet, which is an answer
    // about the row rather than a clear one: said, with why, so it never reads `undecided`.
    // A node with no consensus has the row answered by its scope (`NodeConditions::Settle`).
    auto const standDown = [&](std::string_view why) {
        if (consensus != nullptr)
            for (auto const condition: ForeignFleetWatch::WatchedConditions)
                conditions.NotEvaluated(condition, why);
        return std::unique_ptr<DiscoveryTier> {};
    };

    if (cfg.discoveryAddress.empty())
        return standDown("discovery is off (--discovery=), so no other fleet can be seen");

    // Discovery runs beside consensus only. A DEFAULTED address on a node that runs none is
    // the ordinary worker -- `--listen-raft=` turns consensus off and discovery with it -- so
    // nothing starts. A TYPED one is refused rather than ignored: `StartupPolicyRejection`
    // already turns it away before this runs, so this is the belt to that braces, and a null
    // consensus tier here would mean discovering peers for a cluster this node is not in. In the
    // table's words.
    if (consensus == nullptr)
    {
        if (!cfg.discoveryAddressExplicit)
            return std::unique_ptr<DiscoveryTier> {};
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, std::string { DiscoveryNeedsConsensusRefusal }) };
    }

    // **Never announced: an endpoint every peer resolves to itself** (`AnnouncesOnlyThisMachine`).
    // A consensus port bound to loopback is a cluster that never leaves this machine -- the
    // startup rules accept it and refuse the contradictions around it -- so a DEFAULTED
    // discovery stands down on it rather than beacon every peer to itself, and a TYPED one is
    // refused: the operator asked for something this node must not do. The service holds the
    // same rule at its only door; this is the policy, that is the guarantee.
    //
    // Asked of the ONE summary this node answers with -- the same object its FLEET-SUMMARY
    // responder signs -- so a beacon and an answer can never describe this node differently.
    auto const announced = self.Current();
    if (Cluster::AnnouncesOnlyThisMachine(announced))
    {
        if (cfg.discoveryAddressExplicit)
            return std::unexpected { Refusal(NodeRefusalCause::EarlierRule,
                                             std::string { DiscoveryAnnouncesOnlyThisMachineRefusal }) };
        logger.Logf(LogLevel::Info,
                    "discovery stands down: this node's consensus address {} reaches only the machine that dials "
                    "it, so a beacon would send every peer to itself",
                    announced.raftEndpoint);
        return standDown(std::format("discovery stands down: this node's consensus address {} reaches only this "
                                     "machine, so it announces nothing and sees no other fleet",
                                     announced.raftEndpoint));
    }

    // The consensus tier's own keys (#178): discovery proves this node with the key the Raft
    // peer wire proves it with, and classifies a peer's key against the roster that wire reads.
    return DiscoveryTier::Start(
        cfg,
        self,
        consensus->Keys(),
        conditions,
        [&consensus](std::span<Cluster::DesiredMember const> peers) {
            // Desired, not proposed. Whether this node is the one that may act on it
            // is the reconciler's question, and asking it here would have every node
            // on the segment proposing the same change at once.
            consensus->Desire(peers);
        },
        metrics,
        logger,
        formation);
}

} // namespace FastCache::Node
