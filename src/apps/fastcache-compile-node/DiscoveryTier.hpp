// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ConsensusTier.hpp"
#include "ForeignFleetWatch.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeRefusal.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/ThrottledReport.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <core/net/IDatagramSocket.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// What this node says about itself, from its configuration and formation record: its cluster,
/// id and consensus endpoint, and the state its recorded mode announces
/// (`Cluster::NodeModeRow::announces`).
///
/// What discovery announces until the formation controller exists to say it; no leader is named,
/// because the configuration does not know one.
/// @param cfg The configuration, its formation applied.
/// @param raftEndpoint Where peers dial this node's consensus; empty for a learner.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary ConfiguredFleetSummary(NodeConfig const& cfg, std::string_view raftEndpoint);

/// Where this node's beacons go: exactly the operator's `--discovery` when they NAMED one, and every
/// up interface's directed broadcast when they did not (`Cluster::DirectedBroadcastDestinations`).
///
/// Decided by PROVENANCE (`NodeConfig::discoveryAddressExplicit`), never by comparing the value to
/// the default: an operator who types the default's own spelling, `255.255.255.255:6681`, asked for
/// the limited broadcast and gets it -- and a default that happened to be typed out in a config
/// file is still one the operator named.
/// @param cfg The parsed configuration.
/// @param port The beacon port every destination carries: the discovery row's.
/// @param interfaces This machine's interfaces, for the default; must outlive the answer.
/// @param clock When the default re-enumerates; must outlive the answer.
/// @param logger Where the default says where it beacons; must outlive the answer.
/// @return The destinations, or why a named `--discovery` is not `<address>:<port>`.
[[nodiscard]] std::expected<std::shared_ptr<Cluster::IBeaconDestinations const>, std::string> BeaconDestinationsFor(
    NodeConfig const& cfg,
    std::uint16_t port,
    IInterfaceAddressSource const& interfaces,
    core::platform::IClock& clock,
    ILogger& logger);

/// What this node answers `FleetSummary` with before its consensus tier exists: the configured
/// summary at the address peers dial (`ConsensusDialAddressOf`, the derivation the tier runs
/// under), or a summary naming NO cluster on a node that runs no consensus -- which the responder
/// refuses rather than signs, so a pure worker never vouches for a cluster it does not run.
/// @param cfg The configuration, its formation applied.
/// @return The summary.
[[nodiscard]] CompileCacheWire::FleetSummary AnsweredFleetSummary(NodeConfig const& cfg);

/// What discovery hands a node's formation, and asks it: where every proven fleet goes after the
/// tier's watch, and whether a proven fleet is this one split.
///
/// Both null for a node that runs no formation controller -- every proven fleet is then dropped
/// after the watch, and no split is ever evidenced -- which is what a tier a case builds without one
/// gets too.
struct DiscoveryFormation
{
    Cluster::ISplitEvidenceSource const* evidence { nullptr }; ///< Whether a proven fleet is this one split.
    Cluster::IFleetObserver* fleets { nullptr };               ///< Where every proven fleet goes after the watch.
};

/// A summary that does not change: what `ConfiguredFleetSummary` computed at startup.
///
/// Enough while a node's cluster, id and mode change only across a reform, which rebuilds the
/// discovery tier and this with it.
class FixedFleetSummary final: public Cluster::IFleetSummarySource
{
  public:
    /// @param summary What this node says about itself.
    explicit FixedFleetSummary(CompileCacheWire::FleetSummary summary):
        _summary { std::move(summary) }
    {
    }

    [[nodiscard]] CompileCacheWire::FleetSummary Current() const override
    {
        return _summary;
    }

  private:
    CompileCacheWire::FleetSummary _summary;
};

/// LAN discovery, running.
///
/// The loop that turns `Cluster::DiscoveryService` -- which is synchronous, and
/// pure enough that a whole segment forming a cluster is a unit test -- into a
/// thread that announces this node and listens for its peers. Everything it decides
/// lives below it; what it adds is a socket, an interval and somewhere to put the
/// answer.
///
/// **It proposes nothing.** Discovery answers "who has proved a key the roster holds for
/// them, and where do they answer"; admitting a node is a Raft decision only a leader may
/// make, and a discovery layer that proposed directly would have every node on the
/// segment proposing the same change at once. So the authenticated set goes to
/// `ConsensusTier::Desire`, and the reconciler there decides whether this node is
/// the one that may act on it.
///
/// **And the set holds no stranger** (#178): a machine whose key the roster does not hold is
/// reported by `DiscoveryService` and never reaches `Desire`, so discovery can move a known
/// member to a new address but can no longer add anybody.
///
/// Another fleet's proven summary goes through the tier's `ForeignFleetWatch` -- which raises
/// `foreign-fleet-visible` while two established fleets see each other -- and on to the fleet
/// observer it was given, never to `Desire`.
class DiscoveryTier
{
  public:
    /// How long a receive parks before the loop re-checks its stop flag.
    ///
    /// The same rule as every other loop here: POSIX does not unblock a parked
    /// receive when another thread closes the socket, so a poll timeout is the only
    /// portable way a stop is ever observed.
    static constexpr std::chrono::milliseconds PollTimeout { 250 };

    /// Told which peers have proved the key the roster holds for them.
    using PeerObserver = std::function<void(std::span<Cluster::DesiredMember const>)>;

    /// Everything a tier is built over; every reference must outlive the tier.
    struct Parts
    {
        std::unique_ptr<core::net::IDatagramSocket> socket; ///< Where datagrams come from and go; owned.
        core::platform::IClock& clock;                      ///< Beacon timing, expiry, and the watch's memory.
        ISecureRandom& random;                              ///< Where challenge nonces come from.
        Cluster::DiscoveryConfig config;                    ///< Where to beacon, and how often.
        Consensus::IRaftPeerKeys const& keys;               ///< This node's key and the roster's.
        Cluster::IFleetSummarySource const& self;           ///< What this node says about itself.
        NodeConditions& conditions;                         ///< Where the watch's rows are answered.
        Cluster::ISplitEvidenceSource const& evidence;      ///< Whether a proven fleet is this one split.
        Cluster::IFleetObserver& fleets;                    ///< Where every proven fleet goes after the watch.
        PeerObserver onPeers;                               ///< Told the authenticated set.
        IMetricsSink& metrics;                              ///< Where refusals and bounds are counted.
        ILogger& logger;                                    ///< Where beacons, joins and rejections are reported.
    };

    /// How often a beacon the local stack refused at every destination is said, at most, and a
    /// change of which links refuse one; and how long no destination may refuse it before that is
    /// said. Each refusal line carries how many occurrences it stands for.
    static constexpr std::chrono::seconds RefusedBeaconReportInterval { 60 };

    /// Start discovery over a real socket, the production clock and randomness, or explain why it
    /// cannot run.
    /// @param cfg The parsed configuration.
    /// @param self What this node announces -- the one summary the node's FLEET-SUMMARY responder
    ///        signs too; must outlive the tier.
    /// @param keys This node's key and the roster's; must outlive the tier.
    /// @param conditions Where `foreign-fleet-visible` is answered; must outlive the tier.
    /// @param onPeers Told the authenticated set; must outlive the tier.
    /// @param metrics Where proofs under keys the roster does not accept are counted.
    /// @param logger Where beacons, joins and rejections are reported.
    /// @param formation Where proven fleets go, and what says whether one is this fleet split; both
    ///        must outlive the tier. Stated by every caller, never defaulted: a defaulted collaborator
    ///        is how a new production caller would drop the formation without a word.
    /// @return The running tier, or the fatal reason.
    [[nodiscard]] static std::expected<std::unique_ptr<DiscoveryTier>, NodeRefusal> Start(
        NodeConfig const& cfg,
        Cluster::IFleetSummarySource const& self,
        Consensus::IRaftPeerKeys const& keys,
        NodeConditions& conditions,
        PeerObserver onPeers,
        IMetricsSink& metrics,
        ILogger& logger,
        DiscoveryFormation formation);

    /// Build a tier over a socket somebody else chose, without starting its thread.
    ///
    /// The injection seam, and it exists because the alternative is untestable: the
    /// conditions this loop is for -- two nodes finding each other, one of them
    /// holding a key the other's roster does not know -- need a segment, and
    /// `Net/InMemoryDatagram` is the segment this repository already has. A caller that takes this door drives
    /// `Step` itself, so a whole cluster forming is a scripted sequence with no
    /// threads and no sleeps in it -- and the clock and the randomness are the case's too.
    /// @param parts What the tier is built over.
    /// @return The tier, not yet running.
    [[nodiscard]] static std::unique_ptr<DiscoveryTier> Over(Parts parts);

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
    /// -- see `core::net::DatagramAddress`. Out of line so that reason does not make
    /// `Core/HostPort.hpp` a dependency of everything including this header.
    /// @return `host:port`, bracketed when the host is an IPv6 literal.
    [[nodiscard]] std::string BoundEndpoint() const;

    /// How many peers have proved the key the roster holds for them.
    /// @return The count.
    [[nodiscard]] std::size_t AuthenticatedCount() const
    {
        return _directory.AuthenticatedPeers().size();
    }

  private:
    /// What `Start` builds for a production tier and the tier keeps alive: the parts a case
    /// injects, owned here because nothing else outlives the tier.
    struct Production;

    /// @param parts What the tier is built over.
    explicit DiscoveryTier(Parts parts);

    /// Say, throttled, that a beacon was refused at every destination (`BeaconSendOutcome::AllRefused`),
    /// and what the stack answered at each.
    /// @param refused This beat's refusals.
    void ReportRefusedBeacon(std::span<Cluster::BeaconRefusal const> refused);

    /// Say that a beacon was refused on some links while others took it
    /// (`BeaconSendOutcome::PartlyRefused`): counted per change of which links refuse it, and said
    /// at most once per `RefusedBeaconReportInterval`.
    /// @param report This beat's outcome.
    void ReportPartlyRefusedBeacon(Cluster::BeaconSendReport const& report);

    /// Say that every destination takes the beacon again, once, after a refusal -- and only once no
    /// destination has refused it for a whole `RefusedBeaconReportInterval`.
    /// @param report This beat's outcome.
    void ReportBeaconAcceptedEverywhere(Cluster::BeaconSendReport const& report);

    /// Hand the authenticated set to the observer -- the peers whose proven key is STILL the
    /// one the roster holds for them, and who have somewhere to be dialled.
    void PublishAuthenticated();

    /// Owned for a production tier, null for one a case built. Declared first so it is destroyed
    /// LAST: everything below holds references into it.
    std::unique_ptr<Production> _production;

    ILogger& _logger;
    PeerObserver _onPeers;

    /// The roster a proven key is re-checked against at every publish; see `PublishAuthenticated`.
    Consensus::IRaftPeerKeys const& _keys;

    // Declaration order IS construction order, and each is referenced by the one
    // below it -- the reference chain the other tiers own for the same reason.
    std::unique_ptr<core::net::IDatagramSocket> _socket;
    core::platform::IClock& _clock;
    Cluster::PeerDirectory _directory;
    ForeignFleetWatch _watch;
    std::chrono::seconds _beaconInterval;
    core::platform::SteadyTimePoint _nextBeacon;
    /// A beacon refused at every destination, counted per beat.
    Cluster::ThrottledReport _refusedEverywhere { RefusedBeaconReportInterval };
    /// A change of which links refuse a partly refused beacon, counted per change.
    Cluster::ThrottledReport _refusedLinkChanges { RefusedBeaconReportInterval };
    /// The links refusing a partly refused beacon, as last counted; kept until the recovery is said,
    /// so a link that refuses every other beat is ONE change rather than one per refusal.
    std::vector<std::string> _refusedLinks;
    /// When a destination last refused a beacon, all or some; empty when nothing has since it was said.
    std::optional<core::platform::SteadyTimePoint> _lastRefusal;
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
/// A null result is success and means discovery does not run: an empty `--discovery`, the
/// defaulted one on a node that runs no consensus, which discovery runs beside, or the
/// defaulted one on a node whose consensus endpoint reaches only the machine dialling it
/// (`Cluster::AnnouncesOnlyThisMachine`), which is never announced. A typed `--discovery` on
/// either kind of node is refused. Discovery is what makes a *changing* fleet possible, not
/// what makes a fleet possible.
///
/// A node that runs consensus and no discovery answers `foreign-fleet-visible` as not
/// evaluated, saying why: nothing on it could see another fleet.
/// @param cfg The parsed configuration.
/// @param consensus The running consensus tier; null when none was configured.
/// @param self What this node says about itself: `AnsweredFleetSummary(cfg)`, held ONCE by `main`
///        and handed to this and to the FLEET-SUMMARY responder alike, so the two cannot come to
///        describe this node differently; must outlive the tier.
/// @param conditions Where `foreign-fleet-visible` is answered.
/// @param metrics Where discovery's refusals are counted.
/// @param logger Where progress and refusals are reported.
/// @param formation Where proven fleets go and what evidences a split; see `DiscoveryTier::Start`.
/// @return The tier, a null tier meaning "not configured", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<DiscoveryTier>, NodeRefusal> StartDiscoveryOrExplain(
    NodeConfig const& cfg,
    std::unique_ptr<ConsensusTier> const& consensus,
    Cluster::IFleetSummarySource const& self,
    NodeConditions& conditions,
    IMetricsSink& metrics,
    ILogger& logger,
    DiscoveryFormation formation);

/// What an operator is told when discovery's two sockets cannot be bound.
///
/// Both are named, because either can be the one that failed and the opener cannot tell which --
/// a message blaming the beacon port alone sends an operator to look at a port that bound
/// perfectly. The reply socket is spelled by its PORT KIND, as the worksheet spells it: its number
/// when `--discovery-reply-port` pins it, `*` and "port chosen by the kernel at bind" when not --
/// never decided a second time from the flag.
/// @param beacon The shared socket beacons are heard on.
/// @param reply The socket this node answers from.
/// @param why What the socket layer said.
/// @return One sentence, without a full stop.
[[nodiscard]] std::string DiscoveryBindFailure(SurfaceEndpoint const& beacon,
                                               SurfaceEndpoint const& reply,
                                               std::string_view why);

} // namespace FastCache::Node
