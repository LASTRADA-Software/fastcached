// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretExposureWatcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using FastCache::Config;
using FastCache::ConfigReloader;
using FastCache::SecretExposure;
using FastCache::SecretExposureWatcher;
using FastCache::SecretFileFinding;
using FastCache::WatchSecretExposure;

TEST_CASE("SecretExposureWatcher: the memory rule, on every platform", "[config][secret][reload]")
{
    // **The rule that decides whether an operator is TOLD anything, tested where
    // `chmod` does not exist.** Every case below the POSIX guard needs a real file
    // mode, so without this the say-once, kind-change and forget rules would be
    // untested on Windows -- the platform whose acquisition half is a security
    // descriptor rather than three bits, and where a regression in the memory would
    // therefore be invisible. `Platform/FileTrust` draws the same split one level
    // down, for the same reason its comment gives: the branch a developer cannot run
    // is still exercised against a constructed input.
    std::filesystem::path const key { "/etc/fastcached/cluster.key" };
    std::filesystem::path const token { "/etc/fastcached/dashboard.token" };

    SecretExposureWatcher watcher;

    SECTION("a new finding is reported, and the same one again is not")
    {
        std::array<SecretFileFinding, 1> const found { SecretFileFinding { .path = key,
                                                                           .exposure = SecretExposure::AnyLocalAccount } };

        auto const first = watcher.Transitions(found);
        REQUIRE(first.size() == 1);
        CHECK(first.front().contains(key.string()));

        CHECK(watcher.Transitions(found).empty());
    }

    SECTION("the same path at a different exposure is a fresh transition")
    {
        // The remedy differs between the two, so the sentence the operator is holding
        // has stopped describing the file. This is what makes the memory
        // `(path, exposure)` rather than `path`.
        std::array<SecretFileFinding, 1> const narrow { SecretFileFinding { .path = key,
                                                                            .exposure = SecretExposure::OwnersOwnGroup } };
        std::array<SecretFileFinding, 1> const wide { SecretFileFinding { .path = key,
                                                                          .exposure = SecretExposure::AnyLocalAccount } };

        REQUIRE(watcher.Transitions(narrow).size() == 1);
        CHECK(watcher.Transitions(wide).size() == 1);
    }

    SECTION("a path that leaves the set is forgotten")
    {
        std::array<SecretFileFinding, 1> const found { SecretFileFinding { .path = key,
                                                                           .exposure = SecretExposure::AnyLocalAccount } };

        REQUIRE(watcher.Transitions(found).size() == 1);
        CHECK(watcher.Transitions({}).empty());
        CHECK(watcher.Transitions(found).size() == 1);
    }

    SECTION("several files are reported independently, in the caller's order")
    {
        std::array<SecretFileFinding, 2> const both {
            SecretFileFinding { .path = key, .exposure = SecretExposure::AnyLocalAccount },
            SecretFileFinding { .path = token, .exposure = SecretExposure::AnyLocalAccount },
        };

        auto const first = watcher.Transitions(both);
        REQUIRE(first.size() == 2);
        CHECK(first[0].contains(key.string()));
        CHECK(first[1].contains(token.string()));

        // One of the two fixed: the other is still standing and still silent.
        std::array<SecretFileFinding, 1> const remaining { SecretFileFinding {
            .path = token, .exposure = SecretExposure::AnyLocalAccount } };
        CHECK(watcher.Transitions(remaining).empty());
    }

    SECTION("Undetermined is a transition like any other")
    {
        // A platform that would not answer is not one that answered "safe", so the
        // memory has to hold that state too rather than treating it as absence.
        std::array<SecretFileFinding, 1> const unknown { SecretFileFinding { .path = key,
                                                                             .exposure = SecretExposure::Undetermined } };

        REQUIRE(watcher.Transitions(unknown).size() == 1);
        CHECK(watcher.Transitions(unknown).empty());
    }
}

#if !defined(_WIN32)

namespace
{

/// The configuration a start would have assembled from @p path.
/// @param path The file that was read.
/// @param secret What `requirepass:` in it says, or empty for a file carrying none.
/// @return The snapshot a `ConfigReloader` is seeded with.
Config InitialConfig(std::filesystem::path const& path, std::string_view secret)
{
    Config initial {};
    initial.requirePass = std::string { secret };
    initial.configPath = path.string();
    return initial;
}

/// A watched configuration file: the sink, the reloader, and the subscription.
///
/// **The member order IS the rule.** `said` is declared before `reloader`, so it is
/// destroyed after it -- the report closure lives in the reloader's subscriber list and
/// holds a reference to `said`, which is what `WatchSecretExposure` means by "must
/// outlive @p reloader" and the same ordering `DaemonBody` states about its logger. Six
/// sections used to restate that in a comment and none of them enforced it, so a
/// seventh declaring its vector second would have compiled, passed, and left a
/// subscriber pointing at a destroyed object. Copying and moving are deleted because
/// the constructor hands out `this`.
struct Watched
{
    std::vector<std::string> said;
    ConfigReloader reloader;

    /// @param path The configuration file to watch.
    /// @param secret What `requirepass:` in that file says, or empty for none.
    Watched(std::filesystem::path const& path, std::string_view secret):
        reloader { InitialConfig(path, secret), path, {} }
    {
        // `false`: no case here supplies the secret on the command line. That the
        // provenance gate declines an argv-supplied secret is `SecretProvenance_test`'s
        // subject, asserted there against `DaemonSecretFiles` directly.
        WatchSecretExposure(reloader, false, [this](std::string_view warning) { said.emplace_back(warning); });
    }

    Watched(Watched const&) = delete;
    Watched(Watched&&) = delete;
    Watched& operator=(Watched const&) = delete;
    Watched& operator=(Watched&&) = delete;
    ~Watched() = default;
};

} // namespace

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

    // `ScratchDirectory::Write` rather than a local `ofstream`: it closes the stream
    // and THROWS when the write did not land, where a local copy would discard the
    // stream state and turn "no new warning arrived" into an assertion that passes by
    // testing nothing. It truncates and leaves an existing file's mode alone, which is
    // what the rewrites below depend on.
    auto const write = [&scratch](char const* stem, std::string_view yaml, ::mode_t mode) {
        scratch.Write(stem, yaml);
        auto const path = scratch / stem;
        REQUIRE(::chmod(path.c_str(), mode) == 0);
        return path;
    };

    static constexpr ::mode_t privateMode = S_IRUSR | S_IWUSR;
    static constexpr ::mode_t worldReadableMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    static constexpr ::mode_t groupReadableMode = S_IRUSR | S_IWUSR | S_IRGRP;

    SECTION("a secret appearing in an always-loose file is reported")
    {
        // The ticket's headline, and the one a startup-only check cannot see: the file
        // was already world-readable and carried no secret, so the start had nothing
        // to say and was right to say nothing.
        auto const path = write("gains-secret.yaml", "log_level: info\n", worldReadableMode);
        Watched watched { path, "" };
        REQUIRE(watched.said.empty());

        scratch.Write("gains-secret.yaml", "log_level: info\nrequirepass: hunter2\n");
        REQUIRE(watched.reloader.Reload().has_value());

        REQUIRE(watched.said.size() == 1);
        CHECK(watched.said.front().contains(path.string()));
        CHECK(watched.said.front().contains("chmod o-r"));
        // It goes to journald or a Windows event record, which more accounts can read
        // than could read the file. A diagnostic quoting the value would be the
        // exposure it is reporting, published by the report.
        CHECK_FALSE(watched.said.front().contains("hunter2"));
    }

    SECTION("a mode that loosens under an unchanged secret is reported")
    {
        // **The case a snapshot-diffing implementation cannot see.** Nothing in the
        // configuration moved -- the two snapshots the reloader publishes are equal
        // field for field -- and the file went from private to world-readable. Only
        // re-asking the filesystem answers this.
        auto const path = write("loosens.yaml", "requirepass: hunter2\n", privateMode);
        Watched watched { path, "hunter2" };
        REQUIRE(watched.said.empty());

        REQUIRE(::chmod(path.c_str(), worldReadableMode) == 0);
        REQUIRE(watched.reloader.Reload().has_value());

        REQUIRE(watched.said.size() == 1);
        CHECK(watched.said.front().contains(path.string()));
    }

    SECTION("an exposure that changes KIND is reported again")
    {
        // **What makes the memory `(path, exposure)` rather than `path`.** The subject
        // never leaves the set and never stops being exposed, so a watcher remembering
        // only the PATH stays silent -- and the operator is left holding `chmod g-r`
        // for a file that now needs `chmod o-r`. Two exposures, two remedies; the one
        // already said no longer describes the file.
        //
        // Not owned by root, so 0640 is `OwnersOwnGroup` rather than the delegated
        // arrangement the packages ship.
        auto const path = write("widens.yaml", "requirepass: hunter2\n", groupReadableMode);
        Watched watched { path, "hunter2" };
        REQUIRE(watched.said.size() == 1);
        CHECK(watched.said.front().contains("chmod g-r"));

        REQUIRE(::chmod(path.c_str(), worldReadableMode) == 0);
        REQUIRE(watched.reloader.Reload().has_value());

        REQUIRE(watched.said.size() == 2);
        CHECK(watched.said.back().contains("chmod o-r"));
    }

    SECTION("a standing exposure is said once, not at every reload")
    {
        // The other half, and the one that decides whether anybody reads the first
        // half: a warning repeated at every SIGHUP is the alarm-nobody-reads failure
        // arriving by a different route. Asserted on the COUNT, because both the
        // remembering and the repeating version warn at startup.
        auto const path = write("standing.yaml", "requirepass: hunter2\n", worldReadableMode);
        Watched watched { path, "hunter2" };
        REQUIRE(watched.said.size() == 1);

        REQUIRE(watched.reloader.Reload().has_value());
        REQUIRE(watched.reloader.Reload().has_value());
        CHECK(watched.said.size() == 1);
    }

    SECTION("a secret removed and put back is a fresh transition")
    {
        // The memory is what the LAST observation found, so a path that stopped being
        // a subject is forgotten. The operator's warning described a state that
        // stopped being true in between, and the return to exposure is new news.
        auto const path = write("returns.yaml", "requirepass: hunter2\n", worldReadableMode);
        Watched watched { path, "hunter2" };
        REQUIRE(watched.said.size() == 1);

        scratch.Write("returns.yaml", "log_level: info\n");
        REQUIRE(watched.reloader.Reload().has_value());
        CHECK(watched.said.size() == 1);

        scratch.Write("returns.yaml", "requirepass: hunter2\n");
        REQUIRE(watched.reloader.Reload().has_value());
        CHECK(watched.said.size() == 2);
    }

    SECTION("a restricted file stays silent across reloads")
    {
        // The control. Without it, "report a transition" and "report at every reload"
        // are told apart only by the counts above -- and a rule that warned about the
        // ORDINARY 0600 file would fire on nearly every deployment.
        auto const path = write("quiet.yaml", "requirepass: hunter2\n", privateMode);
        Watched watched { path, "hunter2" };

        REQUIRE(watched.reloader.Reload().has_value());
        CHECK(watched.said.empty());
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
