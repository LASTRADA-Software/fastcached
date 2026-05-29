// SPDX-License-Identifier: Apache-2.0
//
// fastcached — Fast Cache Daemon entry point.
//
// Wiring: CLI -> optional YAML file -> ConfigReloader -> CacheEngine over
// InMemoryLruStorage -> BlockingListener -> RunBlockingServerLoop, hosted
// by the requested IDaemonHost (foreground / POSIX daemon / Windows
// service). SIGINT/SIGTERM and SCM stop trigger graceful shutdown;
// SIGHUP and SCM PARAMCHANGE trigger config reload.

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CowTreeStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/LayeredStorage.hpp>
#include <FastCache/Cache/ShardedStorage.hpp>
#include <FastCache/Cache/TracingStorage.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Server/BlockingServerLoop.hpp>
#include <FastCache/Server/PooledServerLoop.hpp>
#include <FastCache/Server/ReactorServerLoop.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <span>
#include <string_view>
#include <thread>

namespace
{

constexpr std::string_view ProgramVersion = FastCache::VersionString;

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

/// Merge CLI flags into the YAML-loaded Config. A CLI value overrides
/// the file value when the corresponding flag was explicitly passed —
/// driven by the per-flag "explicit" booleans on `CliResult`, not by
/// value comparison against the default. The latter would silently
/// drop `--threads=0` / `--storage-shards=0` / `--storage-durability=batched`
/// / any other typed value that matches the field's default.
FastCache::Config Merge(FastCache::Config fileCfg, FastCache::CliResult const& cli)
{
    auto const& cliCfg = cli.config;
    if (cli.bindAddressExplicit)
        fileCfg.bindAddress = cliCfg.bindAddress;
    if (cli.portExplicit)
        fileCfg.port = cliCfg.port;
    if (cli.maxMemoryBytesExplicit)
        fileCfg.maxMemoryBytes = cliCfg.maxMemoryBytes;
    if (cli.logLevelExplicit)
        fileCfg.logLevel = cliCfg.logLevel;
    if (!cliCfg.configPath.empty())
        fileCfg.configPath = cliCfg.configPath;
    if (cliCfg.daemon)
        fileCfg.daemon = true;
    if (!cliCfg.pidfile.empty())
        fileCfg.pidfile = cliCfg.pidfile;
    if (cliCfg.serviceName != FastCache::Config {}.serviceName)
        fileCfg.serviceName = cliCfg.serviceName;
    if (cli.storagePathExplicit)
        fileCfg.storagePath = cliCfg.storagePath;
    if (cli.storageDurabilityExplicit)
        fileCfg.storageDurability = cliCfg.storageDurability;
    if (cli.storageMaxValueBytesExplicit)
        fileCfg.storageMaxValueBytes = cliCfg.storageMaxValueBytes;
    if (cli.executionModelExplicit)
        fileCfg.executionModel = cliCfg.executionModel;
    if (cli.workerThreadsExplicit)
        fileCfg.workerThreads = cliCfg.workerThreads;
    if (cli.storageShardsExplicit)
        fileCfg.storageShards = cliCfg.storageShards;
    return fileCfg;
}

/// Pick a default shard count when the user left it at 0 (auto):
/// min(16, hardware_concurrency), floor of 1.
[[nodiscard]] std::size_t AutoShardCount() noexcept
{
    auto const hw = std::thread::hardware_concurrency();
    auto const cap = std::min<unsigned>(hw == 0 ? 1U : hw, 16U);
    return static_cast<std::size_t>(cap);
}

/// Resolve the effective shard count.
///
/// - User-specified non-zero: honored verbatim.
/// - Auto (0) for in-memory: fan out (`AutoShardCount`) so threaded mode
///   gets parallelism without the user opting in.
/// - Auto (0) for persistent: defaults to **single-file mode** (1
///   shard). The README documents `--storage=<path>` as a single file;
///   inferring a multi-shard directory from `hardware_concurrency` would
///   silently `mkdir` over the user's intended file. If the path
///   already exists as a directory, treat that as the user explicitly
///   opting into fan-out and pick `AutoShardCount`.
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
        return AutoShardCount();
    return 1;
}

/// Resolve `ExecutionModel::Auto` to a concrete model based on the
/// active storage backend: reactor when the cache is in-memory (no
/// disk I/O to overlap with), threaded when CoW on-disk storage is
/// configured (per-shard locks and page-store fsyncs benefit from
/// parallel workers). Pass-through for an explicit Threaded/Reactor.
[[nodiscard]] FastCache::ExecutionModel ResolveExecutionModel(FastCache::ExecutionModel requested,
                                                              bool usingPersistentStorage) noexcept
{
    if (requested != FastCache::ExecutionModel::Auto)
        return requested;
    return usingPersistentStorage ? FastCache::ExecutionModel::Threaded : FastCache::ExecutionModel::Reactor;
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

/// Daemon body: holds the actual server lifecycle. Runs under whatever
/// IDaemonHost was selected (Foreground / Posix double-fork / Windows
/// service).
int DaemonBody(FastCache::Config const& effective)
{
    FastCache::ConsoleLogger logger { std::cerr, effective.logLevel };
    FastCache::ConfigReloader reloader { effective, effective.configPath };
    FastCache::SteadyClock clock;

    // ----- storage factory ---------------------------------------------------
    //
    // Physical shapes:
    //   - In-memory: N InMemoryLruStorage instances (one per shard).
    //   - Persistent: N CowTreeStorage instances on disk, each one fronted
    //     by an in-memory LRU cache via LayeredStorage. Reads hit RAM
    //     first; writes pass through to disk (canonical CAS) and mirror
    //     into the RAM cache. `--max-memory` is the L1 (in-memory cache)
    //     budget; the on-disk file grows to whatever the filesystem
    //     allows (no L2 byte cap for now).
    //
    // Concurrency wrapping (orthogonal to physical fan-out):
    //   - Threaded execution: always wrap in ShardedStorage (even at
    //     shards==1) so workers serialise via the per-shard mutex. The
    //     LayeredStorage (L1, L2) pair lives inside one shard, under one
    //     mutex — coherent atomicity.
    //   - Reactor execution: single-shard backends run unwrapped; the
    //     reactor is single-threaded so no wrapper is needed.

    auto const usingPersistent = !effective.storagePath.empty();
    auto const resolvedExecutionModel = ResolveExecutionModel(effective.executionModel, usingPersistent);
    auto const physicalShards = ResolvePhysicalShards(effective.storageShards, usingPersistent, effective.storagePath);
    auto const useShardingWrapper = physicalShards > 1 || resolvedExecutionModel == FastCache::ExecutionModel::Threaded;

    std::unique_ptr<FastCache::IStorage> backend;
    FastCache::InMemoryLruStorage* singleMemBackend = nullptr;
    FastCache::LayeredStorage* singleLayeredBackend = nullptr;
    FastCache::ShardedStorage* shardedBackend = nullptr;

    auto const perShardBytes = physicalShards > 0 ? effective.maxMemoryBytes / physicalShards : effective.maxMemoryBytes;

    /// Open a CowTreeStorage at `path` with the configured durability /
    /// max-value, and wrap it in a LayeredStorage(InMemoryLruStorage,
    /// CowTreeStorage). The L1 cache owns the per-shard memory budget;
    /// the disk tier is unbounded for now.
    auto buildLayeredShard =
        [&](std::filesystem::path const& path) -> std::expected<std::unique_ptr<FastCache::LayeredStorage>, std::string> {
        FastCache::CowTreeStorage::Options opts;
        opts.path = path;
        opts.maxBytes = 0; // L2 is unbounded; L1 holds the byte budget
        opts.durability = ToPageStoreDurability(effective.storageDurability);
        opts.maxValueBytes = effective.storageMaxValueBytes;
        auto opened = FastCache::CowTreeStorage::Open(opts);
        if (!opened.has_value())
            return std::unexpected(opened.error().ToString());
        auto l1 = std::make_unique<FastCache::InMemoryLruStorage>(perShardBytes);
        return std::make_unique<FastCache::LayeredStorage>(std::move(l1), std::move(*opened));
    };

    if (useShardingWrapper)
    {
        // Sharded path: build N physical inner storages, hand them to a
        // ShardedStorage wrapper.
        std::vector<std::unique_ptr<FastCache::IStorage>> inners;
        inners.reserve(physicalShards);

        if (!usingPersistent)
        {
            for (std::size_t i = 0; i < physicalShards; ++i)
                inners.emplace_back(std::make_unique<FastCache::InMemoryLruStorage>(perShardBytes));
        }
        else if (physicalShards == 1)
        {
            // Single-file persistent under threaded execution — still
            // wrap in a single-shard ShardedStorage so the per-shard
            // mutex serialises workers.
            auto layered = buildLayeredShard(effective.storagePath);
            if (!layered.has_value())
            {
                logger.Logf(
                    FastCache::LogLevel::Fatal, "failed to open storage '{}': {}", effective.storagePath, layered.error());
                return EXIT_FAILURE;
            }
            inners.push_back(std::move(*layered));
        }
        else
        {
            // Multi-shard persistent: storagePath is a directory of shard-NN.cow files.
            std::filesystem::path const dir { effective.storagePath };
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
            {
                logger.Logf(FastCache::LogLevel::Fatal,
                            "failed to create storage directory '{}': {}",
                            effective.storagePath,
                            ec.message());
                return EXIT_FAILURE;
            }
            for (std::size_t i = 0; i < physicalShards; ++i)
            {
                auto const path = dir / std::format("shard-{:02d}.cow", i);
                auto layered = buildLayeredShard(path);
                if (!layered.has_value())
                {
                    logger.Logf(FastCache::LogLevel::Fatal, "failed to open shard '{}': {}", path.string(), layered.error());
                    return EXIT_FAILURE;
                }
                inners.push_back(std::move(*layered));
            }
        }

        auto sharded = std::make_unique<FastCache::ShardedStorage>(std::move(inners));
        shardedBackend = sharded.get();
        backend = std::move(sharded);
    }
    else
    {
        // Unwrapped single-shard reactor path: build the storage with
        // its concrete type so the Resize subscriber can target it
        // without a downcast.
        if (!usingPersistent)
        {
            auto mem = std::make_unique<FastCache::InMemoryLruStorage>(effective.maxMemoryBytes);
            singleMemBackend = mem.get();
            backend = std::move(mem);
        }
        else
        {
            auto layered = buildLayeredShard(effective.storagePath);
            if (!layered.has_value())
            {
                logger.Logf(
                    FastCache::LogLevel::Fatal, "failed to open storage '{}': {}", effective.storagePath, layered.error());
                return EXIT_FAILURE;
            }
            singleLayeredBackend = layered->get();
            backend = std::move(*layered);
        }
    }

    // Optionally wrap in TracingStorage when trace logging is requested.
    std::unique_ptr<FastCache::TracingStorage> tracer;
    FastCache::IStorage* storagePtr = backend.get();
    if (effective.logLevel <= FastCache::LogLevel::Trace)
    {
        tracer = std::make_unique<FastCache::TracingStorage>(*backend, logger, clock);
        storagePtr = tracer.get();
    }

    FastCache::CacheEngine engine { *storagePtr, clock };

    reloader.Subscribe(
        [&logger, singleMemBackend, singleLayeredBackend, shardedBackend](auto const& /*prev*/, auto const& next) {
            logger.SetMinLevel(next->logLevel);
            if (singleMemBackend != nullptr)
                singleMemBackend->Resize(next->maxMemoryBytes);
            if (singleLayeredBackend != nullptr)
                singleLayeredBackend->Resize(next->maxMemoryBytes);
            if (shardedBackend != nullptr)
                shardedBackend->ResizeTotal(next->maxMemoryBytes);
        });

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
    auto const executionName = (resolvedExecutionModel == FastCache::ExecutionModel::Threaded)
                                   ? std::string_view { "threaded" }
                                   : std::string_view { "reactor" };
    // Only annotate the resolved model with "(auto)" when it was actually
    // resolved from Auto — there's nothing to disambiguate when the user
    // typed --execution-model=threaded.
    auto const executionAnnotation =
        (effective.executionModel == FastCache::ExecutionModel::Auto) ? std::string_view { " (auto)" } : std::string_view {};
    auto const shardingMode = useShardingWrapper ? std::string_view { "" } : std::string_view { " (unwrapped)" };
    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={}:{} max-memory={} config={} storage={} "
                "durability={} max-value={} execution={}{} shards={}{}",
                ProgramVersion,
                effective.bindAddress,
                effective.port,
                FastCache::FormatByteSize(effective.maxMemoryBytes),
                effective.configPath.empty() ? std::string_view { "<none>" } : std::string_view { effective.configPath },
                effective.storagePath.empty() ? std::string_view { "<in-memory>" }
                                              : std::string_view { effective.storagePath },
                durabilityName,
                FastCache::FormatByteSize(effective.storageMaxValueBytes),
                executionName,
                executionAnnotation,
                physicalShards,
                shardingMode);

    InstallStopHandlers();
    logger.Log(FastCache::LogLevel::Info, "ready, accepting connections");

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

    int exitCode = EXIT_SUCCESS;
    if (resolvedExecutionModel == FastCache::ExecutionModel::Reactor)
    {
        FastCache::ReactorServerOptions serverOpts;
        serverOpts.bindAddress = effective.bindAddress;
        serverOpts.port = effective.port;
        exitCode = FastCache::RunReactorServer(serverOpts, engine, logger);
    }
    else
    {
        // Threaded mode: BlockingListener + pooled accept loop.
        auto listener = FastCache::BlockingListener::Bind(effective.bindAddress, effective.port);
        if (!listener || !listener->IsBound())
        {
            logger.Logf(FastCache::LogLevel::Fatal,
                        "cannot bind: {}",
                        listener ? listener->BindError() : std::string_view { "null listener" });
            reloaderQuit.store(true, std::memory_order_release);
            return EXIT_FAILURE;
        }

        // Watchdog: poll DaemonControls for stop; close the listener so the
        // accept loop exits.
        std::atomic<bool> watchdogQuit { false };
        std::jthread watchdog { [&] {
            auto& controls = FastCache::DaemonControls::Instance();
            while (!watchdogQuit.load(std::memory_order_acquire))
            {
                if (controls.StopRequested())
                {
                    listener->Close();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
            }
        } };

        std::atomic<bool> stop { false };
        auto const accepted = FastCache::RunPooledServerLoop(*listener, engine, logger, stop, effective.workerThreads);
        watchdogQuit.store(true, std::memory_order_release);
        logger.Logf(FastCache::LogLevel::Info, "served {} connection(s)", accepted);
    }

    reloaderQuit.store(true, std::memory_order_release);
    return exitCode;
}

} // namespace

int main(int argc, char const* const* argv)
{
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
            std::print("{}", FastCache::CliUsage());
            return EXIT_SUCCESS;
        case FastCache::CliOutcome::Run:
            break;
    }

    FastCache::Config effective;
    if (!parsed->config.configPath.empty())
    {
        auto loaded = FastCache::ReadYamlConfig(parsed->config.configPath);
        if (!loaded.has_value())
        {
            std::println(std::cerr, "fastcached: {}", loaded.error().ToString());
            return EXIT_FAILURE;
        }
        effective = Merge(std::move(*loaded), *parsed);
        effective.configPath = parsed->config.configPath;
    }
    else
    {
        effective = parsed->config;
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
