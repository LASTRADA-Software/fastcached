// SPDX-License-Identifier: Apache-2.0
//
// fastcached — Fast Cache Daemon entry point.
//
// Wiring: CLI -> YAML file (named by --config, else discovered at the
// platform's default location) -> ConfigReloader -> CacheEngine over
// the storage backend -> RunReactorServer, hosted by the requested
// IDaemonHost (foreground / POSIX daemon / Windows service).
// SIGINT/SIGTERM and SCM stop trigger graceful shutdown;
// SIGHUP and SCM PARAMCHANGE trigger config reload.

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Cache/WriteErrorReportingStorage.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/PathKind.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/HealthProbe.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/ServiceControl.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Platform/WindowsEventLogger.hpp>
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>
#include <FastCache/Protocol/RedisMutationObserver.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>
#include <FastCache/Protocol/StreamWaiterRegistry.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace
{

constexpr std::string_view ProgramVersion = FastCache::VersionString;

/// Read the metrics port from the FASTCACHED_METRICS_PORT environment variable.
/// This lets a container's daemon CMD and its separate `--healthcheck` probe
/// agree on a custom port via a single `-e FASTCACHED_METRICS_PORT=...`, without
/// the static image HEALTHCHECK having to learn the daemon's runtime args.
///
/// Reuses the CLI's `FastCache::ParsePort`, so the environment fallback accepts
/// exactly the same syntax and range as the `--metrics-port` flag.
/// @return The parsed port, or nullopt when unset/empty/invalid.
[[nodiscard]] std::optional<std::uint16_t> MetricsPortFromEnv()
{
    auto const raw = FastCache::ReadEnvironmentVariable("FASTCACHED_METRICS_PORT");
    if (!raw.has_value())
        return std::nullopt;

    // ParsePort already rejects the empty string, so an unset and a blank
    // variable land in the same place without a second emptiness rule here.
    auto const parsed = FastCache::ParsePort(*raw);
    if (!parsed.has_value())
        return std::nullopt;
    return *parsed;
}

/// Install `templatePath` at the machine-wide config location, unless a config
/// is already there. The `--seed-config` action: how a packaging format with no
/// conffile mechanism ships a default config that survives its own upgrades.
/// @param templatePath The shipped template to copy.
/// @return Process exit code.
[[nodiscard]] int SeedDefaultConfig(std::string const& templatePath)
{
    FastCache::SystemConfigPathProbe const probe;
    auto const destination = FastCache::SystemConfigPath(probe, FastCache::DaemonApplicationName);
    auto const seeded = destination.and_then([&](auto const& dest) {
        return FastCache::SeedConfigFile(templatePath, dest, FastCache::DirectoryPolicy::AdministratorsOnly);
    });
    if (!seeded.has_value())
    {
        std::println(std::cerr, "fastcached: {}", seeded.error().ToString());
        return EXIT_FAILURE;
    }

    std::println(
        "fastcached: {} {}", *seeded == FastCache::SeedOutcome::Written ? "wrote" : "kept existing", destination->string());
    return EXIT_SUCCESS;
}

extern "C" void HandleStopSignal(int /*signum*/)
{
    FastCache::DaemonControls::Instance().RequestStop();
}

#if !defined(_WIN32)
extern "C" void HandleReloadSignal(int /*signum*/)
{
    FastCache::DaemonControls::Instance().RequestReload();
}
#endif

void InstallStopHandlers()
{
    std::signal(SIGINT, &HandleStopSignal);
    std::signal(SIGTERM, &HandleStopSignal);
#if !defined(_WIN32)
    std::signal(SIGHUP, &HandleReloadSignal);
#endif
}

/// Pick a default shard count when the user left it at 0 (auto):
/// min(16, hardware_concurrency), floor of 1.
[[nodiscard]] std::size_t AutoShardCount() noexcept
{
    auto const hw = std::thread::hardware_concurrency();
    auto const cap = std::min<unsigned>(hw == 0 ? 1U : hw, 16U);
    return static_cast<std::size_t>(cap);
}

/// Resolve the effective shard count. Sharding fans writes across independent
/// CoW files so the multi-core reactors write in parallel.
///
/// - User-specified non-zero: honored verbatim.
/// - Auto (0) for in-memory: fan out (`AutoShardCount`).
/// - Auto (0) for persistent: fan out **by default** so disk writes
///   parallelise, EXCEPT for paths that name a single file
///   (`FastCache::PathNamesAFile`), which stay single-file for backward
///   compatibility — the path is used as one file, never `mkdir`-ed over. The
///   install-time handover asks the same predicate before it creates anything, so
///   what the first start opens and what `--install-service` may create at that
///   path cannot drift apart.
[[nodiscard]] std::size_t ResolvePhysicalShards(std::size_t requested,
                                                bool usingPersistent,
                                                std::filesystem::path const& storagePath) noexcept
{
    if (requested != 0)
        return requested;
    if (!usingPersistent)
        return AutoShardCount();
    return FastCache::PathNamesAFile(storagePath) ? 1 : AutoShardCount();
}

/// Translate the user-facing StorageDurability into the page-store enum.
CowTree::FilePageStore::Durability ToPageStoreDurability(FastCache::StorageDurability d) noexcept
{
    switch (d)
    {
        case FastCache::StorageDurability::Fsync:
            return CowTree::FilePageStore::Durability::Fsync;
        case FastCache::StorageDurability::Batched:
            return CowTree::FilePageStore::Durability::Batched;
        case FastCache::StorageDurability::None:
            return CowTree::FilePageStore::Durability::None;
    }
    return CowTree::FilePageStore::Durability::Batched;
}

/// Translate the config-layer LRU recency policy into the cache backend's enum.
/// @param r Config recency policy.
/// @return The corresponding InMemoryLruStorage mode.
[[nodiscard]] FastCache::LruMode ToLruMode(FastCache::LruRecency r) noexcept
{
    return r == FastCache::LruRecency::Strict ? FastCache::LruMode::Strict : FastCache::LruMode::Approximate;
}

/// Build an L1 tier with the configured byte budget and in-memory codec.
///
/// Every L1 instance goes through here so the memory-compression settings cannot be
/// applied to some shards and silently missed on others.
/// @param effective  Merged configuration.
/// @param maxBytes   Byte budget for this instance (per-shard where sharded).
/// @return The configured storage.
[[nodiscard]] std::unique_ptr<FastCache::InMemoryLruStorage> MakeL1(FastCache::Config const& effective, std::size_t maxBytes)
{
    auto l1 = std::make_unique<FastCache::InMemoryLruStorage>(
        maxBytes, effective.storageMaxValueBytes, ToLruMode(effective.lruRecency));
    l1->SetCompression({ .codec = effective.memoryCompression,
                         .level = effective.memoryCompressionLevel,
                         .minBytes = effective.memoryCompressionMinBytes });
    return l1;
}

/// Holds the assembled storage chain. The reload subscriber resizes it
/// through the virtual `IStorage::Resize`, so no typed observer pointers
/// are needed.
struct StorageBackendBundle
{
    std::unique_ptr<FastCache::IStorage> backend;
};

/// Advice to append to a storage failure when this process cannot write there.
///
/// The daemon no longer runs as the platform's most privileged account, so a
/// storage directory it cannot write is now something an ordinary, correct
/// install can hit: the operator set `storage_path` after the service was
/// registered, so nothing ever handed that directory over. Under a supervisor the
/// only symptom is a job that exits at every start, and the errno text alone says
/// nothing about which account was refused or how to grant it.
///
/// Gated on an actual write attempt rather than on the error text, because the
/// three call sites that need this most report a *string* from the storage layer
/// rather than an `error_code`, and because parsing a localized "permission
/// denied" is not something to build advice on. What the probe establishes is
/// exactly what the message then claims -- that this process cannot create a file
/// there -- so a full disk gets a true statement rather than a wrong diagnosis.
///
/// Windows only: this is where the identity changed. The POSIX services already
/// ran unprivileged and their packaging hands their directories over, and naming
/// an account here would have to guess between the macOS `_fastcached` and the
/// Linux `fastcached` -- and would be wrong for a foreground run under either.
///
/// And on Windows only when this process IS that service. The probe proves that
/// *this* process cannot write, which says nothing about who the service runs as:
/// an administrator running the daemon in a console would otherwise be handed a
/// diagnosis naming an identity that is not running their process, and an `icacls`
/// line granting an account that may not be registered at all. `--daemon` is
/// precisely the "started by the SCM" signal, because it is what the registration
/// bakes into the service's command line.
///
/// @param effective The merged configuration -- its storage location, whether this
///        process was started as the service, and the service's own name, which is
///        what the SCM derives its virtual account from (`Config{}.serviceName`
///        would name the wrong account for an install made with `--service-name`).
/// @return A sentence naming the remedy, or an empty string.
[[nodiscard]] std::string StorageAccessHint([[maybe_unused]] FastCache::Config const& effective)
{
#if defined(_WIN32)
    if (!effective.daemon)
        return {};

    std::filesystem::path const storagePath { effective.storagePath };

    // The directory itself when it exists, else the parent that would have to
    // hold it -- a path that could not be created is a permission problem one
    // level up.
    std::error_code ec;
    auto const directory = std::filesystem::is_directory(storagePath, ec) ? storagePath : storagePath.parent_path();
    if (directory.empty())
        return {};

    // A directory that is not there produces the same failed `ofstream` as one
    // this account cannot write, and the two need opposite answers. Saying
    // "grant NT SERVICE\... access" for a path nobody created sends the operator
    // to an `icacls` command that fails on its own terms.
    if (std::error_code dirEc; !std::filesystem::is_directory(directory, dirEc))
    {
        // "is not a directory" rather than "does not exist": the guard tests the
        // former, and the two need different answers. A `--storage` whose parent is
        // a regular FILE would otherwise be answered with "create it", against a
        // path that is already there.
        std::error_code existsEc;
        return std::filesystem::exists(directory, existsEc)
                   ? std::format("\nThe storage path {} cannot be used: {} exists but is not a directory.",
                                 storagePath.string(),
                                 directory.string())
                   : std::format("\nThe storage path {} has no directory to live in: {} does not exist. "
                                 "Create it, then grant the service account access to it.",
                                 storagePath.string(),
                                 directory.string());
    }

    auto const probe = directory / ".fastcached-access-probe";
    {
        std::ofstream out { probe };
        if (out)
        {
            out.close();
            std::filesystem::remove(probe, ec);
            return {};
        }
    }
    std::filesystem::remove(probe, ec);

    return std::format("\nThis process cannot create files in {}. The service runs as the virtual account "
                       "NT SERVICE\\{}, which has no rights of its own to a directory an administrator made. "
                       "Grant it from an elevated prompt:\n"
                       "    icacls \"{}\" /grant \"NT SERVICE\\{}\":(OI)(CI)F",
                       directory.string(),
                       effective.serviceName,
                       directory.string(),
                       effective.serviceName);
#else
    return {};
#endif
}

/// What an operator is told when a store will not open, whichever shape of
/// storage produced it.
///
/// One function because the three call sites below differ only in the noun --
/// the whole store, or one shard of it -- and a message assembled separately at
/// each is a message that drifts at two of them.
///
/// It also picks the remedy. `StorageAccessHint` has to *probe* for its answer,
/// because a permission problem arrives as an unhelpfully generic failure,
/// whereas `InUse` needs no probe at all: the code IS the diagnosis, and it is
/// the one open failure whose cause is another process rather than anything
/// about the file or the directory. So an operator whose real problem is the
/// other daemon is never handed an `icacls` command instead.
/// @param what      What the path names to an operator: "storage" or "shard".
/// @param path      The path that would not open.
/// @param effective The merged configuration.
/// @param error     What `CowTreeStorage::Open` reported.
/// @return The sentence to fail with.
[[nodiscard]] std::string StorageOpenFailure(std::string_view what,
                                             std::string_view path,
                                             FastCache::Config const& effective,
                                             FastCache::StorageError const& error)
{
    auto const remedy = error.code == FastCache::StorageErrorCode::InUse
                            ? std::string { "\nAnother process already has this store open. A storage path belongs "
                                            "to one daemon: stop the other one, or give this daemon a path of its own." }
                            : StorageAccessHint(effective);
    return std::format("failed to open {} '{}': {}{}", what, path, error.ToString(), remedy);
}

/// Open a CowTreeStorage at `path` and wrap it in a LayeredStorage(L1
/// InMemoryLruStorage, L2 CowTreeStorage). The L1 cache owns the
/// per-shard memory budget; the disk tier is unbounded for now.
///
/// Fails with the `StorageError` rather than a rendered string, so the CODE
/// survives to the caller that composes the operator's message. Flattened here,
/// "another process has this store open" would be one more opaque open failure
/// by the time anybody could act on it.
/// @param path              Where this shard's B+tree lives.
/// @param effective         The merged configuration.
/// @param perShardBytes     RAM budget for this shard's L1.
/// @param perShardDiskBytes On-disk budget for this shard's L2; 0 is unbounded.
/// @param logger            Where an unenforceable exclusive claim is reported.
/// @return The layered shard, or why its store would not open.
[[nodiscard]] std::expected<std::unique_ptr<FastCache::LayeredStorage>, FastCache::StorageError> BuildLayeredShard(
    std::filesystem::path const& path,
    FastCache::Config const& effective,
    std::size_t perShardBytes,
    std::size_t perShardDiskBytes,
    FastCache::ILogger& logger)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = path;
    // L2 disk budget: 0 = unbounded (grows as needed); non-zero caps the
    // on-disk tier and makes the CoW tree evict its LRU tail to fit. L1 holds
    // the RAM byte budget (perShardBytes) independently.
    opts.maxBytes = perShardDiskBytes;
    opts.durability = ToPageStoreDurability(effective.storageDurability);
    opts.maxValueBytes = effective.storageMaxValueBytes;
    opts.compression = effective.compression;
    opts.compressionLevel = effective.compressionLevel;
    opts.compressionMinBytes = effective.compressionMinBytes;
    auto opened = FastCache::CowTreeStorage::Open(opts);
    if (!opened.has_value())
        return std::unexpected(opened.error());

    // Read before the store disappears into the LayeredStorage, and said out
    // loud rather than assumed: a guard that silently does nothing reads
    // exactly like one that works.
    if ((*opened)->StoreLockState() == CowTree::FilePageStore::LockState::Unavailable)
        logger.Logf(FastCache::LogLevel::Warn,
                    "{} is on a filesystem that cannot lock; nothing stops a second daemon opening it",
                    path.string());
    auto l1 = MakeL1(effective, perShardBytes);
    return std::make_unique<FastCache::LayeredStorage>(std::move(l1), std::move(*opened));
}

/// The store files a persistent configuration names, in shard order.
///
/// The one place that knows the layout, so nothing derives `shard-NN.cow` for
/// itself.
/// @param storagePath    The configured `--storage` path.
/// @param physicalShards How many shards the configuration resolves to.
/// @return One path per shard: the path itself when unsharded, else
///         `shard-NN.cow` beneath it.
[[nodiscard]] std::vector<std::filesystem::path> StorageShardPaths(std::string const& storagePath,
                                                                   std::size_t physicalShards)
{
    if (physicalShards <= 1)
        return { std::filesystem::path { storagePath } };

    // Built by hand rather than with `std::ranges::to`, which clang 22 over
    // libstdc++ 14 rejects here ("deduced return type cannot be used before it
    // is defined") -- and which nothing else in this tree uses, so the one
    // caller is not worth the portability.
    std::filesystem::path const dir { storagePath };
    std::vector<std::filesystem::path> paths;
    paths.reserve(physicalShards);
    for (auto const i: std::views::iota(std::size_t { 0 }, physicalShards))
        paths.push_back(dir / std::format("shard-{:02d}.cow", i));
    return paths;
}

/// Construct the multi-shard inner storages for a ShardedStorage
/// wrapper. Handles three shapes: in-memory fan-out, single-file
/// persistent, and multi-file persistent (directory of shard-NN.cow).
[[nodiscard]] std::expected<std::vector<std::unique_ptr<FastCache::IStorage>>, std::string> BuildShardedInners(
    FastCache::Config const& effective,
    bool usingPersistent,
    std::size_t physicalShards,
    std::size_t perShardBytes,
    std::size_t perShardDiskBytes,
    FastCache::ILogger& logger)
{
    std::vector<std::unique_ptr<FastCache::IStorage>> inners;
    inners.reserve(physicalShards);

    if (!usingPersistent)
    {
        for (std::size_t i = 0; i < physicalShards; ++i)
            inners.emplace_back(MakeL1(effective, perShardBytes));
        return inners;
    }

    if (physicalShards == 1)
    {
        auto layered = BuildLayeredShard(effective.storagePath, effective, perShardBytes, perShardDiskBytes, logger);
        if (!layered.has_value())
            return std::unexpected(StorageOpenFailure("storage", effective.storagePath, effective, layered.error()));
        inners.push_back(std::move(*layered));
        return inners;
    }

    std::filesystem::path const dir { effective.storagePath };
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return std::unexpected(std::format("failed to create storage directory '{}': {}{}",
                                           effective.storagePath,
                                           ec.message(),
                                           StorageAccessHint(effective)));

    for (auto const& path: StorageShardPaths(effective.storagePath, physicalShards))
    {
        auto layered = BuildLayeredShard(path, effective, perShardBytes, perShardDiskBytes, logger);
        if (!layered.has_value())
            return std::unexpected(StorageOpenFailure("shard", path.string(), effective, layered.error()));
        inners.push_back(std::move(*layered));
    }
    return inners;
}

/// Build the full storage chain and the typed observer pointers used by
/// the reload subscriber.
///
/// Physical shapes:
///   - In-memory: N InMemoryLruStorage instances (one per shard).
///   - Persistent: N CowTreeStorage instances on disk, each one fronted
///     by an in-memory LRU cache via LayeredStorage. Reads hit RAM
///     first; writes pass through to disk (canonical CAS) and mirror
///     into the RAM cache.
///
/// Concurrency wrapping (orthogonal to physical fan-out):
///   - Threaded execution: always wrap in ShardedStorage (even at
///     shards==1) so workers serialise via the per-shard mutex.
///   - Reactor execution: single-shard backends run unwrapped.
[[nodiscard]] std::expected<StorageBackendBundle, std::string> BuildStorageBackend(FastCache::Config const& effective,
                                                                                   bool usingPersistent,
                                                                                   bool useShardingWrapper,
                                                                                   std::size_t physicalShards,
                                                                                   FastCache::ILogger& logger)
{
    auto const perShardBytes = physicalShards > 0 ? effective.maxMemoryBytes / physicalShards : effective.maxMemoryBytes;
    // Split the on-disk budget the same way as the RAM budget. 0 stays 0
    // (unbounded) after the division, so the default remains "grow as needed".
    auto const perShardDiskBytes =
        physicalShards > 0 ? effective.storageMaxDiskBytes / physicalShards : effective.storageMaxDiskBytes;
    StorageBackendBundle bundle;

    if (useShardingWrapper)
    {
        auto inners =
            BuildShardedInners(effective, usingPersistent, physicalShards, perShardBytes, perShardDiskBytes, logger);
        if (!inners.has_value())
            return std::unexpected(std::move(inners.error()));
        bundle.backend = std::make_unique<FastCache::ShardedStorage>(std::move(*inners));
        return bundle;
    }

    // Unwrapped single-shard reactor path.
    if (!usingPersistent)
    {
        bundle.backend = MakeL1(effective, effective.maxMemoryBytes);
        return bundle;
    }

    auto layered = BuildLayeredShard(effective.storagePath, effective, perShardBytes, perShardDiskBytes, logger);
    if (!layered.has_value())
        return std::unexpected(StorageOpenFailure("storage", effective.storagePath, effective, layered.error()));
    bundle.backend = std::move(*layered);
    return bundle;
}

/// The store files that actually exist under a configured `--storage` path.
///
/// Deliberately an enumeration rather than `StorageShardPaths`: that one answers
/// "which files will this configuration open", which depends on a shard count
/// the daemon derives from the CPU count when the operator did not name one. A
/// conversion has to cover every file the daemon could open on ANY future start,
/// and the set on disk is exactly that.
/// @param storagePath The configured `--storage` path.
/// @return The store files, or why there are none to convert.
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string> ExistingStorePaths(
    std::string const& storagePath)
{
    std::filesystem::path const configured { storagePath };
    std::error_code ec;

    if (std::filesystem::is_regular_file(configured, ec))
        return std::vector<std::filesystem::path> { configured };
    if (!std::filesystem::is_directory(configured, ec))
        return std::unexpected(
            std::format("no store at '{}': it is neither a store file nor a directory of shards", configured.string()));

    // Incremented explicitly with an error_code. A range-for over
    // `directory_iterator` advances through the THROWING `operator++`, so an
    // entry vanishing mid-scan or a subdirectory that cannot be read would
    // terminate the process instead of reporting "cannot list".
    std::vector<std::filesystem::path> shards;
    auto entry = std::filesystem::directory_iterator { configured, ec };
    auto const end = std::filesystem::directory_iterator {};
    for (; !ec && entry != end; entry.increment(ec))
    {
        auto const name = entry->path().filename().string();
        if (name.starts_with("shard-") && name.ends_with(".cow"))
            shards.push_back(entry->path());
    }
    if (ec)
        return std::unexpected(std::format("cannot list '{}': {}", configured.string(), ec.message()));
    if (shards.empty())
        return std::unexpected(std::format("no shard-NN.cow files under '{}'", configured.string()));

    // Sorted so the report reads in shard order rather than in whatever order
    // the filesystem hands them back.
    std::ranges::sort(shards);
    return shards;
}

/// Convert every store the configuration names to this build's record layout.
///
/// Acts on the EFFECTIVE configuration rather than on a path of its own, so it
/// converts exactly the files `BuildShardedInners` would open -- including the
/// directory-of-shards shape, which a `--migrate-storage=<path>` taking one
/// path could not express without the operator knowing the naming scheme.
///
/// Reports per shard rather than in total. An operator whose fleet has one bad
/// shard needs to know which one, and a summary line saying "4 stores, 0
/// converted" hides the case where three were converted and the fourth was
/// refused.
/// @param effective The merged configuration.
/// @return Process exit code.
[[nodiscard]] int MigrateConfiguredStorage(FastCache::Config const& effective)
{
    if (effective.storagePath.empty())
    {
        std::println(std::cerr,
                     "fastcached: --migrate-storage needs --storage: there is no on-disk store to convert "
                     "in a memory-only configuration");
        return EXIT_FAILURE;
    }

    // What is ON DISK, not what the shard formula predicts. With no explicit
    // --storage-shards the daemon derives the count from the CPU count, so a
    // conversion run inside a CPU-limited container -- or after the machine was
    // resized -- would walk a different set than the daemon opens, exit zero,
    // and leave the daemon still refusing to start on the shard it missed. That
    // is precisely the failure this command exists to end, so it enumerates
    // rather than predicts: every `shard-NN.cow` present is converted, whatever
    // count the daemon resolves to next time.
    auto const paths = ExistingStorePaths(effective.storagePath);
    if (!paths.has_value())
    {
        std::println(std::cerr, "fastcached: {}", paths.error());
        return EXIT_FAILURE;
    }

    auto failures = 0;
    for (auto const& path: *paths)
    {
        FastCache::CowTreeStorage::Options opts;
        opts.path = path;
        opts.maxValueBytes = effective.storageMaxValueBytes;

        auto const report = FastCache::CowTreeStorage::Migrate(opts);
        auto const line = FastCache::DescribeMigration(path, report);
        if (!report.has_value())
        {
            std::println(std::cerr, "fastcached: {}", line);
            ++failures;
            continue;
        }
        std::println("fastcached: {}", line);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/// Daemon body: holds the actual server lifecycle. Runs under whatever
/// IDaemonHost was selected (Foreground / Posix double-fork / Windows
/// service).
int DaemonBody(FastCache::Config const& effective, std::span<FastCache::RejectedCandidate const> rejected)
{
    // A service gets the event log, everything else gets stderr. The factory answers
    // nullptr wherever there is no event log, which is how this stays one expression
    // rather than a platform branch — and `effective.daemon` rather than "am I on
    // Windows", so a foreground run keeps its terminal output on a machine that has
    // one (#179).
    FastCache::ConsoleLogger consoleLogger {
        std::cerr, effective.logLevel, effective.logTimestamps ? FastCache::LogTimestamps::Yes : FastCache::LogTimestamps::No
    };
    auto const eventLogger =
        effective.daemon ? FastCache::MakeWindowsEventLogger(effective.serviceName, effective.logLevel) : nullptr;
    FastCache::ILogger& logger =
        eventLogger ? static_cast<FastCache::ILogger&>(*eventLogger) : static_cast<FastCache::ILogger&>(consoleLogger);

    // Through the logger, not just the stderr line main() already printed: a
    // service started by the SCM has no console for that to land on, and this
    // is exactly the deployment where a rejected machine-wide config would
    // otherwise be invisible — the daemon serving on built-in defaults while an
    // operator edits a file it has declined to read.
    for (auto const& [path, reason]: rejected)
        logger.Logf(FastCache::LogLevel::Error, "{}: {}", path.string(), reason);
    FastCache::ConfigReloader reloader { effective, effective.configPath };
    FastCache::SteadyClock steadyClock;
    // The engine reads the clock once per command, and on Windows that is a
    // QueryPerformanceCounter — ~16 ns, which measured as roughly a third of the
    // cost of serving a cached GET. Serving a value sampled once per reactor
    // iteration removes it from the per-command path entirely. The same object
    // goes to the server loop below (`serverOpts.clock`), because only whoever
    // owns the event loop can refresh it.
    FastCache::CachedClock clock { steadyClock };

    auto const usingPersistent = !effective.storagePath.empty();

    // The server scales across cores by running N independent single-threaded
    // reactors (--threads, default: hardware concurrency). Each connection is
    // pinned to one reactor for its lifetime.
    auto const hardwareThreads = std::max(1U, std::thread::hardware_concurrency());
    auto const reactorCount =
        effective.workerThreads != 0 ? static_cast<unsigned>(effective.workerThreads) : hardwareThreads;

    auto physicalShards = ResolvePhysicalShards(effective.storageShards, usingPersistent, effective.storagePath);
    // Several reactor threads share one storage, so keep at least as many
    // in-memory shards as reactors to hold per-shard lock contention down.
    if (!usingPersistent && reactorCount > 1)
        physicalShards = std::max<std::size_t>(physicalShards, reactorCount);

    // Wrap the backend in a ShardedStorage (its per-shard mutex serialises
    // access) whenever more than one thread can reach it: an explicit
    // multi-shard layout, the persistent backend, the reactor running on more
    // than one thread, or the metrics endpoint (its fc-admin thread calls
    // engine.Snapshot() concurrently with the reactor, and InMemoryLruStorage::
    // Snapshot is only safe under the shard's exclusive lock). A lone reactor
    // over in-memory storage with no metrics stays unwrapped — one thread owns
    // it, so the wrapper would be pure overhead.
    auto const useShardingWrapper = physicalShards > 1 || usingPersistent || reactorCount > 1 || effective.metricsEnabled;

    // Daemon-lifetime sinks: WATCH registry, pubsub, and keyspace notifier.
    //
    // Two ordering constraints meet here. They are created BEFORE
    // `reloaderThread` (declared further below) so LIFO stack unwind destroys
    // the thread before any object its subscribers capture by reference. And
    // they are created before the STORAGE CHAIN, because the reclaim log below
    // is wired into the tiers inside it — a log declared after the backend
    // would be destroyed while those tiers still held a pointer to it.
    FastCache::PubSubRegistry pubsub;
    FastCache::StreamWaiterRegistry streamWaiters;
    FastCache::WatchRegistry watches;
    auto const eventsMask = FastCache::ParseKeyspaceEvents(effective.notifyKeyspaceEvents);
    if (!eventsMask.has_value())
    {
        logger.Logf(FastCache::LogLevel::Fatal,
                    "fastcached: invalid --notify-keyspace-events '{}': {}",
                    effective.notifyKeyspaceEvents,
                    eventsMask.error().context);
        return EXIT_FAILURE;
    }
    FastCache::KeyspaceNotifier keyspaceNotifier { &pubsub, *eventsMask };

    // Connection-level metrics sink, shared by the server loop and the admin
    // HTTP endpoint. Wiring it into RunReactorServer is what actually collects
    // the connection counters; command/capacity stats come from the engine.
    // Declared here rather than beside the engine because the reclaim log below
    // reports its dropped notifications into it, and the log has to outlive the
    // storage chain.
    FastCache::AtomicMetricsSink metrics;

    // The observer every storage mutation reaches, and the log the tiers report
    // their own reclaims into. Both sit above the storage chain in declaration
    // order and therefore outlive it.
    FastCache::RedisMutationObserver mutationObserver { &watches, &keyspaceNotifier };
    FastCache::ReclaimLog reclaimLog { &mutationObserver, &metrics };

    auto bundle = BuildStorageBackend(effective, usingPersistent, useShardingWrapper, physicalShards, logger);
    if (!bundle.has_value())
    {
        logger.Logf(FastCache::LogLevel::Fatal, "{}", bundle.error());
        return EXIT_FAILURE;
    }
    auto backend = std::move(bundle->backend);

    // Always surface value-write failures. A write that cannot be persisted
    // (full disk / I/O error / corruption / read-only) is otherwise invisible
    // at the default log level: it never reaches the trace-only TracingStorage,
    // so a store silently does not happen. This decorator logs each such failure
    // at Warn and counts it into Snapshot().writeErrors (Prometheus
    // `fastcached_write_errors_total`). It wraps the raw backend so it observes
    // the true storage result (e.g. the disk error LayeredStorage propagates).
    FastCache::WriteErrorReportingStorage writeErrorReporter { *backend, logger };
    FastCache::IStorage* storagePtr = &writeErrorReporter;

    // Optionally wrap in TracingStorage when trace logging is requested.
    std::unique_ptr<FastCache::TracingStorage> tracer;
    if (effective.logLevel <= FastCache::LogLevel::Trace)
    {
        tracer = std::make_unique<FastCache::TracingStorage>(*storagePtr, logger, clock);
        storagePtr = tracer.get();
    }

    // Storage-layer fan-out: NotifyingStorage wraps the inner chain with the
    // RedisMutationObserver declared above, which fires WatchRegistry::Touched
    // on every successful mutation. This closes two cross-protocol bugs:
    //   * memcached writes never called Touched, so a Redis WATCH on a
    //     key mutated by a memcached client silently passed EXEC;
    //   * FLUSHDB had no per-key fan-out, leaving every WATCH'd key
    //     undirty after a database wipe.
    // Double-firing WATCH dirties is harmless because MarkDirty is idempotent,
    // so the per-verb Touched calls in the Redis handlers can coexist with it.
    //
    // Per-verb keyspace events also stay where they are — the observer
    // publishes only `expired` and `evicted`, which no handler is in a
    // position to fire because no verb is executing when they happen.
    FastCache::NotifyingStorage notifyingStorage { *storagePtr, &mutationObserver };
    // One call wires both halves of the reclaim path: the tiers below record
    // into the log, this decorator drains it once their call has returned and
    // every storage lock is released.
    notifyingStorage.SetReclaimLog(&reclaimLog);
    storagePtr = &notifyingStorage;

    // The sink is handed to the engine, not only to the server loop: a value that
    // will not decode is known at THIS layer and nowhere else -- the storage below
    // has already verified what it returned, and the protocol above has no single
    // seam where a storage error becomes a reply. Without it
    // `fastcache_cache_malformed_values_total` would stay at zero on the one process
    // that can observe the event (#296).
    FastCache::CacheEngine engine { *storagePtr, clock, FastCache::DefaultSystemWallClock(), &metrics };

    auto const durabilityName = [&] {
        switch (effective.storageDurability)
        {
            case FastCache::StorageDurability::Fsync:
                return std::string_view { "fsync" };
            case FastCache::StorageDurability::Batched:
                return std::string_view { "batched" };
            case FastCache::StorageDurability::None:
                return std::string_view { "none" };
        }
        return std::string_view { "?" };
    }();
    // Authentication policy: built once, shared read-only across connections.
    // Never log the secret itself — only whether auth is on. Wrapped in a
    // SharedAuthSource so a SIGHUP-driven reload can swap in a freshly-built
    // policy without restarting connections; in-flight verifies finish on the
    // policy they captured.
    auto const makePolicy = [](FastCache::Config const& c) -> std::shared_ptr<FastCache::AuthPolicy const> {
        if (c.requirePass.empty())
            return {};
        return std::make_shared<FastCache::AuthPolicy const>(c.authUsername, c.requirePass);
    };
    FastCache::SharedAuthSource authSource { makePolicy(effective) };

    reloader.Subscribe([&logger, &notifyingStorage, &authSource, &makePolicy](auto const& /*prev*/, auto const& next) {
        logger.SetMinLevel(next->logLevel);
        // Through the top of the chain, not straight at the backend. Shrinking
        // the budget evicts until it fits, which on a large cache is a great
        // many keys at once; entering below NotifyingStorage would leave every
        // one of them recorded in the reclaim log with nothing to drain it,
        // dropping all but the first bound-many and holding the rest until
        // some later client call — never, on an idle daemon.
        notifyingStorage.Resize(next->maxMemoryBytes);
        // Rotate the shared secret: building a fresh AuthPolicy and atomically
        // swapping it in lets a SIGHUP'd operator update requirepass without
        // restarting the daemon. In-flight verifies finish against the policy
        // they captured (kept alive by the returned shared_ptr).
        authSource.Store(makePolicy(*next));
    });

    // TLS context: built once and shared read-only across connections. Fails
    // fast on a missing build feature or unreadable cert/key.
#if defined(FC_TLS_ENABLED)
    std::unique_ptr<FastCache::TlsContext> tlsContext;
#endif
    auto const anyTlsBind = std::ranges::any_of(effective.binds, [](auto const& b) { return b.tls; });
    if (effective.tlsEnabled || anyTlsBind)
    {
#if defined(FC_TLS_ENABLED)
        if (effective.tlsCertPath.empty() || effective.tlsKeyPath.empty())
        {
            logger.Log(FastCache::LogLevel::Fatal, "fastcached: --tls requires both --tls-cert and --tls-key");
            return EXIT_FAILURE;
        }
        auto created = FastCache::TlsContext::Create(effective.tlsCertPath, effective.tlsKeyPath);
        if (!created.has_value())
        {
            logger.Logf(FastCache::LogLevel::Fatal, "fastcached: TLS init failed: {}", created.error().ToString());
            return EXIT_FAILURE;
        }
        tlsContext = std::move(*created);
#else
        logger.Log(FastCache::LogLevel::Fatal,
                   "fastcached: --tls requested but this build has no TLS support "
                   "(rebuild with -DFASTCACHED_ENABLE_TLS=ON)");
        return EXIT_FAILURE;
#endif
    }

    auto const shardingMode = useShardingWrapper ? std::string_view { "" } : std::string_view { " (unwrapped)" };
    // The bind summary reflects what is actually being listened on. The
    // original banner formatted the legacy single-bind fields verbatim and
    // ignored `binds`, so a daemon brought up via `--listen` always logged
    // the defaults of the unused legacy fields rather than its real endpoints.
    // We build the binds-shaped list from `effective` here so the banner
    // is computable before the equivalent `serverOpts.binds` is populated
    // a few dozen lines below; the synthesis rule is the same (prefer the
    // explicit list, otherwise fold the legacy single-bind triplet into a
    // synthetic BindConfig).
    //
    // The fallback is a NAMED object and the choice is made between two spans,
    // not between two vectors. Spelling it as a conditional over the vectors
    // themselves gave the operator a prvalue common type, so the configured list
    // was COPIED wholesale — every bind, on every start — just to render one line,
    // and the discarded temporary is what GCC 16 reports as a dangling pointer
    // through the span this is about to be read as. An array rather than a vector
    // because the synthetic case is exactly one bind and needs no allocation.
    std::array<FastCache::BindConfig, 1> const legacyBind { FastCache::BindConfig {
        .address = effective.bindAddress, .port = effective.port, .tls = effective.tlsEnabled } };
    auto const bannerBinds = !effective.binds.empty() ? std::span<FastCache::BindConfig const> { effective.binds }
                                                      : std::span<FastCache::BindConfig const> { legacyBind };
    auto const bindSummary = FastCache::FormatBindSummary(bannerBinds);
    // `anyTlsBind` was computed up-top against `effective.binds`; under the
    // current ordering `serverOpts.binds` is either `effective.binds` (when
    // non-empty) or the synthesised legacy single-bind. Either way, a TLS
    // banner field that reports "off" while a TLS listener is up is the
    // operator-misleading bug — fold `anyTlsBind` into the TLS field below.
    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={} max-memory={} config={} storage={} "
                "durability={} compression={} max-value={} reactors={} shards={}{} auth={} tls={}",
                ProgramVersion,
                bindSummary,
                FastCache::FormatByteSize(effective.maxMemoryBytes),
                effective.configPath.empty() ? std::string_view { "<none>" } : std::string_view { effective.configPath },
                effective.storagePath.empty() ? std::string_view { "<in-memory>" }
                                              : std::string_view { effective.storagePath },
                durabilityName,
                FastCache::Compression::NameOf(effective.compression),
                FastCache::FormatByteSize(effective.storageMaxValueBytes),
                reactorCount,
                physicalShards,
                shardingMode,
                authSource.Current() ? std::string_view { "on" } : std::string_view { "off" },
                (effective.tlsEnabled || anyTlsBind) ? std::string_view { "on" } : std::string_view { "off" });

    InstallStopHandlers();
    // The "ready, accepting connections" line is emitted by the server loop
    // itself, only once its listener is actually bound and listening — see
    // RunReactorServer. Logging it here (before bind) would race a client
    // that connects on the strength of the message.

    // Shared daemon-lifetime objects MUST be declared BEFORE reloaderThread
    // below: the jthread joins in its destructor during stack unwind, which
    // happens in LIFO order. If pubsub / watches / keyspaceNotifier were
    // declared after the thread, a SIGHUP racing shutdown could invoke a
    // reload subscriber against a freshly-destroyed notifier (UAF). Putting
    // them ahead of the thread guarantees the thread joins BEFORE these
    // objects are torn down.
    FastCache::ReactorServerOptions serverOpts;
    // Hand the reactors the very clock the engine reads, so their per-iteration
    // refresh is what keeps it current.
    serverOpts.clock = &clock;
    // Listener endpoints: prefer the explicit list when given, otherwise
    // synthesise one from the legacy single-bind fields. This keeps the
    // common single-port case working without an explicit --listen, and
    // lets advanced operators bring up multiple binds (e.g. plaintext on a
    // private interface + TLS on the public one) with repeated --listen /
    // --listen-tls flags.
    if (!effective.binds.empty())
    {
        serverOpts.binds = effective.binds;
    }
    else
    {
        serverOpts.binds.push_back(
            FastCache::BindConfig { .address = effective.bindAddress, .port = effective.port, .tls = effective.tlsEnabled });
    }
    // (ValidateBindFlagShape ran in main() before the daemon host was
    // invoked, so the mixed-shape rejection has already happened.)
    // Reject duplicate {address, port} endpoints before we hit the kernel:
    // SO_REUSEPORT would let both bind succeed and silently split traffic
    // 50/50 between mismatched protocols (plaintext vs TLS).
    if (auto const v = FastCache::ValidateBinds(serverOpts.binds); !v.has_value())
    {
        logger.Logf(FastCache::LogLevel::Fatal, "fastcached: {}", v.error().context);
        return EXIT_FAILURE;
    }
    serverOpts.listenBacklog = effective.listenBacklog;
    // The active expiry cycle. Config carries milliseconds because that is what
    // survives a merge, a YAML round trip and a `--help` line; the reaper wants
    // a Duration. The two defaults are asserted equal rather than trusted:
    // `Config` cannot include `Cache/`, so the number is written in both places,
    // and a `--help` line advertising a default the daemon does not use is
    // exactly the kind of drift nothing else would catch.
    static_assert(
        FastCache::DefaultActiveExpiryIntervalMs
        == std::chrono::duration_cast<std::chrono::milliseconds>(FastCache::ExpiryReaperOptions {}.interval).count());
    static_assert(FastCache::DefaultActiveExpiryScanBudget == FastCache::ExpiryReaperOptions::DefaultScanBudget);
    serverOpts.expiry.interval = std::chrono::milliseconds { effective.activeExpiryIntervalMs };
    serverOpts.expiry.scanBudget = effective.activeExpiryScanBudget;
    // One reactor per core (each single-threaded, connections pinned). One
    // reactor = a single event loop; N reactors scale across cores without any
    // cross-thread coroutine migration.
    serverOpts.reactorThreads = reactorCount;
    // Pin reactors to cores when asked (PerCore) and there's more than one;
    // a lone reactor gains nothing from pinning.
    serverOpts.pinReactorsToCpu = effective.cpuAffinity == FastCache::CpuAffinity::PerCore && reactorCount > 1;
    // When --log-source is set, every connection prefixes its log lines with
    // the client IP.
    serverOpts.logSource = effective.logSource;
    // --log-everything widens the Trace command log to non-data commands too.
    // Carried on the session bundle that every connection's handlers receive.
    serverOpts.session.logEverything = effective.logEverything;
    serverOpts.session.authSource = &authSource;
    // pubsub / watches / keyspaceNotifier are declared further up, above the
    // storage chain, so the NotifyingStorage decorator can wire them into it
    // and so the reclaim log outlives the tiers that hold a pointer to it.
    // Just publish the pointers into the session here.
    serverOpts.session.pubsub = &pubsub;
    serverOpts.session.streamWaiters = &streamWaiters;
    serverOpts.session.watches = &watches;
    serverOpts.session.keyspaceNotifier = &keyspaceNotifier;

    // The RESP wire cap must admit the largest value the cache will store, so a
    // client can push a value up to --storage-max-value without the connection
    // being dropped mid-command. Keep the protocol default floor when the
    // configured value is smaller; --storage-max-value drives both limits.
    serverOpts.session.maxPayloadBytes = std::max(serverOpts.session.maxPayloadBytes, effective.storageMaxValueBytes);
    // Make the notifier reloadable: a SIGHUP that changes
    // notify-keyspace-events now updates the live bitmask on the
    // already-running notifier. Existing connections' cached
    // state->keyspaceEnabled stays as it was at connect time (that's
    // documented per-connection behaviour); NEW connections opened after
    // the reload read the updated bitmask immediately. A parse error in
    // the reloaded config is logged but the previous mask is kept — the
    // daemon never silently falls back to an empty mask.
    //
    // Subscribe BEFORE the reloaderThread is constructed: the thread's
    // destructor joins on unwind, and we must guarantee the subscriber
    // table is fully populated before the thread can dispatch a reload.
    reloader.Subscribe([&logger, &keyspaceNotifier](auto const& /*prev*/, auto const& next) {
        auto const newMask = FastCache::ParseKeyspaceEvents(next->notifyKeyspaceEvents);
        if (!newMask.has_value())
        {
            logger.Logf(FastCache::LogLevel::Error,
                        "config reload: invalid notify-keyspace-events '{}': {} (keeping previous mask)",
                        next->notifyKeyspaceEvents,
                        newMask.error().context);
            return;
        }
        keyspaceNotifier.SetClasses(*newMask);
    });

    // Reloader thread MUST be declared AFTER every object its subscribers
    // capture by reference (logger, keyspaceNotifier, ...). jthread joins
    // in its destructor; stack unwind destroys locals in reverse
    // declaration order, so the thread's destructor runs BEFORE the
    // captured objects' destructors. This prevents a SIGHUP racing
    // shutdown from invoking a subscriber against freed memory.
    std::atomic<bool> reloaderQuit { false };
    std::jthread reloaderThread { [&reloader, &logger, &reloaderQuit] {
        auto& controls = FastCache::DaemonControls::Instance();
        while (!reloaderQuit.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds { 250 });
            if (!controls.TakeReloadRequest())
                continue;
            auto const result = reloader.Reload();
            if (!result.has_value())
                logger.Logf(FastCache::LogLevel::Error, "config reload failed: {}", result.error().ToString());
            else
                logger.Log(FastCache::LogLevel::Info, "config reloaded");
        }
    } };
#if defined(FC_TLS_ENABLED)
    serverOpts.tlsContext = tlsContext.get(); // null unless --tls is active
#endif

    // Optional admin HTTP endpoint (/metrics, /healthz) on its own port and
    // thread. A blocking listener is plenty for scrape-rate traffic; SetTimeouts
    // below makes the accept loop poll so Shutdown() is observed even on POSIX
    // (where Close() does not unblock a parked accept()) and bounds a stalled
    // client's request read so it cannot wedge the single-threaded endpoint.
    std::unique_ptr<FastCache::BlockingListener> adminListener;
    std::unique_ptr<FastCache::AdminHttpServer> adminServer;
    std::jthread adminThread;
    if (effective.metricsEnabled)
    {
        adminListener = FastCache::BlockingListener::Bind(effective.metricsBindAddress, effective.metricsPort);
        if (!adminListener || !adminListener->IsBound())
        {
            logger.Logf(FastCache::LogLevel::Error,
                        "fastcached: cannot bind metrics endpoint {}:{} ({})",
                        effective.metricsBindAddress,
                        effective.metricsPort,
                        adminListener ? adminListener->BindError() : std::string_view { "null listener" });
        }
        else
        {
            // Both values belong to the endpoint rather than to this call site --
            // the worker serves the same server and had grown its own copy of the
            // pair, which is two places for one decision to drift.
            adminListener->SetTimeouts(FastCache::AdminHttpServer::AcceptPoll, FastCache::AdminHttpServer::RequestTimeout);

            // Uptime reads `steadyClock`, not the cached one the engine uses.
            // The cached clock only advances when a reactor completes a loop
            // iteration, so a daemon sitting idle would report a frozen uptime
            // until the next request arrived. This runs on the admin thread at
            // scrape rate, where a real clock read costs nothing worth saving.
            auto const adminStartedAt = steadyClock.Now();
            adminServer = std::make_unique<FastCache::AdminHttpServer>(
                *adminListener,
                metrics,
                [&engine, &steadyClock, adminStartedAt] {
                    return FastCache::MetricsSnapshot {
                        .storage = engine.Snapshot(),
                        // What the merged view above had to leave out. With
                        // `--storage` the backend is an in-memory tier over a CoW
                        // tree, and `LayeredStorage::Snapshot()` reports the
                        // tree's item count, bytes and budget alone -- so the RAM
                        // tier an operator sized with `--memory` has never
                        // appeared on this scrape at all.
                        .storageTiers = engine.SnapshotTiers(),
                        // Absent, and said out loud rather than left to the default:
                        // this is a cache, not a compile node, and cores it does not
                        // schedule against are noise on its scrape. Naming the field
                        // is also what keeps a field added to the middle of the
                        // struct from silently defaulting here.
                        .host = std::nullopt,
                        .uptime = FastCache::Uptime { std::chrono::duration_cast<std::chrono::seconds>(steadyClock.Now()
                                                                                                       - adminStartedAt) },
                    };
                },
                logger);
            adminThread = std::jthread { [&adminServer] {
                FC_THREAD_NAME("fc-admin");
                FastCache::SyncRun(adminServer->Run());
            } };
            logger.Logf(FastCache::LogLevel::Info,
                        "metrics endpoint on http://{}:{}/metrics (and /healthz)",
                        effective.metricsBindAddress,
                        effective.metricsPort);
        }
    }

    int const exitCode = FastCache::RunReactorServer(serverOpts, engine, logger, /*admission*/ nullptr, &metrics);

    if (adminServer)
        adminServer->Shutdown(); // unblocks the admin accept loop so adminThread joins
    reloaderQuit.store(true, std::memory_order_release);
    return exitCode;
}

} // namespace

int main(int argc, char const* const* argv)
{
    FC_THREAD_NAME("fastcached-main");
    std::span<char const* const> const args { argv + 1, argc > 0 ? static_cast<std::size_t>(argc - 1) : 0 };

    auto const parsed = FastCache::ParseCli(args);
    if (!parsed.has_value())
    {
        std::println(std::cerr, "fastcached: {}", parsed.error().ToString());
        std::println(std::cerr, "{}", FastCache::CliUsage());
        return EXIT_FAILURE;
    }

    switch (parsed->outcome)
    {
        case FastCache::CliOutcome::ShowVersion:
            std::println("fastcached {}", ProgramVersion);
            return EXIT_SUCCESS;
        case FastCache::CliOutcome::ShowHelp:
            std::print("{}",
                       FastCache::CliUsage(FastCache::StdoutSupportsColor() ? FastCache::UsageColor::Colored
                                                                            : FastCache::UsageColor::Plain));
            return EXIT_SUCCESS;
        case FastCache::CliOutcome::SeedConfig:
            return SeedDefaultConfig(parsed->seedConfigTemplate);
        case FastCache::CliOutcome::Run:
        case FastCache::CliOutcome::InstallService:
        case FastCache::CliOutcome::UninstallService:
        case FastCache::CliOutcome::HealthCheck:
        case FastCache::CliOutcome::MigrateStorage:
            // These all need the effective config assembled below; they branch
            // apart afterwards.
            break;
    }

    // The config file to read: the one the operator named, or else whichever
    // platform default is actually there (EffectiveConfigPath owns that rule and
    // the tests for it). Resolved into a LOCAL and never back into
    // parsed->config, which is what --install-service registers: a discovered
    // path baked into a service's launch arguments would outrank the file itself
    // forever, and InlineCredentialRejection would start naming a path nobody
    // typed.
    auto const lookup = FastCache::EffectiveConfigPath(
        parsed->config.configPath, FastCache::SystemConfigPathProbe {}, FastCache::DaemonApplicationName);
    auto const configPath = lookup.path.string();

    // A config that exists and is readable but was passed over anyway has to say
    // so: silence would mean an operator editing a file the daemon has quietly
    // decided not to obey, over a permission problem only they can fix. Printed
    // here for the commands that never reach the daemon body, and repeated
    // through the logger once there is one — a Windows service has no console,
    // so stderr alone would put the message nowhere in the one deployment where
    // a machine-wide config is the norm.
    for (auto const& [path, reason]: lookup.rejected)
        std::println(std::cerr, "fastcached: {}: {}", path.string(), reason);

    FastCache::Config effective;
    bool metricsPortYamlExplicit = false;
    // Aggregate "was the legacy single-bind triplet typed by the operator,
    // CLI or YAML?" so a downstream mix-with-`listeners:` check sees both
    // sources. Starts from the CLI explicit bits and ORs in YAML presence.
    auto bindShapeCli = *parsed;
    effective = parsed->config;
    if (!configPath.empty())
    {
        auto loaded = FastCache::ReadYamlConfigWithPresence(configPath);
        if (!loaded.has_value())
        {
            // A file that does not parse is fatal for the command that *serves*,
            // and for any file the operator named — but not for a discovered one
            // on the way to some other action. Refusing to uninstall a service
            // because the file at the default location has a typo blocks the very
            // recovery the operator reached for, and fails a container health
            // check whose daemon was started with explicit flags and is serving
            // perfectly well.
            // ...and for the conversion, which acts on the merged `storage_path`:
            // proceeding on a config this run could not read would convert a
            // different set of files than the daemon opens, or none at all, and
            // report success either way.
            if (!parsed->config.configPath.empty() || parsed->outcome == FastCache::CliOutcome::Run
                || parsed->outcome == FastCache::CliOutcome::MigrateStorage)
            {
                std::println(std::cerr, "fastcached: {}", loaded.error().ToString());
                return EXIT_FAILURE;
            }

            // Ignored, and `effective.configPath` deliberately left empty with
            // it: nothing later should re-read a file this run declined.
            std::println(std::cerr, "fastcached: ignoring {}: {}", configPath, loaded.error().ToString());
        }
        else
        {
            metricsPortYamlExplicit = loaded->metricsPortExplicit;
            bindShapeCli.bindAddressExplicit = bindShapeCli.bindAddressExplicit || loaded->bindAddressExplicit;
            bindShapeCli.portExplicit = bindShapeCli.portExplicit || loaded->portExplicit;
            bindShapeCli.tlsEnabledExplicit = bindShapeCli.tlsEnabledExplicit || loaded->tlsEnabledExplicit;
            effective = FastCache::Merge(std::move(loaded->config), *parsed);
            effective.configPath = configPath;
        }
    }

    // Environment fallback for the metrics port: honour FASTCACHED_METRICS_PORT
    // only when the port has NOT been set explicitly on the CLI *or* in the YAML
    // config. This makes the precedence CLI > config-file > env > default, so a
    // stray environment variable can never silently override a `metrics_port:`
    // the operator wrote in their config file — even when they wrote the
    // compiled-in default value (e.g. `metrics_port: 9259` is now distinguishable
    // from "the key was absent" thanks to the YAML presence bit). A container
    // that wants the env form simply omits the config/CLI value, letting both
    // the daemon and its `--healthcheck` probe agree on the port via a single
    // `-e FASTCACHED_METRICS_PORT=...`.
    if (!parsed->metricsPortExplicit && !metricsPortYamlExplicit)
        if (auto const envPort = MetricsPortFromEnv())
            effective.metricsPort = *envPort;

    // Converting the store acts on the files and exits; it never runs the daemon
    // body. BEFORE the serving-shape checks below, and after the merge because it
    // is the MERGED --storage it acts on: a store of the wrong vintage is what
    // stops the daemon starting, so demanding the operator first fix an unrelated
    // `--listen`/`--notify-keyspace-events` typo would be refusing to let them
    // repair the thing that is actually broken. The node's `--migrate-cache`
    // sits early for the same reason.
    if (parsed->outcome == FastCache::CliOutcome::MigrateStorage)
        return MigrateConfiguredStorage(effective);

    // Reject shapes that would silently drop user-typed values: combining the
    // legacy single-bind triplet (`--bind / --port / --tls` OR YAML
    // `bind: / port: / tls:`) with `--listen / --listen-tls` (or YAML
    // `listeners:`) makes the legacy values vanish — DaemonBody picks
    // `binds` and discards the singletons. Validate BEFORE handing off to
    // the daemon host (which may fork) so the error reaches the operator.
    if (auto const shape = FastCache::ValidateBindFlagShape(bindShapeCli, effective.binds); !shape.has_value())
    {
        std::println(std::cerr, "fastcached: {}", shape.error().context);
        return EXIT_FAILURE;
    }

    // Validate notify-keyspace-events BEFORE handing off to the daemon
    // host (which may fork) so a typo reaches the operator's terminal,
    // not a syslog the child has already detached from. DaemonBody
    // re-parses the same value — parsing is pure and the error here was
    // caught well before any state was constructed.
    if (auto const eventsMask = FastCache::ParseKeyspaceEvents(effective.notifyKeyspaceEvents); !eventsMask.has_value())
    {
        std::println(std::cerr,
                     "fastcached: invalid --notify-keyspace-events '{}': {}",
                     effective.notifyKeyspaceEvents,
                     eventsMask.error().context);
        return EXIT_FAILURE;
    }

    // Health check: probe the running daemon's /healthz on loopback and exit
    // 0/1. Loopback regardless of the configured metrics bind address, since the
    // probe runs inside the same host/container as the daemon.
    if (parsed->outcome == FastCache::CliOutcome::HealthCheck)
        return FastCache::HttpHealthProbe("127.0.0.1", effective.metricsPort, "/healthz") ? EXIT_SUCCESS : EXIT_FAILURE;

    // Service-control requests act on the service manager and exit; they never
    // run the daemon body.
    //
    // The *command-line* config is what gets registered, not the merged one:
    // the launch arguments should hold what the operator typed, plus --config,
    // and let the file govern everything else. Registering the merged config
    // instead froze every YAML value into the supervisor's argument list at
    // install time — and because a CLI value outranks YAML in Merge, later
    // edits to that same file then had no effect, silently, for exactly the
    // keys the operator had bothered to set. It also copied `requirepass:` out
    // of a mode-0600 config file into a world-readable service registration.
    // The YAML is still parsed above, so a broken config is still rejected here
    // rather than at first start.
    if (parsed->outcome == FastCache::CliOutcome::InstallService
        || parsed->outcome == FastCache::CliOutcome::UninstallService)
    {
        // The command-line config, never the merged one -- see the comment above.
        // Turned into a ServiceSpec here because that is the seam the platform
        // registration speaks: `fastcache-compile-node` builds its own from its
        // own configuration type rather than reaching for the daemon's.
        auto spec = FastCache::MakeDaemonServiceSpec(FastCache::CurrentExecutablePath(), parsed->config);

        // ...and here, deliberately, the MERGED one -- for a purpose the rule
        // above does not cover. Nothing from `effective` is registered: this adds
        // a path to hand over, never a flag to bake in, so the hazard that rule
        // exists for (a path in ProgramArguments outranking the very file it came
        // from, forever) cannot arise.
        //
        // It is needed because the daemon no longer runs as the machine's most
        // privileged account. `storage_path` is read from YAML at every start, so
        // the registration cannot see it -- and an upgrade that changed the
        // service's identity without handing over the cache directory LocalSystem
        // had created would leave the daemon unable to open its own storage, at
        // every start, on precisely the installs that bothered to configure
        // persistence.
        //
        // Only what is actually configured. A daemon with no `storage_path` is
        // memory-only and needs no directory at all, so granting a speculative
        // default would create one nothing ever uses.
        //
        // Whether the path may be CREATED is not decided here. `storage_path` is
        // allowed to name one CoW file, and the handover is what would `mkdir` over
        // it -- so the handover is where that is refused, once, for every producer
        // of an owned path rather than for this one.
        if (!effective.storagePath.empty()
            && !std::ranges::contains(spec.ownedPaths, std::filesystem::path { effective.storagePath }))
            spec.ownedPaths.emplace_back(effective.storagePath);
        auto const result = parsed->outcome == FastCache::CliOutcome::InstallService
                                ? FastCache::InstallService(spec, parsed->serviceScope)
                                : FastCache::UninstallService(spec, parsed->serviceScope);
        if (result.exitCode == 0)
            std::println("fastcached: {}", result.message);
        else
            std::println(std::cerr, "fastcached: {}", result.message);
        return result.exitCode;
    }

    std::unique_ptr<FastCache::IDaemonHost> host;
    if (effective.daemon)
    {
#if defined(_WIN32)
        host = FastCache::MakeWindowsServiceHost(effective.serviceName);
#else
        host = FastCache::MakePosixDaemonHost(effective.pidfile);
#endif
    }
    if (!host)
        host = std::make_unique<FastCache::ForegroundHost>();

    return host->Run([&effective, &lookup] { return DaemonBody(effective, lookup.rejected); });
}
