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
#include <FastCache/Cache/InMemoryLruStorage.hpp>
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
/// the default. Same simple heuristic the MVP shipped with.
FastCache::Config Merge(FastCache::Config fileCfg, FastCache::Config const& cliCfg)
{
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
    return fileCfg;
}

/// Daemon body: holds the actual server lifecycle. Runs under whatever
/// IDaemonHost was selected (Foreground / Posix double-fork / Windows
/// service).
int DaemonBody(FastCache::Config const& effective)
{
    FastCache::ConsoleLogger logger { std::cerr, effective.logLevel };
    FastCache::ConfigReloader reloader { effective, effective.configPath };
    FastCache::SteadyClock clock;
    FastCache::InMemoryLruStorage storage { effective.maxMemoryBytes };
    FastCache::CacheEngine engine { storage, clock };

    reloader.Subscribe([&logger, &storage](auto const& /*prev*/, auto const& next) {
        logger.SetMinLevel(next->logLevel);
        storage.Resize(next->maxMemoryBytes);
    });

    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={}:{} max-memory={} bytes config={}",
                ProgramVersion,
                effective.bindAddress,
                effective.port,
                effective.maxMemoryBytes,
                effective.configPath.empty() ? std::string_view { "<none>" } : std::string_view { effective.configPath });

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

    FastCache::ReactorServerOptions serverOpts;
    serverOpts.bindAddress = effective.bindAddress;
    serverOpts.port = effective.port;
    auto const exitCode = FastCache::RunReactorServer(serverOpts, engine, logger);

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
        effective = Merge(std::move(*loaded), parsed->config);
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
