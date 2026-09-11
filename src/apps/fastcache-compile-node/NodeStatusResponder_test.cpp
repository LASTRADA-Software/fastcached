// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeStatusResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

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
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Admits exactly the peers it was given, and nobody else.
///
/// Not `OpenMembership`, and that is the whole point of the fixture: a fake that admits
/// everyone cannot tell *the gate is wired* from *the gate admits everyone*, which is
/// the shape a loopback-only fixture already cost this tree once (#235).
class ListedMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param members Who may ask.
    explicit ListedMembership(std::vector<std::string> members) noexcept:
        _members { std::move(members) }
    {
    }

    /// @copydoc Distributed::IMembershipOracle::Classify
    [[nodiscard]] Distributed::Membership Classify(std::string_view peerAddress) const override
    {
        return std::ranges::find(_members, peerAddress) != _members.end() ? Distributed::Membership::Member
                                                                          : Distributed::Membership::Outsider;
    }

  private:
    std::vector<std::string> _members;
};

/// The peer every `AnswerNow` below arrives from.
///
/// Named rather than spelled at each site, because half these cases turn on it being on
/// a member list and the other half on it not being.
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

/// Run a responder's `Answer` to completion.
///
/// Every arm here is synchronous -- these verbs read nothing and suspend nowhere -- so
/// the coroutine has already finished by the time the task is handed back.
/// @param responder What to ask.
/// @param frame The request.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> AnswerNow(IFrameResponder& responder, std::span<std::byte const> frame)
{
    return SyncRun(responder.Answer(frame, std::string { CallerAddress }));
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
};

/// Build a configuration of that shape.
/// @param shape What to resolve.
/// @return The configuration.
[[nodiscard]] NodeConfig NodeConfigOf(ConfigShape const& shape)
{
    NodeConfig cfg;
    cfg.nodeListen = "127.0.0.1:0";
    if (shape.admin)
        cfg.adminListen = std::format("127.0.0.1:{}", AdminPort);
    if (shape.raft)
        cfg.raftListen = std::format("127.0.0.1:{}", RaftPort);
    if (shape.discovery)
        cfg.discoveryAddress = std::format("0.0.0.0:{}", DiscoveryPort);
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
            IClock const& clock,
            NodeComponents components = {},
            std::optional<ToolchainReading> initial = std::nullopt,
            DirectSources direct = {}):
        cfg { NodeConfigOf(shape) },
        runtime { initial.value_or(UnwiredSentinel) },
        status { cfg,
                 clock,
                 clock.Now(),
                 "1.2.3",
                 "node-a",
                 components,
                 NodeRuntimeSources { .runtime = initial.has_value() ? &runtime : nullptr,
                                      .capacity = direct.capacity,
                                      .scheduler = direct.scheduler } }
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
    ManualClock clock;
    Fixture const adminOnly { { .admin = true }, clock };
    auto const fields = adminOnly.status.Describe();

    CHECK(SurfaceOf(fields, Wire::WireSurface::Admin).has_value());
    CHECK_FALSE(SurfaceOf(fields, Wire::WireSurface::Raft).has_value());
    CHECK_FALSE(SurfaceOf(fields, Wire::WireSurface::Discovery).has_value());
    CHECK(Unwrap(SurfaceOf(fields, Wire::WireSurface::Admin)).port == AdminPort);

    SECTION("and all three when all three are configured")
    {
        Fixture const all { { .admin = true, .raft = true, .discovery = true }, clock };
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
    ManualClock clock;

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
        Fixture const fixture { { .admin = true, .raft = true, .discovery = true, .tlsPair = true }, clock };
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
    ManualClock clock;
    Fixture const fixture { {}, clock };

    CHECK(fixture.status.Describe().uptimeSeconds == 0);
    clock.Advance(90s);
    CHECK(fixture.status.Describe().uptimeSeconds == 90);
    clock.Advance(30s);
    CHECK(fixture.status.Describe().uptimeSeconds == 120);
}

TEST_CASE("Component bits report what started, one bit each", "[node][node-status]")
{
    // Asserted per bit rather than against a total, because a total is one number four
    // wrong assignments can produce: swap `Worker` and `Scheduler` and every node running
    // both still reports the same word.
    ManualClock clock;
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
    ManualClock clock;

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
    ManualClock clock;
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
    ManualClock clock;
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
    ManualClock clock;
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
    ManualClock clock;
    NullLogger logger;
    CompileCapacity capacity { 4, 1U << 20U, std::chrono::seconds { 1 }, logger };
    Fixture const fixture { {}, clock, { .worker = true }, std::nullopt, DirectSources { .capacity = &capacity } };

    CHECK(fixture.status.Describe().runtime.compileSlots == std::optional<std::uint32_t> { 4 });
    CHECK(fixture.status.Describe().runtime.compilesInFlight == std::optional<std::uint32_t> { 0 });

    REQUIRE(capacity.TryTakeSlot());
    REQUIRE(capacity.TryTakeSlot());
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
    ManualClock clock;
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
    ManualClock clock;

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
    ManualClock clock;
    Fixture fixture { {}, clock, { .worker = true }, ToolchainReading {} };

    fixture.runtime.PublishRegistration(3, 3, clock.Now());
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 0 });

    clock.Advance(30s);
    fixture.runtime.PublishRegistration(0, 3, std::nullopt);

    auto const fields = fixture.status.Describe();
    CHECK(fields.runtime.registrarsRegistered == std::optional<std::uint32_t> { 0 });
    CHECK(fields.runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 30 });

    // And it keeps ageing without being republished -- a duration computed per call
    // rather than a number stamped once.
    clock.Advance(45s);
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 75 });

    // A round that DOES accept resets it.
    fixture.runtime.PublishRegistration(3, 3, clock.Now());
    CHECK(fixture.status.Describe().runtime.lastRegistrationSecondsAgo == std::optional<std::uint64_t> { 0 });
}

TEST_CASE("A node running no scheduler reports NO role, which is not `undecided`", "[node][node-status][scheduler-role]")
{
    // **A conjunction, and each half alone passes under the defect.** `Undecided` is a
    // real reading -- an election is in progress and this node is in it -- so reporting
    // it for a node that runs no scheduler at all claims participation in something that
    // is not happening. The absent case must be absent AND the undecided case must be
    // present, or the two have been collapsed.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    ManualWallClock wallClock;

    Fixture const none { {}, clock, { .worker = true } };
    CHECK_FALSE(none.status.Describe().runtime.schedulerRole.has_value());

    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
    Fixture const electing { {}, clock, { .scheduler = true }, std::nullopt, DirectSources { .scheduler = &scheduler } };
    CHECK(electing.status.Describe().runtime.schedulerRole == std::optional { Wire::WireSchedulerRole::Undecided });
}

TEST_CASE("Each scheduler role crosses the wire as its own tag, with the leader it knows",
          "[node][node-status][scheduler-role]")
{
    // Driven over all three: a mapping that collapses two roles is invisible in any case
    // that names only one, and the pair a fleet most needs separated -- leader and
    // follower -- renders identically in the component mask today.
    ManualClock clock;
    AtomicMetricsSink metrics;
    NullLogger logger;
    ManualWallClock wallClock;
    Distributed::SchedulerService scheduler { clock, wallClock, metrics, logger, {}, {} };
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
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { { .admin = true, .raft = true }, clock, { .cacheTier = true, .worker = true } };
    NodeStatusResponder responder { fixture.status, membership, metrics };

    clock.Advance(5s);
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

TEST_CASE("NodeMetrics reports every counter this build carries, zeroes included", "[node][node-status]")
{
    // **A counter is a tally, so zero is the truth about events that never happened.**
    // Dropping a zero row would make *nothing happened* and *this build has no such
    // counter* one answer, which is the one distinction a client reading these has no
    // other way to make -- and it is the failure a fixture that pre-moves a few counters
    // cannot see, since every row it checks is non-zero by construction.
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { {}, clock };
    NodeStatusResponder responder { fixture.status, membership, metrics };

    metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 7);

    auto const reply = AnswerNow(responder, HeaderFor(Wire::Op::NodeMetrics));
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).status == Wire::Status::Ok);

    auto const outer = WireFields::SplitAll(PayloadOf(reply, Unwrap(header)));
    REQUIRE(outer.has_value());
    // Derived from the table, never a literal: a hand-written expectation would go stale
    // the day a counter is added, and would go stale SILENTLY in the direction that reads
    // as passing.
    REQUIRE(Unwrap(outer).size() == CounterTable.size());

    std::optional<std::uint64_t> completed;
    std::size_t zeroes = 0;
    for (auto const& row: Unwrap(outer))
    {
        auto const pair = WireFields::SplitExactly(row, 2);
        REQUIRE(pair.has_value());
        auto const name = Wire::AsStringView(Unwrap(pair)[0]);
        auto const value = Wire::DecodeU64Field(Unwrap(pair)[1]);
        REQUIRE(value.has_value());
        if (name == "fastcache_worker_jobs_completed_total")
            completed = value;
        if (Unwrap(value) == 0)
            ++zeroes;
    }

    CHECK(completed == std::optional<std::uint64_t> { 7 });
    // The property the row count alone cannot assert: rows that read zero are PRESENT.
    CHECK(zeroes == CounterTable.size() - 1);
}

TEST_CASE("The operator verbs are refused by name to a non-member, and counted once", "[node][node-status]")
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { "10.0.0.9" } };
    Fixture const fixture { { .admin = true }, clock };
    NodeStatusResponder responder { fixture.status, membership, metrics };

    // `CallerAddress` is not on that list.
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
        auto const early = responder.RefusePeer(CallerAddress, static_cast<std::uint8_t>(Wire::Op::NodeStatus));
        CHECK(early.has_value());
        CHECK(metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember) == 2);
    }

    SECTION("and a listed member is served, so the gate is not simply refusing everyone")
    {
        ListedMembership const listed { { std::string { CallerAddress } } };
        NodeStatusResponder served { fixture.status, listed, metrics };
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
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { {}, clock };
    NodeStatusResponder responder { fixture.status, membership, metrics };

    auto const shape = ShapeOf(AnswerNow(responder, HeaderFor(Wire::Op::Fetch)));
    CHECK(shape.status == Wire::Status::Error);
    CHECK(shape.code == Wire::UnimplementedVerb);

    // Uncounted, deliberately: it is what a HEALTHY build answers, once per exchange, so
    // a counter here would bury the scan somebody would read it for. Asserted over the
    // WHOLE table rather than against the two rows this file knows about -- a future arm
    // that quietly counted something else would pass a named check.
    CHECK(std::ranges::all_of(CounterTable, [&metrics](auto const& row) { return metrics.Read(row.counter) == 0; }));
}

TEST_CASE("The operator surface requires no credential, so a plain worker can answer", "[node][node-status]")
{
    // **The property that keeps these verbs usable at all.** The credential on this
    // listener is the SCHEDULER's -- `MergedResponder` routes `CheckCredential` there --
    // so a node running no scheduler has none to check. `CredentialOutcome::NoPolicy`
    // answers `Ok` and marks NOTHING, so a surface answering `AuthRequired` true here
    // would leave every operator verb permanently `Unauthenticated` on exactly the
    // deployment they exist for: one machine, one node, no fleet.
    //
    // Asserted through `DecidePrePayload`, which is what actually decides it, rather than
    // by reading the getter back -- the getter agrees with itself under any
    // implementation.
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { {}, clock };
    NodeStatusResponder const responder { fixture.status, membership, metrics };

    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::NodeStatus);
    CHECK_FALSE(responder.AuthRequired(opRaw));
    CHECK(Wire::DecidePrePayload({ .opRaw = opRaw,
                                   .declaredLength = 0,
                                   .sessionCap = Wire::MaxControlPayload,
                                   .authRequired = responder.AuthRequired(opRaw),
                                   .credentialAccepted = false })
          == Wire::PrePayloadDecision::Serve);

    // And the control, which is what says the assertion above measures anything: the verb
    // is NOT on the pre-auth allowlist, so a surface that DOES hold a policy still makes
    // it wait for one. Without this, *these verbs never need a credential* and *this
    // surface holds none* are one passing test.
    CHECK_FALSE(Wire::IsPreAuthAllowed(opRaw));
    CHECK(Wire::DecidePrePayload({ .opRaw = opRaw,
                                   .declaredLength = 0,
                                   .sessionCap = Wire::MaxControlPayload,
                                   .authRequired = true,
                                   .credentialAccepted = false })
          == Wire::PrePayloadDecision::Unauthenticated);
}

TEST_CASE("A frame-ceiling probe against the operator verbs is counted; an unknown opcode is not", "[node][node-status]")
{
    // Both verbs are FIELDLESS and bounded to the control cap, so a header declaring
    // megabytes against one came from no client of this tree at any version. The
    // unknown-opcode arm is the discriminating half: it is unreachable through
    // `MergedResponder` and counting it would put a port scan and a probe in one series.
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { {}, clock };
    NodeStatusResponder const responder { fixture.status, membership, metrics };

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
    ManualClock clock;
    AtomicMetricsSink metrics;
    ListedMembership const membership { { std::string { CallerAddress } } };
    Fixture const fixture { {}, clock };
    NodeStatusResponder node { fixture.status, membership, metrics };

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
