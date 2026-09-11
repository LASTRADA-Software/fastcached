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
    Fixture(ConfigShape const& shape, IClock const& clock, NodeComponents components = {}):
        cfg { NodeConfigOf(shape) },
        status { cfg, clock, clock.Now(), "1.2.3", "node-a", components }
    {
    }

    Fixture(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;

    // Declaration order IS construction order, and `status` below binds `cfg` above it.
    NodeConfig cfg;              ///< What the operator asked for.
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
