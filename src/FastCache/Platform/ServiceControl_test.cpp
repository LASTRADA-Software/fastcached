// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Markup.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Platform/Firewall.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ProcessExit.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/FirewallFakes.hpp>
#include <tests/ScriptedServiceCalls.hpp>
#include <tests/SteppedDrainWait.hpp>
#include <tests/Unwrap.hpp>

using FastCache::Testing::Unwrap;

namespace
{
/// @p cfg as it would arrive from a command line that NAMED every setting.
///
/// A registration carries what the operator typed, not what differs from a
/// default (#349), so a case whose premise is "the operator ran
/// `--install-service` with these flags" has to say so. Every explicit bit is
/// set by walking `CliOptions()` rather than by listing them here: a new flag
/// joins these fixtures by adding its row, and cannot be left out of them by
/// omission.
///
/// Cases about what a registration says when the operator named NOTHING pass a
/// default-constructed `CliResult` instead, which is the other half of the same
/// property.
/// @param cfg The configuration values to carry.
/// @return A parse result holding @p cfg with every flag marked as typed.
[[nodiscard]] FastCache::CliResult AsTyped(FastCache::Config const& cfg)
{
    FastCache::CliResult typed {};
    typed.config = cfg;
    for (auto const& spec: FastCache::CliOptions())
        if (spec.explicitBit != nullptr)
            typed.*spec.explicitBit = true;
    return typed;
}

/// The `ServiceSpec` the daemon would register for @p cli.
///
/// These cases are about which *flags* survive a round trip into a
/// supervisor, which is unchanged; what moved is that the platform half now
/// speaks `ServiceSpec` rather than the daemon's `Config`. Routing through
/// `MakeDaemonServiceSpec` keeps each case asking its original question.
/// @param exePath Executable to register.
/// @param cli Command-line parse to bake in.
/// @return The spec.
[[nodiscard]] FastCache::ServiceSpec SpecFor(std::filesystem::path const& exePath, FastCache::CliResult const& cli)
{
    return FastCache::MakeDaemonServiceSpec(exePath, cli);
}

/// The spec for an invocation that named every one of @p cfg's settings.
/// @param exePath Executable to register.
/// @param cfg Configuration to bake in.
/// @return The spec.
[[nodiscard]] FastCache::ServiceSpec SpecFor(std::filesystem::path const& exePath, FastCache::Config const& cfg)
{
    return SpecFor(exePath, AsTyped(cfg));
}

/// The command line the SCM would be launched with for @p cli.
/// @param exePath Executable to register.
/// @param cli Command-line parse to bake in.
/// @return The fully-quoted command line.
[[nodiscard]] std::string CommandLineFor(std::filesystem::path const& exePath, FastCache::CliResult const& cli)
{
    return FastCache::BuildServiceCommandLine(SpecFor(exePath, cli));
}

/// The command line for an invocation that named every one of @p cfg's settings.
/// @param exePath Executable to register.
/// @param cfg Configuration to bake in.
/// @return The fully-quoted command line.
[[nodiscard]] std::string CommandLineFor(std::filesystem::path const& exePath, FastCache::Config const& cfg)
{
    return CommandLineFor(exePath, AsTyped(cfg));
}
} // namespace

TEST_CASE("ServiceControl: a command line that named nothing registers nothing", "[platform][service]")
{
    // The other half of #349's property. A registration carries what the operator
    // NAMED, so an invocation that named no setting registers no flag -- and the
    // next start re-derives every one of them exactly as this one did, including
    // the host-derived ones.
    FastCache::CliResult const cli {};
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cli);
    REQUIRE(cmd == "\"fastcached\" --daemon --service-name=FastCached");
}

TEST_CASE("ServiceControl: the executable path is always quoted", "[platform][service]")
{
    FastCache::CliResult const cli {};
    auto const cmd = CommandLineFor(std::filesystem::path { "C:/Program Files/fastcached.exe" }, cli);
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
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--port=6000"));
    REQUIRE(cmd.contains("--bind=0.0.0.0"));
    REQUIRE(cmd.contains("--threads=8"));
    REQUIRE(cmd.contains("--max-memory=134217728"));
    REQUIRE(cmd.contains("--storage-shards=4"));
}

TEST_CASE("ServiceControl: a flag the operator never named is omitted", "[platform][service]")
{
    // Not "flags left at their default", which is what this used to assert and is
    // the wrong question: `--max-memory` at its default may be a value the operator
    // typed, and #349 is what dropping it costs. What is omitted is what was never
    // named.
    FastCache::CliResult const cli {};
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cli);
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
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--log-level=debug"));
    REQUIRE(cmd.contains("--storage-durability=fsync"));
}

TEST_CASE("ServiceControl: the service name is always emitted, quoted when it has spaces", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.serviceName = "My Cache";
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(cmd.contains("--service-name=\"My Cache\""));
}

TEST_CASE("ServiceControl: a relative storage path is absolutized", "[platform][service]")
{
    FastCache::Config cfg {};
    cfg.storagePath = "relative/cache.cow";
    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cfg);

    auto const expected = std::filesystem::absolute("relative/cache.cow").string();
    REQUIRE(cmd.contains(expected));
    // The bare relative path must not survive — a service's working directory is
    // not the install directory, so it would resolve to the wrong location.
    REQUIRE(!cmd.contains("--storage=relative/cache.cow"));
}

TEST_CASE("ServiceControl: a registration refused by name declines, having changed nothing", "[platform][service][exit]")
{
    // A name the registration would not use is refused before the service manager is even opened,
    // so the operation DECLINES (2) rather than failing (1): nothing was changed. Safe
    // to call on every host for that reason -- the rejection returns first, where a name it
    // accepted would register a real service. Off Windows and macOS the stub declines the same
    // way, for its own reason, and the case holds it to the ending alone.
    FastCache::Config cfg {};
    cfg.serviceName = "a/b";
    auto const spec = SpecFor(std::filesystem::path { "fastcached" }, cfg);
    REQUIRE(FastCache::ServiceNameRejection(spec).has_value());

    auto const installed = FastCache::InstallService(spec);
    auto const removed = FastCache::UninstallService(spec);
    CHECK(installed.outcome == FastCache::ServiceControlOutcome::Declined);
    CHECK(removed.outcome == FastCache::ServiceControlOutcome::Declined);
    CHECK(installed.Ending() == FastCache::CommandEnding::Declined);
    CHECK(removed.ExitCode() == FastCache::CommandExitCode(FastCache::CommandEnding::Declined));
#if defined(_WIN32) || defined(__APPLE__)
    CHECK(removed.message == *FastCache::ServiceNameRejection(spec));
#endif
}

TEST_CASE("ServiceControl: install/uninstall are unsupported without a supervisor", "[platform][service]")
{
#if !defined(_WIN32) && !defined(__APPLE__)
    FastCache::Config const cfg {};
    auto const spec = SpecFor(std::filesystem::path { "fastcached" }, cfg);
    auto const installed = FastCache::InstallService(spec);
    auto const removed = FastCache::UninstallService(spec);
    // Declined: with no supervisor to act on, nothing was changed.
    REQUIRE(installed.outcome == FastCache::ServiceControlOutcome::Declined);
    REQUIRE(removed.outcome == FastCache::ServiceControlOutcome::Declined);
    REQUIRE(installed.Ending() == FastCache::CommandEnding::Declined);
#else
    // Deliberately not called here: on Windows and macOS these really do
    // register a service, which a unit test must not do to the host. The
    // scripts/macos-service-e2e.sh and the Windows MSI path cover them.
    //
    // Skipped rather than passed. Nothing here observes the property, and a
    // green result claiming otherwise is what #685 is about -- "covered by
    // another test" is a reason to skip this one, never a reason to pass it.
    SKIP("install/uninstall are exercised by the platform end-to-end paths, not by this unit test");
#endif
}

// ----------------------------------------------------------------------------
// launchd

using FastCache::BuildLaunchdPlist;
using FastCache::BuildServiceArgv;
using FastCache::EmitDaemonFlag;
using FastCache::ServiceScope;

namespace
{
/// @p path's whole text, through the stream buffer for `PosixDaemonHost_test`'s measured reason.
/// @param path A file the case has already found.
/// @return Its bytes.
[[nodiscard]] std::string ReadWholeFile(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    REQUIRE(in);
    std::ostringstream contents;
    contents << in.rdbuf();
    return std::move(contents).str();
}

/// The plist a default config produces in @p scope, for the assertions below.
[[nodiscard]] std::string PlistFor(FastCache::Config const& cfg, ServiceScope scope)
{
    return BuildLaunchdPlist(SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, cfg), scope, "/tmp/logs");
}
} // namespace

TEST_CASE("ServiceControl: launchd argv never carries --daemon", "[platform][service][launchd]")
{
    // The single most important property here. launchd supervises the process
    // it spawned; a job that double-forks is reaped immediately as "exited",
    // so the service silently never runs.
    FastCache::Config const cfg {};
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--daemon") == argv.end());

    auto const plist = PlistFor(cfg, ServiceScope::User);
    REQUIRE(!plist.contains("--daemon"));
}

TEST_CASE("ServiceControl: argv keeps values unquoted", "[platform][service][launchd]")
{
    // A ProgramArguments element is a literal argument, so the quoting that the
    // Windows command line needs would reach the daemon as part of the value.
    FastCache::Config cfg {};
    cfg.serviceName = "My Cache";
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--service-name=My Cache") != argv.end());
}

TEST_CASE("ServiceControl: argv element 0 is the executable", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    auto const argv =
        BuildServiceArgv(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(argv.front() == "/opt/fastcached/bin/fastcached");
}

TEST_CASE("ServiceControl: an unset path flag is omitted rather than absolutized", "[platform][service][launchd]")
{
    // Absolutizing first would turn the empty default into the caller's working
    // directory and pin the service to whatever shell registered it. "Unset" is a
    // command line that did not NAME the flag -- which is the only reading that
    // survives #349, since a path the operator explicitly emptied is an
    // instruction rather than an absence (the case below).
    FastCache::CliResult const cli {};
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cli, EmitDaemonFlag::No);
    REQUIRE(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("--storage="); }));
    REQUIRE(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("--config="); }));
}

TEST_CASE("ServiceControl: an explicitly emptied path is registered, not absolutized", "[platform][service]")
{
    // `ParseText` never fails, so `--storage=` is a reachable instruction and it
    // means something: with `--config` naming a file that carries `storage_path:`,
    // a CLI value outranks the file, so the operator has turned persistence OFF.
    // Deciding this row by presence dropped the flag, let the file win at every
    // start, and left the daemon persisting to disk -- #349's shape in the rows the
    // first pass had excused as safe.
    //
    // Emitted verbatim, because `std::filesystem::absolute("")` is the installing
    // shell's working directory: absolutizing here would register a cache location
    // nobody named.
    auto const args = std::array<char const*, 2> { "--install-service", "--storage=" };
    auto const parsed = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->storagePathExplicit);
    REQUIRE(parsed->config.storagePath.empty());

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, *parsed, EmitDaemonFlag::No);
    CHECK(std::ranges::contains(argv, "--storage="));

    // And it round-trips: the daemon reading that back arrives at the empty value
    // the operator named, rather than at a directory.
    auto const replay = std::array<char const*, 1> { "--storage=" };
    auto const reparsed = FastCache::ParseCli(std::span<char const* const> { replay });
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->config.storagePath.empty());
}

TEST_CASE("ServiceControl: a registered duration is read back by the daemon as the same length", "[platform][service]")
{
    // #1402. A registration replays its command line forever, so a duration is written in the
    // grammar the flag reads, and only the daemon's own parser can say it was. WHAT
    // DISTINGUISHES: `1500ms`, which no larger unit divides, and `2min`, which one does. A
    // registration writing the bare count re-parses as "names no unit" for both; one rounding to
    // a coarser unit loses the first.
    for (auto const text: { std::string_view { "1500ms" }, std::string_view { "2min" } })
    {
        INFO("typed: " << text);
        auto const flag = std::format("--expiry-interval={}", text);
        auto const args = std::array<char const*, 1> { flag.c_str() };
        auto const typed = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(typed.has_value());

        std::string token;
        for (auto const& arg: BuildServiceArgv(std::filesystem::path { "fastcached" }, *typed, EmitDaemonFlag::No))
            if (arg.starts_with("--expiry-interval="))
                token = arg;
        REQUIRE(!token.empty());

        auto const replay = std::array<char const*, 1> { token.c_str() };
        auto const reparsed = FastCache::ParseCli(std::span<char const* const> { replay });
        REQUIRE(reparsed.has_value());
        CHECK(reparsed->config.activeExpiryInterval == typed->config.activeExpiryInterval);
        CHECK(reparsed->activeExpiryIntervalExplicit);
    }
}

TEST_CASE("ServiceControl: the launchd label is reverse-DNS and lowercased", "[platform][service][launchd]")
{
    FastCache::Config cfg {};
    REQUIRE(FastCache::LaunchdLabel(SpecFor("fastcached", cfg)) == "software.lastrada.fastcached");

    cfg.serviceName = "FastCachedSmoke";
    REQUIRE(FastCache::LaunchdLabel(SpecFor("fastcached", cfg)) == "software.lastrada.fastcachedsmoke");
}

TEST_CASE("ServiceControl: the SCM logon identity is named, not implied", "[platform][service][scm]")
{
    // Naming nobody is LocalSystem: unrestricted access to every local resource and
    // a member of the local Administrators group. Neither of this project's services
    // has any use for that, so neither leaves it to the default.
    FastCache::Config const cfg {};
    auto const daemon = SpecFor("fastcached", cfg);
    REQUIRE(daemon.windowsLogon == FastCache::WindowsLogonAccount::VirtualAccount);
    REQUIRE(Unwrap(FastCache::WindowsLogonName(daemon)) == "NT SERVICE\\FastCached");

    // A virtual account is derived from the service name by the SCM itself, so the
    // spelling has to match the name exactly or the service logs on as nobody.
    FastCache::ServiceSpec worker {};
    worker.serviceName = "FastCacheCompileNode";
    worker.windowsLogon = FastCache::WindowsLogonAccount::VirtualAccount;

    auto const name = FastCache::WindowsLogonName(worker);
    REQUIRE(name.has_value());
    REQUIRE(Unwrap(name) == "NT SERVICE\\FastCacheCompileNode");

    // It follows --service-name, because that is what the SCM derives it from: a
    // fixed string here would name an identity a renamed service does not have.
    worker.serviceName = "FastCacheCompileNodeSmoke";
    REQUIRE(Unwrap(FastCache::WindowsLogonName(worker)) == "NT SERVICE\\FastCacheCompileNodeSmoke");
}

TEST_CASE("ServiceControl: a system job's log directory is its own", "[platform][service][launchd]")
{
    // InstallService hands this directory to the account the job runs as, and a
    // machine may run both fastcached and fastcache-compile-node system-wide.
    // One shared directory meant each install chowned the other's to itself:
    // registering the worker took the daemon's, and a package reinstall took it
    // back.
    auto const daemon = FastCache::DefaultLogDirectory("software.lastrada.fastcached", ServiceScope::System, "/Users/jo");
    auto const worker =
        FastCache::DefaultLogDirectory("software.lastrada.fastcachecompilenode", ServiceScope::System, "/Users/jo");

    REQUIRE(daemon != worker);
    REQUIRE(daemon.filename() == "software.lastrada.fastcached");
    // A machine-wide job must not reach into anybody's home directory.
    REQUIRE(!daemon.string().contains("/Users/jo"));

    // A per-user agent is not chowned and its files are already label-named, so
    // it keeps the flat directory an operator may have open in `tail -f`.
    auto const agent = FastCache::DefaultLogDirectory("software.lastrada.fastcached", ServiceScope::User, "/Users/jo");
    REQUIRE(agent == std::filesystem::path { "/Users/jo" } / "Library/Logs/fastcached");
}

TEST_CASE("ServiceControl: scope defaults are filled in for a file-configured service", "[platform][service][launchd]")
{
    // A parse that named NOTHING, which is what "the operator left this unset"
    // means since #349 -- `AsTyped` would mark `--storage` as named, and a named
    // `--storage=` is an instruction to persist nowhere, which `WithScopeDefaults`
    // must then leave alone. That is asserted as its own section below.
    FastCache::CliResult const cli {};
    auto const spec = SpecFor("fastcached", cli);
    REQUIRE(!spec.applicationName.empty());

    SECTION("a user agent gets a cache under the invoking account's home")
    {
        // Kept in-memory it would lose the whole cache at every logout, which for
        // a compile cache is most of the value -- and launchd expands neither `~`
        // nor `$HOME` in ProgramArguments, so the concrete path has to be resolved
        // at install time.
        auto const filled = WithScopeDefaults(spec, ServiceScope::User, "/Users/jo", {});

        // The tail is spelled out -- `fastcached` in it is the applicationName,
        // which is the point -- while the separator between home and it is left
        // to the platform, because this case runs everywhere and only macOS
        // renders it with a slash.
        auto const expected = std::filesystem::path { "/Users/jo" } / "Library/Caches/fastcached/cache";
        REQUIRE(std::ranges::contains(filled.arguments, std::format("--storage={}", expected.string())));
        REQUIRE(std::ranges::contains(filled.ownedPaths, expected, &FastCache::OwnedPath::path));
    }

    SECTION("a config the operator named is never overridden by a storage default")
    {
        // A CLI value outranks YAML in Merge, so injecting --storage alongside
        // --config would pin the cache location and make every later storage_path
        // edit a silent no-op.
        FastCache::CliResult named {};
        named.config.configPath = "/etc/fastcached/fastcached.yaml";
        auto const filled = WithScopeDefaults(SpecFor("fastcached", named), ServiceScope::User, "/Users/jo", {});

        REQUIRE(std::ranges::none_of(filled.arguments, [](std::string const& a) { return a.starts_with("--storage="); }));
    }

    SECTION("an explicitly emptied --storage is not filled back in")
    {
        // The other way an operator says "do not persist", and it has to survive the
        // same way a named path does: `--install-service --storage=` asks for a
        // memory-only service, and quietly handing it a cache directory would give
        // them one that persists at every start. `HasArgument` asks the argument
        // list, so this works for exactly the reason the section above does -- and
        // only because #349 made the flag reach that list at all.
        FastCache::CliResult emptied {};
        emptied.storagePathExplicit = true;
        auto const filled = WithScopeDefaults(SpecFor("fastcached", emptied), ServiceScope::User, "/Users/jo", {});

        REQUIRE(std::ranges::contains(filled.arguments, "--storage="));
        REQUIRE(std::ranges::none_of(filled.arguments,
                                     [](std::string const& a) { return a.starts_with("--storage=") && a.size() > 10; }));
        REQUIRE(filled.ownedPaths.empty());
    }

    SECTION("a system daemon is pointed at the packaged config")
    {
        auto const filled =
            WithScopeDefaults(spec, ServiceScope::System, "/Users/jo", "/opt/fastcached/etc/fastcached.yaml");

        REQUIRE(std::ranges::contains(filled.arguments, "--config=/opt/fastcached/etc/fastcached.yaml"));
        // ServiceAccountReadDenial validates this, so leaving it empty would
        // demote an install-time error to a silent fall-through to defaults.
        REQUIRE(filled.configPath == "/opt/fastcached/etc/fastcached.yaml");
    }

    SECTION("an absent packaged config points launchd at nothing")
    {
        auto const filled = WithScopeDefaults(spec, ServiceScope::System, "/Users/jo", {});

        REQUIRE(std::ranges::none_of(filled.arguments, [](std::string const& a) { return a.starts_with("--config="); }));
    }
}

TEST_CASE("ServiceControl: each scope default is accepted on its own", "[platform][service][launchd]")
{
    // **The mechanical guard for [#396](https://github.com/LASTRADA-Software/fastcached/issues/396),
    // and it is mechanical for one reason: naming the two flags by hand passes
    // under the very defect.** `applicationName` used to decide both defaults, so
    // a spec that had files got `--config` AND `--storage`. A case asserting "the
    // daemon receives both" is true before and after; a case asserting "a spec
    // accepting only `--config` receives no `--storage`" cannot even be WRITTEN
    // against one bit, because there was no way to say it.
    //
    // So the walk is over `ScopeDefaultTable()` and the assertion is per row: the
    // row's own flag arrives, and **no other row's does**. A third default is
    // covered with no edit here, which is the property this repository keeps
    // finding it lacks -- the same argument as `RowsInEnumeratorOrder`.
    REQUIRE(!FastCache::ScopeDefaultTable().empty());
    // Two rows minimum, or "no other row's flag" is vacuous and the whole case
    // passes by having nothing to compare against.
    REQUIRE(FastCache::ScopeDefaultTable().size() >= 2);

    // **Both scopes, and that is not thoroughness -- it is what makes the walk
    // mean anything.** The first version of this case had the right SHAPE -- it
    // walked the table rather than naming two flags -- and drove each row at its
    // OWN scope only. It PASSED against the one-bit shape: the two defaults apply
    // in opposite scopes, so the other row's flag could not have appeared there
    // whatever decided it.
    //
    // So **deriving a guard is necessary and not sufficient, and only the
    // counterfactual establishes which one you have.** The same sentence as
    // `assert count == 1` being necessary and not sufficient. Nothing about the
    // first version looked wrong; running it against the defect is what said so.
    for (auto const& row: FastCache::ScopeDefaultTable())
    {
        INFO("accepted default: " << row.flag);

        // Accepting exactly ONE default. Everything else about the spec is what a
        // file-configured service looks like, because that is the shape the old
        // bit could not distinguish.
        FastCache::ServiceSpec spec {};
        spec.serviceName = "OneDefault";
        spec.applicationName = "one-default";
        spec.acceptedScopeDefaults = FastCache::ScopeDefaults({ row.which });

        for (auto const scope: { ServiceScope::User, ServiceScope::System })
        {
            auto const filled = WithScopeDefaults(spec, scope, "/Users/jo", "/opt/fastcached/etc/fastcached.yaml");
            auto const carries = [&filled](std::string_view flag) {
                return std::ranges::any_of(filled.arguments,
                                           [flag](std::string const& arg) { return arg.starts_with(flag); });
            };

            // The accepted one arrives, in its own scope and only there.
            CHECK(carries(row.flag) == (scope == row.scope));

            // And a default this spec did not accept is filled in nowhere. This is
            // the half the one-bit shape fails, and it fails it at the OTHER row's
            // scope -- which is why the scope loop is here rather than in a section
            // of its own.
            for (auto const& other: FastCache::ScopeDefaultTable())
            {
                if (other.which == row.which)
                    continue;
                INFO("must never carry: " << other.flag);
                CHECK_FALSE(carries(other.flag));
            }
        }
    }

    SECTION("a default this service does not accept is never filled in, at either scope")
    {
        // The converse of the walk above, and the clause that keeps a registration
        // survivable by the binary it registers: a flag the parser rejects turns
        // into "unrecognised argument" at every start.
        for (auto const& row: FastCache::ScopeDefaultTable())
        {
            INFO("refused default: " << row.flag);
            FastCache::ServiceSpec spec {};
            spec.serviceName = "RefusesOne";
            spec.applicationName = "refuses-one";
            // Every default EXCEPT this row's, so the spec is otherwise as
            // permissive as it can be -- a spec accepting nothing would pass this
            // for the wrong reason.
            auto accepted = FastCache::ScopeDefaults({});
            for (auto const& other: FastCache::ScopeDefaultTable())
                if (other.which != row.which)
                    accepted[static_cast<std::size_t>(other.which)] = true;
            spec.acceptedScopeDefaults = accepted;

            for (auto const scope: { ServiceScope::User, ServiceScope::System })
            {
                auto const filled = WithScopeDefaults(spec, scope, "/Users/jo", "/opt/fastcached/etc/fastcached.yaml");
                CHECK(std::ranges::none_of(filled.arguments,
                                           [&row](std::string const& arg) { return arg.starts_with(row.flag); }));
            }
        }
    }

    SECTION("a service naming no application derives nothing, whatever it accepts")
    {
        // The one thing `applicationName` still decides, and it is a property of
        // the VALUE rather than of the parser: both defaults are looked up UNDER
        // that name, so there is nothing to derive without one. Accepting
        // everything here is the point -- if the set alone decided, a
        // `--storage=<home>/Library/Caches//cache` would be appended, which is a
        // path nobody asked for rather than a refusal.
        FastCache::ServiceSpec spec {};
        spec.serviceName = "NoApplication";
        spec.applicationName = {};
        auto accepted = FastCache::ScopeDefaults({});
        for (auto const& row: FastCache::ScopeDefaultTable())
            accepted[static_cast<std::size_t>(row.which)] = true;
        spec.acceptedScopeDefaults = accepted;

        for (auto const scope: { ServiceScope::User, ServiceScope::System })
        {
            auto const filled = WithScopeDefaults(spec, scope, "/Users/jo", "/opt/fastcached/etc/fastcached.yaml");
            CHECK(filled.arguments.empty());
            CHECK(filled.configPath.empty());
            CHECK(filled.ownedPaths.empty());
        }
    }

    SECTION("the daemon is the service that accepts both")
    {
        // Named rather than inferred: the daemon accepting both is what made one
        // bit look sufficient, so it is worth pinning that it still does -- and
        // that the guard above is not passing because nothing accepts more than
        // one default.
        auto const daemon = SpecFor("fastcached", FastCache::CliResult {});
        for (auto const& row: FastCache::ScopeDefaultTable())
        {
            INFO("default: " << row.flag);
            CHECK(FastCache::AcceptsScopeDefault(daemon.acceptedScopeDefaults, row.which));
        }
    }
}

TEST_CASE("ServiceControl: a service that keeps no files is given no path flags", "[platform][service][launchd]")
{
    // The registration has to survive the registered binary's OWN parser.
    // `fastcache-compile-node` is configured entirely from argv and accepts
    // neither flag, so a default baked in here produced a job that answered its
    // own command line with "unrecognised argument" at every start -- registered,
    // reported installed, and dead at every boot. The application name was
    // hardcoded to the daemon's, so every spec got the daemon's defaults.
    //
    // Asserted against a bare spec rather than the worker's, because this file
    // must not depend on an app target: what is being pinned is the rule, and
    // NodeConfig_test.cpp pins that the worker actually claims it.
    FastCache::ServiceSpec argvOnly {};
    argvOnly.serviceName = "ArgvOnly";
    argvOnly.applicationName = {};

    for (auto const scope: { ServiceScope::User, ServiceScope::System })
    {
        auto const filled = WithScopeDefaults(argvOnly, scope, "/Users/jo", "/opt/fastcached/etc/fastcached.yaml");

        CHECK(filled.arguments.empty());
        CHECK(filled.configPath.empty());
        CHECK(filled.ownedPaths.empty());
    }
}

TEST_CASE("ServiceControl: the plist path follows the scope", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    auto const user = FastCache::LaunchdPlistPath(SpecFor("fastcached", cfg), ServiceScope::User, "/Users/jo");
    auto const system = FastCache::LaunchdPlistPath(SpecFor("fastcached", cfg), ServiceScope::System, "/Users/jo");

    REQUIRE(user == std::filesystem::path { "/Users/jo/Library/LaunchAgents/software.lastrada.fastcached.plist" });
    REQUIRE(system == std::filesystem::path { "/Library/LaunchDaemons/software.lastrada.fastcached.plist" });
    // The system daemon is machine-wide; a home directory must not leak into it.
    REQUIRE(!system.string().contains("/Users/jo"));
}

TEST_CASE("A refused start is not restarted by launchd in either scope", "[platform][service][launchd][recovery]")
{
    // launchd has no retry count, so every KeepAlive shape that restarts a clean non-zero exit
    // restarts a refused start forever, once per ThrottleInterval. The system daemon's `<true/>`
    // did exactly that. Both scopes now render the one bounded shape: restart on a crash only.
    FastCache::Config const cfg {};
    constexpr std::string_view CrashOnly = "    <key>KeepAlive</key>\n    <dict>\n"
                                           "        <key>Crashed</key>\n        <true/>\n"
                                           "    </dict>\n";
    for (auto const scope: { ServiceScope::User, ServiceScope::System })
    {
        INFO("scope " << FastCache::ServiceScopeName(scope));
        auto const plist = PlistFor(cfg, scope);
        CHECK(plist.contains(CrashOnly));
        CHECK_FALSE(plist.contains("<key>KeepAlive</key>\n    <true/>"));
        CHECK_FALSE(plist.contains("<key>SuccessfulExit</key>"));
        // And a crash loop is still throttled rather than immediate.
        CHECK(plist.contains("    <key>ThrottleInterval</key>\n    <integer>30</integer>\n"));
    }
}

TEST_CASE("Every shipped systemd unit restarts a failure and never a refusal", "[platform][service][systemd][recovery]")
{
    // A refusal leaves the process as its own code (`ProcessExit::Refused`), so systemd can be told
    // not to restart it and every other failure keeps `Restart=on-failure`. Read off the files the
    // packages install -- every `*.service` under packaging/linux, so a new unit is held to it.
    //
    // The codes a unit exempts are DERIVED from the table: every row a START can end with that a
    // supervisor does not restart, except the clean exit `on-failure` already leaves alone. A one-shot
    // command's usage exit is no start's, and no unit runs one.
    std::string exempted;
    for (auto const& row: FastCache::ProcessExitRows)
        if (row.endsAStart && !row.restarted && row.code != 0)
            exempted += std::format("{}{}", exempted.empty() ? "" : " ", row.code);
    REQUIRE_FALSE(exempted.empty());
    auto const prevent = std::format("\nRestartPreventExitStatus={}\n", exempted);

    auto const directory = std::filesystem::path { FASTCACHED_SOURCE_DIR } / "packaging" / "linux";
    REQUIRE(std::filesystem::is_directory(directory));
    auto units = std::size_t { 0 };
    for (auto const& entry: std::filesystem::directory_iterator { directory })
    {
        if (entry.path().extension() != ".service")
            continue;
        ++units;
        INFO(entry.path().filename().string());
        auto const text = ReadWholeFile(entry.path());
        auto const serviceSection = text.find("[Service]\n");
        REQUIRE(serviceSection != std::string::npos);
        // Both keys belong to [Service]; systemd ignores them, with only a warning, elsewhere.
        auto const service = std::string_view { text }.substr(serviceSection);
        CHECK(service.contains(prevent));
        CHECK(service.contains("\nRestart=on-failure\n"));
        // No start limit of the unit's own: one counted an operator's restarts with the refusals,
        // and the fourth `systemctl restart` in ten minutes left the service stopped (measured).
        CHECK_FALSE(text.contains("StartLimitBurst="));
        CHECK_FALSE(text.contains("StartLimitIntervalSec="));
    }
    CHECK(units == 3);
}

TEST_CASE("ServiceControl: only the system job runs as the service account", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    REQUIRE(PlistFor(cfg, ServiceScope::System).contains("<key>UserName</key>"));
    REQUIRE(PlistFor(cfg, ServiceScope::System).contains("_fastcached"));
    // A LaunchAgent already runs as the logged-in user; naming a UserName it
    // cannot assume makes launchd refuse the job.
    REQUIRE(!PlistFor(cfg, ServiceScope::User).contains("<key>UserName</key>"));

    // GroupName is deliberately absent even for the system job: launchd already
    // uses the account's primary group when it is omitted, so it names a second
    // thing to resolve for no gain. When that resolution failed, the job did not
    // fail with it -- launchd left it in "spawn scheduled" forever and the
    // kickstart waiting on the spawn hung until the installer killed the script.
    REQUIRE(!PlistFor(cfg, ServiceScope::System).contains("<key>GroupName</key>"));
}

TEST_CASE("ServiceControl: resource policy keys are always present", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    for (auto const scope: { ServiceScope::User, ServiceScope::System })
    {
        auto const plist = PlistFor(cfg, scope);
        // Without ProcessType launchd applies its "Background" band and throttles
        // CPU and I/O; without the limit the job inherits a 256-descriptor soft
        // cap, far below what a connection-per-client server needs.
        REQUIRE(plist.contains("<key>ProcessType</key>"));
        REQUIRE(plist.contains("<string>Interactive</string>"));
        REQUIRE(plist.contains("<key>NumberOfFiles</key>"));
        REQUIRE(plist.contains("<key>RunAtLoad</key>"));
    }
}

TEST_CASE("ServiceControl: plist values are XML-escaped", "[platform][service][launchd]")
{
    // `&` is legal in a macOS path. Unescaped it produces a malformed document
    // and launchd rejects the whole job without explaining why.
    FastCache::Config cfg {};
    cfg.storagePath = "/tmp/a&b<c>d/cache";
    auto const plist = PlistFor(cfg, ServiceScope::User);

    REQUIRE(plist.contains("a&amp;b&lt;c&gt;d"));
    REQUIRE(!plist.contains("a&b<c>d"));
}

namespace
{
/// A spec whose sole launch argument is exactly @p bytes.
///
/// An ARGUMENT, which is a `std::string`, and that is the whole reason these cases
/// reach the plist through one rather than through `exePath` or `storagePath`: those
/// are `std::filesystem::path` (or are absolutized into one), and this tree's own rule
/// records that `path`'s narrow constructor THROWS on a Windows host for bytes that
/// are not UTF-8 -- before any `error_code` overload runs. A case supplying such bytes
/// through a path would therefore die in the fixture on three of the CI legs and never
/// reach the function under test. An argument is one of the four fields #1357 names,
/// so nothing is given up by going this way.
///
/// ONE field, not two. This fixture also set `serviceAccount`, which was harmless for
/// the assertions and wrong for attribution: reverting the account-name fix then
/// failed all three cases built on it rather than the one case about the account, so
/// "the failures are the ones I expect and only those" could not be read. Measured by
/// doing exactly that revert. The account has its own case, which sets the field
/// itself.
/// @param bytes The value to put in the argument.
/// @return The spec.
[[nodiscard]] FastCache::ServiceSpec SpecCarrying(std::string bytes)
{
    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, FastCache::Config {});
    spec.arguments = std::vector<std::string> { std::move(bytes) };
    return spec;
}
} // namespace

TEST_CASE("ServiceControl: a plist field that is not UTF-8 is replaced, not carried", "[platform][service][launchd]")
{
    // #1357. macOS paths are BYTES, so an installation under a path carrying a byte
    // that belongs to no UTF-8 sequence is unusual and legal -- and the five-arm
    // entity switch this function used to escape with wrote that byte verbatim.
    //
    // WHAT DISTINGUISHES: a case over an ordinary ASCII path passes under the defect,
    // which is why this one supplies the bytes and asserts the DOCUMENT. `0x80` is a
    // lone continuation byte -- valid in no UTF-8 sequence in any position -- and it
    // is not a control character in any locale, which keeps this case about ENCODING
    // rather than about the control-character rule below.
    auto const replacement = std::string { FastCache::MarkupReplacement };
    auto const plist = FastCache::BuildLaunchdPlist(SpecCarrying("gcc\x80-1"), ServiceScope::System, "/tmp/logs");

    // The property, stated as the property: whatever it was given, what comes out is
    // a document a parser will accept. Under the defect this is FALSE -- the raw 0x80
    // made the whole plist not UTF-8.
    REQUIRE(FastCache::IsValidUtf8(plist));
    REQUIRE(!plist.contains("\x80"));

    // And the bytes around it are untouched, so this is a replacement rather than a
    // field that got dropped.
    REQUIRE(plist.contains("gcc" + replacement + "-1"));

    // A TRUNCATED sequence is the other invalid shape, and it does not collapse to one
    // replacement: `0xC3` is a two-byte lead, `0x28` is not a continuation, so the lead
    // decodes to nothing and is replaced ALONE while the `(` is ordinary text that
    // survives. That is `EscapeMarkup`'s documented rule -- a code point the production
    // merely excludes is consumed whole, but bytes that decoded to nothing advance
    // singly -- and asserting it here is what stops somebody "simplifying" the walk
    // into one replacement per invalid RUN, which would silently merge a mangled path
    // with the character after it.
    auto const truncated = FastCache::BuildLaunchdPlist(SpecCarrying("gcc\xC3\x28-1"), ServiceScope::System, "/tmp/logs");
    REQUIRE(FastCache::IsValidUtf8(truncated));
    REQUIRE(truncated.contains("gcc" + replacement + "(-1"));
}

TEST_CASE("ServiceControl: a C0 control XML forbids is replaced, and the three it allows are kept",
          "[platform][service][launchd]")
{
    // The sharper half of #1357, and the half escaping cannot fix: XML 1.0 forbids
    // 0x01-0x08, 0x0B, 0x0C and 0x0E-0x1F **outright**, so `&#x1;` is as unparseable
    // as the raw byte and there is nothing to escape a control TO. Substituting is
    // the only move available.
    auto const plist = FastCache::BuildLaunchdPlist(SpecCarrying("a\x01"
                                                                 "b"),
                                                    ServiceScope::System,
                                                    "/tmp/logs");

    // WHAT DISTINGUISHES: the raw byte is what the defect wrote, and the character
    // reference is what a reader would reach for instead -- neither may appear.
    REQUIRE(!plist.contains("\x01"));
    REQUIRE(!plist.contains("&#x1;"));
    REQUIRE(!plist.contains("&#x01;"));
    REQUIRE(plist.contains("a" + std::string { FastCache::MarkupReplacement } + "b"));

    // The control bytes the production DOES admit are carried, or this would be a
    // stricter rule than XML's wearing XML's name. A tab in a launch argument is
    // legal in the document; whether it is a sensible thing to register is
    // `SupervisorTextRejection`'s question, and it does not refuse these either.
    auto const tabbed = FastCache::BuildLaunchdPlist(SpecCarrying("a\tb"), ServiceScope::System, "/tmp/logs");
    REQUIRE(tabbed.contains("a\tb"));
    REQUIRE(!FastCache::SupervisorTextRejection(SpecCarrying("a\tb")).has_value());
}

TEST_CASE("ServiceControl: the account name is escaped like every other text node", "[platform][service][launchd]")
{
    // Found while fixing #1357 and fixed with it: `UserName` was interpolated with no
    // escaping at all, so it was a second route to the malformed document the rest of
    // this function's escaping exists to prevent. `serviceAccount` is a FIELD rather
    // than the constant it used to be -- its own comment says a second binary may
    // want a different one -- so "the daemon's account is always `_fastcached`" was
    // never a property of the function.
    //
    // WHAT DISTINGUISHES: `&` is the byte whose unescaped presence makes the document
    // malformed, and under the defect it appeared raw.
    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, FastCache::Config {});
    spec.serviceAccount = "cache&co";
    auto const plist = FastCache::BuildLaunchdPlist(spec, ServiceScope::System, "/tmp/logs");

    REQUIRE(plist.contains("<string>cache&amp;co</string>"));
    REQUIRE(!plist.contains("<string>cache&co</string>"));
}

TEST_CASE("ServiceControl: a registration carrying text no supervisor can record is refused", "[platform][service][launchd]")
{
    // The other half of #1357, and the half the ticket's acceptance clause offered as
    // an alternative to substitution. It is BOTH here, because substitution alone
    // keeps the document parseable while leaving it naming a path that does not
    // exist: `--install-service` would print success and the job would fail on every
    // boot with nothing to diagnose it by. That is the same outcome as the malformed
    // plist, one step further on, and this project's rule for registrations is to
    // refuse while somebody is watching.
    //
    // WHAT DISTINGUISHES: each arm names a byte NO EXISTING RULE refuses.
    // `ServiceNameRejection` tests `serviceName` alone and tests it with
    // `std::iscntrl`, which answers false for a UTF-8 encoding error -- so both arms
    // below returned nullopt before this rule existed.
    REQUIRE(FastCache::SupervisorTextRejection(SpecCarrying("gcc\xC3\x28-1")).has_value());
    REQUIRE(FastCache::LaunchdTextRejection(SpecCarrying("a\x01"
                                                         "b"))
                .has_value());

    // Encoding is asked of the service name too, which `ServiceNameRejection` does
    // not do: it answers a path-traversal question and is also called on its own from
    // the uninstall and status paths, so neither rule subsumes the other.
    FastCache::Config encoded {};
    encoded.serviceName = "fast\xC3\x28"
                          "cached";
    REQUIRE(FastCache::SupervisorTextRejection(SpecFor("fastcached", encoded)).has_value());

    // The rule is reachable through the gate both platforms' InstallService share,
    // rather than being a function nothing calls.
    REQUIRE(FastCache::ServiceRegistrationRejection(SpecCarrying("gcc\xC3\x28-1"), FastCache::SupervisorKind::Launchd)
                .has_value());
    REQUIRE(
        FastCache::ServiceRegistrationRejection(SpecCarrying("gcc\xC3\x28-1"), FastCache::SupervisorKind::Scm).has_value());

    // THE POSITIVE CONTROL, which is the direction a refusal test skips: a guard
    // nobody has watched ACCEPT is not known to work either, and a rule that refused
    // everything would pass every assertion above.
    REQUIRE(!FastCache::SupervisorTextRejection(SpecCarrying("--storage=/var/db/fc")).has_value());
    REQUIRE(!FastCache::SupervisorTextRejection(SpecCarrying("caf\xC3\xA9")).has_value()); // U+00E9, legal text
    REQUIRE(!FastCache::LaunchdTextRejection(SpecCarrying("caf\xC3\xA9")).has_value());
    for (auto const supervisor: { FastCache::SupervisorKind::Scm, FastCache::SupervisorKind::Launchd })
        REQUIRE(
            !FastCache::ServiceRegistrationRejection(SpecFor("fastcached", FastCache::Config {}), supervisor).has_value());
}

TEST_CASE("ServiceControl: a code point only XML forbids is refused for launchd and not for the SCM", "[platform][service]")
{
    // The scope of the rule IS the subject here. `SupervisorTextRejection` went into
    // the gate BOTH platforms' InstallService calls while asking XML's `Char`
    // production -- so `--display-name $'Fast\x0bCache'` was refused on WINDOWS, where
    // the SCM stores UTF-16 and carries a vertical tab perfectly well, under a message
    // about property lists and launchd. A registration replays forever, so that is a
    // machine that cannot be provisioned until somebody reads a message about an
    // operating system they are not running.
    //
    // WHAT DISTINGUISHES: the SAME spec, asked twice, answering differently. A case
    // that only asserted the launchd refusal passes under the defect, and so does one
    // that only asserts the bytes are valid UTF-8 -- they are.
    FastCache::Config cfg {};
    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, cfg);
    spec.displayName = "Fast\x0b"
                       "Cache";
    REQUIRE(FastCache::IsValidUtf8(spec.displayName)); // the premise: this is text.

    CHECK(FastCache::ServiceRegistrationRejection(spec, FastCache::SupervisorKind::Launchd).has_value());
    CHECK(!FastCache::ServiceRegistrationRejection(spec, FastCache::SupervisorKind::Scm).has_value());

    // U+FFFE is the other half of the `Char` production's exclusions and the half a
    // byte-wise check cannot see: perfectly good UTF-8, and still not carriable.
    auto noncharacter = spec;
    noncharacter.displayName = "Fast\xEF\xBF\xBE"
                               "Cache";
    REQUIRE(FastCache::IsValidUtf8(noncharacter.displayName));
    CHECK(FastCache::ServiceRegistrationRejection(noncharacter, FastCache::SupervisorKind::Launchd).has_value());
    CHECK(!FastCache::ServiceRegistrationRejection(noncharacter, FastCache::SupervisorKind::Scm).has_value());

    // And the half that is NOT about a document format is asked of both, because this
    // process's `char` is UTF-8 everywhere it ships: bytes that are not text are not
    // text for either supervisor, and a registration records them forever.
    auto mangled = spec;
    mangled.displayName = "Fast\xC3\x28"
                          "Cache";
    REQUIRE(!FastCache::IsValidUtf8(mangled.displayName));
    for (auto const supervisor: { FastCache::SupervisorKind::Scm, FastCache::SupervisorKind::Launchd })
        CHECK(FastCache::ServiceRegistrationRejection(mangled, supervisor).has_value());
}

TEST_CASE("ServiceControl: a refusal names WHICH launch argument carries the byte", "[platform][service]")
{
    // Every argument was labelled "a launch argument" and the offset is relative to
    // that argument, so with six `ProgramArguments` an operator was told *a launch
    // argument carries a byte at offset 3* and could not tell which one. The index is
    // what makes it actionable; the flag name is taken only when it is safe to print.
    //
    // WHAT DISTINGUISHES: the bad byte is in the THIRD argument, so a refusal naming
    // no index reads identically whichever argument carries it.
    FastCache::Config cfg {};
    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, cfg);
    spec.arguments = std::vector<std::string> { "--port=11211", "--threads=4", "--storage=/var/db/f\x01i", "--metrics" };

    auto const rejection = FastCache::LaunchdTextRejection(spec);
    REQUIRE(rejection.has_value());
    auto const& refusal = Unwrap(rejection);
    CHECK(refusal.contains("launch argument 3"));
    CHECK(refusal.contains("(--storage)"));
    // The offset is within that argument, so it points at the byte rather than at a
    // position in a command line nobody assembled.
    CHECK(refusal.contains("offset 19"));

    // The flag hint is dropped rather than guessed when the argument is not a flag,
    // and the index still identifies it.
    auto positional = spec;
    positional.arguments = std::vector<std::string> { "--metrics", "plain\x01value" };
    auto const bare = FastCache::LaunchdTextRejection(positional);
    REQUIRE(bare.has_value());
    // The label runs straight into the verb with no hint between them. Asserted that
    // way rather than as "no `(` anywhere": every refusal parenthesises the byte it
    // names, so a bare-`(` assertion cannot fail for the reason it was written for.
    CHECK(Unwrap(bare).contains("launch argument 2 carries"));
}

TEST_CASE("ServiceControl: non-default flags reach ProgramArguments", "[platform][service][launchd]")
{
    FastCache::Config cfg {};
    cfg.port = 21987;
    cfg.maxMemoryBytes = 128U * 1024U * 1024U;
    auto const plist = PlistFor(cfg, ServiceScope::User);

    REQUIRE(plist.contains("<string>--port=21987</string>"));
    REQUIRE(plist.contains("<string>--max-memory=134217728</string>"));
}

TEST_CASE("ServiceControl: the plist is a well-formed document", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    auto const plist = PlistFor(cfg, ServiceScope::System);

    REQUIRE(plist.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    REQUIRE(plist.contains("<!DOCTYPE plist PUBLIC"));
    REQUIRE(plist.contains("<plist version=\"1.0\">"));
    REQUIRE(plist.ends_with("</plist>\n"));

    // Every opened element is closed.
    auto const count = [&plist](std::string_view needle) {
        std::size_t n = 0;
        auto pos = plist.find(needle);
        while (pos != std::string::npos)
        {
            ++n;
            pos = plist.find(needle, pos + 1);
        }
        return n;
    };
    REQUIRE(count("<dict>") == count("</dict>"));
    REQUIRE(count("<array>") == count("</array>"));
}

TEST_CASE("ServiceControl: service scope round-trips through its CLI spelling", "[platform][service][launchd]")
{
    REQUIRE(FastCache::ParseServiceScope("user").value() == ServiceScope::User);
    REQUIRE(FastCache::ParseServiceScope("system").value() == ServiceScope::System);

    REQUIRE(FastCache::ServiceScopeName(ServiceScope::User) == "user");
    REQUIRE(FastCache::ServiceScopeName(ServiceScope::System) == "system");
}

TEST_CASE("ServiceControl: an unknown service scope is rejected", "[platform][service][launchd]")
{
    // Case-sensitive on purpose: every other enum flag in the CLI is, and
    // accepting "System" here would make the parser inconsistent.
    for (auto const* const bad: { "System", "", "root", "daemon", "agent" })
        REQUIRE(!FastCache::ParseServiceScope(bad).has_value());

    auto const err = FastCache::ParseServiceScope("nope").error();
    // No field: `--service-scope` stamps its own spelling (CliParser_test asks the row).
    REQUIRE(err.field.empty());
    REQUIRE(err.context.contains("nope"));
}

TEST_CASE("ServiceControl: security-relevant flags reach the supervisor", "[platform][service]")
{
    // The table used to stop after nine fields, so `--install-service --tls
    // --metrics ...` reported success and registered a plaintext, unmonitored
    // daemon. Every flag an operator can type alongside --install-service has
    // to survive the trip, or the success message is a lie.
    FastCache::Config cfg {};
    cfg.tlsEnabled = true;
    cfg.tlsCertPath = "/etc/fastcached/server.crt";
    cfg.tlsKeyPath = "/etc/fastcached/server.key";
    cfg.metricsEnabled = true;
    cfg.metricsBindAddress = "0.0.0.0";
    cfg.metricsPort = 9999;
    cfg.pidfile = "/var/run/fastcached.pid";
    cfg.notifyKeyspaceEvents = "KEA";

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    auto const has = [&argv](std::string_view flag) {
        return std::ranges::find(argv, flag) != argv.end();
    };

    // Path flags are compared against what absolute() makes of them, not
    // against the literal spelling: a POSIX-looking path carries no drive
    // letter, so on Windows it is *relative* and gets rebased onto the current
    // drive. Asserting the literal passes on macOS and fails on Windows for a
    // reason that has nothing to do with the flag being carried.
    auto const absolute = [](std::string_view path) {
        return std::filesystem::absolute(path).string();
    };

    REQUIRE(has(std::format("--tls-cert={}", absolute("/etc/fastcached/server.crt"))));
    REQUIRE(has(std::format("--tls-key={}", absolute("/etc/fastcached/server.key"))));
    REQUIRE(has(std::format("--pidfile={}", absolute("/var/run/fastcached.pid"))));
    REQUIRE(has("--metrics-bind=0.0.0.0"));
    REQUIRE(has("--metrics-port=9999"));
    REQUIRE(has("--notify-keyspace-events=KEA"));

    // Valueless switches: `--tls=true` is not a spelling CliParser accepts, so
    // emitting one would produce a service that refuses to start.
    REQUIRE(has("--tls"));
    REQUIRE(has("--metrics"));
}

TEST_CASE("ServiceControl: every listener is re-emitted", "[platform][service]")
{
    // One token per bind, TLS-tagged individually: a multi-endpoint daemon that
    // came back listening on fewer ports than it was installed with would look
    // like a network fault, not a packaging bug.
    FastCache::Config cfg {};
    cfg.binds = { { .address = "127.0.0.1", .port = 11211, .tls = false },
                  { .address = "0.0.0.0", .port = 11212, .tls = true } };

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--listen=127.0.0.1:11211") != argv.end());
    REQUIRE(std::ranges::find(argv, "--listen-tls=0.0.0.0:11212") != argv.end());
}

TEST_CASE("ServiceControl: a password is never written into the launch arguments", "[platform][service]")
{
    // Launch arguments land in a world-readable plist (or the SCM's ImagePath),
    // so emitting the secret would publish it to the very accounts
    // --requirepass exists to exclude.
    FastCache::Config cfg {};
    cfg.requirePass = "hunter2";

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::none_of(argv, [](std::string const& a) { return a.contains("hunter2"); }));

    auto const plist = PlistFor(cfg, ServiceScope::System);
    REQUIRE(!plist.contains("hunter2"));
}

TEST_CASE("ServiceControl: dropping a password is reported, not silent", "[platform][service]")
{
    // The alternative to refusing is an install that comes up unauthenticated
    // while printing "installed and started" — the failure mode this guards.
    FastCache::Config cfg {};
    cfg.requirePass = "hunter2";
    auto const rejection = FastCache::InlineCredentialRejection(SpecFor("fastcached", cfg));
    REQUIRE(rejection.has_value());
    REQUIRE(rejection.value_or("").contains("--config"));

    // --config alongside it is refused too, and this is the case that matters:
    // nothing here can tell whether the named file carries `requirepass:` — the
    // installer-seeded YAML does not — so accepting the combination was the
    // silent drop under another name. The operator was told their password had
    // been registered and got a daemon serving with no authentication at all.
    cfg.configPath = "/opt/fastcached/etc/fastcached.yaml";
    auto const withConfig = FastCache::InlineCredentialRejection(SpecFor("fastcached", cfg));
    REQUIRE(withConfig.has_value());
    REQUIRE(withConfig.value_or("").contains("/opt/fastcached/etc/fastcached.yaml"));

    // And a config with no secret at all is never in the way.
    REQUIRE(!FastCache::InlineCredentialRejection(SpecFor("fastcached", FastCache::Config {})).has_value());
}

TEST_CASE("ServiceControl: an IPv6 listener round-trips through its own parser", "[platform][service]")
{
    // ParseListenSpec splits an unbracketed spec on its last ':' and rejects a
    // literal outright, so `::` emitted bare came back as `--listen=:::11211` —
    // a service that registered cleanly and then failed at every single start,
    // restarted forever by KeepAlive.
    FastCache::Config cfg {};
    cfg.binds = { { .address = "::", .port = 11211, .tls = false },
                  { .address = "2001:db8::1", .port = 11212, .tls = true } };

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--listen=[::]:11211") != argv.end());
    REQUIRE(std::ranges::find(argv, "--listen-tls=[2001:db8::1]:11212") != argv.end());

    // An IPv4 address has no brackets to gain, and adding them would be just as
    // wrong: the bracketed grammar demands a literal.
    cfg.binds = { { .address = "127.0.0.1", .port = 11211, .tls = false } };
    auto const v4 = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(v4, "--listen=127.0.0.1:11211") != v4.end());
}

TEST_CASE("ServiceControl: a TLS listener's kind survives the registration", "[platform][service]")
{
    // A TLS endpoint must come back as one. Re-registering it as a plain `--listen`
    // would silently serve the cache in the clear at the next start -- the daemon
    // would come up, answer, and simply not be encrypted, which is the shape of
    // failure this file exists to catch: registers cleanly, then does the wrong
    // thing forever.
    //
    // This case used to assert the same property for `--listen-dispatch`. That flag
    // is gone -- the fleet's scheduler moved to `fastcache-compile-node` -- and the
    // property it was guarding is the general one:
    // whichever listener flag an endpoint was spelled with is the one it comes back
    // as. TLS is the surviving second kind, so it inherits the guard rather than
    // leaving `ListenFlagFor` with no test at all.
    FastCache::Config cfg {};
    cfg.binds = { { .address = "127.0.0.1", .port = 6674, .tls = false },
                  { .address = "127.0.0.1", .port = 6679, .tls = true } };

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, AsTyped(cfg), EmitDaemonFlag::No);
    CHECK(std::ranges::find(argv, "--listen=127.0.0.1:6674") != argv.end());
    CHECK(std::ranges::find(argv, "--listen-tls=127.0.0.1:6679") != argv.end());
    // And the TLS endpoint is not ALSO emitted as a plain listener, which would open
    // the port twice with different policies.
    CHECK(std::ranges::find(argv, "--listen=127.0.0.1:6679") == argv.end());
}

TEST_CASE("ServiceControl: a value ending in a backslash survives quoting", "[platform][service]")
{
    // Inside quotes a backslash run immediately before the closing `"` is
    // halved, so a lone trailing one escaped the quote instead of ending the
    // token: the argument ran on and swallowed every later flag into itself.
    // std::filesystem::absolute readily produces such a path for a directory.
    FastCache::Config cfg {};
    cfg.storagePath = R"(C:\Program Files\fastcached\cache\)";
    cfg.pidfile = R"(C:\run\fastcached.pid)";

    auto const cmd = CommandLineFor(std::filesystem::path { "fastcached" }, cfg);

    // The pidfile flag must still be recognisable as its own token; if the
    // storage value swallowed it, this is what would go missing.
    REQUIRE(cmd.contains("--pidfile="));

    // On POSIX the paths are relative (no drive letter), so absolute() rebases
    // them on the working directory — assert the property that holds on both:
    // the doubled trailing run, which is what the parser halves back to one.
    if (cmd.contains(R"(cache\)"))
        REQUIRE(cmd.contains(R"(cache\\")"));
}

TEST_CASE("ServiceControl: a service name that escapes its directory is refused", "[platform][service]")
{
    // The name is concatenated into the directory launchd scans, so a separator
    // writes a root-owned plist somewhere no uninstall path knows about — after
    // the install has printed success.
    auto rejects = [](std::string_view name) {
        FastCache::Config cfg {};
        cfg.serviceName = name;
        return FastCache::ServiceNameRejection(SpecFor("fastcached", cfg)).has_value();
    };

    REQUIRE(rejects("../../../../etc/periodic/daily/zz"));
    REQUIRE(rejects("fast/cached"));
    REQUIRE(rejects(R"(fast\cached)"));
    REQUIRE(rejects(".."));
    REQUIRE(rejects(".hidden")); // launchd's directory scan skips dotfiles.
    REQUIRE(rejects("fast\ncached"));
    REQUIRE(rejects(""));

    // A deny-list, not an allow-list: the SCM has accepted spaces and
    // punctuation since forever, and breaking those registrations to fix a
    // traversal only separators can express would be a poor trade.
    REQUIRE(!rejects("FastCached"));
    REQUIRE(!rejects("My Cache"));
    REQUIRE(!rejects("fastcached-2"));
}

TEST_CASE("ServiceControl: every registration rule gates an install", "[platform][service]")
{
    // One gate for both platforms' InstallService, so a rule cannot be enforced
    // on one supervisor and forgotten on the other.
    FastCache::Config named {};
    named.serviceName = "../escape";
    REQUIRE(FastCache::ServiceRegistrationRejection(SpecFor("fastcached", named), FastCache::SupervisorKind::Launchd)
                .has_value());

    FastCache::Config secret {};
    secret.requirePass = "hunter2";
    REQUIRE(
        FastCache::ServiceRegistrationRejection(SpecFor("fastcached", secret), FastCache::SupervisorKind::Scm).has_value());

    REQUIRE(!FastCache::ServiceRegistrationRejection(SpecFor("fastcached", FastCache::Config {}),
                                                     FastCache::SupervisorKind::Launchd)
                 .has_value());
}

TEST_CASE("ServiceControl: the timestamp switch registers whichever spelling produces the value", "[platform][service]")
{
    // **Issue #507, and the four combinations are the case.** The emitter used to
    // emit the POSITIVE flag whenever a value differed from its default, which spells
    // "on". That is right while every default is false and inverted the moment one is
    // not: with `logTimestamps` defaulting true under macOS (#496), an operator's
    // explicit `--no-log-timestamps` differed from the default and was registered as
    // `--log-timestamps`. The thing they turned off, turned back on, at every boot,
    // silently -- a registration replays its command line forever.
    //
    // Neither half alone is visible. On a false default the old emitter is correct,
    // so a case run only there passes under the bug; and with only one value driven,
    // the wrong spelling is still A spelling and "something was emitted" holds. The
    // defect lived in the COMBINATION, so the case drives both values, and does it
    // against the pure decision rather than against `BuildServiceArgv`.
    //
    // The second axis is now PROVENANCE rather than the platform default (#349), and
    // that strictly widens what is asserted: the row `--log-timestamps` on a
    // true-defaulting host used to register nothing at all -- correct output for the
    // moment, and a pin the next build could move, since `DefaultLogTimestamps` is
    // exactly the compile-time constant #496 changed. A host now runs one default and
    // no longer needs to be asked for the other, because no default is consulted.
    struct Combination
    {
        bool value;    ///< What the operator asked for.
        bool wasTyped; ///< Whether they named the switch at all.
        std::string_view expected;
    };

    // "" is the registration having nothing to say, and under provenance that is
    // exactly one thing: the operator named no switch, so the next start decides it
    // the same way this one did.
    constexpr auto Combinations = std::to_array<Combination>({
        { .value = true, .wasTyped = true, .expected = "log-timestamps" },
        { .value = false, .wasTyped = true, .expected = "no-log-timestamps" },
        { .value = true, .wasTyped = false, .expected = "" },
        { .value = false, .wasTyped = false, .expected = "" },
    });

    for (auto const& c: Combinations)
    {
        INFO("value: " << c.value << " wasTyped: " << c.wasTyped);
        auto const spelling = FastCache::SwitchSpellingFor("log-timestamps", "no-log-timestamps", c.value, c.wasTyped);
        CHECK(spelling.value_or("") == c.expected);
    }

    // A one-sided switch has no spelling for "off", so an explicit false registers
    // nothing rather than a bare `--`. Unreachable from argv -- `--metrics` can only
    // set true -- which is why it is asserted here rather than left to be discovered.
    CHECK(!FastCache::SwitchSpellingFor("metrics", {}, false, true).has_value());
    CHECK(FastCache::SwitchSpellingFor("metrics", {}, true, true).value_or("") == "metrics");

    // And the round trip, because a spelling is only correct if the daemon reading it
    // back arrives at the value that produced it. Both directions, through the
    // project's own parser -- the emitter and the parser are the two ends of one
    // contract and a test that only inspected the string could not see them disagree.
    for (auto const& c: Combinations)
    {
        INFO("value: " << c.value << " wasTyped: " << c.wasTyped);
        auto const spelling = FastCache::SwitchSpellingFor("log-timestamps", "no-log-timestamps", c.value, c.wasTyped);
        if (!spelling.has_value())
            continue;

        auto const flag = std::format("--{}", *spelling);
        auto const args = std::array<char const*, 1> { flag.c_str() };
        auto const parsed = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(parsed.has_value());
        CHECK(parsed->config.logTimestamps == c.value);
        // Provenance travels too: a value the operator named must read as named on
        // the far side, or the next registration cannot tell it from a default.
        CHECK(parsed->logTimestampsExplicit);
    }
}

TEST_CASE("ServiceControl: a pinned --max-memory equal to the host default is registered", "[platform][service]")
{
    // **Issue #349, end to end through the real parser.** `--max-memory` defaults to
    // a quarter of host RAM clamped to [512m, 8g], so on a 32 GiB machine the daemon
    // reports 8 GiB and the operator pins exactly that. Under a value comparison the
    // pin IS the default, so nothing was registered and the service re-derived its
    // budget from RAM at every start: add memory, or resize the VM, and the pinned
    // budget silently moved -- for precisely the operator who bothered to pin it.
    //
    // Spelled in bytes because that is a spelling `ParseMaxMemory` accepts on every
    // host; the point is the coincidence with the default, not the suffix.
    auto const pin = std::format("--max-memory={}", FastCache::DefaultMaxMemoryBytes());
    auto const args = std::array<char const*, 2> { "--install-service", pin.c_str() };
    auto const parsed = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(parsed.has_value());

    // The premise: this really is the value the daemon would have chosen anyway, so
    // no value comparison anywhere can tell it from silence.
    REQUIRE(parsed->config.maxMemoryBytes == FastCache::DefaultMaxMemoryBytes());
    REQUIRE(parsed->maxMemoryBytesExplicit);

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, *parsed, EmitDaemonFlag::No);
    CHECK(std::ranges::contains(argv, pin));
}

TEST_CASE("ServiceControl: every flag that can reach a registration does, one row at a time", "[platform][service]")
{
    // **The mechanical guard, and it is mechanical on purpose (#349).**
    // `BuildServiceArgv` re-spells every flag by hand, so it is a second place a CLI
    // flag is written down, and #349 was one of those spellings deciding by VALUE
    // what the parse had already recorded as PROVENANCE. Fixing the row that was
    // caught would have left thirty with the same shape, which is how the defect
    // reached a second binary in the first place -- so the guard walks
    // `CliOptions()` instead, and a new row cannot regress by omission.
    //
    // **One row at a time**, which is what makes it stronger than the whole-config
    // sweep it replaces. That one drove every field away from its default at once,
    // so a line naming a neighbour's field or bit was covered by the neighbour's own
    // emission; and it could not express the timestamp pair at all, since a
    // registration carries whichever spelling produces the value and never both.
    // Per row, both spellings are ordinary cases.
    //
    // Two assertions per row, and the second is the one presence alone cannot make:
    //
    //  - **named, at its default** -- the one input no value comparison can tell
    //    from silence, which is exactly what #349 was. Skipped for a valueless flag
    //    (its default is the value it cannot express) and for a row emitted on
    //    presence (there is nothing to emit until it has a value);
    //  - **named, carrying a value** -- the flag must reach the supervisor AND the
    //    emitted token must MOVE. A line reading `emitIfExplicit("metrics-port",
    //    cfg.port, cli.metricsPortExplicit)` passes a presence test forever: the
    //    spelling is right, only the field is wrong, and every registration would
    //    pin the cache port as the metrics port.
    struct Excuse
    {
        std::string_view flag;   ///< The row this applies to.
        std::string_view reason; ///< Why, in the words the code uses.
    };

    /// A value distinct from the row's own default, so the emitted token has to move.
    struct Typed
    {
        std::string_view flag;  ///< The row this applies to.
        std::string_view value; ///< What the operator would have typed.
    };

    // The rows a registration must NOT carry. Every one is either not daemon state
    // at all, or is refused outright -- there is no row here excused for being
    // inconvenient to assert.
    constexpr auto NeverRegistered = std::to_array<Excuse>({
        { .flag = "--install-service", .reason = "a service must never re-install itself" },
        { .flag = "--uninstall-service", .reason = "nor deregister itself" },
        { .flag = "--service-scope", .reason = "install-time only, and not Config state" },
        { .flag = "--service-start", .reason = "install-time only: the supervisor's record, not Config state" },
        { .flag = "--firewall-allow", .reason = "install-time only: it scopes the firewall rules the install creates" },
        { .flag = "--seed-config", .reason = "an installer step, not daemon state" },
        // Converting the store is a one-shot act. A registration carrying it would
        // re-run the conversion at every boot, on a store that after the first run
        // has nothing left to convert.
        { .flag = "--migrate-storage", .reason = "a one-shot verb, not daemon state" },
        { .flag = "--daemon", .reason = "carried by EmitDaemonFlag, not from Config" },
        { .flag = "--healthcheck", .reason = "probe and exit, instead of serving" },
        { .flag = "--help", .reason = "not daemon state" },
        { .flag = "--version", .reason = "not daemon state" },
        // The one Config field with no safe representation in launch arguments: a
        // supervisor records them where every local account can read them, so
        // emitting the secret would publish it to exactly the accounts it exists to
        // keep out. ServiceRegistrationRejection reports the omission instead, and
        // "dropping a password is reported, not silent" asserts that.
        { .flag = "--requirepass", .reason = "world-readable launch arguments; refused instead" },
    });

    // Rows that carry no explicit bit and so cannot be asked whether they were
    // named. They are emitted on presence, which is sound only because an empty one
    // of these names nothing a registration could carry -- unlike `--storage`,
    // which since #349 asks provenance like everything else.
    constexpr auto RegisteredOnPresence = std::to_array<Excuse>({
        { .flag = "--config", .reason = "no explicit bit; emitted on presence" },
        { .flag = "--pidfile", .reason = "no explicit bit; emitted on presence" },
        { .flag = "--listen", .reason = "repeatable; an empty listener set registers nothing" },
        { .flag = "--listen-tls", .reason = "repeatable; an empty listener set registers nothing" },
    });

    // One row's stimulus is a property of the BUILD rather than a literal, so the
    // table below is `const` and not `constexpr`. `--memory-compression` defaults to
    // `none`, and every other value it accepts is a codec
    // `FASTCACHED_ENABLE_COMPRESSION` compiled in -- `ParseCompression` refuses one
    // that is not, so a literal `zstd` would fail this sweep on a supported
    // configuration for a reason that has nothing to do with a registration. Asked
    // of the codec table, which is the question `ParseCompression` itself asks.
    //
    // With no codec compiled in there is exactly ONE legal value, so for that row
    // "the token moved" is not a weakened assertion but an unaskable one. It is
    // named here rather than skipped at the assertion, and the presence half still
    // runs on every build.
    constexpr auto NonIdentityCodecs =
        std::to_array({ FastCache::CompressionCodec::Zstd, FastCache::CompressionCodec::Lz4 });
    //
    // The loop holds no ITERATOR, which is portability rather than taste and is the
    // reason `MakeConfigFileSettings` gives for the same shape: `std::array`'s
    // iterator is a raw pointer on libstdc++, so clang-tidy's
    // `readability-qualified-auto` demands `auto*` -- and it is a class type in
    // MSVC's debug STL, which then refuses that spelling. A range-based `for` over
    // values satisfies both.
    auto const secondCodec = [&NonIdentityCodecs] -> std::optional<FastCache::CompressionCodec> {
        for (auto const codec: NonIdentityCodecs)
            if (FastCache::Compression::IsAvailable(codec))
                return codec;
        return std::nullopt;
    }();
    auto const memoryCodecStimulus =
        FastCache::Compression::NameOf(secondCodec.value_or(FastCache::CompressionCodec::Identity));

    // Whether a row's emitted token CAN move at all on this build: true for every
    // row but the one above, and there only where something was compiled in to move
    // to.
    auto const movementIsAskable = [&secondCodec](std::string_view flag) {
        return flag != "--memory-compression" || secondCodec.has_value();
    };

    // One per value row, and every one differs from that row's default -- which is
    // what makes "the token moved" mean "this line reads this row's field".
    auto const TypedValues = std::to_array<Typed>({
        { .flag = "--config", .value = "fastcached.yaml" },
        { .flag = "--bind", .value = "0.0.0.0" },
        { .flag = "--port", .value = "6000" },
        // Clamped to [512m, 8g], so 128m is below every reachable default.
        { .flag = "--max-memory", .value = "134217728" },
        { .flag = "--log-level", .value = "debug" },
        { .flag = "--auth-username", .value = "operator" },
        { .flag = "--metrics-bind", .value = "0.0.0.0" },
        { .flag = "--metrics-port", .value = "9999" },
        { .flag = "--tls-cert", .value = "cert.pem" },
        { .flag = "--tls-key", .value = "key.pem" },
        { .flag = "--listen", .value = "127.0.0.1:11211" },
        { .flag = "--listen-tls", .value = "127.0.0.1:11212" },
        { .flag = "--notify-keyspace-events", .value = "KEA" },
        { .flag = "--storage", .value = "cache.cow" },
        { .flag = "--storage-durability", .value = "fsync" },
        { .flag = "--storage-max-value", .value = "4096" },
        { .flag = "--storage-max-disk", .value = "8192" },
        { .flag = "--compression", .value = "none" },
        { .flag = "--compression-level", .value = "9" },
        { .flag = "--compression-min-bytes", .value = "1024" },
        { .flag = "--memory-compression", .value = memoryCodecStimulus },
        { .flag = "--memory-compression-level", .value = "9" },
        { .flag = "--memory-compression-min-bytes", .value = "1024" },
        { .flag = "--lru-mode", .value = "strict" },
        { .flag = "--cpu-affinity", .value = "none" },
        { .flag = "--threads", .value = "5" },
        { .flag = "--listen-backlog", .value = "64" },
        { .flag = "--storage-shards", .value = "7" },
        { .flag = "--expiry-interval", .value = "250ms" },
        { .flag = "--expiry-scan", .value = "64" },
        { .flag = "--pidfile", .value = "fastcached.pid" },
        { .flag = "--service-name", .value = "MyCache" },
    });

    // Both lookups answer in VALUES rather than handing back an iterator: a
    // `std::to_array` iterator is a raw pointer on libstdc++ and libc++ and a class
    // type on MSVC, so no single spelling of one compiles everywhere.
    auto const excused = [](auto const& table, std::string_view flag) {
        return std::ranges::any_of(table, [flag](Excuse const& row) { return row.flag == flag; });
    };
    auto const typedValueFor = [&TypedValues](std::string_view flag) -> std::string_view {
        for (auto const& row: TypedValues)
            if (row.flag == flag)
                return row.value;
        return {};
    };
    auto const tokenFor = [](std::vector<std::string> const& argv, std::string_view flag) -> std::string {
        for (auto const& arg: argv)
            if (FastCache::FlagMatches(arg, flag))
                return arg;
        return {};
    };
    auto const registrationFor = [](FastCache::CliResult const& cli) {
        return BuildServiceArgv(std::filesystem::path { "fastcached" }, cli, EmitDaemonFlag::No);
    };

    for (auto const& spec: FastCache::CliOptions())
    {
        if (excused(NeverRegistered, spec.primary))
            continue;

        INFO("flag: " << spec.primary);
        REQUIRE(spec.apply != nullptr);

        // Named, and nothing else touched.
        FastCache::CliResult named {};
        if (spec.explicitBit != nullptr)
            named.*spec.explicitBit = true;
        auto const atDefault = tokenFor(registrationFor(named), spec.primary);
        if (spec.arity == FastCache::Arity::Value && !excused(RegisteredOnPresence, spec.primary))
            CHECK(!atDefault.empty());

        // Named, and carrying what typing it would supply -- for a valueless flag
        // that is its own `apply`, which is exactly what typing it does.
        FastCache::CliResult typed {};
        REQUIRE(spec.apply(typed, typedValueFor(spec.primary)).has_value());
        if (spec.explicitBit != nullptr)
            typed.*spec.explicitBit = true;
        auto const withValue = tokenFor(registrationFor(typed), spec.primary);
        CHECK(!withValue.empty());
        // Movement is a value row's question. A valueless flag's `apply` may well
        // produce the platform default -- `--no-log-timestamps` on a host that
        // already defaults off emits the same token either way -- so demanding it
        // there would demand a contradiction on one platform. A wrong FIELD on a
        // switch is caught above instead: the emission follows the other field, so
        // no token under this spelling appears at all. `movementIsAskable` is the
        // other exemption and is a property of the BUILD -- see `secondCodec`.
        if (spec.arity == FastCache::Arity::Value && movementIsAskable(spec.primary))
            CHECK(withValue != atDefault);
    }

    // Every value row needs a stimulus, or the sweep would drive it with an empty
    // string: some parsers accept that and some refuse it, so a missing entry would
    // fail somewhere unrelated or silently assert nothing. Asserted here so a new
    // value flag joins the table rather than tripping over it.
    for (auto const& spec: FastCache::CliOptions())
    {
        if (spec.arity != FastCache::Arity::Value || excused(NeverRegistered, spec.primary))
            continue;
        INFO("flag: " << spec.primary);
        CHECK(!typedValueFor(spec.primary).empty());
    }

    // And no table is allowed to grow silently into the whole option table: every
    // row on all three must still BE a row, so a flag that is renamed or retired
    // takes its excuse with it rather than leaving a line nothing reads.
    auto const rowsAreLive = [](auto const& table) {
        for (auto const& row: table)
        {
            INFO("listed flag: " << row.flag);
            CHECK(
                std::ranges::any_of(FastCache::CliOptions(), [&row](auto const& spec) { return spec.primary == row.flag; }));
        }
    };
    rowsAreLive(NeverRegistered);
    rowsAreLive(RegisteredOnPresence);
    rowsAreLive(TypedValues);

    // An excuse with no reason is the shape this file exists to refuse: it reads
    // like a decision and records none.
    auto const excusesAreReasoned = [](auto const& table) {
        for (auto const& row: table)
        {
            INFO("excused flag: " << row.flag);
            CHECK(!row.reason.empty());
        }
    };
    excusesAreReasoned(NeverRegistered);
    excusesAreReasoned(RegisteredOnPresence);
}

// ============================================================================
// The launchctl timeout verdict (#535)
//
// These run on EVERY platform, which is why the decision was split out of the
// `__APPLE__` branch that acquires the readings. The reason is the absence of a
// SEAM rather than of a machine -- CI does build and `ctest` on macOS, so an
// earlier draft's "no machine this project builds on" was false. What no test can
// reach is a decision inside a file-local function that spawns a real process and
// waits on it.
//
// The numbers below are MEASURED, by lifting the acquisition loop into a
// standalone program and driving it over a parked process and a spinning one on
// Linux. `posix_spawn` and `waitpid` are POSIX; `wait4` and `rusage` are 4.3BSD
// and were never specified by POSIX, which is worth saying precisely because
// `wait4` is the one call this comment separately flags as unverified on macOS --
// a portability claim doing load-bearing work it cannot support is the defect
// this whole change is about:
//
//     parked `sleep`   ->      665us of cpu against 1012ms of elapsed   (0.07%)
//     busy loop        ->   846448us of cpu against 1012ms of elapsed   (83.6%)
//
// That measurement REFUTED this code's first design, which read zero cpu as "it
// never ran". Nothing reads zero: exec and dynamic linking are not free, so a
// process that does nothing at all still costs hundreds of microseconds. The
// separation is a ratio, not a floor.
//
// What these cases cannot say, stated rather than implied: they pin the DECISION
// over a record. That `wait4` after a SIGKILL yields the killed child's cpu ON
// MACOS is acquisition, needs a macOS host, and is asserted nowhere here -- it
// was verified on Linux only.
// ============================================================================

namespace
{

/// A timed-out call with a given duty cycle over 1000ms of elapsed.
/// @param cpu The cpu reading, or nullopt when none could be taken.
/// @return The readings.
[[nodiscard]] FastCache::LaunchctlReadings TimedOutWith(std::optional<std::chrono::microseconds> cpu)
{
    return { .outcome = FastCache::LaunchctlOutcome::TimedOut,
             .exitStatus = 0,
             .elapsed = std::chrono::milliseconds { 1'000 },
             .budget = std::chrono::milliseconds { 1'000 },
             .cpu = cpu };
}

/// A timed-out call whose elapsed and budget DIFFER, so a case can tell which
/// one the duty cycle divides by.
///
/// The fixture above deliberately cannot: it sets both to 1000ms, which is fine
/// for the band arithmetic and useless for the denominator. And the denominator
/// is the whole subject of this change -- a polled wait overshoots, so a
/// classifier reading `budget` would be measuring time the process did not have.
/// @param cpu The cpu reading.
/// @return Readings with a 1000ms elapsed inside a 60000ms budget.
[[nodiscard]] FastCache::LaunchctlReadings OvershotWith(std::chrono::microseconds cpu)
{
    return { .outcome = FastCache::LaunchctlOutcome::TimedOut,
             .exitStatus = 0,
             .elapsed = std::chrono::milliseconds { 1'000 },
             .budget = std::chrono::milliseconds { 60'000 },
             .cpu = cpu };
}

} // namespace

TEST_CASE("A launchctl timeout is read as a duty cycle, with the band between reported as neither",
          "[platform][service][launchctl]")
{
    using FastCache::LaunchctlFinding;
    using FastCache::LaunchctlFindingOf;

    SECTION("the measured anchors land where they should")
    {
        // The two real readings, scaled to this fixture's 1000ms elapsed. If either
        // stopped classifying, the bands would no longer separate the only two
        // behaviours anyone has actually observed.
        CHECK(LaunchctlFindingOf(TimedOutWith(std::chrono::microseconds { 657 })) == LaunchctlFinding::Waiting);
        CHECK(LaunchctlFindingOf(TimedOutWith(std::chrono::microseconds { 836'000 })) == LaunchctlFinding::BurningCpu);
    }

    SECTION("zero is not special, which is the correction")
    {
        // It reads as `Waiting` like any other low duty cycle. The first design
        // gave zero its own meaning -- "it never ran" -- and measurement showed
        // nothing ever reaches it.
        CHECK(LaunchctlFindingOf(TimedOutWith(std::chrono::microseconds { 0 })) == LaunchctlFinding::Waiting);
    }

    SECTION("the band between the anchors is inconclusive, not rounded to an edge")
    {
        // 30% is neither parked nor spinning, and assigning it to the nearer edge
        // would be inventing a reading nobody took.
        CHECK(LaunchctlFindingOf(TimedOutWith(std::chrono::microseconds { 300'000 })) == LaunchctlFinding::Inconclusive);
    }

    SECTION("no reading at all is inconclusive too, for a different reason")
    {
        CHECK(LaunchctlFindingOf(TimedOutWith(std::nullopt)) == LaunchctlFinding::Inconclusive);
    }

    SECTION("a call that ended on its own is not a timeout to diagnose")
    {
        FastCache::LaunchctlReadings exited;
        exited.outcome = FastCache::LaunchctlOutcome::Exited;
        exited.exitStatus = 3;
        CHECK(LaunchctlFindingOf(exited) == LaunchctlFinding::NotATimeout);

        FastCache::LaunchctlReadings unstarted;
        unstarted.outcome = FastCache::LaunchctlOutcome::NotStarted;
        CHECK(LaunchctlFindingOf(unstarted) == LaunchctlFinding::NotATimeout);
    }
}

TEST_CASE("The timeout message carries the measured elapsed, not the ceiling", "[platform][service][launchctl]")
{
    // The defect this replaces interpolated `LaunchctlTimeoutSeconds`, so the
    // sentence read `60s` whatever the call cost. A polled wait overshoots its
    // deadline by up to one poll interval and on a loaded host by more, so the two
    // numbers differ in the ORDINARY case -- and the difference is itself a reading
    // about the machine.
    auto readings = TimedOutWith(std::chrono::microseconds { 0 });
    readings.elapsed = std::chrono::milliseconds { 60'123 };
    readings.budget = std::chrono::milliseconds { 60'000 };

    auto const text = FastCache::LaunchctlStatusText(readings);
    CHECK(text.contains("60123ms"));
    CHECK(text.contains("60000ms budget"));
    // The verb lives at the call site now (`LaunchctlFailureVerb`), so this
    // phrase carries the readings rather than the classification -- otherwise the
    // two together read "kickstart timed out (timed out after ...)".
    CHECK(text.contains("killed"));

    // And NOT the configured ceiling. The pre-change message was
    // `killed after {}s with no result`, interpolating `LaunchctlTimeoutSeconds`,
    // so it read "60s" whatever the call actually cost -- this is the assertion
    // that would have failed against it, which makes this case a regression test
    // for #535 rather than a description of it.
    //
    // It replaces a `CHECK_FALSE(contains("refused"))` that could not fail:
    // "refused" never appeared in this message in any version, so it read as a
    // guard and guarded nothing.
    CHECK_FALSE(text.contains("60s"));
}

TEST_CASE("The duty cycle is taken over elapsed, not over the budget", "[platform][service][launchctl]")
{
    using FastCache::LaunchctlFinding;
    using FastCache::LaunchctlFindingOf;

    // Nothing else here can tell the two apart: every other case sets elapsed
    // equal to budget, so swapping the denominator in the classifier leaves them
    // all green. Measured that way -- the substitution passes six CHECKs.
    //
    // 836ms of cpu inside 1000ms of ELAPSED is 83.6%, which is BurningCpu. The
    // same reading against the 60000ms BUDGET is 1.4%, which would be Waiting.
    // So this one case is the only thing standing between the classifier and a
    // denominator that measures time the process never had.
    CHECK(LaunchctlFindingOf(OvershotWith(std::chrono::microseconds { 836'000 })) == LaunchctlFinding::BurningCpu);
}

TEST_CASE("An elapsed of zero cannot be divided by, and says so", "[platform][service][launchctl]")
{
    // The guard exists; nothing drove it. A duty cycle over zero elapsed is not a
    // small number, it is no number.
    auto readings = TimedOutWith(std::chrono::microseconds { 5 });
    readings.elapsed = std::chrono::milliseconds { 0 };
    CHECK(FastCache::LaunchctlFindingOf(readings) == FastCache::LaunchctlFinding::Inconclusive);
}

TEST_CASE("The message says which finding it reached", "[platform][service][launchctl]")
{
    // THE OPERATOR-FACING PAYOFF OF THE WHOLE TICKET, and nothing asserted it.
    // Measured: deleting `LaunchctlStatusText`'s entire `switch` over the finding
    // and returning the bare `text` left all 27 assertions green -- so every
    // sentence an operator would actually read, including the microseconds ->
    // milliseconds conversion, shipped unverified.
    using FastCache::LaunchctlStatusText;

    // Burning cpu: the reading is rendered, and in MILLISECONDS. 836000us is the
    // spinner anchor, and 836ms is the only correct way to show it.
    auto const burning = LaunchctlStatusText(TimedOutWith(std::chrono::microseconds { 836'000 }));
    CHECK(burning.contains("burning cpu"));
    CHECK(burning.contains("836ms"));

    // Waiting: says what was seen and explicitly refuses to say which cause.
    auto const waiting = LaunchctlStatusText(TimedOutWith(std::chrono::microseconds { 657 }));
    CHECK(waiting.contains("almost no cpu"));
    CHECK(waiting.contains("does not say whether"));

    // Inconclusive, from a reading inside the band.
    CHECK(LaunchctlStatusText(TimedOutWith(std::chrono::microseconds { 300'000 })).contains("does not separate"));

    // And from no reading at all, which is a different reason for the same word.
    CHECK(LaunchctlStatusText(TimedOutWith(std::nullopt)).contains("does not separate"));
}

TEST_CASE("The message never claims the job will start at the next boot", "[platform][service][launchctl]")
{
    // #535's worst half. That sentence was emitted unconditionally, and it is a
    // PREDICTION these readings cannot support: a low duty cycle covers both a
    // loaded host, where a retry works, and a stall, where it does not.
    //
    // Conditioning it was the first plan and it was wrong for the same reason the
    // zero-cpu design was: there is no reading here that establishes the true
    // case. So the claim is gone rather than gated, which is the other half of
    // what the ticket allows.
    //
    // WHAT THIS CASE DOES NOT DO, stated because it would otherwise read as
    // coverage of the removal: the sentence lived in `InstallService`'s format
    // string inside the `__APPLE__` branch, never in `LaunchctlStatusText`. So
    // this passes identically against the code before the change, and it is a
    // FORWARD guard -- it stops the phrase being reintroduced here, where a
    // future author would most naturally put it -- rather than a regression test
    // for the deletion. The deletion itself is in the platform branch and is
    // asserted by nothing, on any platform, like the acquisition half above it.
    for (auto const cpu:
         { std::chrono::microseconds { 0 }, std::chrono::microseconds { 300'000 }, std::chrono::microseconds { 900'000 } })
    {
        CAPTURE(cpu.count());
        auto const text = FastCache::LaunchctlStatusText(TimedOutWith(cpu));
        CHECK_FALSE(text.contains("next login or boot"));
    }
    CHECK_FALSE(FastCache::LaunchctlStatusText(TimedOutWith(std::nullopt)).contains("next login or boot"));
}

TEST_CASE("Only an exit status of zero is a launchctl success", "[platform][service][launchctl]")
{
    // `LaunchctlSucceeded` used to live inside the `__APPLE__` branch, which made
    // it the one decision in this change that no test could reach -- against the
    // change's own thesis. Its truth value has to match what the four call sites
    // compared before (`== 0` at one, `!= 0` at the others), and this is what says so rather than a reviewer's
    // arithmetic: the old sentinels were -1 and -2, and `WEXITSTATUS` yields
    // 0-255, so no collision was possible and every site is preserved exactly.
    using FastCache::LaunchctlOutcome;
    using FastCache::LaunchctlReadings;
    using FastCache::LaunchctlSucceeded;

    LaunchctlReadings ok;
    ok.outcome = LaunchctlOutcome::Exited;
    ok.exitStatus = 0;
    CHECK(LaunchctlSucceeded(ok));

    LaunchctlReadings nonZero;
    nonZero.outcome = LaunchctlOutcome::Exited;
    nonZero.exitStatus = 1;
    CHECK_FALSE(LaunchctlSucceeded(nonZero));

    for (auto const outcome: { LaunchctlOutcome::NotStarted, LaunchctlOutcome::Signalled, LaunchctlOutcome::TimedOut })
    {
        LaunchctlReadings failed;
        failed.outcome = outcome;
        failed.exitStatus = 0; // deliberately zero: the OUTCOME must decide, not this
        CHECK_FALSE(LaunchctlSucceeded(failed));
    }
}

TEST_CASE("A call that ran and was killed is not one that could not be started", "[platform][service][launchctl]")
{
    // `NotStarted` used to cover both "the spawn failed" and "it ran and died on
    // a signal", so a `launchctl` that crashed was reported as one that never
    // started -- a diagnostic naming the wrong cause, which is the class of
    // defect this whole change is about.
    FastCache::LaunchctlReadings signalled;
    signalled.outcome = FastCache::LaunchctlOutcome::Signalled;
    signalled.terminatingSignal = 9;

    auto const text = FastCache::LaunchctlStatusText(signalled);
    CHECK(text.contains("signal 9"));
    CHECK_FALSE(text.contains("could not be started"));

    FastCache::LaunchctlReadings unstarted;
    unstarted.outcome = FastCache::LaunchctlOutcome::NotStarted;
    CHECK(FastCache::LaunchctlStatusText(unstarted).contains("could not be started"));
}

TEST_CASE("Only a timeout is named a timeout, at every call site", "[platform][service][launchctl]")
{
    // One spelling for both failing call sites. `bootstrap` kept saying "failed"
    // for a timeout after `kickstart` was fixed -- byte-identical defects, and the
    // unobserved one is the one that survives a fix aimed at the observed one.
    using FastCache::LaunchctlFailureVerb;
    using FastCache::LaunchctlOutcome;
    using FastCache::LaunchctlReadings;

    LaunchctlReadings timedOut;
    timedOut.outcome = LaunchctlOutcome::TimedOut;
    CHECK(LaunchctlFailureVerb(timedOut) == "timed out");

    for (auto const outcome: { LaunchctlOutcome::Exited, LaunchctlOutcome::NotStarted, LaunchctlOutcome::Signalled })
    {
        LaunchctlReadings other;
        other.outcome = outcome;
        CHECK(LaunchctlFailureVerb(other) == "failed");
    }
}

TEST_CASE("ServiceControl: each start mode is one row naming its SCM start type and launchd keys",
          "[platform][service][service-start]")
{
    // The values are the Win32 constants SERVICE_AUTO_START (2) and SERVICE_DEMAND_START (3),
    // spelled as numbers so the case runs on every platform; ServiceControl.cpp static_asserts
    // them against <windows.h> where that header exists.
    auto const& automatic = FastCache::ServiceStartRowOf(FastCache::ServiceStart::Auto);
    CHECK(automatic.name == "auto");
    CHECK(automatic.scmStartType == 2U);
    CHECK(automatic.runAtLoad);
    CHECK(automatic.startsAtInstall);

    auto const& manual = FastCache::ServiceStartRowOf(FastCache::ServiceStart::Manual);
    CHECK(manual.name == "manual");
    CHECK(manual.scmStartType == 3U);
    CHECK_FALSE(manual.runAtLoad);
    CHECK_FALSE(manual.startsAtInstall);
}

TEST_CASE("ServiceControl: --service-start round-trips through its spelling, and an unknown one is refused by name",
          "[platform][service][service-start]")
{
    for (auto const& row: FastCache::ServiceStartTable())
    {
        auto const parsed = FastCache::ParseServiceStart(row.name);
        REQUIRE(parsed.has_value());
        CHECK(Unwrap(parsed) == row.start);
    }

    auto const refused = FastCache::ParseServiceStart("boot");
    REQUIRE_FALSE(refused.has_value());
    // No field: `--service-start` stamps its own spelling (CliParser_test asks the row).
    CHECK(refused.error().field.empty());
    CHECK(refused.error().context.contains("'boot'"));
    CHECK(refused.error().context.contains("auto"));
    CHECK(refused.error().context.contains("manual"));
}

TEST_CASE("ServiceControl: a manual job's plist does not run at load, and an automatic one does",
          "[platform][service][launchd][service-start]")
{
    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, FastCache::CliResult {});
    REQUIRE(spec.startMode == FastCache::ServiceStart::Auto);
    CHECK(BuildLaunchdPlist(spec, ServiceScope::System, "/tmp/logs").contains("<key>RunAtLoad</key>\n    <true/>"));

    spec.startMode = FastCache::ServiceStart::Manual;
    auto const manual = BuildLaunchdPlist(spec, ServiceScope::System, "/tmp/logs");
    CHECK(manual.contains("<key>RunAtLoad</key>\n    <false/>"));
    CHECK_FALSE(manual.contains("<key>RunAtLoad</key>\n    <true/>"));
}

TEST_CASE("ServiceControl: the daemon's spec carries the start mode typed, and never replays the flag",
          "[platform][service][service-start]")
{
    std::array<char const*, 2> const argv { "--install-service", "--service-start=manual" };
    auto const parsed = FastCache::ParseCli(argv);
    REQUIRE(parsed.has_value());
    auto const spec = SpecFor(std::filesystem::path { "fastcached" }, Unwrap(parsed));
    CHECK(spec.startMode == FastCache::ServiceStart::Manual);
    // A start mode is the SUPERVISOR's record, not something the running daemon reads:
    // replaying it would hand every boot a flag the process ignores.
    CHECK(std::ranges::none_of(spec.arguments, [](std::string const& arg) { return arg.starts_with("--service-start"); }));

    CHECK(SpecFor(std::filesystem::path { "fastcached" }, FastCache::CliResult {}).startMode
          == FastCache::ServiceStart::Auto);
}

TEST_CASE("ServiceControl: a manual system job is not kept alive unconditionally, which would start it at load",
          "[platform][service][launchd][service-start]")
{
    // launchd starts a `KeepAlive = true` job as soon as it is loaded, whatever `RunAtLoad`
    // says: "keep it running" is a demand to run it. A system-scope manual job kept alive
    // unconditionally would be started by `bootstrap` at install and at every boot -- the
    // auto-start the operator declined, reported as manual. No job is, in either mode: the
    // unconditional form also restarts a refused start forever, so every job takes the
    // restart-on-crash form, which keeps a job the operator started from staying down.
    constexpr std::string_view Unconditional = "<key>KeepAlive</key>\n    <true/>";
    constexpr std::string_view OnCrash = "<key>KeepAlive</key>\n    <dict>\n        <key>Crashed</key>\n        <true/>";

    auto spec = SpecFor(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, FastCache::CliResult {});
    auto const automatic = BuildLaunchdPlist(spec, ServiceScope::System, "/tmp/logs");
    CHECK_FALSE(automatic.contains(Unconditional));
    CHECK(automatic.contains(OnCrash));

    spec.startMode = FastCache::ServiceStart::Manual;
    auto const manual = BuildLaunchdPlist(spec, ServiceScope::System, "/tmp/logs");
    CHECK_FALSE(manual.contains(Unconditional));
    CHECK(manual.contains(OnCrash));

    // The user scope never keeps a job alive unconditionally, in either mode.
    CHECK(BuildLaunchdPlist(spec, ServiceScope::User, "/tmp/logs").contains(OnCrash));
}

TEST_CASE("ServiceControl: a refused start mode or scope lists every spelling its table accepts",
          "[platform][service][service-start]")
{
    // The accepted list is read off the table, so a row added later is named by the
    // refusal without anybody editing the message. Asserted per ROW, so a new row the
    // message missed fails here, and the whole text once, so the joiner's shape is pinned.
    auto const start = FastCache::ParseServiceStart("boot");
    REQUIRE_FALSE(start.has_value());
    for (auto const& row: FastCache::ServiceStartTable())
    {
        CAPTURE(row.name);
        CHECK(start.error().context.contains(row.name));
    }
    CHECK(start.error().context == "unknown start mode 'boot'; expected auto or manual");

    auto const scope = FastCache::ParseServiceScope("root");
    REQUIRE_FALSE(scope.has_value());
    for (auto const each: { ServiceScope::User, ServiceScope::System })
    {
        CAPTURE(FastCache::ServiceScopeName(each));
        CHECK(scope.error().context.contains(FastCache::ServiceScopeName(each)));
    }
    CHECK(scope.error().context == "unknown service scope 'root'; expected user or system");
}

TEST_CASE("ServiceControl: what an install does when the SCM will not create the service is one row per error",
          "[platform][service][scm]")
{
    // Numbers, so this runs on every platform; ServiceControl.cpp static_asserts them
    // against <windows.h>.
    constexpr std::uint32_t ServiceExists = 1073;        // ERROR_SERVICE_EXISTS
    constexpr std::uint32_t MarkedForDelete = 1072;      // ERROR_SERVICE_MARKED_FOR_DELETE
    constexpr std::uint32_t DuplicateDisplayName = 1078; // ERROR_DUPLICATE_SERVICE_NAME
    constexpr std::uint32_t AccessDenied = 5;            // ERROR_ACCESS_DENIED

    // An upgrade no longer deletes the registration, so the next install meets it: re-apply.
    CHECK(FastCache::CreateRefusalStepFor(ServiceExists) == FastCache::CreateRefusalStep::Reconfigure);
    // An older MSI deletes without waiting for the stop, so the new install can meet a
    // registration the SCM has not finished removing: wait for it, bounded.
    CHECK(FastCache::CreateRefusalStepFor(MarkedForDelete) == FastCache::CreateRefusalStep::AwaitDeletion);
    // Another service already DISPLAYS this name: nothing here may take it over.
    CHECK(FastCache::CreateRefusalStepFor(DuplicateDisplayName) == FastCache::CreateRefusalStep::Refuse);
    // Everything not in the table is reported, never retried.
    CHECK(FastCache::CreateRefusalStepFor(AccessDenied) == FastCache::CreateRefusalStep::Refuse);
    CHECK(FastCache::CreateRefusalStepFor(0) == FastCache::CreateRefusalStep::Refuse);

    // One row per error: a second row for one code would make the answer depend on order.
    auto const table = FastCache::CreateRefusalTable();
    for (auto const& row: table)
        CHECK(std::ranges::count(table, row.win32Error, &FastCache::CreateRefusalRow::win32Error) == 1);
}

namespace
{
/// A drain clock that charges each sleep a fixed cost, whatever was asked for, so a
/// wait MEASURED by it and one counted in polls come out different.
class ChargingDrainWait final: public FastCache::IDrainWait
{
  public:
    explicit ChargingDrainWait(std::chrono::milliseconds charge):
        _charge { charge }
    {
    }

    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return _now;
    }

    void Sleep(std::chrono::milliseconds /*requested*/) noexcept override
    {
        _now += _charge;
        ++_sleeps;
    }

    /// @return Sleeps taken.
    [[nodiscard]] int Sleeps() const noexcept
    {
        return _sleeps;
    }

  private:
    int _sleeps = 0;
    std::chrono::milliseconds _charge;
    core::platform::SteadyTimePoint _now {};
};

/// Answers `Create` from a script whose last entry repeats, and `Reapply` with one value.
class ScriptedScmRegistrar final: public FastCache::IScmRegistrar
{
  public:
    ScriptedScmRegistrar(std::vector<std::uint32_t> creates, std::uint32_t reapply):
        _creates { std::move(creates) },
        _reapply { reapply }
    {
    }

    [[nodiscard]] std::uint32_t Create() override
    {
        auto const answer = _creates[std::min(static_cast<std::size_t>(_createCalls), _creates.size() - 1)];
        ++_createCalls;
        return answer;
    }

    [[nodiscard]] std::uint32_t Reapply() override
    {
        ++_reapplyCalls;
        return _reapply;
    }

    /// @return `Create` calls made.
    [[nodiscard]] int CreateCalls() const noexcept
    {
        return _createCalls;
    }

    /// @return `Reapply` calls made.
    [[nodiscard]] int ReapplyCalls() const noexcept
    {
        return _reapplyCalls;
    }

  private:
    int _createCalls = 0;
    int _reapplyCalls = 0;
    std::vector<std::uint32_t> _creates;
    std::uint32_t _reapply;
};

constexpr std::uint32_t Win32Ok = 0;
constexpr std::uint32_t Win32AccessDenied = 5;
constexpr std::uint32_t Win32MarkedForDelete = 1072;
constexpr std::uint32_t Win32ServiceExists = 1073;
constexpr std::uint32_t Win32DuplicateDisplayName = 1078;
} // namespace

TEST_CASE("ServiceControl: an install that creates the service neither waits nor re-applies", "[platform][service][scm]")
{
    ScriptedScmRegistrar registrar { { Win32Ok }, Win32Ok };
    ChargingDrainWait wait { std::chrono::seconds { 1 } };

    auto const registration = FastCache::RegisterWithScm(registrar, wait);

    CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Created);
    CHECK_FALSE(registration.deletionWaited.has_value());
    CHECK(registrar.CreateCalls() == 1);
    CHECK(registrar.ReapplyCalls() == 0);
    CHECK(wait.Sleeps() == 0);
}

TEST_CASE("ServiceControl: an existing service is re-applied without waiting", "[platform][service][scm]")
{
    ScriptedScmRegistrar registrar { { Win32ServiceExists }, Win32Ok };
    ChargingDrainWait wait { std::chrono::seconds { 1 } };

    auto const registration = FastCache::RegisterWithScm(registrar, wait);

    CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Reapplied);
    CHECK_FALSE(registration.deletionWaited.has_value());
    CHECK(registrar.ReapplyCalls() == 1);
    CHECK(wait.Sleeps() == 0);
}

TEST_CASE("ServiceControl: a registration still being deleted is waited out, and the wait is measured",
          "[platform][service][scm]")
{
    // Each sleep is charged 7 s whatever it asked for (250 ms), so the measured wait
    // and a count of polls disagree.
    ChargingDrainWait wait { std::chrono::seconds { 7 } };

    SECTION("the deletion finishes and the service is created")
    {
        ScriptedScmRegistrar registrar { { Win32MarkedForDelete, Win32MarkedForDelete, Win32MarkedForDelete, Win32Ok },
                                         Win32Ok };
        auto const registration = FastCache::RegisterWithScm(registrar, wait);

        CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Created);
        CHECK(registrar.CreateCalls() == 4);
        CHECK(wait.Sleeps() == 2);
        REQUIRE(registration.deletionWaited.has_value());
        CHECK(Unwrap(registration.deletionWaited) == std::chrono::seconds { 14 });
    }

    SECTION("the deletion finishes onto a service that exists again, which is re-applied")
    {
        ScriptedScmRegistrar registrar { { Win32MarkedForDelete, Win32MarkedForDelete, Win32ServiceExists }, Win32Ok };
        auto const registration = FastCache::RegisterWithScm(registrar, wait);

        CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Reapplied);
        CHECK(registrar.ReapplyCalls() == 1);
        REQUIRE(registration.deletionWaited.has_value());
        CHECK(Unwrap(registration.deletionWaited) == std::chrono::seconds { 7 });
    }

    SECTION("the deletion outlasts the ceiling, and the message names the MEASURED wait")
    {
        ScriptedScmRegistrar registrar { { Win32MarkedForDelete }, Win32Ok };
        auto const registration = FastCache::RegisterWithScm(registrar, wait);

        CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Failed);
        CHECK(registration.win32Error == Win32MarkedForDelete);
        CHECK(registrar.ReapplyCalls() == 0);
        // Sleeps at 0, 7, 14, 21 and 28 s; at 35 s the 30 s ceiling has passed.
        REQUIRE(registration.deletionWaited.has_value());
        CHECK(Unwrap(registration.deletionWaited) == std::chrono::seconds { 35 });
        CHECK(FastCache::ScmRegistrationFailureMessage(registration, "FastCached")
              == "service 'FastCached' was still being deleted after 35.0 s; stop the process that holds it and "
                 "run the install again");
    }
}

TEST_CASE("ServiceControl: a re-apply the SCM answers 'being deleted' did not wait, and does not claim to",
          "[platform][service][scm]")
{
    // The race: CreateService saw the service, and it was deleted before ChangeServiceConfig.
    ScriptedScmRegistrar registrar { { Win32ServiceExists }, Win32MarkedForDelete };
    ChargingDrainWait wait { std::chrono::seconds { 7 } };

    auto const registration = FastCache::RegisterWithScm(registrar, wait);

    CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Failed);
    CHECK(registration.win32Error == Win32MarkedForDelete);
    CHECK_FALSE(registration.deletionWaited.has_value());
    CHECK(wait.Sleeps() == 0);

    auto const message = FastCache::ScmRegistrationFailureMessage(registration, "FastCached");
    CHECK(message == "service 'FastCached' is being deleted; stop the process that holds it and run the install again");
    CHECK_FALSE(message.contains("after"));
}

TEST_CASE("ServiceControl: an install refused for any other reason is reported, never retried", "[platform][service][scm]")
{
    ChargingDrainWait wait { std::chrono::seconds { 7 } };

    SECTION("access denied names elevation")
    {
        ScriptedScmRegistrar registrar { { Win32AccessDenied }, Win32Ok };
        auto const registration = FastCache::RegisterWithScm(registrar, wait);
        CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Failed);
        CHECK(registrar.CreateCalls() == 1);
        CHECK(registrar.ReapplyCalls() == 0);
        CHECK(FastCache::ScmRegistrationFailureMessage(registration, "FastCached")
              == "access denied registering the service; run from an elevated (Administrator) prompt");
    }

    SECTION("another service displaying the name is refused with its error")
    {
        ScriptedScmRegistrar registrar { { Win32DuplicateDisplayName }, Win32Ok };
        auto const registration = FastCache::RegisterWithScm(registrar, wait);
        CHECK(registration.outcome == FastCache::ScmRegistrationOutcome::Failed);
        CHECK(registrar.CreateCalls() == 1);
        CHECK(registrar.ReapplyCalls() == 0);
        CHECK_FALSE(registration.deletionWaited.has_value());
        CHECK(FastCache::ScmRegistrationFailureMessage(registration, "FastCached")
              == "registering service 'FastCached' failed (error 1078)");
    }
}

TEST_CASE("ServiceControl: a re-applied service is told to restart, a created one to start", "[platform][service][scm]")
{
    using FastCache::ScmInstallSuccessMessage;
    using FastCache::ScmRegistrationOutcome;

    CHECK(ScmInstallSuccessMessage(ScmRegistrationOutcome::Created, "FastCached", "auto-start", "")
          == "installed service 'FastCached' (auto-start); start it now with: sc start FastCached");

    auto const reapplied =
        ScmInstallSuccessMessage(ScmRegistrationOutcome::Reapplied, "FastCached", "manual start", "\nwarning: x");
    CHECK(reapplied
          == "updated the registration of service 'FastCached' (manual start); it takes effect at the service's next "
             "start -- if it is running, restart it with: net stop FastCached && net start FastCached, otherwise "
             "start it with: sc start FastCached\nwarning: x");
    CHECK_FALSE(reapplied.contains("start it now"));
}

namespace
{
/// `IOwnedPathHandover` that records each call and answers from a script.
class ScriptedHandover final: public FastCache::IOwnedPathHandover
{
  public:
    /// Which of the two calls a path reached.
    enum class Call : std::uint8_t
    {
        Share,
        Seclude,
    };

    /// @param denials Path to the denial its call answers; a path not named succeeds.
    explicit ScriptedHandover(std::vector<std::pair<std::filesystem::path, std::string>> denials):
        _denials { std::move(denials) }
    {
    }

    [[nodiscard]] std::optional<std::string> Share(std::filesystem::path const& path) override
    {
        return Answer(Call::Share, path);
    }

    [[nodiscard]] std::optional<std::string> Seclude(std::filesystem::path const& path,
                                                     std::span<std::filesystem::path const> credentialLeaves) override
    {
        _secludedLeaves.assign(credentialLeaves.begin(), credentialLeaves.end());
        return Answer(Call::Seclude, path);
    }

    /// @return Every call made, in order.
    [[nodiscard]] std::vector<std::pair<Call, std::filesystem::path>> const& Calls() const noexcept
    {
        return _calls;
    }

    /// @return The credential leaves the last `Seclude` was given.
    [[nodiscard]] std::vector<std::filesystem::path> const& SecludedLeaves() const noexcept
    {
        return _secludedLeaves;
    }

  private:
    [[nodiscard]] std::optional<std::string> Answer(Call call, std::filesystem::path const& path)
    {
        _calls.emplace_back(call, path);
        auto const* const denial = core::findOrNull(_denials, path, &std::pair<std::filesystem::path, std::string>::first);
        return denial != nullptr ? std::optional { denial->second } : std::nullopt;
    }

    std::vector<std::pair<std::filesystem::path, std::string>> _denials;
    std::vector<std::pair<Call, std::filesystem::path>> _calls;
    std::vector<std::filesystem::path> _secludedLeaves;
};
} // namespace

TEST_CASE("ServiceControl: a shared path that cannot be handed over warns and a private one refuses the install",
          "[platform][service][owned-paths]")
{
    using FastCache::OwnedPath;
    using FastCache::PathPrivacy;
    using Call = ScriptedHandover::Call;

    auto const cache = std::filesystem::path { "C:/ProgramData/fastcache-node/cache" };
    auto const state = std::filesystem::path { "C:/ProgramData/fastcache-node" };
    auto const later = std::filesystem::path { "C:/ProgramData/fastcache-node/later" };
    auto const paths = std::to_array<OwnedPath>({
        { .path = cache, .privacy = PathPrivacy::Shared, .credentialFiles = {} },
        { .path = state, .privacy = PathPrivacy::Private, .credentialFiles = { std::filesystem::path { "node-key" } } },
        { .path = later, .privacy = PathPrivacy::Shared, .credentialFiles = {} },
    });

    SECTION("each path reaches the call its privacy names, and Seclude carries the credential leaves")
    {
        // The distinguishing half: a private path handed over through `Share` is the defect
        // this exists to prevent -- an entry ADDED to what `%ProgramData%` lets every account
        // read -- and it would succeed, so only WHICH call ran can tell.
        auto handover = ScriptedHandover { {} };
        auto const result = FastCache::HandOverOwnedPaths(paths, handover);
        CHECK_FALSE(result.refusal.has_value());
        CHECK(result.warnings.empty());
        CHECK(handover.Calls()
              == std::vector<std::pair<Call, std::filesystem::path>> {
                  { Call::Share, cache }, { Call::Seclude, state }, { Call::Share, later } });
        // The credential leaf reaches Seclude, or the identity key would be told to /reset
        // rather than be deleted when it is exposed.
        CHECK(handover.SecludedLeaves() == std::vector<std::filesystem::path> { std::filesystem::path { "node-key" } });
    }

    SECTION("a shared path that fails is a warning and the walk goes on")
    {
        auto handover = ScriptedHandover { { { cache, "could not create it" } } };
        auto const result = FastCache::HandOverOwnedPaths(paths, handover);
        CHECK_FALSE(result.refusal.has_value());
        CHECK(result.warnings == "\nwarning: could not create it");
        CHECK(handover.Calls().size() == 3);
    }

    SECTION("a private path that fails refuses and nothing after it is touched")
    {
        auto handover = ScriptedHandover { { { state, "its access list could not be replaced (error 5)" } } };
        auto const result = FastCache::HandOverOwnedPaths(paths, handover);
        REQUIRE(result.refusal.has_value());
        // WHICH refusal: the path, the reason the handover gave, and the consequence.
        CHECK(Unwrap(result.refusal).contains(state.string()));
        CHECK(Unwrap(result.refusal).contains("its access list could not be replaced (error 5)"));
        CHECK(Unwrap(result.refusal).contains("other local accounts could read it"));
        CHECK(result.warnings.empty());
        CHECK(handover.Calls()
              == std::vector<std::pair<Call, std::filesystem::path>> { { Call::Share, cache }, { Call::Seclude, state } });
    }
}

TEST_CASE("ServiceControl: a refused install says what became of the registration", "[platform][service][owned-paths]")
{
    using FastCache::RefusedInstallMessage;
    using FastCache::RefusedRegistration;

    constexpr std::string_view Refusal = "the state directory could not be given an access list of its own";
    auto const notMade = RefusedInstallMessage(Refusal, "FastCacheCompileNode", RefusedRegistration::NotMade);
    auto const removed = RefusedInstallMessage(Refusal, "FastCacheCompileNode", RefusedRegistration::Removed);
    auto const notRemoved = RefusedInstallMessage(Refusal, "FastCacheCompileNode", RefusedRegistration::NotRemoved);
    auto const kept = RefusedInstallMessage(Refusal, "FastCacheCompileNode", RefusedRegistration::Kept);

    for (auto const& message: { notMade, removed, notRemoved, kept })
    {
        INFO(message);
        CHECK(message.starts_with(Refusal));
    }

    CHECK(notMade.contains("nothing was registered"));
    CHECK(removed.contains("was removed again"));
    // The one outcome that leaves the operator something to do by hand names the command.
    CHECK(notRemoved.contains("could NOT be removed"));
    CHECK(notRemoved.contains("sc delete FastCacheCompileNode"));
    CHECK(kept.contains("left in place"));

    CHECK(std::ranges::none_of(std::array { removed, notRemoved, kept },
                               [&notMade](std::string const& other) { return other == notMade; }));
    CHECK(removed != notRemoved);
    CHECK(removed != kept);
    CHECK(notRemoved != kept);
}

TEST_CASE("ServiceControl: fastcached opens its non-loopback listeners and, with --metrics, its admin port",
          "[platform][service][firewall]")
{
    auto const program = std::filesystem::path { "C:/Program Files/fastcached/bin/fastcached.exe" };

    FastCache::Config loopback {};
    CHECK(FastCache::DaemonFirewallRules(loopback, program, {}).empty());

    FastCache::Config open {};
    open.bindAddress = "0.0.0.0";
    open.port = 6674;
    auto const single = FastCache::DaemonFirewallRules(open, program, { "10.0.0.0/8" });
    REQUIRE(single.size() == 1);
    CHECK(single.front().name == "FastCached cache tcp/6674");
    CHECK(single.front().group == "fastcached: FastCached");
    CHECK(single.front().remoteAddresses == std::vector<std::string> { "10.0.0.0/8" });

    FastCache::Config listeners {};
    listeners.binds = { FastCache::BindConfig { .address = "127.0.0.1", .port = 6674, .tls = false },
                        FastCache::BindConfig { .address = "0.0.0.0", .port = 6690, .tls = true } };
    listeners.metricsEnabled = true;
    listeners.metricsBindAddress = "0.0.0.0";
    listeners.metricsPort = 9464;
    auto const several = FastCache::DaemonFirewallRules(listeners, program, {});
    REQUIRE(several.size() == 2);
    CHECK(several[0].name == "FastCached cache tcp/6690");
    CHECK(several[1].name == "FastCached metrics tcp/9464");
}

namespace
{

/// An absolute path on this host: the firewall refuses a rule whose program is not one.
#if defined(_WIN32)
constexpr auto FirewallProgram = std::string_view { "C:/Program Files/fastcached/bin/fastcached.exe" };
#else
constexpr auto FirewallProgram = std::string_view { "/opt/fastcached/bin/fastcached" };
#endif

/// A daemon named @p serviceName listening on the wildcard, so its registration opens one rule.
[[nodiscard]] FastCache::Config OpenDaemon(std::string const& serviceName)
{
    FastCache::Config cfg {};
    cfg.serviceName = serviceName;
    cfg.bindAddress = "0.0.0.0";
    cfg.port = 6674;
    return cfg;
}

} // namespace

TEST_CASE("ServiceControl: an uninstall whose firewall removal is refused still reports the service removed",
          "[platform][service][firewall]")
{
    // The ruling: the operator asked for the service to go, and an MSI uninstall must not fail over
    // a firewall rule -- so the exit code is the deletion's, and the words name what stayed behind.
    auto const ours = FastCache::DaemonFirewallRules(OpenDaemon("Probe"), FirewallProgram, {});
    REQUIRE(ours.size() == 1);
    auto theirs = ours.front();
    theirs.group = "Operator rules";
    FastCache::Testing::RecordingFirewall firewall;
    firewall.rules = { ours.front(), theirs };

    auto const deleted = FastCache::WithRemovalFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Removed,
                                          .message = "uninstalled service 'Probe'" },
        &firewall,
        "Probe");
    INFO(deleted.message);
    CHECK(deleted.outcome == FastCache::ServiceControlOutcome::Removed);
    CHECK(deleted.ExitCode() == 0);
    CHECK(deleted.message.starts_with("uninstalled service 'Probe'"));
    CHECK(deleted.message.contains("have the same name ignoring case"));
    CHECK(deleted.message.contains("still in place: Probe cache tcp/6674"));
    CHECK(deleted.message.contains("Remove-NetFirewallRule -Group 'fastcached: Probe'"));
    CHECK(firewall.rules.size() == 2);

    // A service that was never there leaves nothing to protect, so its rules go -- and the outcome
    // stays the uninstall's, whatever the firewall did.
    firewall.rules = { ours.front() };
    auto const missing = FastCache::WithRemovalFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::NotInstalled,
                                          .message = "no service named 'Probe' is installed" },
        &firewall,
        "Probe");
    INFO(missing.message);
    CHECK(missing.outcome == FastCache::ServiceControlOutcome::NotInstalled);
    CHECK(missing.ExitCode() == FastCache::CommandExitCode(FastCache::CommandEnding::Declined));
    CHECK(missing.message.starts_with("no service named 'Probe' is installed"));
    CHECK(missing.message.contains("firewall: removed 1 rule(s)"));
    CHECK(firewall.rules.empty());
}

TEST_CASE("ServiceControl: an uninstall whose deletion was refused leaves the firewall alone",
          "[platform][service][firewall]")
{
    // A non-elevated uninstall: OpenService(DELETE) is refused, so the service is still installed
    // and running. Removing its rules -- or telling the operator how to -- would close its ports.
    // Both ways a removal can leave the service in place: DECLINED (access denied, a decision) and
    // FAILED (a call that failed on its own); neither is a service that is gone.
    auto const ours = FastCache::DaemonFirewallRules(OpenDaemon("Probe"), FirewallProgram, {});
    REQUIRE(ours.size() == 1);
    for (auto const outcome: { FastCache::ServiceControlOutcome::Declined, FastCache::ServiceControlOutcome::Failed })
    {
        FastCache::Testing::RecordingFirewall firewall;
        firewall.rules = ours;

        auto const refused = FastCache::WithRemovalFirewall(
            FastCache::ServiceControlResult { .outcome = outcome, .message = "access denied opening the service" },
            &firewall,
            "Probe");
        INFO(refused.message);
        CHECK(refused.outcome == outcome);
        CHECK(firewall.rules == ours);
        CHECK(refused.message.starts_with("access denied opening the service"));
        CHECK(refused.message.contains(
            "any rules of this service are left in place, since the service may still be registered"));
        CHECK_FALSE(refused.message.contains("Remove-NetFirewallRule"));
        CHECK_FALSE(refused.message.contains("firewall: removed"));
    }

    // Where no firewall is managed there is nothing to say at all.
    auto const unmanaged =
        FastCache::WithRemovalFirewall(FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Failed,
                                                                         .message = "access denied opening the service" },
                                       nullptr,
                                       "Probe");
    CHECK(unmanaged.message == "access denied opening the service");
}

TEST_CASE("ServiceControl: only an install that registered touches the firewall, and a refusal does not fail it",
          "[platform][service][firewall]")
{
    auto const rules = FastCache::DaemonFirewallRules(OpenDaemon("Probe"), FirewallProgram, {});
    REQUIRE(rules.size() == 1);

    FastCache::Testing::RecordingFirewall firewall;
    auto const refused = FastCache::WithRegistrationFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Failed, .message = "access denied" },
        &firewall,
        "Probe",
        rules);
    CHECK(refused.outcome == FastCache::ServiceControlOutcome::Failed);
    CHECK(refused.message == "access denied");
    CHECK(firewall.adds == 0);

    auto const registered = FastCache::WithRegistrationFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Created,
                                          .message = "installed service 'Probe'" },
        &firewall,
        "Probe",
        rules);
    CHECK(registered.outcome == FastCache::ServiceControlOutcome::Created);
    CHECK(registered.message
          == "installed service 'Probe'\nfirewall: allowed inbound Probe cache tcp/6674 (every network profile)");
    CHECK(firewall.InGroup("fastcached: Probe") == rules);

    firewall.refuseRemove = "access denied reading the firewall";
    auto const unopened = FastCache::WithRegistrationFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Created,
                                          .message = "installed service 'Probe'" },
        &firewall,
        "Probe",
        rules);
    INFO(unopened.message);
    CHECK(unopened.outcome == FastCache::ServiceControlOutcome::Created);
    CHECK(
        unopened.message.contains("warning: the firewall rules were not all created (access denied reading the firewall)"));
}

TEST_CASE("ServiceControl: a bracketed loopback bind opens nothing, and one bound to localhost opens a rule and says why",
          "[platform][service][firewall]")
{
    // `--bind` keeps the brackets it was given, so `[::1]` must be read as the loopback it is.
    FastCache::Config bracketed {};
    bracketed.bindAddress = "[::1]";
    CHECK(FastCache::DaemonFirewallRules(bracketed, FirewallProgram, {}).empty());

    // ...and a bracketed wildcard as the wildcard.
    FastCache::Config wildcard {};
    wildcard.binds = { FastCache::BindConfig { .address = "[::]", .port = 6690, .tls = false } };
    auto const opened = FastCache::DaemonFirewallRules(wildcard, FirewallProgram, {});
    REQUIRE(opened.size() == 1);
    CHECK(opened.front().name == "FastCached cache tcp/6690");

    // A NAME is not loopback, `localhost` included: the rule is opened, and the note says why.
    auto named = OpenDaemon("Probe");
    named.bindAddress = "127.0.0.1";
    named.metricsEnabled = true;
    named.metricsBindAddress = "localhost";
    named.metricsPort = 9464;
    auto const rules = FastCache::DaemonFirewallRules(named, FirewallProgram, {});
    REQUIRE(rules.size() == 1);
    CHECK(rules.front().name == "Probe metrics tcp/9464");
    CHECK(rules.front().bindHost == "localhost");

    FastCache::Testing::RecordingFirewall firewall;
    auto const installed = FastCache::WithRegistrationFirewall(
        FastCache::ServiceControlResult { .outcome = FastCache::ServiceControlOutcome::Created,
                                          .message = "installed service 'Probe'" },
        &firewall,
        "Probe",
        rules);
    INFO(installed.message);
    CHECK(installed.message.contains("allowed inbound Probe metrics tcp/9464"));
    CHECK(installed.message.contains("note: Probe metrics tcp/9464 is opened for a surface bound to 'localhost': a name is "
                                     "whatever the resolver answers"));
    CHECK(installed.message.contains("bind 127.0.0.1 or ::1 to need no rule"));
}

TEST_CASE("A Windows registration restarts a failed service three times and then leaves it stopped",
          "[platform][service][recovery]")
{
    // The list `sc qfailure` shows, as the install registers it. It used to end in a restart, which
    // the SCM repeats for every failure past the end -- so a start refused by its own configuration
    // was restarted every thirty seconds, forever, writing the same refusal into the event log.
    // A refusal (`ProcessExit::Refused`) is retried here too, three times, because the SCM cannot be
    // told otherwise without hiding its code from `sc query` -- `ServiceRestartAttempts` says why.
    CHECK(FastCache::ScmRecoveryActions()
          == std::vector {
              FastCache::ScmRecoveryAction { .type = FastCache::ScActionRestart, .delayMs = 1'000 },
              FastCache::ScmRecoveryAction { .type = FastCache::ScActionRestart, .delayMs = 1'000 },
              FastCache::ScmRecoveryAction { .type = FastCache::ScActionRestart, .delayMs = 30'000 },
              FastCache::ScmRecoveryAction { .type = FastCache::ScActionNone, .delayMs = 0 },
          });
    CHECK(FastCache::ServiceRecoveryResetPeriod == std::chrono::minutes { 10 });
}

// ----------------------------------------------------------------------------
// The registration seam: every ending a registration can reach, on every host.

namespace
{
/// A registration of the daemon with no setting named, whose name every rule accepts.
/// @param exePath The executable it launches.
/// @return The spec.
[[nodiscard]] FastCache::ServiceSpec RegistrableSpec(std::filesystem::path const& exePath)
{
    auto const spec = SpecFor(exePath, FastCache::CliResult {});
    REQUIRE_FALSE(FastCache::ServiceNameRejection(spec).has_value());
    return spec;
}

/// One way an SCM call can answer, and how the registration must end on it.
struct ScmVerdict
{
    std::string_view why;                 ///< The answer, in words.
    FastCache::Testing::ScmScript script; ///< What the service manager answers.
    FastCache::CommandEnding ending;      ///< How the registration ends.
    std::string_view says;                ///< What it tells the operator.
};

/// One way launchd or the system around it can answer, and how the registration must end on it.
struct LaunchdVerdict
{
    std::string_view why;                     ///< The answer, in words.
    FastCache::ServiceScope scope;            ///< Which domain.
    bool knowsItsExecutable;                  ///< False: the spec names no executable.
    FastCache::Testing::LaunchdScript script; ///< What launchd answers.
    FastCache::CommandEnding ending;          ///< How the registration ends.
    std::string_view says;                    ///< What it tells the operator.
};
} // namespace

TEST_CASE("An SCM registration declines on a decision and fails on a call that failed on its own",
          "[platform][service][scm][exit]")
{
    // A decision -- access denied, a service that exists or does not -- is 2 and a retry meets it
    // again; a call that failed on its own, or a path the environment could not answer, is 1.
    // Each code is one no DECISION reads: `ERROR_SHUTDOWN_IN_PROGRESS`, `ERROR_INVALID_SERVICE_ACCOUNT`,
    // `ERROR_INVALID_HANDLE`, `ERROR_SERVICE_MARKED_FOR_DELETE`.
    using FastCache::CommandEnding;
    using FastCache::Testing::ScmScript;
    auto const installs = std::vector<ScmVerdict> {
        { .why = "the executable's own path is unknown",
          .script = ScmScript { .executable = {} },
          .ending = CommandEnding::Failed,
          .says = "could not determine the fastcached executable path" },
        { .why = "the manager refuses this account",
          .script = ScmScript { .openManagerError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Declined,
          .says = "access denied opening the service manager" },
        { .why = "the manager would not open",
          .script = ScmScript { .openManagerError = 1115 },
          .ending = CommandEnding::Failed,
          .says = "OpenSCManager failed (error 1115)" },
        { .why = "the service already exists, and its registration is re-applied",
          .script = ScmScript { .createError = FastCache::ScmErrorServiceExists },
          .ending = CommandEnding::Completed,
          .says = "updated the registration of service 'FastCached'" },
        { .why = "the service already exists, and re-applying is refused to this account",
          .script =
              ScmScript { .createError = FastCache::ScmErrorServiceExists, .reapplyError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Declined,
          .says = "access denied registering the service" },
        { .why = "creating a service is refused to this account",
          .script = ScmScript { .createError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Declined,
          .says = "access denied registering the service" },
        { .why = "the service could not be created",
          .script = ScmScript { .createError = 1057 },
          .ending = CommandEnding::Failed,
          .says = "registering service 'FastCached' failed (error 1057)" },
        { .why = "every call succeeded",
          .script = ScmScript {},
          .ending = CommandEnding::Completed,
          .says = "installed service 'FastCached' (auto-start)" },
        { .why = "the restart policy was refused, and the registration stands",
          .script = ScmScript { .recoveryError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Completed,
          .says = "the restart policy could not be set (error 5)" },
        { .why = "the service SID type was refused, and the registration stands",
          .script = ScmScript { .sidTypeError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Completed,
          .says = "the service SID type could not be set (error 5)" },
    };
    for (auto const& verdict: installs)
    {
        INFO(verdict.why);
        FastCache::Testing::ScriptedScmCalls calls { verdict.script };
        auto const result = FastCache::ScmInstall(RegistrableSpec("C:/fastcached/fastcached.exe"), calls);
        INFO(result.message);
        CHECK(result.Ending() == verdict.ending);
        CHECK(result.message.contains(verdict.says));
        // Every path out closes what it opened, and never a handle twice.
        CHECK(calls.OpenHandles() == 0);
        CHECK(std::ranges::count(calls.Calls(), std::string { "Close(unknown handle)" }) == 0);
    }

    auto const removals = std::vector<ScmVerdict> {
        { .why = "the manager refuses this account",
          .script = ScmScript { .openManagerError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Declined,
          .says = "access denied opening the service manager" },
        { .why = "the manager would not open",
          .script = ScmScript { .openManagerError = 1115 },
          .ending = CommandEnding::Failed,
          .says = "OpenSCManager failed (error 1115)" },
        { .why = "no such service is installed",
          .script = ScmScript { .openError = FastCache::ScmErrorServiceDoesNotExist },
          .ending = CommandEnding::Declined,
          .says = "no service named 'FastCached' is installed" },
        { .why = "opening the service is refused to this account",
          .script = ScmScript { .openError = FastCache::ScmErrorAccessDenied },
          .ending = CommandEnding::Declined,
          .says = "access denied opening the service" },
        { .why = "the service would not open",
          .script = ScmScript { .openError = 6 },
          .ending = CommandEnding::Failed,
          .says = "OpenService failed (error 6)" },
        { .why = "the service was stopped and its delete failed",
          .script = ScmScript { .deleteError = 1072 },
          .ending = CommandEnding::Failed,
          .says = "DeleteService failed (error 1072)" },
        { .why = "every call succeeded",
          .script = ScmScript {},
          .ending = CommandEnding::Completed,
          .says = "uninstalled service 'FastCached'" },
    };
    for (auto const& verdict: removals)
    {
        INFO(verdict.why);
        FastCache::Testing::ScriptedScmCalls calls { verdict.script };
        auto const result = FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), calls);
        INFO(result.message);
        CHECK(result.Ending() == verdict.ending);
        CHECK(result.message.contains(verdict.says));
        CHECK(calls.OpenHandles() == 0);
        CHECK(std::ranges::count(calls.Calls(), std::string { "Close(unknown handle)" }) == 0);
    }
}

TEST_CASE("An SCM removal forgets the event source only once the service is gone", "[platform][service][scm]")
{
    // A service still installed without its provider logs records nobody can read, and nothing
    // short of a reinstall puts it back: a failed delete must leave the event source alone.
    using FastCache::Testing::ScmScript;
    FastCache::Testing::ScriptedScmCalls removed { ScmScript {} };
    REQUIRE(FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), removed).Ending()
            == FastCache::CommandEnding::Completed);
    CHECK(removed.Calls()
          == std::vector<std::string> {
              "OpenManager(Connect)", "Open", "Stop", "StillRunning", "Delete", "Close", "Close", "RemoveEventSource" });

    FastCache::Testing::ScriptedScmCalls stuck { ScmScript { .deleteError = 1072 } };
    REQUIRE(FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), stuck).Ending()
            == FastCache::CommandEnding::Failed);
    CHECK(std::ranges::count(stuck.Calls(), std::string { "RemoveEventSource" }) == 0);
}

TEST_CASE("An SCM registration opens the manager with the least each operation needs", "[platform][service][scm]")
{
    // An install creates a service -- or opens the existing one to re-apply it -- and an uninstall
    // only opens one, so neither asks for more: a mask wider than the operation is access a
    // registration holds and never uses. The values are the SDK's, asserted against `<windows.h>`
    // in the Windows build.
    CHECK(FastCache::ScmManagerAccessMask(FastCache::ScmManagerAccess::Create) == 0x0003);
    CHECK(FastCache::ScmManagerAccessMask(FastCache::ScmManagerAccess::Connect) == 0x0001);

    FastCache::Testing::ScriptedScmCalls installing { FastCache::Testing::ScmScript {} };
    REQUIRE(FastCache::ScmInstall(RegistrableSpec("C:/fastcached/fastcached.exe"), installing).Ending()
            == FastCache::CommandEnding::Completed);
    CHECK(std::ranges::count(installing.Calls(), std::string { "OpenManager(Create)" }) == 1);
    FastCache::Testing::ScriptedScmCalls removing { FastCache::Testing::ScmScript {} };
    REQUIRE(FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), removing).Ending()
            == FastCache::CommandEnding::Completed);
    CHECK(std::ranges::count(removing.Calls(), std::string { "OpenManager(Connect)" }) == 1);
}

TEST_CASE("An SCM registration asks for the service it was given, under the logon it names", "[platform][service][scm]")
{
    FastCache::Testing::ScriptedScmCalls calls { FastCache::Testing::ScmScript {} };
    auto const spec = RegistrableSpec("C:/fastcached/fastcached.exe");
    REQUIRE(FastCache::ScmInstall(spec, calls).Ending() == FastCache::CommandEnding::Completed);
    auto const& requested = Unwrap(calls.Requested());
    CHECK(requested.name == spec.serviceName);
    CHECK(requested.commandLine == FastCache::BuildServiceCommandLine(spec));
    CHECK(requested.logonName == FastCache::WindowsLogonName(spec));
    CHECK(requested.startType == FastCache::ServiceStartRowOf(spec.startMode).scmStartType);
}

TEST_CASE("A registration says whether it created the service or re-applied one that was there",
          "[platform][service][scm][launchd]")
{
    // What a refusal after the registration may undo: a registration this install CREATED is
    // removed again, one it RE-APPLIED is an upgrade's and stays (`InstallWithServiceFirewall`).
    using FastCache::ServiceControlOutcome;
    using FastCache::Testing::LaunchdScript;
    using FastCache::Testing::ScmScript;
    auto const spec = RegistrableSpec("C:/fastcached/fastcached.exe");

    FastCache::Testing::ScriptedScmCalls fresh { ScmScript {} };
    CHECK(FastCache::ScmInstall(spec, fresh).outcome == ServiceControlOutcome::Created);
    FastCache::Testing::ScriptedScmCalls existing { ScmScript { .createError = FastCache::ScmErrorServiceExists } };
    CHECK(FastCache::ScmInstall(spec, existing).outcome == ServiceControlOutcome::Reapplied);

    auto const job = RegistrableSpec("/opt/fastcached/bin/fastcached");
    FastCache::Testing::ScriptedLaunchdCalls newJob { LaunchdScript {} };
    CHECK(FastCache::LaunchdInstall(job, FastCache::ServiceScope::User, newJob).outcome == ServiceControlOutcome::Created);
    FastCache::Testing::ScriptedLaunchdCalls oldJob { LaunchdScript { .jobFileExists = true } };
    CHECK(FastCache::LaunchdInstall(job, FastCache::ServiceScope::User, oldJob).outcome == ServiceControlOutcome::Reapplied);

    // Every outcome's row, written out: an install's two leave a registration and no removal does.
    for (auto const outcome: FastCache::Enumerators<ServiceControlOutcome>())
    {
        INFO(static_cast<int>(outcome));
        auto const installs = outcome == ServiceControlOutcome::Created || outcome == ServiceControlOutcome::Reapplied;
        auto const gone = outcome == ServiceControlOutcome::Removed || outcome == ServiceControlOutcome::NotInstalled;
        CHECK(FastCache::Registered(outcome) == installs);
        CHECK(FastCache::NoneRemains(outcome) == gone);
    }
}

TEST_CASE("An SCM install a private path refuses deletes the registration it created, and keeps one it re-applied",
          "[platform][service][scm]")
{
    // The handover's refusal, reached through the registration rather than beside it: the MSI
    // starts the service whatever the install answered, so a registration THIS install created
    // must not outlive the refusal -- and one it merely re-applied was there before it.
    using FastCache::Testing::ScmScript;
    auto spec = RegistrableSpec("C:/fastcached/fastcached.exe");
    spec.ownedPaths.push_back(FastCache::OwnedPath {
        .path = "C:/ProgramData/fastcached/state", .privacy = FastCache::PathPrivacy::Private, .credentialFiles = {} });

    FastCache::Testing::ScriptedScmCalls created { ScmScript { .secludeDenial = "the list would not apply" } };
    auto const refused = FastCache::ScmInstall(spec, created);
    INFO(refused.message);
    CHECK(refused.outcome == FastCache::ServiceControlOutcome::Failed);
    CHECK(refused.message.contains("was removed again"));
    CHECK(std::ranges::count(created.Calls(), std::string { "Delete" }) == 1);
    CHECK(created.OpenHandles() == 0);

    FastCache::Testing::ScriptedScmCalls reapplied { ScmScript { .createError = FastCache::ScmErrorServiceExists,
                                                                 .secludeDenial = "the list would not apply" } };
    auto const kept = FastCache::ScmInstall(spec, reapplied);
    INFO(kept.message);
    CHECK(kept.outcome == FastCache::ServiceControlOutcome::Failed);
    CHECK(kept.message.contains("re-applied registration is left in place"));
    CHECK(std::ranges::count(reapplied.Calls(), std::string { "Delete" }) == 0);
    CHECK(reapplied.OpenHandles() == 0);

    FastCache::Testing::ScriptedScmCalls stuck { ScmScript { .deleteError = 1072,
                                                             .secludeDenial = "the list would not apply" } };
    CHECK(FastCache::ScmInstall(spec, stuck).message.contains("could NOT be removed"));
    CHECK(stuck.OpenHandles() == 0);
}

TEST_CASE("An SCM removal waits for the service to stop, bounded, and says when it deleted one still running",
          "[platform][service][scm]")
{
    using FastCache::Testing::ScmScript;
    FastCache::Testing::SteppedDrainWait stoppedWait;
    FastCache::Testing::ScriptedScmCalls stopping { ScmScript { .runningPolls = 3 } };
    auto const removed = FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), stopping, stoppedWait);
    INFO(removed.message);
    CHECK(removed.outcome == FastCache::ServiceControlOutcome::Removed);
    CHECK(std::ranges::count(stopping.Calls(), std::string { "StillRunning" }) == 4);
    CHECK_FALSE(removed.message.contains("had not stopped"));

    // The control: a service that never reports STOPPED is deleted at the bound, and SAID to be.
    FastCache::Testing::SteppedDrainWait ceilingWait;
    FastCache::Testing::ScriptedScmCalls running { ScmScript { .runningPolls = 1'000'000 } };
    auto const marked = FastCache::ScmUninstall(RegistrableSpec("C:/fastcached/fastcached.exe"), running, ceilingWait);
    INFO(marked.message);
    CHECK(marked.outcome == FastCache::ServiceControlOutcome::Removed);
    CHECK(marked.message.contains(std::format("it had not stopped after {} s", FastCache::UninstallStopCeiling.count())));
    CHECK(ceilingWait.Elapsed() >= FastCache::UninstallStopCeiling);
}

TEST_CASE("A launchd registration declines on a decision and fails on a call that failed on its own",
          "[platform][service][launchd][exit]")
{
    using FastCache::CommandEnding;
    using FastCache::ServiceScope;
    using FastCache::Testing::LaunchdScript;
    auto const installs = std::vector<LaunchdVerdict> {
        { .why = "the spec names no executable",
          .scope = ServiceScope::User,
          .knowsItsExecutable = false,
          .script = LaunchdScript {},
          .ending = CommandEnding::Failed,
          .says = "could not determine the executable path to register" },
        { .why = "a system job by an account that is not root",
          .scope = ServiceScope::System,
          .knowsItsExecutable = true,
          .script = LaunchdScript {},
          .ending = CommandEnding::Declined,
          .says = "requires root" },
        { .why = "a user agent by root",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .root = true },
          .ending = CommandEnding::Declined,
          .says = "must not run as root" },
        { .why = "the invoking user's home is unknown",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .home = {} },
          .ending = CommandEnding::Failed,
          .says = "could not determine the invoking user's home directory" },
        { .why = "the service account does not exist",
          .scope = ServiceScope::System,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .root = true, .accountExists = false },
          .ending = CommandEnding::Declined,
          .says = "service account does not exist" },
        { .why = "the job's directory could not be created",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .createError = std::make_error_code(std::errc::io_error) },
          .ending = CommandEnding::Failed,
          .says = "could not create" },
        { .why = "the job file could not be written",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .writes = false },
          .ending = CommandEnding::Failed,
          .says = "could not write" },
        { .why = "a private path could not be closed to other accounts, before anything was registered",
          .scope = ServiceScope::System,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .root = true, .handOverRefusal = "the mode would not apply" },
          .ending = CommandEnding::Failed,
          .says = "nothing was registered" },
        { .why = "the written job would not load",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .bootstrapStatus = 5 },
          .ending = CommandEnding::Failed,
          .says = "`launchctl bootstrap gui/501` failed" },
        { .why = "every call succeeded",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript {},
          .ending = CommandEnding::Completed,
          .says = "installed and started launchd job" },
    };
    for (auto const& verdict: installs)
    {
        INFO(verdict.why);
        auto spec = RegistrableSpec("/opt/fastcached/bin/fastcached");
        if (!verdict.knowsItsExecutable)
            spec.exePath.clear();
        FastCache::Testing::ScriptedLaunchdCalls calls { verdict.script };
        auto const result = FastCache::LaunchdInstall(spec, verdict.scope, calls);
        INFO(result.message);
        CHECK(result.Ending() == verdict.ending);
        CHECK(result.message.contains(verdict.says));
    }

    auto const removals = std::vector<LaunchdVerdict> {
        { .why = "a system job by an account that is not root",
          .scope = ServiceScope::System,
          .knowsItsExecutable = true,
          .script = LaunchdScript {},
          .ending = CommandEnding::Declined,
          .says = "requires root" },
        { .why = "the invoking user's home is unknown",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .home = {} },
          .ending = CommandEnding::Failed,
          .says = "could not determine the invoking user's home directory" },
        { .why = "no job file is installed",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .removal = false },
          .ending = CommandEnding::Declined,
          .says = "no launchd job installed at" },
        { .why = "the job file could not be removed",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript { .removal = std::unexpected { std::make_error_code(std::errc::io_error) } },
          .ending = CommandEnding::Failed,
          .says = "could not remove" },
        { .why = "every call succeeded",
          .scope = ServiceScope::User,
          .knowsItsExecutable = true,
          .script = LaunchdScript {},
          .ending = CommandEnding::Completed,
          .says = "removed launchd job" },
    };
    for (auto const& verdict: removals)
    {
        INFO(verdict.why);
        FastCache::Testing::ScriptedLaunchdCalls calls { verdict.script };
        auto const result =
            FastCache::LaunchdUninstall(RegistrableSpec("/opt/fastcached/bin/fastcached"), verdict.scope, calls);
        INFO(result.message);
        CHECK(result.Ending() == verdict.ending);
        CHECK(result.message.contains(verdict.says));
    }
}

TEST_CASE("A launchd registration boots a previous job out of every domain before it loads the new one",
          "[platform][service][launchd]")
{
    // The domain a job landed in was decided when it was installed, so the teardown asks every
    // candidate, and loads into the first domain launchd knows -- here `gui/501`.
    FastCache::Testing::ScriptedLaunchdCalls calls { FastCache::Testing::LaunchdScript {} };
    auto const spec = RegistrableSpec("/opt/fastcached/bin/fastcached");
    REQUIRE(FastCache::LaunchdInstall(spec, FastCache::ServiceScope::User, calls).Ending()
            == FastCache::CommandEnding::Completed);
    auto const label = FastCache::LaunchdLabel(spec);
    auto const& made = calls.Calls();
    auto const at = [&made](std::string const& call) {
        INFO(call);
        auto const index = std::ranges::distance(made.begin(), std::ranges::find(made, call));
        REQUIRE(index < std::ssize(made));
        return index;
    };
    auto const plist = FastCache::LaunchdPlistPath(spec, FastCache::ServiceScope::User, "/Users/operator");
    CHECK(at("launchctl bootout gui/501/" + label) < at("launchctl bootstrap gui/501 " + plist.string()));
    CHECK(at("launchctl bootout user/501/" + label) < at("launchctl bootstrap gui/501 " + plist.string()));
    CHECK(at("launchctl bootstrap gui/501 " + plist.string()) < at("launchctl kickstart -k gui/501/" + label));
}
