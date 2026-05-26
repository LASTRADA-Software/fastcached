// SPDX-License-Identifier: Apache-2.0
//
// fastcached — Fast Cache Daemon entry point.
//
// MVP wiring: CLI -> Config -> CacheEngine over InMemoryLruStorage ->
// BlockingListener -> RunBlockingServerLoop. Foreground mode only —
// daemon / Windows service / SIGHUP reload come in subsequent passes.

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Server/BlockingServerLoop.hpp>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <print>
#include <span>
#include <string_view>

namespace
{

constexpr std::string_view kProgramVersion = "0.0.1";

/// Process-wide stop flag. Set by the SIGINT/SIGTERM handler (POSIX) or by
/// SetConsoleCtrlHandler (Windows). The accept loop polls it between
/// accepts; the OS additionally interrupts the blocking accept() via
/// closesocket()/close() from the same path.
std::atomic<bool> g_shouldStop { false };
FastCache::BlockingListener* g_activeListener { nullptr };

extern "C" void HandleStopSignal(int /*signum*/)
{
    g_shouldStop.store(true, std::memory_order_release);
    // Unblock the accept() syscall by closing the listening socket.
    if (auto* const listener = g_activeListener)
        listener->Close();
}

void InstallStopHandlers()
{
    std::signal(SIGINT, &HandleStopSignal);
    std::signal(SIGTERM, &HandleStopSignal);
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
            std::println("fastcached {}", kProgramVersion);
            return EXIT_SUCCESS;
        case FastCache::CliOutcome::ShowHelp:
            std::print("{}", FastCache::CliUsage());
            return EXIT_SUCCESS;
        case FastCache::CliOutcome::Run:
            break;
    }

    auto const& cfg = parsed->config;

    FastCache::ConsoleLogger logger { std::cerr, cfg.logLevel };
    logger.Logf(FastCache::LogLevel::Info,
                "fastcached {} starting; bind={}:{} max-memory={} bytes",
                kProgramVersion,
                cfg.bindAddress,
                cfg.port,
                cfg.maxMemoryBytes);

    FastCache::SteadyClock clock;
    FastCache::InMemoryLruStorage storage { cfg.maxMemoryBytes };
    FastCache::CacheEngine engine { storage, clock };

    auto listener = FastCache::BlockingListener::Bind(cfg.bindAddress, cfg.port);
    if (!listener || !listener->IsBound())
    {
        logger.Logf(FastCache::LogLevel::Error,
                    "fastcached: cannot bind: {}",
                    listener ? listener->BindError() : std::string_view { "null listener" });
        return EXIT_FAILURE;
    }

    g_activeListener = listener.get();
    InstallStopHandlers();
    logger.Log(FastCache::LogLevel::Info, "ready, accepting connections");

    auto const served = FastCache::RunBlockingServerLoop(*listener, engine, logger, g_shouldStop);

    logger.Logf(FastCache::LogLevel::Info, "shutting down; served {} connection(s)", served);
    g_activeListener = nullptr;
    return EXIT_SUCCESS;
}
