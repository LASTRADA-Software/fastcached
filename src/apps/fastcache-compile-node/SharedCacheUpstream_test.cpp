// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "ConsensusTier.hpp"
#include "FrameEndpoint.hpp"
#include "NodeCredential.hpp"
#include "NodeIoLoop.hpp"
#include "NodeProofResponder.hpp"
#include "Responders.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheResponder.hpp"
#include "SharedCacheSession.hpp"
#include "SharedCacheTier.hpp"
#include "SharedCacheUpstream.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>
#include <core/async/SyncRun.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/NodeProofFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ReactorHomeFakes.hpp>
#include <tests/SharedCacheFleet.hpp>
#include <tests/SharedTierFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::AStoredObject;
using FastCache::Testing::MemoryOpener;
using CacheMachine = FastCache::Testing::SharedCacheFleet::CacheMachine;
using ClientNode = FastCache::Testing::SharedCacheFleet::ClientNode;
using namespace std::chrono_literals;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A home that counts what a pool hands it, and passes it on to the loop it stands for.
class CountingHome final: public IReactorHome
{
  public:
    /// @param io The loop; must outlive this.
    explicit CountingHome(NodeIoLoop& io) noexcept:
        _io { io }
    {
    }

    [[nodiscard]] core::net::EventLoop& Loop() noexcept override
    {
        return _io.Loop();
    }

    void Retire(std::shared_ptr<void> owned) override
    {
        if (owned != nullptr)
            _retired.fetch_add(1, std::memory_order_acq_rel);
        _io.Retire(std::move(owned));
    }

    /// @return How many objects it was handed.
    [[nodiscard]] std::size_t Retired() const noexcept
    {
        return _retired.load(std::memory_order_acquire);
    }

  private:
    NodeIoLoop& _io;
    std::atomic<std::size_t> _retired { 0 };
};

/// The state a client applies: @p machine named, its key the roster's, its endpoint @p port on loopback.
/// @param machine The machine the setting names.
/// @param port Where it answers.
/// @return The state.
[[nodiscard]] Cluster::ClusterState Naming(std::string const& machine, std::uint16_t port)
{
    Cluster::ClusterState state;
    // The member record as it is written at admission: its advertised 0xFC endpoint in `schedulerEndpoint`.
    Cluster::Apply(state,
                   Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                      .key = machine,
                                      .value = "10.0.0.3:6680",
                                      .schedulerEndpoint = std::format("127.0.0.1:{}", port),
                                      .publicKey = Testing::TestKeyPair(machine).PublicKey(),
                                      .role = std::nullopt });
    state.settings.push_back(Cluster::Setting { .name = std::string { Cluster::SharedCacheSetting }, .value = machine });
    return state;
}

/// @param port Where `cache-c` answers.
/// @return The state naming `cache-c` there.
[[nodiscard]] Cluster::ClusterState NamingCacheC(std::uint16_t port)
{
    return Naming("cache-c", port);
}

/// @param port Where `cache-d` answers.
/// @return The state naming `cache-d` there.
[[nodiscard]] Cluster::ClusterState NamingCacheD(std::uint16_t port)
{
    return Naming("cache-d", port);
}

/// Hand @p task's answer to @p done.
/// @param task The operation.
/// @param done Where its answer goes; shared, so it outlives a case that stopped waiting for it.
template <typename T>
core::async::Task<void> Deliver(core::async::Task<T> task, std::shared_ptr<std::promise<T>> done)
{
    done->set_value(co_await std::move(task));
}

/// How long a case waits for an operation on the reactor before it is a red.
constexpr auto OperationBound = Testing::WaitHangGuard;

/// Run what @p start begins on @p io's reactor, as production runs an upstream operation, and wait
/// for its answer -- bounded, on the case's thread.
///
/// **A wait that runs out is a clean red, never an unwind under a running operation.** The posted
/// work owns what it writes to -- the promise is shared, the starter copied in -- but the operation
/// itself borrows the case's client node, so unwinding while it runs would free what it is using.
/// So a late operation is waited out FIRST, for a second hang guard, and only then does the case
/// fail; one that outlives both is a hang no unwind can make safe, and ends the binary loudly.
/// @param io The loop.
/// @param start Called ON the reactor, so the operation begins there.
/// @return The answer.
template <typename T>
[[nodiscard]] T OnReactor(NodeIoLoop& io, std::function<core::async::Task<T>()> start)
{
    auto done = std::make_shared<std::promise<T>>();
    auto answer = done->get_future();
    io.Reactor().post([&io, start = std::move(start), done] { io.Reactor().spawn(Deliver<T>(start(), done)); });
    auto const inTime = answer.wait_for(OperationBound) == std::future_status::ready;
    if (!inTime && answer.wait_for(Testing::WaitHangGuard) != std::future_status::ready)
    {
        std::fputs("OnReactor: an operation outlived two hang guards and still borrows the case's state; "
                   "ending the binary rather than unwinding under it\n",
                   stderr);
        std::abort();
    }
    REQUIRE(inTime);
    return answer.get();
}

/// A peer that accepts one connection and says nothing on it until @p silentFor has passed, then
/// closes: long enough that an operation ending sooner was ended by a deadline of its own.
struct SilentPeer
{
    explicit SilentPeer(std::chrono::milliseconds silentFor):
        listener { BlockingListener::Bind("127.0.0.1", 0) }
    {
        REQUIRE(listener != nullptr);
        REQUIRE(listener->IsBound());
        listener->SetTimeouts(Testing::WaitHangGuard, std::chrono::milliseconds { 0 });
        port = listener->boundPort();
        holder = std::jthread { [this, silentFor] { Hold(silentFor); } };
    }

    std::unique_ptr<BlockingListener> listener;
    std::uint16_t port { 0 };
    std::jthread holder; ///< Declared last: joined before the listener it reads goes.

  private:
    /// Read what the caller sends, then wait out the silence -- or the caller leaving -- and close.
    /// No assertion here: this is a helper thread.
    void Hold(std::chrono::milliseconds silentFor) const
    {
        auto accepted = core::async::syncRun(listener->accept());
        if (!accepted.has_value())
            return;
        auto const socket = *std::move(accepted);
        socket->setReceiveDeadline(silentFor);
        auto buffer = std::array<std::byte, 512> {};
        std::ignore = core::async::syncRun(ReadOnce(socket.get(), buffer)); // the challenge
        std::ignore = core::async::syncRun(ReadOnce(socket.get(), buffer)); // the silence
        socket->close();
    }

    /// One read, as a task `syncRun` can drive.
    static core::async::Task<core::net::IoResult> ReadOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
    {
        co_return co_await socket->read(buffer);
    }
};

/// A client policy whose every budget is short, so a case measuring a deadline takes well under a
/// second when the deadline works.
/// @param ioTimeout The handshake and exchange budget at the announced endpoint.
/// @return The policy.
[[nodiscard]] SharedCacheDialPolicy ShortPolicy(std::chrono::milliseconds ioTimeout)
{
    return SharedCacheDialPolicy {
        .hintConnectTimeout = 300ms, .hintHandshakeTimeout = 200ms, .connectTimeout = 1'000ms, .ioTimeout = ioTimeout
    };
}

/// How long a silent peer stays silent, and so the least an operation without its deadline takes.
constexpr auto SilentFor = 3'000ms;

/// The most an operation ended by a short deadline may take here: well under `SilentFor`, well over
/// every short budget -- the two readings the deadline cases tell apart.
constexpr auto WithinDeadline = 1'500ms;
static_assert(WithinDeadline < SilentFor);

/// Whether @p node sent a frame of @p op.
/// @param node The client.
/// @param op The verb.
/// @return True when any write it made opened with that verb.
[[nodiscard]] bool Sent(ClientNode const& node, Wire::Op op)
{
    return std::ranges::contains(node.connector.Ops(), static_cast<std::uint8_t>(op));
}

} // namespace

TEST_CASE("A node stores to and fetches from the named machine once it proves its key", "[node][shared-cache][upstream]")
{
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    ClientNode b { "pc-8" };
    a.directory.Applied(NamingCacheC(c.port));
    b.directory.Applied(NamingCacheC(c.port));

    auto const value = AStoredObject();
    CHECK(core::async::syncRun(a.upstream.Store("k", value)) == UpstreamStore::Stored);
    auto const fetched = core::async::syncRun(b.upstream.Fetch("k"));
    REQUIRE(fetched.has_value());
    CHECK(c.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheHits) == 1);
    CHECK(b.upstream.Report().state == Wire::WireSharedCacheState::Proven);
    CHECK(b.upstream.Report().machineId == "cache-c");
    CHECK(b.upstream.Configured());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    // The FLEET verbs, and never the private tier's: FETCH/STORE on another machine are refused.
    CHECK(Sent(a, Wire::Op::SharedStore));
    CHECK(Sent(b, Wire::Op::SharedFetch));
    CHECK_FALSE(Sent(a, Wire::Op::Store));
    CHECK_FALSE(Sent(b, Wire::Op::Fetch));
}

TEST_CASE("A node sends nothing to a machine that proves another key", "[node][shared-cache][upstream]")
{
    // The address the state records answers -- as ANOTHER fleet machine. Everything this node may
    // send before the server has proved itself is the challenge; the proof and the object never leave.
    CacheMachine impostor { "pc-9" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(impostor.port));

    CHECK_FALSE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);

    constexpr auto challenge = static_cast<std::uint8_t>(Wire::Op::NodeChallenge);
    CHECK(std::ranges::all_of(a.connector.Ops(), [](std::uint8_t op) { return op == challenge; }));
    CHECK(a.connector.Ops().size() == 2); // one challenge per operation, and nothing else
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 2);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 0);
    CHECK(impostor.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 0);
    CHECK(impostor.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == 0);
    auto const report = a.upstream.Report();
    CHECK(report.state == Wire::WireSharedCacheState::WrongKey);
    // The reason names the machine that answered, so an operator knows which address moved.
    CHECK(report.detail.contains("pc-9"));
}

TEST_CASE("A machine that names itself the shared cache under another key is refused as a wrong key",
          "[node][shared-cache][upstream]")
{
    // The other half of an impostor: the right NAME, and a key that is not the one the roster
    // records for it. Counted as the same wrong key as another machine answering -- the announced
    // endpoint proved a key that is not the named machine's -- and nothing but the challenge leaves.
    CacheMachine impostor { "cache-c", "not-cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(impostor.port));

    CHECK_FALSE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 0);
    CHECK(a.connector.Ops() == std::vector { static_cast<std::uint8_t>(Wire::Op::NodeChallenge) });
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::WrongKey);
}

TEST_CASE("A shared cache that does not answer is counted as failed and raised until one proves",
          "[node][shared-cache][upstream][conditions]")
{
    // Nothing listens where the state says the named machine answers: a network, not an impostor --
    // counted apart from a wrong key -- and this node's builds are not reaching the fleet's cache,
    // which is a condition an operator acts on rather than a log line.
    auto const gone = CacheMachine { "cache-c" }.port;
    ClientNode a { "pc-7" };
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
    a.directory.Applied(NamingCacheC(gone));

    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 0);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Unreachable);
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);

    // The named machine answers again, somewhere the next apply records: the first proof clears it.
    {
        CacheMachine c { "cache-c" };
        a.directory.Applied(NamingCacheC(c.port));
        CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);
        CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
    }

    // And the other way out: raised again -- the machine is gone, at its hint and at its name --
    // then the setting is unset. Nothing is dialled any more, and the first operation that finds so
    // clears what the last failure raised.
    a.directory.Applied(NamingCacheC(gone));
    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    REQUIRE(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
    a.directory.Applied(Cluster::ClusterState {});
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::NotConfigured);
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
}

TEST_CASE("No credential reaches the shared cache", "[node][shared-cache][upstream][credential]")
{
    // The leg carries no AUTH at all: the upstream holds no credential source, and this is the
    // observation that it never grows one by another route.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);
    REQUIRE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK_FALSE(Sent(a, Wire::Op::Auth));
}

TEST_CASE("The shared-cache upstream holds no credential source by construction",
          "[node][shared-cache][upstream][credential]")
{
    // The case above observes that no AUTH leaves; this one says why none CAN. A credential reaches
    // an outbound leg only through `ICredentialSource` (the rulebook's one seam), so a leg that holds
    // none presents nothing whatever its call sites write -- and the only way to give it one is to
    // name the seam in these files. Named there, this case fails, and whoever added it reads why the
    // proven session is the identity here and `--requirepass` must not travel to another machine.
    //
    // A scan rather than a type trait: a trait can refuse ONE constructor shape, and a new parameter
    // with a default, or a member set later, is another. Every such route spells the type's name.
    //
    // The FILES are derived, not listed: every non-test `SharedCache*` source, so a file added to
    // this leg later is scanned without anybody remembering to add it.
    std::filesystem::path const nodeDir =
        std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node";
    std::regex const seam { R"(\bICredentialSource\b|\bConfiguredCredential\b|\bCc::Credential\b)" };

    // The one reader both the scan and its control go through: the code lines that name the seam,
    // a comment -- the class's own documentation discusses the seam -- not being a use.
    auto const seamLines = [&seam](std::filesystem::path const& file) {
        std::ifstream in { file, std::ios::binary };
        std::ostringstream contents;
        contents << in.rdbuf();
        std::istringstream lines { std::move(contents).str() };
        std::vector<std::string> found;
        std::string line;
        std::size_t number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            auto const first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line.compare(first, 2, "//") == 0)
                continue;
            if (std::regex_search(line, seam))
                found.push_back(std::format("{}:{}: {}", file.filename().string(), number, line));
        }
        return found;
    };

    std::vector<std::string> scanned;
    for (auto const& entry: std::filesystem::directory_iterator { nodeDir })
    {
        auto const name = entry.path().filename().string();
        auto const extension = entry.path().extension().string();
        if (!name.starts_with("SharedCache") || name.ends_with("_test.cpp") || (extension != ".hpp" && extension != ".cpp"))
            continue;
        scanned.push_back(name);
        for (auto const& use: seamLines(entry.path()))
            FAIL_CHECK("the shared-cache leg names the credential seam on a code line: " << use);
    }
    // The derivation reached the read side's four files: a glob that matched nothing would pass.
    for (auto const* readSide:
         { "SharedCacheUpstream.hpp", "SharedCacheUpstream.cpp", "SharedCacheSession.hpp", "SharedCacheSession.cpp" })
    {
        INFO(readSide);
        CHECK(std::ranges::contains(scanned, std::string { readSide }));
    }

    // The positive control, through the SAME reader: the seam is found on a code line where it IS
    // held, so a clean result above is the reader finding nothing rather than being blind.
    CHECK_FALSE(seamLines(nodeDir / "RemoteUpstream.hpp").empty());
}

TEST_CASE("A stale dial hint is counted as stale and the announced name is dialled in the same operation",
          "[node][shared-cache][upstream][hint]")
{
    CacheMachine c { "cache-c" };
    CacheMachine other { "pc-9" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(a.upstream.Fetch("warm-up")) == std::nullopt); // proves, records the hint
    REQUIRE(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);

    // The address the hint holds now answers as another machine -- and the session the warm-up
    // proved is left to age out, so this operation dials rather than reusing it.
    a.dialer.SetHintForTesting(a.directory.Current().resolved, std::format("127.0.0.1:{}", other.port));
    a.clock.advance(SharedSessionIdleLimit);
    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheStaleHints) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 0);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);

    // And the hint the name's proof recorded is the right address again: the next operation that
    // dials proves at the hint, so nothing more is counted stale.
    a.clock.advance(SharedSessionIdleLimit);
    REQUIRE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheStaleHints) == 1);
}

TEST_CASE("An unresolved or unset shared cache is skipped and says which", "[node][shared-cache][upstream]")
{
    ClientNode a { "pc-7" };
    // Unset: not configured, not a failure, not counted.
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::NotConfigured);
    CHECK_FALSE(a.upstream.Configured());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheUnresolved) == 0);

    // Named, with no endpoint announced yet: configured, declined, counted -- and nothing dialled.
    auto state = NamingCacheC(1);
    state.members.front().schedulerEndpoint.clear(); // cache-c has not said where it answers yet
    a.directory.Applied(state);
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    CHECK(a.upstream.Configured());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheUnresolved) == 1);
    CHECK(a.connector.Ops().empty());
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Unresolved);
    CHECK(a.upstream.Report().detail.contains("0xFC endpoint"));
}

TEST_CASE("A named machine that accepts and never answers is given up within the handshake deadline",
          "[node][shared-cache][upstream][deadline]")
{
    // On a reactor, as production runs it: the deadline is armed only there. The peer stays silent
    // for `SilentFor` and then closes, so without the handshake's own deadline the operation still
    // ends -- as a failed proof, but seconds later. The time is what tells them apart.
    CacheMachine c { "cache-c" }; // the reactor this node runs on
    SilentPeer silent { SilentFor };
    ClientNode a { "pc-7", &c.io, ShortPolicy(400ms) };
    a.directory.Applied(NamingCacheC(silent.port));

    auto const started = std::chrono::steady_clock::now();
    auto const fetched = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("k"); });
    auto const took = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(fetched.has_value());
    CHECK(took < WithinDeadline);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Unreachable);
}

TEST_CASE("A proven session that stops answering is given up within the exchange deadline",
          "[node][shared-cache][upstream][deadline]")
{
    // The proof completes; the SHARED-FETCH after it is held for `SilentFor` and then answered with
    // the value. With the exchange's own deadline the session is closed first and the operation is
    // a miss well inside it; without it, the value arrives seconds later.
    CacheMachine c { "cache-c" };
    ClientNode writer { "pc-8" };
    writer.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(writer.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);

    c.front.Stall(&c.io.Reactor(), Wire::Op::SharedFetch, SilentFor);
    ClientNode a { "pc-7", &c.io, ShortPolicy(400ms) };
    a.directory.Applied(NamingCacheC(c.port));

    auto const started = std::chrono::steady_clock::now();
    auto const fetched = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("k"); });
    auto const took = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(fetched.has_value());
    CHECK(took < WithinDeadline);
    // It proved: what was bounded is the exchange, not the handshake.
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);
}

TEST_CASE("A proven session that stops answering a store is given up within the exchange deadline",
          "[node][shared-cache][upstream][deadline]")
{
    // The store half of the case above: the SHARED-STORE is held for `SilentFor` and then answered
    // Ok. With the store exchange's own deadline the session is closed first and the store is
    // declined well inside it; without it, the store is acknowledged seconds later.
    CacheMachine c { "cache-c" };
    c.front.Stall(&c.io.Reactor(), Wire::Op::SharedStore, SilentFor);
    ClientNode a { "pc-7", &c.io, ShortPolicy(400ms) };
    a.directory.Applied(NamingCacheC(c.port));

    auto const value = AStoredObject();
    auto const started = std::chrono::steady_clock::now();
    auto const stored = OnReactor<UpstreamStore>(c.io, [&a, &value] { return a.upstream.Store("k", value); });
    auto const took = std::chrono::steady_clock::now() - started;

    CHECK(stored == UpstreamStore::Declined);
    CHECK(took < WithinDeadline);
    // It proved: what was bounded is the exchange, not the handshake.
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);
}

TEST_CASE("A dial hint that accepts and never answers costs its own short budget before the name proves",
          "[node][shared-cache][upstream][hint][deadline]")
{
    // The hint's second staleness direction: the address answers the dial, then says nothing. Its
    // handshake has a budget of its own, far below the name's, so the name is dialled and proves in
    // the same operation -- within the hint's budget rather than a whole exchange's.
    CacheMachine c { "cache-c" };
    SilentPeer silent { SilentFor };
    ClientNode a { "pc-7", &c.io, ShortPolicy(2'000ms) };
    a.directory.Applied(NamingCacheC(c.port));
    a.dialer.SetHintForTesting(a.directory.Current().resolved, std::format("127.0.0.1:{}", silent.port));

    auto const value = AStoredObject();
    auto const started = std::chrono::steady_clock::now();
    auto const stored = OnReactor<UpstreamStore>(c.io, [&a, &value] { return a.upstream.Store("k", value); });
    auto const took = std::chrono::steady_clock::now() - started;

    CHECK(stored == UpstreamStore::Stored);
    // Under the name's 2 s handshake budget, which a hint sharing it would have spent in full.
    CHECK(took < WithinDeadline);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheStaleHints) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 0);
}

TEST_CASE("The named machine refusing this node's key is its own answer and not a stale hint",
          "[node][shared-cache][upstream][hint]")
{
    // The right machine answers and proves its key, then refuses THIS node's proof -- its roster does
    // not hold this node. Not a network ("did not answer"), not an impostor, and at the hint not a
    // stale address: the hint reached the right machine.
    CacheMachine c { "cache-c" };
    ClientNode stranger { "pc-6" }; // not in cache-c's roster
    stranger.directory.Applied(NamingCacheC(c.port));
    CHECK(core::async::syncRun(stranger.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    CHECK(stranger.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 1);
    CHECK(stranger.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey) == 0);
    CHECK(stranger.upstream.Report().state == Wire::WireSharedCacheState::ProofRefused);
    CHECK(stranger.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
    CHECK_FALSE(Sent(stranger, Wire::Op::SharedStore));

    // At the hint: a node proven before, whose key the named machine has since stopped holding.
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(a.upstream.Fetch("warm-up")) == std::nullopt);
    REQUIRE(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);
    Testing::PublishKeyRoster(c.roster, { "pc-8", "cache-c" });

    // The session the warm-up proved is still kept, and the server re-judges admission on EVERY
    // verb: a machine it has stopped admitting is refused over its kept session too, before any new
    // proof is asked for -- and that refusal ENDS the session and is said at once.
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::ProofRefused);
    CHECK(a.upstream.Report().detail.contains(SharedStrangerWhy));
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);

    // With no clock step, the next operation dials the hint -- there is nothing kept to reuse -- and
    // meets the refusal at the handshake: the right machine, so not a stale hint.
    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::NodeChallenge)) == 2);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheStaleHints) == 0);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::ProofRefused);
}

TEST_CASE("A kept session the named machine stops admitting ends at its refusal, however steady the build",
          "[node][shared-cache][upstream][session]")
{
    // A build whose misses come closer together than the idle limit never lets a kept session age
    // out. So if a refusal kept the session, the node would be refused at every verb for as long as
    // the build ran, reporting `Proven` with the condition clear. Each operation after the refusal
    // proves afresh instead, and says why it cannot.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(a.upstream.Store("warm-up", AStoredObject())) == UpstreamStore::Stored);
    REQUIRE(a.upstream.Report().state == Wire::WireSharedCacheState::Proven);
    Testing::PublishKeyRoster(c.roster, { "pc-8", "cache-c" });

    constexpr auto Stores = 5;
    for (auto const i: std::views::iota(0, Stores))
    {
        a.clock.advance(SharedSessionIdleLimit - std::chrono::milliseconds { 1 });
        CHECK(core::async::syncRun(a.upstream.Store(std::format("k-{}", i), AStoredObject())) == UpstreamStore::Declined);
    }

    // The warm-up's proof, then one per operation after the one refused over the kept session.
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::NodeChallenge)) == Stores);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == Stores - 1);
    // The kept session was refused once, and then never used again.
    CHECK(c.metrics.Read(IMetricsSink::Counter::NodeSharedCacheRequestsRefusedNotAMember) == 1);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::ProofRefused);
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
}

TEST_CASE("A burst of misses costs one handshake", "[node][shared-cache][upstream][session]")
{
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));

    constexpr auto Misses = 10;
    for (auto const i: std::views::iota(0, Misses))
        CHECK_FALSE(core::async::syncRun(a.upstream.Fetch(std::format("absent-{}", i))).has_value());

    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(c.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);
    // And every one of them was asked of the named machine, on that one session.
    CHECK(c.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == Misses);
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::NodeChallenge)) == 1);
}

TEST_CASE("A session idle past its limit is re-proved rather than reused", "[node][shared-cache][upstream][session]")
{
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    std::ignore = core::async::syncRun(a.upstream.Fetch("one"));

    // Just inside the limit: reused.
    a.clock.advance(SharedSessionIdleLimit - std::chrono::milliseconds { 1 });
    std::ignore = core::async::syncRun(a.upstream.Fetch("two"));
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);

    // Past it: the server would have swept it, so it is not even tried.
    a.clock.advance(SharedSessionIdleLimit);
    std::ignore = core::async::syncRun(a.upstream.Fetch("three"));
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 2);
}

TEST_CASE("A kept session the server closed is re-established within the same operation",
          "[node][shared-cache][upstream][session]")
{
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);

    // The server end went away between operations (a restart, a network blip) and this end has not
    // noticed: the kept session looks open, is handed out, and breaks under the SHARED-FETCH write.
    // A session CLOSED here would say nothing -- the pool never hands one out -- so the break is placed
    // at the write, once.
    a.connector.FailNextWriteOfForTesting(Wire::Op::SharedFetch);
    auto const fetched = core::async::syncRun(a.upstream.Fetch("k"));
    CHECK(fetched.has_value()); // retried once, on a fresh proven session, inside this call
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::SharedFetch)) == 2);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 2);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheProofsFailed) == 0);
}

TEST_CASE("A failure on a fresh session is not retried", "[node][shared-cache][upstream][session]")
{
    // A session proven moments ago whose first exchange finds the connection gone: the machine proved
    // itself on this very connection, so nothing kept went stale, and the failure is the answer. The
    // break is placed under the SHARED-FETCH write, once -- a retry would have succeeded.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    a.connector.FailNextWriteOfForTesting(Wire::Op::SharedFetch);

    CHECK_FALSE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::NodeChallenge)) == 1);
    CHECK(std::ranges::count(a.connector.Ops(), static_cast<std::uint8_t>(Wire::Op::SharedFetch)) == 1);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    // And the broken session was not kept: the next operation proves afresh.
    CHECK_FALSE(core::async::syncRun(a.upstream.Fetch("k")).has_value());
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 2);
}

TEST_CASE("An exchange that expired on a kept session is final, not retried", "[node][shared-cache][upstream][session]")
{
    // A kept session whose exchange runs out its deadline: the machine is there and slow, which a
    // fresh proof does not change -- a retry would spend a handshake and a second whole budget for
    // the same answer. So the expiry is the answer, and only a session that DIED is retried.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7", &c.io, ShortPolicy(400ms) };
    a.directory.Applied(NamingCacheC(c.port));
    std::ignore = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("one"); });
    REQUIRE(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);

    c.front.Stall(&c.io.Reactor(), Wire::Op::SharedFetch, SilentFor);
    auto const started = std::chrono::steady_clock::now();
    auto const fetched = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("two"); });
    auto const took = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(fetched.has_value());
    CHECK(took < WithinDeadline);
    // Reused, expired, and not proved again.
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    CHECK(c.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);
}

TEST_CASE("An idle kept session is hung up by this end before the server sweeps it as silent",
          "[node][shared-cache][upstream][session]")
{
    // The server counts a connection that names no verb within `FrameServer::HeaderTimeout` as a
    // sweep, a series no honest client appears in. The pool's clock never moves here, so only the idle
    // timer on the reactor can end the session; the server's sweep would end it too, a little later,
    // so the counter says WHICH did.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7", &c.io };
    a.directory.Applied(NamingCacheC(c.port));
    std::ignore = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("one"); });
    REQUIRE(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    REQUIRE(c.endpoint->OpenConnections() == 1);

    auto const idleFrom = std::chrono::steady_clock::now();
    REQUIRE(Testing::WaitUntil(
        "the idle kept session's connection to end",
        [&c] { return c.endpoint->OpenConnections() == 0; },
        [&c] { return std::format("{} connection(s) open", c.endpoint->OpenConnections()); },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = 2 * FrameServer::HeaderTimeout }));
    auto const idled = std::chrono::steady_clock::now() - idleFrom;

    CHECK(c.metrics.Read(IMetricsSink::Counter::FrameRequestDeadlineSweeps) == 0);
    // Ended by the idle limit, well inside the server's own window.
    CHECK(idled >= SharedSessionIdleLimit - SharedSessionHangUpMargin / 2);
    CHECK(idled < FrameServer::HeaderTimeout);
}

TEST_CASE("A pool destroyed off its reactor hands its kept sessions to the reactor, which still hangs them up",
          "[node][shared-cache][upstream][session]")
{
    // A kept session is a reactor socket with a reactor timer on it, and this case destroys the pool
    // on ITS thread while the reactor runs -- which `TeardownIsSerialisedWithDispatch` forbids for
    // anything the reactor owns. So the session must reach the home rather than be freed here, and its
    // idle timer must still hang it up, on the reactor, before the server would sweep it.
    CacheMachine c { "cache-c" };
    CountingHome home { c.io };
    {
        ClientNode a { "pc-7", &c.io, {}, &home };
        a.directory.Applied(NamingCacheC(c.port));
        std::ignore = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&a] { return a.upstream.Fetch("one"); });
        REQUIRE(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    } // the pool goes here, on the case's thread
    CHECK(home.Retired() == 1);
    CHECK(c.endpoint->OpenConnections() == 1); // not closed off the reactor

    REQUIRE(Testing::WaitUntil(
        "the orphaned session's connection to end",
        [&c] { return c.endpoint->OpenConnections() == 0; },
        [&c] { return std::format("{} connection(s) open", c.endpoint->OpenConnections()); },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = 2 * FrameServer::HeaderTimeout }));
    CHECK(c.metrics.Read(IMetricsSink::Counter::FrameRequestDeadlineSweeps) == 0);
}

TEST_CASE("A kept session is dropped when the named machine changes", "[node][shared-cache][upstream][session]")
{
    CacheMachine c { "cache-c" };
    CacheMachine d { "cache-d" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    std::ignore = core::async::syncRun(a.upstream.Fetch("one"));
    auto const provenAtC = c.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted);

    a.directory.Applied(NamingCacheD(d.port));
    std::ignore = core::async::syncRun(a.upstream.Fetch("two"));
    CHECK(c.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == provenAtC);
    CHECK(d.metrics.Read(IMetricsSink::Counter::NodeProofsAccepted) == 1);
    CHECK(c.opener.metrics.Read(IMetricsSink::Counter::NodeSharedCacheMisses) == 1); // "two" never reached C
}

TEST_CASE("The pool hands a kept session only to the target it was proven for, and only while it is young",
          "[node][shared-cache][upstream][session]")
{
    // The pool's own two guards, asked of the pool alone: they are the ONLY place a session proven
    // for one target is refused to another, so they are pinned here as well as through the upstream. A
    // kept session is any open sealed socket here; a target that names no endpoint is one the dialer
    // refuses without dialling, so a miss in the pool is visible as a lease neither reused nor proven.
    ClientNode a { "pc-7" };
    auto const target = [](std::string const& machine) {
        return Cluster::ResolvedSharedCache { .resolution = Cluster::SharedCacheResolution::Resolved,
                                              .machineId = machine,
                                              .endpoint = {},
                                              .key = Testing::TestKeyPair(machine).PublicKey() };
    };
    auto keep = [&a](Cluster::ResolvedSharedCache const& provenFor) {
        auto pair = core::net::testing::InMemorySocketPair::create();
        auto session = std::make_unique<SealedFrameSocket>(std::move(pair.client), SealedFrameEnd::Caller, 4096, nullptr);
        a.sessions.Give(provenFor, std::move(session));
        return std::move(pair.server); // the far end, kept open by the case
    };

    // Kept for cache-c: cache-c gets it back.
    auto const farC = keep(target("cache-c"));
    CHECK(core::async::syncRun(a.sessions.Take(target("cache-c"))).reused);

    // Kept for cache-c: cache-d does not, and the dialer is asked instead.
    auto const farC2 = keep(target("cache-c"));
    auto const forD = core::async::syncRun(a.sessions.Take(target("cache-d")));
    CHECK_FALSE(forD.reused);
    CHECK(forD.socket == nullptr);

    // Kept, then aged to the limit: not handed out.
    auto const farC3 = keep(target("cache-c"));
    a.clock.advance(SharedSessionIdleLimit);
    auto const aged = core::async::syncRun(a.sessions.Take(target("cache-c")));
    CHECK_FALSE(aged.reused);

    // Kept to capacity: a second session offered back is closed rather than kept.
    auto const farC4 = keep(target("cache-c"));
    auto const farC5 = keep(target("cache-c"));
    CHECK(core::async::syncRun(a.sessions.Take(target("cache-c"))).reused);
    CHECK_FALSE(core::async::syncRun(a.sessions.Take(target("cache-c"))).reused);

    // Kept for cache-c: the same id under ANOTHER key does not get it. A machine forgotten and
    // admitted again keeps its id, and what its old key proved is worth nothing.
    auto const farC6 = keep(target("cache-c"));
    auto rekeyed = target("cache-c");
    rekeyed.key = Testing::TestKeyPair("cache-c-rekeyed").PublicKey();
    CHECK_FALSE(core::async::syncRun(a.sessions.Take(rekeyed)).reused);

    // Kept for cache-c at one endpoint: the same id and key announced ELSEWHERE does not get it.
    auto moved = target("cache-c");
    moved.endpoint = "127.0.0.1:1";
    auto const farC7 = keep(moved);
    CHECK_FALSE(core::async::syncRun(a.sessions.Take(target("cache-c"))).reused);
}

TEST_CASE("The shared-cache condition follows an applied state, not only an operation",
          "[node][shared-cache][upstream][conditions]")
{
    // `shared-cache-unproven` is a LIVE row: it describes the state now. So the apply re-judges it,
    // rather than leaving the last failure raised -- naming a machine nobody asks about any more --
    // until some build happens to miss.
    auto const gone = CacheMachine { "cache-c" }.port;
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(gone));
    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    REQUIRE(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);

    SECTION("the same machine: what the last operation found still describes it")
    {
        a.directory.Applied(NamingCacheC(gone));
        a.upstream.StateApplied();
        CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
        CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Unreachable);
    }
    SECTION("unset: cleared at the apply")
    {
        a.directory.Applied(Cluster::ClusterState {});
        a.upstream.StateApplied();
        CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
    }
    SECTION("another machine: not-evaluated, and reported as not tried -- never clear")
    {
        a.directory.Applied(NamingCacheD(gone));
        a.upstream.StateApplied();
        CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::NotEvaluated);
        CHECK(a.upstream.Report().machineId == "cache-d");
        CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::NotTried);
    }
}

TEST_CASE("A setting naming a machine this node cannot reach by key raises the condition at the apply",
          "[node][shared-cache][upstream][conditions]")
{
    // Named by the setting and recorded nowhere: no key, no endpoint. Said when the state is applied,
    // from a condition nothing has raised yet -- not at the first build that happens to miss -- and
    // counted as nothing, since an apply is not an operation.
    ClientNode a { "pc-7" };
    REQUIRE(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
    a.directory.Applied(Testing::NamingSharedCache("ghost"));
    a.upstream.StateApplied();
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
    CHECK(a.upstream.Report().machineId == "ghost");
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::Unresolved);
    CHECK(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheUnresolved) == 0);
}

/// @return A state naming `cache-c` with its key recorded and no endpoint announced yet.
[[nodiscard]] Cluster::ClusterState NamingUnannouncedCacheC()
{
    auto state = NamingCacheC(1);
    state.members.front().schedulerEndpoint.clear();
    return state;
}

TEST_CASE("A machine named before it could be reached is re-judged at the apply that resolves it",
          "[node][shared-cache][upstream][conditions]")
{
    // The ordinary order: an operator names a machine before it has announced its endpoint. Every
    // node raises the row at that apply -- and when the machine announces, every IDLE node must
    // clear it there, since no operation is coming to. `Unresolved` is a fact about the state.
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingUnannouncedCacheC());
    a.upstream.StateApplied();
    REQUIRE(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Raised);
    REQUIRE(a.upstream.Report().state == Wire::WireSharedCacheState::Unresolved);

    a.directory.Applied(NamingCacheC(1));
    // The report follows the directory on its own, before any apply hook runs.
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::NotTried);
    a.upstream.StateApplied();
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::NotEvaluated);
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::NotTried);
    CHECK(a.connector.Ops().empty());
}

TEST_CASE("An operation still running when an apply moves the setting says nothing about the machine it dialled",
          "[node][shared-cache][upstream][conditions]")
{
    // Interleaved, not raced: the apply lands INSIDE the operation, after it took `cache-c` as its
    // target and before it learns that `cache-c` did not answer. The apply has already judged
    // `cache-d`; the late verdict about `cache-c` must not re-raise the row naming it.
    auto const gone = CacheMachine { "cache-c" }.port;
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(gone));
    a.upstream.StateApplied();
    auto moved = false;
    a.connector.BeforeNextDialForTesting([&a, &moved, gone] {
        a.directory.Applied(NamingCacheD(gone));
        a.upstream.StateApplied();
        moved = true;
    });

    CHECK(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Declined);
    REQUIRE(moved);
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::NotEvaluated);
    CHECK(a.upstream.Report().machineId == "cache-d");
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::NotTried);
}

/// What `main` builds and wires for the fleet half: `MakeCacheUpstream` over parts naming a host, so
/// the switch between the proven and in-process halves, told of every state through the consensus
/// tier's `SharedCacheListeners`. Nothing names this node, so the switch reads through the proven half.
struct AppliedThroughListeners
{
    /// @return The parts `main` would pass, over this fixture's loop and host.
    [[nodiscard]] UpstreamParts Parts() noexcept
    {
        return UpstreamParts { .upstream = {},
                               .credential = credential,
                               .directory = a.directory,
                               .prover = &a.prover,
                               .io = loop.io,
                               .clock = a.clock,
                               .metrics = a.metrics,
                               .conditions = &a.conditions,
                               .logger = a.logger,
                               .host = &host };
    }

    /// Apply @p state the way the consensus tier's apply callback does.
    /// @param state The state the cluster applied.
    void Apply(Cluster::ClusterState const& state)
    {
        SharedCacheListeners { .directory = a.directory, .host = host, .upstream = built.get() }.Applied(state);
    }

    /// @return The condition's state.
    [[nodiscard]] Wire::ConditionState Unproven() const
    {
        return a.conditions.StateOf(NodeCondition::SharedCacheUnproven);
    }

    ClientNode a { "pc-7" };
    MemoryOpener opener;
    SharedCacheHost host { "pc-7", opener, nullptr, a.logger, ReconcileOn::Caller };
    NodeConfig unauthenticated;
    ConfiguredCredential credential { unauthenticated, nullptr };
    /// A loop that TURNS, which a bare `NodeIoLoop` does not: `Start()` runs no thread for one that
    /// adopted no server. Production never meets that -- every upstream operation is reached from an
    /// answer its surface serves on this very loop -- so the loop borrowed is a serving one's.
    CacheMachine loop { "cache-d" };
    std::unique_ptr<ICacheUpstream> built { MakeCacheUpstream(Parts()) };
};

TEST_CASE("Each state the consensus tier applies re-judges the built fleet upstream's condition",
          "[node][shared-cache][upstream][conditions][wiring]")
{
    // The three re-judgements, reached the way production reaches them -- through the listener set
    // the apply callback calls, into the switch `MakeCacheUpstream` builds -- rather than by calling
    // `SharedCacheUpstream::StateApplied` on an object `main` never holds.
    AppliedThroughListeners node;
    REQUIRE(UpstreamKindOf(node.Parts()) == UpstreamKind::FleetSharedCache);
    auto const gone = CacheMachine { "cache-c" }.port;
    auto const value = AStoredObject();
    node.Apply(NamingCacheC(gone));
    REQUIRE(OnReactor<UpstreamStore>(node.loop.io, [&node, &value] { return node.built->Store("k", value); })
            == UpstreamStore::Declined);
    REQUIRE(node.Unproven() == Wire::ConditionState::Raised);

    SECTION("the same machine: kept, since what the last operation found still describes it")
    {
        node.Apply(NamingCacheC(gone));
        CHECK(node.Unproven() == Wire::ConditionState::Raised);
    }
    SECTION("out of the setting: cleared at the apply")
    {
        node.Apply(Cluster::ClusterState {});
        CHECK(node.Unproven() == Wire::ConditionState::Clear);
    }
    SECTION("a newly resolved machine: not-evaluated at the apply, nothing tried there yet")
    {
        node.Apply(NamingCacheD(gone));
        CHECK(node.Unproven() == Wire::ConditionState::NotEvaluated);
    }
}

TEST_CASE("A state naming a machine nobody can reach by key raises the built fleet upstream's condition at the apply",
          "[node][shared-cache][upstream][conditions][wiring]")
{
    // From Clear, so the raise is the apply's and not a leftover of an operation.
    AppliedThroughListeners node;
    REQUIRE(node.Unproven() == Wire::ConditionState::Clear);
    node.Apply(Testing::NamingSharedCache("ghost"));
    CHECK(node.Unproven() == Wire::ConditionState::Raised);
}

TEST_CASE("A machine that becomes reachable by key takes the built fleet upstream's condition out of raised",
          "[node][shared-cache][upstream][conditions][wiring]")
{
    // Out of raised at the apply, and not into clear: reachable is not reached, so the row is
    // not-evaluated until an operation tries the machine.
    AppliedThroughListeners node;
    node.Apply(NamingUnannouncedCacheC());
    REQUIRE(node.Unproven() == Wire::ConditionState::Raised);
    node.Apply(NamingCacheC(1));
    CHECK(node.Unproven() == Wire::ConditionState::NotEvaluated);
}

/// @return What @p conditions sends for `shared-cache-unproven`.
[[nodiscard]] Wire::NodeConditionFields SentUnproven(NodeConditions const& conditions)
{
    auto const rows = conditions.Snapshot();
    auto const found =
        std::ranges::find(rows, RowFor(NodeCondition::SharedCacheUnproven).id, &Wire::NodeConditionFields::id);
    REQUIRE(found != rows.end());
    return *found;
}

TEST_CASE("A shared cache nobody has tried reads not-evaluated with its reason, never clear, until an operation decides",
          "[node][shared-cache][upstream][conditions]")
{
    // Not tried is NEITHER failing to reach the named machine nor reaching it, so the row says so --
    // with the reason an operator reads -- and only an operation moves it: here to clear, once the key
    // is proven.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    a.upstream.StateApplied();
    auto const untried = SentUnproven(a.conditions);
    CHECK(untried.state == "not-evaluated");
    CHECK(untried.detail.starts_with("not tried:"));
    CHECK(untried.detail.contains("cache-c"));
    CHECK(a.upstream.Report().state == Wire::WireSharedCacheState::NotTried);
    CHECK(a.connector.Ops().empty());

    // An upstream built while the setting already names the machine answers the same from the start,
    // not only from the next apply.
    NodeConditions fresh;
    SharedCacheUpstream const late { a.directory, a.sessions, a.reactor, a.metrics, &fresh, a.logger, a.policy };
    CHECK(fresh.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::NotEvaluated);

    REQUIRE(core::async::syncRun(a.upstream.Store("k", AStoredObject())) == UpstreamStore::Stored);
    CHECK(a.conditions.StateOf(NodeCondition::SharedCacheUnproven) == Wire::ConditionState::Clear);
}

TEST_CASE("The fleet's shared cache is built on one loop: its idle session is hung up there, before the sweep",
          "[node][shared-cache][upstream][session][wiring]")
{
    // `MakeCacheUpstream` is what `main` calls, and the one place the dialer's connector and the
    // pool's home are chosen: both from the parts' single loop. Here that loop runs the named
    // machine's listener too, so a session dialled on it and kept by the pool must be hung up by
    // THIS end on the loop's own timer, and the server's sweep of a silent peer must stay at zero --
    // which a pool whose home was another loop, or none, could not do.
    CacheMachine c { "cache-c" };
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(c.port));
    NodeConfig unauthenticated;
    ConfiguredCredential const credential { unauthenticated, nullptr };
    UpstreamParts const parts { .upstream = {},
                                .credential = credential,
                                .directory = a.directory,
                                .prover = &a.prover,
                                .io = c.io,
                                .clock = a.clock,
                                .metrics = a.metrics,
                                .conditions = &a.conditions,
                                .logger = a.logger,
                                .host = nullptr };
    REQUIRE(UpstreamKindOf(parts) == UpstreamKind::FleetSharedCache);
    auto const built = MakeCacheUpstream(parts);
    std::ignore = OnReactor<std::optional<std::vector<std::byte>>>(c.io, [&built] { return built->Fetch("one"); });
    REQUIRE(a.metrics.Read(IMetricsSink::Counter::NodeSharedCacheSessionsOpened) == 1);
    REQUIRE(c.endpoint->OpenConnections() == 1);

    auto const idleFrom = std::chrono::steady_clock::now();
    REQUIRE(Testing::WaitUntil(
        "the built upstream's idle session to end",
        [&c] { return c.endpoint->OpenConnections() == 0; },
        [&c] { return std::format("{} connection(s) open", c.endpoint->OpenConnections()); },
        Testing::WaitOptions { .step = {}, .context = {}, .bound = 2 * FrameServer::HeaderTimeout }));
    auto const idled = std::chrono::steady_clock::now() - idleFrom;
    CHECK(c.metrics.Read(IMetricsSink::Counter::FrameRequestDeadlineSweeps) == 0);
    CHECK(idled < FrameServer::HeaderTimeout);
}

namespace
{

/// The directory's own answer -- after which, on the FIRST read only, it applies @p next: an apply
/// landing between one read of a report and any second read the report might make.
class ApplyAfterFirstRead final: public ISharedCacheTargetSource
{
  public:
    /// @param directory The directory read and then applied to; must outlive this.
    /// @param next The state applied after the first read.
    ApplyAfterFirstRead(SharedCacheDirectory& directory, Cluster::ClusterState next):
        _directory { directory },
        _next { std::move(next) }
    {
    }

    [[nodiscard]] SharedCacheTarget Current() const override
    {
        auto target = _directory.Current();
        if (!_applied)
        {
            _applied = true;
            _directory.Applied(_next);
        }
        return target;
    }

  private:
    SharedCacheDirectory& _directory;
    Cluster::ClusterState _next;
    mutable bool _applied { false };
};

/// An advertised endpoint that never moves.
class FixedAdvertised final: public Cc::IAdvertisedEndpointSource
{
  public:
    [[nodiscard]] std::string Current() const override
    {
        return "10.0.0.7:6674";
    }
};

} // namespace

TEST_CASE("A node-status report straddling an apply describes one state, never a mix of two", "[node][shared-cache][status]")
{
    // The report reads the directory once, and every part is judged against that read. An apply that
    // names THIS machine lands right after it: a report whose fleet half read the directory again
    // would carry `ThisMachine` from the second read and the fleet half's `NotTried` verdict -- a
    // state the cluster was never in.
    ClientNode a { "pc-7" };
    a.directory.Applied(NamingCacheC(1));
    REQUIRE(a.directory.Current().source == Wire::WireSharedCacheSource::Setting);

    NullLogger logger;
    MemoryOpener opener;
    SharedCacheHost host { "pc-7", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(Testing::NamingSharedCache("pc-7"));
    host.Reconcile();
    REQUIRE(host.Status().serving);

    ApplyAfterFirstRead const straddling { a.directory, Testing::NamingSharedCache("pc-7") };
    FixedAdvertised const advertised;
    NodeSharedCacheStatus const status { straddling, &a.upstream, &host, advertised };

    // The snapshot the report took: cache-c, named by the setting.
    auto const report = status.Report();
    CHECK(report.source == Wire::WireSharedCacheSource::Setting);
    CHECK(report.machineId == "cache-c");
    CHECK_FALSE(
        (report.source == Wire::WireSharedCacheSource::ThisMachine && report.state == Wire::WireSharedCacheState::NotTried));

    // And the next report, with nothing in flight, describes the state the apply left.
    auto const after = status.Report();
    CHECK(after.source == Wire::WireSharedCacheSource::ThisMachine);
    CHECK(after.state == Wire::WireSharedCacheState::Serving);
    CHECK(after.endpoint == "10.0.0.7:6674");
}
