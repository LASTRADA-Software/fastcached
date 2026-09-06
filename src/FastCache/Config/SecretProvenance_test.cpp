// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/SecretProvenance.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <tests/PathFlagCoverage.hpp>
#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using FastCache::CliOptions;
using FastCache::CliResult;
using FastCache::Config;
using FastCache::DaemonPublicPathFlags;
using FastCache::DaemonSecretFiles;
using FastCache::DaemonSecretFileTable;
using FastCache::SecretCameFromConfigFile;
using FastCache::SecretFileWarnings;
using FastCache::SecretProvenanceFacts;

TEST_CASE("SecretProvenance: the rule over the facts is one function both executables reach", "[config][secret]")
{
    // The DECISION half of the acquisition/decision split. `fastcached` reads the
    // three facts off `Config` and `CliResult`, `fastcache-compile-node` reads them
    // off `NodeConfig` and a command-line-only parse -- and neither writes the rule.
    // Asserted as a truth table, because what makes a shared rule worth having is
    // that every combination has ONE answer rather than two that agree today.
    struct Case
    {
        SecretProvenanceFacts facts;
        bool expected;
        char const* what;
    };
    static constexpr auto cases = std::to_array<Case>({
        { .facts = { .secretInForce = true, .namedOnCommandLine = false, .fileWasRead = true },
          .expected = true,
          .what = "a secret out of a file" },
        { .facts = { .secretInForce = false, .namedOnCommandLine = false, .fileWasRead = true },
          .expected = false,
          .what = "no secret, so no file to protect it" },
        { .facts = { .secretInForce = true, .namedOnCommandLine = true, .fileWasRead = true },
          .expected = false,
          .what = "argv supplied it, so the exposure is ps" },
        { .facts = { .secretInForce = true, .namedOnCommandLine = false, .fileWasRead = false },
          .expected = false,
          .what = "no file was read, so there is none to name" },
        { .facts = { .secretInForce = false, .namedOnCommandLine = true, .fileWasRead = false },
          .expected = false,
          .what = "nothing at all" },
    });

    for (auto const& c: cases)
    {
        INFO(c.what);
        CHECK(SecretCameFromConfigFile(c.facts) == c.expected);
    }
}

TEST_CASE("SecretProvenance: the daemon's subject list is what the gate lets through", "[config][secret]")
{
    // The gate on the whole of #384's startup check, asserted where production asks
    // it. Asked as PROVENANCE, because a value comparison cannot tell an operator who
    // typed `--requirepass=` -- asking for no authentication -- from one who typed
    // nothing at all; the bit comes from the parse (`CliResult::requirePassExplicit`)
    // and is passed as the fact it is.
    std::filesystem::path const configFile { "/etc/fastcached/fastcached.yaml" };

    SECTION("no secret in force")
    {
        Config cfg {};
        cfg.configPath = configFile.string();
        CHECK(DaemonSecretFiles(cfg, false).empty());
    }

    SECTION("a secret from a file")
    {
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = configFile.string();
        CHECK(DaemonSecretFiles(cfg, false) == std::vector<std::filesystem::path> { configFile });
    }

    SECTION("a secret the command line supplied is a different exposure")
    {
        // It is in `ps`, which is what `InlineCredentialRejection` refuses to bake
        // into a registration -- a different problem with a different answer, and
        // answering "no file" here is not a claim that it is safe.
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = configFile.string();
        CHECK(DaemonSecretFiles(cfg, true).empty());
    }

    SECTION("a secret typed at the flag's own empty default is no secret")
    {
        // `--requirepass=` parses, and under CLI-over-file precedence it means "no
        // authentication whatever the file says". The bit is set and the value is
        // empty, and it is the VALUE that decides there is nothing to protect.
        Config cfg {};
        cfg.configPath = configFile.string();
        CHECK(DaemonSecretFiles(cfg, true).empty());
    }

    SECTION("no file was read")
    {
        // A run that declined a discovered file leaves `configPath` empty, so
        // there is no file to warn about. Without this clause the warning would
        // name the empty path -- an alarm about nothing, which is how a check
        // teaches people to ignore it.
        Config cfg {};
        cfg.requirePass = "hunter2";
        CHECK(DaemonSecretFiles(cfg, false).empty());
    }
}

#if !defined(_WIN32)

TEST_CASE("SecretProvenance: the startup warning fires on an exposed file and not a restricted one", "[config][secret]")
{
    // **#384's acceptance, both halves.** The second is the one that matters
    // most: a secret in a correctly-restricted file must produce NOTHING, or the
    // check becomes an alarm everyone learns to ignore -- and 0600 is the ordinary
    // case, so a rule that warned about it would fire on nearly every deployment.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-warning" };

    auto const configAt = [&scratch](char const* stem, ::mode_t mode) {
        scratch.Write(stem, "requirepass: hunter2\n");
        auto const path = scratch / stem;
        REQUIRE(::chmod(path.c_str(), mode) == 0);
        return path;
    };

    // The daemon's whole startup answer, exactly as `DaemonBody` composes it: the
    // subject list, then the renderer. Written out here rather than behind a
    // one-shot helper, because the helper would be a second production path nothing
    // calls -- and a function with no production caller is the defect it was written
    // to fix.
    auto const daemonWarning = [](Config const& cfg, bool secretNamedOnCommandLine) {
        auto const warnings = SecretFileWarnings(DaemonSecretFiles(cfg, secretNamedOnCommandLine));
        return warnings.empty() ? std::string {} : warnings.front();
    };

    auto const warningFor = [&daemonWarning](std::filesystem::path const& path) {
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = path.string();
        return daemonWarning(cfg, false);
    };

    SECTION("a world-readable file warns, and says what to do")
    {
        auto const path = configAt("exposed.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        auto const warning = warningFor(path);
        INFO("warning: " << warning);
        REQUIRE_FALSE(warning.empty());
        // Names the file and the remedy. A warning an operator cannot act on is
        // the one that scrolls past, which is the objection this design has to
        // answer rather than inherit.
        CHECK(warning.contains(path.string()));
        CHECK(warning.contains("chmod o-r"));
    }

    SECTION("a restricted file says nothing at all")
    {
        CHECK(warningFor(configAt("private.yaml", S_IRUSR | S_IWUSR)).empty());
    }

    SECTION("a secret from the command line says nothing, whatever the file's mode")
    {
        // The file is wide open and it is still not what protects the secret.
        auto const path = configAt("argv.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = path.string();
        CHECK(daemonWarning(cfg, true).empty());
    }

    SECTION("a file with no secret in force says nothing, whatever its mode")
    {
        auto const path = configAt("nosecret.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        Config cfg {};
        cfg.configPath = path.string();
        CHECK(daemonWarning(cfg, false).empty());
    }

    SECTION("every exposed file is named, not just the first")
    {
        // #752's whole shape: a worker holds FIVE secret-bearing files where the
        // daemon holds one, and an operator handed the first exposure and none of
        // the others fixes one file and believes they are done. A `std::string`
        // return could not carry this, which is why the aggregate exists.
        auto const first = configAt("first.key", S_IRUSR | S_IWUSR | S_IROTH);
        auto const safe = configAt("safe.key", S_IRUSR | S_IWUSR);
        auto const last = configAt("last.key", S_IRUSR | S_IWUSR | S_IROTH);
        std::array<std::filesystem::path, 3> const files { first, safe, last };

        auto const warnings = SecretFileWarnings(files);
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].contains(first.string()));
        CHECK(warnings[1].contains(last.string()));
        // Order is the caller's, so an operator reads them in the order the rows
        // were declared rather than in whatever order the filesystem answered.
        CHECK_FALSE(warnings[0].contains(last.string()));
    }

    SECTION("one file named twice is one sentence")
    {
        // A single-machine deployment legitimately points two settings at one file --
        // the cluster key and the scheduler token, say. The same remedy for the same
        // path twice reads as two problems, which is the alarm fatigue this check
        // exists to avoid rather than cause. Asserted on the COUNT and not merely on
        // "a warning arrived": both the deduplicating and the repeating version warn.
        auto const shared = configAt("shared.key", S_IRUSR | S_IWUSR | S_IROTH);
        auto const other = configAt("other.key", S_IRUSR | S_IWUSR | S_IROTH);
        std::array<std::filesystem::path, 3> const files { shared, other, shared };

        auto const warnings = SecretFileWarnings(files);
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].contains(shared.string()));
        CHECK(warnings[1].contains(other.string()));
    }

    SECTION("the warning never contains the secret")
    {
        // It goes to journald or a Windows event record, which are readable by
        // more accounts than the file was. A diagnostic that quoted the value
        // would be the exposure it is reporting, published by the report.
        auto const path = configAt("leak.yaml", S_IRUSR | S_IWUSR | S_IROTH);
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = path.string();
        auto const warning = daemonWarning(cfg, false);
        REQUIRE_FALSE(warning.empty());
        CHECK_FALSE(warning.contains("hunter2"));
    }
}

#endif

TEST_CASE("SecretProvenance: which paths are never asked about at all", "[config][secret]")
{
    // Outside the POSIX guard deliberately. Neither case reaches a mode bit, and both
    // guard a branch of `SecretFileWarnings` that exists precisely so the platform is
    // NOT asked -- so running them only where `chmod` exists would leave the two skips
    // untested on the platform whose answer for a file it cannot inspect
    // (`Undetermined`, from an unreadable security descriptor) is the one that would
    // turn a mistyped `--cluster-key-file` into a sentence about permissions.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-skips" };

    SECTION("a path that is not there says nothing")
    {
        // Distinct from "the platform would not say who may read this", which IS
        // reported. `SecretFileExposure` answers `Undetermined` for a failed `stat`,
        // so asking it unconditionally would answer a mistyped `--cluster-key-file`
        // with a sentence about permissions -- while whoever loads the file answers
        // with the real diagnosis a line later.
        std::array<std::filesystem::path, 1> const files { scratch / "never-written.key" };
        CHECK(SecretFileWarnings(files).empty());
    }

    SECTION("an unnamed setting names no file")
    {
        std::array<std::filesystem::path, 1> const files { std::filesystem::path {} };
        CHECK(SecretFileWarnings(files).empty());
    }
}

TEST_CASE("DaemonSecretFiles: every path-valued flag is classified, secret or not", "[config][secret]")
{
    // **The coverage guard, and it is mandatory rather than opt-in.** A list that is
    // exact about the flags it knows and silent about the ones it does not reads
    // identically to complete coverage (#492), which is the failure #752 describes:
    // answering the narrow half while looking complete. So a seventh `=<path>` row
    // cannot be added to `CliOptions()` without its author saying which kind it is.
    //
    // The join itself is `Testing::ClassifyPathFlags`, shared with the worker's twin --
    // one rule asked of two option tables, rather than two Catch2 cases that would
    // drift on which directions they check.
    auto const secret = DaemonSecretFileTable();
    auto const publicPaths = DaemonPublicPathFlags();
    auto const coverage = FastCache::Testing::ClassifyPathFlags<CliResult>(
        CliOptions(), FastCache::Testing::FlagsOf(secret), FastCache::Testing::FlagsOf(publicPaths));

    SECTION("every =<path> row of CliOptions() is in exactly one table")
    {
        // Three assertions rather than one, because "nobody classified it", "both
        // tables claim it" and "a table names a flag that no longer exists" are three
        // different repairs. A single verdict would name none of them.
        INFO("unclassified: " << FastCache::Testing::Join(coverage.unclassified));
        CHECK(coverage.unclassified.empty());

        INFO("classified twice: " << FastCache::Testing::Join(coverage.classifiedTwice));
        CHECK(coverage.classifiedTwice.empty());

        INFO("naming no row: " << FastCache::Testing::Join(coverage.namingNoRow));
        CHECK(coverage.namingNoRow.empty());

        // The positive control. A scan that matched nothing would leave all three
        // lists empty and pass, and "no violations found" and "the scan found nothing
        // to look at" are the two states this codebase keeps having to tell apart.
        CHECK(coverage.pathRows > 0);
        CHECK(coverage.pathRows == secret.size() + publicPaths.size());
    }

    SECTION("every public row says why, and --tls-cert is one of them")
    {
        for (auto const& row: publicPaths)
        {
            INFO("public row: " << row.flag);
            // The reason is a forcing function, not a dead field: a blank one would
            // spell "forgot" in the vocabulary of "decided".
            CHECK_FALSE(row.why.empty());
        }

        // Decided explicitly rather than by omission, because it is the one an author
        // would add by symmetry with `--tls-key`. A certificate is presented to every
        // client during the handshake, so warning about the mode of a file that is
        // MEANT to be readable is the alarm that teaches operators to ignore the one
        // that matters.
        CHECK(FastCache::Testing::Names(publicPaths, "--tls-cert"));
        CHECK(FastCache::Testing::Names(secret, "--tls-key"));
    }
}

TEST_CASE("DaemonSecretFiles: a path-reached secret is not provenance-gated", "[config][secret]")
{
    // **#752's design decision, on the daemon.** #384's rule is gated on provenance
    // because `--requirepass` can also arrive in argv, where the exposure is `ps` and
    // belongs to `InlineCredentialRejection`. A key FILE has no second route: the path
    // is not the secret and the file is. Routing `--tls-key` through the provenance
    // gate would silently skip every argv-named key, which is a change no test
    // asserting merely that "some warning arrives" could see.
    Config cfg;
    cfg.tlsKeyPath = "/etc/fastcached/server.key";
    cfg.tlsCertPath = "/etc/fastcached/server.crt";

    SECTION("named in argv, with no configuration file at all")
    {
        // Both halves of the provenance gate answer "not this file's business" here,
        // and the key must still be asked about.
        CHECK(DaemonSecretFiles(cfg, /*secretNamedOnCommandLine*/ true)
              == std::vector<std::filesystem::path> { cfg.tlsKeyPath });
    }

    SECTION("the certificate is never asked about")
    {
        auto const files = DaemonSecretFiles(cfg, false);
        CHECK(std::ranges::find(files, std::filesystem::path { cfg.tlsCertPath }) == files.end());
    }

    SECTION("a key named with --tls off is still a key on disk")
    {
        // Whether a surface exists is not a fact about the configuration, so a rule
        // whose premise is "somebody will read this" cannot state its premise without
        // guessing. `tlsEnabled` is left false here deliberately.
        REQUIRE_FALSE(cfg.tlsEnabled);
        CHECK(DaemonSecretFiles(cfg, false) == std::vector<std::filesystem::path> { cfg.tlsKeyPath });
    }

    SECTION("an unnamed flag contributes nothing")
    {
        CHECK(DaemonSecretFiles(Config {}, false).empty());
    }

    SECTION("the configuration file comes first")
    {
        // It is the file an operator most often has open, and the order is the
        // caller's rather than the filesystem's.
        cfg.requirePass = "hunter2";
        cfg.configPath = "/etc/fastcached/fastcached.yaml";
        CHECK(DaemonSecretFiles(cfg, false)
              == std::vector<std::filesystem::path> { std::filesystem::path { cfg.configPath }, cfg.tlsKeyPath });
    }
}

#if !defined(_WIN32)

TEST_CASE("DaemonSecretFiles: an exposed --tls-key warns and a private one does not", "[config][secret]")
{
    // **#864's acceptance, and the two halves are asserted separately.** A test that
    // only showed the warning arriving would pass just as well against an
    // implementation that warns about every key file it is handed, which is the alarm
    // that gets the real one ignored.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-daemon-tls-key" };
    scratch.Write("server.key", "-----BEGIN PRIVATE KEY-----\n");
    auto const key = scratch / "server.key";

    Config cfg;
    cfg.tlsKeyPath = key.string();

    SECTION("world-readable warns, and says what to do")
    {
        REQUIRE(::chmod(key.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0);

        auto const warnings = SecretFileWarnings(DaemonSecretFiles(cfg, false));
        REQUIRE(warnings.size() == 1);
        CHECK(warnings.front().contains(key.string()));
        CHECK(warnings.front().contains("chmod o-r"));
    }

    SECTION("mode 0600 says nothing at all")
    {
        REQUIRE(::chmod(key.c_str(), S_IRUSR | S_IWUSR) == 0);
        CHECK(SecretFileWarnings(DaemonSecretFiles(cfg, false)).empty());
    }
}

#endif
