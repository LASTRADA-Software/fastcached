// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/Config.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using FastCache::BuildServiceCommandLine;

TEST_CASE("ServiceControl: default config yields a minimal command line", "[platform][service]")
{
    FastCache::Config const cfg {};
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd == "\"fastcached\" --daemon --service-name=FastCached");
}

TEST_CASE("ServiceControl: the executable path is always quoted", "[platform][service]")
{
    FastCache::Config const cfg {};
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "C:/Program Files/fastcached.exe" }, cfg);
    REQUIRE(cmd.starts_with("\"C:/Program Files/fastcached.exe\" --daemon"));
}

TEST_CASE("ServiceControl: non-default scalar flags are baked in", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.port = 6000;
    cfg.bindAddress = "0.0.0.0";
    cfg.workerThreads = 8;
    cfg.maxMemoryBytes = 128U * 1024U * 1024U;
    cfg.storageShards = 4;
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--port=6000"));
    REQUIRE(cmd.contains("--bind=0.0.0.0"));
    REQUIRE(cmd.contains("--threads=8"));
    REQUIRE(cmd.contains("--max-memory=134217728"));
    REQUIRE(cmd.contains("--storage-shards=4"));
}

TEST_CASE("ServiceControl: flags left at their default are omitted", "[platform][service]")
{
    FastCache::Config const cfg {};
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(!cmd.contains("--port="));
    REQUIRE(!cmd.contains("--bind="));
    REQUIRE(!cmd.contains("--max-memory="));
    REQUIRE(!cmd.contains("--threads="));
    REQUIRE(!cmd.contains("--log-level="));
    REQUIRE(!cmd.contains("--storage="));
}

TEST_CASE("ServiceControl: enum flags use their CLI spellings", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.logLevel = FastCache::LogLevel::Debug;
    cfg.storageDurability = FastCache::StorageDurability::Fsync;
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--log-level=debug"));
    REQUIRE(cmd.contains("--storage-durability=fsync"));
}

TEST_CASE("ServiceControl: the service name is always emitted, quoted when it has spaces", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.serviceName = "My Cache";
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--service-name=\"My Cache\""));
}

TEST_CASE("ServiceControl: a relative storage path is absolutized", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.storagePath = "relative/cache.cow";
    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);

    auto const expected = std::filesystem::absolute("relative/cache.cow").string();
    REQUIRE(cmd.contains(expected));
    // The bare relative path must not survive — a service's working directory is
    // not the install directory, so it would resolve to the wrong location.
    REQUIRE(!cmd.contains("--storage=relative/cache.cow"));
}

TEST_CASE("ServiceControl: install/uninstall are unsupported off Windows", "[platform][service]")
{
#if !defined(_WIN32)
    FastCache::Config const cfg {};
    auto const installed = FastCache::InstallWindowsService(cfg);
    auto const removed = FastCache::UninstallWindowsService(cfg);
    REQUIRE(installed.exitCode != 0);
    REQUIRE(removed.exitCode != 0);
#else
    SUCCEED("InstallWindowsService is exercised by the Windows end-to-end path");
#endif
}
