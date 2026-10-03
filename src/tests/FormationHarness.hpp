// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/BeaconDestinations.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/DiscoveryService.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/EndpointDialer.hpp>
#include <apps/fastcache-compile-node/EnrollChannel.hpp>
#include <apps/fastcache-compile-node/EnrollmentResponder.hpp>
#include <apps/fastcache-compile-node/EnrollmentWindow.hpp>
#include <apps/fastcache-compile-node/FleetProbe.hpp>
#include <apps/fastcache-compile-node/FleetSummaryResponder.hpp>
#include <apps/fastcache-compile-node/FleetTextResponder.hpp>
#include <apps/fastcache-compile-node/ForeignFleetWatch.hpp>
#include <apps/fastcache-compile-node/FormationController.hpp>
#include <apps/fastcache-compile-node/FormationLoop.hpp>
#include <apps/fastcache-compile-node/FormationRuntime.hpp>
#include <apps/fastcache-compile-node/LiveStatsResponder.hpp>
#include <apps/fastcache-compile-node/MachineStandingTestUtils.hpp>
#include <apps/fastcache-compile-node/NodeConditions.hpp>
#include <apps/fastcache-compile-node/NodeConfig.hpp>
#include <apps/fastcache-compile-node/NodeFormation.hpp>
#include <apps/fastcache-compile-node/NodeFrameSurface.hpp>
#include <apps/fastcache-compile-node/NodeStatusResponder.hpp>
#include <apps/fastcache-compile-node/PeerIdentity.hpp>
#include <apps/fastcache-compile-node/Responders.hpp>
#include <apps/fastcache-compile-node/SessionResponder.hpp>
#include <apps/fastcache-compile-node/SharedCacheResponder.hpp>
#include <core/async/SyncRun.hpp>
#include <core/net/IDatagramSocket.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemoryDatagram.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/CoHostedDatagram.hpp>
#include <tests/ExactAudience.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

/// @file FormationHarness.hpp
/// A whole office in one process: machines on one simulated LAN, each forming, yielding, asking,
/// being approved, restarting and being forgotten through the REAL formation pieces.
///
/// Per machine, real: `DiscoveryService` over an in-memory datagram bus, `ForeignFleetWatch`,
/// `FormationController` over an in-memory record store, production's `RosterKeys` adopting the applied
/// state, and production's `DialledEnrollChannel` and `DialledFleetProbe`, dialling through a harness
/// dialer to the target machine's REAL `MergedResponder` -- its `EnrollmentResponder` where production
/// builds one and its `FleetSummaryResponder` on every body. The harness fakes the wire and nothing
/// above it. Each surface is built by production's predicates (`ServesScheduler`, `ServesEnrollment`),
/// and every machine beacons and beats at production's intervals.
///
/// Consensus is NOT here: a per-cluster applied state stands in for it, applying every proposal at
/// once and telling every member it records -- never one it no longer records, which learns of its
/// forget from a voter's signed `KeyRevoked` when it dials. That is `RaftClusterHarness`'s subject
/// rather than this one's. A start is judged as `main` judges one, and a reform goes through
/// production's own `AdoptForReform` -- adopted again, judged by the startup rules, published -- paced
/// by `NextReformPace`, then a fresh body.
namespace FastCache::Testing
{

/// A deterministic random source that never repeats a draw: a counter run through SplitMix64,
/// seeded per machine, so two machines never mint one cluster id and no nonce is served twice.
class CountingSecureRandom final: public ISecureRandom
{
  public:
    /// @param seed What distinguishes this source from every other one.
    explicit CountingSecureRandom(std::uint64_t seed) noexcept:
        _state { seed * 0x9E3779B97F4A7C15ULL }
    {
    }

    /// @copydoc ISecureRandom::Fill
    [[nodiscard]] std::expected<void, SecureRandomError> Fill(std::span<std::byte> out) override
    {
        auto word = std::uint64_t { 0 };
        auto left = std::size_t { 0 };
        for (auto& byte: out)
        {
            if (left == 0)
            {
                word = Next();
                left = sizeof(word);
            }
            byte = static_cast<std::byte>(word & 0xFFU);
            word >>= 8U;
            --left;
        }
        return {};
    }

  private:
    /// @return The next SplitMix64 output.
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        _state += 0x9E3779B97F4A7C15ULL;
        auto mixed = _state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return mixed ^ (mixed >> 31U);
    }

    std::uint64_t _state; ///< The counter SplitMix64 mixes into each draw.
};

/// A datagram socket that is plugged into the LAN or not: unplugged, what it sends goes nowhere
/// and what arrives for it is dropped, as an unplugged segment does -- never an error either side.
class LanSocket final: public core::net::IDatagramSocket
{
  public:
    /// @param inner The bus socket.
    /// @param onLan Whether the machine is plugged in; read at every send and receive.
    LanSocket(std::unique_ptr<core::net::IDatagramSocket> inner, bool const& onLan):
        _inner { std::move(inner) },
        _onLan { onLan }
    {
    }

    /// @copydoc core::net::IDatagramSocket::send
    [[nodiscard]] std::expected<void, core::net::NetError> send(std::span<std::byte const> payload,
                                                                core::net::DatagramAddress const& to) override
    {
        if (!_onLan)
            return {};
        return _inner->send(payload, to);
    }

    /// @copydoc core::net::IDatagramSocket::receive
    [[nodiscard]] std::expected<core::net::ReceivedDatagram, core::net::DatagramWait> receive(
        std::chrono::milliseconds timeout) override
    {
        auto received = _inner->receive(timeout);
        if (!_onLan && received.has_value())
            return std::unexpected { core::net::DatagramWait::TimedOut };
        return received;
    }

    /// @copydoc core::net::IDatagramSocket::close
    void close() noexcept override
    {
        _inner->close();
    }

    /// @copydoc core::net::IDatagramSocket::boundAddress
    [[nodiscard]] core::net::DatagramAddress boundAddress() const override
    {
        return _inner->boundAddress();
    }

  private:
    std::unique_ptr<core::net::IDatagramSocket> _inner;
    bool const& _onLan;
};

/// Machines on one simulated LAN, formed by the real formation pieces. See the file comment.
class FormationHarness
{
  public:
    /// How long one beat is: what `Step` advances both clocks by. Finer than either cadence below, so
    /// each runs at its own interval rather than at the beat's.
    static constexpr std::chrono::seconds Beat { 1 };

    /// How often a machine beacons: production's discovery interval, never a harness number.
    static constexpr std::chrono::seconds BeaconEvery = Cluster::DefaultBeaconInterval;

    /// How often a machine's formation beats: production's (`FormationRuntime::TickInterval`).
    static constexpr std::chrono::seconds TickEvery = Node::FormationRuntime::TickInterval;

    /// Long enough for two machines on one segment to meet and for one to decide to ask the other:
    /// a beacon each, and two formation beats.
    static constexpr std::chrono::seconds MeetWithin = BeaconEvery + 2 * TickEvery;

    /// The address an operator's verbs come from: listed as a fleet member on every machine.
    static constexpr std::string_view OperatorAddress = "10.9.9.9";

    /// @param clock What every machine measures intervals on. Must outlive this.
    /// @param wall What every record's instants read. Must outlive this.
    FormationHarness(core::platform::ManualClock& clock, core::platform::ManualWallClock& wall):
        _clock { clock },
        _wall { wall }
    {
    }

    FormationHarness(FormationHarness const&) = delete;
    FormationHarness(FormationHarness&&) = delete;
    FormationHarness& operator=(FormationHarness const&) = delete;
    FormationHarness& operator=(FormationHarness&&) = delete;

    /// Tears every body down before the machines they borrow from.
    ~FormationHarness()
    {
        for (auto& [id, machine]: _machines)
            machine->body.reset();
    }

    /// Add a machine that minted its own cluster at @p mintedAtUnixSeconds, and start it solitary.
    /// @param nodeId Its id; its key is `TestKeyPair(nodeId)`.
    /// @param mintedAtUnixSeconds When its cluster was minted.
    void AddMachine(std::string const& nodeId, std::uint64_t mintedAtUnixSeconds)
    {
        AddMachine(nodeId, mintedAtUnixSeconds, [](Node::NodeConfig& /*cfg*/) {});
    }

    /// `AddMachine`, with its command line shaped by @p shape beyond what every machine names.
    /// @param nodeId Its id; its key is `TestKeyPair(nodeId)`.
    /// @param mintedAtUnixSeconds When its cluster was minted.
    /// @param shape What its operator typed besides.
    void AddMachine(std::string const& nodeId,
                    std::uint64_t mintedAtUnixSeconds,
                    std::function<void(Node::NodeConfig&)> const& shape)
    {
        REQUIRE_FALSE(_machines.contains(nodeId));
        auto const index = _machines.size() + 1;
        auto machine = std::make_unique<Machine>(*this, nodeId, std::format("10.0.0.{}", index), index);
        shape(machine->base);
        // The record a first start mints -- under a FIXED id, `c-<nodeId>`, rather than one the
        // machine's random draws, so a case knows which of two machines minted in the same second
        // keeps its cluster (the lower id) and asserts WHICH one yields.
        auto minted = Minted(std::format("c-{}", nodeId), mintedAtUnixSeconds);
        REQUIRE(machine->store.Save(minted).has_value());
        auto& added = *_machines.emplace(nodeId, std::move(machine)).first->second;
        StartBody(added);
    }

    /// Hand @p nodeId the seed @p seedNodeId answers at, as `--fleet-seed` would.
    /// @param nodeId The machine told.
    /// @param seedNodeId The machine whose `0xFC` endpoint it is told.
    void Seed(std::string const& nodeId, std::string const& seedNodeId)
    {
        At(nodeId).typedSeeds.push_back(At(seedNodeId).self.nodeEndpoint);
    }

    /// Plug @p nodeId into the LAN or pull it out: beacons, probes and polls reach it or not.
    /// @param nodeId The machine.
    /// @param reachable Whether it is plugged in.
    void SetOnLan(std::string const& nodeId, bool reachable)
    {
        auto& machine = At(nodeId);
        machine.onLan = reachable;
        machine.routable = reachable;
    }

    /// Move @p nodeId to another subnet: no beacon crosses to it or from it, and a dialled exchange
    /// still routes -- what a typed seed exists for.
    /// @param nodeId The machine.
    void SetOnOtherSubnet(std::string const& nodeId)
    {
        auto& machine = At(nodeId);
        machine.onLan = false;
        machine.routable = true;
    }

    /// One beat: both clocks advance; every machine whose beacon is due beacons, and the segment
    /// drains; every discovery and watch is maintained, every formation whose beat is due beats,
    /// every machine's own scheduler is asked for a lease, and a forgotten machine that dials hears
    /// its key is revoked; then every reform asked for runs.
    void Step()
    {
        _clock.advance(Beat);
        _wall.advance(Beat);
        for (auto& [id, machine]: _machines)
            if (machine->startAt.has_value() && _clock.now() >= *machine->startAt)
            {
                machine->startAt.reset();
                Reform(*machine);
            }
        for (auto& [id, machine]: _machines)
            if (machine->body != nullptr && _clock.now() >= machine->body->nextBeacon)
            {
                static_cast<void>(machine->body->discovery.SendBeacon());
                machine->body->nextBeacon = _clock.now() + BeaconEvery;
            }
        Drain();
        for (auto& [id, machine]: _machines)
        {
            if (machine->body == nullptr)
                continue;
            machine->body->discovery.Maintain();
            machine->body->watch.Tick();
            if (_clock.now() >= machine->body->nextTick)
            {
                machine->body->controller.Tick();
                machine->body->nextTick = _clock.now() + TickEvery;
            }
            Serve(*machine);
            DialAsForgotten(*machine);
        }
        RunReforms();
    }

    /// Run beats for @p span.
    /// @param span How long.
    void RunFor(std::chrono::seconds span)
    {
        for ([[maybe_unused]] auto const beat: std::views::iota(std::int64_t { 0 }, static_cast<std::int64_t>(span / Beat)))
            Step();
    }

    /// `--enroll-approve=<joiner>@<key>` at @p leaderNodeId, under the joiner's IDENTITY key -- the
    /// key an operator compares from the joiner's `--print-identity`, never the one its row holds.
    /// The row must hold exactly that key, or a joiner that asked under a wrong one would be
    /// approved under it.
    /// @param leaderNodeId The machine leading the fleet.
    /// @param joinerNodeId The machine asking.
    void Approve(std::string const& leaderNodeId, std::string const& joinerNodeId)
    {
        auto& leader = At(leaderNodeId);
        auto const printed = TestKeyPair(joinerNodeId).PublicKey();
        auto const row = Serving(leaderNodeId).window.Find(joinerNodeId);
        INFO(joinerNodeId << " has no row on " << leaderNodeId << "'s enrollment list");
        REQUIRE(row.has_value());
        REQUIRE(Ed25519PublicKey { Unwrap(row).publicKey } == printed);
        RequireAnswered(Operate(leader, CompileCacheWire::EncodeEnrollApprove(joinerNodeId, printed)));
        RunReforms();
    }

    /// Send @p frame to @p nodeId's node port from `OperatorAddress`, through the gate every header passes.
    /// @param nodeId The machine.
    /// @param frame A whole request frame.
    /// @return The reply.
    [[nodiscard]] std::vector<std::byte> AskAsOperator(std::string const& nodeId, std::vector<std::byte> const& frame)
    {
        return Operate(At(nodeId), frame);
    }

    /// @param nodeId The machine.
    /// @return The request ceiling its node port's gate reads: its merged surface's own.
    [[nodiscard]] std::size_t SurfaceCeilingOf(std::string const& nodeId)
    {
        auto& machine = At(nodeId);
        REQUIRE(machine.body != nullptr);
        return machine.body->Surface().MaxRequestBytes();
    }

    /// `--enroll-reject=<joiner>` at @p leaderNodeId.
    /// @param leaderNodeId The machine leading the fleet.
    /// @param joinerNodeId The machine asking.
    void Reject(std::string const& leaderNodeId, std::string const& joinerNodeId)
    {
        RequireAnswered(
            Operate(At(leaderNodeId),
                    CompileCacheWire::EncodeEnrollControl(CompileCacheWire::EnrollControlVerb::Reject, joinerNodeId)));
    }

    /// `--enroll-auto-approve=<duration>` at @p leaderNodeId.
    /// @param leaderNodeId The machine leading the fleet.
    /// @param duration How long the window stays armed.
    void ArmAutoApprove(std::string const& leaderNodeId, std::chrono::seconds duration)
    {
        RequireAnswered(Operate(At(leaderNodeId), CompileCacheWire::EncodeEnrollAutoApprove(duration)));
    }

    /// `--cluster-forget=<nodeId>` at @p leaderNodeId: the record goes and its key is revoked, in one
    /// entry (`Cluster::PrepareForget`), applied and told to every member the state still records --
    /// never to the forgotten one, which consensus stops replicating to. It learns at its next dial
    /// (`Step`), as production's learns: from a voter's signed `KeyRevoked`.
    /// @param leaderNodeId The machine leading the fleet.
    /// @param nodeId The machine forgotten.
    void Forget(std::string const& leaderNodeId, std::string const& nodeId)
    {
        auto const clusterId = ClusterOf(leaderNodeId);
        auto& cluster = _clusters.at(clusterId);
        auto live = std::optional<Ed25519PublicKey> {};
        for (auto const& member: cluster.state.members)
            if (member.id == nodeId)
                live = member.publicKey;
        auto const command = Cluster::PrepareForget(cluster.state, ConfigurationOf(cluster.state), nodeId, live);
        INFO((command.has_value() ? std::string {} : command.error().context));
        REQUIRE(command.has_value());
        Propose(clusterId, *command);
        RunReforms();
    }

    /// Restart @p nodeId: everything in memory goes but its store, and the record is adopted again.
    /// @param nodeId The machine.
    void Restart(std::string const& nodeId)
    {
        auto& machine = At(nodeId);
        machine.body.reset();
        machine.startAt.reset();
        machine.refused.reset();
        machine.reformRun = 0;
        StartBody(machine);
    }

    /// @param nodeId The machine.
    /// @return The mode its record says.
    [[nodiscard]] Cluster::NodeMode ModeOf(std::string const& nodeId) const
    {
        return Serving(nodeId).controller.Mode();
    }

    /// @param nodeId The machine.
    /// @return The cluster it runs: its fleet's once admitted, its own before.
    [[nodiscard]] std::string ClusterOf(std::string const& nodeId) const
    {
        return Cluster::CurrentClusterId(Serving(nodeId).controller.Record());
    }

    /// @param nodeId The machine.
    /// @return Its condition rows.
    [[nodiscard]] Node::NodeConditions const& ConditionsOf(std::string const& nodeId) const
    {
        return At(nodeId).conditions;
    }

    /// @param nodeId The machine.
    /// @return Every line it logged, one per line: what a failing case prints to say why.
    [[nodiscard]] std::string LogOf(std::string const& nodeId) const
    {
        auto text = std::string {};
        for (auto const& record: At(nodeId).logger.Snapshot())
            text += record.message + '\n';
        return text;
    }

    /// @param nodeId The machine.
    /// @return Whether a body serves on it: none while a reform waits its backoff or after one was refused.
    [[nodiscard]] bool IsServing(std::string const& nodeId) const
    {
        return At(nodeId).body != nullptr;
    }

    /// @param nodeId The machine.
    /// @return Why its last reform was refused -- what `main` would exit saying -- or nothing.
    [[nodiscard]] std::optional<std::string> RefusalOf(std::string const& nodeId) const
    {
        return At(nodeId).refused;
    }

    /// @param nodeId The machine.
    /// @return The mode its STORE records: what a restart would adopt, whether or not a body serves it.
    [[nodiscard]] Cluster::NodeMode RecordedModeOf(std::string const& nodeId) const
    {
        auto const kept = At(nodeId).store.Load();
        REQUIRE(kept.has_value());
        auto const record = kept.value_or(std::nullopt);
        REQUIRE(record.has_value());
        return Unwrap(record).mode;
    }

    /// @param nodeId The machine.
    /// @return Every configuration its reforms published, in order.
    [[nodiscard]] std::vector<Node::NodeConfig> const& PublishedBy(std::string const& nodeId) const
    {
        return At(nodeId).publisher.published;
    }

    /// @param nodeId The machine.
    /// @return The ids its discovery holds as AUTHENTICATED: each proved the key its roster holds.
    [[nodiscard]] std::vector<std::string> AuthenticatedPeersOf(std::string const& nodeId) const
    {
        auto ids = std::vector<std::string> {};
        for (auto const& peer: Serving(nodeId).directory.AuthenticatedPeers())
            ids.push_back(peer.nodeId);
        return ids;
    }

    /// @param nodeId The machine.
    /// @return How many leases its OWN scheduler granted it while its record said pending: work a
    ///         serving scheduler did, never a reading of the mode table.
    [[nodiscard]] std::uint64_t ServedWhilePending(std::string const& nodeId) const
    {
        return At(nodeId).servedWhilePending;
    }

  private:
    struct Machine;

    /// What a cluster's consensus would hold: the applied state, and who leads it.
    struct ClusterRun
    {
        Cluster::ClusterState state;    ///< Applied at once, as a quorum of one would.
        std::string leaderId;           ///< Its founder, which leads it throughout.
        std::string leaderNodeEndpoint; ///< Where the leader's `0xFC` port answers.
    };

    /// The consensus a machine's body proposes through: the cluster its record names.
    ///
    /// Always accepts, and never answers `NotLeader` to a machine that does not lead: no case needs
    /// it to, since the controller proposes only as its cluster's leader (`TickMember`), and a node
    /// whose scheduler does not lead refuses an operator's verb before it gets here.
    class MachineAdmin final: public Distributed::IClusterAdmin
    {
      public:
        /// @param harness The harness.
        /// @param machine The machine.
        MachineAdmin(FormationHarness& harness, Machine& machine) noexcept:
            _harness { harness },
            _machine { machine }
        {
        }

        /// @copydoc Distributed::IClusterAdmin::ClusterState
        [[nodiscard]] Cluster::ClusterState ClusterState() const override
        {
            auto const found = _harness._clusters.find(_harness.ClusterOf(_machine.self.nodeId));
            return found != _harness._clusters.end() ? found->second.state : Cluster::ClusterState {};
        }

        /// @copydoc Distributed::IClusterAdmin::ProposeToCluster
        [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
        {
            _harness.Propose(_harness.ClusterOf(_machine.self.nodeId), command);
            return {};
        }

      private:
        FormationHarness& _harness;
        Machine& _machine;
    };

    /// A connection to a machine's node port: what is written to it is answered, when it is first
    /// read, by that machine's REAL merged responder -- the enrollment surface and the fleet-summary
    /// responder its body built -- as a peer at the dialling machine's host. The harness fakes the
    /// wire and nothing above it.
    class AnsweringSocket final: public core::net::ISocket
    {
      public:
        /// @param target The machine dialled; its body answers at the first read.
        /// @param callerHost Where the connection appears to come from.
        AnsweringSocket(Machine& target, std::string callerHost):
            _target { target },
            _callerHost { std::move(callerHost) }
        {
        }

        /// @copydoc core::net::ISocket::write
        [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> bytes) override
        {
            _sent.insert(_sent.end(), bytes.begin(), bytes.end());
            return core::net::IoAwaitable { core::net::IoResult { bytes.size() } };
        }

        /// @copydoc core::net::ISocket::writeVectored
        [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                           std::shared_ptr<void const> /*keepAlive*/ = {}) override
        {
            auto total = std::size_t { 0 };
            for (auto const& segment: segments)
            {
                _sent.insert(_sent.end(), segment.begin(), segment.end());
                total += segment.size();
            }
            return core::net::IoAwaitable { core::net::IoResult { total } };
        }

        /// @copydoc core::net::ISocket::read
        [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
        {
            if (!_answered)
            {
                _answered = true;
                _reply = Gate(_target.body->Surface(), _sent, Node::PeerIdentity { .host = _callerHost });
            }
            auto const take = std::min(_reply.size() - _cursor, buffer.size());
            std::copy_n(_reply.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
            _cursor += take;
            return core::net::IoAwaitable { core::net::IoResult { take } };
        }

        /// What the node port answers @p sent with: through the same pre-payload gate the frame endpoint
        /// asks of every header -- the cap, admission, the credential, the in-flight budget -- and only
        /// a request it lets through reaches the responder. The cap is the surface's OWN
        /// `MaxRequestBytes()`, the fold the endpoint reads (`FrameEndpoint.cpp`, `state.responder`), over
        /// the owners this body merged -- the every-node ones included, built as `main` builds them -- so
        /// a body that merged too few would be refused here as a node would refuse it, not patched over.
        ///
        /// Two inputs are fixed here rather than read, each for a stated reason: the in-flight budget is
        /// 0 because the harness answers one request at a time to completion, so nothing else is ever
        /// in flight on its surface; and no credential was accepted because no harness caller presents
        /// one -- the operator is admitted by ADDRESS (`OperatorAddress`), as a fleet member is.
        /// @param surface The target's merged responder.
        /// @param sent One whole request frame.
        /// @param peer Who asks.
        /// @return The reply.
        [[nodiscard]] static std::vector<std::byte> Gate(Node::MergedResponder& surface,
                                                         std::span<std::byte const> sent,
                                                         Node::PeerIdentity const& peer)
        {
            auto const header = CompileCacheWire::DecodeRequestHeader(sent);
            REQUIRE(header.has_value());
            auto const refusal =
                Node::DecideHeaderRefusal(Node::HeaderGate { .responder = surface, .what = "node port", .inFlightBytes = 0 },
                                          peer,
                                          Unwrap(header),
                                          surface.MaxRequestBytes());
            if (refusal.has_value())
                return Unwrap(refusal).reply;
            return core::async::syncRun(surface.Answer(sent, peer)).bytes;
        }

        /// @copydoc core::net::ISocket::close
        void close() noexcept override
        {
            _closed = true;
        }

        /// @copydoc core::net::ISocket::isClosed
        [[nodiscard]] bool isClosed() const noexcept override
        {
            return _closed;
        }

      private:
        Machine& _target;              ///< Whose responder answers.
        std::string _callerHost;       ///< The dialling machine's host.
        std::vector<std::byte> _sent;  ///< Everything written so far.
        std::vector<std::byte> _reply; ///< The answer, once asked.
        std::size_t _cursor { 0 };     ///< How much of it was read.
        bool _answered { false };      ///< Whether the responder was asked.
        bool _closed { false };        ///< Whether the caller closed it.
    };

    /// What a machine dials through: the machine answering at the endpoint, when both are routable.
    /// Production's `DialledEnrollChannel` and `DialledFleetProbe` run over it unmodified.
    class MachineDialer final: public Node::IEndpointDialer
    {
      public:
        /// @param harness The harness.
        /// @param machine The dialling machine.
        MachineDialer(FormationHarness& harness, Machine& machine) noexcept:
            _harness { harness },
            _machine { machine }
        {
        }

        /// @copydoc Node::IEndpointDialer::Dial
        [[nodiscard]] std::unique_ptr<core::net::ISocket> Dial(std::string_view endpoint,
                                                               core::net::DialOptions /*options*/) override
        {
            auto* const target = _harness.Reach(_machine, endpoint);
            if (target == nullptr)
                return nullptr;
            return std::make_unique<AnsweringSocket>(*target, _machine.host);
        }

      private:
        FormationHarness& _harness;
        Machine& _machine;
    };

    /// What one serving body holds: rebuilt at every restart and every reform.
    ///
    /// Each surface is built where production builds it, by production's predicates over the adopted
    /// configuration: a scheduler only where `ServesScheduler` says, the enrollment surface only
    /// where `ServesEnrollment` says, and the fleet-summary responder on every body.
    struct Body
    {
        /// Build a body for @p machine from @p cfg and @p record, as `main` builds one after the adoption.
        /// @param machine The machine.
        /// @param adopted The configuration the record shaped.
        /// @param record The adopted record.
        /// @param run The cluster the record names, as its consensus holds it.
        Body(Machine& machine, Node::NodeConfig adopted, Cluster::FormationRecord record, ClusterRun const& run):
            cfg { std::move(adopted) },
            live { cfg, nullptr },
            judge { live, machine.endpoints, Node::StartupPolicyRejection },
            controller { Node::FormationParts { .store = machine.store,
                                                .enroll = machine.enroll,
                                                .probe = machine.probe,
                                                .endpoints = machine.endpoints,
                                                .announced = machine.announced,
                                                .admin = machine.admin,
                                                .seeds = [&machine] { return machine.Seeds(); },
                                                .reform = machine.reform,
                                                .clock = machine.harness._clock,
                                                .wall = machine.harness._wall,
                                                .random = machine.random,
                                                .metrics = machine.metrics,
                                                .logger = machine.logger,
                                                .judge = judge,
                                                .conditions = &machine.conditions },
                         machine.self,
                         std::move(record) },
            watch { controller, controller, machine.harness._clock, machine.conditions, controller, BeaconEvery },
            keys { TestKeyPair(machine.self.nodeId), Node::BootstrapMembersOf(cfg) },
            directory { machine.harness._clock, controller, keys },
            discovery { *machine.socket,
                        machine.harness._clock,
                        machine.random,
                        directory,
                        Cluster::DiscoveryConfig { .beaconDestinations =
                                                       std::make_shared<Cluster::FixedBeaconDestination const>(
                                                           core::net::testing::DatagramBus::broadcastAddress()) },
                        controller,
                        watch,
                        keys,
                        machine.metrics,
                        machine.logger },
            window { machine.harness._clock,
                     Node::ServesEnrollment(cfg, Node::ServesScheduler(cfg)) ? &machine.conditions : nullptr,
                     &machine.metrics,
                     machine.harness._wall },
            summary { controller, machine.identity },
            audience { machine.self.nodeEndpoint },
            session { verifier, Node::SessionKeys {}, machine.random, machine.harness._wall, machine.metrics },
            sharedCache { cfg,
                          machine.membership,
                          machine.harness._clock,
                          machine.metrics,
                          machine.logger,
                          nullptr,
                          Node::ReconcileOn::Caller },
            nextBeacon { machine.harness._clock.now() },
            nextTick { machine.harness._clock.now() }
        {
            // The scheduler it built, or null: what `main` asks `ServesEnrollment` of.
            Distributed::SchedulerService* serving = nullptr;
            if (Node::ServesScheduler(cfg))
            {
                serving = &scheduler.emplace(machine.harness._clock,
                                             machine.harness._wall,
                                             machine.metrics,
                                             machine.logger,
                                             signer,
                                             Cluster::CurrentClusterId(controller.Record()));
                serving->AdministerWith(machine.admin);
                serving->SetRole(run.leaderId == machine.self.nodeId ? Distributed::SchedulerRole::Leader
                                                                     : Distributed::SchedulerRole::Follower,
                                 run.leaderId == machine.self.nodeId ? std::string_view {} : run.leaderNodeEndpoint,
                                 Distributed::StandaloneSchedulerTerm);
            }
            if (Node::ServesEnrollment(cfg, serving != nullptr))
                responder.emplace(
                    window, *serving, machine.membership, controller, machine.identity, machine.metrics, machine.logger);
            // Composed by production's own function, over the owners this body built. It builds no
            // TIER -- no cache, scheduler or worker tier, and no consensus to verify a node proof against
            // -- so those families are absent here as on a node running none of them, and the surface's
            // ceiling is the fold of what it does merge.
            merged.emplace(Node::ComposeSurfaceComponents(nullptr,
                                                          nullptr,
                                                          nullptr,
                                                          machine.nodeStatus,
                                                          responder.has_value() ? &*responder : nullptr,
                                                          machine.liveStats,
                                                          machine.fleetText,
                                                          nullptr,
                                                          &summary,
                                                          session,
                                                          sharedCache));
        }

        /// @return What its node port answers with: built by every constructor, after the surfaces
        ///         it merges, which is why it is held in an `optional` at all.
        [[nodiscard]] Node::MergedResponder& Surface()
        {
            if (!merged.has_value())
                std::abort();
            return *merged;
        }

        Node::NodeConfig cfg;                 ///< What the record shaped: what every predicate reads.
        Node::LiveNodeConfig live;            ///< `cfg` as the judge reads it: a harness body never reloads.
        Node::StartupShapeJudge judge;        ///< Production's judge of every move, over `live`.
        Node::FormationController controller; ///< The real controller.
        Node::ForeignFleetWatch watch;        ///< Between discovery and the controller, as production chains them.
        /// Production's keys: what the formation bootstraps with, then the applied state AND the
        /// configuration, as the consensus tier adopts both (`Tell`). The configuration keeps a forgotten
        /// member's key live while consensus still counts that member (#1555) -- a real window in
        /// production, where the two change apart. This harness cannot separate them: it derives the
        /// configuration from the state at the same instant, so the window is always shut here and no
        /// case can see it. The call is kept so the roster is fed as production feeds it.
        Cluster::RosterKeys keys;
        Cluster::PeerDirectory directory;    ///< Discovery's table.
        Cluster::DiscoveryService discovery; ///< The real discovery service.
        Node::EnrollmentWindow window;       ///< Its enrollment list; reports itself only where enrollment is served.
        Distributed::KeyPairLeaseSigner const signer = TestLeaseSigner(); ///< What its scheduler signs with.
        std::optional<Distributed::SchedulerService> scheduler;           ///< Only where `ServesScheduler` says.
        std::optional<Node::EnrollmentResponder> responder;               ///< Only where `ServesEnrollment` says.
        Node::FleetSummaryResponder summary;                              ///< Which fleet it is in, signed.
        /// The session component over a body that holds no roster: every ticket it is shown is
        /// refused, which is all a formation case needs of it.
        ExactAudience audience;
        Distributed::SpentTickets spent;                                   ///< What its verifier spent.
        Distributed::TicketVerifier const verifier { nullptr, audience, spent }; ///< Over no roster.
        Node::SessionResponder session;                                    ///< Ticket minting and AUTH.
        /// The fleet's shared cache every node builds, dormant: nothing names this machine.
        Node::SharedCacheService sharedCache;
        std::optional<Node::MergedResponder> merged;                      ///< What its node port answers with.
        core::platform::SteadyTimePoint nextBeacon;                       ///< When it beacons next.
        core::platform::SteadyTimePoint nextTick;                         ///< When its formation beats next.
    };

    /// One machine: what outlives its bodies.
    struct Machine
    {
        /// @param owner The harness.
        /// @param nodeId Its id.
        /// @param ownHost Its address on the LAN.
        /// @param seed What its random source is seeded with.
        Machine(FormationHarness& owner, std::string const& nodeId, std::string ownHost, std::size_t seed):
            harness { owner },
            host { std::move(ownHost) },
            self { .nodeId = nodeId,
                   .publicKey = TestKeyPair(nodeId).PublicKey(),
                   .nodeEndpoint = std::format("{}:6674", host),
                   .raftEndpoint = std::format("{}:6680", host) },
            random { seed },
            socket { std::make_unique<LanSocket>(
                owner._bus.open(core::net::DatagramAddress { .host = host, .port = TestBeaconPort }), onLan) },
            dialer { owner, *this },
            enroll { dialer },
            probe { dialer, random, owner._clock },
            admin { owner, *this },
            identity { TestKeyPair(nodeId) }
        {
            base.nodeId = nodeId;
            base.identityPublicKey = self.publicKey;
            // Every worker names its scheduler today: the `--scheduler is required` row reads
            // `cfg.schedulers`, never `SchedulersOf`, so a ZERO-CONFIG worker is refused at every start
            // and every reform until Task 24 moves registration to `SchedulersOf`. Named here as an
            // operator must name it now -- the machine's own node port, as a node that schedules does.
            base.schedulers = { self.nodeEndpoint };
            base.hostNames =
                Node::NodeHostNames { .fqdn = nodeId + ".corp.example", .dnsSuffix = "corp.example", .withheld = {} };
        }

        /// @return The typed seeds, as `OrderSeeds` hands them on.
        [[nodiscard]] std::vector<Cluster::SeedCandidate> Seeds() const
        {
            auto seeds = std::vector<Cluster::SeedCandidate> {};
            for (auto const& endpoint: typedSeeds)
                seeds.push_back(
                    Cluster::SeedCandidate { .endpoint = endpoint, .source = Cluster::SeedSource::FleetSeedFlag });
            return seeds;
        }

        FormationHarness& harness;                                ///< Where it lives.
        std::string host;                                         ///< Its address on the LAN.
        Node::SelfFacts self;                                     ///< Who it says it is.
        bool onLan { true };                                      ///< Whether its beacons cross the segment.
        bool routable { true };                                   ///< Whether a dialled exchange reaches it.
        CountingSecureRandom random;                              ///< Its own draws.
        std::unique_ptr<core::net::IDatagramSocket> socket;       ///< Its beacon socket.
        InMemoryFormationStore store;                             ///< Its record: the one thing a restart keeps.
        RecordingArchiver archiver;                               ///< Where a left cluster's store goes.
        ScratchDirectory scratch { "formation-harness" };         ///< Where its fleet endpoints are remembered.
        Cluster::FleetEndpointsFile endpoints { scratch.Path() }; ///< The remembered endpoints.
        ScriptedAnnouncedMemos announced;                         ///< What its members announced: nothing, in this harness.
        MachineDialer dialer;                                     ///< What it dials through.
        Node::DialledEnrollChannel enroll;                        ///< Production's polls, over the dialer.
        Node::DialledFleetProbe probe;                            ///< Production's probes, over the dialer.
        MachineAdmin admin;                                       ///< Its proposals.
        Node::ReformRequest reform;                               ///< Raised by a move.
        Node::NodeConditions conditions;                          ///< Its rows; outlive every body.
        AtomicMetricsSink metrics;                                ///< Its counters.
        CapturingLogger logger;                                   ///< Its log, which `LogOf` reads back.
        Ed25519KeyPair identity;                                  ///< What its responders sign with.
        /// Its operator, admitted to the control verbs from `OperatorAddress`. `MachineTicket`: an
        /// operator's control verb needs a route that IDENTIFIES the caller, and `--fleet-open` admits
        /// nobody to it -- the list stands in for the ticket the operator's own node mints.
        ListedMembership membership { { std::string { OperatorAddress } },
                                      Distributed::MembershipParticipant::MachineTicket };
        /// The operator owners every built node merges (`SurfaceComponents`: never null), built as `main`
        /// builds them, over a source slot nothing attaches -- so each answers that it has nothing to
        /// read. Built for their place on the surface, whose ceiling is their fold.
        LiveStatsSourceSlot liveSources;
        SilentNodeStatus describe; ///< What `NodeStatus` answers: nothing this harness asserts.
        FixedStanding const standing {}; ///< The standing it reports: no roster, nothing pending.
        Node::NodeStatusResponder nodeStatus { describe, liveSources, membership, standing, metrics };
        Node::LiveStatsResponder liveStats { liveSources, membership, AdminCredential {}, harness._loop, metrics };
        Node::FleetTextResponder fleetText { liveSources, membership, AdminCredential {}, metrics };
        std::vector<std::string> typedSeeds;           ///< Its `--fleet-seed` values.
        std::uint64_t servedWhilePending { 0 };        ///< See `ServedWhilePending`.
        Node::NodeConfig base;                         ///< What its start shaped: every body is adopted into a copy.
        std::optional<Node::AdoptedFormation> running; ///< What the serving body was adopted from.
        RecordingPublisher publisher;                  ///< What its reforms published.
        std::size_t reformRun { 0 };                   ///< Reforms in a row (`NextReformPace`).
        core::platform::SteadyTimePoint bodyBegan {};  ///< When the serving body started.
        std::optional<core::platform::SteadyTimePoint> startAt; ///< A reform waiting its backoff row.
        std::optional<std::string> refused;                     ///< Why its last reform was refused: it serves nothing.
        std::unique_ptr<Body> body;                             ///< What serves now; null while none does.
    };

    /// @param nodeId A machine's id.
    /// @return The machine.
    [[nodiscard]] Machine& At(std::string const& nodeId)
    {
        auto const found = _machines.find(nodeId);
        INFO("no machine " << nodeId);
        REQUIRE(found != _machines.end());
        return *found->second;
    }

    /// @param nodeId A machine's id.
    /// @return The machine.
    [[nodiscard]] Machine const& At(std::string const& nodeId) const
    {
        auto const found = _machines.find(nodeId);
        INFO("no machine " << nodeId);
        REQUIRE(found != _machines.end());
        return *found->second;
    }

    /// @param nodeId A machine's id.
    /// @return The body serving on it, which a case asking about it requires there to be.
    [[nodiscard]] Body const& Serving(std::string const& nodeId) const
    {
        auto const& machine = At(nodeId);
        INFO(nodeId << " serves nothing: " << machine.refused.value_or("a reform is waiting its backoff"));
        REQUIRE(machine.body != nullptr);
        return *machine.body;
    }

    /// The machine answering at @p endpoint, when @p from can reach it: both routable.
    /// @param from The dialling machine.
    /// @param endpoint Where it dials.
    /// @return The machine, or null when nothing answers there.
    [[nodiscard]] Machine* Reach(Machine const& from, std::string_view endpoint)
    {
        for (auto& [id, machine]: _machines)
            if (machine->self.nodeEndpoint == endpoint && from.routable && machine->routable && machine->body != nullptr)
                return machine.get();
        return nullptr;
    }

    /// Start @p machine's process: adopt its record as `main` does at a start (`AdoptFormation`, which
    /// mints when none is kept), judge the shape by the startup rules, and build a body from it.
    /// @param machine The machine.
    void StartBody(Machine& machine)
    {
        auto cfg = machine.base;
        auto adopted = Node::AdoptFormation(cfg, machine.store, machine.archiver, machine.endpoints, machine.random, _wall);
        INFO(machine.self.nodeId << ": " << (adopted.has_value() ? std::string { "adopted" } : adopted.error()));
        REQUIRE(adopted.has_value());
        auto const refusal = Node::StartupPolicyRejection(cfg);
        INFO(machine.self.nodeId << " is refused at startup: " << refusal.value_or("(none)"));
        REQUIRE_FALSE(refusal.has_value());
        machine.running = *std::move(adopted);
        BuildBody(machine, std::move(cfg));
    }

    /// Reform @p machine as `RunNodeBodies` does: production's `AdoptForReform` adopts the record into
    /// a fresh copy of the start's configuration, JUDGES it by the startup rules, records what the
    /// body runs by and publishes it. A refusal serves nothing -- `main` would exit saying it.
    /// @param machine The machine.
    void Reform(Machine& machine)
    {
        // Set by the first start, which every reform follows.
        auto* const running = machine.running.has_value() ? &*machine.running : nullptr;
        REQUIRE(running != nullptr);
        auto next = Node::AdoptForReform(machine.base,
                                         *running,
                                         Node::ReformAdoption { .store = machine.store,
                                                                .archiver = machine.archiver,
                                                                .endpoints = machine.endpoints,
                                                                .publisher = &machine.publisher });
        if (!next.has_value())
        {
            machine.logger.Logf(LogLevel::Error, "{}; refusing to start", next.error());
            machine.refused = std::move(next).error();
            return;
        }
        BuildBody(machine, *std::move(next));
    }

    /// Build @p machine's body from @p cfg and the record it was adopted from; then tell it the state
    /// its cluster has applied, as its tier would.
    /// @param machine The machine.
    /// @param cfg What the record shaped.
    void BuildBody(Machine& machine, Node::NodeConfig cfg)
    {
        auto adopted = Unwrap(machine.running);

        // A cluster this machine runs alone exists from its first body: its founder, recorded under
        // its key, which an admission's roster must hold for the proven key to vouch for it.
        auto const clusterId = Cluster::CurrentClusterId(adopted.record);
        if (!_clusters.contains(clusterId))
        {
            auto run =
                ClusterRun { .state = {}, .leaderId = machine.self.nodeId, .leaderNodeEndpoint = machine.self.nodeEndpoint };
            run.state.members.push_back(
                Cluster::ClusterMember { .id = machine.self.nodeId,
                                         .raftEndpoint = machine.self.raftEndpoint,
                                         .schedulerEndpoint = {},
                                         .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                         .seat = Cluster::MemberSeat::Voter,
                                         .publicKey = machine.self.publicKey });
            _clusters.emplace(clusterId, std::move(run));
        }
        auto const& run = _clusters.at(clusterId);
        machine.body = std::make_unique<Body>(machine, std::move(cfg), std::move(adopted.record), run);
        machine.bodyBegan = _clock.now();
        Tell(machine, run, clusterId);
    }

    /// Tell @p machine's body what its cluster applied: the controller, and the keys discovery
    /// classifies by -- both, as the tier's hooks and its roster do.
    /// @param machine The machine.
    /// @param run The cluster's consensus.
    /// @param clusterId Which cluster.
    static void Tell(Machine& machine, ClusterRun const& run, std::string const& clusterId)
    {
        machine.body->keys.Adopt(run.state);
        machine.body->keys.AdoptConfiguration(ConfigurationOf(run.state));
        machine.body->controller.OnClusterState(run.state, clusterId, run.leaderId, run.leaderNodeEndpoint);
    }

    /// @param state A cluster's applied state.
    /// @return The configuration its consensus would count: every member, by its seat.
    [[nodiscard]] static Consensus::Configuration ConfigurationOf(Cluster::ClusterState const& state)
    {
        auto configuration = Consensus::Configuration {};
        for (auto const& member: state.members)
            (member.seat == Cluster::MemberSeat::Voter ? configuration.voters : configuration.learners).push_back(member.id);
        return configuration;
    }

    /// Apply @p command to @p clusterId's state and tell every member the state still records --
    /// never one it no longer records, to which consensus does not replicate.
    /// @param clusterId The cluster.
    /// @param command What was proposed.
    void Propose(std::string const& clusterId, Cluster::Command const& command)
    {
        auto& run = _clusters.at(clusterId);
        Cluster::Apply(run.state, command);
        for (auto& [id, machine]: _machines)
            if (machine->body != nullptr && ClusterOf(id) == clusterId
                && std::ranges::contains(run.state.members, id, &Cluster::ClusterMember::id))
                Tell(*machine, run, clusterId);
    }

    /// Ask @p machine's own scheduler, if its body built one, for a lease on its own worker -- the
    /// work a node serving its own builds does -- and count a grant made while the record says pending.
    /// @param machine The machine.
    void Serve(Machine& machine)
    {
        auto& scheduler = machine.body->scheduler;
        if (!scheduler.has_value())
            return;
        auto const local =
            Distributed::CallerContext { .membership = Distributed::Membership::Member, .peerId = "127.0.0.1" };
        // Every beat, which is also its heartbeat.
        static_cast<void>(scheduler->Register(
            local,
            Distributed::WorkerRegistration {
                .fingerprint = "harness-cc", .endpoint = machine.self.nodeEndpoint, .slots = 1, .codecs = {} }));
        auto const granted = scheduler->Lease(
            local, CompileCacheWire::LeaseRequest { .fingerprint = "harness-cc", .key = "tu", .acceptedCodecs = {} });
        if (granted.status != CompileCacheWire::Status::Ok)
            return;
        if (machine.body->controller.Mode() == Cluster::NodeMode::Pending)
            ++machine.servedWhilePending;
        auto const grant = CompileCacheWire::DecodeLeaseGrant(granted.payload);
        REQUIRE(grant.has_value());
        static_cast<void>(scheduler->Release(local, CompileCacheWire::AsStringView(Unwrap(grant).leaseToken), "tu"));
    }

    /// A machine whose cluster revoked its key and no longer records it dials a voter, as its tier
    /// does at every round, and hears the voter's signed `KeyRevoked` -- the route production takes
    /// once consensus stops replicating to a forgotten member.
    /// @param machine The machine.
    void DialAsForgotten(Machine& machine)
    {
        auto const found = _clusters.find(ClusterOf(machine.self.nodeId));
        if (found == _clusters.end())
            return;
        auto const& state = found->second.state;
        if (!state.IsRevoked(machine.self.publicKey)
            || std::ranges::contains(state.members, machine.self.nodeId, &Cluster::ClusterMember::id))
            return;
        machine.body->controller.OnOwnKeyRevoked(found->second.leaderId);
    }

    /// Rebuild every body whose move asked for a reform, until none is pending -- a bounded number of
    /// rounds, since a body whose start asks for another reform at once would otherwise never end.
    void RunReforms()
    {
        constexpr auto MaxRounds = 8;
        for ([[maybe_unused]] auto const round: std::views::iota(0, MaxRounds))
        {
            auto again = false;
            for (auto& [id, machine]: _machines)
                if (machine->reform.Take())
                {
                    // Paced as `RunFormationLoop` paces it: a run of quick reforms waits its backoff row
                    // with nothing serving, and the move a zero-config node makes once waits nothing.
                    auto const pace = Node::NextReformPace(machine->reformRun, _clock.now() - machine->bodyBegan);
                    machine->reformRun = pace.run;
                    machine->body.reset();
                    if (pace.wait > std::chrono::milliseconds::zero())
                        machine->startAt = _clock.now() + pace.wait;
                    else
                        Reform(*machine);
                    again = true;
                }
            if (!again)
                return;
        }
        FAIL("a reform asked for another at every start, " << MaxRounds << " rounds running");
    }

    /// Let every datagram in flight arrive: pump every socket until a whole round moves nothing.
    void Drain()
    {
        constexpr auto MaxRounds = 64;
        for ([[maybe_unused]] auto const round: std::views::iota(0, MaxRounds))
        {
            auto moved = false;
            for (auto& [id, machine]: _machines)
                while (machine->body != nullptr
                       && machine->body->discovery.PumpOnce(std::chrono::milliseconds { 0 })
                              != Cluster::DiscoveryEvent::Nothing)
                    moved = true;
            if (!moved)
                return;
        }
        FAIL("the segment never went quiet in " << MaxRounds << " rounds");
    }

    /// Answer an operator's control frame at @p machine's node port, from `OperatorAddress`.
    /// @param machine The machine leading the fleet.
    /// @param frame The control frame.
    /// @return The reply.
    [[nodiscard]] static std::vector<std::byte> Operate(Machine& machine, std::vector<std::byte> const& frame)
    {
        INFO(machine.self.nodeId << " serves nothing: " << machine.refused.value_or("a reform is waiting its backoff"));
        REQUIRE(machine.body != nullptr);
        return AnsweringSocket::Gate(
            machine.body->Surface(), frame, Node::PeerIdentity { .host = std::string { OperatorAddress } });
    }

    /// Require @p reply to be an answer rather than a refusal, saying which refusal it was.
    /// @param reply A control verb's reply.
    static void RequireAnswered(std::span<std::byte const> reply)
    {
        auto const header = CompileCacheWire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        auto const read = Unwrap(header);
        if (read.status != CompileCacheWire::Status::Error)
            return;
        auto const refusal =
            CompileCacheWire::DecodeErrorPayload(reply.subspan(CompileCacheWire::ReplyHeaderSize, read.payloadLength));
        FAIL("the operator's verb was refused: "
             << refusal.transform([](auto const& pair) { return std::string { pair.second }; }).value_or("(unreadable)"));
    }

    core::platform::ManualClock& _clock;
    core::platform::ManualWallClock& _wall;
    core::net::testing::TestLoop _loop { _clock }; ///< What a live-stats owner is bound to; never turned.
    core::net::testing::DatagramBus _bus;
    std::map<std::string, ClusterRun> _clusters;
    std::map<std::string, std::unique_ptr<Machine>> _machines;
};

} // namespace FastCache::Testing
