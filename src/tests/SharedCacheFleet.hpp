// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/FrameEndpoint.hpp>
#include <apps/fastcache-compile-node/NodeConditions.hpp>
#include <apps/fastcache-compile-node/NodeIoLoop.hpp>
#include <apps/fastcache-compile-node/NodeMembership.hpp>
#include <apps/fastcache-compile-node/NodeProofClient.hpp>
#include <apps/fastcache-compile-node/NodeProofResponder.hpp>
#include <apps/fastcache-compile-node/ReactorHome.hpp>
#include <apps/fastcache-compile-node/Responders.hpp>
#include <apps/fastcache-compile-node/SharedCacheDirectory.hpp>
#include <apps/fastcache-compile-node/SharedCacheHost.hpp>
#include <apps/fastcache-compile-node/SharedCacheResponder.hpp>
#include <apps/fastcache-compile-node/SharedCacheSession.hpp>
#include <apps/fastcache-compile-node/SharedCacheUpstream.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ConsensusStandingFakes.hpp>
#include <tests/NodeProofFakes.hpp>
#include <tests/ReactorHomeFakes.hpp>
#include <tests/SharedTierFakes.hpp>

namespace FastCache::Testing
{

/// The fleet's shared cache, in process: real `FrameEndpoint`s on loopback serving each cache
/// machine's merged listener exactly as production composes it (`SurfaceComponents { .nodeProof,
/// .sharedCache }` on `NodeSurface::Node`), a real host, responder and proof responder per cache
/// machine, a real directory, dialer, session pool, upstream and proof client per node -- and ONE
/// `Cluster::ClusterState` the fleet applies. `Publish()` fans it to every directory, upstream and
/// host, which is what "the next apply" is.
///
/// **A class of its own beside `FleetHarness`, not a mode of it**: `FleetHarness` is socket-free by
/// design, and the shared cache's properties are properties of a HANDSHAKE, which only real sockets
/// hold to their rules. Still deterministic: nodes run on the case's thread over blocking sockets,
/// machines answer on their own loops, and nothing waits on a timer. The fixtures the upstream's own
/// cases stand up one at a time are nested here, so those cases and the fleet share one copy.
///
/// **Every mutator goes through `Cluster::ValidateAgainst`, then `Cluster::Apply`**, and `REQUIRE`s
/// the validation -- so a case cannot arrange a state production would refuse.
///
/// What it does NOT model:
/// - **The leader.** The state is applied, never replicated; lane 2a records a member's endpoint
///   and tests that recording. `Announce` writes the record as it is written there.
/// - **A caller from another address.** Every caller is loopback; admission BY ADDRESS is the unit
///   cases' subject (`testing.md`'s loopback rule). Admission by KEY, which is what the shared
///   cache asks, is fully real here.
class SharedCacheFleet
{
  public:
    /// The host every recorded `0xFC` endpoint names, which `RecordingConnector` dials at loopback.
    ///
    /// Not loopback itself: a member record holds only an endpoint another machine can dial
    /// (`IsPeerDialableEndpoint`, which `Cluster::ValidateAgainst` asks), so a record naming
    /// `127.0.0.1` is one production refuses. A `.test` name (RFC 6761) resolves nowhere, so only
    /// this fixture's connector can reach it, and the PORT tells the machines apart.
    static constexpr std::string_view FleetHost = "fleet.test";

    /// Records the verb of the first frame in every write a dialled socket makes: an AUTH pipelined
    /// ahead of a FETCH is the first frame of that write, so "no AUTH" and "nothing after the
    /// challenge" are both assertions about this list.
    class RecordingSocket final: public core::net::testing::SocketDecorator
    {
      public:
        /// @param owned The dialled socket.
        /// @param ops Where the verbs go.
        /// @param failNext A verb whose next write finds the connection closed under it; consumed by it.
        RecordingSocket(std::unique_ptr<core::net::ISocket> owned,
                        std::vector<std::uint8_t>& ops,
                        std::optional<std::uint8_t>& failNext):
            SocketDecorator { *owned },
            _owned { std::move(owned) },
            _ops { ops },
            _failNext { failNext }
        {
        }

        RecordingSocket(RecordingSocket const&) = delete;
        RecordingSocket(RecordingSocket&&) = delete;
        RecordingSocket& operator=(RecordingSocket const&) = delete;
        RecordingSocket& operator=(RecordingSocket&&) = delete;
        ~RecordingSocket() override = default;

        [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> bytes) override
        {
            Note(bytes);
            return SocketDecorator::write(bytes);
        }

        [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                           std::shared_ptr<void const> keepAlive = {}) override
        {
            if (!segments.empty())
                Note(segments.front());
            return SocketDecorator::writeVectored(segments, std::move(keepAlive));
        }

      private:
        void Note(std::span<std::byte const> bytes)
        {
            auto const header = CompileCacheWire::DecodeRequestHeader(bytes);
            if (!header.has_value())
                return;
            _ops.push_back(header->opRaw);
            if (_failNext == header->opRaw)
            {
                // The connection breaks under this write: what a session sees when the peer went away.
                _failNext.reset();
                _owned->close();
            }
        }

        std::unique_ptr<core::net::ISocket> _owned;
        std::vector<std::uint8_t>& _ops;
        std::optional<std::uint8_t>& _failNext;
    };

    /// A blocking connector whose every socket records what it writes.
    class RecordingConnector final: public core::net::IConnector
    {
      public:
        [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                         std::uint16_t port,
                                                                         core::net::DialOptions options) override
        {
            if (auto hook = std::exchange(_beforeDial, {}))
                hook();
            // A recorded endpoint names `FleetHost`, which every machine of this fleet answers on.
            if (host == FleetHost)
                host = "127.0.0.1";
            auto socket = co_await _inner.connect(std::move(host), port, options);
            if (!socket.has_value())
                co_return socket;
            co_return std::unique_ptr<core::net::ISocket> { std::make_unique<RecordingSocket>(
                *std::move(socket), _ops, _failNext) };
        }

        /// Run @p hook once, at the start of the next dial: after an operation has taken its target and
        /// before it learns how reaching it went -- the window an apply can land in.
        /// @param hook What to run; on the dialling thread.
        void BeforeNextDialForTesting(std::function<void()> hook)
        {
            _beforeDial = std::move(hook);
        }

        /// Break the connection under the next write of @p op, on whichever dialled socket makes it --
        /// what a session sees when the peer went away and nothing on this end noticed yet: its next write
        /// or read fails at the transport. The socket is closed only AT that write, so a kept session
        /// still looks open to the pool that hands it out.
        /// @param op The verb.
        void FailNextWriteOfForTesting(CompileCacheWire::Op op) noexcept
        {
            _failNext = static_cast<std::uint8_t>(op);
        }

        /// @return The verb of every write, in order, across every socket.
        [[nodiscard]] std::vector<std::uint8_t> const& Ops() const noexcept
        {
            return _ops;
        }

      private:
        core::net::BlockingConnector _inner;
        std::vector<std::uint8_t> _ops;
        std::optional<std::uint8_t> _failNext;
        std::function<void()> _beforeDial;
    };

    /// A trust that admits no server; see `ClientNode::presenceTrust`.
    class RefusingTrust final: public Node::IServerTrust
    {
      public:
        [[nodiscard]] Node::ServerStanding StandingOf(std::string_view /*serverId*/,
                                                      Ed25519PublicKey const& /*serverKey*/) const override
        {
            return Node::ServerStanding::NotVoter;
        }

        [[nodiscard]] std::string_view Expected() const override
        {
            return "nobody";
        }
    };

    /// A surface that answers as @p inner does, except that it can hold every answer to one verb for a
    /// while before giving it: a proven server that stops replying AFTER the proof.
    ///
    /// A decorator rather than a stall inside the tier, because a tier that blocked would block the
    /// reactor the client's own deadline runs on, and the case would measure nothing.
    class StallingResponder final: public Node::IFrameResponder
    {
      public:
        /// @param inner What answers; must outlive this.
        explicit StallingResponder(Node::IFrameResponder& inner) noexcept:
            _inner { inner }
        {
        }

        /// Hold every answer to @p verb for @p hold on @p reactor's clock, then answer it. Set before
        /// the operation that meets it is posted to the reactor.
        /// @param reactor The loop the endpoint answers on.
        /// @param verb The verb whose answers are held: SHARED-FETCH or SHARED-STORE.
        /// @param hold How long.
        void Stall(core::net::EventLoop* reactor, CompileCacheWire::Op verb, std::chrono::milliseconds hold) noexcept
        {
            _reactor = reactor;
            _verb = static_cast<std::uint8_t>(verb);
            _hold = hold;
        }

        [[nodiscard]] core::async::Task<Node::FrameReply> Answer(std::span<std::byte const> frame,
                                                                 Node::PeerIdentity peer) override
        {
            auto const header = CompileCacheWire::DecodeRequestHeader(frame);
            if (_reactor != nullptr && header.has_value() && header->opRaw == _verb)
                std::ignore = co_await Testing::AwaitUntil(
                    _reactor,
                    "the stall the case asked for to run out",
                    [] { return false; },
                    [] { return std::string {}; },
                    Testing::ReactorWaitOptions { .context = {},
                                                  .bound = std::chrono::duration_cast<core::platform::SteadyDuration>(_hold),
                                                  .rest = std::chrono::duration_cast<core::platform::SteadyDuration>(
                                                      std::chrono::milliseconds { 10 }) });
            co_return co_await _inner.Answer(frame, std::move(peer));
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(Node::PeerIdentity const& peer,
                                                                       std::uint8_t opRaw) const override
        {
            return _inner.RefusePeer(peer, opRaw);
        }

        [[nodiscard]] Node::CredentialVerdict CheckCredential(std::span<std::byte const> payload) const override
        {
            return _inner.CheckCredential(payload);
        }

        [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                          std::uint8_t opRaw,
                                                          std::string_view detail) const override
        {
            return _inner.RefusalReply(decision, opRaw, detail);
        }

        [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(Node::EndpointRefusal refusal,
                                                                  std::uint8_t opRaw,
                                                                  std::string_view detail) const override
        {
            return _inner.EndpointRefusalReply(refusal, opRaw, detail);
        }

        [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t opRaw) const noexcept override
        {
            return _inner.RequestTimeout(opRaw);
        }

        [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
        {
            return _inner.MaxRequestBytes();
        }

        [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
        {
            return _inner.MaxOpenConnections();
        }

        [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
        {
            return _inner.MaxInFlightBytes();
        }

        [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t opRaw) const noexcept override
        {
            return _inner.HoldsOwnByteBudget(opRaw);
        }

        [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t opRaw) const noexcept override
        {
            return _inner.PeerWatchCounter(opRaw);
        }

        [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t opRaw) const noexcept override
        {
            return _inner.ProgressInterval(opRaw);
        }

        [[nodiscard]] Node::IFrameStream* StreamFor(std::uint8_t opRaw) noexcept override
        {
            return _inner.StreamFor(opRaw);
        }

        [[nodiscard]] Node::INodeProver* NodeProver() noexcept override
        {
            return _inner.NodeProver();
        }

      private:
        Node::IFrameResponder& _inner;
        core::net::EventLoop* _reactor { nullptr };
        std::uint8_t _verb { 0 };
        std::chrono::milliseconds _hold { 0 };
    };

    /// A shared-cache machine on loopback that signs as @p signsAs -- under @p keyOf's key, when that is
    /// named, and its own otherwise -- composed as production composes the node's merged listener.
    ///
    /// Standalone, it serves from the start and admits the four test machines' keys; in a
    /// `SharedCacheFleet`, `Apply` replaces both with what the fleet's state says, at every `Publish`.
    /// Declaration order is teardown order: the endpoint goes first, then the loop
    /// (FrameEndpoint_test.cpp's `Fleet` explains why).
    struct CacheMachine
    {
        explicit CacheMachine(std::string signsAs, std::string const& keyOf = {}):
            id { std::move(signsAs) },
            identity { Testing::TestKeyPair(keyOf.empty() ? id : keyOf) }
        {
            Testing::PublishKeyRoster(roster, { "pc-7", "pc-8", "cache-c", "cache-d" });
            Cluster::ClusterState state;
            state.settings.push_back(Cluster::Setting { .name = std::string { Cluster::SharedCacheSetting }, .value = id });
            host.Applied(state);
            host.Reconcile();
            auto listened = core::net::listen(io.Reactor(), core::net::ListenOptions { .host = "127.0.0.1", .port = 0 });
            REQUIRE(listened.has_value());
            port = (*listened)->boundPort();
            endpoint = Node::FrameEndpoint::StartWithListener(io,
                                                              Node::NodeSurface::Node,
                                                              *std::move(listened),
                                                              std::format("127.0.0.1:{}", port),
                                                              front,
                                                              metrics,
                                                              logger);
            REQUIRE(endpoint != nullptr);
            io.Start();
        }

        /// Take what the cluster applied, as a node running consensus does: which keys this machine
        /// admits (`Node::PublishClusterKeys`, production's one derivation) and whether it serves the
        /// shared tier -- reconciled at once, so a case reads the result without waiting on a thread.
        /// @param state The fleet's state.
        void Apply(Cluster::ClusterState const& state)
        {
            Node::PublishClusterKeys(roster, state);
            host.Applied(state);
            host.Reconcile();
        }

        std::string id;
        Ed25519KeyPair identity;
        AtomicMetricsSink metrics;
        NullLogger logger;
        Distributed::KeyRosterMembership roster;
        SystemSecureRandom random;
        MemoryOpener opener;
        Node::SharedCacheHost host { id, opener, nullptr, logger, Node::ReconcileOn::Caller };
        Testing::ScriptedAppliedState consensus { Node::AppliedStateReading::CaughtUp };
        Node::NodeProofResponder prover { id, identity, roster, consensus, random, metrics, logger };
        Node::SharedCacheResponder responder { host, roster, metrics };
        Node::MergedResponder merged { Node::SurfaceComponents { .nodeProof = &prover, .sharedCache = &responder } };
        StallingResponder front { merged };
        Node::NodeIoLoop io;
        std::uint16_t port { 0 };
        std::unique_ptr<Node::FrameEndpoint> endpoint;
    };

    /// One client node: its directory, its identity, its upstream.
    ///
    /// On a blocking connector with no reactor by default, driven by `core::async::syncRun` on the
    /// case's thread -- or on @p io's reactor and its connector, as production runs it, where the
    /// deadlines and the idle timer are armed. Destroyed from the case's thread either way: the pool
    /// hands its kept reactor sessions to its home rather than freeing them, as production relies on.
    struct ClientNode
    {
        /// @param machine This node's id.
        /// @param io The loop to run on; null for a blocking connector.
        /// @param dialPolicy The dial and exchange bounds.
        /// @param poolHome Where the pool's kept sessions go; null for @p io itself, or for a home over a
        ///        loop nobody turns when there is no @p io.
        explicit ClientNode(std::string machine,
                            Node::NodeIoLoop* io = nullptr,
                            Node::SharedCacheDialPolicy dialPolicy = {},
                            Node::IReactorHome* poolHome = nullptr):
            id { std::move(machine) },
            key { Testing::TestKeyPair(id) },
            policy { dialPolicy },
            home { HomeFor(io, poolHome, blockingHome) },
            reactor { io != nullptr ? &io->Reactor() : nullptr },
            dialer { prover, io != nullptr ? io->Connector() : connector, reactor, metrics, policy }
        {
        }

        ClientNode(ClientNode const&) = delete;
        ClientNode(ClientNode&&) = delete;
        ClientNode& operator=(ClientNode const&) = delete;
        ClientNode& operator=(ClientNode&&) = delete;
        ~ClientNode() = default;

        /// @param io The loop the node runs on, or null.
        /// @param poolHome A home the case supplies, or null.
        /// @param blocking The home a blocking node's pool gets.
        /// @return @p poolHome when the case supplied one, then @p io, then @p blocking.
        [[nodiscard]] static Node::IReactorHome& HomeFor(Node::NodeIoLoop* io,
                                                         Node::IReactorHome* poolHome,
                                                         Node::IReactorHome& blocking) noexcept
        {
            if (poolHome != nullptr)
                return *poolHome;
            if (io != nullptr)
                return *io;
            return blocking;
        }

        std::string id;
        Ed25519KeyPair key;
        Node::SharedCacheDialPolicy policy;
        /// The home a blocking pool gets: its sockets belong to no reactor, so nothing timed there fires.
        Testing::UnturnedLoopHome blockingHome;
        Node::IReactorHome& home;
        core::net::EventLoop* reactor;
        SystemSecureRandom random;
        Node::SharedCacheDirectory directory { id, {} };
        /// The trust `NodeProofClient`'s synchronous `Prove` would use. The upstream never calls it -- it
        /// passes a `NamedMachineTrust` per operation to `ProveAsync` -- so this refuses everybody.
        RefusingTrust presenceTrust;
        Node::NodeProofClient prover { id, key, presenceTrust, nullptr, nullptr, random };
        RecordingConnector connector;
        AtomicMetricsSink metrics;
        NullLogger logger;
        Node::SharedCacheDialer dialer;
        /// The pool's clock: a kept session's idle age is read from it.
        core::platform::ManualClock clock;
        Node::SharedSessionPool sessions { dialer, clock, home };
        Node::NodeConditions conditions;
        Node::SharedCacheUpstream upstream { directory, sessions, reactor, metrics, &conditions, logger, policy };
    };

    SharedCacheFleet() = default;
    SharedCacheFleet(SharedCacheFleet const&) = delete;
    SharedCacheFleet(SharedCacheFleet&&) = delete;
    SharedCacheFleet& operator=(SharedCacheFleet const&) = delete;
    SharedCacheFleet& operator=(SharedCacheFleet&&) = delete;
    ~SharedCacheFleet() = default;

    /// A cache machine, admitted as a learner under its own key, its merged listener bound on
    /// loopback. Dormant, and admitting only the fleet's keys, from the start.
    /// @param id Its id.
    /// @return It.
    CacheMachine& AddCacheMachine(std::string id)
    {
        Admit(id);
        auto& machine = *_machines.emplace_back(std::make_unique<CacheMachine>(std::move(id)));
        machine.Apply(_state);
        return machine;
    }

    /// A node that reads through to the fleet's shared cache, admitted as a learner under its key.
    /// @param id Its id.
    /// @return It.
    ClientNode& AddNode(std::string id)
    {
        Admit(id);
        return *_nodes.emplace_back(std::make_unique<ClientNode>(std::move(id)));
    }

    /// Name @p id as the fleet's shared cache; empty unsets the setting.
    /// @param id The machine.
    void Name(std::string const& id)
    {
        Commit(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                  .key = std::string { Cluster::SharedCacheSetting },
                                  .value = id,
                                  .schedulerEndpoint = {},
                                  .publicKey = std::nullopt });
    }

    /// Record @p id's `0xFC` endpoint as its own port, as a machine announcing itself does.
    /// @param id The machine.
    void Announce(std::string const& id)
    {
        AnnounceAt(id, id);
    }

    /// Record @p id's `0xFC` endpoint as @p signsAs's port: an address reassigned between two
    /// machines of the fleet.
    /// @param id The machine whose record moves.
    /// @param signsAs The machine that answers there.
    void AnnounceAt(std::string const& id, std::string const& signsAs)
    {
        // `AddLearner` applies wholesale, so the record keeps its consensus endpoint and its key.
        auto const member = std::ranges::find(_state.members, id, &Cluster::ClusterMember::id);
        REQUIRE(member != _state.members.end());
        Commit(Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                  .key = id,
                                  .value = member->raftEndpoint,
                                  .schedulerEndpoint = std::format("{}:{}", FleetHost, Machine(signsAs).port),
                                  .publicKey = TestKeyPair(id).PublicKey() });
    }

    /// Forget @p id: its record goes and its key is revoked, as `--cluster-forget` does.
    /// @param id The machine.
    void Forget(std::string const& id)
    {
        Commit(Cluster::Command { .kind = Cluster::CommandKind::Forget,
                                  .key = id,
                                  .value = {},
                                  .schedulerEndpoint = {},
                                  .publicKey = std::nullopt });
    }

    /// Apply the state everywhere, in production's order: every machine takes its keys and its
    /// tier, and every node's directory learns the state before its upstream re-judges from it.
    void Publish()
    {
        for (auto const& machine: _machines)
            machine->Apply(_state);
        // A client node serves nothing, so it has no host to tell.
        for (auto const& node: _nodes)
            Node::ApplySharedCacheState(_state, node->directory, nullptr, &node->upstream);
    }

    /// @param id A machine added to this fleet.
    /// @return It.
    [[nodiscard]] CacheMachine& Machine(std::string_view id)
    {
        auto const found = std::ranges::find(
            _machines, id, [](std::unique_ptr<CacheMachine> const& machine) { return std::string_view { machine->id }; });
        REQUIRE(found != _machines.end());
        return **found;
    }

    /// @param id A node added to this fleet.
    /// @return It.
    [[nodiscard]] ClientNode& NodeOf(std::string_view id)
    {
        auto const found = std::ranges::find(
            _nodes, id, [](std::unique_ptr<ClientNode> const& node) { return std::string_view { node->id }; });
        REQUIRE(found != _nodes.end());
        return **found;
    }

    /// @return The state as of the last mutator, published or not.
    [[nodiscard]] Cluster::ClusterState const& State() const noexcept
    {
        return _state;
    }

  private:
    /// Admit @p id as a learner under its own key, at a consensus endpoint of its own.
    /// @param id The machine or node.
    void Admit(std::string const& id)
    {
        ++_admitted;
        Commit(Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                  .key = id,
                                  .value = std::format("10.0.0.{}:6680", _admitted),
                                  .schedulerEndpoint = {},
                                  .publicKey = TestKeyPair(id).PublicKey() });
    }

    /// Validate @p command against the state as production would, then apply it.
    /// @param command The change.
    void Commit(Cluster::Command const& command)
    {
        auto const valid = Cluster::ValidateAgainst(_state, command);
        INFO("a fleet change production would refuse: " << (valid.has_value() ? std::string {} : valid.error().context));
        REQUIRE(valid.has_value());
        Cluster::Apply(_state, command);
    }

    Cluster::ClusterState _state;
    std::size_t _admitted { 0 };
    /// Declared BEFORE the nodes, so destroyed AFTER them: a node's kept session is a socket into a
    /// machine's loop, and it must close while that loop still turns (`FrameEndpoint_test.cpp`'s
    /// `Fleet`). A PRECAUTION for a loop-driven node, which this fleet does not build: `AddNode`'s
    /// nodes dial on a blocking connector and park nothing on a machine's loop, so swapping these two
    /// declarations stays green today, and that is no evidence the order is unnecessary.
    std::vector<std::unique_ptr<CacheMachine>> _machines;
    std::vector<std::unique_ptr<ClientNode>> _nodes;
};

} // namespace FastCache::Testing
