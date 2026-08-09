// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/Config.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

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

TEST_CASE("ServiceControl: install/uninstall are unsupported without a supervisor", "[platform][service]")
{
#if !defined(_WIN32) && !defined(__APPLE__)
    FastCache::Config const cfg {};
    auto const installed = FastCache::InstallService(cfg);
    auto const removed = FastCache::UninstallService(cfg);
    REQUIRE(installed.exitCode != 0);
    REQUIRE(removed.exitCode != 0);
#else
    // Deliberately not called here: on Windows and macOS these really do
    // register a service, which a unit test must not do to the host. The
    // scripts/macos-service-e2e.sh and the Windows MSI path cover them.
    SUCCEED("InstallService is exercised by the platform end-to-end paths");
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
    return BuildLaunchdPlist(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, cfg, scope, "/tmp/logs");
}
} // namespace

TEST_CASE("ServiceControl: launchd argv never carries --daemon", "[platform][service][launchd]")
{
    // The single most important property here. launchd supervises the process
    // it spawned; a job that double-forks is reaped immediately as "exited",
    // so the service silently never runs.
    FastCache::Config const cfg {};
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
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
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--service-name=My Cache") != argv.end());
}

TEST_CASE("ServiceControl: argv element 0 is the executable", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    auto const argv = BuildServiceArgv(std::filesystem::path { "/opt/fastcached/bin/fastcached" }, cfg, EmitDaemonFlag::No);
    REQUIRE(argv.front() == "/opt/fastcached/bin/fastcached");
}

TEST_CASE("ServiceControl: an unset path flag is omitted rather than absolutized", "[platform][service][launchd]")
{
    // Absolutizing first would turn the empty default into the caller's working
    // directory and pin the service to whatever shell registered it.
    FastCache::Config const cfg {};
    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
    REQUIRE(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("--storage="); }));
    REQUIRE(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("--config="); }));
}

TEST_CASE("ServiceControl: the launchd label is reverse-DNS and lowercased", "[platform][service][launchd]")
{
    FastCache::Config cfg {};
    REQUIRE(FastCache::LaunchdLabel(cfg) == "software.lastrada.fastcached");

    cfg.serviceName = "FastCachedSmoke";
    REQUIRE(FastCache::LaunchdLabel(cfg) == "software.lastrada.fastcachedsmoke");
}

TEST_CASE("ServiceControl: the plist path follows the scope", "[platform][service][launchd]")
{
    FastCache::Config const cfg {};
    auto const user = FastCache::LaunchdPlistPath(cfg, ServiceScope::User, "/Users/jo");
    auto const system = FastCache::LaunchdPlistPath(cfg, ServiceScope::System, "/Users/jo");

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

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
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

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
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

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
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
    auto const rejection = FastCache::InlineCredentialRejection(cfg);
    REQUIRE(rejection.has_value());
    REQUIRE(rejection.value_or("").contains("--config"));

    // --config alongside it is refused too, and this is the case that matters:
    // nothing here can tell whether the named file carries `requirepass:` — the
    // installer-seeded YAML does not — so accepting the combination was the
    // silent drop under another name. The operator was told their password had
    // been registered and got a daemon serving with no authentication at all.
    cfg.configPath = "/opt/fastcached/etc/fastcached.yaml";
    auto const withConfig = FastCache::InlineCredentialRejection(cfg);
    REQUIRE(withConfig.has_value());
    REQUIRE(withConfig.value_or("").contains("/opt/fastcached/etc/fastcached.yaml"));

    // And a config with no secret at all is never in the way.
    REQUIRE(!FastCache::InlineCredentialRejection(FastCache::Config {}).has_value());
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

    auto const argv = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(argv, "--listen=[::]:11211") != argv.end());
    REQUIRE(std::ranges::find(argv, "--listen-tls=[2001:db8::1]:11212") != argv.end());

    // An IPv4 address has no brackets to gain, and adding them would be just as
    // wrong: the bracketed grammar demands a literal.
    cfg.binds = { { .address = "127.0.0.1", .port = 11211, .tls = false } };
    auto const v4 = BuildServiceArgv(std::filesystem::path { "fastcached" }, cfg, EmitDaemonFlag::No);
    REQUIRE(std::ranges::find(v4, "--listen=127.0.0.1:11211") != v4.end());
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

    auto const cmd = BuildServiceCommandLine(std::filesystem::path { "fastcached" }, cfg);

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
        return FastCache::ServiceNameRejection(cfg).has_value();
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
    REQUIRE(FastCache::ServiceRegistrationRejection(named).has_value());

    FastCache::Config secret {};
    secret.requirePass = "hunter2";
    REQUIRE(FastCache::ServiceRegistrationRejection(secret).has_value());

    REQUIRE(!FastCache::ServiceRegistrationRejection(FastCache::Config {}).has_value());
}
