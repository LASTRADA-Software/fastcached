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
