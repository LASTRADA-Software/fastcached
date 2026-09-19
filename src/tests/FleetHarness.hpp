// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <Dispatch.hpp>
#include <WorkerProtocol.hpp>
#include <apps/fastcache-compile-node/EndpointDialer.hpp>
#include <apps/fastcache-compile-node/NodeConfig.hpp>
#include <apps/fastcache-compile-node/NodeCredential.hpp>
#include <apps/fastcache-compile-node/NodePresenceTier.hpp>
#include <apps/fastcache-compile-node/NodeRoster.hpp>
#include <apps/fastcache-compile-node/SchedulerLink.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScriptedSocket.hpp>
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
/// `SetWorkerReply` was given.
///
/// Time moves only in `Step`.
///
/// ## Rosters (#178)
///
/// Every scheduler signs its grants with its own identity key -- `TestKeyPair` of its
/// endpoint, which is also its member id -- and holds a cluster state of its own, set by
/// `SetClusterStateAt`: an ex-leader that has not heard a change is a node holding an older
/// one. A voter's endorsement reaches a scheduler on NODE-ANNOUNCE (`EndorseAt`). A
/// `RosterWorker` is a machine with no consensus: a production `FastCache::Node::NodeRoster` rooted in
/// `--voter-key` anchors, the production lease validator over it, and a presence round that is
/// production's `FastCache::Node::AnnouncePresence` -- `SchedulerLink`'s redirects and fallbacks, dialled
/// through this harness (`SetUnreachable`), the certified roster adopted from whichever
/// scheduler answered.
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
    void AddScheduler(std::string endpoint)
    {
        auto node = std::make_unique<Node>(*this, std::move(endpoint));
        _nodes.push_back(std::move(node));
    }

    /// Make @p endpoint the leader and every other scheduler its follower.
    ///
    /// One call, both halves: a test that set only the new leader would leave the
    /// old one still answering as leader, which is a fleet no election produces
    /// and would let a wrong client pass.
    /// @param endpoint The scheduler that now leads; must have been added.
    void ElectLeader(std::string_view endpoint)
    {
        (void) NodeAt(endpoint); // refuse an endpoint nobody added, loudly
        for (auto const& node: _nodes)
            if (node->endpoint == endpoint)
                node->service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
            else
                node->service.SetRole(Distributed::SchedulerRole::Follower, endpoint, Distributed::StandaloneSchedulerTerm);
    }

    /// Where every subsequent request appears to come from.
    ///
    /// **A constant here cannot carry a membership property.** The default is `127.0.0.1`, and
    /// loopback is never forgotten -- deliberately, so a node always admits its own machine --
    /// so a forget published against the default caller applies to nobody and a case asserting
    /// a refusal goes green having exercised nothing. Any case about admission sets this first.
    /// @param host The caller's host, as `ISocket::PeerAddress()` would report it.
    void SetCallerHost(std::string host)
    {
        _callerHost = std::move(host);
    }

    /// Let one node's admission oracle decide what its callers are.
    ///
    /// **Per NODE, and that is the point rather than generality.** The state #1471 is about is
    /// two machines disagreeing about one host -- a forget committed on the leader and not yet
    /// applied on the second -- and one oracle shared across the fleet cannot express it at all.
    ///
    /// A node given no oracle keeps answering `Member`, so every case written before this still
    /// asserts what it always did. The oracle is borrowed, not owned: the production objects
    /// belong to the case, because a `NodeMembership` needs a config and a logger this harness
    /// has no business inventing.
    /// @param endpoint Which node; must have been added.
    /// @param oracle Who decides its callers, or nullptr to go back to admitting everybody.
    void SetMembershipAt(std::string_view endpoint, Distributed::IMembershipOracle const* oracle)
    {
        NodeAt(endpoint).membership = oracle;
    }

    /// Register a worker with one scheduler.
    /// @param scheduler Which scheduler's registry to put it in.
    /// @param workerEndpoint Where the worker answers compiles.
    /// @param fingerprint The toolchain it serves.
    /// @param slots How many jobs it will take at once.
    void RegisterWorker(std::string_view scheduler,
                        std::string_view workerEndpoint,
                        std::string_view fingerprint,
                        std::uint32_t slots = 1)
    {
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
        _clock.Advance(by);
        _wallClock.Advance(by);
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

    /// What a worker endpoint answers a COMPILE with.
    /// @param reply A complete reply frame.
    void SetWorkerReply(std::vector<std::byte> reply)
    {
        _workerReply = std::move(reply);
    }

    /// Every exchange the fleet has served, in order.
    /// @return The log.
    [[nodiscard]] std::vector<Call> const& Calls() const noexcept
    {
        return _calls;
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

    /// The cluster's state as @p scheduler has applied it.
    ///
    /// Per node, because the case #178 is about is two nodes disagreeing: an ex-leader that
    /// never heard the change revoking it holds the state from before.
    /// @param scheduler Which scheduler; must have been added.
    /// @param state What it applied.
    void SetClusterStateAt(std::string_view scheduler, Cluster::ClusterState state)
    {
        NodeAt(scheduler).cluster.state = std::move(state);
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

    /// @p voter endorses the roster @p state holds, now, and says so to @p scheduler on
    /// NODE-ANNOUNCE -- the wire field and the scheduler's decode, not a direct call.
    /// @param scheduler Who hears it.
    /// @param voter Who endorses; signs with its own test key.
    /// @param state The state @p voter applied.
    void EndorseAt(std::string_view scheduler, std::string const& voter, Cluster::ClusterState const& state)
    {
        auto const key = TestKeyPair(voter);
        auto const endorsement = Cluster::SignEndorsement(
            Cluster::RosterEndorsement { .clusterId = std::string { ClusterId },
                                         .version = state.rosterVersion,
                                         .rosterDigest = Cluster::DigestOfRoster(Cluster::ProjectRoster(state)),
                                         .notAfter = _wallClock.Now() + Cluster::RosterEndorsementLifetime,
                                         .endorser = voter,
                                         .signature = {} },
            [&key](std::span<std::byte const> message) { return key.Sign(message); });
        auto const encoded = Cluster::EncodeEndorsement(endorsement);
        auto const reply = NodeAt(scheduler).service.AnnounceNode(
            SetupCaller(),
            Distributed::NodePresence { .endpoint = voter,
                                        .version = "harness",
                                        .capacity = Distributed::NodeCapacity { .logicalCores = 8 },
                                        .load = Distributed::NodeLoad {},
                                        .conditions = std::nullopt,
                                        .endorsement = encoded });
        if (reply.status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a voter's announcement" };
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

    /// A machine that runs no consensus and verifies every grant against the roster its
    /// voters certify (#178). Production parts throughout; see the class comment.
    class RosterWorker final
    {
      public:
        /// @param fleet The harness it lives in, for the clocks, the sink and the dialer.
        /// @param endpoint Where it answers compiles, and the key its presence is filed under.
        /// @param anchors The voters whose keys it is started with (`--voter-key`).
        /// @param schedulers Its `--scheduler` list, in order.
        RosterWorker(FleetHarness& fleet,
                     std::string endpoint,
                     std::vector<std::string> const& anchors,
                     std::vector<std::string> schedulers):
            _fleet { fleet },
            _advertised { std::move(endpoint) },
            _roster { BuildRoster(fleet, anchors) },
            _link { Unwrap(FastCache::Node::SchedulerLink::For(std::move(schedulers))) },
            _lease { Distributed::SchedulerTermRegressionNotice::Silent() },
            _validator { Cc::SignedLeaseValidator(*_roster->Lease(), _advertised, fleet._wallClock, _lease, fleet._metrics) }
        {
            // Registered into the harness's fleet, as a completed REGISTER round pins it (#401).
            _lease.fleet.Pin(std::string { ClusterId });
        }

        /// One presence round: production's announcement, through this harness's dialer.
        /// @return Whether a scheduler recorded this machine.
        bool Announce()
        {
            auto const capacity = CompileCacheWire::CapacityFields {};
            auto const load = CompileCacheWire::LoadFields {};
            auto const endpoint = _advertised.Current();
            return FastCache::Node::AnnouncePresence(FastCache::Node::PresenceMessage { .endpoint = endpoint,
                                                                                        .capacity = capacity,
                                                                                        .load = load,
                                                                                        .credential = _credential,
                                                                                        .notice = Unwatched(),
                                                                                        .logger = _fleet._logger },
                                                     _roster.get(),
                                                     _link,
                                                     _fleet);
        }

        /// What this worker's compile port answers @p token with.
        /// @param token The grant a client presents.
        /// @param fingerprint The toolchain the client asks for.
        /// @return The refusal, or nothing when the grant is honoured.
        [[nodiscard]] std::optional<Distributed::LeaseRefusal> Check(std::string_view token, std::string_view fingerprint)
        {
            return _validator(token, fingerprint).refusal;
        }

        /// @return The roster this worker holds, summarised; nothing before the first.
        [[nodiscard]] std::optional<Distributed::RosterSummary> Roster() const
        {
            return _roster->Summary();
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

        /// No `--requirepass`: the harness's schedulers ask for none.
        struct NoCredential final: FastCache::Node::ICredentialSource
        {
            [[nodiscard]] Cc::Credential Current() const override
            {
                return {};
            }
        };

        /// The production roster for a worker trusting @p anchors, with no state directory.
        [[nodiscard]] static std::unique_ptr<FastCache::Node::NodeRoster> BuildRoster(
            FleetHarness& fleet, std::vector<std::string> const& anchors)
        {
            FastCache::Node::NodeConfig cfg;
            cfg.clusterId = std::string { ClusterId };
            for (auto const& voter: anchors)
                cfg.voterKeys.push_back(TestKeyPair(voter).PublicKey());
            auto built = FastCache::Node::NodeRoster::Build(cfg, fleet._wallClock, fleet._metrics, fleet._logger);
            if (!built.has_value())
                throw std::runtime_error { "FleetHarness: a worker's roster could not be built: " + built.error() };
            return *std::move(built);
        }

        FleetHarness& _fleet;
        Advertised _advertised;
        NoCredential _credential;
        std::unique_ptr<FastCache::Node::NodeRoster> _roster;
        FastCache::Node::SchedulerLink _link;
        Distributed::WorkerLeaseState _lease;
        Cc::LeaseValidator _validator;
    };

    /// Dial a scheduler in this fleet, as a node's presence round would.
    /// @param endpoint Which scheduler.
    /// @param options Ignored; nothing here blocks.
    /// @return A connection answering from that scheduler, or null when it is unreachable or
    ///         nobody was added there.
    [[nodiscard]] std::unique_ptr<ISocket> Dial(std::string_view endpoint, DialOptions options) override
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
    /// @param credential Ignored; see the class comment.
    /// @param budget Ignored; nothing here blocks.
    /// @return The outcome, decoded by the launcher's own client code.
    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& credential,
                                            Cc::ExchangeBudget budget) override
    {
        (void) budget;
        // Logged BEFORE the answer, so the log is in the order requests were SENT --
        // an exchange nested inside this one's compile hook would otherwise appear
        // to have happened first. Its outcome is filled in below, by index rather
        // than by reference, because that nested call can reallocate the vector.
        auto const slot = _calls.size();
        _calls.push_back(Call { .endpoint = std::string { hostPort },
                                .opRaw = OpOf(frame),
                                .kind = Cc::CacheOutcomeKind::Transport,
                                .code = CompileCacheWire::ErrorCode::MalformedFrame });

        auto reply = Answer(hostPort, frame);
        // The launcher's own framing, over a socket that replays what the addressed
        // scheduler actually produced. Asserting against hand-written reply bytes
        // would let the two ends drift apart independently, which is the failure
        // #340 was.
        ScriptedSocket socket { std::move(reply) };
        auto outcome = SyncRun(Cc::ExchangeFramed(&socket, &Unwatched(), std::move(frame), credential));

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
            protocol { service, harness._metrics }
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

            [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& /*command*/) override
            {
                return std::unexpected { ConsensusError { .code = ConsensusErrorCode::NotLeader,
                                                          .context = "the harness proposes nothing",
                                                          .knownLeader = std::nullopt } };
            }
        } cluster;
        Distributed::SchedulerService service;
        Distributed::SchedulerProtocol protocol;
        /// Who decides this node's callers, or null to admit everybody (the default, and what
        /// every case predating #1471 relies on). Borrowed -- see `SetMembershipAt`.
        Distributed::IMembershipOracle const* membership { nullptr };
    };

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
        return _workerReply;
    }

    /// The context the harness ARRANGES the world with: a member on loopback, always.
    ///
    /// Separate from `Caller` deliberately. Setup helpers that throw on refusal -- registering a
    /// worker, for one -- must not be subject to the membership a case is testing, or arranging
    /// the fleet fails before the assertion runs.
    /// @return A member calling from loopback.
    [[nodiscard]] static Distributed::CallerContext SetupCaller() noexcept
    {
        return Distributed::CallerContext { .membership = Distributed::Membership::Member, .peerId = "127.0.0.1" };
    }

    /// The context handed to @p node's service for the current caller.
    ///
    /// **Decided by that node's oracle when it has one**, so a membership property is
    /// falsifiable here at all: before #1471 this returned a hardcoded `Member`, which made any
    /// case about who is admitted pass against an oracle that refused nobody.
    ///
    /// No oracle means `Member`, which is what every case about dispatch, leases and release
    /// routing needs -- those are not about admission and would be testing nothing else if this
    /// started refusing them.
    /// @param node Whose oracle to ask.
    /// @return The context, carrying that node's verdict about `_callerHost`.
    [[nodiscard]] Distributed::CallerContext Caller(Node const& node) const
    {
        auto const verdict =
            node.membership != nullptr ? node.membership->Classify(_callerHost) : Distributed::Membership::Member;
        return Distributed::CallerContext { .membership = verdict, .peerId = _callerHost };
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
    class AnsweringSocket final: public ISocket
    {
      public:
        AnsweringSocket(FleetHarness& fleet, std::string endpoint):
            _fleet { fleet },
            _endpoint { std::move(endpoint) }
        {
        }

        [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
        {
            _sent.insert(_sent.end(), bytes.begin(), bytes.end());
            return IoAwaitable { IoResult { bytes.size() } };
        }

        [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                                std::shared_ptr<void const> /*keepAlive*/ = {}) override
        {
            std::size_t total = 0;
            for (auto const& segment: segments)
            {
                _sent.insert(_sent.end(), segment.begin(), segment.end());
                total += segment.size();
            }
            return IoAwaitable { IoResult { total } };
        }

        [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
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
            return IoAwaitable { IoResult { take } };
        }

        void Close() noexcept override
        {
            _closed = true;
        }

        [[nodiscard]] bool IsClosed() const noexcept override
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

    ManualClock _clock;
    ManualWallClock _wallClock;
    AtomicMetricsSink _metrics;
    NullLogger _logger;
    /// Schedulers a dial does not reach; see `SetUnreachable`.
    std::vector<std::string> _unreachable;
    /// Where requests appear to come from. Loopback by default, which is what every case
    /// predating #1471 assumed and what a node always admits.
    std::string _callerHost { "127.0.0.1" };
    std::vector<std::unique_ptr<Node>> _nodes;
    std::vector<std::string> _workerEndpoints;
    std::vector<std::byte> _workerReply;
    std::vector<Call> _calls;
    std::function<void()> _onCompile;
};

} // namespace FastCache::Testing
