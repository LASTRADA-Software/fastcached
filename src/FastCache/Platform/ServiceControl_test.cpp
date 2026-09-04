// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("ServiceControl: install/uninstall are unsupported without a supervisor", "[platform][service]")
{
#if !defined(_WIN32) && !defined(__APPLE__)
    FastCache::Config const cfg {};
    auto const spec = SpecFor(std::filesystem::path { "fastcached" }, cfg);
    auto const installed = FastCache::InstallService(spec);
    auto const removed = FastCache::UninstallService(spec);
    REQUIRE(installed.exitCode != 0);
    REQUIRE(removed.exitCode != 0);
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
        REQUIRE(std::ranges::contains(filled.ownedPaths, expected));
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

TEST_CASE("ServiceControl: KeepAlive differs by scope", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};

    // A user agent that loses the race for the port exits cleanly, which
    // KeepAlive=<true/> would treat as "restart" — a permanent crash loop.
    auto const user = PlistFor(cfg, ServiceScope::User);
    REQUIRE(user.contains("<key>KeepAlive</key>"));
    REQUIRE(user.contains("<key>Crashed</key>"));

    // The system daemon owns the port outright, so restart-always is right.
    auto const system = PlistFor(cfg, ServiceScope::System);
    REQUIRE(system.contains("<key>KeepAlive</key>"));
    REQUIRE(!system.contains("<key>Crashed</key>"));
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
        for (std::size_t pos = plist.find(needle); pos != std::string::npos; pos = plist.find(needle, pos + 1))
            ++n;
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
    REQUIRE(err.field == "service-scope");
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
    // is gone -- the fleet's scheduler moved to `fastcache-compile-node
    // --serve-scheduler` -- and the property it was guarding is the general one:
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
    REQUIRE(FastCache::ServiceRegistrationRejection(SpecFor("fastcached", named)).has_value());

    FastCache::Config secret {};
    secret.requirePass = "hunter2";
    REQUIRE(FastCache::ServiceRegistrationRejection(SpecFor("fastcached", secret)).has_value());

    REQUIRE(!FastCache::ServiceRegistrationRejection(SpecFor("fastcached", FastCache::Config {})).has_value());
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
        { .flag = "--expiry-interval", .value = "250" },
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
