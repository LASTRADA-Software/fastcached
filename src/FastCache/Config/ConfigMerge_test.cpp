// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>

namespace
{

/// Assemble a configuration from a file this case writes and a command line, the way
/// `main` and the reloader do.
///
/// Real text on both sides rather than a hand-built `CliResult`: the explicit bits are
/// what make precedence work, and a result written out here could claim one no applier
/// ever granted.
/// @param scratch Where the file goes.
/// @param file The document, or nothing for "no configuration file at all".
/// @param args The command line, program name excluded.
/// @param metricsPortEnv The environment fallback, already resolved.
/// @return The assembly, or its refusal.
[[nodiscard]] std::expected<FastCache::EffectiveConfig, FastCache::ConfigError> Assemble(
    FastCache::Testing::ScratchDirectory const& scratch,
    std::optional<std::string_view> file,
    std::vector<std::string> args = {},
    std::optional<std::uint16_t> metricsPortEnv = std::nullopt)
{
    auto path = std::filesystem::path {};
    if (file.has_value())
    {
        scratch.Write("cfg.yaml", *file);
        path = scratch / "cfg.yaml";
    }
    return FastCache::AssembleEffectiveConfig(
        path, FastCache::ConfigSources { .args = std::move(args), .metricsPortEnv = metricsPortEnv });
}

} // namespace

// --- AssembleEffectiveConfig ------------------------------------------------
//
// The one place the daemon's configuration is put together: file, then command
// line, then environment, every source through `CliOptions()`'s own appliers.
// `main()` calls it at startup and `ConfigReloader` calls it again on every SIGHUP,
// so a property asserted here holds at both
// ([#622](https://github.com/LASTRADA-Software/fastcached/issues/622)).

TEST_CASE("AssembleEffectiveConfig: the file is the baseline and the command line runs over it", "[config][merge][assemble]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-precedence" };
    auto const assembled = Assemble(scratch, "port: 12000\nmax_memory: 4096\nlog_level: warn\n", { "--max-memory=1024" });
    REQUIRE(assembled.has_value());

    // Named on the command line: the command line wins.
    CHECK(assembled->Configuration().maxMemoryBytes == 1024);
    // Named only in the file: the file stands.
    CHECK(assembled->Configuration().port == 12000);
    CHECK(assembled->Configuration().logLevel == FastCache::LogLevel::Warn);
    // And the snapshot names the file it came from, which no flag supplied.
    CHECK(assembled->Configuration().configPath == (scratch / "cfg.yaml").string());
}

TEST_CASE("AssembleEffectiveConfig: a command line typing the default still outranks the file", "[config][merge][assemble]")
{
    // The input no value comparison can answer for, and the one a per-field merge
    // shipped wrong four times: the operator typed the compiled-in default ON PURPOSE.
    // Precedence is which applier ran last, so the typed default wins because it ran.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-typed-default" };
    auto const portFlag = std::format("--port={}", FastCache::DefaultPort);
    auto const assembled = Assemble(scratch,
                                    "port: 12000\nstorage_shards: 4\nlru_mode: strict\n",
                                    { portFlag, "--storage-shards=0", "--lru-mode=approximate" });
    REQUIRE(assembled.has_value());

    CHECK(assembled->Configuration().port == FastCache::DefaultPort);
    CHECK(assembled->Configuration().storageShards == 0U);
    CHECK(assembled->Configuration().lruRecency == FastCache::LruRecency::Approximate);
}

TEST_CASE("AssembleEffectiveConfig: with no file the command line stands alone", "[config][merge][assemble]")
{
    // Including the settings no file can carry -- `--daemon`, `--pidfile` -- and the
    // listener list, so assembling against no file is the parse itself rather than
    // "the defaults plus the rows a file shares".
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-nofile" };
    auto const assembled =
        Assemble(scratch, std::nullopt, { "--daemon", "--pidfile=/run/fc.pid", "--listen=127.0.0.1:11211", "--threads=5" });
    REQUIRE(assembled.has_value());

    CHECK(assembled->Configuration().daemon);
    CHECK(assembled->Configuration().pidfile == "/run/fc.pid");
    CHECK(assembled->Configuration().workerThreads == 5);
    REQUIRE(assembled->Configuration().binds.size() == 1);
    CHECK(assembled->Configuration().binds.front().port == 11211);
    // Nothing to name, so nothing is named -- a path the operator never typed must
    // not be invented here.
    CHECK(assembled->Configuration().configPath.empty());
}

TEST_CASE("AssembleEffectiveConfig: a file of nothing but comments is the defaults", "[config][merge][assemble]")
{
    // Every shipped reference configuration is this shape until somebody uncomments a
    // line, and it must behave exactly like running with no flags.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-comments" };
    auto const assembled = Assemble(scratch, "# port: 6674\n# max_memory: 4g\n");
    REQUIRE(assembled.has_value());

    auto config = assembled->Configuration();
    config.configPath.clear();
    CHECK(config == FastCache::CliResult {}.config);
}

TEST_CASE("AssembleEffectiveConfig: the environment applies only where nobody named the port", "[config][merge][assemble]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-env" };
    constexpr std::uint16_t EnvPort = 9999;

    SECTION("neither the file nor the command line named it")
    {
        auto const assembled = Assemble(scratch, "log_level: warn\n", {}, EnvPort);
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().metricsPort == EnvPort);
    }

    SECTION("the file named it, at the compiled-in default")
    {
        // The case a value comparison cannot answer: `metrics_port:` written out as
        // the default is a decision, and the bit its applier set is what says so.
        auto const assembled =
            Assemble(scratch, std::format("metrics_port: {}\n", FastCache::DefaultMetricsPort), {}, EnvPort);
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().metricsPort == FastCache::DefaultMetricsPort);
    }

    SECTION("the command line named it")
    {
        auto const assembled = Assemble(scratch, "log_level: warn\n", { "--metrics-port=7777" }, EnvPort);
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().metricsPort == 7777);
    }
}

TEST_CASE("AssembleEffectiveConfig: a file that is not there is FileNotFound", "[config][merge][assemble]")
{
    // Not `ParseError`: `YAML::BadFile` derives from `YAML::Exception`, and a general
    // catch once sent an operator who mistyped `--config` hunting for a syntax
    // mistake in a file that does not exist. What to DO about it stays with the
    // caller, because a file the operator named and one the daemon merely found are
    // not the same failure.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-missing" };
    auto const assembled = FastCache::AssembleEffectiveConfig(scratch / "absent.yaml", FastCache::ConfigSources {});
    REQUIRE_FALSE(assembled.has_value());
    CHECK(assembled.error().code == FastCache::ConfigErrorCode::FileNotFound);
}

TEST_CASE("AssembleEffectiveConfig: a file that fails anywhere is refused whole", "[config][merge][assemble]")
{
    // Declined, never half-applied: "some of the settings, up to the bad line" is a
    // configuration nobody wrote. The good line comes FIRST, so a reader that applied
    // as it went would already have taken it.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-badkey" };
    auto const assembled = Assemble(scratch, "port: 12000\nprot: 12000\n");
    REQUIRE_FALSE(assembled.has_value());
    CHECK(assembled.error().code == FastCache::ConfigErrorCode::UnknownKey);
    CHECK(assembled.error().field == "prot");
    CHECK(assembled.error().line == 2);
}

TEST_CASE("AssembleEffectiveConfig: a setting named in the file is named, and one nobody named is not",
          "[config][merge][assemble]")
{
    // `main()` asks this for a refusal a start makes once -- the legacy single-bind
    // triplet beside the listener list -- and it has to see a key the FILE declared,
    // not only a flag. It used to be ten hand-kept presence bits beside the reader.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-named" };
    auto const assembled = Assemble(scratch, "bind: 0.0.0.0\nport: 12000\n");
    REQUIRE(assembled.has_value());
    CHECK(assembled->Named(&FastCache::CliResult::bindAddressExplicit));
    CHECK(assembled->Named(&FastCache::CliResult::portExplicit));
    CHECK_FALSE(assembled->Named(&FastCache::CliResult::tlsEnabledExplicit));
}

TEST_CASE("AssembleEffectiveConfig: a file's listen and listen_tls fill one list", "[config][merge][assemble][bind]")
{
    // Each key spells its flag, so a file lists `host:port` exactly as argv does --
    // IPv6 in brackets, and quoted, because a bare `[` starts a YAML sequence.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-listen" };
    auto const assembled =
        Assemble(scratch, "listen:\n  - 10.0.0.1:11211\n  - \"[::1]:11212\"\nlisten_tls: 10.0.0.1:6380\n");
    REQUIRE(assembled.has_value());

    auto const& binds = assembled->Configuration().binds;
    REQUIRE(binds.size() == 3);
    CHECK(binds[0].address == "10.0.0.1");
    CHECK(binds[0].port == 11211U);
    CHECK_FALSE(binds[0].tls);
    CHECK(binds[1].address == "::1");
    CHECK_FALSE(binds[1].tls);
    CHECK(binds[2].port == 6380U);
    CHECK(binds[2].tls);
}

TEST_CASE("AssembleEffectiveConfig: a command line naming either listener flag replaces every endpoint the file declared",
          "[config][merge][assemble][bind]")
{
    // Mixing partial file endpoints with partial command-line ones would make which
    // ports are served depend on which spelling appeared where. A TLS flag replaces the
    // file's PLAIN listeners too, because both rows fill and clear one list.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-listen-replace" };
    constexpr std::string_view file = "listen: 10.0.0.1:11211\nlisten_tls: 10.0.0.1:6380\n";

    SECTION("a TLS flag over both file lists")
    {
        auto const assembled = Assemble(scratch, file, { "--listen-tls=127.0.0.1:6390" });
        REQUIRE(assembled.has_value());
        auto const& binds = assembled->Configuration().binds;
        REQUIRE(binds.size() == 1);
        CHECK(binds.front().address == "127.0.0.1");
        CHECK(binds.front().port == 6390U);
        CHECK(binds.front().tls);
    }

    SECTION("a command line naming neither leaves the file's endpoints alone")
    {
        // The control: without it, a clear that ran unconditionally would pass the
        // section above for the wrong reason.
        auto const assembled = Assemble(scratch, file, { "--threads=2" });
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().binds.size() == 2);
    }
}

TEST_CASE("AssembleEffectiveConfig: a presence setting in a file is true or false and nothing else",
          "[config][merge][assemble]")
{
    // The daemon's old reader took yaml-cpp's booleans, so `metrics: yes` and
    // `metrics: On` were accepted -- measured on 0.2.0-568 -- while the worker's file
    // refused both. One reader now, one rule.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-boolean" };
    constexpr auto spellings = std::to_array<std::string_view>({ "yes", "On", "1" });
    for (auto const spelling: spellings)
    {
        INFO("spelling: " << spelling);
        auto const assembled = Assemble(scratch, std::format("metrics: {}\n", spelling));
        REQUIRE_FALSE(assembled.has_value());
        CHECK(assembled.error().code == FastCache::ConfigErrorCode::TypeMismatch);
        CHECK(assembled.error().field == "metrics");
    }

    auto const on = Assemble(scratch, "metrics: true\n");
    REQUIRE(on.has_value());
    CHECK(on->Configuration().metricsEnabled);
}

TEST_CASE("AssembleEffectiveConfig: the timestamp setting is named by the flag's own key, in both polarities",
          "[config][merge][assemble]")
{
    // The default is platform-dependent (on under macOS, #496), so a presence key alone
    // cannot say OFF there: `log_timestamps: false` means "do not pass
    // `--log-timestamps`", which is the platform's answer. `no_log_timestamps: true` is
    // how a file says off, exactly as `--no-log-timestamps` is how argv says it.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-timestamps" };

    SECTION("the negative key turns it off wherever the platform turns it on")
    {
        auto const assembled = Assemble(scratch, "no_log_timestamps: true\n");
        REQUIRE(assembled.has_value());
        CHECK_FALSE(assembled->Configuration().logTimestamps);
        CHECK(assembled->Named(&FastCache::CliResult::logTimestampsExplicit));
    }

    SECTION("with the default injected, so the case discriminates off macOS too")
    {
        // `DefaultLogTimestamps` is ON only under macOS, so the assembled sections
        // above assert the off direction vacuously everywhere else. The same rows,
        // through the same file layer the assembly runs, over each default in turn.
        for (auto const injected: { true, false })
        {
            INFO("default: " << injected);
            FastCache::CliResult off;
            off.config.logTimestamps = injected;
            REQUIRE(FastCache::ApplyFileSettings(
                        FastCache::CliOptions(),
                        { FastCache::YamlSetting { .key = "no_log_timestamps", .values = { "true" }, .line = 1 } },
                        scratch / "cfg.yaml",
                        off)
                        .has_value());
            CHECK_FALSE(off.config.logTimestamps);

            FastCache::CliResult untouched;
            untouched.config.logTimestamps = injected;
            REQUIRE(FastCache::ApplyFileSettings(
                        FastCache::CliOptions(),
                        { FastCache::YamlSetting { .key = "log_timestamps", .values = { "false" }, .line = 1 },
                          FastCache::YamlSetting { .key = "no_log_timestamps", .values = { "false" }, .line = 2 } },
                        scratch / "cfg.yaml",
                        untouched)
                        .has_value());
            CHECK(untouched.config.logTimestamps == injected);
        }
    }

    SECTION("the positive key turns it on wherever the platform leaves it off")
    {
        auto const assembled = Assemble(scratch, "log_timestamps: true\n");
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().logTimestamps);
    }

    SECTION("false on either key passes nothing, which is the platform default")
    {
        auto const assembled = Assemble(scratch, "log_timestamps: false\nno_log_timestamps: false\n");
        REQUIRE(assembled.has_value());
        // The VALUE is the platform's. The setting still reads as named, because a key
        // in a file is the operator naming it whatever it says (`ApplyFileSettings`).
        CHECK(assembled->Configuration().logTimestamps == FastCache::DefaultLogTimestamps);
    }

    SECTION("a typed flag still outranks the file")
    {
        auto const assembled = Assemble(scratch, "no_log_timestamps: true\n", { "--log-timestamps" });
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().logTimestamps);
    }
}

TEST_CASE("AssembleEffectiveConfig: a path setting expands the environment from a file and not from argv",
          "[config][merge][assemble]")
{
    // A file has no shell in front of it; argv had one. So `${NAME}` in `storage_path:`
    // is expanded, `--storage=${NAME}` arrives as the operator's shell left it, and a
    // secret beside it is never expanded -- a `$` in a password is part of the password.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-assemble-expand" };
    FastCache::Testing::ScopedEnv const root { "FC_ASSEMBLE_ROOT", "/srv/fc" };

    SECTION("from a file")
    {
        auto const assembled = Assemble(scratch,
                                        "storage_path: ${FC_ASSEMBLE_ROOT}/cache\ntls_cert: "
                                        "$FC_ASSEMBLE_ROOT/c.pem\nrequirepass: \"$FC_ASSEMBLE_ROOT\"\n");
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().storagePath == "/srv/fc/cache");
        CHECK(assembled->Configuration().tlsCertPath == "/srv/fc/c.pem");
        CHECK(assembled->Configuration().requirePass == "$FC_ASSEMBLE_ROOT");
    }

    SECTION("from the command line")
    {
        auto const assembled = Assemble(scratch, std::nullopt, { "--storage=${FC_ASSEMBLE_ROOT}/cache" });
        REQUIRE(assembled.has_value());
        CHECK(assembled->Configuration().storagePath == "${FC_ASSEMBLE_ROOT}/cache");
    }

    SECTION("an unset variable is refused at its key and line")
    {
        auto const assembled = Assemble(scratch, "port: 12000\nstorage_path: ${FC_ASSEMBLE_NEVER_SET}/cache\n");
        REQUIRE_FALSE(assembled.has_value());
        CHECK(assembled.error().code == FastCache::ConfigErrorCode::UndefinedVariable);
        CHECK(assembled.error().field == "storage_path");
        CHECK(assembled.error().line == 2);
    }
}

// --- ValidateBindFlagShape ---------------------------------------------------

TEST_CASE("ValidateBindFlagShape: a legacy bind setting beside a listener is refused, from either source",
          "[config][bind][validate]")
{
    // Pre-fix, main.cpp silently picked `binds` and discarded `bindAddress`. The two
    // shapes can arrive from different SOURCES, and the value is lost the same way.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-bind-shape" };
    struct Mix
    {
        std::optional<std::string_view> file;
        std::vector<std::string> args;
        std::string_view named;
    };
    auto const mixes = std::vector<Mix> {
        { .file = std::nullopt, .args = { "--bind=1.2.3.4", "--listen=1.2.3.4:6379" }, .named = "--bind" },
        { .file = std::nullopt, .args = { "--port=6379", "--listen=1.2.3.4:6379" }, .named = "--port" },
        { .file = std::nullopt, .args = { "--tls", "--listen-tls=1.2.3.4:6379" }, .named = "--tls" },
        { .file = "bind: 1.2.3.4\n", .args = { "--listen=1.2.3.4:6379" }, .named = "--bind" },
        { .file = "tls: true\nlisten_tls: 1.2.3.4:6379\n", .args = {}, .named = "--tls" },
    };
    for (auto const& mix: mixes)
    {
        INFO("file: " << mix.file.value_or("<none>") << " named: " << mix.named);
        auto const assembled = Assemble(scratch, mix.file, mix.args);
        REQUIRE(assembled.has_value());
        auto const shape = FastCache::ValidateBindFlagShape(*assembled);
        REQUIRE_FALSE(shape.has_value());
        CHECK(shape.error().field == "listen");
        CHECK(shape.error().context.contains(mix.named));
    }
}

TEST_CASE("ValidateBindFlagShape: either shape alone is accepted", "[config][bind][validate]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-bind-shape-alone" };

    SECTION("listeners alone")
    {
        auto const assembled = Assemble(scratch, "listen: 1.2.3.4:6379\n", { "--listen-tls=1.2.3.4:6380" });
        REQUIRE(assembled.has_value());
        CHECK(FastCache::ValidateBindFlagShape(*assembled).has_value());
    }

    SECTION("the legacy triplet alone, from both sources")
    {
        // binds is empty, so main.cpp synthesises one from bindAddress/port/tls and
        // nothing is silently dropped.
        auto const assembled = Assemble(scratch, "bind: 1.2.3.4\ntls: true\n", { "--port=6379" });
        REQUIRE(assembled.has_value());
        CHECK(FastCache::ValidateBindFlagShape(*assembled).has_value());
    }
}

TEST_CASE("ValidateBinds: distinct endpoints pass", "[config][bind][validate]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "0.0.0.0", .port = 11211, .tls = false },
        { .address = "0.0.0.0", .port = 6380, .tls = true },
        { .address = "127.0.0.1", .port = 11211, .tls = false },
    };
    auto const v = FastCache::ValidateBinds(binds);
    REQUIRE(v.has_value());
}

TEST_CASE("ValidateBinds: duplicate {addr,port} pairs are rejected", "[config][bind][validate]")
{
    // The two entries differ only in `tls`. SO_REUSEPORT would let both bind
    // and the kernel would split traffic randomly across them — protocol
    // confusion the moment a plaintext client lands on the TLS listener.
    std::vector<FastCache::BindConfig> binds {
        { .address = "0.0.0.0", .port = 6379, .tls = false },
        { .address = "0.0.0.0", .port = 6379, .tls = true },
    };
    auto const v = FastCache::ValidateBinds(binds);
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().field == "listen");
}

TEST_CASE("ValidateBinds: empty list is trivially valid", "[config][bind][validate]")
{
    // The empty-binds shape is rejected one layer up (main.cpp synthesises a
    // legacy fallback) — ValidateBinds itself imposes no minimum.
    std::vector<FastCache::BindConfig> binds {};
    REQUIRE(FastCache::ValidateBinds(binds).has_value());
}

TEST_CASE("FormatBindSummary: single plaintext bind", "[config][bind][summary]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "127.0.0.1", .port = 11211, .tls = false },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "127.0.0.1:11211");
}

TEST_CASE("FormatBindSummary: single TLS bind carries [tls] marker", "[config][bind][summary]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "127.0.0.1", .port = 6379, .tls = true },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "127.0.0.1:6379 [tls]");
}

TEST_CASE("FormatBindSummary: two binds (plain + TLS) join with ', '", "[config][bind][summary]")
{
    // Regression test for the original banner bug: the daemon brought up
    // with `--listen a:1 --listen-tls b:2` used to log
    // the defaults of the unused legacy fields instead of its real endpoints.
    std::vector<FastCache::BindConfig> binds {
        { .address = "10.0.0.1", .port = 11211, .tls = false },
        { .address = "10.0.0.1", .port = 6380, .tls = true },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "10.0.0.1:11211, 10.0.0.1:6380 [tls]");
}

TEST_CASE("FormatBindSummary: empty list renders <none>", "[config][bind][summary]")
{
    // Defensive — RunReactorServer rejects empty `binds` one layer up, but
    // FormatBindSummary itself stays total.
    std::vector<FastCache::BindConfig> binds {};
    REQUIRE(FastCache::FormatBindSummary(binds) == "<none>");
}
