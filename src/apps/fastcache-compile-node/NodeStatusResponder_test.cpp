// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"
#include "MachineStandingTestUtils.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeRoster.hpp"
#include "NodeStatusResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/MembershipFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ListedMembership;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The live-stats sources `NodeMetrics` reads, over a snapshot a case scripts.
///
/// **The production capture, not a stand-in for it**: `Capture(Cache)` is `CaptureCacheSubject`, the
/// function the node's own sources call, so what a case reads back is what a node would send.
class CapturedReadings final: public ILiveStatsSources
{
  public:
    /// @param metrics The counters a capture reads; must outlive this.
    /// @param scripted What the snapshot provider states.
    CapturedReadings(IMetricsSink const& metrics, MetricsSnapshot scripted) noexcept:
        _snapshot { std::move(scripted) },
        _metrics { metrics }
    {
    }

    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject subject) const override
    {
        if (!_attached || subject != Wire::LiveSubject::Cache)
            return std::nullopt;
        // The node's own set, through the function production calls (#1484, #1501). Left at
        // the default it answered for a process serving every surface, so the case compared the
        // responder's reading against a capture the node would never produce -- a fake more
        // permissive than the thing it stands for, which is the one kind of fixture defect
        // reading the fake never finds.
        auto const served = NodeServedSurfacesFor(Testing::FirstStart(NodeConfig {}));
        return CaptureCacheSubject(_metrics, _snapshot, served.Span());
    }

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return std::nullopt;
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "n1.test:6674";
    }

    /// @copydoc ILiveStatsSources::FleetText
    ///
    /// No fleet: `NodeMetrics` reads a node's own tier, and a case here that reached the fleet
    /// document would be asserting something this responder never renders.
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = {} });
    }

    /// Detach the sources, as a node does once it is stopping: every capture after this answers nothing.
    void Detach() noexcept
    {
        _attached = false;
    }

  private:
    MetricsSnapshot _snapshot;
    IMetricsSink const& _metrics;
    bool _attached { true };
};

/// The peer every `AnswerNow` below arrives from.
///
/// Named rather than spelled at each site, because half these cases turn on it being admitted
/// and the other half on it not being. Not this machine, so a fake admitting it stands for the
/// open policy -- the one route that admits a remote host by address.
constexpr std::string_view CallerAddress = "10.0.0.7";

/// Which port each surface is given, so an assertion can name one number.
constexpr std::uint32_t AdminPort = 9101;
constexpr std::uint32_t RaftPort = 9102;
constexpr std::uint32_t DiscoveryPort = 9103;

/// A request frame naming @p op and carrying nothing.
/// @param op The verb.
/// @return The encoded frame.
[[nodiscard]] std::vector<std::byte> HeaderFor(Wire::Op op)
{
    std::vector<std::byte> frame(Wire::RequestHeaderSize);
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(op), 0);
    return frame;
}

/// What every case that does not ask `explain-admission` about a machine is handed: no roster.
Testing::FixedStanding const HoldsNoRoster {};

/// Run a responder's `Answer` to completion.
///
/// Every arm here is synchronous -- these verbs read nothing and suspend nowhere -- so
/// the coroutine has already finished by the time the task is handed back.
/// @param responder What to ask.
/// @param frame The request.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> AnswerNow(IFrameResponder& responder, std::span<std::byte const> frame)
{
    return core::async::syncRun(responder.Answer(frame, PeerIdentity { .host = std::string { CallerAddress } })).bytes;
}

/// Run a responder's `Answer` to completion, from @p peer.
/// @param responder What to ask.
/// @param frame The request.
/// @param peer Who asks, and what its connection established.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> AnswerFrom(IFrameResponder& responder,
                                                std::span<std::byte const> frame,
                                                PeerIdentity peer)
{
    return core::async::syncRun(responder.Answer(frame, std::move(peer))).bytes;
}

/// The bytes following a reply header.
/// @param reply The whole reply frame.
/// @param header Its already-decoded header.
/// @return The payload.
[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> reply, Wire::ReplyHeader const& header)
{
    REQUIRE(reply.size() >= Wire::ReplyHeaderSize + header.payloadLength);
    return reply.subspan(Wire::ReplyHeaderSize, header.payloadLength);
}

/// The status byte and error code a reply carries.
struct ReplyShape
{
    Wire::Status status {}; ///< What the reply said.

    /// The refusal code, for an error reply; disengaged for an `Ok` one.
    ///
    /// An `optional` rather than a default-constructed `ErrorCode`, which names a value
    /// that is not an enumerator -- the enum has no zero. Picking an arbitrary one to
    /// satisfy the analyser would be worse than the warning: it asserts a refusal that
    /// did not happen, in a field a case then reads.
    std::optional<Wire::ErrorCode> code {};

    std::string detail; ///< Whatever words came with it.
};

/// Read @p reply back as a client would.
/// @param reply The encoded reply.
/// @return Its shape.
[[nodiscard]] ReplyShape ShapeOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    ReplyShape shape;
    shape.status = Unwrap(header).status;
    if (shape.status != Wire::Status::Ok)
    {
        auto const refusal = Wire::DecodeErrorPayload(PayloadOf(reply, Unwrap(header)));
        REQUIRE(refusal.has_value());
        shape.code = Unwrap(refusal).first;
        shape.detail = std::string { Unwrap(refusal).second };
    }
    return shape;
}

/// A node configuration that resolves exactly the surfaces named.
struct ConfigShape
{
    bool admin { false };         ///< Give `--listen-admin` an address.
    bool raft { false };          ///< Give `--listen-raft` an address.
    bool discovery { false };     ///< Give `--discovery` an address.
    bool tlsPair { false };       ///< Name a certificate AND a key.
    bool tlsCertOnly { false };   ///< Name a certificate and no key.
    bool tlsSelfSigned { false }; ///< Ask for material to be made.
    bool raftWildcard { false };  ///< Give `--listen-raft` a bare port, which binds the wildcard.
    std::string_view raftSelf {}; ///< `--raft-self`, or nothing.
};

/// Build a configuration of that shape.
/// @param shape What to resolve.
/// @return The configuration.
[[nodiscard]] NodeConfig NodeConfigOf(ConfigShape const& shape)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.nodeListen = "127.0.0.1:0";
    if (shape.admin)
        cfg.adminListen = std::format("127.0.0.1:{}", AdminPort);
    // Every surface off unless the shape asks for it -- consensus and discovery are on by
    // default, so a shape naming neither turns both off, as `--listen-raft=` and
    // `--discovery=` would.
    cfg.raftListen.clear();
    if (shape.raft)
        cfg.raftListen = shape.raftWildcard ? std::format("{}", RaftPort) : std::format("127.0.0.1:{}", RaftPort);
    cfg.raftSelf = std::string { shape.raftSelf };
    cfg.discoveryAddress = shape.discovery ? std::format("0.0.0.0:{}", DiscoveryPort) : std::string {};
    if (shape.tlsPair || shape.tlsCertOnly)
        cfg.tlsCertFile = "cert.pem";
    if (shape.tlsPair)
        cfg.tlsKeyFile = "key.pem";
    cfg.tlsSelfSigned = shape.tlsSelfSigned;
    return cfg;
}

/// What the unwired arm parks in the state object nothing is supposed to read.
///
/// **Deliberately a reading nobody could mistake for silence.** The *absent* case below
/// asserts that a null `NodeRuntimeSources::runtime` is honoured, and parking a DEFAULT
/// reading there would let an implementation that ignores the null pass by coincidence:
/// `Surveying`, zero of zero is exactly what "this node reported nothing" looks like
/// from outside, so the two states would be indistinguishable in the one case written to
/// distinguish them.
constexpr ToolchainReading UnwiredSentinel { .state = Wire::ToolchainState::Serving, .served = 99, .discovered = 99 };

/// The sources a case wires DIRECTLY, as opposed to the one the fixture owns.
///
/// Separate from the fixture's own `NodeRuntimeState` because these two are read LIVE
/// rather than published into, and a case that wires them owns them: a `CompileCapacity`
/// needs a logger and a `SchedulerService` needs four collaborators, none of which a
/// fixture should be inventing on a case's behalf.
struct DirectSources
{
    CompileCapacity const* capacity { nullptr };                ///< Live slot accounting.
    Distributed::SchedulerService const* scheduler { nullptr }; ///< Role and known leader.
    /// Where consensus counts this node; null is a node running no consensus (#1449).
    IConsensusStandingSource const* consensus { nullptr };
    /// The node's condition registry; null is a build that reports none (#1364).
    NodeConditions const* conditions { nullptr };
    /// The enrollment list; null is a node that serves none.
    EnrollmentWindow const* enrollment { nullptr };
    /// What the node says about the fleet's shared cache; null is a build that carries none.
    ISharedCacheStatusSource const* sharedCache { nullptr };
    /// The roster the node verifies grants against; null is nothing wired.
    NodeRoster const* roster { nullptr };
};

/// A `ConfiguredNodeStatus` beside the configuration it holds a reference to.
///
/// Held together deliberately, and this fixture is a FINDING rather than a convenience.
/// `ConfiguredNodeStatus` binds `NodeConfig const&` the way production does --
/// `WorkerBody`'s parameter outlives the whole body -- so a case written as
/// `ConfiguredNodeStatus s { NodeConfigOf(shape), ... }` reads freed memory on its very
/// next line. It did: of the four cases built that way, two reported a wrong PORT and a
/// wrong TLS flag and two PASSED, which is what a small dangling read looks like on the
/// platform this was written on. There is now no way to spell the temporary.
struct Fixture
{
    /// @param shape Which surfaces to resolve.
    /// @param clock Where uptime comes from; must outlive this.
    /// @param components What this node is to report running.
    /// @param initial What the worker has published, or nothing to leave the runtime
    ///        source UNWIRED -- which is a node that publishes no such facts, not one
    ///        that publishes empty ones.
    /// @param direct Sources this case owns and the fixture only points at.
    Fixture(ConfigShape const& shape,
            core::platform::IClock const& clock,
            NodeComponents components = {},
            std::optional<ToolchainReading> initial = std::nullopt,
            DirectSources direct = {}):
        cfg { NodeConfigOf(shape) },
        runtime { initial.value_or(UnwiredSentinel) },
        status { cfg,
                 clock,
                 clock.now(),
                 "1.2.3",
                 "node-a",
                 components,
                 NodeRuntimeSources { .runtime = initial.has_value() ? &runtime : nullptr,
                                      .capacity = direct.capacity,
                                      .scheduler = direct.scheduler,
                                      .enrollment = direct.enrollment,
                                      .consensus = direct.consensus,
                                      .conditions = direct.conditions,
                                      .roster = direct.roster,
                                      .sharedCache = direct.sharedCache } }
    {
    }

    Fixture(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;

    // Declaration order IS construction order, and `status` below binds both `cfg` and
    // `runtime` above it.
    NodeConfig cfg;              ///< What the operator asked for.
    NodeRuntimeState runtime;    ///< Where the worker publishes; read only when wired.
    ConfiguredNodeStatus status; ///< What the node answers with.
};

/// The reported surface of that tag, or nothing when the node reported none.
/// @param fields What `NodeStatus` answered.
/// @param tag Which surface to look for.
/// @return The row, or `std::nullopt`.
[[nodiscard]] std::optional<Wire::SurfaceReport> SurfaceOf(Wire::NodeStatusFields const& fields, Wire::WireSurface tag)
{
    auto const it = std::ranges::find(fields.surfaces, tag, &Wire::SurfaceReport::surface);
    return it == fields.surfaces.end() ? std::nullopt : std::optional { *it };
}

} // namespace

TEST_CASE("A node reports only the surfaces its configuration resolves", "[node][node-status]")
{
    // **Absent is not zero, at the CELL.** A client must be able to tell *this node runs
    // no admin surface* from *it runs one whose port I could not read*, and a reported
    // `0` renders as a dialable-looking number in every format that carries it. The
    // discriminating assertion is therefore that the ROW is missing, never that some
    // port is zero -- an implementation pushing `{ Admin, 0 }` passes any test that only
    // reads ports.
    core::platform::ManualClock clock;
    Fixture const adminOnly { { .admin = true }, clock };
    auto const fields = adminOnly.status.Describe();

    CHECK(SurfaceOf(fields, Wire::WireSurface::Admin).has_value());
    CHECK_FALSE(SurfaceOf(fields, Wire::WireSurface::Raft).has_value());
    CHECK_FALSE(SurfaceOf(fields, Wire::WireSurface::Discovery).has_value());
    CHECK(Unwrap(SurfaceOf(fields, Wire::WireSurface::Admin)).port == AdminPort);

    SECTION("and all three when all three are configured")
    {
        // Consensus on the wildcard: a defaulted discovery stands down beside a consensus address
        // that reaches only this machine, which a loopback one would be.
        Fixture const all { { .admin = true, .raft = true, .discovery = true, .raftWildcard = true }, clock };
        auto const everything = all.status.Describe();
        CHECK(Unwrap(SurfaceOf(everything, Wire::WireSurface::Admin)).port == AdminPort);
        CHECK(Unwrap(SurfaceOf(everything, Wire::WireSurface::Raft)).port == RaftPort);
        CHECK(Unwrap(SurfaceOf(everything, Wire::WireSurface::Discovery)).port == DiscoveryPort);

        // The `0xFC` port is reported by NOBODY, on purpose: a client learns it by
        // DIALLING it -- it is already connected to have asked at all -- so reporting it
        // would be a field that can only ever be right or stale. Asserted because
        // `WireSurface`'s omission is a decision somebody will otherwise read as an
        // oversight and helpfully fix.
        CHECK(everything.surfaces.size() == 3);
    }

    SECTION("and none at all when none is")
    {
        Fixture const bare { {}, clock };
        CHECK(bare.status.Describe().surfaces.empty());
    }
}

TEST_CASE("The reported admin scheme follows the TLS MATERIAL, not a boolean", "[node][node-status]")
{
    // **There is deliberately no `--tls` flag to read.** TLS is on by naming material
    // (`--tls-cert` with `--tls-key`) or by asking for material to be made
    // (`--tls-self-signed`), which is what the config table refuses to let a bare boolean
    // stand in for. The middle arm is the discriminating one: an implementation testing
    // `!tlsCertFile.empty()` alone passes the first and third and reports a TLS admin
    // surface this node cannot bind.
    core::platform::ManualClock clock;

    auto reported = [&clock](ConfigShape const& shape) {
        Fixture const fixture { shape, clock };
        return Unwrap(SurfaceOf(fixture.status.Describe(), Wire::WireSurface::Admin)).tls;
    };

    CHECK(reported({ .admin = true, .tlsPair = true }));
    CHECK_FALSE(reported({ .admin = true, .tlsCertOnly = true }));
    CHECK(reported({ .admin = true, .tlsSelfSigned = true }));
    CHECK_FALSE(reported({ .admin = true }));

    SECTION("and it is the ADMIN surface's scheme, never the node's")
    {
        // Raft and discovery carry `tls = false` whatever the admin material says: this
        // is a per-surface column, and one TLS flag folded across the map would tell a
        // client to speak TLS to a UDP beacon.
        Fixture const fixture { { .admin = true, .raft = true, .discovery = true, .tlsPair = true, .raftWildcard = true },
                                clock };
        auto const fields = fixture.status.Describe();
        CHECK(Unwrap(SurfaceOf(fields, Wire::WireSurface::Admin)).tls);
        CHECK_FALSE(Unwrap(SurfaceOf(fields, Wire::WireSurface::Raft)).tls);
        CHECK_FALSE(Unwrap(SurfaceOf(fields, Wire::WireSurface::Discovery)).tls);
    }
}

TEST_CASE("Uptime is re-read per call, not captured once", "[node][node-status]")
{
    // A snapshot taken at construction reports a constant, which every single-call test
    // agrees with. Two calls across an advanced clock is what separates them.
    core::platform::ManualClock clock;
    Fixture const fixture { {}, clock };

    CHECK(fixture.status.Describe().uptimeSeconds == 0);
    clock.advance(90s);
    CHECK(fixture.status.Describe().uptimeSeconds == 90);
    clock.advance(30s);
    CHECK(fixture.status.Describe().uptimeSeconds == 120);
}

TEST_CASE("Component bits report what started, one bit each", "[node][node-status]")
{
    // Asserted per bit rather than against a total, because a total is one number four
    // wrong assignments can produce: swap `Worker` and `Scheduler` and every node running
    // both still reports the same word.
    core::platform::ManualClock clock;
    namespace Bits = Wire::NodeComponentBit;

    auto bitsFor = [&clock](NodeComponents const& components) {
        Fixture const fixture { {}, clock, components };
        return fixture.status.Describe().components;
    };

    CHECK(bitsFor({ .cacheTier = true }) == Bits::CacheTier);
    CHECK(bitsFor({ .worker = true }) == Bits::Worker);
    CHECK(bitsFor({ .scheduler = true }) == Bits::Scheduler);
    CHECK(bitsFor({ .consensus = true }) == Bits::Consensus);
    CHECK(bitsFor({}) == 0U);
    CHECK(bitsFor({ .cacheTier = true, .worker = true, .scheduler = true, .consensus = true })
          == (Bits::CacheTier | Bits::Worker | Bits::Scheduler | Bits::Consensus));
}

TEST_CASE("A node reports which of the three toolchain states its worker is in", "[node][node-status][toolchains]")
{
    // Driven over all three with a DIFFERENT count each, because the failures worth
    // catching survive a single-state check: an implementation that drops the state
    // reports absent, and one that hard-codes a state agrees with whichever case names
    // it. Neither passes all three.
    core::platform::ManualClock clock;

    auto reported = [&clock](ToolchainReading reading) {
        Fixture const fixture { {}, clock, { .worker = true }, reading };
        return fixture.status.Describe().runtime;
    };

    auto const surveying = reported({ .state = Wire::ToolchainState::Surveying, .served = 0, .discovered = 5 });
    auto const serving = reported({ .state = Wire::ToolchainState::Serving, .served = 2, .discovered = 5 });
    auto const barren = reported({ .state = Wire::ToolchainState::NothingToServe, .served = 0, .discovered = 5 });

    // Compared as OPTIONALS rather than dereferenced, which asserts engagement and
    // value in one -- and is forced anyway: `Unwrap` value-initializes on its failure
    // path and this enum has no zero enumerator, for the reason `ReplyShape::code`
    // above is an optional too.
    CHECK(surveying.toolchains == std::optional { Wire::ToolchainState::Surveying });
    CHECK(serving.toolchains == std::optional { Wire::ToolchainState::Serving });
    CHECK(barren.toolchains == std::optional { Wire::ToolchainState::NothingToServe });

    // The counts travel WITH the state. `Surveying` and `NothingToServe` both serve
    // zero, so the count alone cannot separate them and the state alone cannot say how
    // far a survey has got -- which is why neither is reported without the other.
    CHECK(surveying.toolchainsServed == 0);
    CHECK(serving.toolchainsServed == 2);
    CHECK(barren.toolchainsServed == 0);
    CHECK(surveying.toolchainsDiscovered == 5);
}

TEST_CASE("The worker component BIT and the toolchain state answer different questions", "[node][node-status][toolchains]")
{
    // **This is the whole of #1295.** `NodeComponents::worker` is a literal `true` on
    // this binary -- it compiles, that is what it is for -- so the bit is a compile-time
    // constant that cannot distinguish a node still walking its include trees from one
    // serving compiles. The fix is not to make the bit conditional; it is that the two
    // questions stopped sharing one bool.
    //
    // The discriminating shape is a CONJUNCTION, and each half alone is passed by the
    // defect: the bit must be identical across the two nodes (so it is shown to carry no
    // information) AND the state must differ (so something else does).
    core::platform::ManualClock clock;
    namespace Bits = Wire::NodeComponentBit;

    Fixture const walking { {},
                            clock,
                            { .worker = true },
                            ToolchainReading { .state = Wire::ToolchainState::Surveying, .served = 0, .discovered = 4 } };
    Fixture const ready { {},
                          clock,
                          { .worker = true },
                          ToolchainReading { .state = Wire::ToolchainState::Serving, .served = 4, .discovered = 4 } };

    auto const walkingFields = walking.status.Describe();
    auto const readyFields = ready.status.Describe();

    // Identical by the old answer...
    CHECK(walkingFields.components == readyFields.components);
    CHECK((walkingFields.components & Bits::Worker) != 0U);
    CHECK((readyFields.components & Bits::Worker) != 0U);

    // ...and different by the new one.
    REQUIRE(walkingFields.runtime.toolchains.has_value());
    REQUIRE(readyFields.runtime.toolchains.has_value());
    CHECK(walkingFields.runtime.toolchains != readyFields.runtime.toolchains);
}

TEST_CASE("A node that publishes no runtime facts reports them ABSENT, not as zeroes", "[node][node-status][toolchains]")
{
    // **Absent is not zero, and the discriminating assertion is `has_value()`.** A node
    // whose worker publishes nothing must be distinguishable from one whose worker
    // surveys nothing, and `Surveying 0 of 0` is a real reading that a client renders as
    // a dialable-looking fact.
    //
    // The fixture parks `UnwiredSentinel` in the state object precisely so this case
    // cannot pass by coincidence: an implementation that reads the source without
    // checking the null reports `Serving`, 99 of 99, which no assertion below tolerates.
    core::platform::ManualClock clock;
    Fixture const fixture { {}, clock, { .worker = true } };
    auto const fields = fixture.status.Describe();

    CHECK_FALSE(fields.runtime.toolchains.has_value());
    CHECK(fields.runtime.toolchainsServed == 0);
    CHECK(fields.runtime.toolchainsDiscovered == 0);
}

TEST_CASE("The toolchain reading is re-read per call, not captured once", "[node][node-status][toolchains]")
{
    // The same property `Describe()`'s own doc comment claims and that the uptime case
    // pins for the clock -- and it is the one that matters most here, because the state
    // this reports is by construction one that CHANGES after the status object is built:
    // the survey runs on the heartbeat thread's first round, minutes later on a cold
    // machine. A snapshot taken at construction would report `Surveying` forever, and
    // every single-call test above would still agree with it.
    core::platform::ManualClock clock;
    // NOT `const`: this case publishes into the fixture, which is what production's
    // heartbeat thread does and what the whole property is about.
    Fixture fixture { {},
                      clock,
                      { .worker = true },
                      ToolchainReading { .state = Wire::ToolchainState::Surveying, .served = 0, .discovered = 3 } };

    // Returns the OPTIONAL, so every comparison below asserts engagement and value
    // together. `Unwrap` is unusable here: it value-initializes on its failure path
    // and this enum has no zero enumerator.
    auto const state = [&fixture] {
        return fixture.status.Describe().runtime.toolchains;
    };
    auto const served = [&fixture] {
        return fixture.status.Describe().runtime.toolchainsServed;
    };

    CHECK(state() == std::optional { Wire::ToolchainState::Surveying });
    CHECK(served() == 0);

    fixture.runtime.PublishToolchains({ .state = Wire::ToolchainState::Serving, .served = 3, .discovered = 3 });

    CHECK(state() == std::optional { Wire::ToolchainState::Serving });
    CHECK(served() == 3);

    // And a machine that loses its last compiler while serving goes back, which is the
    // transition the periodic re-survey actually produces.
    fixture.runtime.PublishToolchains({ .state = Wire::ToolchainState::NothingToServe, .served = 0, .discovered = 3 });
    CHECK(state() == std::optional { Wire::ToolchainState::NothingToServe });
    CHECK(served() == 0);
}

TEST_CASE("A node reports the compile slots it offers and the ones in use", "[node][node-status][capacity]")
{
    // Read LIVE rather than published, so the discriminating assertion is that the
    // in-flight figure MOVES between two calls on one status object. A snapshot taken at
    // construction reports a constant that every single-call check agrees with.
    core::platform::ManualClock clock;
    NullLogger logger;
    CompileCapacity capacity { 4, 1U << 20U, std::chrono::seconds { 1 }, logger };
    Fixture const fixture { {}, clock, { .worker = true }, std::nullopt, DirectSources { .capacity = &capacity } };

    CHECK(fixture.status.Describe().runtime.compileSlots == std::optional<std::uint32_t> { 4 });
    CHECK(fixture.status.Describe().runtime.compilesInFlight == std::optional<std::uint32_t> { 0 });

    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    REQUIRE(capacity.TryTakeSlot() == SlotAdmission::Taken);
    CHECK(fixture.status.Describe().runtime.compilesInFlight == std::optional<std::uint32_t> { 2 });
    // The cap does not move with the load, which is the other half of reporting both.
    CHECK(fixture.status.Describe().runtime.compileSlots == std::optional<std::uint32_t> { 4 });

    capacity.ReleaseSlot();
    CHECK(fixture.status.Describe().runtime.compilesInFlight == std::optional<std::uint32_t> { 1 });
}

TEST_CASE("A node with no compile accounting reports no slots, rather than none free", "[node][node-status][capacity]")
{
    // **Absent is not zero, and here the zero is a real reading for a different node.**
    // An idle worker reports `0` in flight; a node running no worker tier must not, or
    // the two are one answer -- and `0 of 0` reads as a machine busy doing nothing
    // rather than one that was never going to compile anything.
    core::platform::ManualClock clock;
    Fixture const fixture { {}, clock, { .cacheTier = true } };
    auto const fields = fixture.status.Describe();

    CHECK_FALSE(fields.runtime.compileSlots.has_value());
    CHECK_FALSE(fields.runtime.compilesInFlight.has_value());
}

TEST_CASE("Registration has three states, and the middle one is not silence", "[node][node-status][registration]")
{
    // Nothing publishes / nothing published YET / published and registered NOWHERE. The
    // third is the one an operator acts on -- a `--scheduler` nobody answers -- and it is
    // the one a two-state model reports as the first.
    core::platform::ManualClock clock;

    SECTION("nothing publishes runtime facts at all")
    {
        Fixture const fixture { {}, clock, { .worker = true } };
        CHECK_FALSE(fixture.status.Describe().runtime.registrarsRegistered.has_value());
    }

    SECTION("a publisher that has not reported a round yet says nothing")
    {
        // The toolchain reading exists from construction; the registration one cannot,
        // because the registrars do not exist when the status object is built.
        Fixture const fixture { {}, clock, { .worker = true }, ToolchainReading {} };
        CHECK_FALSE(fixture.status.Describe().runtime.registrarsRegistered.has_value());
    }

    SECTION("a node registered nowhere reports zero of N, which is a reading")
    {
        Fixture fixture { {}, clock, { .worker = true }, ToolchainReading {} };
        fixture.runtime.PublishRegistration(0, 3, std::nullopt);
        auto const fields = fixture.status.Describe();

        CHECK(fields.runtime.registrarsRegistered == std::optional<std::uint32_t> { 0 });
        CHECK(fields.runtime.registrarsTotal == std::optional<std::uint32_t> { 3 });
        // And it has never got through, which is ABSENT rather than a large number.
        CHECK_FALSE(fields.runtime.lastRegistrationSecondsAgo.has_value());
    }
}

TEST_CASE("A round that accepted nothing does not erase when this node last registered", "[node][node-status][registration]")
{
    // **The discriminating case for `PublishRegistration`'s keep rule.** An
    // implementation that stores whatever the round handed it reports *never registered*
    // the first time a scheduler is unreachable -- which is the exact moment an operator
    // asks, and the answer that sends them to the wrong half of the fleet. `registered`
    // correctly drops to zero; the INSTANT must survive.
    core::platform::ManualClock clock;
    Fixture fixture { {}, clock, { .worker = true }, ToolchainReading {} };

    fixture.runtime.PublishRegistration(3, 3, clock.now());
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 0 });

    clock.advance(30s);
    fixture.runtime.PublishRegistration(0, 3, std::nullopt);

    auto const fields = fixture.status.Describe();
    CHECK(fields.runtime.registrarsRegistered == std::optional<std::uint32_t> { 0 });
    CHECK(fields.runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 30 });

    // And it keeps ageing without being republished -- a duration computed per call
    // rather than a number stamped once.
    clock.advance(45s);
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 75 });

    // A round that DOES accept resets it.
    fixture.runtime.PublishRegistration(3, 3, clock.now());
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 0 });
}

TEST_CASE("A node running no scheduler reports NO role, which is not `undecided`", "[node][node-status][scheduler-role]")
{
    // **A conjunction, and each half alone passes under the defect.** `Undecided` is a
    // real reading -- an election is in progress and this node is in it -- so reporting
    // it for a node that runs no scheduler at all claims participation in something that
    // is not happening. The absent case must be absent AND the undecided case must be
    // present, or the two have been collapsed.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    core::platform::ManualWallClock wallClock;

    Fixture const none { {}, clock, { .worker = true } };
    CHECK_FALSE(none.status.Describe().runtime.schedulerRole.has_value());

    auto const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, signer, {} };
    Fixture const electing { {}, clock, { .scheduler = true }, std::nullopt, DirectSources { .scheduler = &scheduler } };
    CHECK(electing.status.Describe().runtime.schedulerRole == std::optional { Wire::WireSchedulerRole::Undecided });
}

TEST_CASE("Each scheduler role crosses the wire as its own tag, with the leader it knows",
          "[node][node-status][scheduler-role]")
{
    // Driven over all three: a mapping that collapses two roles is invisible in any case
    // that names only one, and the pair a fleet most needs separated -- leader and
    // follower -- renders identically in the component mask today.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    core::platform::ManualWallClock wallClock;
    auto const signer = Testing::TestLeaseSigner();
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, signer, {} };
    Fixture const fixture { {}, clock, { .scheduler = true }, std::nullopt, DirectSources { .scheduler = &scheduler } };

    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    CHECK(fixture.status.Describe().runtime.schedulerRole == std::optional { Wire::WireSchedulerRole::Leader });

    scheduler.SetRole(Distributed::SchedulerRole::Follower, "10.0.0.9:6676", Distributed::StandaloneSchedulerTerm);
    auto const following = fixture.status.Describe();
    CHECK(following.runtime.schedulerRole == std::optional { Wire::WireSchedulerRole::Follower });
    // The whole point of reporting a follower: it names where to ask instead.
    CHECK(following.runtime.leaderEndpoint == "10.0.0.9:6676");

    scheduler.SetRole(Distributed::SchedulerRole::Undecided, {}, Distributed::StandaloneSchedulerTerm);
    auto const undecided = fixture.status.Describe();
    CHECK(undecided.runtime.schedulerRole == std::optional { Wire::WireSchedulerRole::Undecided });
    // Empty is the READING -- no leader is known -- and the role beside it is what says
    // this node was in a position to know.
    CHECK(undecided.runtime.leaderEndpoint.empty());
}

TEST_CASE("A consensus node reports the address peers DIAL, which is not the one it bound", "[node][node-status][consensus]")
{
    // #1328. The ordinary joiner binds the wildcard and names where it is reached, so the
    // one reading that distinguishes the dial address from the bind is that one: a bound
    // `127.0.0.1` fixture would print the same string under both.
    core::platform::ManualClock clock;
    Fixture const joiner { { .raft = true, .raftWildcard = true, .raftSelf = "10.0.0.4" }, clock };
    auto const fields = joiner.status.Describe();

    CHECK(fields.runtime.consensusEndpoint == std::optional { std::format("10.0.0.4:{}", RaftPort) });
    // The surface row goes on reporting the port it BOUND, and only that.
    CHECK(Unwrap(SurfaceOf(fields, Wire::WireSurface::Raft)).port == RaftPort);

    SECTION("and none on a node that runs no consensus, even one naming --raft-self")
    {
        // `RunsConsensus` is the port, never the name, so the name alone states nothing.
        Fixture const worker { { .raftSelf = "10.0.0.4" }, clock };
        CHECK_FALSE(worker.status.Describe().runtime.consensusEndpoint.has_value());
    }

    SECTION("and none, rather than an empty one, on a consensus node that names itself neither way")
    {
        // The configuration startup refuses; a status source built over it must still not
        // engage the field with nothing in it, which the wire would read back as absent
        // anyway and a direct reader would render as a blank address.
        Fixture const unstated { { .raft = true, .raftWildcard = true }, clock };
        CHECK_FALSE(unstated.status.Describe().runtime.consensusEndpoint.has_value());
    }

    SECTION("and this machine's name once it has resolved, on a consensus node naming no --raft-self")
    {
        Fixture named { { .raft = true, .raftWildcard = true }, clock };
        ApplyHostNames(named.cfg,
                       NodeHostNames { .fqdn = "laptop.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
        CHECK(named.status.Describe().runtime.consensusEndpoint
              == std::optional { std::format("laptop.corp.example:{}", RaftPort) });
    }
}

TEST_CASE("ToolchainStateFor maps a served count onto the two states it decides", "[node][node-status][toolchains]")
{
    // The one rule every publication site shares. `Surveying` is deliberately not
    // reachable through it: that state is the ABSENCE of a concluded survey rather than a
    // function of the count, and a caller holding a count has already concluded one.
    STATIC_REQUIRE(ToolchainStateFor(0) == Wire::ToolchainState::NothingToServe);
    STATIC_REQUIRE(ToolchainStateFor(1) == Wire::ToolchainState::Serving);
    STATIC_REQUIRE(ToolchainStateFor(64) == Wire::ToolchainState::Serving);
}

TEST_CASE("NodeStatus answers a member with what the node is", "[node][node-status]")
{
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    // A LIST, not `OpenMembership`, and that is the whole point of the fixture: a fake that
    // admits everyone cannot tell *the gate is wired* from *the gate admits everyone*, which
    // is the shape a loopback-only fixture already cost this tree once (#235). The route is
    // named at the site because which route admits is this case's fact (#1497).
    ListedMembership const membership { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { { .admin = true, .raft = true }, clock, { .cacheTier = true, .worker = true } };
    CapturedReadings const readings { metrics, {} };
    NodeStatusResponder responder { fixture.status, readings, membership, HoldsNoRoster, metrics };

    clock.advance(5s);
    auto const reply = AnswerNow(responder, HeaderFor(Wire::Op::NodeStatus));
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).status == Wire::Status::Ok);

    // Read back through the CLIENT's decoder rather than by inspecting the fields the
    // responder was handed: an encoder and a decoder that disagree pass any test that
    // only asks the source object what it holds.
    auto const fields = Wire::DecodeNodeStatus(PayloadOf(reply, Unwrap(header)));
    REQUIRE(fields.has_value());
    auto const& described = Unwrap(fields);
    CHECK(described.version == "1.2.3");
    CHECK(described.nodeId == "node-a");
    CHECK(described.uptimeSeconds == 5);
    CHECK(described.components == (Wire::NodeComponentBit::CacheTier | Wire::NodeComponentBit::Worker));
    CHECK(Unwrap(SurfaceOf(described, Wire::WireSurface::Admin)).port == AdminPort);
    CHECK(Unwrap(SurfaceOf(described, Wire::WireSurface::Raft)).port == RaftPort);
}

TEST_CASE("NodeMetrics answers the reading /metrics renders, the cache tier's figures and every zero included",
          "[node][node-status][node-metrics]")
{
    // #1406. WHAT DISTINGUISHES: the storage block. A reply of the counter catalogue alone -- what this
    // verb answered before -- carries the counters too, so a case asserting only a counter passes
    // under the defect; the tier's items and delete hits are the half a node with no admin surface
    // could not see.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { {}, clock };

    auto snapshot = MetricsSnapshot {};
    snapshot.storage = StorageStats { .itemCount = 3, .deleteHits = 2, .deleteMisses = 1 };
    CapturedReadings readings { metrics, snapshot };
    NodeStatusResponder responder { fixture.status, readings, membership, HoldsNoRoster, metrics };

    metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 7);

    auto const reply = AnswerNow(responder, HeaderFor(Wire::Op::NodeMetrics));
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).status == Wire::Status::Ok);

    auto const decoded = DecodeStatsReading(PayloadOf(reply, Unwrap(header)));
    REQUIRE(decoded.has_value());
    auto const& reading = decoded.value();

    // The whole reading, compared as one value: every counter, every block, the version. Derived
    // from a capture of the same sink and snapshot rather than from a list of expected figures,
    // which would go stale silently the day a figure is added.
    auto const served = NodeServedSurfacesFor(Testing::FirstStart(NodeConfig {}));
    CHECK(reading == CaptureStatsReading(metrics, snapshot, served.Span()));

    REQUIRE(reading.snapshot.storage.has_value());
    CHECK(Unwrap(reading.snapshot.storage).itemCount == 3);
    CHECK(Unwrap(reading.snapshot.storage).deleteHits == 2);
    CHECK(Unwrap(reading.snapshot.storage).deleteMisses == 1);

    // A counter is a tally, so zero is the truth about events that never happened: rows that read
    // zero are PRESENT, not dropped.
    auto const* const completed = reading.counters.Find(IMetricsSink::Counter::WorkerJobsCompleted);
    REQUIRE(completed != nullptr);
    CHECK(*completed == CounterReading::Of(7));
    auto const zeroes = std::ranges::count(reading.counters.Positional(), CounterReading::Of(0));
    // DERIVED, not `size() - 4`. Three rows read absent on a node rather than zero because it
    // serves none of the surfaces they are attributed to (#1484), and a hardcoded number here
    // would rot the moment a fourth row is attributed -- while still passing, which is the
    // direction that matters.
    auto const writable = std::ranges::count_if(
        CounterTable, [&served](auto const& row) { return CounterHasAWriterIn(row.counter, served.Span()); });
    CHECK(static_cast<std::size_t>(zeroes) == static_cast<std::size_t>(writable) - 1);

    SECTION("and a node whose sources are detached is stopping, and says so uncounted")
    {
        readings.Detach();
        auto const refused = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::NodeMetrics)));
        CHECK(refused.status == Wire::Status::Error);
        CHECK(refused.code == Wire::ErrorCode::EndpointBusy);
        CHECK(std::ranges::all_of(CounterTable, [&metrics](auto const& row) {
            return row.counter == IMetricsSink::Counter::WorkerJobsCompleted || metrics.Read(row.counter) == 0;
        }));
    }
}

TEST_CASE("The operator verbs are refused by name to a non-member, and counted once", "[node][node-status]")
{
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { "10.0.0.9" }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { { .admin = true }, clock };
    CapturedReadings const readings { metrics, {} };
    NodeStatusResponder responder { fixture.status, readings, membership, HoldsNoRoster, metrics };

    // `CallerAddress` is not one the policy admits.
    auto const shape = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::NodeStatus)));

    // WHICH refusal, not merely that one happened: `NotAMember` and `Unauthenticated`
    // send an operator to two different files.
    CHECK(shape.status == Wire::Status::Error);
    CHECK(shape.code == Wire::ErrorCode::NotAMember);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 1);

    SECTION("and nothing of the node leaks into the refusal")
    {
        // The port map is what the gate exists for, so a refusal naming a port would make
        // the gate decorative.
        CHECK_FALSE(shape.detail.contains(std::to_string(AdminPort)));
    }

    SECTION("exactly once, whichever door the request came through")
    {
        // `RefusePeer` is the ONE implementation and `Answer` calls it, so the early
        // refusal and the authoritative one cannot disagree -- and cannot double-count,
        // which a second gate written inline in `Answer` would.
        auto const early = responder.RefusePeer(PeerIdentity { .host = std::string { CallerAddress } },
                                                static_cast<std::uint8_t>(Wire::Op::NodeStatus));
        CHECK(early.has_value());
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 2);
    }

    SECTION("and a listed member is served, so the gate is not simply refusing everyone")
    {
        ListedMembership const listed { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
        NodeStatusResponder served { fixture.status, readings, listed, HoldsNoRoster, metrics };
        CHECK(ShapeOf(AnswerNow(served, HeaderFor(Wire::Op::NodeStatus))).status == Wire::Status::Ok);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 1);
    }
}

TEST_CASE("A verb this component does not own is UnimplementedVerb and is not counted", "[node][node-status]")
{
    // **`UnimplementedVerb`, never `DispatchNotPermitted`**, which is the distinction
    // #283/#340 cost this tree: a client STEPS OVER the first and proceeds, and treats
    // the second as fatal. Getting it backwards gave a credentialled client a permanent
    // 0% hit rate that read as a cold cache -- correct objects, green build, no counter
    // moving.
    //
    // Reachable only by calling `Answer` directly, since `MergedResponder` routes by
    // family. That is exactly why the arm exists: a predicate enforced only at the door
    // is one a later caller walks around.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { {}, clock };
    CapturedReadings const readings { metrics, {} };
    NodeStatusResponder responder { fixture.status, readings, membership, HoldsNoRoster, metrics };

    auto const shape = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::Fetch)));
    CHECK(shape.status == Wire::Status::Error);
    CHECK(shape.code == Wire::UnimplementedVerb);

    // Uncounted, deliberately: it is what a HEALTHY build answers, once per exchange, so
    // a counter here would bury the scan somebody would read it for. Asserted over the
    // WHOLE table rather than against the two rows this file knows about -- a future arm
    // that quietly counted something else would pass a named check.
    CHECK(std::ranges::all_of(CounterTable, [&metrics](auto const& row) { return metrics.Read(row.counter) == 0; }));
}

TEST_CASE("A frame-ceiling probe against the operator verbs is counted; an unknown opcode is not", "[node][node-status]")
{
    // Both verbs are FIELDLESS and bounded to the control cap, so a header declaring
    // megabytes against one came from no client of this tree at any version. The
    // unknown-opcode arm is the discriminating half: it is unreachable through
    // `MergedResponder` and counting it would put a port scan and a probe in one series.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { {}, clock };
    CapturedReadings const readings { metrics, {} };
    NodeStatusResponder const responder { fixture.status, readings, membership, HoldsNoRoster, metrics };

    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::NodeStatus);

    auto const tooLarge = ShapeOf(responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, opRaw, "512 MiB"));
    CHECK(tooLarge.code == Wire::ErrorCode::PayloadTooLarge);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedPayloadTooLarge) == 1);

    auto const unknown = ShapeOf(responder.RefusalReply(Wire::PrePayloadDecision::UnknownOpcode, opRaw, {}));
    CHECK(unknown.code == Wire::ErrorCode::UnknownOpcode);
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedPayloadTooLarge) == 1);

    SECTION("and a busy surface says so against the operator series, not the cache's")
    {
        // The counter names the REQUEST that was refused rather than the load that
        // exhausted the budget. Both are asserted: the cache's staying at zero is what
        // says the two are not being summed.
        auto const busy = ShapeOf(responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, opRaw, {}));
        CHECK(busy.code == Wire::ErrorCode::EndpointBusy);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy) == 1);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy) == 0);
    }
}

TEST_CASE("MergedResponder routes the Node family to the node responder and nowhere else", "[node][node-status]")
{
    // The router's `Node` arm. `-Werror=switch` did NOT catch its absence on MSVC --
    // C4062 is off by default -- so the arm silently returned nullptr and every operator
    // verb answered *served nowhere* on the one platform this was developed on.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } }, Distributed::MembershipParticipant::OpenPolicy };
    Fixture const fixture { {}, clock };
    CapturedReadings const readings { metrics, {} };
    NodeStatusResponder node { fixture.status, readings, membership, HoldsNoRoster, metrics };

    MergedResponder responder { SurfaceComponents { .node = &node } };
    CHECK(ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::NodeStatus))).status == Wire::Status::Ok);
    CHECK(ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::NodeMetrics))).status == Wire::Status::Ok);

    SECTION("and a cache verb still reaches nobody, so the arm routes rather than catching all")
    {
        auto const shape = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::Fetch)));
        CHECK(shape.status == Wire::Status::Error);
        CHECK(shape.code == Wire::UnimplementedVerb);
    }

    SECTION("and a node with no operator component answers served-nowhere rather than crashing")
    {
        MergedResponder without { SurfaceComponents {} };
        auto const shape = ShapeOf(AnswerNow(without, HeaderFor(Wire::Op::NodeStatus)));
        CHECK(shape.status == Wire::Status::Error);
        CHECK(shape.code == Wire::UnimplementedVerb);
    }
}

/// An `explain-admission` reply, decoded; REQUIREs an `Ok` reply that decodes.
/// @param reply The reply bytes.
/// @return The explanation.
[[nodiscard]] Wire::AdmissionExplanationFields ExplanationOf(std::span<std::byte const> reply)
{
    REQUIRE(ShapeOf(reply).status == Wire::Status::Ok);
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    auto const decoded = Wire::DecodeAdmissionExplanation(PayloadOf(reply, Unwrap(header)));
    REQUIRE(decoded.has_value());
    return Unwrap(decoded);
}

/// The roster the explain-admission cases hold, and the key roster their fold holds, from one
/// state: pc-07 a learner and `gone` forgotten. `new-pc` waits in the enrollment window.
/// @return The roster.
[[nodiscard]] Cluster::Roster OfficeRoster()
{
    auto roster = Cluster::Roster {};
    roster.members.push_back(Cluster::RosterMember { .id = "pc-07",
                                                     .raftEndpoint = {},
                                                     .seat = Cluster::MemberSeat::Learner,
                                                     .publicKey = Testing::TestKeyPair("pc-07").PublicKey(),
                                                     .schedulerEndpoint = {} });
    roster.revoked.push_back(Cluster::RevokedKey { .id = "gone", .publicKey = Testing::TestKeyPair("gone").PublicKey() });
    return roster;
}

TEST_CASE("explain-admission about the caller names the route that admitted it -- loopback, a ticket, or nothing",
          "[node][node-status][admission]")
{
    // The SELF form, and the reason it passes the membership door: a caller the node refuses can
    // ask WHY, and is told only what its own connection established. Over the production fold --
    // this machine and a key roster -- where pc-07's key is live.
    //
    // On a node that HOLDS a roster, with a waiting machine in its window: a roster the fixture
    // does not hold cannot leak, so a self form that reported roster standing or a third party's
    // routes would pass over `HoldsNoRoster`. Every caller below gets no standing and exactly the
    // routes its own connection established.
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    Fixture const fixture { {}, clock };
    CapturedReadings const readings { metrics, {} };
    Testing::RosterFold const fold { { "pc-07" }, { "gone" } };
    Testing::FixedStanding const standing { OfficeRoster(), { "new-pc" } };
    NodeStatusResponder responder { fixture.status, readings, fold.admitted, standing, metrics };
    auto const self = Wire::EncodeExplainAdmissionRequest("");

    auto const onThisMachine = ExplanationOf(AnswerFrom(responder, self, PeerIdentity { .host = "127.0.0.1" }));
    CHECK(onThisMachine.verdict == Wire::WireMembership::Member);
    CHECK(onThisMachine.decidedBy == Wire::WireMembershipRoute::Loopback);
    CHECK_FALSE(onThisMachine.standing.has_value());
    CHECK(onThisMachine.subject == "127.0.0.1");

    // pc-07 is a learner with a live key, and its PROVEN routes would add `proven-key`: the ticket
    // this connection presented is the only route it established.
    auto const ticketed = ExplanationOf(AnswerFrom(
        responder, self, PeerIdentity { .host = "10.0.0.7", .authenticatedMachine = Testing::IdentityOf("pc-07") }));
    CHECK(ticketed.verdict == Wire::WireMembership::Member);
    CHECK(ticketed.decidedBy == Wire::WireMembershipRoute::MachineTicket);
    CHECK_FALSE(ticketed.standing.has_value());
    CHECK(ticketed.subject == "pc-07");

    // A connection that presented a forgotten machine's revoked TICKET is answered as a stranger
    // is: its host, refused, by nobody. The ticket may be a captured one, and naming `gone` or
    // `key-revoked` would tell its holder a third party was forgotten.
    auto const captured = ExplanationOf(AnswerFrom(
        responder,
        self,
        PeerIdentity { .host = "10.0.0.7", .revokedMachine = RevokedKeyEvidence { Testing::IdentityOf("gone") } }));
    CHECK(captured.verdict == Wire::WireMembership::Outsider);
    CHECK(captured.decidedBy == 0);
    CHECK_FALSE(captured.standing.has_value());
    CHECK(captured.subject == "10.0.0.7");

    // The machine itself -- a connection that PROVED the revoked key -- is told, by name.
    auto const forgotten = ExplanationOf(
        AnswerFrom(responder, self, PeerIdentity { .host = "10.0.0.7", .proven = Testing::IdentityOf("gone") }));
    CHECK(forgotten.verdict == Wire::WireMembership::Forgotten);
    CHECK(forgotten.decidedBy == Wire::WireMembershipRoute::KeyTombstone);
    CHECK_FALSE(forgotten.standing.has_value());
    CHECK(forgotten.subject == "gone");

    // And a stranger is ANSWERED, not refused: refused by no route, which is what every gated verb
    // already tells it -- and nothing is counted, since nothing was refused.
    auto const stranger = ExplanationOf(AnswerFrom(responder, self, PeerIdentity { .host = "10.0.0.7" }));
    CHECK(stranger.verdict == Wire::WireMembership::Outsider);
    CHECK(stranger.decidedBy == 0);
    CHECK_FALSE(stranger.standing.has_value());
    CHECK(stranger.subject == "10.0.0.7");

    // The lookup path: a stranger whose subject -- its host, since its connection proved and
    // presented nothing -- is spelled like a recorded id, a pending one and a revoked one. A self
    // form that looked its subject up in the roster would report a seat or a third party's routes.
    for (auto const* host: { "pc-07", "new-pc", "gone" })
    {
        INFO(host);
        auto const named = ExplanationOf(AnswerFrom(responder, self, PeerIdentity { .host = host }));
        CHECK(named.verdict == Wire::WireMembership::Outsider);
        CHECK(named.decidedBy == 0);
        CHECK_FALSE(named.standing.has_value());
        CHECK(named.subject == host);
    }
    CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 0);

    // The door agrees: it admits the verb, and still refuses a stranger every other one.
    CHECK_FALSE(
        responder.RefusePeer(PeerIdentity { .host = "10.0.0.7" }, static_cast<std::uint8_t>(Wire::Op::ExplainAdmission))
            .has_value());
    CHECK(responder.RefusePeer(PeerIdentity { .host = "10.0.0.7" }, static_cast<std::uint8_t>(Wire::Op::NodeStatus))
              .has_value());
}

TEST_CASE("explain-admission about a machine answers from the roster through the enforced fold, and is refused to a "
          "stranger",
          "[node][node-status][admission]")
{
    core::platform::ManualClock clock;
    AtomicMetricsSink metrics;
    Fixture const fixture { {}, clock };
    CapturedReadings const readings { metrics, {} };

    // The roster this node holds and the key roster its fold holds, from one state: pc-07 a
    // learner, `gone` forgotten, and `new-pc` waiting in the enrollment window.
    Testing::FixedStanding const standing { OfficeRoster(), { "new-pc" } };
    Testing::RosterFold const fold { { "pc-07" }, { "gone" } };

    auto const ask = [&](Distributed::IMembershipOracle const& oracle, std::string_view subject) {
        NodeStatusResponder responder { fixture.status, readings, oracle, standing, metrics };
        return ExplanationOf(
            AnswerFrom(responder, Wire::EncodeExplainAdmissionRequest(subject), PeerIdentity { .host = "127.0.0.1" }));
    };

    SECTION("a live machine reports its seat and both key routes, and nothing else")
    {
        auto const answer = ask(fold.admitted, "pc-07");
        CHECK(answer.standing == std::optional { Wire::WireMachineStanding::Learner });
        CHECK(answer.verdict == Wire::WireMembership::Member);
        CHECK(answer.decidedBy == (Wire::WireMembershipRoute::ProvenIdentity | Wire::WireMembershipRoute::MachineTicket));
        CHECK(answer.subject == "pc-07");

        // By key as well as by id.
        auto const byKey = ask(fold.admitted, FormatEd25519PublicKey(Testing::TestKeyPair("pc-07").PublicKey()));
        CHECK(byKey.standing == std::optional { Wire::WireMachineStanding::Learner });
        CHECK(byKey.decidedBy == answer.decidedBy);
    }

    SECTION("a forgotten machine is revoked, refused by its tombstone")
    {
        auto const answer = ask(fold.admitted, "gone");
        CHECK(answer.standing == std::optional { Wire::WireMachineStanding::Revoked });
        CHECK(answer.verdict == Wire::WireMembership::Forgotten);
        CHECK(answer.decidedBy == Wire::WireMembershipRoute::KeyTombstone);
    }

    SECTION("a waiting and an unknown machine are admitted by nothing")
    {
        auto const waiting = ask(fold.admitted, "new-pc");
        CHECK(waiting.standing == std::optional { Wire::WireMachineStanding::Pending });
        CHECK(waiting.verdict == Wire::WireMembership::Outsider);
        CHECK(waiting.decidedBy == 0);

        auto const unknown = ask(fold.admitted, "nobody");
        CHECK(unknown.standing == std::optional { Wire::WireMachineStanding::Unknown });
        CHECK(unknown.verdict == Wire::WireMembership::Outsider);
        CHECK(unknown.decidedBy == 0);
    }

    SECTION("on an open node the open policy joins the key routes, and admits even an unknown machine")
    {
        // The route set is the fold's, not a second walk: a reader that computed the key routes on
        // its own would miss `fleet-open` here, which is the operator's actual answer.
        Distributed::OpenMembership const open;
        Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
        auto const live = ask(openly, "pc-07");
        CHECK(live.decidedBy
              == (Wire::WireMembershipRoute::ProvenIdentity | Wire::WireMembershipRoute::MachineTicket
                  | Wire::WireMembershipRoute::OpenPolicy));
        auto const unknown = ask(openly, "nobody");
        CHECK(unknown.verdict == Wire::WireMembership::Member);
        CHECK(unknown.decidedBy == Wire::WireMembershipRoute::OpenPolicy);
        // And a revoked key still outranks it.
        CHECK(ask(openly, "gone").verdict == Wire::WireMembership::Forgotten);
    }

    SECTION("on an open node an anonymous caller is told nothing about a machine, the same for every standing")
    {
        // `--fleet-open` makes every caller a member, so membership alone would tell an anonymous
        // remote caller which machines are revoked, learners or unknown -- third-party standing the
        // ticket path collapses for a captured ticket. The machine form asks for an IDENTIFIED
        // caller instead, and answers the same refusal whatever the machine's standing.
        Distributed::OpenMembership const open;
        Distributed::AnyOfMembership const openly { { &fold.loopback, &open, &fold.keys } };
        NodeStatusResponder responder { fixture.status, readings, openly, standing, metrics };
        auto const anonymous = PeerIdentity { .host = "10.0.0.7" };
        REQUIRE(Distributed::ExplainConnection(openly, anonymous).verdict == Distributed::Membership::Member);

        auto refusals = std::vector<std::vector<std::byte>> {};
        for (auto const* const subject: { "pc-07", "gone", "new-pc", "nobody" })
        {
            INFO(subject);
            auto const reply = AnswerFrom(responder, Wire::EncodeExplainAdmissionRequest(subject), anonymous);
            auto const shape = ShapeOf(reply);
            CHECK(shape.status == Wire::Status::Error);
            CHECK(shape.code == Wire::ErrorCode::IdentifiedCallerRequired);
            refusals.emplace_back(reply.begin(), reply.end());
        }
        // Byte for byte the same answer for a live, a revoked, a waiting and an unknown machine.
        CHECK(std::ranges::all_of(refusals, [&refusals](auto const& reply) { return reply == refusals.front(); }));
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 0);

        // The control: the same remote host presenting a fleet machine's TICKET is identified, and
        // is answered with the standing.
        auto const ticketed = ExplanationOf(
            AnswerFrom(responder,
                       Wire::EncodeExplainAdmissionRequest("gone"),
                       PeerIdentity { .host = "10.0.0.7", .authenticatedMachine = Testing::IdentityOf("pc-07") }));
        CHECK(ticketed.standing == std::optional { Wire::WireMachineStanding::Revoked });
    }

    SECTION("a stranger asking about a machine is refused, counted once, and told nothing about it")
    {
        NodeStatusResponder responder { fixture.status, readings, fold.admitted, standing, metrics };
        auto const reply =
            AnswerFrom(responder, Wire::EncodeExplainAdmissionRequest("pc-07"), PeerIdentity { .host = "10.0.0.7" });
        auto const shape = ShapeOf(reply);
        CHECK(shape.status == Wire::Status::Error);
        CHECK(shape.code == Wire::ErrorCode::NotAMember);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 1);
    }

    SECTION("a node that holds no roster says so rather than calling every machine unknown")
    {
        NodeStatusResponder responder { fixture.status, readings, fold.admitted, HoldsNoRoster, metrics };
        auto const shape = ShapeOf(
            AnswerFrom(responder, Wire::EncodeExplainAdmissionRequest("pc-07"), PeerIdentity { .host = "127.0.0.1" }));
        CHECK(shape.status == Wire::Status::Error);
        CHECK(shape.code == Wire::ErrorCode::NoCluster);
    }

    SECTION("a payload that is not one field is refused by name and counted, even from a stranger")
    {
        // Asked from `CallerAddress`, which this fold does NOT admit: the verb passes the door, so a
        // stranger's malformed request is refused as malformed -- counted on its own row -- rather
        // than as a stranger's. The counter's documentation says a refused caller can move it.
        NodeStatusResponder responder { fixture.status, readings, fold.admitted, standing, metrics };
        auto const shape = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::ExplainAdmission)));
        CHECK(shape.status == Wire::Status::Error);
        CHECK(shape.code == Wire::ErrorCode::MalformedFrame);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeAdmissionExplanationsRefusedMalformed) == 1);
    }
}

TEST_CASE("an admission explanation survives the wire, and an unknown verdict is refused", "[node][node-status][forget]")
{
    // The round trip, asserted on the FIELDS and not only on equality: a codec that flattened
    // the route set to its winner would round-trip a value that compares equal to itself.
    auto const original = Wire::AdmissionExplanationFields {
        .verdict = Wire::WireMembership::Member,
        .decidedBy = Wire::WireMembershipRoute::Loopback | Wire::WireMembershipRoute::MachineTicket,
    };

    auto const decoded = Wire::DecodeAdmissionExplanation(Wire::EncodeAdmissionExplanation(original));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == original);
    CHECK(Unwrap(decoded).verdict == Wire::WireMembership::Member);
    CHECK((Unwrap(decoded).decidedBy & Wire::WireMembershipRoute::MachineTicket) != 0);

    SECTION("a verdict byte this build cannot name is REFUSED, never defaulted to Outsider")
    {
        // Defaulting would report a host as refused-by-nobody on a build that had learned a
        // fourth answer, which reads exactly like the healthy case -- and this verb exists to
        // remove a confident wrong signal rather than to add one.
        auto damaged = Wire::EncodeAdmissionExplanation(original);
        auto const fields = WireFields::SplitExactly(damaged, 4);
        REQUIRE(fields.has_value());

        // The verdict is the first field's single byte; 0x7F names no `WireMembership`.
        auto const at = static_cast<std::size_t>(Unwrap(fields)[0].data() - damaged.data());
        damaged[at] = std::byte { 0x7F };

        CHECK_FALSE(Wire::DecodeAdmissionExplanation(damaged).has_value());
    }

    SECTION("but an unknown ROUTE bit is KEPT, which is the opposite decision and deliberate")
    {
        // The verdict is one value and must be known; the set is evidence and may be partial. A
        // reader that dropped a bit it could not name would under-report authorship on a fleet
        // mid-upgrade -- reporting fewer deciders than there were.
        auto const withUnknown = Wire::AdmissionExplanationFields {
            .verdict = Wire::WireMembership::Member,
            .decidedBy = Wire::WireMembershipRoute::Loopback | 0x8000U,
        };
        auto const back = Wire::DecodeAdmissionExplanation(Wire::EncodeAdmissionExplanation(withUnknown));
        REQUIRE(back.has_value());
        CHECK((Unwrap(back).decidedBy & 0x8000U) != 0);
    }
}

namespace
{
/// A consensus tier that answers whatever standing a case sets.
struct ScriptedStanding final: IConsensusStandingSource
{
    std::optional<Consensus::Standing> standing; ///< What the next question is answered with.

    [[nodiscard]] std::optional<Consensus::Standing> CurrentStanding() const override
    {
        return standing;
    }

    /// Caught up: nothing here asks about the applied state.
    [[nodiscard]] AppliedStateReading CurrentAppliedState() const override
    {
        return AppliedStateReading::CaughtUp;
    }
};
} // namespace

TEST_CASE("The consensus standing is absent with no consensus, and read per request through the slot",
          "[node][node-status][consensus][learner]")
{
    // #1449. A learner and a following voter report one role and one component mask, and
    // this is the field that tells them apart. Read through the SLOT the way production
    // binds it -- the status is built before the tier exists -- so a case that handed the
    // fake straight to the status would pass under a slot that never forwarded anything.
    core::platform::ManualClock clock;

    SECTION("a node running no consensus reports NOTHING")
    {
        Fixture fix { ConfigShape {}, clock };
        CHECK_FALSE(fix.status.Describe().runtime.consensusStanding.has_value());
    }

    SECTION("a slot answers what is attached, per request, and nothing once it is detached")
    {
        ConsensusStandingSlot slot;
        Fixture fix { ConfigShape {}, clock, {}, std::nullopt, DirectSources { .consensus = &slot } };

        // Before the tier exists: nothing to ask, so nothing is claimed.
        CHECK_FALSE(fix.status.Describe().runtime.consensusStanding.has_value());

        ScriptedStanding tier;
        tier.standing = Consensus::Standing::Learner;
        {
            auto const attached = slot.Attach(&tier);
            CHECK(fix.status.Describe().runtime.consensusStanding == std::optional { Wire::WireConsensusStanding::Learner });

            // A promotion committed a moment ago is visible on the next request.
            tier.standing = Consensus::Standing::Voter;
            CHECK(fix.status.Describe().runtime.consensusStanding == std::optional { Wire::WireConsensusStanding::Voter });
        }

        // Detached, as the tier is on the way out: absent rather than a read of freed memory.
        CHECK_FALSE(fix.status.Describe().runtime.consensusStanding.has_value());
    }

    SECTION("every standing travels as its own tag")
    {
        ScriptedStanding tier;
        Fixture fix { ConfigShape {}, clock, {}, std::nullopt, DirectSources { .consensus = &tier } };
        for (auto const& [standing, tag]:
             { std::pair { Consensus::Standing::NoCluster, Wire::WireConsensusStanding::NoCluster },
               std::pair { Consensus::Standing::Voter, Wire::WireConsensusStanding::Voter },
               std::pair { Consensus::Standing::Learner, Wire::WireConsensusStanding::Learner },
               std::pair { Consensus::Standing::Outsider, Wire::WireConsensusStanding::Outsider } })
        {
            tier.standing = standing;
            CHECK(fix.status.Describe().runtime.consensusStanding == std::optional { tag });
        }
    }
}

TEST_CASE("A node reports its condition rows as they stand when asked, and none at all when unwired",
          "[node][node-status][conditions]")
{
    // #1364. Three answers the reply must keep apart. WIRED with a row raised is the list, every
    // row in table order, carrying the words and the remedy the node holds -- text is SENT, so the
    // reader need not share this build's table. WIRED with nothing raised is the same list, all
    // `clear` or `not-evaluated`: the node saying "none" rather than going quiet. UNWIRED is no list
    // at all, which is what a build older than conditions answers and must not read as either.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    conditions.Clear(NodeCondition::EnrollmentWindowOpen);
    Fixture wired { ConfigShape {}, clock, {}, std::nullopt, DirectSources { .conditions = &conditions } };

    // Every row travels, including the ones nobody has decided yet -- and those ask for attention,
    // because "nobody evaluated this" is not "none raised".
    auto const early = Unwrap(wired.status.Describe().runtime.conditions);
    REQUIRE(early.size() == NodeConditionTable.size());
    CHECK(std::ranges::count_if(early, [](auto const& row) { return Wire::AsksForAttention(row); })
          == static_cast<std::ptrdiff_t>(NodeConditionTable.size() - 1));

    // Every row decided benign: still the whole list, and nothing in it asks.
    for (auto const& row: NodeConditionTable)
        conditions.Clear(row.condition);
    auto const settled = Unwrap(wired.status.Describe().runtime.conditions);
    REQUIRE(settled.size() == NodeConditionTable.size());
    CHECK(std::ranges::none_of(settled, [](auto const& row) { return Wire::AsksForAttention(row); }));

    // Re-read per call: a raise after construction is in the NEXT reply, which is what lets a live
    // row be watched clearing from `fastcache-cli`.
    conditions.Raise(NodeCondition::EnrollmentWindowOpen, "the window is open");
    auto const raised = Unwrap(wired.status.Describe().runtime.conditions);
    auto const row =
        std::ranges::find(raised, RowFor(NodeCondition::EnrollmentWindowOpen).id, &Wire::NodeConditionFields::id);
    REQUIRE(row != raised.end());
    CHECK(row->state == "raised");
    CHECK(row->persistence == "live");
    CHECK(row->severity == "alert");
    CHECK(row->detail == "the window is open");
    CHECK(row->remedy == RowFor(NodeCondition::EnrollmentWindowOpen).remedy);

    // The unwired arm: absent, not an empty list and not a list of `clear`.
    Fixture unwired { ConfigShape {}, clock };
    CHECK_FALSE(unwired.status.Describe().runtime.conditions.has_value());
}

TEST_CASE("A node reports the identity key it holds, and nothing on a node that holds none", "[node][node-status][identity]")
{
    // #178. Read from the configuration the start applied the key to -- through the SAME
    // reference production binds, so the case cannot pass on a copy the status never reads.
    core::platform::ManualClock clock;
    Fixture fix { ConfigShape {}, clock };

    // Absent, not a key of zeroes: a node with no state directory holds no key.
    CHECK_FALSE(fix.status.Describe().runtime.identityPublicKey.has_value());

    auto key = Ed25519PublicKey {};
    for (auto const index: std::views::iota(std::size_t { 0 }, key.size()))
        key[index] = static_cast<std::byte>(0xC0 + index);
    fix.cfg.identityPublicKey = key;

    auto const fields = fix.status.Describe();
    REQUIRE(fields.runtime.identityPublicKey.has_value());
    CHECK(Unwrap(fields.runtime.identityPublicKey) == key);

    // And the bytes survive the reply a client decodes, byte for byte.
    auto const decoded = Wire::DecodeNodeStatus(Wire::EncodeNodeStatus(fields));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).runtime.identityPublicKey == fields.runtime.identityPublicKey);
}

TEST_CASE("A node reports where it keeps its identity, and why there", "[node][node-status][formation][defaults]")
{
    // One machine can hold two identities -- the service's in the machine-wide directory and a
    // hand-started node's in the account's own -- and an operator who finds both needs the
    // reason beside each path to tell them apart. Both or neither.
    core::platform::ManualClock clock;
    Fixture unresolved { {}, clock };
    auto const none = unresolved.status.Describe().runtime;
    CHECK_FALSE(none.stateDirectory.has_value());
    CHECK_FALSE(none.stateDirectoryReason.has_value());

    Fixture named { {}, clock };
    named.cfg.clusterDir = "/srv/fastcache-node";
    auto const runtime = named.status.Describe().runtime;
    CHECK(runtime.stateDirectory == std::optional { std::filesystem::path { "/srv/fastcache-node" }.string() });
    CHECK(runtime.stateDirectoryReason
          == std::optional { std::string { DescribeStateDirectoryOrigin(StateDirectoryOrigin::Named) } });

    // And it survives the wire, both fields.
    auto const decoded = Wire::DecodeNodeRuntime(Wire::EncodeNodeRuntime(runtime));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).stateDirectory == runtime.stateDirectory);
    CHECK(Unwrap(decoded).stateDirectoryReason == runtime.stateDirectoryReason);
}

TEST_CASE("A leader with an armed window reports auto-approve and the seconds left, and a manual one reports none",
          "[node][node-status][enrollment][auto-approve]")
{
    core::platform::ManualClock clock;
    EnrollmentWindow window { clock };
    Fixture node { ConfigShape {}, clock, {}, std::nullopt, DirectSources { .enrollment = &window } };

    auto const manual = node.status.Describe().runtime;
    CHECK(manual.enrollment == std::optional { Wire::WireEnrollmentState::Manual });
    // Absent, never a zero: a zero would read as a window ending this instant.
    CHECK_FALSE(manual.enrollmentAutoApproveSecondsLeft.has_value());

    REQUIRE(window.ArmAutoApprove(std::chrono::minutes { 10 }).has_value());
    auto const armed = node.status.Describe().runtime;
    CHECK(armed.enrollment == std::optional { Wire::WireEnrollmentState::AutoApprove });
    CHECK(armed.enrollmentAutoApproveSecondsLeft == std::optional<std::uint64_t> { 600 });
}

namespace
{

/// A shared-cache status that reports one fixed record.
class FixedSharedStatus final: public ISharedCacheStatusSource
{
  public:
    [[nodiscard]] Wire::SharedCacheStatusFields Report() const override
    {
        return fields;
    }

    [[nodiscard]] Wire::SharedCacheStatusFields ReportFor(SharedCacheTarget const& /*target*/) const override
    {
        return fields;
    }

    Wire::SharedCacheStatusFields fields; ///< What `Report()` answers.
};

} // namespace

TEST_CASE("A node reports the shared cache its status source describes, and nothing when none is wired",
          "[node][status][shared-cache]")
{
    core::platform::ManualClock clock;
    FixedSharedStatus shared;
    shared.fields = Wire::SharedCacheStatusFields { .source = Wire::WireSharedCacheSource::Setting,
                                                    .machineId = "cache-c",
                                                    .endpoint = "10.0.0.3:6674",
                                                    .state = Wire::WireSharedCacheState::Unreachable,
                                                    .detail = "connection refused" };
    Fixture const wired { {}, clock, { .cacheTier = true }, std::nullopt, DirectSources { .sharedCache = &shared } };
    CHECK(wired.status.Describe().runtime.sharedCache == std::optional { shared.fields });

    // Asked per request: a report that moved shows on the next answer, and survives the wire.
    shared.fields.state = Wire::WireSharedCacheState::Proven;
    auto const fields = wired.status.Describe();
    CHECK(Unwrap(fields.runtime.sharedCache).state == Wire::WireSharedCacheState::Proven);
    auto const decoded = Wire::DecodeNodeStatus(Wire::EncodeNodeStatus(fields));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).runtime.sharedCache == fields.runtime.sharedCache);

    // Nothing wired is ABSENT -- a sender too old to say -- never a `none` it did not state.
    Fixture const bare { {}, clock, { .cacheTier = true } };
    CHECK_FALSE(bare.status.Describe().runtime.sharedCache.has_value());
}

TEST_CASE("A node reports the fleet id to paste, with its voters' keys, and the pin it trusts by",
          "[node][node-status][pin]")
{
    core::platform::ManualClock clock;
    // A consensus member's roster -- the applied state -- built as `main` builds it.
    auto member = NodeConfig {};
    member.nodeId = "n1";
    member.raftListen = "127.0.0.1:6680";
    member.raftSelf = "127.0.0.1";
    auto const roster = NodeRoster::Build(Testing::FirstStart(member), clock, nullptr);
    REQUIRE(roster.has_value());
    REQUIRE(Unwrap(roster) != nullptr);
    Fixture node { {}, clock, {}, std::nullopt, DirectSources { .roster = Unwrap(roster).get() } };

    // No formation record and no voters: nothing to paste, and the pin still answered -- unpinned is
    // the answer, never an absence.
    auto const bare = node.status.Describe();
    CHECK_FALSE(bare.runtime.fleetId.has_value());
    CHECK(bare.runtime.fleetPin == std::optional { Wire::NodeFleetPinFields {} });

    // The applied state's voters, the ones with a key, in id order -- a learner's key is not one a pin
    // should name.
    auto const office = Testing::TestKeyPair("n-office").PublicKey();
    auto const desk = Testing::TestKeyPair("n-desk").PublicKey();
    auto state = Cluster::ClusterState {};
    for (auto const& [id, seat, key]:
         { std::tuple { "n-office", Cluster::MemberSeat::Voter, office },
           std::tuple { "n-laptop", Cluster::MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey() },
           std::tuple { "n-desk", Cluster::MemberSeat::Voter, desk } })
        state.members.push_back(
            Cluster::ClusterMember { .id = id,
                                     .raftEndpoint = std::string { id } + ":6680",
                                     .schedulerEndpoint = {},
                                     .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                     .seat = seat,
                                     .publicKey = key });
    std::ranges::sort(state.members, {}, &Cluster::ClusterMember::id);
    Unwrap(roster)->Applied(state);
    REQUIRE(Unwrap(roster)->Summary().has_value());
    node.cfg.formation = NodeFormationView { .mode = Cluster::NodeMode::Voter,
                                             .clusterId = "0123456789abcdef0123456789abcdef",
                                             .createdAtUnixSeconds = 100,
                                             .foundedHere = true,
                                             .fleetMembers = {},
                                             .fleetSchedulers = {} };
    node.cfg.fleetPin = Cluster::PinnedFleet { .clusterId = "fedcba9876543210fedcba9876543210", .voterKeys = { desk } };
    auto const pinned = node.status.Describe();
    auto const paste =
        std::format("0123456789abcdef0123456789abcdef@{},{}", FormatEd25519PublicKey(desk), FormatEd25519PublicKey(office));
    CHECK(pinned.runtime.fleetId == std::optional { paste });
    // What is printed is what --fleet-id reads back.
    auto const reread = Cluster::ParsePinnedFleet(paste);
    REQUIRE(reread.has_value());
    CHECK(Unwrap(reread).voterKeys == std::vector { desk, office });
    CHECK(pinned.runtime.fleetPin
          == std::optional { Wire::NodeFleetPinFields {
              .fleet = std::format("fedcba9876543210fedcba9876543210@{}", FormatEd25519PublicKey(desk)) } });
}
