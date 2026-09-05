// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretExposureWatcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using FastCache::CliResult;
using FastCache::Config;
using FastCache::ConfigReloader;
using FastCache::WatchSecretExposure;

#if !defined(_WIN32)

TEST_CASE("SecretExposureWatcher: a reload reports a transition into exposure, once", "[config][secret][reload]")
{
    // **#753.** `requirepass` is `Reloadable::Yes`, so a configuration file can GAIN a
    // secret after startup -- and #384's check ran at startup only, which left that
    // path silent in exactly the way the ticket exists to close.
    //
    // Every case here drives the REAL `ConfigReloader` and a real file on disk,
    // because the two things that must be true are that the subscription is attached
    // at all and that what it asks reaches the filesystem. A fake reloader would
    // establish neither.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-reload" };

    auto const write = [&scratch](char const* stem, std::string_view yaml, ::mode_t mode) {
        scratch.Write(stem, yaml);
        auto const path = scratch / stem;
        REQUIRE(::chmod(path.c_str(), mode) == 0);
        return path;
    };

    auto const rewrite = [](std::filesystem::path const& path, std::string_view yaml) {
        std::ofstream out { path, std::ios::trunc };
        out << yaml;
    };

    static constexpr ::mode_t Private = S_IRUSR | S_IWUSR;
    static constexpr ::mode_t WorldReadable = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

    SECTION("a secret appearing in an always-loose file is reported")
    {
        // The ticket's headline, and the one a startup-only check cannot see: the file
        // was already world-readable and carried no secret, so the start had nothing
        // to say and was right to say nothing.
        auto const path = write("gains-secret.yaml", "log_level: info\n", WorldReadable);

        Config initial {};
        initial.configPath = path.string();
        ConfigReloader reloader { initial, path, {} };

        std::vector<std::string> said;
        WatchSecretExposure(reloader, CliResult {}, [&said](std::string_view w) { said.emplace_back(w); });
        REQUIRE(said.empty());

        rewrite(path, "log_level: info\nrequirepass: hunter2\n");
        REQUIRE(reloader.Reload().has_value());

        REQUIRE(said.size() == 1);
        CHECK(said.front().contains(path.string()));
        CHECK(said.front().contains("chmod o-r"));
        // It goes to journald or a Windows event record, which more accounts can read
        // than could read the file. A diagnostic quoting the value would be the
        // exposure it is reporting, published by the report.
        CHECK_FALSE(said.front().contains("hunter2"));
    }

    SECTION("a mode that loosens under an unchanged secret is reported")
    {
        // **The case a snapshot-diffing implementation cannot see.** Nothing in the
        // configuration moved -- the two snapshots the reloader publishes are equal
        // field for field -- and the file went from private to world-readable. Only
        // re-asking the filesystem answers this.
        auto const path = write("loosens.yaml", "requirepass: hunter2\n", Private);

        Config initial {};
        initial.requirePass = "hunter2";
        initial.configPath = path.string();
        ConfigReloader reloader { initial, path, {} };

        std::vector<std::string> said;
        WatchSecretExposure(reloader, CliResult {}, [&said](std::string_view w) { said.emplace_back(w); });
        REQUIRE(said.empty());

        REQUIRE(::chmod(path.c_str(), WorldReadable) == 0);
        REQUIRE(reloader.Reload().has_value());

        REQUIRE(said.size() == 1);
        CHECK(said.front().contains(path.string()));
    }

    SECTION("a standing exposure is said once, not at every reload")
    {
        // The other half, and the one that decides whether anybody reads the first
        // half: a warning repeated at every SIGHUP is the alarm-nobody-reads failure
        // arriving by a different route. Asserted on the COUNT, because both the
        // remembering and the repeating version warn at startup.
        auto const path = write("standing.yaml", "requirepass: hunter2\n", WorldReadable);

        Config initial {};
        initial.requirePass = "hunter2";
        initial.configPath = path.string();
        ConfigReloader reloader { initial, path, {} };

        std::vector<std::string> said;
        WatchSecretExposure(reloader, CliResult {}, [&said](std::string_view w) { said.emplace_back(w); });
        REQUIRE(said.size() == 1);

        REQUIRE(reloader.Reload().has_value());
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.size() == 1);
    }

    SECTION("a secret removed and put back is a fresh transition")
    {
        // The memory is what the LAST observation found, so a path that stopped being
        // a subject is forgotten. The operator's warning described a state that
        // stopped being true in between, and the return to exposure is new news.
        auto const path = write("returns.yaml", "requirepass: hunter2\n", WorldReadable);

        Config initial {};
        initial.requirePass = "hunter2";
        initial.configPath = path.string();
        ConfigReloader reloader { initial, path, {} };

        std::vector<std::string> said;
        WatchSecretExposure(reloader, CliResult {}, [&said](std::string_view w) { said.emplace_back(w); });
        REQUIRE(said.size() == 1);

        rewrite(path, "log_level: info\n");
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.size() == 1);

        rewrite(path, "requirepass: hunter2\n");
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.size() == 2);
    }

    SECTION("a restricted file stays silent across reloads")
    {
        // The control. Without it, "report a transition" and "report at every reload"
        // are told apart only by the counts above -- and a rule that warned about the
        // ORDINARY 0600 file would fire on nearly every deployment.
        auto const path = write("quiet.yaml", "requirepass: hunter2\n", Private);

        Config initial {};
        initial.requirePass = "hunter2";
        initial.configPath = path.string();
        ConfigReloader reloader { initial, path, {} };

        std::vector<std::string> said;
        WatchSecretExposure(reloader, CliResult {}, [&said](std::string_view w) { said.emplace_back(w); });
        REQUIRE(reloader.Reload().has_value());
        CHECK(said.empty());
    }
}

#endif

TEST_CASE("SecretExposureWatcher: the daemon attaches it, and that is asserted", "[config][secret][reload]")
{
    // **A watcher nothing constructs is the bug it was written to fix.** The rule is
    // in the wire-and-protocol rulebook under `PurgeExpired`, which was correct,
    // tested, and had no production caller at all -- so the wiring is asserted rather
    // than assumed. The cases above prove the mechanism; only this proves it runs.
    //
    // A source scan because `main.cpp` is in no test target. Comments are stripped
    // first: a COMMENT is not a call site, and this file's own subject is exactly the
    // kind of thing that gets discussed in a comment near where it is not called.
    std::ifstream source { std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcached" / "main.cpp" };
    REQUIRE(source.is_open());

    std::string code;
    for (std::string line; std::getline(source, line);)
    {
        auto const first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0)
            continue;
        code += line;
        code += '\n';
    }

    // The positive control, and it is the half that makes the scan mean anything: a
    // renamed file, a moved body or an empty read would otherwise report "not called"
    // for a daemon that calls it perfectly well. This anchor predates the change and
    // is the line the watcher must be attached beside.
    REQUIRE(code.contains("ConfigReloader reloader"));

    CHECK(code.contains("WatchSecretExposure("));
}
