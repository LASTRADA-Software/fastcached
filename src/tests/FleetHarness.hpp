// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Consensus/RaftConfig.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/CompileReplySeal.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <CompileCorrelation.hpp>
#include <Dispatch.hpp>
#include <WorkerProtocol.hpp>
#include <apps/fastcache-compile-node/ConsensusStanding.hpp>
#include <apps/fastcache-compile-node/DiscoveryTier.hpp>
#include <apps/fastcache-compile-node/EndpointDialer.hpp>
#include <apps/fastcache-compile-node/FleetSummaryResponder.hpp>
#include <apps/fastcache-compile-node/FleetTextResponder.hpp>
#include <apps/fastcache-compile-node/FrameEndpoint.hpp>
#include <apps/fastcache-compile-node/LiveStatsResponder.hpp>
#include <apps/fastcache-compile-node/MachineStandingTestUtils.hpp>
#include <apps/fastcache-compile-node/NodeConfig.hpp>
#include <apps/fastcache-compile-node/NodeFormation.hpp>
#include <apps/fastcache-compile-node/NodeFrameSurface.hpp>
#include <apps/fastcache-compile-node/NodeMembership.hpp>
#include <apps/fastcache-compile-node/NodePresenceTier.hpp>
#include <apps/fastcache-compile-node/NodeProofResponder.hpp>
#include <apps/fastcache-compile-node/NodeRoster.hpp>
#include <apps/fastcache-compile-node/NodeStatusResponder.hpp>
#include <apps/fastcache-compile-node/Responders.hpp>
#include <apps/fastcache-compile-node/SchedulerLink.hpp>
#include <apps/fastcache-compile-node/SchedulingRedirect.hpp>
#include <apps/fastcache-compile-node/SessionResponder.hpp>
#include <apps/fastcache-compile-node/SharedCacheResponder.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ExactAudience.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

/// A notice these cases do not inspect.
///
/// Shared on purpose: every case here asserts the OUTCOME's `credentialIgnored`
/// flag, not the diagnostic, and a fresh object per call would imply they cared. The
/// cases that do care build their own recording notice, because a shared one reports
/// once and would let whichever case ran first silence the rest -- a coupling to
/// Catch2's ordering that is invisible until it fails.
/// @return A notice with no sink.
///
/// **`inline`, and it has to be.** This is a non-member function DEFINED in a header, so two
/// translation units including `FleetHarness.hpp` are two definitions and the link fails --
/// `multiple definition of Unwatched()`. It went unnoticed because the harness had exactly ONE
/// includer until #1471 added a second, which is the shape of every latent ODR violation in a
/// header: correct-looking, and only discoverable by using the header twice.
///
/// `inline` rather than `static`: the comment above says the notice is SHARED on purpose, and
/// internal linkage would give each translation unit its own. Harmless for a sink-less notice
/// today, and a silent divergence from the stated intent the moment one records anything.
[[nodiscard]] inline FastCache::Cc::CredentialNotice& Unwatched()
{
    static FastCache::Cc::CredentialNotice notice = FastCache::Cc::CredentialNotice::Silent();
    return notice;
}

namespace FastCache::Testing
{

/// A compile fleet running deterministically in one process.
///
/// Test infrastructure, header-only and never linked into the library — the same
/// shape as `Consensus/RaftClusterHarness.hpp`, which is the model, and the same
/// argument for existing.
///
/// ## Why this exists rather than more unit tests
///
/// `SchedulerService`, `WorkerRegistry` and `LeaseTable` each have cases pinning
/// their rules in isolation, and that is not the same as the fleet being right.
/// Several rules in `.agent/rules/distributed-compilation.md` are about a
/// **sequence** across two machines — a lease granted by one scheduler and
/// resolved against that same one after leadership has moved — and no
/// single-transition test reaches an interleaving. The only other way to arrange
/// one today is a script that spawns processes, binds ports and waits on a
/// wall clock, which cannot schedule the interleaving at all: it can only hope
/// for it.
///
/// ## Why it lives in `src/tests/`
///
/// By shape it belongs beside `RaftClusterHarness`. It does not go there.
/// `RaftClusterHarness` sits in `Consensus/` because everything it touches sits
/// in `Consensus/`; a fleet spans `Distributed/`, the node and the launcher's
/// client sources, so putting it under `src/FastCache/` would make a library
/// directory include an app header. This tree already enforces that `Net/` must
/// not depend on `Core/` with a ctest; library-depending-on-app is the worse
/// version of the same thing and has no gate to catch it.
///
/// ## What is real here and what is not
///
/// Real: `SchedulerService` and `SchedulerProtocol` decide and frame every reply;
/// the launcher's own `Cc::ExchangeFramed` writes and reads the client side;
/// `Cc::Dispatch` runs unmodified, so the lease-redirect and release-routing
/// logic under test is the shipped logic.
///
/// Not real: there is no socket and no worker process. A request is handed to the
/// addressed scheduler's protocol object, and its answer is replayed to the
/// client through a `ScriptedSocket`. The worker's endpoint answers whatever
/// `SetWorkerReply` (or, per address, `AddWorkerAddress`) was given, or reaches nothing
/// once `SetWorkerUnreachable` has taken it off the network.
///
/// Time moves only in `Step`.
///
/// ## Machine tickets
///
/// A request presenting a `MachineTicket` credential to a scheduler is decided as production's
/// session surface decides it: that node's production `Distributed::TicketVerifier`, over a
/// `StateLeaseRoster` adopting the node's own cluster state, a per-node spent set and an audience
/// that is exactly the endpoint dialled. The AUTH reply is scripted ahead of the command's, and the
/// command's caller is `Distributed::CallerContextOf` over the node's oracle with the ticket's
/// machine folded in -- so a case sets a production `NodeMembership` (`PublishMembershipAt`) and a
/// non-loopback caller host (`SetCallerHost`) before it asserts anything about admission.
///
/// A WORKER address answers a presented ticket without looking at it, unless
/// `VerifyTicketsAtWorker` named it: then the same production verifier decides, over an audience
/// the case supplies -- production's `NodeAudience` when the question is which endpoints a ticket
/// may name, as it is for a dial hint.
///
/// ## Rosters (#178)
///
/// Every scheduler signs its grants with its own identity key -- `TestKeyPair` of its
/// endpoint, which is also its member id -- and holds a cluster state of its own, set by
/// `SetClusterStateAt`: an ex-leader that has not heard a change is a node holding an older
/// one. A node's presence round dials a scheduler through this harness (`Dial`, `SetUnreachable`), so
/// `SchedulerLink`'s redirects and fallbacks run as production runs them. A `LearnerWorker` is a
/// machine admitted as a LEARNER: a production `FastCache::Node::NodeRoster` fed the state of the
/// scheduler it learns from -- re-applied at every `Step` and at `Apply`, as a learner applies what its
/// leader replicates -- with the production lease validator and ticket verifier over it.
class FleetHarness final: public Cc::IEndpointExchange, public FastCache::Node::IEndpointDialer
{
  public:
    /// What one exchange did, in the order it happened.
    struct Call
    {
        std::string endpoint; ///< Who was asked. Usually the fact under test.
        std::uint8_t opRaw;   ///< Which verb, as the byte on the wire.
        /// How it ended. Recorded because "the client asked the right machine" and
        /// "the right machine did the thing" are separate facts, and a case that
        /// checks only the first cannot tell a resolved lease from a refused one.
        Cc::CacheOutcomeKind kind;
        /// The refusal, meaningful only when @ref kind is `Rejected`.
        CompileCacheWire::ErrorCode code;
    };

    /// The id every admitted caller of this harness proved, and so the machine every worker it
    /// registers is: its test key is the one each grant names (W-4). Public, so a case building a
    /// worker's reply signs it as that machine -- or, on purpose, as another.
    static constexpr std::string_view ProvenMachine = "harness-machine";

    /// The fleet every node in this harness belongs to.
    ///
    /// One id across all of them, because a grant minted by any of these schedulers
    /// must verify on any of these workers -- that is what the harness exists to
    /// drive. Named rather than empty so the comparison is a real one (#322).
    static constexpr std::string_view ClusterId = "fleet-harness";

    FleetHarness()
    {
        // A worker that refuses is the default because the release is the subject
        // here, and the rule under test is that it happens on EVERY path out of the
        // compile -- a refused job included. A test wanting a successful compile
        // says so with `SetWorkerReply`.
        _workerReply = CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::NoCapacity, "harness worker");
    }

    /// Add a scheduler at @p endpoint.
    ///
    /// It starts as a follower knowing no leader, because that is what a node that
    /// has not yet heard from consensus is. `ElectLeader` is what makes a fleet.
    /// @param endpoint How clients address it, e.g. `"sched-a:6676"`.
    /// @throws std::runtime_error when a scheduler or a learner is already there: `Answer` would
    ///         route every request to whichever it asks first, and the other would be unreachable.
    void AddScheduler(std::string endpoint)
    {
        RefuseTaken(endpoint, "a scheduler");
        auto node = std::make_unique<Node>(*this, std::move(endpoint));
        _nodes.push_back(std::move(node));
        Commit(*_nodes.back());
    }

    /// Add a LEARNER's `0xFC` surface at @p endpoint: a node running consensus and no scheduler,
    /// which answers the fleet's scheduling verbs with the leader it follows (#1639).
    ///
    /// Composed by production's own `ComposeSurfaceComponents` and routed by production's
    /// `MergedResponder`, over the owners a built learner has: the `SchedulingRedirectResponder` for the
    /// `Scheduler` family, and the every-node owners (`NodeStatus`, live stats, fleet text, session,
    /// the fleet's shared cache), the identity prover a node running consensus serves, and the
    /// fleet-summary owner its identity key answers with -- each built as `main` builds it, over
    /// sources nothing attaches -- **except the session owner**, which is not built as `main` builds
    /// it: it holds no identity key (`SessionKeys {}`), so it mints no ticket where a built learner
    /// would, and its ticket verifier checks against no roster, under an `ExactAudience` of this
    /// endpoint. Enrollment is refused for the reason a learner gives
    /// (`EnrollmentAbsenceOf`). It is a learner running **no cache tier and no worker tier**, the two
    /// components a configuration may add, so those families are refused as served nowhere. The request
    /// cap is therefore production's fold -- the shared cache's, the largest -- and not the redirect's
    /// own 64 KiB. The redirect reads a `KnownSchedulingLeader` that a `SchedulingLeaderPublisher`
    /// feeds from the role observer's endpoint and every pass's reading, as `ConsensusTier`'s two
    /// observers do. A request passes production's header gate (`Node::DecideHeaderRefusal`) before
    /// `Answer`, for the caller `SetCallerHost` and `SetCallerIdentity` describe.
    ///
    /// **What a learner endpoint models, and what it does not:**
    /// - the verbs a LAUNCHER sends it, presenting no credential: the scheduling family, and
    ///   `MINT-TICKET` to the session component;
    /// - **no `AUTH`**: an exchange presenting a credential is refused loudly (`Exchange` throws).
    ///   Production answers `AUTH` in the endpoint's own loop (`AnswerAuth` in `FrameEndpoint.cpp`,
    ///   private), so modelling it here would be a second copy of that switch -- the copy of endpoint
    ///   logic this harness exists not to have;
    /// - **no presence dial**: `Dial` reaches schedulers only, so a learner endpoint is unreachable to
    ///   a node's presence round;
    /// - **no worker**: it is no compile port (`LearnerWorker` models a learner's), and no worker
    ///   may be registered at its endpoint.
    ///
    /// **Its own metrics sink** (`RedirectingLearnerMetrics`); see `RedirectingLearner::metrics`.
    /// @param endpoint How clients address it, e.g. `"learner-c:6674"`.
    /// @throws std::runtime_error when anything already answers there.
    void AddRedirectingLearner(std::string endpoint)
    {
        RefuseTaken(endpoint, "a learner");
        if (std::ranges::contains(_workerEndpoints, endpoint))
            throw std::runtime_error { "FleetHarness: a learner at " + endpoint + " would shadow the worker there" };
        _redirectingLearners.push_back(
            std::make_unique<RedirectingLearner>(std::move(endpoint), _clock, _wallClock, _logger));
        Hear(*_redirectingLearners.back());
    }

    /// The counters one learner's surface moved; see `AddRedirectingLearner`.
    /// @param endpoint Which learner; must have been added.
    /// @return Its sink.
    [[nodiscard]] AtomicMetricsSink& RedirectingLearnerMetrics(std::string_view endpoint)
    {
        return RedirectingLearnerAt(endpoint).metrics;
    }

    /// Make @p endpoint the leader and every other scheduler its follower.
    ///
    /// One call, both halves: a test that set only the new leader would leave the
    /// old one still answering as leader, which is a fleet no election produces
    /// and would let a wrong client pass.
    ///
    /// Every learner (`AddRedirectingLearner`) hears it too, as its role observer would.
    /// @param endpoint The scheduler that now leads; must have been added.
    void ElectLeader(std::string_view endpoint)
    {
        (void) NodeAt(endpoint); // refuse an endpoint nobody added, loudly
        _leader = std::string { endpoint };
        for (auto const& node: _nodes)
        {
            if (node->endpoint == endpoint)
                node->service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
            else
                node->service.SetRole(Distributed::SchedulerRole::Follower, endpoint, Distributed::StandaloneSchedulerTerm);
            NoteReadings(*node);
        }
        for (auto const& learner: _redirectingLearners)
            Hear(*learner);
    }

    /// Where every subsequent request appears to come from.
    ///
    /// **A constant here cannot carry a membership property.** The default is `127.0.0.1`, and
    /// loopback admits every caller that shows no key -- deliberately, so a node always admits
    /// its own machine -- so a case asserting a refusal from the default caller goes green having
    /// exercised nothing. (A forget names a KEY, never an address: a revoked key is refused from
    /// loopback too.) Any case about admission sets this first.
    /// @param host The caller's host, as `core::net::ISocket::PeerAddress()` would report it.
    void SetCallerHost(std::string host)
    {
        _callerHost = std::move(host);
    }

    /// The identity every subsequent request's connection PROVED, or nothing for the default.
    ///
    /// **A machine is forgotten by its key, so a forget is falsifiable here only through one.**
    /// With an identity set, a node that has an oracle decides the caller exactly as production's
    /// endpoint does -- `Distributed::CallerContextOf`, the address routes folded with the key --
    /// so a revoked key is refused wherever `SetCallerHost` says it dials from. Without one, the
    /// caller is decided by its address alone, as every case predating the key roster assumes.
    /// @param identity The proven identity, or nullopt to go back to the address alone.
    void SetCallerIdentity(std::optional<ProvenIdentity> identity)
    {
        _callerIdentity = std::move(identity);
    }

    /// Let one node's admission oracle decide what its callers are.
    ///
    /// **Per NODE, and that is the point rather than generality.** The state #1471 is about is
    /// two machines disagreeing about one host -- a forget committed on the leader and not yet
    /// applied on the second -- and one oracle shared across the fleet cannot express it at all.
    ///
    /// A node given no oracle is decided by the fold every node composes when it is not open --
    /// this machine (loopback) and a key roster of its own applied state -- and NEVER by "everybody
    /// is a proven member" (W-8): a fake more permissive than production lets a fold or publish
    /// regression stay green. The oracle is borrowed, not owned: the production objects belong to
    /// the case, because a `NodeMembership` needs a config and a logger this harness has no
    /// business inventing.
    /// @param endpoint Which node; must have been added.
    /// @param oracle Who decides its callers, or nullptr to go back to the node's default fold.
    void SetMembershipAt(std::string_view endpoint, Distributed::IMembershipOracle const* oracle)
    {
        auto& node = NodeAt(endpoint);
        node.membership = oracle != nullptr ? oracle : &node.fold;
    }

    /// Register a worker with one scheduler.
    /// @param scheduler Which scheduler's registry to put it in.
    /// @param workerEndpoint Where the worker answers compiles.
    /// @param fingerprint The toolchain it serves.
    /// @param slots How many jobs it will take at once.
    /// @throws std::runtime_error when a learner answers there (`AddRedirectingLearner`), which would
    ///         shadow it.
    void RegisterWorker(std::string_view scheduler,
                        std::string_view workerEndpoint,
                        std::string_view fingerprint,
                        std::uint32_t slots = 1)
    {
        RefuseWorkerAtLearner(workerEndpoint);
        auto const reply = NodeAt(scheduler).service.Register(
            // `SetupCaller`, never the case's caller: this ARRANGES the fleet and throws when
            // refused, so a case that set a forgotten caller host would fail here during setup
            // rather than at the assertion it was written for -- which reads as the harness
            // being broken. Registration IS membership-gated in production (`Gate` refuses
            // `Register` too); what this says is that the worker doing the registering is not
            // the client under test. Do not unify these two.
            SetupCaller(),
            Distributed::WorkerRegistration {
                .fingerprint = fingerprint, .endpoint = workerEndpoint, .slots = slots, .codecs = {} });
        if (reply.status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a worker registration" };
        _workerEndpoints.emplace_back(workerEndpoint);
    }

    /// Register a worker the way a machine behind a DNS NAME does, and return its id so a
    /// case can heartbeat it from wherever that machine is now.
    ///
    /// Through `SetupCaller` for `RegisterWorker`'s reason, so it is seen on loopback until a
    /// `HeartbeatFrom` says otherwise -- and loopback is never a dial hint.
    /// @param scheduler Which scheduler; must be leading.
    /// @param advertised What the worker advertises, e.g. `laptop.corp:6677`.
    /// @param fingerprint The toolchain it serves.
    /// @return The id the scheduler assigned.
    /// @throws std::runtime_error when a learner answers there (`AddRedirectingLearner`), which would
    ///         shadow it.
    [[nodiscard]] std::string RegisterWorkerNamed(std::string_view scheduler,
                                                  std::string_view advertised,
                                                  std::string_view fingerprint)
    {
        RefuseWorkerAtLearner(advertised);
        auto const reply = NodeAt(scheduler).service.Register(
            SetupCaller(),
            Distributed::WorkerRegistration {
                .fingerprint = fingerprint, .endpoint = advertised, .slots = 1, .codecs = {} });
        auto const decoded = CompileCacheWire::DecodeRegisterReply(reply.payload);
        if (reply.status != CompileCacheWire::Status::Ok || !decoded.has_value())
            throw std::runtime_error { "FleetHarness: the scheduler refused a worker registration" };
        _workerEndpoints.emplace_back(advertised);
        return decoded->workerId;
    }

    /// Heartbeat @p workerId as the kernel would report it arriving from @p observedHost,
    /// with the interface addresses the worker says it answers on.
    /// @param scheduler Which scheduler; must be leading.
    /// @param workerId The id `RegisterWorkerNamed` returned.
    /// @param observedHost The peer host the scheduler's connection reports.
    /// @param interfaces What the worker reports it answers on.
    void HeartbeatFrom(std::string_view scheduler,
                       std::string_view workerId,
                       std::string observedHost,
                       std::vector<std::string> const& interfaces)
    {
        auto const caller = Distributed::CallerContext { .membership = Distributed::Membership::Member,
                                                         .peerId = std::move(observedHost),
                                                         .provenNodeId = std::string { ProvenMachine } };
        if (NodeAt(scheduler).service.Heartbeat(caller, workerId, Distributed::NodeLoad {}, {}, interfaces).status
            != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a heartbeat" };
    }

    /// Announce a MACHINE to one scheduler, as `NodeAnnounce` does.
    ///
    /// **The verb a node sends whatever components it runs**, which is why this is beside
    /// `RegisterWorker` rather than folded into it: a machine with `--slots=0` sends this and
    /// never registers anything, and a harness that could only express a registration could
    /// not build the fleet
    /// [#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440) is about -- a
    /// scheduler-only leader whose own row and own history the page has to carry.
    /// @param scheduler Which scheduler to tell.
    /// @param machineEndpoint The machine announcing itself.
    /// @param history Closed buckets it is handing over; empty is the ordinary case.
    void AnnounceMachine(std::string_view scheduler,
                         std::string_view machineEndpoint,
                         std::span<Distributed::FleetBucket const> history = {})
    {
        if (AnnounceMachineAnswer(scheduler, machineEndpoint, std::nullopt, history).status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a machine announcement" };
    }

    /// Announce a machine carrying its condition rows (#1364), and hand back what the scheduler
    /// ANSWERED rather than throwing on a refusal -- a case about refusing a row that is not text has
    /// to be able to see the refusal, which `AnnounceMachine`'s arranging contract hides.
    /// @param scheduler Which scheduler to tell.
    /// @param machineEndpoint The machine announcing itself.
    /// @param conditions What it says is wrong with it; `std::nullopt` is a build that says nothing.
    /// @param history Closed buckets it is handing over.
    /// @return The scheduler's reply.
    [[nodiscard]] Distributed::SchedulerReply AnnounceMachineAnswer(
        std::string_view scheduler,
        std::string_view machineEndpoint,
        std::optional<std::vector<CompileCacheWire::NodeConditionFields>> conditions,
        std::span<Distributed::FleetBucket const> history = {})
    {
        return NodeAt(scheduler).service.AnnounceNode(
            // `SetupCaller` for `RegisterWorker`'s reason: this ARRANGES the fleet, so a case
            // that set a refused caller host must fail at its own assertion rather than here.
            SetupCaller(),
            Distributed::NodePresence { .endpoint = machineEndpoint,
                                        .version = "harness",
                                        .capacity = Distributed::NodeCapacity { .logicalCores = 8 },
                                        .load = Distributed::NodeLoad {},
                                        .conditions = std::move(conditions) },
            history);
    }

    /// The Machines rows one scheduler would draw.
    ///
    /// `NodeReports()` and never the worker entries, because that is what the page walks: a
    /// machine serving two toolchains is two registry entries and ONE row, and a machine
    /// running no worker is no entry at all and still a row.
    /// @param scheduler Which scheduler to ask.
    /// @return One report per machine it knows about.
    [[nodiscard]] std::vector<Distributed::NodeReport> MachinesAt(std::string_view scheduler)
    {
        return NodeAt(scheduler).service.Workers().NodeReports();
    }

    /// Where @p scheduler files what other machines hand it.
    ///
    /// Borrowed, and the case owns it: a history store needs paths and a logger this harness
    /// has no business inventing -- `SetMembershipAt`'s arrangement, for its reason.
    /// @param scheduler Which scheduler.
    /// @param sink Where handed-over buckets go, or null to discard them.
    void SetHistorySinkAt(std::string_view scheduler, Distributed::IFleetHistorySink* sink)
    {
        NodeAt(scheduler).service.SetHistorySink(sink);
    }

    /// Advance every clock in the fleet.
    ///
    /// The only way time moves. A lease expiry, a heartbeat age and a grant's
    /// absolute deadline are all read from these, so a test states the passage of
    /// time rather than sleeping for it.
    /// @param by How far.
    void Step(std::chrono::milliseconds by)
    {
        _clock.advance(by);
        _wallClock.advance(by);
        // Every learner applies what the scheduler it learns from has applied by now.
        for (auto* const learner: _learners)
            learner->Apply();
        // And every node's consensus runs a pass, reporting who leads -- what keeps a roster
        // current in production, at a pass rather than at an exchange.
        for (auto const& node: _nodes)
            NoteReadings(*node);
        for (auto const& learner: _redirectingLearners)
            Hear(*learner);
    }

    /// Run @p hook the next time a compile is sent to a worker.
    ///
    /// **This is what makes an interleaving arrangeable.** `Cc::Dispatch` leases,
    /// compiles and releases in one call, so a test cannot get between the grant
    /// and the release from outside — and the moment between them is exactly where
    /// leadership moving is interesting. The hook fires inside the compile
    /// exchange, on the caller's thread, deterministically.
    /// @param hook What to do; cleared after it fires.
    void OnCompile(std::function<void()> hook)
    {
        _onCompile = std::move(hook);
    }

    /// A successful COMPILE reply as the worker this harness registers sends it: signed under
    /// `TestKeyPair(ProvenMachine)`, the key every grant here names (W-4).
    ///
    /// A case about an IMPOSTOR passes another machine's key as @p signer: the reply is then
    /// well formed, correlated and signed -- by somebody the grant did not name.
    /// @param object The object field, enveloped as a worker sends it.
    /// @param correlation What the worker says it compiled.
    /// @param signer Whose key signs it.
    /// @return A complete reply frame.
    [[nodiscard]] static std::vector<std::byte> SignedWorkerReply(std::span<std::byte const> object,
                                                                  std::string_view correlation,
                                                                  std::string const& signer = std::string { ProvenMachine })
    {
        auto const signature = SealCompileReply(TestKeyPair(signer), CompileCacheWire::AsBytes(correlation), object);
        return CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok,
                                             CompileCacheWire::EncodeCompileResult(CompileCacheWire::CompileResult {
                                                 .exitCode = 0,
                                                 .object = object,
                                                 .stdoutText = {},
                                                 .stderrText = {},
                                                 .correlation = CompileCacheWire::AsBytes(correlation),
                                                 .signature = std::span<std::byte const> { signature } }));
    }

    /// The reply an HONEST worker sends for @p request's job: an object, and the correlation the
    /// launcher recomputes -- signed under the key the grant names unless @p signer says otherwise --
    /// so a case asserting `Compiled` asserts the compile was ACCEPTED, not merely that some address
    /// answered.
    /// @param request The job the launcher dispatches; only the fields the correlation folds are read.
    /// @param signer Whose identity key signs it: the registered worker's unless a case is about an
    ///        impostor; nullopt for a reply nobody signed.
    /// @return The framed reply.
    [[nodiscard]] static std::vector<std::byte> CompiledReply(Cc::DispatchRequest const& request,
                                                              std::optional<std::string> const& signer = std::string {
                                                                  ProvenMachine })
    {
        constexpr std::string_view Object = "OBJ";
        auto const correlation =
            Cc::CompileCorrelation(Cc::CorrelatedCompile { .preprocessed = request.preprocessed,
                                                           .args = request.args,
                                                           .fingerprint = request.fingerprint,
                                                           .sourceName = request.sourceName,
                                                           .compileDir = request.compileDir,
                                                           .compileDirReplacement = request.compileDirReplacement,
                                                           .sourceRoot = request.sourceRoot,
                                                           .sourceRootReplacement = request.sourceRootReplacement });
        auto const enveloped = CompileCacheWire::EncodeCodecEnvelope(
            CompileCacheWire::IdentityCodec, static_cast<std::uint32_t>(Object.size()), CompileCacheWire::AsBytes(Object));
        if (signer.has_value())
            return SignedWorkerReply(enveloped, correlation, *signer);
        return CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok,
                                             CompileCacheWire::EncodeCompileResult(CompileCacheWire::CompileResult {
                                                 .exitCode = 0,
                                                 .object = enveloped,
                                                 .stdoutText = {},
                                                 .stderrText = {},
                                                 .correlation = CompileCacheWire::AsBytes(correlation),
                                                 .signature = {} }));
    }

    /// What a worker endpoint answers a COMPILE with.
    /// @param reply A complete reply frame.
    void SetWorkerReply(std::vector<std::byte> reply)
    {
        _workerReply = std::move(reply);
    }

    /// Take a WORKER address off the network, or put it back: a compile dialled there
    /// reaches nothing (`Unreached`), as a machine whose VPN address moved answers.
    ///
    /// Separate from `SetUnreachable`, which is about a scheduler's presence dial and is
    /// consulted only by `Dial`; this is consulted by the client's `Exchange`.
    /// @param address The endpoint as the client would dial it.
    /// @param unreachable Whether it answers.
    void SetWorkerUnreachable(std::string_view address, bool unreachable)
    {
        std::erase(_unreachableWorkers, std::string { address });
        if (unreachable)
            _unreachableWorkers.emplace_back(address);
    }

    /// Make @p address answer COMPILE -- the machine a dial hint names -- with @p reply, or
    /// with the shared worker reply when none is given.
    ///
    /// Registers nothing with any scheduler: this is an address a client may DIAL, which is
    /// a different fact from a worker a scheduler may GRANT.
    /// @param address The endpoint.
    /// @param reply What that address answers a COMPILE with.
    /// @throws std::runtime_error when a learner answers there (`AddRedirectingLearner`), which would
    ///         shadow it.
    void AddWorkerAddress(std::string address, std::optional<std::vector<std::byte>> reply = std::nullopt)
    {
        RefuseWorkerAtLearner(address);
        if (reply.has_value())
            _workerReplyAt.insert_or_assign(address, *std::move(reply));
        _workerEndpoints.push_back(std::move(address));
    }

    /// Make the worker answering at @p address verify a presented machine ticket as its session
    /// surface does: the production `Distributed::TicketVerifier`, over a roster adopting
    /// @p stateOf's applied state at every exchange, a spent set of its own and @p audience -- and
    /// the counted refusal ahead of the command's reply when it refuses (`Distributed::AnswerTicket`),
    /// which the launcher's framing reads as the whole exchange refused.
    ///
    /// Without it a worker address answers whatever was presented, so no case could tell whether the
    /// ticket a dial carried is one the machine it reached would take -- which is the question a
    /// dial HINT raises: the launcher mints for the address it dials, not for the name the worker
    /// advertises.
    /// @param address The endpoint as the client dials it; `AddWorkerAddress` it as well.
    /// @param audience What that worker answers to; borrowed, and must outlive the harness's use.
    ///        Production's `FastCache::Node::NodeAudience` for a case about which endpoints a ticket
    ///        may name.
    /// @param stateOf The scheduler whose applied state the worker's roster holds.
    void VerifyTicketsAtWorker(std::string address, Distributed::IAudience const& audience, std::string_view stateOf)
    {
        auto& node = NodeAt(stateOf);
        auto worker = std::make_unique<TicketedWorker>(_clock);
        worker->address = std::move(address);
        worker->audience = &audience;
        worker->stateOf = std::string { stateOf };
        // A starting worker applies the state its fleet has committed so far, as consensus hands it
        // over; from then on only a commit (`Commit`) moves it.
        AdoptAt(*worker, node);
        _ticketedWorkers.push_back(std::move(worker));
    }

    /// Every exchange the fleet has served, in order.
    /// @return The log.
    [[nodiscard]] std::vector<Call> const& Calls() const noexcept
    {
        return _calls;
    }

    /// The COMPILE exchanges logged from @p from on, in the order they were sent.
    ///
    /// Which ADDRESS a compile went to is the fact a dial-order case is about, and how it
    /// ended there is the fact that says whether a second dial was allowed.
    /// @param from How many calls to skip: the log's size before the launcher ran.
    /// @return One call per COMPILE -- a dial that reached nothing included.
    [[nodiscard]] std::vector<Call> CompileCallsSince(std::size_t from) const
    {
        auto const compileOp = static_cast<std::uint8_t>(CompileCacheWire::Op::Compile);
        std::vector<Call> compiles;
        for (auto const& call: std::span { _calls }.subspan(from))
            if (call.opRaw == compileOp)
                compiles.push_back(call);
        return compiles;
    }

    /// The addresses COMPILE was sent to, from @p from on.
    /// @param from How many calls to skip: the log's size before the launcher ran.
    /// @return One endpoint per COMPILE, in order -- a dial that reached nothing included.
    [[nodiscard]] std::vector<std::string> CompiledAt(std::size_t from) const
    {
        std::vector<std::string> compiledAt;
        for (auto const& call: CompileCallsSince(from))
            compiledAt.push_back(call.endpoint);
        return compiledAt;
    }

    /// Whether @p scheduler still has @p key marked as being built.
    /// @param scheduler Which scheduler to ask.
    /// @param key The object key.
    /// @return True while a live lease suppresses it there.
    [[nodiscard]] bool IsInFlight(std::string_view scheduler, std::string_view key)
    {
        return NodeAt(scheduler).service.Leases().IsInFlight(key);
    }

    /// The fleet's counters, shared by every scheduler in it.
    /// @return The sink.
    [[nodiscard]] AtomicMetricsSink& Metrics() noexcept
    {
        return _metrics;
    }

    /// Admit @p machine to every scheduler's cluster, under `TestKeyPair(machine)`, as a LEARNER --
    /// the seat a machine that is not a voter holds.
    ///
    /// Every node at once, and its roster version moved, as an applied admission would. A node whose
    /// membership was published (`PublishMembershipAt`) is republished.
    /// @param machine The machine's id, which is also the id its tickets name.
    void AdmitMachine(std::string const& machine)
    {
        for (auto const& node: _nodes)
        {
            node->cluster.state.members.push_back(
                Cluster::ClusterMember { .id = machine,
                                         .raftEndpoint = machine,
                                         .schedulerEndpoint = {},
                                         .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                         .seat = Cluster::MemberSeat::Learner,
                                         .publicKey = TestKeyPair(machine).PublicKey() });
            ++node->cluster.state.rosterVersion;
            Commit(*node);
        }
    }

    /// Forget @p machine everywhere at once, as an applied `Forget` does (#1555): its record goes
    /// and its key joins the revoked list in the same entry. Republished as `AdmitMachine`
    /// is.
    /// @param machine The machine's id.
    void ForgetMachine(std::string const& machine)
    {
        for (auto const& node: _nodes)
        {
            std::erase_if(node->cluster.state.members,
                          [&machine](Cluster::ClusterMember const& member) { return member.id == machine; });
            node->cluster.state.revokedKeys.push_back(
                Cluster::RevokedKey { .id = machine, .publicKey = TestKeyPair(machine).PublicKey() });
            ++node->cluster.state.rosterVersion;
            Commit(*node);
        }
    }

    /// A ticket @p machine would mint for @p audience, now, with its own test key.
    /// @param machine Who it speaks for.
    /// @param audience The endpoint it may be presented to.
    /// @param nonce Distinguishes two tickets otherwise equal: a node spends each once.
    /// @return The credential a launcher would present.
    [[nodiscard]] Cc::Credential TicketFor(std::string const& machine, std::string const& audience, std::uint8_t nonce = 1)
    {
        auto claims = Distributed::MachineTicketClaims {
            .machineId = machine,
            .audience = audience,
            .expiresAtUnixSeconds =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                               (_wallClock.now() + Distributed::MachineTicketLifetime).time_since_epoch())
                                               .count()),
            .nonce = {},
        };
        claims.nonce.front() = std::byte { nonce };
        return Cc::Credential { .kind = CompileCacheWire::AuthKind::MachineTicket,
                                .username = {},
                                .secret = Distributed::MintMachineTicket(TestKeyPair(machine), claims) };
    }

    /// The same claims as @p ticket, signed by @p signer's key instead: a ticket nobody the claims
    /// name could have minted.
    /// @param ticket A ticket `TicketFor` minted.
    /// @param signer Whose key signs the copy.
    /// @return The forgery.
    [[nodiscard]] static Cc::Credential ForgeAs(Cc::Credential const& ticket, std::string const& signer)
    {
        auto const decoded = Distributed::DecodeMachineTicket(CompileCacheWire::AsBytes(ticket.secret.View()));
        if (!decoded.has_value())
            throw std::runtime_error { "FleetHarness: ForgeAs was handed something that is not a ticket" };
        return Cc::Credential { .kind = CompileCacheWire::AuthKind::MachineTicket,
                                .username = {},
                                .secret = Distributed::MintMachineTicket(TestKeyPair(signer), decoded->claims) };
    }

    /// Let a production `NodeMembership` decide @p endpoint's callers, published with that node's
    /// cluster state now and again at every `AdmitMachine` and `ForgetMachine`.
    /// @param endpoint Which node; must have been added.
    /// @param membership The node's admission policy; borrowed, and must outlive the harness's use.
    void PublishMembershipAt(std::string_view endpoint, FastCache::Node::NodeMembership& membership)
    {
        auto& node = NodeAt(endpoint);
        node.published = &membership;
        node.membership = &membership.Oracle();
        Commit(node);
    }

    /// The cluster's state as @p scheduler has applied it.
    ///
    /// Per node, because the case #178 is about is two nodes disagreeing: an ex-leader that
    /// never heard the change revoking it holds the state from before.
    /// @param scheduler Which scheduler; must have been added.
    /// @param state What it applied.
    void SetClusterStateAt(std::string_view scheduler, Cluster::ClusterState state)
    {
        auto& node = NodeAt(scheduler);
        node.cluster.state = std::move(state);
        Commit(node);
    }

    /// A state whose voters are @p voters, each keyed with its test key, with @p revoked
    /// forgotten -- and its roster version, as `Apply` would have derived it.
    /// @param voters The voters, by member id.
    /// @param revoked Machines the cluster forgot, which revoked their test keys.
    /// @param version The roster version the state carries.
    /// @return The state.
    [[nodiscard]] static Cluster::ClusterState StateOf(std::vector<std::string> const& voters,
                                                       std::vector<std::string> const& revoked,
                                                       std::uint64_t version)
    {
        Cluster::ClusterState state;
        for (auto const& id: voters)
            state.members.push_back(
                Cluster::ClusterMember { .id = id,
                                         .raftEndpoint = id,
                                         .schedulerEndpoint = id,
                                         .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                         .seat = Cluster::MemberSeat::Voter,
                                         .publicKey = TestKeyPair(id).PublicKey() });
        // As `Apply` forgets (#1555): the member leaves `members` and its key joins the revoked
        // list in the same entry, so it is no longer a voter at all.
        for (auto const& id: revoked)
        {
            std::erase_if(state.members, [&id](Cluster::ClusterMember const& member) { return member.id == id; });
            state.revokedKeys.push_back(Cluster::RevokedKey { .id = id, .publicKey = TestKeyPair(id).PublicKey() });
        }
        state.rosterVersion = version;
        return state;
    }

    /// What @p scheduler's service noted as a recorded member's moved `0xFC` endpoint, in the
    /// order it was told: the member that proved it, and where it now answers. Noted only by a
    /// scheduler that ANSWERED the announcement -- the leader -- so this is the evidence that an
    /// announcement reached it, where a reply alone says only that SOME scheduler took it.
    /// @param scheduler Which scheduler; must have been added.
    /// @return Each noted (member, endpoint), oldest first.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> AnnouncedEndpointsAt(std::string_view scheduler)
    {
        return NodeAt(scheduler).cluster.noted;
    }

    /// @p scheduler stops knowing who leads: a follower whose election timer fired, or a leader
    /// CheckQuorum deposed -- what a voter whose address vanished is, since no leader's message
    /// reaches it. It answers every verb `NotLeader` naming NOBODY, which no round can follow.
    /// @param scheduler Which scheduler; must have been added.
    void ForgetLeaderAt(std::string_view scheduler)
    {
        auto& node = NodeAt(scheduler);
        // `Undecided`, as `SchedulerRoleFor` maps a candidate that knows no leader.
        node.service.SetRole(Distributed::SchedulerRole::Undecided, {}, Distributed::StandaloneSchedulerTerm);
        NoteReadings(node);
    }

    /// Take @p scheduler off the network, or put it back: a dial to it fails, as a machine
    /// that is down or partitioned from the caller answers.
    /// @param scheduler Which scheduler.
    /// @param unreachable Whether it answers.
    void SetUnreachable(std::string_view scheduler, bool unreachable)
    {
        (void) NodeAt(scheduler);
        std::erase(_unreachable, std::string { scheduler });
        if (unreachable)
            _unreachable.emplace_back(scheduler);
    }

    /// A grant @p scheduler mints for @p key, for a worker registered there serving
    /// @p fingerprint: its own `Lease` verb, signed with its own key.
    /// @param scheduler Who grants.
    /// @param fingerprint The toolchain.
    /// @param key The object key.
    /// @return The token a client would present.
    [[nodiscard]] std::string GrantAt(std::string_view scheduler, std::string_view fingerprint, std::string_view key)
    {
        auto const reply = NodeAt(scheduler).service.Lease(
            SetupCaller(), CompileCacheWire::LeaseRequest { .fingerprint = fingerprint, .key = key, .acceptedCodecs = {} });
        if (reply.status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a lease" };
        auto const grant = CompileCacheWire::DecodeLeaseGrant(reply.payload);
        if (!grant.has_value())
            throw std::runtime_error { "FleetHarness: the scheduler answered a lease with no grant" };
        return std::string { CompileCacheWire::AsStringView(grant->leaseToken) };
    }

    /// A machine admitted as a LEARNER of this fleet (#178): a production `FastCache::Node::NodeRoster`,
    /// built for a learner's configuration and fed what that learner APPLIED -- `NodeRoster::Applied`,
    /// which the consensus tier's apply callback calls with every committed state -- with the
    /// production lease validator over it for the grants its compile port is shown, and the
    /// production ticket verifier for an audience that is exactly this machine.
    ///
    /// What it applied is a scheduler's state copied at `Apply`, which every `Step` also does for
    /// every live learner; between two of those a case can hold a learner that has not caught up.
    class LearnerWorker final
    {
      public:
        /// @param fleet The harness it lives in, for the clocks, the sink, the states and the step.
        /// @param endpoint Its member id, and the endpoint every grant and ticket for it names.
        /// @param leader The scheduler whose applied state this learner applies; must have been
        ///        added.
        LearnerWorker(FleetHarness& fleet, std::string endpoint, std::string leader):
            _fleet { fleet },
            _advertised { endpoint },
            _endpoint { std::move(endpoint) },
            _leader { std::move(leader) },
            _roster { BuildRoster(_endpoint, _leader, fleet._clock) },
            _lease { Distributed::SchedulerTermRegressionNotice::Silent() },
            // The key every worker this harness registers proved (`SetupCaller`), so the key every
            // grant names: copied by the validator inside this full expression (W-4).
            _validator { Cc::SignedLeaseValidator(
                *_roster->Lease(),
                _advertised,
                std::span<std::byte const> { TestKeyPair(std::string { ProvenMachine }).PublicKey() },
                fleet._wallClock,
                _lease,
                fleet._metrics) }
        {
            // Registered into the harness's fleet, as a completed REGISTER round pins it (#401).
            _lease.fleet.Pin(std::string { ClusterId });
            Apply();
            _fleet._learners.push_back(this);
        }

        LearnerWorker(LearnerWorker const&) = delete;
        LearnerWorker(LearnerWorker&&) = delete;
        LearnerWorker& operator=(LearnerWorker const&) = delete;
        LearnerWorker& operator=(LearnerWorker&&) = delete;

        ~LearnerWorker()
        {
            std::erase(_fleet._learners, this);
        }

        /// Apply what the leader's state says NOW, as a learner's consensus tier hands its roster
        /// every committed state, and hear from it as a leader its state counts: what `Step` does for
        /// every learner, which this harness never cuts off.
        void Apply()
        {
            _roster->Applied(_fleet.NodeAt(_leader).cluster.state);
            _roster->ConsensusPass(
                Distributed::LeaderReading { .leads = false, .leader = _leader, .silentFor = std::chrono::seconds { 0 } });
        }

        /// What this worker's compile port answers @p token with.
        /// @param token The grant a client presents.
        /// @param fingerprint The toolchain the client asks for.
        /// @return The refusal, or nothing when the grant is honoured.
        [[nodiscard]] std::optional<Distributed::LeaseRefusal> Check(std::string_view token, std::string_view fingerprint)
        {
            return _validator(token, fingerprint).refusal;
        }

        /// What this learner's session surface answers a machine ticket with.
        /// @param credential The ticket a launcher presents.
        /// @return The machine it speaks for, or why it is refused.
        [[nodiscard]] std::expected<ProvenIdentity, Distributed::TicketRefusal> CheckTicket(Cc::Credential const& credential)
        {
            auto const verifier = Distributed::TicketVerifier { _roster->Lease(), _audience, _spent };
            return verifier.Verify(CompileCacheWire::AsBytes(credential.secret.View()), _fleet._wallClock.now())
                .transform_error([](Distributed::TicketRejection const& rejection) { return rejection.Reason(); });
        }

        /// @return The roster this worker applied, summarised.
        [[nodiscard]] Distributed::RosterSummary Roster() const
        {
            return _roster->Summary().value_or(Distributed::RosterSummary {});
        }

      private:
        /// Where this worker answers: fixed, since nothing here moves an address.
        struct Advertised final: Cc::IAdvertisedEndpointSource
        {
            explicit Advertised(std::string at):
                endpoint { std::move(at) }
            {
            }

            [[nodiscard]] std::string Current() const override
            {
                return endpoint;
            }

            std::string endpoint;
        };

        /// The production roster for a learner of @p leader's fleet: a node that runs consensus,
        /// so its roster is the state it applies and nothing it was handed -- stamping leader
        /// contact on @p clock, the harness's own.
        [[nodiscard]] static std::unique_ptr<FastCache::Node::NodeRoster> BuildRoster(std::string const& endpoint,
                                                                                      std::string const& leader,
                                                                                      core::platform::IClock const& clock)
        {
            auto cfg = FastCache::Node::NodeConfig {};
            cfg.nodeId = endpoint;
            cfg.formation = FastCache::Node::NodeFormationView {
                .mode = Cluster::NodeMode::Learner,
                .clusterId = std::string { ClusterId },
                .createdAtUnixSeconds = 0,
                .foundedHere = false,
                .fleetMembers = { Cluster::ClusterMember { .id = leader,
                                                           .raftEndpoint = leader,
                                                           .schedulerEndpoint = leader,
                                                           .schedulerEndpointHistory =
                                                               Cluster::SchedulerEndpointHistory::Announced,
                                                           .seat = Cluster::MemberSeat::Voter,
                                                           .publicKey = TestKeyPair(leader).PublicKey() } },
                .fleetSchedulers = { leader },
            };
            if (!FastCache::Node::RunsConsensus(cfg))
                throw std::runtime_error { "FleetHarness: a learner's configuration runs no consensus" };
            auto built = FastCache::Node::NodeRoster::Build(cfg, clock, nullptr);
            if (!built.has_value())
                throw std::runtime_error { "FleetHarness: a learner's roster could not be built: " + built.error().reason };
            return *std::move(built);
        }

        FleetHarness& _fleet;
        Advertised _advertised;
        std::string _endpoint;
        std::string _leader;
        std::unique_ptr<FastCache::Node::NodeRoster> _roster; ///< Declared before `_validator`, which borrows it.
        Distributed::WorkerLeaseState _lease;
        Cc::LeaseValidator _validator;
        ExactAudience _audience { _endpoint };
        Distributed::SpentTickets _spent;
    };

    /// Dial a scheduler in this fleet, as a node's presence round would.
    /// @param endpoint Which scheduler.
    /// @param options Ignored; nothing here blocks.
    /// @return A connection answering from that scheduler, or null when it is unreachable or
    ///         nobody was added there.
    [[nodiscard]] std::unique_ptr<core::net::ISocket> Dial(std::string_view endpoint,
                                                           core::net::DialOptions options) override
    {
        (void) options;
        if (std::ranges::contains(_unreachable, endpoint)
            || std::ranges::none_of(_nodes, [endpoint](auto const& node) { return node->endpoint == endpoint; }))
            return nullptr;
        return std::make_unique<AnsweringSocket>(*this, std::string { endpoint });
    }

    /// Answer one exchange, from whichever endpoint it was addressed to.
    /// @param hostPort The endpoint the client chose. **This is the fact most of
    ///        these tests are about**: which machine the client decided to ask.
    /// @param frame The complete request.
    /// @param credential What the client presents: a machine ticket to a scheduler, or to a worker
    ///        address `VerifyTicketsAtWorker` named, is decided by that machine's verifier (see the
    ///        class comment); presented to a learner, it is refused by throwing, since a learner
    ///        endpoint models no `AUTH` (`AddRedirectingLearner`); anything else is answered `Ok` and
    ///        admits nothing, as a surface with nothing to verify answers it.
    /// @param budget Ignored; nothing here blocks.
    /// @return The outcome, decoded by the launcher's own client code.
    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& credential,
                                            Cc::ExchangeBudget budget) override
    {
        (void) budget;
        // A learner endpoint models no AUTH (see `AddRedirectingLearner`): refused loudly, BEFORE the
        // call is logged, rather than answered `Ok` by `AnswerAuthenticated`'s fallback, which would
        // verify nothing in silence.
        if (credential.Configured() && FindRedirectingLearner(hostPort) != nullptr)
            throw std::runtime_error { "FleetHarness: a learner endpoint models no AUTH, and " + std::string { hostPort }
                                       + " was presented a credential" };
        // Logged BEFORE the answer, so the log is in the order requests were SENT --
        // an exchange nested inside this one's compile hook would otherwise appear
        // to have happened first. Its outcome is filled in below, by index rather
        // than by reference, because that nested call can reallocate the vector.
        auto const slot = _calls.size();
        _calls.push_back(Call { .endpoint = std::string { hostPort },
                                .opRaw = OpOf(frame),
                                .kind = Cc::CacheOutcomeKind::Transport,
                                .code = CompileCacheWire::ErrorCode::MalformedFrame });

        if (std::ranges::contains(_unreachableWorkers, hostPort))
            // Recorded as the Transport it is, with nothing answered: `CacheOutcome {}` is
            // the seeded "nothing was reached", which is `Unreached`.
            return Cc::CacheOutcome {};

        auto reply = credential.Configured() ? AnswerAuthenticated(hostPort, frame, credential) : Answer(hostPort, frame);
        // The launcher's own framing, over a socket that replays what the addressed
        // scheduler actually produced. Asserting against hand-written reply bytes
        // would let the two ends drift apart independently, which is the failure
        // #340 was.
        ScriptedSocket socket { std::move(reply) };
        auto outcome = core::async::syncRun(Cc::ExchangeFramed(&socket, &Unwatched(), std::move(frame), credential));

        _calls[slot].kind = outcome.kind;
        _calls[slot].code = outcome.code;
        return outcome;
    }

  private:
    /// One scheduler and everything it owns.
    struct Node
    {
        /// @param harness The fleet it belongs to, for the shared clocks and sink.
        /// @param at How clients address it.
        Node(FleetHarness& harness, std::string at):
            endpoint { std::move(at) },
            // Its endpoint is its member id, and its test key the one it signs every grant
            // with (#178).
            signer { endpoint, TestKeyPair(endpoint) },
            service { harness._clock,
                      harness._wallClock,
                      harness._metrics,
                      harness._logger,
                      signer,
                      // One fleet, so one cluster id across every node the harness
                      // builds: a grant minted by any of them must verify on any
                      // other, which is what the harness exists to exercise (#322).
                      FleetHarness::ClusterId },
            protocol { service, harness._metrics },
            roster { harness._clock }
        {
            service.AdministerWith(cluster);
        }

        std::string endpoint;
        Distributed::KeyPairLeaseSigner signer;
        /// The state this node applied; declared before `service`, which borrows it.
        struct AppliedState final: Distributed::IClusterAdmin
        {
            Cluster::ClusterState state;

            [[nodiscard]] Cluster::ClusterState ClusterState() const override
            {
                return state;
            }

            /// What the service noted, in order: the member that PROVED it and the endpoint it
            /// announced. Recorded rather than proposed -- the harness runs no consensus -- so a case
            /// can assert WHICH scheduler a member's announcement reached (`AnnouncedEndpointsAt`).
            std::vector<std::pair<std::string, std::string>> noted;

            /// @copydoc Distributed::IClusterAdmin::NoteAnnouncedEndpoint
            void NoteAnnouncedEndpoint(Consensus::NodeId const& member, std::string endpoint) override
            {
                noted.emplace_back(member, std::move(endpoint));
            }

            [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& /*command*/) override
            {
                return std::unexpected { ConsensusError { .code = ConsensusErrorCode::NotLeader,
                                                          .context = "the harness proposes nothing",
                                                          .knownLeader = std::nullopt } };
            }
        } cluster;
        Distributed::SchedulerService service;
        Distributed::SchedulerProtocol protocol;
        /// The fold every node composes when it is not open: this machine, and a key roster of the
        /// state it applied, published at every commit (`Commit`). The default decider of its
        /// callers -- production's participants, so no caller is admitted that a node would refuse.
        Distributed::LoopbackMembership loopback;
        Distributed::KeyRosterMembership keys;
        Distributed::AnyOfMembership fold { { &loopback, &keys } };
        /// Who decides this node's callers: `fold` unless a case set another. Borrowed -- see
        /// `SetMembershipAt`. Never null, and never "everybody, proven".
        Distributed::IMembershipOracle const* membership { &fold };
        /// The production policy `membership` is, when `PublishMembershipAt` set one; republished
        /// whenever this node's state changes.
        FastCache::Node::NodeMembership* published { nullptr };
        /// What this node verifies a machine ticket against: its own applied state, adopted at
        /// every COMMIT and kept current by every consensus pass (`Step`), as production's
        /// consensus tier feeds it -- on the harness's clock, which a case advances; a spent set of
        /// its own; and the one endpoint it answers to.
        Distributed::StateLeaseRoster roster;
        Distributed::SpentTickets spent;
        ExactAudience audience { endpoint };
        Distributed::TicketVerifier verifier { &roster, audience, spent };
    };

    /// What a COMMIT on @p node hands its consumers, as production's apply callback does: its lease
    /// roster adopts the state, its key roster and any published policy are republished, and every
    /// ticket-verifying worker whose state is this node's adopts it too.
    ///
    /// **At a commit and never per exchange** (W-8): a roster re-adopted at every exchange cannot
    /// show a consumer that missed a publish, which is the regression this harness must not hide.
    /// @param node Which node committed.
    void Commit(Node& node)
    {
        node.roster.Adopt(node.cluster.state);
        PublishKeys(node.keys, node.cluster.state);
        if (node.published != nullptr)
            node.published->PublishCluster(node.cluster.state);
        for (auto const& worker: _ticketedWorkers)
            if (worker->stateOf == node.endpoint)
                AdoptAt(*worker, node);
        NoteReadings(node);
    }

    /// Publish @p state's live and revoked keys into @p keys, as a node's key roster is published at
    /// every commit.
    /// @param keys The roster.
    /// @param state What the node applied.
    static void PublishKeys(Distributed::KeyRosterMembership& keys, Cluster::ClusterState const& state)
    {
        std::map<std::string, Ed25519PublicKey, std::less<>> live;
        for (auto const& member: state.members)
            live.emplace(member.id, member.publicKey);
        std::vector<Ed25519PublicKey> revoked;
        revoked.reserve(state.revokedKeys.size());
        for (auto const& key: state.revokedKeys)
            revoked.push_back(key.publicKey);
        keys.Publish(std::move(live), std::move(revoked));
    }

    /// A learner's `0xFC` surface and everything it owns; see `AddRedirectingLearner`.
    ///
    /// Declaration order is construction order, and every member below borrows one above it.
    struct RedirectingLearner
    {
        /// @param at How clients address it, and its node id.
        /// @param clock The harness's clock, which its shared cache and live-stats loop read.
        /// @param wallClock The harness's wall clock, which its session component reads.
        /// @param logger Where its components log.
        RedirectingLearner(std::string at,
                           core::platform::ManualClock& clock,
                           core::platform::ManualWallClock& wallClock,
                           ILogger& logger):
            endpoint { std::move(at) },
            cfg { LearnerConfig(endpoint) },
            identity { TestKeyPair(endpoint) },
            loop { clock },
            liveStats { liveSources, fold, AdminCredential {}, loop, metrics },
            session { verifier, FastCache::Node::SessionKeys {}, random, wallClock, metrics },
            sharedCache { cfg, fold, clock, metrics, logger, nullptr, FastCache::Node::ReconcileOn::Caller },
            nodeProof { cfg.nodeId, identity, fold, consensusStanding, random, metrics, logger },
            surface { FastCache::Node::ComposeSurfaceComponents(
                nullptr,
                nullptr,
                &redirect,
                nullptr,
                nodeStatus,
                std::unexpected { FastCache::Node::EnrollmentAbsenceOf(cfg, false)
                                      .value_or(FastCache::Node::EnrollmentAbsence::NoIdentityKey) },
                liveStats,
                fleetText,
                &nodeProof,
                &formation,
                session,
                sharedCache) }
        {
        }

        /// A learner's configuration: the mode its formation record holds, and the predicate `main`
        /// asks before it builds the redirect -- refused here when it would not.
        /// @param nodeId Its id.
        /// @return The configuration.
        /// @throws std::runtime_error when the configuration would not redirect the scheduling verbs.
        [[nodiscard]] static FastCache::Node::NodeConfig LearnerConfig(std::string const& nodeId)
        {
            auto cfg = FastCache::Node::NodeConfig {};
            cfg.nodeId = nodeId;
            cfg.formation = FastCache::Node::NodeFormationView {
                .mode = Cluster::NodeMode::Learner,
                .clusterId = std::string { ClusterId },
                .createdAtUnixSeconds = 0,
                .foundedHere = false,
                .fleetMembers = {},
                .fleetSchedulers = {},
            };
            if (!FastCache::Node::RedirectsScheduling(cfg))
                throw std::runtime_error { "FleetHarness: a learner's configuration does not redirect scheduling" };
            return cfg;
        }

        std::string endpoint;
        FastCache::Node::NodeConfig cfg; ///< What `main` would have built it from.
        Ed25519KeyPair identity;         ///< Its identity key: what its node-proof and formation answers sign.
        /// Its own sink, standing for the one sink `main` hands every component on a node's surface.
        ///
        /// **Every component on this surface that holds a sink is handed THIS one** -- the redirect
        /// included, should it ever count -- so a case asserting that the learner counted nothing
        /// reads every counter the surface could move. The redirect, the router and the endpoint's
        /// header gate count nothing on the scheduling path today.
        AtomicMetricsSink metrics;
        /// The fold a node composes when it is not open -- this machine, and a key roster of the
        /// state it applied (`Hear`) -- so a launcher on the learner's own machine is a member. Every
        /// component asks it, as every component of a node asks `membership.Oracle()`.
        Distributed::LoopbackMembership loopback;
        Distributed::KeyRosterMembership keys;
        Distributed::AnyOfMembership fold { { &loopback, &keys } };
        /// What the redirect names, written only through `publisher`.
        FastCache::Node::KnownSchedulingLeader knownLeader;
        FastCache::Node::SchedulingLeaderPublisher publisher { knownLeader };
        FastCache::Node::SchedulingRedirectResponder redirect { fold, knownLeader };
        /// The every-node owners, built as `main` builds them over a source slot nothing attaches --
        /// each answers that it has nothing to read. Built for their place on the surface, whose
        /// request cap is their fold.
        LiveStatsSourceSlot liveSources;
        SilentNodeStatus describe;
        FixedStanding const standing {};
        FastCache::Node::NodeStatusResponder nodeStatus { describe, liveSources, fold, standing, metrics };
        /// What the live-stats owner is bound to; never turned, since no case here subscribes.
        core::net::testing::TestLoop loop;
        FastCache::Node::LiveStatsResponder liveStats;
        FastCache::Node::FleetTextResponder fleetText { liveSources, fold, AdminCredential {}, metrics };
        /// The session owner, over no roster: every ticket it is shown is refused -- counted, on `metrics`.
        ExactAudience audience { endpoint };
        Distributed::SpentTickets spent;
        Distributed::TicketVerifier verifier { nullptr, audience, spent };
        ScriptedSecureRandom random;
        FastCache::Node::SessionResponder session;
        /// The fleet's shared cache every node builds, dormant: nothing names this machine.
        FastCache::Node::SharedCacheService sharedCache;
        /// The identity prover a node running consensus serves, over a standing slot nothing attaches.
        FastCache::Node::ConsensusStandingSlot consensusStanding;
        FastCache::Node::NodeProofResponder nodeProof;
        /// What it answers `FLEET-SUMMARY` with, signed under `identity`.
        FastCache::Node::FixedFleetSummary const summary { CompileCacheWire::FleetSummary {
            .clusterId = std::string { ClusterId }, .nodeId = endpoint } };
        FastCache::Node::FleetSummaryResponder formation { summary, identity };
        /// Production's composition of all of the above, routed by production's router.
        FastCache::Node::MergedResponder surface;
    };

    /// What @p learner's consensus tier tells it at a pass: who leads -- the harness's leader, whose
    /// `0xFC` endpoint is its endpoint here, as a member record's `schedulerEndpoint` is -- and that
    /// it was heard just now; and the state that leader applied, which its key roster publishes.
    /// @param learner Which learner.
    void Hear(RedirectingLearner& learner)
    {
        learner.publisher.LeaderChanged(_leader.value_or(std::string {}));
        learner.publisher.LeaderContact(
            Distributed::LeaderReading { .leads = false, .leader = _leader, .silentFor = std::chrono::seconds { 0 } },
            Consensus::RaftConfig {}.electionTimeoutMax);
        if (_leader.has_value())
            PublishKeys(learner.keys, NodeAt(*_leader).cluster.state);
    }

    /// The learner at @p endpoint, or null when none was added there.
    /// @param endpoint Who to find.
    /// @return The learner, or null.
    [[nodiscard]] RedirectingLearner* FindRedirectingLearner(std::string_view endpoint) const
    {
        auto const found = std::ranges::find(
            _redirectingLearners, endpoint, [](auto const& learner) { return std::string_view { learner->endpoint }; });
        return found != _redirectingLearners.end() ? found->get() : nullptr;
    }

    /// The learner at @p endpoint.
    /// @param endpoint Who to find.
    /// @return The learner.
    /// @throws std::runtime_error when nothing was added there, for `NodeAt`'s reason.
    [[nodiscard]] RedirectingLearner& RedirectingLearnerAt(std::string_view endpoint) const
    {
        auto* const learner = FindRedirectingLearner(endpoint);
        if (learner == nullptr)
            throw std::runtime_error { "FleetHarness: no learner at " + std::string { endpoint } };
        return *learner;
    }

    /// What @p learner's surface answers @p frame with, through the gate its endpoint asks of every
    /// header -- production's `Node::DecideHeaderRefusal`: the surface-wide cap, admission, the
    /// per-verb ceiling, the in-flight budget -- and `Answer` only for a request it lets through. The
    /// cap is the surface's own `MaxRequestBytes()`, the fold over every owner composed, which the
    /// endpoint reads.
    ///
    /// The in-flight budget is fixed at 0 rather than read, as `FormationHarness::Gate` fixes it: the
    /// harness answers one request at a time to completion, so nothing else is ever in flight on this
    /// surface. The peer carries no authenticated machine because `Exchange` refuses to present a
    /// credential to a learner (`AddRedirectingLearner`), so no `AUTH` ever established one.
    /// @param learner Which learner.
    /// @param frame The request.
    /// @return The reply; empty, which closes, for a frame with no header.
    [[nodiscard]] std::vector<std::byte> AnswerAtRedirectingLearner(RedirectingLearner& learner,
                                                                    std::span<std::byte const> frame)
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(frame);
        if (!header.has_value())
            return {};
        auto const peer = ConnectionFacts { .host = _callerHost, .proven = _callerIdentity };
        auto refusal = FastCache::Node::DecideHeaderRefusal(
            FastCache::Node::HeaderGate { .responder = learner.surface, .what = "node port", .inFlightBytes = 0 },
            peer,
            *header,
            learner.surface.MaxRequestBytes());
        if (refusal.has_value())
            return std::move(refusal->reply);
        return core::async::syncRun(learner.surface.Answer(frame, peer)).bytes;
    }

    /// Refuse a second answerer at @p endpoint: a scheduler or a learner already there.
    /// @param endpoint The endpoint about to be added.
    /// @param what What is being added, for the message.
    /// @throws std::runtime_error when one is.
    void RefuseTaken(std::string_view endpoint, std::string_view what) const
    {
        if (FindRedirectingLearner(endpoint) != nullptr
            || std::ranges::any_of(_nodes, [endpoint](auto const& node) { return node->endpoint == endpoint; }))
            throw std::runtime_error { std::format(
                "FleetHarness: {} at {} would share the endpoint with a node already there", what, endpoint) };
    }

    /// Refuse a worker at a learner's endpoint, which `Answer` would never reach.
    /// @param endpoint The worker's endpoint.
    /// @throws std::runtime_error when a learner answers there.
    void RefuseWorkerAtLearner(std::string_view endpoint) const
    {
        if (FindRedirectingLearner(endpoint) != nullptr)
            throw std::runtime_error { std::format("FleetHarness: a worker at {} would be shadowed by the learner there",
                                                   endpoint) };
    }

    /// One consensus pass's reading on @p node: it leads, or it follows the harness's leader.
    /// @param node Which node.
    void NoteReadings(Node& node)
    {
        auto const leads = _leader.has_value() && *_leader == node.endpoint;
        node.roster.NoteLeaderReading(Distributed::LeaderReading {
            .leads = leads, .leader = leads ? std::nullopt : _leader, .silentFor = std::chrono::seconds { 0 } });
        for (auto const& worker: _ticketedWorkers)
            if (worker->stateOf == node.endpoint)
                worker->roster.NoteLeaderReading(Distributed::LeaderReading {
                    .leads = false, .leader = worker->stateOf, .silentFor = std::chrono::seconds { 0 } });
    }

    /// The replies to AUTH presenting @p credential and then @p frame, from @p hostPort.
    ///
    /// A machine ticket to a scheduler is verified by that node, and its outcome decides the
    /// command's caller through the production fold; the AUTH reply is `Ok` or the counted refusal
    /// (`Distributed::AnswerTicket`). Anything else is answered as production's session surface
    /// answers a credential that is not a ticket -- `NoPolicy`: `Ok`, ESTABLISHING NOTHING, so the
    /// command's caller is the fold's verdict on the connection alone.
    /// @param hostPort The addressed endpoint.
    /// @param frame The command.
    /// @param credential What was presented.
    /// @return AUTH's reply, then the command's.
    [[nodiscard]] std::vector<std::byte> AnswerAuthenticated(std::string_view hostPort,
                                                             std::span<std::byte const> frame,
                                                             Cc::Credential const& credential)
    {
        auto const ticketed = std::ranges::find(
            _ticketedWorkers, hostPort, [](auto const& worker) { return std::string_view { worker->address }; });
        if (credential.kind == CompileCacheWire::AuthKind::MachineTicket && ticketed != _ticketedWorkers.end())
            return AnswerTicketedWorker(**ticketed, hostPort, frame, credential);

        auto const scheduler =
            std::ranges::find_if(_nodes, [hostPort](auto const& node) { return node->endpoint == hostPort; });
        if (credential.kind != CompileCacheWire::AuthKind::MachineTicket || scheduler == _nodes.end())
        {
            auto replies = CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok, {});
            auto const command = Answer(hostPort, frame);
            replies.insert(replies.end(), command.begin(), command.end());
            return replies;
        }

        // The roster is whatever this node's last COMMIT and pass left it holding -- never adopted
        // here, which would make every exchange a fresh apply.
        auto& node = **scheduler;
        auto const answer = Distributed::AnswerTicket(
            _metrics, node.verifier.Verify(CompileCacheWire::AsBytes(credential.secret.View()), _wallClock.now()));
        auto replies = answer.machine.has_value() ? CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok, {})
                                                  : answer.refusalReply;
        auto const context = Distributed::CallerContextOf(*node.membership,
                                                          ConnectionFacts { .host = _callerHost,
                                                                            .proven = _callerIdentity,
                                                                            .authenticatedMachine = answer.machine,
                                                                            .revokedMachine = answer.revoked });
        auto const command = node.protocol.Answer(frame, context);
        replies.insert(replies.end(), command.begin(), command.end());
        return replies;
    }

    /// A worker that verifies machine tickets; see `VerifyTicketsAtWorker`.
    struct TicketedWorker
    {
        /// @param clock The harness's clock, which its roster stamps leader contact on.
        explicit TicketedWorker(core::platform::IClock const& clock):
            roster { clock }
        {
        }

        std::string address;                       ///< Where a compile dials it.
        Distributed::IAudience const* audience {}; ///< What it answers to; borrowed.
        std::string stateOf;                       ///< Whose applied state its roster holds.
        Distributed::StateLeaseRoster roster;      ///< Adopted at that node's every commit (`Commit`).
        Distributed::SpentTickets spent;           ///< Its own: a node spends each ticket once.
    };

    /// @p worker adopts @p node's applied state, and takes a pass's reading of it as its leader.
    /// @param worker The ticket-verifying worker.
    /// @param node The node whose state it holds.
    static void AdoptAt(TicketedWorker& worker, Node const& node)
    {
        worker.roster.Adopt(node.cluster.state);
        worker.roster.NoteLeaderReading(Distributed::LeaderReading {
            .leads = false, .leader = worker.stateOf, .silentFor = std::chrono::seconds { 0 } });
    }

    /// The replies to AUTH presenting @p credential and then @p frame, from a worker that verifies
    /// tickets: the counted refusal or `Ok`, then whatever the address answers the command with.
    /// @param worker Which worker.
    /// @param hostPort The addressed endpoint.
    /// @param frame The command.
    /// @param credential The machine ticket presented.
    /// @return AUTH's reply, then the command's.
    [[nodiscard]] std::vector<std::byte> AnswerTicketedWorker(TicketedWorker& worker,
                                                              std::string_view hostPort,
                                                              std::span<std::byte const> frame,
                                                              Cc::Credential const& credential)
    {
        auto const verifier = Distributed::TicketVerifier { &worker.roster, *worker.audience, worker.spent };
        auto const answer = Distributed::AnswerTicket(
            _metrics, verifier.Verify(CompileCacheWire::AsBytes(credential.secret.View()), _wallClock.now()));
        auto replies = answer.machine.has_value() ? CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok, {})
                                                  : answer.refusalReply;
        auto const command = Answer(hostPort, frame);
        replies.insert(replies.end(), command.begin(), command.end());
        return replies;
    }

    /// The scheduler at @p endpoint.
    /// @param endpoint Who to find.
    /// @return The node.
    /// @throws std::runtime_error when nothing was added at that endpoint — a
    ///         silent miss would make a routing test pass for the wrong reason.
    [[nodiscard]] Node& NodeAt(std::string_view endpoint)
    {
        for (auto const& node: _nodes)
            if (node->endpoint == endpoint)
                return *node;
        throw std::runtime_error { "FleetHarness: no scheduler at " + std::string { endpoint } };
    }

    /// The reply bytes for one request, from whoever it was addressed to.
    /// @param hostPort The addressed endpoint.
    /// @param frame The request.
    /// @return What that endpoint answers.
    [[nodiscard]] std::vector<std::byte> Answer(std::string_view hostPort, std::span<std::byte const> frame)
    {
        if (auto* const learner = FindRedirectingLearner(hostPort); learner != nullptr)
            return AnswerAtRedirectingLearner(*learner, frame);

        auto const worker = std::ranges::find(_workerEndpoints, hostPort);
        if (worker == _workerEndpoints.end())
        {
            auto& node = NodeAt(hostPort);
            return node.protocol.Answer(frame, Caller(node));
        }

        // A worker endpoint. The hook fires here rather than around the whole
        // Dispatch call because this is the only instant that is *between* the
        // grant and the release.
        if (_onCompile)
        {
            auto const hook = std::exchange(_onCompile, {});
            hook();
        }
        if (auto const own = _workerReplyAt.find(hostPort); own != _workerReplyAt.end())
            return own->second;
        return _workerReply;
    }

    /// The context the harness ARRANGES the world with: a member on loopback, always.
    ///
    /// Separate from `Caller` deliberately. Setup helpers that throw on refusal -- registering a
    /// worker, for one -- must not be subject to the membership a case is testing, or arranging
    /// the fleet fails before the assertion runs.
    /// @return A member calling from loopback.
    [[nodiscard]] static Distributed::CallerContext SetupCaller()
    {
        // The key it proved too, as `CallerContextOf` engages both together: a worker registered
        // through here is named in its grants by `TestKeyPair(ProvenMachine)`, which is the key a
        // reply `SignedWorkerReply` builds is signed under (W-4).
        return Distributed::CallerContext { .membership = Distributed::Membership::Member,
                                            .peerId = "127.0.0.1",
                                            .provenNodeId = std::string { ProvenMachine },
                                            .provenKey = TestKeyPair(std::string { ProvenMachine }).PublicKey(),
                                            .identified = true,
                                            .operatorStanding = true };
    }

    /// The context handed to @p node's service for the current caller.
    ///
    /// **Decided by that node's oracle, always**, through `CallerContextOf` exactly as production's
    /// endpoint decides it: the caller's host, and the identity `SetCallerIdentity` says its
    /// connection proved. Before #1471 this returned a hardcoded `Member`, and until W-8 a node
    /// with no oracle still made every caller a PROVEN, IDENTIFIED member -- both more permissive
    /// than any node, so a fold or a publish regression stayed green here.
    ///
    /// A caller proves nothing unless the case says it did: this harness runs no handshake (the
    /// handshake and the seal are a socket's, held to their rules by `FrameEndpoint_test` over a
    /// real one), so the proof a node's presence round makes is the case's `SetCallerIdentity`.
    /// @param node Whose oracle to ask.
    /// @return The context, carrying that node's verdict about this connection.
    [[nodiscard]] Distributed::CallerContext Caller(Node const& node) const
    {
        return Distributed::CallerContextOf(*node.membership,
                                            ConnectionFacts { .host = _callerHost, .proven = _callerIdentity });
    }

    /// The verb byte a framed request carries, for the call log.
    ///
    /// Raw rather than an `Op`, because a frame this build does not know is
    /// exactly the thing worth seeing in a log of what a client sent. A test
    /// compares against `static_cast<std::uint8_t>(Wire::Op::Lease)`.
    /// @param frame The request.
    /// @return Its opcode byte, or `0xFF` when the frame carries no header.
    [[nodiscard]] static std::uint8_t OpOf(std::span<std::byte const> frame)
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(frame);
        return header.has_value() ? header->opRaw : std::uint8_t { 0xFF };
    }

    /// A connection whose answer is computed from what was written to it, by the scheduler it
    /// was dialled at -- `ScriptedSocket`'s replay, with the reply decided when it is first read.
    class AnsweringSocket final: public core::net::ISocket
    {
      public:
        AnsweringSocket(FleetHarness& fleet, std::string endpoint):
            _fleet { fleet },
            _endpoint { std::move(endpoint) }
        {
        }

        [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> bytes) override
        {
            _sent.insert(_sent.end(), bytes.begin(), bytes.end());
            return core::net::IoAwaitable { core::net::IoResult { bytes.size() } };
        }

        [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                           std::shared_ptr<void const> /*keepAlive*/ = {}) override
        {
            std::size_t total = 0;
            for (auto const& segment: segments)
            {
                _sent.insert(_sent.end(), segment.begin(), segment.end());
                total += segment.size();
            }
            return core::net::IoAwaitable { core::net::IoResult { total } };
        }

        [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
        {
            if (!_answered)
            {
                _answered = true;
                _fleet._calls.push_back(Call { .endpoint = _endpoint,
                                               .opRaw = OpOf(_sent),
                                               .kind = Cc::CacheOutcomeKind::Hit,
                                               .code = CompileCacheWire::ErrorCode::MalformedFrame });
                _reply = _fleet.Answer(_endpoint, _sent);
            }
            auto const take = std::min(_reply.size() - _cursor, buffer.size());
            std::copy_n(_reply.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
            _cursor += take;
            return core::net::IoAwaitable { core::net::IoResult { take } };
        }

        void close() noexcept override
        {
            _closed = true;
        }

        [[nodiscard]] bool isClosed() const noexcept override
        {
            return _closed;
        }

      private:
        FleetHarness& _fleet;
        std::string _endpoint;
        std::vector<std::byte> _sent;
        std::vector<std::byte> _reply;
        std::size_t _cursor { 0 };
        bool _answered { false };
        bool _closed { false };
    };

    core::platform::ManualClock _clock;
    core::platform::ManualWallClock _wallClock;
    AtomicMetricsSink _metrics;
    NullLogger _logger;
    /// Schedulers a dial does not reach; see `SetUnreachable`.
    std::vector<std::string> _unreachable;
    /// Where requests appear to come from. Loopback by default, which is what every case
    /// predating #1471 assumed and what a node always admits.
    std::string _callerHost { "127.0.0.1" };
    /// What requests' connections proved; nothing by default. See `SetCallerIdentity`.
    std::optional<ProvenIdentity> _callerIdentity;
    /// Who `ElectLeader` made leader, and what every node's consensus pass reports.
    std::optional<std::string> _leader;
    std::vector<std::unique_ptr<Node>> _nodes;
    /// Every learner surface `AddRedirectingLearner` added, each hearing its leader at every `ElectLeader` and
    /// `Step`.
    std::vector<std::unique_ptr<RedirectingLearner>> _redirectingLearners;
    /// Every live `LearnerWorker`, each re-adopting its state at every `Step`. Borrowed: a learner
    /// registers itself and leaves when it is destroyed.
    std::vector<LearnerWorker*> _learners;
    std::vector<std::string> _workerEndpoints;
    /// Worker addresses a compile dial does not reach; see `SetWorkerUnreachable`.
    std::vector<std::string> _unreachableWorkers;
    /// Per-address COMPILE replies, overriding `_workerReply`; see `AddWorkerAddress`.
    std::map<std::string, std::vector<std::byte>, std::less<>> _workerReplyAt;
    /// Worker addresses that verify a presented ticket; see `VerifyTicketsAtWorker`.
    std::vector<std::unique_ptr<TicketedWorker>> _ticketedWorkers;
    std::vector<std::byte> _workerReply;
    std::vector<Call> _calls;
    std::function<void()> _onCompile;
};

} // namespace FastCache::Testing
