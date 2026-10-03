// SPDX-License-Identifier: Apache-2.0
//
// The live-stats reading: captured once, rendered as `/metrics` text or encoded as binary, and
// decoded by a client into the same struct (#1399). What these cases hold is that the two
// encodings cannot disagree and that a client of another layout refuses rather than misreads.
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <tests/SkewedMetricsSink.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// A sink that cannot carry one catalogue row, the in-process shape of #1353's build skew.
using SkewedSink = FastCache::Testing::SkewedMetricsSink;

/// Every counter a distinct, wide value, so a figure decoded into its neighbour's slot differs.
void GiveEveryCounterItsOwnValue(IMetricsSink& sink)
{
    auto value = std::uint64_t { 1 };
    for (auto const& row: CounterTable)
        sink.Increment(row.counter, (value++ * 1'000'003) + 17);
}

/// Every block present, every field distinct, one tier absent: nothing below passes by
/// round-tripping a default.
[[nodiscard]] MetricsSnapshot EveryBlockPresent()
{
    auto stats = [](std::uint64_t base) {
        StorageStats s;
        auto next = base;
        for (auto const& field: StatsReadingWire::StorageSizeFields)
            s.*field.member = static_cast<std::size_t>(next++);
        for (auto const& field: StatsReadingWire::StorageCounterFields)
            s.*field.member = next++;
        return s;
    };
    TieredStorageStats tiers {};
    tiers[0] = stats(1000);
    return MetricsSnapshot { .storage = stats(100),
                             .storageTiers = tiers,
                             .host = HostCapacity { .logicalCores = 32,
                                                    .configuredSlots = 30,
                                                    .totalMemoryBytes = 68'719'476'736,
                                                    .diskCapacityBytes = 2'000'398'934'016,
                                                    .diskFreeBytes = 442'381'631'488,
                                                    .busySlots = 7,
                                                    .cordoned = 1 },
                             .hostLoad = HostLoadReading { .cpu = CpuTicks { .busy = 7'700'001, .total = 9'100'003 },
                                                           .availableMemoryBytes = 21'474'836'480 },
                             .upstreamConfigured = false,
                             .consensus = ConsensusStatus { .configuration = { .voters = { "node-a1", "node-b2", "" },
                                                                               .learners = { "laptop-c3" } },
                                                            .knownLeader = Consensus::NodeId { "node-b2" },
                                                            .term = Consensus::Term { .value = 41 },
                                                            .commitIndex = Consensus::LogIndex { .value = 918'273 },
                                                            .role = Consensus::Role::PreCandidate },
                             .uptime = Uptime { 864'017s } };
}

[[nodiscard]] StatsReading RichReading()
{
    SkewedSink sink { IMetricsSink::Counter::ConnectionsAdmissionRejected };
    GiveEveryCounterItsOwnValue(sink);
    return CaptureStatsReading(sink, EveryBlockPresent(), EverySurface);
}

} // namespace

TEST_CASE("Capturing a reading asks the sink whether it carries each row", "[metrics][livestats]")
{
    // #1353 in the model: a row this build's sink has no slot for is ABSENT in the reading, and
    // every other row holds exactly what the sink reads.
    auto const missing = IMetricsSink::Counter::ConnectionsAdmissionRejected;
    SkewedSink sink { missing };
    GiveEveryCounterItsOwnValue(sink);

    auto const reading = CaptureStatsReading(sink, EveryBlockPresent(), EverySurface);

    for (auto const& row: CounterTable)
    {
        INFO(row.prometheusName);
        auto const* const captured = reading.counters.Find(row.counter);
        REQUIRE(captured != nullptr);
        if (row.counter == missing)
        {
            CHECK_FALSE(captured->Present());
            // WHICH absence, not merely that there is one: this case stages a sink with no slot
            // for the row, so the reason must be the skew one. Asserting only the absence would
            // pass just as well if capture had decided the process writes no such counter.
            CHECK(captured->Why() == CounterAbsence::NoSlotInThisBuild);
        }
        else
            CHECK(*captured == CounterReading::Of(sink.Read(row.counter)));
    }
    CHECK(reading.snapshot == EveryBlockPresent());
}

TEST_CASE("A reading survives the binary form unchanged", "[metrics][livestats]")
{
    auto const reading = RichReading();
    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == reading);
}

TEST_CASE("An absent block survives the binary form as absent rather than as zero", "[metrics][livestats]")
{
    // The empty reading is the other half of the round trip: every optional block absent and
    // every counter absent. A codec that wrote zeroes for absences would decode a cache with
    // no tiers into a cache whose tiers hold nothing, and `/metrics` would then say so.
    StatsReading const nothing {};
    auto const decoded = DecodeStatsReading(EncodeStatsReading(nothing));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == nothing);
    CHECK(std::ranges::none_of(decoded->counters.Positional(), [](auto const& cell) { return cell.Present(); }));
    CHECK_FALSE(decoded->snapshot.storage.has_value());
    CHECK_FALSE(decoded->snapshot.consensus.has_value());
    CHECK_FALSE(decoded->snapshot.hostLoad.has_value());
}

TEST_CASE("Each host-load figure travels absent on its own", "[metrics][livestats]")
{
    // A platform that will not report its CPU still reports its memory, and the other way
    // round. A codec that tied the two together would decode a machine whose CPU could not be
    // read into one whose CPU reads zero -- an idle machine -- or drop a memory figure it had.
    auto const onlyMemory = StatsReading {
        .counters = {},
        .snapshot = MetricsSnapshot { .storage = std::nullopt,
                                      .host = std::nullopt,
                                      .hostLoad = HostLoadReading { .cpu = std::nullopt, .availableMemoryBytes = 4096 } }
    };
    auto const memoryDecoded = DecodeStatsReading(EncodeStatsReading(onlyMemory));
    REQUIRE(memoryDecoded.has_value());
    CHECK(*memoryDecoded == onlyMemory);
    REQUIRE(memoryDecoded->snapshot.hostLoad.has_value());
    CHECK_FALSE(Unwrap(memoryDecoded->snapshot.hostLoad).cpu.has_value());

    auto const onlyCpu =
        StatsReading { .counters = {},
                       .snapshot =
                           MetricsSnapshot { .storage = std::nullopt,
                                             .host = std::nullopt,
                                             .hostLoad = HostLoadReading { .cpu = CpuTicks { .busy = 3, .total = 5 },
                                                                           .availableMemoryBytes = std::nullopt } } };
    auto const cpuDecoded = DecodeStatsReading(EncodeStatsReading(onlyCpu));
    REQUIRE(cpuDecoded.has_value());
    CHECK(*cpuDecoded == onlyCpu);
    REQUIRE(cpuDecoded->snapshot.hostLoad.has_value());
    CHECK_FALSE(Unwrap(cpuDecoded->snapshot.hostLoad).availableMemoryBytes.has_value());

    // And a block with neither is still a block: the process samples load and the platform
    // said nothing, which is not the daemon's absence of the whole question.
    auto const neither = StatsReading {
        .counters = {},
        .snapshot = MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .hostLoad = HostLoadReading {} }
    };
    auto const neitherDecoded = DecodeStatsReading(EncodeStatsReading(neither));
    REQUIRE(neitherDecoded.has_value());
    CHECK(neitherDecoded->snapshot.hostLoad == std::optional { HostLoadReading {} });
}

TEST_CASE("A host-load presence bit this build does not know is refused as malformed", "[metrics][livestats]")
{
    // Only the host-load block present, so its presence byte sits at a position the grammar
    // fixes: the digest, the empty counter bitmap, the snapshot presence byte and the empty
    // tier bitmap come first.
    auto const reading = StatsReading {
        .counters = {},
        .snapshot = MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .hostLoad = HostLoadReading {} }
    };
    auto bytes = EncodeStatsReading(reading);
    auto const bitmapBytes = [](std::size_t count) {
        return (count + 7) / 8;
    };
    // The counter block is TWO bitmaps since #1484 -- presence, then which absence each absent
    // cell is -- and no values here, because this reading carries no present counter. Spelled once
    // so the two offsets below cannot drift apart.
    auto const counterBlock = 2 * bitmapBytes(reading.counters.Positional().size());
    auto const snapshotPresenceAt = sizeof(std::uint64_t) + counterBlock;
    auto const at = snapshotPresenceAt + 1 + bitmapBytes(reading.snapshot.storageTiers.size());
    // Positional controls: the snapshot presence byte names the host-load block alone, and the
    // byte at `at` is that block's own presence, with neither figure set.
    REQUIRE(bytes.size() > at);
    REQUIRE(bytes[snapshotPresenceAt] == std::byte { 0x10 });
    REQUIRE(bytes[at] == std::byte { 0x00 });

    // The control: the untouched bytes decode, with the block present and both figures absent.
    REQUIRE(DecodeStatsReading(bytes).has_value());

    bytes[at] = std::byte { 0x04 };
    auto const refused = DecodeStatsReading(bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == StatsReadingFault::Malformed);
}

TEST_CASE("A decoded reading renders the metrics body the node itself serves", "[metrics][livestats]")
{
    // The property that makes one model worth having: a client holding the decoded struct and
    // the node's own scrape describe the same state, figure for figure, including the skewed
    // row the scrape names and does not render.
    auto const reading = RichReading();
    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());

    auto const served = RenderPrometheus(reading);
    CHECK(RenderPrometheus(*decoded) == served);
    auto const* const skewed = DescriptorOf(IMetricsSink::Counter::ConnectionsAdmissionRejected);
    REQUIRE(skewed != nullptr);
    CHECK(served.contains(std::format("# SKEW {} is", skewed->prometheusName)));
}

TEST_CASE("A decoded reading states the build that captured it, not the build decoding it", "[metrics][livestats]")
{
    // WHAT DISTINGUISHES: a version that is not this build's. A codec that dropped the field, or a renderer
    // that stated the decoding build's own constant, both pass a round trip of a reading this build captured.
    auto reading = RichReading();
    CHECK(reading.version == VersionString);
    reading.version = "9.9.9-another \"build\"";
    auto const decoded = DecodeStatsReading(EncodeStatsReading(reading));
    REQUIRE(decoded.has_value());
    CHECK(decoded->version == reading.version);
    CHECK(RenderPrometheus(*decoded).contains("fastcached_build_info{version=\"9.9.9-another \\\"build\\\"\"} 1\n"));
    CHECK_FALSE(RenderPrometheus(*decoded).contains(std::format("version=\"{}\"", VersionString)));
}

TEST_CASE("A reading laid out by another build is refused by name", "[metrics][livestats]")
{
    auto bytes = EncodeStatsReading(RichReading());
    REQUIRE(DeclaredStatsReadingLayout(bytes) == std::optional { StatsReadingLayout });

    // The control first: the untouched bytes decode.
    REQUIRE(DecodeStatsReading(bytes).has_value());

    bytes[7] ^= std::byte { 0x01 };
    auto const refused = DecodeStatsReading(bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == StatsReadingFault::ForeignLayout);
}

TEST_CASE("The layout digest moves when a catalogue row moves", "[metrics][livestats]")
{
    // Position is the whole identity of a counter on the wire, so an insertion, a removal or a
    // swap between two builds must change the digest they compare. Asked of the digest function
    // over a copy of the catalogue, so the case proves the MECHANISM without editing the tree.
    auto shifted = CounterTable;
    std::swap(shifted[3], shifted[4]);
    CHECK(StatsReadingWire::LayoutDigest(shifted) != StatsReadingLayout);

    auto renamed = CounterTable;
    renamed[0].prometheusName = "fastcached_a_series_this_build_does_not_have";
    CHECK(StatsReadingWire::LayoutDigest(renamed) != StatsReadingLayout);

    // And the control: the catalogue itself digests to the published layout.
    CHECK(StatsReadingWire::LayoutDigest(CounterTable) == StatsReadingLayout);
}

TEST_CASE("An IMetricsSink counter travels at its ordinal and a reordered enum is refused as a foreign layout",
          "[metrics][livestats]")
{
    // instruments (#1366), via team-lead: `IMetricsSink::Counter` said its ordinals were private while this codec
    // sends counters by position. WHAT DISTINGUISHES, in two halves. POSITION: a reading carrying ONE counter sets
    // exactly that ordinal's bit and writes its value first -- a codec keyed by name, or by catalogue order apart
    // from the enum, would set another bit. DETECTION: the catalogue follows the enum row for row, so a reordered
    // enum is two swapped rows; that digest differs, and a reading stamped with it is refused `ForeignLayout`
    // rather than decoded shifted.
    constexpr auto Value = std::uint64_t { 0x1122'3344'5566'7788ULL };
    constexpr auto LayoutBytes = std::size_t { 8 };
    auto const rows = CounterTable.size();
    auto const bitmapBytes = (rows + 7) / 8;
    REQUIRE(rows == CounterCount);

    auto const onlyAt = [&](IMetricsSink::Counter counter) {
        auto reading = StatsReading {};
        auto* const cell = reading.counters.Find(counter);
        REQUIRE(cell != nullptr);
        *cell = CounterReading::Of(Value);
        return EncodeStatsReading(reading);
    };
    auto const bigEndianAt = [](std::vector<std::byte> const& bytes, std::size_t at) {
        auto value = std::uint64_t { 0 };
        for (auto const byte: std::span { bytes }.subspan(at, 8))
            value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
        return value;
    };

    for (auto const counter: { IMetricsSink::Counter::ConnectionsTotal,
                               IMetricsSink::Counter::ConnectionsTotalTls,
                               IMetricsSink::Counter::LiveSubscriptionsRefusedEndpointBusy })
    {
        // The enumerator's ordinal IS the wire position here, which is what this case pins -- asked of the one
        // converter rather than cast (#1366).
        auto const index = CounterIndex(counter);
        REQUIRE(index.has_value());
        auto const ordinal = Unwrap(index);
        CAPTURE(ordinal);
        auto const bytes = onlyAt(counter);
        REQUIRE(bytes.size() >= LayoutBytes + bitmapBytes + 8);
        auto const bitmap = std::span { bytes }.subspan(LayoutBytes, bitmapBytes);
        auto setBits = 0;
        for (auto const byte: bitmap)
            setBits += std::popcount(std::to_integer<unsigned>(byte));
        CHECK(setBits == 1);
        CHECK((bitmap[ordinal / 8] & static_cast<std::byte>(1U << (ordinal % 8))) != std::byte { 0 });
        CHECK(bigEndianAt(bytes, LayoutBytes + bitmapBytes) == Value);
    }

    // A reorder of two neighbouring enumerators is those two catalogue rows swapped.
    auto reordered = CounterTable;
    std::swap(reordered[1], reordered[2]);
    auto const foreign = StatsReadingWire::LayoutDigest(reordered);
    REQUIRE(foreign != StatsReadingLayout);

    auto bytes = onlyAt(IMetricsSink::Counter::ConnectionsTotalTls);
    REQUIRE(DecodeStatsReading(bytes).has_value()); // the control: this build's own stamp decodes
    for (auto const i: std::views::iota(std::size_t { 0 }, LayoutBytes))
        bytes[i] = static_cast<std::byte>((foreign >> (8U * (LayoutBytes - 1 - i))) & 0xFFU);
    REQUIRE(DeclaredStatsReadingLayout(bytes) == std::optional { foreign });
    auto const refused = DecodeStatsReading(bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == StatsReadingFault::ForeignLayout);
}

TEST_CASE("This build's live-stats layout is the pinned one", "[metrics][livestats]")
{
    // The digest follows the tables by itself, so the wire stays safe without this pin. What the
    // pin adds is that a layout change is SEEN: adding, moving or renaming a counter, a storage
    // or host field, a tier or a role changes what every live-stats client can read, and a
    // client built before the change will refuse this node. Update the constant in the same
    // change, and say in its message that clients and nodes upgrade together.
    INFO(std::format("StatsReadingLayout is 0x{:016x}", StatsReadingLayout));
    // Moved by a lease exclusion list: `dispatch_leases_all_excluded` joined the catalogue after
    // `withdrawn`, for a lease whose every matching worker the client had excluded. Clients and
    // nodes upgrade together, as below.
    //
    // Moved by the fleet's shared cache: fifteen counters joined the catalogue -- the
    // shared tier's hits, misses, store failures and seven refusals, and the client's wrong-key,
    // failed-proof, unresolved, stale-hint and sessions-opened tallies. Clients and nodes upgrade
    // together, as below.
    //
    // Moved when a machine became forgotten by its key alone before that:
    // `node_requests_refused_host_forgotten` left the catalogue with the host tombstones it
    // counted, which changes which cells every live-stats reading carries. Clients and nodes
    // upgrade together, as below.
    //
    // Moved by the machine ticket before that: eleven counters joined the catalogue -- a ticket a node
    // accepted (`node_tickets_accepted`) and one row per way a node refuses one (`malformed`,
    // `not_utf8`, `no_roster`, `unknown_machine`, `forged`, `revoked`, `wrong_audience`,
    // `expired`, `replayed`, `spent_set_full`) -- which changes which cells every live-stats
    // reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved when a pending node began refusing an admission its proven fleet did not vouch for: one
    // counter joined the catalogue (`formation_admissions_refused`). Clients and nodes upgrade
    // together, as below.
    //
    // Moved when the enroll door began holding a joiner's id to the one id bound: one counter joined
    // the catalogue, for a request refused because its id is longer than `MaxIdBytes`
    // (`enrollment_requests_refused_id_too_long`). Clients and nodes upgrade together, as below.
    //
    // Moved when zero-config formation gained its controller: two counters joined the catalogue --
    // the joins a solitary node decided to ask for (`formation_yields`) and the ones it gave up on
    // when the fleet stopped answering (`formation_joins_abandoned`) -- which changes which cells
    // every live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved when an approval began naming the key it admits: one counter joined the catalogue, for
    // an approval refused because its key is not the row's (`enrollment_approvals_refused_key_mismatch`),
    // which changes which cells every live-stats reading carries. Clients and nodes upgrade
    // together, as below.
    //
    // Moved when the enrollment list became bounded per source host: two counters joined the
    // catalogue -- a request refused because its host already held its share
    // (`enrollment_requests_refused_host_cap`), and the rows `--enroll-clear` dropped
    // (`enrollment_requests_cleared`) -- which changes which cells every live-stats reading
    // carries. Clients and nodes upgrade together, as below.
    //
    // Moved when a leader could arm an auto-approve deadline: one counter joined the catalogue,
    // for the joiners it admitted with nobody comparing a key (`enrollment_approvals_auto`), which
    // changes which cells every live-stats reading carries. Clients and nodes upgrade together,
    // as below.
    //
    // Moved when a leader began recording every joiner without an open window: two counters
    // joined the catalogue -- a request forgotten after its machine stopped asking before anybody
    // decided about it (`enrollment_requests_expired`), and a joiner an operator admitted by name
    // (`enrollment_approvals_manual`) -- which changes which cells every live-stats reading
    // carries. Clients and nodes upgrade together, as below.
    //
    // Moved when discovery began budgeting its signature checks: one counter joined the catalogue
    // for a proof answering a live challenge whose signature was not checked, past the check budget
    // (`discovery_proof_checks_withheld`). That changes which cells every live-stats reading
    // carries. Clients and nodes upgrade together, as below.
    //
    // Moved when discovery stopped replying with more than it was sent and began rate-limiting its
    // answers: one counter joined the catalogue for a challenge or a proof withheld, larger than
    // the datagram that provoked it or past the answer budget (`discovery_replies_withheld`). That
    // changes which cells every live-stats reading carries. Clients and nodes upgrade together, as
    // below.
    //
    // Moved when discovery began challenging other fleets: one counter joined the catalogue for a
    // beacon dropped at a bound, a new node past the outstanding challenges or a new fleet past
    // the ones remembered (`discovery_beacons_over_bound`). That changes which cells every
    // live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved when a member's seat began saying how it is reached: the no-session drop split in
    // two, and one counter joined the catalogue for a message to a peer the transport can place
    // nowhere, neither dialled nor dialling in (`raft_sends_dropped_unknown_peer`), apart from a
    // learner that dials in and has no session attached (`raft_sends_dropped_no_session`). That
    // changes which cells every live-stats reading carries. Clients and nodes upgrade together,
    // as below.
    //
    // Moved when a learner's transport began reading what the leader writes on the session it
    // dialled: eight counters joined the catalogue for a frame the reading end cannot use. Five are
    // the dialler's, for the direction only it reads -- a tag that does not verify
    // (`raft_peer_dials_ended_frame_tag`), a message naming another member (`_frame_sender`), a
    // frame this build cannot read (`_frame_unreadable`), one over the cap (`_frame_over_cap`) and
    // one that is not this wire (`_frame_bad_magic`) -- and three are the acceptor's endings that
    // had been logged and never counted (`raft_peer_frames_refused_unreadable`, `_over_cap`,
    // `_bad_magic`). That changes which cells every live-stats reading carries. Clients and nodes
    // upgrade together, as below.
    //
    // Moved when a leader began writing to a learner on the session the learner dialled: two
    // counters joined the catalogue -- a message dropped for a peer this node neither dials nor
    // holds a session from (`raft_sends_dropped_no_session`), and a session a newer one from the
    // same id superseded (`raft_inbound_sessions_superseded`) -- which changes which cells every
    // live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved by zero-config formation: two counters left the catalogue with the enrollment window
    // an operator opened -- `enrollment_requests_refused_closed`, since every request is now
    // recorded, and `enrollment_windows_opened`, since there is nothing left to open -- which
    // changes which cells every live-stats reading carries. Clients and nodes upgrade together,
    // as below.
    //
    // Moved by #178's node identity handshake before that: five counters joined the catalogue -- a proof whose
    // key the cluster does not hold or has revoked (`node_proofs_refused_unknown_key`,
    // `_revoked_key`), a request refused on a connection a revoked key marked
    // (`node_requests_refused_key_revoked`), a sealed frame that failed its tag
    // (`node_sealed_frames_refused`) and a joining verb sent unproven
    // (`scheduler_requests_refused_node_identity_required`) -- which changes which cells every
    // live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved by #1555 before that: `enrollment_requests_refused_revoked_key` joined the catalogue, for the
    // enrollment a forgotten machine asks for under the key its forget revoked. Clients and
    // nodes upgrade together, as below.
    //
    // Moved by #178's certified roster before that: six counters joined the catalogue -- the
    // three lease refusals a roster makes possible (`no_roster`, `roster_expired`,
    // `signer_revoked`), the two ways a worker refuses a roster it is offered (`uncertified`,
    // `expired`) and the scheduler's refused endorsements -- and the snapshot gained the roster's
    // remaining lifetime, so `StatsReadingWire::Grammar` went to `-6`. Clients and nodes upgrade
    // together, as below.
    //
    // Moved by #178's enrollment and discovery on keys before that: three counters joined the
    // catalogue for the discovery proofs a key makes possible to refuse (`unknown_key`,
    // `revoked_key`, `forged`), `enrollment_keys_handed_over` became `enrollment_rosters_served`,
    // and `enrollment_requests_refused_already_collected` left with the spend it counted -- which
    // changes which cells every live-stats reading carries. Clients and nodes upgrade together,
    // as below.
    //
    // Moved by #178's Raft peer wire before that: seven counters joined the catalogue for the refusals an
    // identity key makes possible (`unknown_key`, `revoked_key` and `ended_key_withdrawn` at the
    // acceptor; `acceptor_key_unknown`, `acceptor_key_revoked`, `own_key_revoked` and
    // `ended_key_withdrawn` at the dialler), which changes which cells every live-stats reading
    // carries. Clients and nodes upgrade together, as below.
    //
    // Moved by #178 before that: one counter joined the catalogue for a cluster admission whose identity key
    // is not one (`cluster_admissions_refused_malformed_key`), which changes which cells every
    // live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved by #1449: the consensus block's one member list became two -- voters, then
    // learners -- so `StatsReadingWire::Grammar` went to `-5` and the field is named
    // `configuration`. Clients and nodes upgrade together: a `fastcache-cli` built before this
    // refuses a node built after it by name (`ForeignLayout`), rather than reading the
    // learners as the leader's id.
    //
    // Moved by #1428: four counters joined the catalogue for the node proof, then under the cluster key
    // (`node_proofs_accepted`, `_rejected`, `_unchallenged`, `_malformed`), which changes which
    // cells every live-stats reading carries. Clients and nodes upgrade together -- a
    // `fastcache-cli` built before this refuses a node built after it, by name
    // (`ForeignLayout`) rather than by decoding plausible numbers into the wrong fields.
    //
    // Moved by #1471: one counter joined the catalogue for the admission-explanation verb
    // (`node_admission_explanations_refused_malformed`), which changes which cells every
    // live-stats reading carries. Clients and nodes upgrade together, as below.
    //
    // Moved by #1484 before that: the counter cells carry a second bitmap saying WHICH absence
    // each absent cell is, so `StatsReadingWire::Grammar` went to `-4`.
    //
    // Moved when the node's password gate went: seven counters no writer could move any more left
    // the catalogue (the scheduler's three credential rows, the compile surface's three, and the
    // enrollment surface's unauthenticated row). Clients and nodes upgrade together, as above.
    //
    // Moved when a node began minting machine tickets: five counters joined the catalogue
    // (`node_tickets_minted` and the four `node_ticket_mints_refused_*` rows). Clients and nodes
    // upgrade together, as above.
    //
    // Moved when lane 2b's admission counters were appended after lane 2a's formation rows, in
    // integration order (ruling 8). Read off the built test, never computed by hand.
    //
    // Moved again when an operator's control verbs began refusing a caller only `--fleet-open`
    // admitted: two counters joined the catalogue, one per surface that refuses one. Clients and
    // nodes upgrade together, as above.
    //
    // Moved when lane 2c's fifteen shared-cache counters joined the catalogue, after lane 2b's in
    // integration order (ruling 8). Read off the built test, never computed by hand.
    //
    // Moved when lane 3's `dispatch_leases_all_excluded` joined the catalogue after `withdrawn`, in
    // integration order (ruling 8). Read off the built test, never computed by hand.
    CHECK(StatsReadingLayout == 0xed690b6da9395300ULL);
}

TEST_CASE("A truncated or padded reading is refused and never half-read", "[metrics][livestats]")
{
    auto const bytes = EncodeStatsReading(RichReading());

    for (auto const length: std::views::iota(std::size_t { 0 }, bytes.size()))
    {
        auto const refused = DecodeStatsReading(std::span { bytes }.first(length));
        INFO("prefix of " << length << " of " << bytes.size() << " bytes");
        CHECK_FALSE(refused.has_value());
    }

    auto padded = bytes;
    padded.push_back(std::byte { 0 });
    auto const trailing = DecodeStatsReading(padded);
    REQUIRE_FALSE(trailing.has_value());
    CHECK(trailing.error() == StatsReadingFault::TrailingBytes);
}
