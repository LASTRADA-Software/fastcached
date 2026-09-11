// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"
#include "RemoteUpstream.hpp"

#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Cache/WriteErrorReportingStorage.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Core/Compression.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// Per-operation ceiling on talking to the shared cache.
    ///
    /// Bounded rather than generous, and short: a node waiting on an unreachable
    /// shared cache is a node not compiling, and the fall-back costs one local build.
    constexpr std::chrono::milliseconds UpstreamIoTimeout { 5'000 };

    /// Ceiling on OPENING the upstream connection, name resolution included.
    ///
    /// Separate from the I/O ceiling above, and much shorter. The two used to be
    /// one value passed twice, which gave the dial a five-second
    /// resolve-plus-connect budget nobody chose: five seconds is a sensible
    /// per-operation bound and a very long time to wait for a TCP handshake.
    constexpr std::chrono::milliseconds UpstreamConnectTimeout { 1'000 };

    /// How long the upstream's resolved address is reused before it is looked up
    /// again.
    ///
    /// **Both directions this trades off, because that asymmetry is what makes any
    /// particular number defensible.** `--upstream` names one endpoint for the life
    /// of the process, and it used to be resolved on EVERY `Fetch` and `Store` -- one
    /// `getaddrinfo` per cache operation, which on a Linux with `hosts: files dns`
    /// and no local caching resolver is a full round trip every time.
    ///
    /// - **Too old, address REMOVED** (the machine went away): the dial fails, `Fetch`
    ///   reports a miss and `Store` declines, and the build proceeds. It fails CLOSED
    ///   and self-heals at the next refresh. Cheap.
    /// - **Too old, address REASSIGNED to a machine that answers**: this node reads and
    ///   writes a *different* cache. Keys are content-derived, so a hit is by
    ///   construction the same compilation -- but that rests entirely on the trust
    ///   boundary `--upstream` already assumes, and a held address can outlive the
    ///   machine's ownership in a way a fresh lookup cannot. Cloud address recycling
    ///   is the realistic case.
    ///
    /// The second direction is why this is a bounded refresh rather than "resolve once
    /// at construction": a node runs for weeks, and resolving once would mean never
    /// noticing a re-address at all. Thirty seconds keeps the exposure to roughly one
    /// DNS TTL while removing essentially all of the per-operation cost, since a build
    /// issues far more than one cache operation per thirty seconds.
    constexpr std::chrono::milliseconds UpstreamAddressRefreshInterval { 30'000 };

    /// What the on-disk tier's B+tree is called inside `--cache-dir`.
    ///
    /// A file inside the directory rather than the directory itself, because the
    /// operator named a place to keep a cache and this tier is entitled to put more
    /// than one thing there later. It also keeps `--cache-dir` safe to point at a
    /// path that does not exist yet, which is what an operator will do.
    constexpr std::string_view DiskStoreFileName = "objects.cow";

    /// Largest single object the on-disk tier accepts.
    ///
    /// `CowTreeStorage`'s own default is 1 MiB, which is right for the memcached
    /// values it was written for and wrong here by an order of magnitude: a
    /// compile object for a template-heavy translation unit is routinely several,
    /// and refusing exactly the objects most worth not rebuilding would be a cache
    /// that quietly works least where it matters most. 256 MiB is what the daemon's
    /// `--storage-max-value` defaults to, and the page size is a fixed 16 KiB
    /// regardless -- a large value spills to overflow pages rather than widening
    /// every page in the file.
    constexpr std::size_t DiskMaxValueBytes = 256ULL * 1024ULL * 1024ULL;

    /// Open the on-disk half of the tier under `--cache-dir`.
    ///
    /// ONE process per directory, and the store now enforces it rather than
    /// asking to be trusted: `CowTreeStorage::Open` claims the file for the life
    /// of the process, so a second node pointed at one `--cache-dir` is refused
    /// with `StorageErrorCode::InUse`. That refusal is spelled out here because
    /// `--cache-dir` is the flag most likely to be copied between two nodes on
    /// one machine, and a node that will not start is worth one clear sentence.
    /// @param cfg    The parsed configuration; `cacheDir` must not be empty.
    /// @param logger Where an unenforceable claim is reported.
    /// @return The store, or why it could not be opened.
    [[nodiscard]] std::expected<std::unique_ptr<IStorage>, std::string> OpenDiskTier(NodeConfig const& cfg, ILogger& logger)
    {
        auto error = std::error_code {};
        std::filesystem::create_directories(cfg.cacheDir, error);
        if (error)
            return std::unexpected { std::format("cannot create {}: {}", cfg.cacheDir.string(), error.message()) };

        CowTreeStorage::Options options;
        options.path = cfg.cacheDir / DiskStoreFileName;
        options.maxBytes = static_cast<std::size_t>(cfg.cacheDiskBytes);
        options.maxValueBytes = DiskMaxValueBytes;
        options.compression = cfg.compression;
        options.compressionLevel = cfg.compressionLevel;
        options.compressionMinBytes = cfg.compressionMinBytes;
        auto opened = CowTreeStorage::Open(options);
        if (!opened.has_value())
        {
            if (opened.error().code == StorageErrorCode::InUse)
                return std::unexpected { std::format(
                    "cannot open {}: another process already has this cache open. A --cache-dir belongs to one "
                    "node; give this one a path of its own.",
                    options.path.string()) };
            return std::unexpected { std::format("cannot open {}: {}", options.path.string(), opened.error().ToString()) };
        }

        // Said out loud rather than assumed. A guard that silently does nothing
        // reads exactly like one that works, and this is the one place an
        // operator could still end up with two nodes on one store.
        if ((*opened)->StoreLockState() == CowTree::FilePageStore::LockState::Unavailable)
            logger.Logf(LogLevel::Warn,
                        "{} is on a filesystem that cannot lock; nothing stops a second node opening it",
                        options.path.string());
        return std::move(*opened);
    }

    /// Assemble the storage the operator asked for.
    ///
    /// The on-disk half does its reads and writes on the reactor thread the node's
    /// framed surfaces share, so a page split or a batched flush stalls every other
    /// connection on that loop for its duration -- issue #136. Opt-in, and this
    /// flag did nothing at all before, so nothing regresses; it is stated because
    /// it is the property somebody profiling a slow node will need.
    ///
    /// Two independent halves, each present only when it was configured: the
    /// in-memory one when `--cache-memory` is non-zero, the on-disk one when
    /// `--cache-dir` names a path. Both together are the `LayeredStorage` the
    /// daemon's `--storage` builds -- an LRU mirror over a canonical B+tree.
    ///
    /// Whatever comes out is wrapped in a single-shard `ShardedStorage`, and that
    /// wrapper is not about sharding. It is the lock: this tier is mutated on the
    /// reactor thread and its statistics are read by the heartbeat thread and by
    /// whatever scrapes `/metrics`, and `Snapshot()` on these backends writes a
    /// `mutable` member. The daemon reaches for the same wrapper for the same
    /// reason, which is why its `useShardingWrapper` includes `metricsEnabled`.
    /// @param cfg    The parsed configuration; at least one half must be configured.
    /// @param logger Passed to the on-disk half, which reports an unenforceable claim.
    /// @return The storage, or why the on-disk half could not be opened.
    [[nodiscard]] std::expected<std::unique_ptr<IStorage>, std::string> BuildStorage(NodeConfig const& cfg, ILogger& logger)
    {
        std::unique_ptr<IStorage> storage;
        if (!cfg.cacheDir.empty())
        {
            auto disk = OpenDiskTier(cfg, logger);
            if (!disk.has_value())
                return std::unexpected { std::move(disk.error()) };
            storage = std::move(*disk);
        }

        if (cfg.cacheMemoryBytes != 0)
        {
            auto memory = std::make_unique<InMemoryLruStorage>(static_cast<std::size_t>(cfg.cacheMemoryBytes));
            memory->SetCompression(MemoryCompressionOf(cfg));
            storage = storage == nullptr ? std::unique_ptr<IStorage> { std::move(memory) }
                                         : std::make_unique<LayeredStorage>(std::move(memory), std::move(storage));
        }

        // A persistence failure (full disk / I/O error / corruption / read-only)
        // is otherwise invisible at the default log level: it never reaches the
        // trace-only TracingStorage, so a store silently does not happen. The
        // daemon has wrapped its backend in this since the counter existed; the
        // node did not, so it exported `fastcached_write_errors_total` and could
        // never move it, and a node whose disk tier was failing every write
        // looked identical to a healthy one on every surface an operator watches
        // (#574).
        //
        // Wrapped INSIDE the sharding wrapper, so it observes the true result the
        // composed backend returns -- the disk error LayeredStorage propagates --
        // and so it is covered by the lock that wrapper exists to provide.
        std::vector<std::unique_ptr<IStorage>> shards;
        shards.push_back(std::make_unique<WriteErrorReportingStorage>(std::move(storage), logger));
        return std::make_unique<ShardedStorage>(std::move(shards));
    }
} // namespace

CacheTier::CacheTier(std::unique_ptr<IStorage> storage,
                     std::unique_ptr<ICacheUpstream> upstream,
                     ILocalityOracle const& locality,
                     IClock& clock,
                     IMetricsSink& metrics):
    _storage { std::move(storage) },
    _upstream { std::move(upstream) },
    _cache { *_storage, *_upstream, clock, metrics },
    _proxy { _cache, metrics },
    _responder { _proxy, locality, metrics }
{
}

std::expected<std::unique_ptr<CacheTier>, std::string> CacheTier::Start(NodeIoLoop& io,
                                                                        NodeConfig const& cfg,
                                                                        std::unique_ptr<IStorage> storage,
                                                                        ICredentialSource const& credential,
                                                                        ILocalityOracle const& locality,
                                                                        IClock& clock,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger)
{
    // An absent upstream is a named type rather than a null pointer: one developer's
    // machine has no shared cache, and that configuration should not cost every call
    // site a branch.
    std::unique_ptr<ICacheUpstream> upstream;
    if (cfg.upstream.empty())
        upstream = std::make_unique<NoUpstream>();
    else
        // The loop's own connector, so this dial SUSPENDS rather than blocking. It
        // is the point of the whole exercise: this call happens inside a cache
        // answer, and answering used to be serialized -- so one upstream that took
        // five seconds held every local `fastcache-cc` behind it.
        //
        // The reactor is passed too, because with a reactor socket `SO_RCVTIMEO`
        // is inert: the per-operation ceiling is a `DeadlineTimer` that closes the
        // socket, which bounds the whole exchange rather than one call.
        upstream = std::make_unique<RemoteUpstream>(
            cfg.upstream,
            credential,
            // The node is a CLIENT of the shared cache, and had the same
            // silence the launcher did: a token set here against a daemon
            // that does not know AUTH was ignored, and nothing said so.
            [&logger](std::string_view text) { logger.Logf(LogLevel::Warn, "upstream cache: {}", text); },
            io.Connector(),
            &io.Reactor(),
            io.Resolver(),
            clock,
            UpstreamConnectTimeout,
            UpstreamIoTimeout,
            UpstreamAddressRefreshInterval);

    auto tier =
        std::unique_ptr<CacheTier> { new CacheTier { std::move(storage), std::move(upstream), locality, clock, metrics } };

    // No address in this line since #290: the cache verbs are answered on the node's
    // one 0xFC listener, and that listener names itself when it binds. A tier that
    // announced an address of its own would be a second author of it, and the two
    // would disagree the first time a bare port resolved differently.
    //
    // Each half named separately, and an absent one said out loud. "256m in
    // memory, no disk" and "no memory, 10g on disk" are different deployments and
    // an operator reading one startup line has to be able to tell which they got.
    //
    // The codec rides on the half it belongs to, and only where that half EXISTS --
    // naming a codec beside "off" would describe a tier this node does not run. It
    // is said at all because nothing else says it: no metric, no `/fleet.json` field
    // and no other log line reports which codec a tier holds, so without this an
    // operator cannot tell a compressed tier from an uncompressed one, and a setting
    // that silently reached nothing would look exactly like one that worked.
    logger.Logf(LogLevel::Info,
                "local cache tier (memory {}, disk {}, upstream {})",
                cfg.cacheMemoryBytes == 0 ? std::string { "off" }
                                          : std::format("{} {}",
                                                        FormatByteSize(static_cast<std::size_t>(cfg.cacheMemoryBytes)),
                                                        Compression::NameOf(cfg.memoryCompression)),
                cfg.cacheDir.empty()
                    ? std::string { "off" }
                    : std::format("{} {} at {}",
                                  cfg.cacheDiskBytes == 0 ? std::string { "unbounded" }
                                                          : FormatByteSize(static_cast<std::size_t>(cfg.cacheDiskBytes)),
                                  Compression::NameOf(cfg.compression),
                                  cfg.cacheDir.string()),
                cfg.upstream.empty() ? std::string { "none" } : cfg.upstream);
    return tier;
}

InMemoryLruStorage::CompressionOptions MemoryCompressionOf(NodeConfig const& cfg)
{
    return { .codec = cfg.memoryCompression,
             .level = cfg.memoryCompressionLevel,
             .minBytes = cfg.memoryCompressionMinBytes };
}

std::uint64_t IndexReserveBytesOf(CacheTier const* tier)
{
    if (tier == nullptr)
        return 0;

    // The projection, not the current cost: `indexBytes` is empty the instant a full
    // store reopens, so reserving from it reserves nothing on exactly the machine
    // #175 is about. See `StorageStats::indexBytesAtCapacity`.
    //
    // Summed over every tier that reports one rather than singling out the disk tier
    // by name, for the reason `ResidentCacheBytes` gives about its own fold: the
    // taxonomy is open, and a resident tier added later reaches this arithmetic by
    // being a row instead of by somebody remembering.
    std::uint64_t total = 0;
    auto const tiers = tier->SnapshotTiers();
    for (auto const& row: StorageTierTable)
    {
        auto const& stats = tiers[static_cast<std::size_t>(row.tier)];
        if (stats.has_value())
            total += static_cast<std::uint64_t>(stats->indexBytesAtCapacity);
    }
    return total;
}

Distributed::NodeCacheCapacity CacheCapacityOf(CacheTier const* tier)
{
    Distributed::NodeCacheCapacity out {};
    if (tier == nullptr)
        return out;

    auto const tiers = tier->SnapshotTiers();
    for (auto const& row: StorageTierTable)
    {
        auto const index = static_cast<std::size_t>(row.tier);
        // Bound to a reference before it is tested, rather than subscripted again
        // after it: `bugprone-unchecked-optional-access` can follow the guard only
        // on the same expression, and with `WarningsAsErrors` a second subscript is
        // a build failure rather than a note.
        auto const& stats = tiers[index];
        if (stats.has_value())
            out.tierBytesLimit[index] = static_cast<std::uint64_t>(stats->bytesLimit);
    }
    return out;
}

Distributed::NodeCacheLoad CacheLoadOf(CacheTier const* tier, IMetricsSink const& metrics)
{
    Distributed::NodeCacheLoad out {};
    if (tier == nullptr)
        // Absent, not zero, and all the way down: a node with no cache must not
        // report an empty one, which is what a leader would draw as a member whose
        // cache is doing nothing.
        return out;

    auto const tiers = tier->SnapshotTiers();
    for (auto const& row: StorageTierTable)
    {
        auto const index = static_cast<std::size_t>(row.tier);
        auto const& stats = tiers[index];
        if (!stats.has_value())
            continue;
        out.tiers[index] = Distributed::CacheTierUsage { .itemCount = static_cast<std::uint64_t>(stats->itemCount),
                                                         .bytesUsed = static_cast<std::uint64_t>(stats->bytesUsed),
                                                         .evictions = stats->evictions,
                                                         .indexBytes = static_cast<std::uint64_t>(stats->indexBytes) };
    }
    out.hits = metrics.Read(IMetricsSink::Counter::NodeCacheHits);
    out.misses = metrics.Read(IMetricsSink::Counter::NodeCacheMisses);
    return out;
}

std::expected<std::unique_ptr<CacheTier>, std::string> StartCacheTierOrExplain(NodeIoLoop& io,
                                                                               NodeConfig const& cfg,
                                                                               ICredentialSource const& credential,
                                                                               ILocalityOracle const& locality,
                                                                               IClock& clock,
                                                                               IMetricsSink& metrics,
                                                                               ILogger& logger)
{
    // Emptied deliberately. A node that only compiles for others wants no cache port
    // at all, and saying so must not be an error.
    //
    // Said out loud, for the reason the branch below already is: this was the one
    // way to reach "no cache tier" that logged NOTHING, so a node started with
    // `--listen-node=` had no line anywhere saying it was not caching -- while
    // `--cache-memory` and `--cache-dir`, which an operator may well have set
    // beside it, went on reading as though they meant something. It names them
    // when they did, because a flag silently doing nothing is the shape this
    // codebase keeps a list about.
    // **Whether** this surface is served is the row's answer -- the same one
    // `--print-surfaces` reads -- so a worksheet cannot name a port this function then
    // declines to bind. **Which sentence** an operator gets is this function's own,
    // because a row resolving to nothing cannot say which of two reasons applied, and
    // the two send an operator to different flags.
    if (RowFor(NodeSurface::Node).Resolve(cfg).empty() || (cfg.cacheMemoryBytes == 0 && cfg.cacheDir.empty()))
    {
        if (cfg.nodeListen.empty())
        {
            auto const configured = cfg.cacheMemoryExplicit || !cfg.cacheDir.empty();
            logger.Logf(LogLevel::Info,
                        "--listen-node is empty; serving no local cache tier{}",
                        configured ? " (--cache-memory/--cache-dir have no effect without a port)" : "");
        }
        else
            // A port with neither half configured, which is what `--cache-memory 0`
            // means without a `--cache-dir` beside it: nothing to keep objects in, so
            // no tier. Said out loud rather than left to produce a cache port
            // answering out of nothing.
            logger.Logf(LogLevel::Info, "--cache-memory 0 and no --cache-dir; serving no local cache tier");
        return std::unique_ptr<CacheTier> {};
    }

    // Built HERE rather than inside `Start`, because the two failures below are not
    // equally tolerable and only this function can tell them apart. A store that
    // will not open is always fatal -- the operator named a path, and carrying on
    // with a memory-only cache would silently deliver less than they configured --
    // while a port that will not bind is fatal only when they typed it. Building
    // inside `Start` collapsed both into one string, so a bad `--cache-dir` on a
    // node using the DEFAULT cache port would have been logged as a warning and
    // stepped over.
    // Each failure names the flag that caused it. They leave through one return
    // type, so without this a bad `--cache-dir` reached the operator as
    // "--listen-node cannot create /var/lib/...: permission denied" -- which
    // sends them to check a port.
    auto storage = BuildStorage(cfg, logger);
    if (!storage.has_value())
        return std::unexpected { std::format("--cache-dir {}", storage.error()) };

    // The bind, and the judgement about a failed one, moved to
    // `StartNodeSurfaceOrExplain` with the surfaces (#290): a listener that carries
    // two components cannot have its failure judged by one of them.
    return CacheTier::Start(io, cfg, std::move(*storage), credential, locality, clock, metrics, logger);
}

std::expected<std::string, std::string> MigrateDiskTier(NodeConfig const& cfg)
{
    if (cfg.cacheDir.empty())
        return std::unexpected { std::string {
            "--migrate-cache needs --cache-dir: a memory-only node has no on-disk store to convert" } };

    auto const path = cfg.cacheDir / DiskStoreFileName;
    CowTreeStorage::Options options;
    options.path = path;
    options.maxValueBytes = DiskMaxValueBytes;

    auto const report = CowTreeStorage::Migrate(options);
    auto line = DescribeMigration(path, report);
    if (!report.has_value())
        return std::unexpected { std::move(line) };
    return line;
}

} // namespace FastCache::Node
