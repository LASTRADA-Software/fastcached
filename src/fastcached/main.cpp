// SPDX-License-Identifier: Apache-2.0
//
// fastcached — Fast Cache Daemon entry point.
//
// Wiring: CLI -> optional YAML file -> ConfigReloader -> CacheEngine over
// the storage backend -> RunReactorServer, hosted by the requested
// IDaemonHost (foreground / POSIX daemon / Windows service).
// SIGINT/SIGTERM and SCM stop trigger graceful shutdown;
// SIGHUP and SCM PARAMCHANGE trigger config reload.

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/HealthProbe.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/ServiceControl.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <ranges>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace
{

constexpr std::string_view ProgramVersion = FastCache::VersionString;

/// Parse a 1..65535 TCP port from a NUL-terminated string, reusing the CLI's
/// `FastCache::ParsePort` so the environment fallback accepts exactly the same
/// syntax and range as the `--metrics-port` flag (single source of truth).
/// @param raw Candidate string (may be null/empty).
/// @return The port, or nullopt when null/empty/out-of-range/garbage.
[[nodiscard]] std::optional<std::uint16_t> ParsePortString(char const* raw) noexcept
{
    if (raw == nullptr || *raw == '\0')
        return std::nullopt;
    auto const parsed = FastCache::ParsePort(std::string_view { raw });
    if (!parsed.has_value())
        return std::nullopt;
    return *parsed;
}

/// Read the metrics port from the FASTCACHED_METRICS_PORT environment variable.
/// This lets a container's daemon CMD and its separate `--healthcheck` probe
/// agree on a custom port via a single `-e FASTCACHED_METRICS_PORT=...`, without
/// the static image HEALTHCHECK having to learn the daemon's runtime args.
/// @return The parsed port, or nullopt when unset/empty/invalid.
[[nodiscard]] std::optional<std::uint16_t> MetricsPortFromEnv()
{
#if defined(_WIN32)
    // Secure CRT getenv_s keeps the build warning-clean under /WX.
    std::size_t size = 0;
    if (::getenv_s(&size, nullptr, 0, "FASTCACHED_METRICS_PORT") != 0 || size == 0)
        return std::nullopt;
    std::string buffer(size, '\0');
    if (::getenv_s(&size, buffer.data(), buffer.size(), "FASTCACHED_METRICS_PORT") != 0)
        return std::nullopt;
    buffer.resize(size > 0 ? size - 1 : 0); // drop the trailing NUL getenv_s wrote
    return ParsePortString(buffer.c_str());
#else
    return ParsePortString(std::getenv("FASTCACHED_METRICS_PORT"));
#endif
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
///   parallelise, EXCEPT for paths that name a single file — an existing
///   regular file, or a not-yet-existing path with a file extension (e.g.
///   `cache.cow`) — which stay single-file for backward compatibility (the
///   path is used as one file, never `mkdir`-ed over). An existing directory
///   or a not-yet-existing extension-less path (a cache directory) fans out
///   into `shard-NN.cow` files.
[[nodiscard]] std::size_t ResolvePhysicalShards(std::size_t requested,
                                                bool usingPersistent,
                                                std::filesystem::path const& storagePath) noexcept
{
    if (requested != 0)
        return requested;
    if (!usingPersistent)
        return AutoShardCount();
    std::error_code ec;
    if (std::filesystem::is_directory(storagePath, ec))
        return AutoShardCount(); // existing directory of shards
    if (std::filesystem::exists(storagePath, ec))
        return 1; // existing regular file — keep single-file (compat)
    if (storagePath.has_extension())
        return 1;            // a new file-looking path (e.g. cache.cow) — single file
    return AutoShardCount(); // a new cache directory — fan out
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

/// Holds the assembled storage chain. The reload subscriber resizes it
/// through the virtual `IStorage::Resize`, so no typed observer pointers
/// are needed.
struct StorageBackendBundle
{
    std::unique_ptr<FastCache::IStorage> backend;
};

/// Open a CowTreeStorage at `path` and wrap it in a LayeredStorage(L1
/// InMemoryLruStorage, L2 CowTreeStorage). The L1 cache owns the
/// per-shard memory budget; the disk tier is unbounded for now.
[[nodiscard]] std::expected<std::unique_ptr<FastCache::LayeredStorage>, std::string> BuildLayeredShard(
    std::filesystem::path const& path, FastCache::Config const& effective, std::size_t perShardBytes)
{
    FastCache::CowTreeStorage::Options opts;
    opts.path = path;
    opts.maxBytes = 0; // L2 is unbounded; L1 holds the byte budget
    opts.durability = ToPageStoreDurability(effective.storageDurability);
    opts.maxValueBytes = effective.storageMaxValueBytes;
    auto opened = FastCache::CowTreeStorage::Open(opts);
    if (!opened.has_value())
        return std::unexpected(opened.error().ToString());
    auto l1 = std::make_unique<FastCache::InMemoryLruStorage>(
        perShardBytes, effective.storageMaxValueBytes, ToLruMode(effective.lruRecency));
    return std::make_unique<FastCache::LayeredStorage>(std::move(l1), std::move(*opened));
}

/// Construct the multi-shard inner storages for a ShardedStorage
/// wrapper. Handles three shapes: in-memory fan-out, single-file
/// persistent, and multi-file persistent (directory of shard-NN.cow).
[[nodiscard]] std::expected<std::vector<std::unique_ptr<FastCache::IStorage>>, std::string> BuildShardedInners(
    FastCache::Config const& effective, bool usingPersistent, std::size_t physicalShards, std::size_t perShardBytes)
{
    std::vector<std::unique_ptr<FastCache::IStorage>> inners;
    inners.reserve(physicalShards);

    if (!usingPersistent)
    {
        for (std::size_t i = 0; i < physicalShards; ++i)
            inners.emplace_back(std::make_unique<FastCache::InMemoryLruStorage>(
                perShardBytes, effective.storageMaxValueBytes, ToLruMode(effective.lruRecency)));
        return inners;
    }

    if (physicalShards == 1)
    {
        auto layered = BuildLayeredShard(effective.storagePath, effective, perShardBytes);
        if (!layered.has_value())
            return std::unexpected(std::format("failed to open storage '{}': {}", effective.storagePath, layered.error()));
        inners.push_back(std::move(*layered));
        return inners;
    }

    std::filesystem::path const dir { effective.storagePath };
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return std::unexpected(
            std::format("failed to create storage directory '{}': {}", effective.storagePath, ec.message()));

    for (std::size_t i = 0; i < physicalShards; ++i)
    {
        auto const path = dir / std::format("shard-{:02d}.cow", i);
        auto layered = BuildLayeredShard(path, effective, perShardBytes);
        if (!layered.has_value())
            return std::unexpected(std::format("failed to open shard '{}': {}", path.string(), layered.error()));
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
                                                                                   std::size_t physicalShards)
{
    auto const perShardBytes = physicalShards > 0 ? effective.maxMemoryBytes / physicalShards : effective.maxMemoryBytes;
    StorageBackendBundle bundle;

    if (useShardingWrapper)
    {
        auto inners = BuildShardedInners(effective, usingPersistent, physicalShards, perShardBytes);
        if (!inners.has_value())
            return std::unexpected(std::move(inners.error()));
        bundle.backend = std::make_unique<FastCache::ShardedStorage>(std::move(*inners));
        return bundle;
    }

    // Unwrapped single-shard reactor path.
    if (!usingPersistent)
    {
        bundle.backend = std::make_unique<FastCache::InMemoryLruStorage>(
            effective.maxMemoryBytes, effective.storageMaxValueBytes, ToLruMode(effective.lruRecency));
        return bundle;
    }

    auto layered = BuildLayeredShard(effective.storagePath, effective, perShardBytes);
    if (!layered.has_value())
        return std::unexpected(std::format("failed to open storage '{}': {}", effective.storagePath, layered.error()));
    bundle.backend = std::move(*layered);
    return bundle;
}

/// Daemon body: holds the actual server lifecycle. Runs under whatever
/// IDaemonHost was selected (Foreground / Posix double-fork / Windows
/// service).
int DaemonBody(FastCache::Config const& effective)
{
    FastCache::ConsoleLogger logger { std::cerr, effective.logLevel, effective.logTimestamps };
    FastCache::ConfigReloader reloader { effective, effective.configPath };
    FastCache::SteadyClock clock;

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

    auto bundle = BuildStorageBackend(effective, usingPersistent, useShardingWrapper, physicalShards);
    if (!bundle.has_value())
    {
        logger.Logf(FastCache::LogLevel::Fatal, "{}", bundle.error());
        return EXIT_FAILURE;
    }
    auto backend = std::move(bundle->backend);
    auto* const backendPtr = backend.get();

    // Optionally wrap in TracingStorage when trace logging is requested.
    std::unique_ptr<FastCache::TracingStorage> tracer;
    FastCache::IStorage* storagePtr = backend.get();
    if (effective.logLevel <= FastCache::LogLevel::Trace)
    {
        tracer = std::make_unique<FastCache::TracingStorage>(*backend, logger, clock);
        storagePtr = tracer.get();
    }

    FastCache::CacheEngine engine { *storagePtr, clock };

    // Connection-level metrics sink, shared by the server loop and the admin
    // HTTP endpoint. Wiring it into RunReactorServer is what actually collects
    // the connection counters; command/capacity stats come from the engine.
    FastCache::AtomicMetricsSink metrics;

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

    reloader.Subscribe([&logger, backendPtr, &authSource, &makePolicy](auto const& /*prev*/, auto const& next) {
        logger.SetMinLevel(next->logLevel);
        backendPtr->Resize(next->maxMemoryBytes);
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
    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={}:{} max-memory={} config={} storage={} "
                "durability={} max-value={} reactors={} shards={}{} auth={} tls={}",
                ProgramVersion,
                effective.bindAddress,
                effective.port,
                FastCache::FormatByteSize(effective.maxMemoryBytes),
                effective.configPath.empty() ? std::string_view { "<none>" } : std::string_view { effective.configPath },
                effective.storagePath.empty() ? std::string_view { "<in-memory>" }
                                              : std::string_view { effective.storagePath },
                durabilityName,
                FastCache::FormatByteSize(effective.storageMaxValueBytes),
                reactorCount,
                physicalShards,
                shardingMode,
                authSource.Current() ? std::string_view { "on" } : std::string_view { "off" },
                effective.tlsEnabled ? std::string_view { "on" } : std::string_view { "off" });

    InstallStopHandlers();
    // The "ready, accepting connections" line is emitted by the server loop
    // itself, only once its listener is actually bound and listening — see
    // RunReactorServer. Logging it here (before bind) would race a client
    // that connects on the strength of the message.

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

    FastCache::ReactorServerOptions serverOpts;
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
    // Reject duplicate {address, port} endpoints before we hit the kernel:
    // SO_REUSEPORT would let both bind succeed and silently split traffic
    // 50/50 between mismatched protocols (plaintext vs TLS).
    if (auto const v = FastCache::ValidateBinds(serverOpts.binds); !v.has_value())
    {
        logger.Logf(FastCache::LogLevel::Fatal, "fastcached: {}", v.error().context);
        return EXIT_FAILURE;
    }
    serverOpts.listenBacklog = effective.listenBacklog;
    // One reactor per core (each single-threaded, connections pinned). One
    // reactor = a single event loop; N reactors scale across cores without any
    // cross-thread coroutine migration.
    serverOpts.reactorThreads = reactorCount;
    // Pin reactors to cores when asked (PerCore) and there's more than one;
    // a lone reactor gains nothing from pinning.
    serverOpts.pinReactorsToCpu = effective.cpuAffinity == FastCache::CpuAffinity::PerCore && reactorCount > 1;
    serverOpts.session.authSource = &authSource;
    // Publish/subscribe registry: one instance shared read-mostly across every
    // connection (RESP PUBLISH/SUBSCRIBE). Lives for the whole server run.
    FastCache::PubSubRegistry pubsub;
    serverOpts.session.pubsub = &pubsub;
    // Redis transaction WATCH registry: one instance shared across every
    // connection so a write on connection A flips the dirty flag on every
    // WATCH snapshot taken by connection B. Lives for the whole server run.
    FastCache::WatchRegistry watches;
    serverOpts.session.watches = &watches;
    // Redis keyspace-notification publisher. Parses the operator-supplied
    // flag string and rejects unknown letters loudly. Defaults to off.
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
    serverOpts.session.keyspaceNotifier = &keyspaceNotifier;
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
            // Poll accept() every 500ms so Shutdown() is observed on POSIX, and
            // time out a stalled request read after 2s so one idle client cannot
            // wedge the single-threaded admin endpoint (slowloris).
            adminListener->SetTimeouts(std::chrono::milliseconds { 500 }, std::chrono::seconds { 2 });

            auto const adminStartedAt = clock.Now();
            adminServer = std::make_unique<FastCache::AdminHttpServer>(
                *adminListener,
                metrics,
                [&engine, &clock, adminStartedAt] {
                    return FastCache::MetricsSnapshot {
                        .storage = engine.Snapshot(),
                        .uptime = FastCache::Uptime { std::chrono::duration_cast<std::chrono::seconds>(clock.Now()
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
        case FastCache::CliOutcome::Run:
        case FastCache::CliOutcome::InstallService:
        case FastCache::CliOutcome::UninstallService:
        case FastCache::CliOutcome::HealthCheck:
            // These all need the effective config assembled below; they branch
            // apart afterwards.
            break;
    }

    FastCache::Config effective;
    bool metricsPortYamlExplicit = false;
    if (!parsed->config.configPath.empty())
    {
        auto loaded = FastCache::ReadYamlConfigWithPresence(parsed->config.configPath);
        if (!loaded.has_value())
        {
            std::println(std::cerr, "fastcached: {}", loaded.error().ToString());
            return EXIT_FAILURE;
        }
        metricsPortYamlExplicit = loaded->metricsPortExplicit;
        effective = FastCache::Merge(std::move(loaded->config), *parsed);
        effective.configPath = parsed->config.configPath;
    }
    else
    {
        effective = parsed->config;
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

    // Health check: probe the running daemon's /healthz on loopback and exit
    // 0/1. Loopback regardless of the configured metrics bind address, since the
    // probe runs inside the same host/container as the daemon.
    if (parsed->outcome == FastCache::CliOutcome::HealthCheck)
        return FastCache::HttpHealthProbe("127.0.0.1", effective.metricsPort, "/healthz") ? EXIT_SUCCESS : EXIT_FAILURE;

    // Service-control requests act on the SCM and exit; they never run the
    // daemon body. The effective config is reused so every flag passed
    // alongside --install-service is baked into the service command line.
    if (parsed->outcome == FastCache::CliOutcome::InstallService
        || parsed->outcome == FastCache::CliOutcome::UninstallService)
    {
        auto const result = parsed->outcome == FastCache::CliOutcome::InstallService
                                ? FastCache::InstallWindowsService(effective)
                                : FastCache::UninstallWindowsService(effective);
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

    return host->Run([&effective] { return DaemonBody(effective); });
}
