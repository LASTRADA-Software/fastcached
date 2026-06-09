// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigReloader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace
{

std::filesystem::path WriteYaml(std::string_view stem, std::string_view content)
{
    auto path = std::filesystem::temp_directory_path() / "fastcached-test";
    std::filesystem::create_directories(path);
    path /= std::string { stem } + ".yaml";
    std::ofstream out { path };
    out << content;
    return path;
}

} // namespace

TEST_CASE("ConfigReloader: Current() returns the initial snapshot", "[config][reload]")
{
    FastCache::Config initial { .maxMemoryBytes = 1024, .bindAddress = "127.0.0.1", .port = 11500 };
    FastCache::ConfigReloader reloader { initial, {} };
    auto const snapshot = reloader.Current();
    REQUIRE(snapshot->bindAddress == "127.0.0.1");
    REQUIRE(snapshot->port == 11500);
}

TEST_CASE("ConfigReloader::Reload picks up reloadable changes", "[config][reload]")
{
    auto const path = WriteYaml("reload",
                                "bind: 127.0.0.1\n"
                                "port: 11600\n"
                                "max_memory: 1024\n"
                                "log_level: info\n");
    FastCache::Config initial { .maxMemoryBytes = 1024,
                                .bindAddress = "127.0.0.1",
                                .configPath = path.string(),
                                .port = 11600,
                                .logLevel = FastCache::LogLevel::Info };
    FastCache::ConfigReloader reloader { initial, path };

    bool fired = false;
    FastCache::LogLevel newLevel { FastCache::LogLevel::Info };
    reloader.Subscribe([&](auto const& /*prev*/, auto const& next) {
        fired = true;
        newLevel = next->logLevel;
    });

    // Modify file: bump max_memory and switch log_level.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11600\nmax_memory: 4096\nlog_level: warn\n";
    }

    auto const result = reloader.Reload();
    REQUIRE(result.has_value());
    REQUIRE(fired);
    REQUIRE(newLevel == FastCache::LogLevel::Warn);
    REQUIRE(reloader.Current()->maxMemoryBytes == 4096);
}

TEST_CASE("ConfigReloader::Reload rejects changes to immutable fields", "[config][reload]")
{
    auto const path = WriteYaml("immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11700\n"
                                "max_memory: 1024\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024, .bindAddress = "127.0.0.1", .configPath = path.string(), .port = 11700
    };
    FastCache::ConfigReloader reloader { initial, path };

    // Try to change the port — should be rejected.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 22000\nmax_memory: 1024\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "port");
    // Live snapshot unchanged.
    REQUIRE(reloader.Current()->port == 11700);
}

TEST_CASE("ConfigReloader::Reload rejects changes to storage_path (regression for ValidateImmutable gap)",
          "[config][reload][regression]")
{
    // Regression for finding #13 — pre-fix, ValidateImmutable only
    // checked bindAddress and port. Changing storage_path, shards,
    // durability, threads, or storage_max_value via SIGHUP silently
    // swapped Config but left the running backend on the old settings.
    auto const path = WriteYaml("storage-path-immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11710\n"
                                "max_memory: 1024\n"
                                "storage_path: /tmp/a.cow\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .bindAddress = "127.0.0.1",
        .configPath = path.string(),
        .storagePath = "/tmp/a.cow",
        .port = 11710,
    };
    FastCache::ConfigReloader reloader { initial, path };

    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11710\nmax_memory: 1024\nstorage_path: /tmp/b.cow\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "storage");
    // Live snapshot still names a.cow.
    REQUIRE(reloader.Current()->storagePath == "/tmp/a.cow");
}

TEST_CASE("ConfigReloader::Reload rejects changes to storage_durability", "[config][reload][regression]")
{
    auto const path = WriteYaml("storage-durability-immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11720\n"
                                "max_memory: 1024\n"
                                "storage_durability: fsync\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .bindAddress = "127.0.0.1",
        .configPath = path.string(),
        .port = 11720,
        .storageDurability = FastCache::StorageDurability::Fsync,
    };
    FastCache::ConfigReloader reloader { initial, path };

    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11720\nmax_memory: 1024\nstorage_durability: batched\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "storage-durability");
}

TEST_CASE("ConfigReloader::Reload rejects changes to listeners (binds vector)", "[config][reload][regression]")
{
    // Finding #7: pre-fix, ValidateImmutable did not check the new
    // Config::binds vector. A SIGHUP that added/removed/swapped a
    // `listeners:` entry was silently accepted, but main.cpp builds the
    // listener pool from `serverOpts.binds` once at start, so the new
    // listeners never came up — split-brain between reloader.Current()
    // and the live wiring.
    auto const path = WriteYaml("listeners-immutable",
                                "listeners:\n"
                                "  - { address: 127.0.0.1, port: 11730 }\n"
                                "max_memory: 1024\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .configPath = path.string(),
        .binds = { FastCache::BindConfig { .address = "127.0.0.1", .port = 11730, .tls = false } },
    };
    FastCache::ConfigReloader reloader { initial, path };

    // Add a second listener via SIGHUP — must reject, not silently accept.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "listeners:\n"
               "  - { address: 127.0.0.1, port: 11730 }\n"
               "  - { address: 0.0.0.0, port: 11731 }\n"
               "max_memory: 1024\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "listeners");
    // Live snapshot still names the single original listener.
    REQUIRE(reloader.Current()->binds.size() == 1);
    REQUIRE(reloader.Current()->binds.front().port == 11730);
}

TEST_CASE("ConfigReloader: Reload with no config path returns FileNotFound", "[config][reload]")
{
    FastCache::ConfigReloader reloader { FastCache::Config {}, {} };
    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::FileNotFound);
}
