// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// Everything `StartCacheTierOrExplain` needs, over one clock.
///
/// The reactor is declared LAST, so among the members it is destroyed FIRST -- and
/// that is fine here only because no member holds a tier. A case keeps its tier in
/// a local declared AFTER the fixture, which is therefore destroyed BEFORE it, so
/// the tier's destructor posts its listener close onto a reactor that is still
/// there. Reversing the two is not a compile error and would present as a close
/// posted onto a reactor that has gone.
struct Fixture
{
    ManualClock clock;
    AtomicMetricsSink metrics;
    CapturingLogger logger;

    /// A machine with no addresses of its own, which is not a limitation here: the
    /// tier is dialled over loopback, and loopback is answered before the set is
    /// consulted at all. Cases about WHO is admitted live in `CacheProxy_test`,
    /// where the peer address is an argument rather than whatever the kernel says.
    Testing::ScriptedHostAddresses hostAddresses;
    CachedLocalityOracle locality { hostAddresses, clock };
    NodeIoLoop io;

    /// A config bound to a port found at run time, so cases never collide.
    ///
    /// Not a number somebody picked: `catch_discover_tests` runs every case as its
    /// own process and the suite runs in parallel, so a fixed port is a failure
    /// that appears only under `ctest -j`. And not `0` either -- `ParseTcpPort`
    /// rejects it, correctly, since as a CLI value it names no port an operator
    /// could dial -- so a probe is bound and released to find one.
    [[nodiscard]] static NodeConfig BaseConfig()
    {
        auto probe = BlockingListener::Bind("127.0.0.1", 0);
        REQUIRE(probe);
        // Asked of the SOCKET, not of the pointer: `Bind` returns a listener in an
        // errored state rather than nothing, so a null check passes on a bind that
        // failed and the port below comes back 0.
        REQUIRE(probe->IsBound());
        auto const port = probe->BoundPort();
        probe.reset();

        NodeConfig cfg;
        cfg.nodeListen = std::format("127.0.0.1:{}", port);
        return cfg;
    }

    /// Start a tier over @p cfg.
    /// @param cfg What the node was told to be.
    /// @return The tier, a null tier meaning "carry on without one", or the reason.
    /// What the tier's upstream client presents. These cases are about WHICH tier is
    /// built and what it reports, never about the credential -- so this is production's
    /// own "no configuration file, no secret" source rather than a fake.
    NodeConfig unauthenticated;
    ConfiguredCredential credential { unauthenticated, nullptr };

    [[nodiscard]] std::expected<std::unique_ptr<CacheTier>, std::string> Start(NodeConfig const& cfg)
    {
        return StartCacheTierOrExplain(io, cfg, credential, locality, clock, metrics, logger);
    }
};

/// Whether any captured line contains @p needle.
/// @param logger Where the tier reported.
/// @param needle Text to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view needle)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records, [needle](CapturingLogger::Record const& r) { return r.message.contains(needle); });
}

/// One tier's entry, or nullopt when the cache has no such tier.
///
/// A template over the table, because the two this file reads -- `SnapshotTiers()`'s
/// statistics and `CacheCapacityOf`'s budgets -- are both an `EnumTable` keyed by
/// tier, and two helpers with one body is two things to keep in step.
/// @param tiers Any per-tier table.
/// @param tier Which tier to read.
/// @return That tier's entry.
template <typename Table>
[[nodiscard]] auto const& At(Table const& tiers, StorageTier tier)
{
    return tiers[static_cast<std::size_t>(tier)];
}

/// A payload that compresses hugely, so a codec effect cannot be mistaken for noise.
/// @param bytes How long.
/// @return That many identical bytes.
[[nodiscard]] std::vector<std::byte> Compressible(std::size_t bytes)
{
    return std::vector<std::byte>(bytes, std::byte { 0x41 });
}

/// The status of a framed reply.
/// @param reply The reply bytes.
/// @return Its status, or nullopt when it does not decode.
[[nodiscard]] std::optional<Wire::Status> StatusOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    return header.has_value() ? std::optional { header->status } : std::nullopt;
}

} // namespace

TEST_CASE("A node told to hold nothing serves no cache tier", "[node][cache-tier]")
{
    // `--cache-memory 0` used to build an InMemoryLruStorage with a zero budget,
    // which is how that class spells UNBOUNDED -- so the one flag an operator can
    // switch this off with was the one that removed the limit, and a node told to
    // hold nothing grew until the machine ran out of memory. Silent throughout:
    // the cache works, and works better and better.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
}

TEST_CASE("A node with no cache port serves no cache tier", "[node][cache-tier]")
{
    // `--listen-node=` is the documented way to turn the local cache off, and it
    // turns it off WITHOUT touching `--cache-memory` -- whose default is a share of
    // host RAM, so it is left non-zero here rather than relying on the runner's.
    // That pairing, a budget asked for and no tier built, is what made sizing the
    // machine's reservation from the flag reserve a quarter of RAM for nothing
    // (#167).
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.nodeListen.clear();
    cfg.cacheMemoryBytes = 8ULL * 1024 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier == nullptr);

    // Absent, not zero, and all the way down: this is the record `NodeCapacityOf`
    // derives the reservation from, so a memory tier reported here for a tier that
    // was never built is memory the fleet would never get back.
    auto const budget = CacheCapacityOf(tier.get());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());

    // And it says so. This was the one route to "no cache tier" that logged
    // nothing at all, so an operator had no line anywhere telling them this node
    // was not caching.
    CHECK(Logged(fixture.logger, "serving no local cache tier"));
}

TEST_CASE("A cache budget with no port to serve it is called out", "[node][cache-tier]")
{
    // `--cache-memory` and `--cache-dir` do nothing at all without a port, and a
    // flag silently doing nothing is the shape this codebase keeps a list about.
    // Named only when the operator actually set one -- a default nobody typed is
    // not something to warn them about.
    Testing::ScratchDirectory const scratch { "node-cache-no-port" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.nodeListen.clear();
    cfg.cacheDir = scratch.Path();

    auto const started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    CHECK(*started == nullptr);
    CHECK(Logged(fixture.logger, "--cache-memory/--cache-dir have no effect"));
}

TEST_CASE("A memory-only node reports its budget and no disk tier", "[node][cache-tier]")
{
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    // Absent rather than a disk tier holding nothing: an operator who did not ask
    // for `--cache-dir` has no disk tier to be empty.
    CHECK_FALSE(At(tiers, StorageTier::Disk).has_value());

    // And the announced record says the same, because that is the one
    // `NodeCapacityOf` subtracts from this machine's RAM (#167). Asserted here
    // rather than in a case of its own: `CacheCapacityOf` is a projection of the
    // snapshot above, and this file's fixture costs a process and a port probe.
    auto const budget = CacheCapacityOf(tier.get());
    REQUIRE(At(budget.tierBytesLimit, StorageTier::Memory).has_value());
    CHECK(Unwrap(At(budget.tierBytesLimit, StorageTier::Memory)) == cfg.cacheMemoryBytes);
    CHECK_FALSE(At(budget.tierBytesLimit, StorageTier::Disk).has_value());
}

// The name may not START with `--`: `catch_discover_tests` passes it to the
// runner as an argument, and Catch2 reads a leading double dash as a flag and
// exits with "Unrecognised token" -- which ctest reports as a failing test that
// passes when run by hand.
TEST_CASE("Naming --cache-dir gives the node an on-disk tier", "[node][cache-tier]")
{
    // The flag was parsed and round-tripped by the config writer and read by
    // nothing: `CacheTier::Start` built a bare in-memory store and never looked at
    // the path. A node configured with a disk cache had none, and said so nowhere.
    Testing::ScratchDirectory const scratch { "node-cache-dir" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 4 * 1024 * 1024;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    REQUIRE(At(tiers, StorageTier::Memory).has_value());
    REQUIRE(At(tiers, StorageTier::Disk).has_value());
    // Distinct budgets, so a report that named one tier's figure under the other's
    // label could not pass: the memory half is capped and the disk half is not.
    CHECK(Unwrap(At(tiers, StorageTier::Memory)).bytesLimit == cfg.cacheMemoryBytes);
    CHECK(Unwrap(At(tiers, StorageTier::Disk)).bytesLimit == 0);
}

TEST_CASE("A disk-only node keeps its disk tier", "[node][cache-tier]")
{
    // `--cache-memory 0 --cache-dir <path>` turns off the in-memory half and
    // nothing else. Reading it as "no cache at all" would silently discard a tier
    // the operator named a path for.
    Testing::ScratchDirectory const scratch { "node-cache-disk-only" };
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 0;
    cfg.cacheDir = scratch.Path();

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    auto const tier = std::move(*started);
    REQUIRE(tier != nullptr);

    auto const tiers = tier->SnapshotTiers();
    CHECK_FALSE(At(tiers, StorageTier::Memory).has_value());
    CHECK(At(tiers, StorageTier::Disk).has_value());
}

TEST_CASE("A cache directory that cannot be opened is fatal when it was named", "[node][cache-tier]")
{
    // The same rule `--listen-node` follows: an address the operator TYPED is a
    // promise, and a broken promise stops startup rather than being logged and
    // carried on from. `--cache-dir` has no default at all, so naming it is the
    // only way to reach this path.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 1024 * 1024;
    // A path under a regular file, which no platform will create a store beneath.
    Testing::ScratchDirectory const scratch { "node-cache-bad-dir" };
    auto const blocker = scratch / "not-a-directory";
    {
        std::ofstream const file { blocker };
    }
    cfg.cacheDir = blocker / "cache";

    auto const started = fixture.Start(cfg);
    REQUIRE_FALSE(started.has_value());
    // And it names the flag that failed. Both failures this function can report
    // leave through one string, so a directory it could not create used to reach
    // the operator as "--listen-node ..." and send them to check a port.
    CHECK(started.error().contains("--cache-dir"));
    CHECK_FALSE(started.error().contains("--listen-node"));
}

// The provenance rule -- a NAMED address is a promise and a broken promise is fatal,
// a DEFAULTED one is warned past (#286) -- moved to `NodeFrameSurface_test` with the
// bind itself when the cache and scheduler surfaces merged (#290). This tier opens no
// listener any more, so a case here could only have asserted it through a function
// that no longer decides it.

TEST_CASE("A tier says whether it has a shared cache behind it", "[node][cache-tier]")
{
    // #214's wiring, asserted rather than assumed. The scrape's
    // `upstream_configured` gauge is read off the upstream that was BUILT, so this
    // is the join between the configuration and what a scrape reports -- and a
    // field nothing populates renders a confident `0` for every node, which is the
    // failure mode the gauge exists to prevent.
    Fixture fixture;

    SECTION("none configured")
    {
        auto cfg = Fixture::BaseConfig();
        cfg.upstream.clear();

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK_FALSE(tier->HasUpstream());
    }

    SECTION("one configured")
    {
        // Named, not reachable -- and that is the distinction. `Configured()` says an
        // operator asked for a shared cache; whether it answers is what the store
        // counters report, and conflating the two is how a laptop came to look like
        // a fleet whose cache is down.
        auto cfg = Fixture::BaseConfig();
        cfg.upstream = "127.0.0.1:1";

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);
        CHECK(tier->HasUpstream());
    }
}

// --- compression --------------------------------------------------------------
//
// These cases exist because a compression setting that reaches no tier is INERT and
// indistinguishable, from every other surface, from one that works: the object still
// stores, still reads back byte-for-byte, and every counter moves exactly as before.
// So what is asserted is the DIFFERENCE between a tier told to compress and one told
// not to -- never the round trip alone, which passes under the bug.

TEST_CASE("MemoryCompressionOf carries what the node was told", "[node][cache-tier][compression]")
{
    NodeConfig cfg;
    cfg.memoryCompression = CompressionCodec::Zstd;
    cfg.memoryCompressionLevel = 9;
    cfg.memoryCompressionMinBytes = 1234;

    auto const options = MemoryCompressionOf(cfg);
    CHECK(options.codec == CompressionCodec::Zstd);
    CHECK(options.level == 9);
    CHECK(options.minBytes == 1234);

    // The default is OFF, stated here rather than assumed: turning it on by default
    // would change the CPU cost of every existing deployment on upgrade.
    CHECK(MemoryCompressionOf(NodeConfig {}).codec == CompressionCodec::Identity);
}

TEST_CASE("The memory tier compresses only when a codec is named", "[node][cache-tier][compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        SKIP("this build has no zstd, so there is no codec to tell apart from none");

    constexpr std::size_t PayloadBytes = 64 * 1024;
    auto const payload = Compressible(PayloadBytes);

    // One tier per codec, each storing the same object, each asked what it now holds.
    auto residentBytes = [&payload](CompressionCodec codec) {
        Fixture fixture;
        auto cfg = Fixture::BaseConfig();
        cfg.cacheMemoryBytes = 8 * 1024 * 1024;
        cfg.memoryCompression = codec;
        cfg.memoryCompressionMinBytes = 1024;

        auto started = fixture.Start(cfg);
        REQUIRE(started.has_value());
        auto const tier = std::move(*started);
        REQUIRE(tier != nullptr);

        auto const stored = SyncRun(tier->Responder().Answer(
            Wire::EncodeStore(Wire::StoreRequest {
                .key = "object", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = payload }),
            "127.0.0.1"));
        REQUIRE(StatusOf(stored) == Wire::Status::Ok);

        // And it must still be READABLE: a tier that compressed and could not decode
        // would shrink exactly as convincingly.
        auto const fetched = SyncRun(tier->Responder().Answer(Wire::EncodeFetch("object"), "127.0.0.1"));
        REQUIRE(StatusOf(fetched) == Wire::Status::Ok);

        auto const tiers = tier->SnapshotTiers();
        REQUIRE(At(tiers, StorageTier::Memory).has_value());
        return Unwrap(At(tiers, StorageTier::Memory)).bytesUsed;
    };

    auto const plain = residentBytes(CompressionCodec::Identity);
    auto const packed = residentBytes(CompressionCodec::Zstd);

    // The budget counts STORED bytes, which is the whole point: an uncompressed tier
    // is charged the object, a compressed one a fraction of it. A ratio rather than a
    // constant, because how well zstd does on this payload is for zstd to decide.
    CHECK(plain >= PayloadBytes);
    CHECK(packed < plain / 2);
}

TEST_CASE("The disk tier compresses with the codec the node names", "[node][cache-tier][compression]")
{
    if (!Compression::IsAvailable(CompressionCodec::Zstd))
        SKIP("this build has no zstd, so there is no codec to tell apart from none");

    constexpr std::size_t PayloadBytes = 256 * 1024;
    auto const payload = Compressible(PayloadBytes);

    // Measured on the STORE FILE, not on `bytesUsed`.
    //
    // The two tiers denominate their budgets differently, and it is easy to assert
    // the wrong one: `InMemoryLruStorage` charges STORED bytes, so compression shows
    // up there, while `CowTreeStorage` charges `originalLen` -- the pre-compression
    // size (`CowTreeStorage.cpp:1337`) -- so a compressed disk tier reports exactly
    // the same `bytesUsed` as an uncompressed one. Asserting on it here would have
    // compared 65536 with 65536 and read as "the codec did nothing", which is a true
    // observation carrying a false claim.
    //
    // It also means `--cache-disk` bounds LOGICAL bytes: a compressed disk tier holds
    // its cap in pre-compression terms and occupies less than that on the filesystem.
    auto fileBytes = [&payload](CompressionCodec codec, std::string_view scratchName) {
        Testing::ScratchDirectory const scratch { scratchName };
        auto const store = scratch.Path() / "objects.cow";
        {
            Fixture fixture;
            auto cfg = Fixture::BaseConfig();
            // No memory tier, so nothing mirrors the value into an L1 instead.
            cfg.cacheMemoryBytes = 0;
            cfg.cacheDir = scratch.Path();
            cfg.compression = codec;
            cfg.compressionMinBytes = 1024;

            auto started = fixture.Start(cfg);
            REQUIRE(started.has_value());
            auto const tier = std::move(*started);
            REQUIRE(tier != nullptr);

            auto const stored = SyncRun(tier->Responder().Answer(
                Wire::EncodeStore(Wire::StoreRequest {
                    .key = "object", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = payload }),
                "127.0.0.1"));
            REQUIRE(StatusOf(stored) == Wire::Status::Ok);

            // Still readable: a tier that compressed and could not decode would
            // shrink the file exactly as convincingly.
            auto const fetched = SyncRun(tier->Responder().Answer(Wire::EncodeFetch("object"), "127.0.0.1"));
            REQUIRE(StatusOf(fetched) == Wire::Status::Ok);

            // The budget is denominated in logical bytes whatever the codec, which is
            // the other half of the note above and is worth pinning: if this ever
            // starts tracking the compressed size, the assertion below is measuring
            // something else.
            auto const tiers = tier->SnapshotTiers();
            REQUIRE(At(tiers, StorageTier::Disk).has_value());
            CHECK(Unwrap(At(tiers, StorageTier::Disk)).bytesUsed >= PayloadBytes);
        }
        // Sized after the tier is closed, so the last commit has certainly landed.
        std::error_code error;
        auto const size = std::filesystem::file_size(store, error);
        REQUIRE_FALSE(error);
        return static_cast<std::size_t>(size);
    };

    auto const plain = fileBytes(CompressionCodec::Identity, "node-disk-codec-none");
    auto const packed = fileBytes(CompressionCodec::Zstd, "node-disk-codec-zstd");

    // A ratio rather than a constant: how well zstd does on this payload is for zstd
    // to decide, and the file carries pages of tree overhead either way.
    CHECK(plain >= PayloadBytes);
    CHECK(packed < plain / 2);
}

TEST_CASE("The startup line names each tier codec", "[node][cache-tier][compression]")
{
    // Nothing else reports it -- no metric, no fleet.json field -- so without this an
    // operator cannot tell a compressed tier from an uncompressed one.
    Fixture fixture;
    auto cfg = Fixture::BaseConfig();
    cfg.cacheMemoryBytes = 8 * 1024 * 1024;
    cfg.memoryCompression = CompressionCodec::Identity;

    auto started = fixture.Start(cfg);
    REQUIRE(started.has_value());
    REQUIRE(*started != nullptr);

    CHECK(Logged(fixture.logger, "memory 8M none"));
    // The disk half is absent, and an absent tier gets no codec: "off zstd" would
    // describe a tier this node does not run.
    CHECK(Logged(fixture.logger, "disk off"));
}
