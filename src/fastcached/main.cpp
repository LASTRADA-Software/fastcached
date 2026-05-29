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

/// Merge: CLI flags override YAML for any value explicitly changed from
/// the default. Same simple heuristic the MVP shipped with. The full
/// CliResult is passed (not just the Config) so flags whose default is
/// itself a meaningful user-visible value — like `--execution-model=auto`
/// — can override a non-default YAML value when the user typed them
/// explicitly.
FastCache::Config Merge(FastCache::Config fileCfg, FastCache::CliResult const& cli)
{
    auto const& cliCfg = cli.config;
    FastCache::Config defaults;
    if (cliCfg.bindAddress != defaults.bindAddress)
        fileCfg.bindAddress = cliCfg.bindAddress;
    if (cliCfg.port != defaults.port)
        fileCfg.port = cliCfg.port;
    if (cliCfg.maxMemoryBytes != defaults.maxMemoryBytes)
        fileCfg.maxMemoryBytes = cliCfg.maxMemoryBytes;
    if (cliCfg.logLevel != defaults.logLevel)
        fileCfg.logLevel = cliCfg.logLevel;
    if (!cliCfg.configPath.empty())
        fileCfg.configPath = cliCfg.configPath;
    if (cliCfg.daemon)
        fileCfg.daemon = true;
    if (!cliCfg.pidfile.empty())
        fileCfg.pidfile = cliCfg.pidfile;
    if (cliCfg.serviceName != defaults.serviceName)
        fileCfg.serviceName = cliCfg.serviceName;
    if (!cliCfg.storagePath.empty())
        fileCfg.storagePath = cliCfg.storagePath;
    if (cliCfg.storageDurability != defaults.storageDurability)
        fileCfg.storageDurability = cliCfg.storageDurability;
    if (cliCfg.storageMaxValueBytes != defaults.storageMaxValueBytes)
        fileCfg.storageMaxValueBytes = cliCfg.storageMaxValueBytes;
    if (cli.executionModelExplicit)
        fileCfg.executionModel = cliCfg.executionModel;
    if (cliCfg.workerThreads != defaults.workerThreads)
        fileCfg.workerThreads = cliCfg.workerThreads;
    if (cliCfg.storageShards != defaults.storageShards)
        fileCfg.storageShards = cliCfg.storageShards;
    return fileCfg;
}

/// Pick a default shard count when the user left it at 0 (auto):
/// min(16, hardware_concurrency), floor of 1.
[[nodiscard]] std::size_t ResolveShardCount(std::size_t requested) noexcept
{
    if (requested != 0)
        return requested;
    auto const hw = std::thread::hardware_concurrency();
    auto const cap = std::min<unsigned>(hw == 0 ? 1U : hw, 16U);
    return static_cast<std::size_t>(cap);
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
    // Possible shapes:
    //   - In-memory, single shard: one InMemoryLruStorage holds everything.
    //   - In-memory, N shards: N InMemoryLruStorage wrapped in a ShardedStorage.
    //   - Persistent, single shard: one CowTreeStorage at effective.storagePath
    //     (the file). Matches PR #10's contract.
    //   - Persistent, N shards: N CowTreeStorage instances at
    //     <storagePath>/shard-NN.cow inside the directory.
    //
    // The shard count is `--storage-shards` (0 = auto).

    auto const shardCount = ResolveShardCount(effective.storageShards);
    auto const usingPersistent = !effective.storagePath.empty();
    auto const useSharding = shardCount > 1;

    std::unique_ptr<FastCache::IStorage> backend;
    FastCache::InMemoryLruStorage* singleMemBackend = nullptr;
    FastCache::CowTreeStorage* singleDiskBackend = nullptr;
    FastCache::ShardedStorage* shardedBackend = nullptr;

    if (!useSharding)
    {
        // Single shard — preserves the pre-sharding behaviour.
        if (!usingPersistent)
        {
            auto mem = std::make_unique<FastCache::InMemoryLruStorage>(effective.maxMemoryBytes);
            singleMemBackend = mem.get();
            backend = std::move(mem);
        }
        else
        {
            FastCache::CowTreeStorage::Options opts;
            opts.path = effective.storagePath;
            opts.maxBytes = effective.maxMemoryBytes;
            opts.durability = ToPageStoreDurability(effective.storageDurability);
            opts.maxValueBytes = effective.storageMaxValueBytes;
            auto opened = FastCache::CowTreeStorage::Open(opts);
            if (!opened.has_value())
            {
                logger.Logf(FastCache::LogLevel::Fatal,
                            "failed to open storage '{}': {}",
                            effective.storagePath,
                            opened.error().ToString());
                return EXIT_FAILURE;
            }
            singleDiskBackend = opened->get();
            backend = std::move(*opened);
        }
    }
    else
    {
        // Sharded — split the memory budget evenly across shards.
        auto const perShardBytes = effective.maxMemoryBytes / shardCount;
        std::vector<std::unique_ptr<FastCache::IStorage>> shards;
        shards.reserve(shardCount);

        if (!usingPersistent)
        {
            for (std::size_t i = 0; i < shardCount; ++i)
                shards.emplace_back(std::make_unique<FastCache::InMemoryLruStorage>(perShardBytes));
        }
        else
        {
            // Treat the path as a directory; create it if missing.
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

            for (std::size_t i = 0; i < shardCount; ++i)
            {
                FastCache::CowTreeStorage::Options opts;
                opts.path = dir / std::format("shard-{:02d}.cow", i);
                opts.maxBytes = perShardBytes;
                opts.durability = ToPageStoreDurability(effective.storageDurability);
                opts.maxValueBytes = effective.storageMaxValueBytes;
                auto opened = FastCache::CowTreeStorage::Open(opts);
                if (!opened.has_value())
                {
                    logger.Logf(FastCache::LogLevel::Fatal,
                                "failed to open shard '{}': {}",
                                opts.path.string(),
                                opened.error().ToString());
                    return EXIT_FAILURE;
                }
                shards.push_back(std::move(*opened));
            }
        }

        auto sharded = std::make_unique<FastCache::ShardedStorage>(std::move(shards));
        shardedBackend = sharded.get();
        backend = std::move(sharded);
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
        [&logger, singleMemBackend, singleDiskBackend, shardedBackend](auto const& /*prev*/, auto const& next) {
            logger.SetMinLevel(next->logLevel);
            if (singleMemBackend != nullptr)
                singleMemBackend->Resize(next->maxMemoryBytes);
            if (singleDiskBackend != nullptr)
                singleDiskBackend->Resize(next->maxMemoryBytes);
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
    auto const resolvedExecutionModel = ResolveExecutionModel(effective.executionModel, usingPersistent);
    auto const executionName = (resolvedExecutionModel == FastCache::ExecutionModel::Threaded)
                                   ? std::string_view { "threaded" }
                                   : std::string_view { "reactor" };
    // Only annotate the resolved model with "(auto)" when it was actually
    // resolved from Auto — there's nothing to disambiguate when the user
    // typed --execution-model=threaded.
    auto const executionAnnotation = (effective.executionModel == FastCache::ExecutionModel::Auto)
                                         ? std::string_view { " (auto)" }
                                         : std::string_view {};
    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={}:{} max-memory={} config={} storage={} "
                "durability={} max-value={} execution={}{} shards={}",
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
                shardCount);

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
