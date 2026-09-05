// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/SecretProvenance.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>

#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
#endif

using FastCache::CliResult;
using FastCache::Config;
using FastCache::SecretCameFromConfigFile;
using FastCache::SecretFileWarning;
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
        { { .secretInForce = true, .namedOnCommandLine = false, .fileWasRead = true }, true, "a secret out of a file" },
        { { .secretInForce = false, .namedOnCommandLine = false, .fileWasRead = true },
          false,
          "no secret, so no file to protect it" },
        { { .secretInForce = true, .namedOnCommandLine = true, .fileWasRead = true },
          false,
          "argv supplied it, so the exposure is ps" },
        { { .secretInForce = true, .namedOnCommandLine = false, .fileWasRead = false },
          false,
          "no file was read, so there is none to name" },
        { { .secretInForce = false, .namedOnCommandLine = true, .fileWasRead = false }, false, "nothing at all" },
    });

    for (auto const& c: cases)
    {
        INFO(c.what);
        CHECK(SecretCameFromConfigFile(c.facts) == c.expected);
    }
}

TEST_CASE("SecretProvenance: a secret came from a file only when nothing else supplied it", "[config][secret]")
{
    // The gate on the whole of #384's startup check. Asked as PROVENANCE, because
    // a value comparison cannot tell an operator who typed `--requirepass=` --
    // asking for no authentication -- from one who typed nothing at all.
    SECTION("no secret in force")
    {
        Config cfg {};
        cfg.configPath = "/etc/fastcached/fastcached.yaml";
        CHECK_FALSE(SecretCameFromConfigFile(cfg, CliResult {}));
    }

    SECTION("a secret from a file")
    {
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = "/etc/fastcached/fastcached.yaml";
        CHECK(SecretCameFromConfigFile(cfg, CliResult {}));
    }

    SECTION("a secret the command line supplied is a different exposure")
    {
        // It is in `ps`, which is what `InlineCredentialRejection` refuses to bake
        // into a registration -- a different problem with a different answer, and
        // answering false here is not a claim that it is safe.
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = "/etc/fastcached/fastcached.yaml";
        CliResult cli {};
        cli.requirePassExplicit = true;
        CHECK_FALSE(SecretCameFromConfigFile(cfg, cli));
    }

    SECTION("a secret typed at the flag's own empty default is no secret")
    {
        // `--requirepass=` parses, and under CLI-over-file precedence it means "no
        // authentication whatever the file says". The bit is set and the value is
        // empty, and it is the VALUE that decides there is nothing to protect.
        Config cfg {};
        cfg.configPath = "/etc/fastcached/fastcached.yaml";
        CliResult cli {};
        cli.requirePassExplicit = true;
        CHECK_FALSE(SecretCameFromConfigFile(cfg, cli));
    }

    SECTION("no file was read")
    {
        // A run that declined a discovered file leaves `configPath` empty, so
        // there is no file to warn about. Without this clause the warning would
        // name the empty path -- an alarm about nothing, which is how a check
        // teaches people to ignore it.
        Config cfg {};
        cfg.requirePass = "hunter2";
        CHECK_FALSE(SecretCameFromConfigFile(cfg, CliResult {}));
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

    auto const warningFor = [](std::filesystem::path const& path) {
        Config cfg {};
        cfg.requirePass = "hunter2";
        cfg.configPath = path.string();
        return SecretFileWarning(cfg, CliResult {});
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
        CliResult cli {};
        cli.requirePassExplicit = true;
        CHECK(SecretFileWarning(cfg, cli).empty());
    }

    SECTION("a file with no secret in force says nothing, whatever its mode")
    {
        auto const path = configAt("nosecret.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        Config cfg {};
        cfg.configPath = path.string();
        CHECK(SecretFileWarning(cfg, CliResult {}).empty());
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
        auto const warning = SecretFileWarning(cfg, CliResult {});
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
