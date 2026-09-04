// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Core/PathKind.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <aclapi.h>
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/stat.h>
    #include <sys/wait.h>

    #include <csignal>
    #include <cstring>
    #include <fstream>

    #include <fcntl.h>
    #include <pwd.h>
    #include <spawn.h>
    #include <unistd.h>

    #include <mach-o/dyld.h>

extern char** environ;
#endif

namespace FastCache
{

std::filesystem::path CurrentExecutablePath()
{
#if defined(_WIN32)
    std::string buffer(MAX_PATH, '\0');
    while (true)
    {
        auto const copied = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
            return {};
        if (copied < buffer.size())
        {
            buffer.resize(copied);
            return std::filesystem::path { buffer };
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // Sets `size` to what is required.
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    buffer.resize(std::strlen(buffer.c_str()));

    // _NSGetExecutablePath may hand back a path containing symlinks or `..`;
    // launchd records ProgramArguments verbatim, so resolve it once here
    // rather than pinning the job to a path that may later dangle.
    std::error_code ec;
    auto const resolved = std::filesystem::canonical(buffer, ec);
    return ec ? std::filesystem::path { buffer } : resolved;
#else
    // Linux and the BSDs. Resolved rather than read raw for the same reason the
    // macOS arm resolves: whatever records this path keeps it verbatim, and a
    // path through a symlink that later moves pins a service to nothing.
    std::error_code ec;
    auto const resolved = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path {} : resolved;
#endif
}

namespace
{
    /// CLI spelling of a LogLevel, matching the values ParseLogLevel accepts.
    [[nodiscard]] constexpr std::string_view LogLevelName(LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Trace:
                return "trace";
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Error:
                return "error";
            case LogLevel::Fatal:
                return "fatal";
        }
        return "info";
    }

    /// CLI spelling of an LruRecency, matching ParseLruRecency.
    [[nodiscard]] constexpr std::string_view LruRecencyName(LruRecency recency) noexcept
    {
        return recency == LruRecency::Strict ? "strict" : "approximate";
    }

    /// CLI spelling of a CpuAffinity, matching ParseCpuAffinity.
    [[nodiscard]] constexpr std::string_view CpuAffinityName(CpuAffinity affinity) noexcept
    {
        return affinity == CpuAffinity::PerCore ? "per-core" : "none";
    }

    /// CLI spelling of a StorageDurability, matching ParseStorageDurability.
    [[nodiscard]] constexpr std::string_view DurabilityName(StorageDurability durability) noexcept
    {
        switch (durability)
        {
            case StorageDurability::Fsync:
                return "fsync";
            case StorageDurability::Batched:
                return "batched";
            case StorageDurability::None:
                return "none";
        }
        return "batched";
    }

    /// Quote @p value for a Windows command line, by the rules the CRT startup
    /// code and CommandLineToArgvW use to split one back into an argv array.
    ///
    /// Only a value containing a metacharacter is quoted, so ordinary ones still
    /// read plainly in `services.msc`. Inside quotes a backslash is literal
    /// *except* in a run immediately before a `"`, where the run is halved and
    /// the quote is escaped by the odd one out. Testing for a space alone was
    /// not enough: a path flag whose absolutized value ends in `\` — which
    /// std::filesystem::absolute readily produces for a directory — put a
    /// trailing `\"` at the end of the token, which the parser reads as an
    /// escaped literal quote, so the argument ran on and swallowed every later
    /// flag into itself.
    [[nodiscard]] std::string MaybeQuote(std::string_view value)
    {
        if (value.find_first_of(" \t\"") == std::string_view::npos)
            return std::string { value };

        std::string out { '"' };
        std::size_t backslashes = 0;
        for (auto const ch: value)
        {
            if (ch == '\\')
            {
                ++backslashes;
                continue;
            }
            // The pending run is literal unless this character is the quote it
            // would otherwise escape, in which case it doubles and one more is
            // added to escape the quote itself.
            out.append(ch == '"' ? (backslashes * 2) + 1 : backslashes, '\\');
            backslashes = 0;
            out += ch;
        }
        // The closing quote follows a trailing run, so that run doubles too.
        out.append(backslashes * 2, '\\');
        out += '"';
        return out;
    }

    /// Spell @p address the way ParseListenSpec reads it back.
    ///
    /// An IPv6 literal is the only address form containing a `:`, and the
    /// unbracketed grammar splits on the last one — so `::` round-tripped as
    /// `--listen=:::11211`, which the daemon's own parser rejects outright with
    /// "IPv6 literal requires brackets". The service registered fine and then
    /// failed at every start, restarted forever by KeepAlive.
    [[nodiscard]] std::string FormatListenHost(std::string const& address)
    {
        if (address.contains(':'))
            return std::format("[{}]", address);
        return address;
    }

    /// Absolutize a captured path so it survives the fact that a service does
    /// not inherit the installing shell's working directory (on Windows it
    /// starts in `C:\Windows\System32`; a launchd job starts in `/`).
    [[nodiscard]] std::string AbsolutePathArg(std::string const& path)
    {
        std::error_code ec;
        auto const abs = std::filesystem::absolute(path, ec);
        return ec ? path : abs.string();
    }

    /// Flag value renderers. One overload per type that appears in the table
    /// below, so the table itself stays free of per-type formatting.
    /// @{
    [[nodiscard]] std::string FlagValue(std::string const& value)
    {
        return value;
    }
    [[nodiscard]] std::string FlagValue(LogLevel value)
    {
        return std::string { LogLevelName(value) };
    }
    [[nodiscard]] std::string FlagValue(StorageDurability value)
    {
        return std::string { DurabilityName(value) };
    }
    [[nodiscard]] std::string FlagValue(LruRecency value)
    {
        return std::string { LruRecencyName(value) };
    }
    [[nodiscard]] std::string FlagValue(CpuAffinity value)
    {
        return std::string { CpuAffinityName(value) };
    }
    [[nodiscard]] std::string FlagValue(CompressionCodec value)
    {
        return std::string { Compression::NameOf(value) };
    }

    template <typename T>
        requires std::integral<T>
    [[nodiscard]] std::string FlagValue(T value)
    {
        return std::format("{}", value);
    }
    /// @}
} // namespace

std::vector<std::string> BuildServiceArgv(std::filesystem::path const& exePath,
                                          CliResult const& cli,
                                          EmitDaemonFlag daemonFlag)
{
    Config const& cfg = cli.config;
    std::vector<std::string> argv { exePath.string() };

    // Emit `--flag=value` for every flag the operator NAMED, so the recorded
    // launch arguments stay minimal and still say everything that was asked for.
    // The rule is written once here; the table below is pure data.
    //
    // **Provenance, never value** (#349). Comparing against a default-constructed
    // Config drops a pin that happens to equal the default, and a default derived
    // from the host is not a constant: `--max-memory=8g` on a 32 GiB machine IS
    // the default, so the flag vanished from the registration and the service
    // re-derived a budget from RAM at every start. Nothing reported it.
    auto const emitIfExplicit = [&argv](std::string_view flag, auto const& value, bool wasTyped) {
        if (wasTyped)
            argv.push_back(std::format("--{}={}", flag, FlagValue(value)));
    };

    // A path is absolutized because a service does not inherit the installing
    // shell's working directory -- but only a NON-EMPTY one, because
    // `std::filesystem::absolute("")` IS that directory, and absolutizing first
    // would silently pin the service to whatever shell happened to register it.
    //
    // That is a constraint on the absolutizer, and it is **not** a reason to decide
    // by value. `ParseText` never fails, so `--storage=` is a reachable operator
    // instruction and a real pin: `--install-service --config=f.yaml --storage=`
    // against a file carrying `storage_path:` must not come back persisting to
    // disk at every start. So a path row that records provenance asks it like every
    // other row, and an explicitly empty value is emitted VERBATIM -- which
    // round-trips through this project's own parser to the empty value the operator
    // named.
    auto const emitPathIfExplicit = [&argv](std::string_view flag, std::string const& value, bool wasTyped) {
        if (!wasTyped)
            return;
        argv.push_back(value.empty() ? std::format("--{}=", flag) : std::format("--{}={}", flag, AbsolutePathArg(value)));
    };

    // `--config` and `--pidfile` carry no explicit bit at all and so cannot ask.
    // They keep a presence test, and for them an empty value really does name
    // nothing there is any way to register.
    auto const emitPathIfSet = [&argv](std::string_view flag, std::string const& value) {
        if (!value.empty())
            argv.push_back(std::format("--{}={}", flag, AbsolutePathArg(value)));
    };

    // Valueless switches: present or absent, never `--flag=true`, because that
    // is not a spelling CliParser accepts.
    //
    // One helper for both kinds, because provenance consults no default and the
    // only thing separating them is whether a negative spelling exists -- which is
    // an argument. An empty @p offFlag says this switch has none; see
    // `SwitchSpellingFor`, which carries why the two used to be separate.
    auto const emitSwitch = [&argv](std::string_view onFlag, std::string_view offFlag, bool value, bool wasTyped) {
        if (auto const spelling = SwitchSpellingFor(onFlag, offFlag, value, wasTyped); spelling.has_value())
            argv.emplace_back(std::format("--{}", *spelling));
    };

    if (daemonFlag == EmitDaemonFlag::Yes)
        argv.emplace_back("--daemon");

    // Unconditional: it lets the running service identify itself, and on launchd
    // it is what LaunchdLabel derives the job label from.
    argv.push_back(std::format("--service-name={}", cfg.serviceName));

    // Every Config field that has a CLI spelling appears below. The table is
    // exhaustive on purpose: when it covered only a subset, an operator could
    // run `--install-service --requirepass=… --tls …`, be told the service was
    // installed, and get a daemon serving in plaintext with no authentication —
    // the flags were dropped between the command line and the supervisor with
    // nothing reporting it. The only CLI flags deliberately absent are the ones
    // that are not Config state: --install-service / --uninstall-service (a
    // service must never re-install itself), --service-scope (install-time
    // only), --daemon (handled above) and --healthcheck / --help / --version.
    //
    //             flag                    current value                  named by the operator
    emitIfExplicit("bind", cfg.bindAddress, cli.bindAddressExplicit);
    emitIfExplicit("port", cfg.port, cli.portExplicit);
    emitIfExplicit("max-memory", cfg.maxMemoryBytes, cli.maxMemoryBytesExplicit);
    emitIfExplicit("log-level", cfg.logLevel, cli.logLevelExplicit);
    emitSwitch("log-timestamps", "no-log-timestamps", cfg.logTimestamps, cli.logTimestampsExplicit);
    emitSwitch("log-source", {}, cfg.logSource, cli.logSourceExplicit);
    emitSwitch("log-everything", {}, cfg.logEverything, cli.logEverythingExplicit);
    emitPathIfExplicit("storage", cfg.storagePath, cli.storagePathExplicit);
    emitIfExplicit("storage-durability", cfg.storageDurability, cli.storageDurabilityExplicit);
    emitIfExplicit("storage-max-value", cfg.storageMaxValueBytes, cli.storageMaxValueBytesExplicit);
    emitIfExplicit("storage-max-disk", cfg.storageMaxDiskBytes, cli.storageMaxDiskBytesExplicit);
    emitIfExplicit("storage-shards", cfg.storageShards, cli.storageShardsExplicit);
    emitIfExplicit("expiry-interval", cfg.activeExpiryIntervalMs, cli.activeExpiryIntervalMsExplicit);
    emitIfExplicit("expiry-scan", cfg.activeExpiryScanBudget, cli.activeExpiryScanBudgetExplicit);
    emitIfExplicit("threads", cfg.workerThreads, cli.workerThreadsExplicit);
    emitIfExplicit("cpu-affinity", cfg.cpuAffinity, cli.cpuAffinityExplicit);
    emitIfExplicit("lru-mode", cfg.lruRecency, cli.lruRecencyExplicit);
    emitIfExplicit("listen-backlog", cfg.listenBacklog, cli.listenBacklogExplicit);
    emitIfExplicit("compression", cfg.compression, cli.compressionExplicit);
    emitIfExplicit("compression-level", cfg.compressionLevel, cli.compressionLevelExplicit);
    emitIfExplicit("compression-min-bytes", cfg.compressionMinBytes, cli.compressionMinBytesExplicit);
    emitIfExplicit("memory-compression", cfg.memoryCompression, cli.memoryCompressionExplicit);
    emitIfExplicit("memory-compression-level", cfg.memoryCompressionLevel, cli.memoryCompressionLevelExplicit);
    emitIfExplicit("memory-compression-min-bytes", cfg.memoryCompressionMinBytes, cli.memoryCompressionMinBytesExplicit);
    emitIfExplicit("auth-username", cfg.authUsername, cli.authUsernameExplicit);
    emitSwitch("tls", {}, cfg.tlsEnabled, cli.tlsEnabledExplicit);
    emitPathIfExplicit("tls-cert", cfg.tlsCertPath, cli.tlsCertPathExplicit);
    emitPathIfExplicit("tls-key", cfg.tlsKeyPath, cli.tlsKeyPathExplicit);
    emitSwitch("metrics", {}, cfg.metricsEnabled, cli.metricsEnabledExplicit);
    emitIfExplicit("metrics-bind", cfg.metricsBindAddress, cli.metricsBindAddressExplicit);
    emitIfExplicit("metrics-port", cfg.metricsPort, cli.metricsPortExplicit);
    emitIfExplicit("notify-keyspace-events", cfg.notifyKeyspaceEvents, cli.notifyKeyspaceEventsExplicit);
    emitPathIfSet("pidfile", cfg.pidfile);
    emitPathIfSet("config", cfg.configPath);

    // Repeated flags, one token per listener, so a multi-endpoint daemon comes
    // back with the same listener set it was installed with. The address goes
    // through FormatListenHost so what is emitted is what ParseListenSpec
    // accepts — these tokens are re-parsed by the daemon at every start.
    for (auto const& bind: cfg.binds)
        argv.push_back(std::format("--{}={}:{}", ListenFlagFor(bind), FormatListenHost(bind.address), bind.port));

    // requirePass is deliberately NOT here, and its omission is reported rather
    // than silent: see InlineCredentialRejection.
    return argv;
}

ServiceSpec MakeDaemonServiceSpec(std::filesystem::path const& exePath, CliResult const& cli)
{
    Config const& cfg = cli.config;

    // The dedicated account a system-wide job runs as, created by the package's
    // postinstall. Mirrors the `fastcached` user in
    // packaging/linux/fastcached.sysusers. It lives here, with the daemon's own
    // spec, rather than beside the launchd code that used to hardcode it: which
    // account a service runs as is a property of the service, and a second
    // binary registering itself may well want a different one.
    constexpr std::string_view DaemonServiceAccount = "_fastcached";

    // EmitDaemonFlag::No, and the flag carried separately: the spec has to be
    // able to answer both supervisors, and only the SCM wants backgrounding.
    auto argv = BuildServiceArgv(exePath, cli, EmitDaemonFlag::No);

    // argv[0] is the executable, which the spec holds in its own field.
    argv.erase(argv.begin());

    std::vector<std::filesystem::path> owned;
    if (!cfg.storagePath.empty())
        owned.emplace_back(cfg.storagePath);

    return ServiceSpec { .serviceName = cfg.serviceName,
                         .exePath = exePath,
                         .arguments = std::move(argv),
                         .daemonFlag = "--daemon",
                         .displayName = "fastcached",
                         .description = "fastcached — fast cache daemon",
                         .serviceAccount = std::string { DaemonServiceAccount },
                         .ownedPaths = std::move(owned),
                         .inlineCredential = cfg.requirePass.empty() ? InlineCredential::Absent : InlineCredential::Present,
                         .configPath = cfg.configPath,
                         // The daemon IS the file-configured service: it probes a
                         // machine-wide fastcached.yaml at every start and keeps a
                         // per-user cache under this name. WithScopeDefaults may
                         // therefore fill in a --config or --storage the operator
                         // left unset; a service naming nothing gets neither.
                         .applicationName = std::string { DaemonApplicationName },
                         // Named rather than left to the SCM's default, which is
                         // LocalSystem: unrestricted access to every local resource
                         // and a member of the local Administrators group. A cache
                         // daemon listening on a socket has no use for any of it.
                         //
                         // The account costs nothing to create because the SCM
                         // derives it from the service name and manages it itself,
                         // and it can already read what it needs: the
                         // %ProgramData%\fastcached access list grants BUILTIN\Users
                         // read and execute, and a virtual account is an
                         // Authenticated User. What it cannot do is WRITE there,
                         // which is the point -- a service that cannot rewrite its
                         // own configuration cannot be made to load a different one.
                         .windowsLogon = WindowsLogonAccount::VirtualAccount };
}

std::optional<std::string> InlineCredentialRejection(ServiceSpec const& spec)
{
    if (spec.inlineCredential == InlineCredential::Absent)
        return std::nullopt;

    // A supervisor records its launch arguments in a file every local user can
    // read (`/Library/LaunchDaemons/*.plist`, the SCM's ImagePath), so baking a
    // shared secret into them would publish it to exactly the accounts
    // --requirepass exists to keep out. The config file can be mode 0640, so
    // that is where the secret belongs; the daemon reads it at every start and
    // on every reload. Refusing is the only honest option: emitting the flag
    // leaks it, and dropping it silently installs an unauthenticated daemon
    // while reporting success.
    constexpr std::string_view Preamble =
        "--requirepass cannot be baked into a service's launch arguments: they are world-readable. "
        "Put `requirepass:` in a config file (mode 0640, readable by the account the service runs as) "
        "and install with --config=<path> instead.";

    if (spec.configPath.empty())
        return std::string { Preamble };

    // --config does not make the combination safe. Nothing here can tell
    // whether the named file carries `requirepass:` — the installer-seeded YAML
    // does not — so accepting it was the silent drop wearing a different hat:
    // the daemon came up unauthenticated while the operator was told the
    // password had been registered.
    return std::format("{} You passed --config={}, so put it there and drop --requirepass from this command line.",
                       Preamble,
                       spec.configPath);
}

std::optional<std::string> ServiceNameRejection(ServiceSpec const& spec)
{
    // The service name is not merely a label. LaunchdPlistPath concatenates it
    // into `<LaunchAgents|LaunchDaemons>/<label>.plist`, and the SCM keys its
    // registry entry on it — so a '/' or a ".." escapes the directory the
    // supervisor scans, writing a root-owned file somewhere no uninstall path
    // knows about. Even the benign case bites: one stray separator puts the
    // plist where launchd's directory scan never looks, so the job never comes
    // back after a reboot while the install reported success.
    // A deny-list rather than an allow-list, deliberately: the SCM has accepted
    // names with spaces and punctuation since forever, and tightening that here
    // would break existing Windows registrations to fix a traversal that only
    // separators and dots can express.
    if (spec.serviceName.empty())
        return "--service-name must not be empty";

    if (spec.serviceName.find_first_of("/\\") != std::string::npos)
        return std::format("--service-name must not contain a path separator: '{}'", spec.serviceName);

    // A leading dot covers `..` — the traversal that needs no separator — and a
    // dotfile, which launchd's directory scan skips, so the job would silently
    // never come back after a reboot.
    if (spec.serviceName.starts_with('.'))
        return std::format("--service-name must not start with '.': '{}'", spec.serviceName);

    if (std::ranges::any_of(spec.serviceName, [](unsigned char ch) { return std::iscntrl(ch) != 0; }))
        return std::format("--service-name must not contain control characters: '{}'", spec.serviceName);

    return std::nullopt;
}

std::optional<std::string> ServiceRegistrationRejection(ServiceSpec const& spec)
{
    // A table, so a new rule is a new row rather than another `if` threaded
    // through both platforms' InstallService.
    using Validator = std::optional<std::string> (*)(ServiceSpec const&);
    constexpr auto Validators = std::to_array<Validator>({ &ServiceNameRejection, &InlineCredentialRejection });

    for (auto const validator: Validators)
        if (auto rejection = validator(spec))
            return rejection;

    return std::nullopt;
}

std::string BuildServiceCommandLine(ServiceSpec const& spec)
{
    // The executable path is always quoted so an install directory containing
    // spaces (e.g. "C:\Program Files\fastcached") tokenizes correctly.
    std::string out = std::format("\"{}\"", spec.exePath.string());

    // The SCM is the supervisor that wants backgrounding, so this is where the
    // spec's daemon flag is spent. At the FRONT, which keeps the registered
    // command line byte-identical to the one this produced before the arguments
    // moved into a ServiceSpec -- nothing downstream cares about flag order, and
    // a refactor that can be shown to change nothing is worth more than one that
    // merely ought not to.
    auto argv = spec.arguments;
    if (!spec.daemonFlag.empty())
        argv.insert(argv.begin(), spec.daemonFlag);

    for (auto const& arg: argv)
    {
        // Quote the value, not the whole token: the SCM splits `--flag=value`
        // on whitespace, so `--storage="C:\a b"` is one argument while
        // `"--storage=C:\a b"` would reach the daemon with the quotes attached.
        auto const eq = arg.find('=');
        if (eq == std::string::npos)
            out += std::format(" {}", arg);
        else
            out += std::format(" {}={}", arg.substr(0, eq), MaybeQuote(arg.substr(eq + 1)));
    }

    return out;
}

// ----------------------------------------------------------------------------
// launchd

namespace
{
    /// Reverse-DNS prefix of the launchd job label. Paired with the package's
    /// CPACK_PRODUCTBUILD_IDENTIFIER, which must stay identical: the uninstaller
    /// derives the receipt names to forget from it.
    constexpr std::string_view LaunchdLabelPrefix = "software.lastrada.";

    /// Everything that differs between the two launchd domains, as data. A third
    /// scope would be a row here, not a new branch threaded through the
    /// functions below.
    struct ScopeTraits
    {
        ServiceScope scope;
        std::string_view name;           ///< CLI spelling, for --service-scope.
        std::string_view plistDirectory; ///< Absolute, or relative to $HOME when @ref homeRelative.
        /// launchctl domain targets, most preferred first; `{}` is filled with
        /// the uid and an empty entry means "no further candidate".
        std::array<std::string_view, 2> domains;
        bool homeRelative;         ///< plistDirectory is relative to the user's home.
        bool alwaysKeepAlive;      ///< KeepAlive=<true/> vs {Crashed:true}; see below.
        bool runsAsServiceAccount; ///< Emit UserName (the group follows from it).
    };

    constexpr auto ScopeTable = std::to_array<ScopeTraits>({
        // A user agent uses KeepAlive={Crashed:true} rather than <true/>: a second
        // logged-in user's agent loses the race for 127.0.0.1:6674 and exits
        // cleanly with a non-zero status, which <true/> would treat as "restart
        // it", producing a permanent 10-second crash loop. {Crashed:true} restarts
        // only on a signal, so a lost port becomes one log line.
        ScopeTraits { .scope = ServiceScope::User,
                      .name = "user",
                      .plistDirectory = "Library/LaunchAgents",
                      // Two candidates, tried in order. `gui/<uid>` is the Aqua
                      // session and is the right home for a desktop agent, but
                      // it does not exist over SSH, at the login window, or on
                      // a headless runner — where bootstrapping into it fails
                      // with "Bootstrap failed: 5: Input/output error" and the
                      // install has nowhere else to go. `user/<uid>` always
                      // exists, so it is the fallback rather than the default.
                      .domains = { "gui/{}", "user/{}" },
                      .homeRelative = true,
                      .alwaysKeepAlive = false,
                      .runsAsServiceAccount = false },
        // The system daemon is the singleton owner of the port, so an unexpected
        // exit really should be restarted.
        ScopeTraits { .scope = ServiceScope::System,
                      .name = "system",
                      .plistDirectory = "/Library/LaunchDaemons",
                      .domains = { "system", "" },
                      .homeRelative = false,
                      .alwaysKeepAlive = true,
                      .runsAsServiceAccount = true },
    });

    /// The row describing @p scope.
    ///
    /// FindOrNull rather than std::ranges::find because ScopeTable is a
    /// std::array, whose iterator type is not portably nameable; see
    /// FastCache/Core/Ranges.hpp.
    [[nodiscard]] constexpr ScopeTraits const& TraitsOf(ServiceScope scope) noexcept
    {
        auto const* const traits = FindOrNull(ScopeTable, scope, &ScopeTraits::scope);
        return traits != nullptr ? *traits : ScopeTable.front();
    }

    /// XML-escape @p text for use as a plist text node.
    ///
    /// Not cosmetic: a storage path containing `&` (legal on every filesystem
    /// fastcached supports) produces a malformed document, and launchd rejects
    /// the whole job without saying why.
    [[nodiscard]] std::string XmlEscape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (auto const ch: text)
        {
            switch (ch)
            {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                case '"':
                    out += "&quot;";
                    break;
                case '\'':
                    out += "&apos;";
                    break;
                default:
                    out += ch;
                    break;
            }
        }
        return out;
    }
} // namespace

std::expected<ServiceScope, ConfigError> ParseServiceScope(std::string_view text)
{
    if (auto const* const traits = FindOrNull(ScopeTable, text, &ScopeTraits::name))
        return traits->scope;

    return std::unexpected(
        ConfigError { .code = ConfigErrorCode::ParseError,
                      .source = "argv",
                      .field = "service-scope",
                      .context = std::format("unknown service scope '{}'; expected user or system", text) });
}

std::string_view ServiceScopeName(ServiceScope scope) noexcept
{
    return TraitsOf(scope).name;
}

std::string LaunchdLabel(ServiceSpec const& spec)
{
    auto lowered = spec.serviceName;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return std::format("{}{}", LaunchdLabelPrefix, lowered);
}

std::filesystem::path LaunchdPlistPath(ServiceSpec const& spec,
                                       ServiceScope scope,
                                       std::filesystem::path const& homeDirectory)
{
    auto const& traits = TraitsOf(scope);
    auto const fileName = std::format("{}.plist", LaunchdLabel(spec));

    if (traits.homeRelative)
        return homeDirectory / traits.plistDirectory / fileName;
    return std::filesystem::path { traits.plistDirectory } / fileName;
}

std::string BuildLaunchdPlist(ServiceSpec const& spec, ServiceScope scope, std::filesystem::path const& logDirectory)
{
    auto const& traits = TraitsOf(scope);
    auto const label = LaunchdLabel(spec);

    // `spec.daemonFlag` is deliberately not emitted, and that is the whole
    // point of holding it apart: launchd supervises the process it started, and
    // a job that double-forks is reaped instantly as "exited".
    std::string arguments = std::format("        <string>{}</string>\n", XmlEscape(spec.exePath.string()));
    for (auto const& arg: spec.arguments)
        arguments += std::format("        <string>{}</string>\n", XmlEscape(arg));

    // ProcessType is not optional for a cache daemon: launchd gives a job with
    // no declared type the "Background" resource band, which throttles its CPU
    // and I/O. NumberOfFiles is raised because a launchd job inherits a soft
    // limit of 256 descriptors, far below what a connection-per-client server
    // needs — an interactive shell gets 1048576 and would never show the bug.
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    out += "<plist version=\"1.0\">\n";
    out += "<dict>\n";
    out += std::format("    <key>Label</key>\n    <string>{}</string>\n", XmlEscape(label));
    out += std::format("    <key>ProgramArguments</key>\n    <array>\n{}    </array>\n", arguments);
    out += "    <key>RunAtLoad</key>\n    <true/>\n";

    if (traits.alwaysKeepAlive)
        out += "    <key>KeepAlive</key>\n    <true/>\n";
    else
        out += "    <key>KeepAlive</key>\n    <dict>\n"
               "        <key>Crashed</key>\n        <true/>\n"
               "    </dict>\n";

    if (traits.runsAsServiceAccount && !spec.serviceAccount.empty())
    {
        // UserName only. launchd already runs the job under that account's
        // primary group when GroupName is absent, so naming it adds nothing —
        // except a second name to resolve, and a job whose group cannot be
        // resolved does not fail: launchd parks it in "spawn scheduled"
        // indefinitely, and the `kickstart` that waits for the spawn hangs with
        // it until something kills the installer. One name to resolve is one
        // failure mode, not two.
        out += std::format("    <key>UserName</key>\n    <string>{}</string>\n", spec.serviceAccount);
    }

    out += "    <key>ProcessType</key>\n    <string>Interactive</string>\n";
    out += "    <key>ThrottleInterval</key>\n    <integer>30</integer>\n";
    out += "    <key>ExitTimeOut</key>\n    <integer>30</integer>\n";
    out += "    <key>SoftResourceLimits</key>\n    <dict>\n"
           "        <key>NumberOfFiles</key>\n        <integer>8192</integer>\n"
           "    </dict>\n";
    out += std::format("    <key>StandardOutPath</key>\n    <string>{}</string>\n",
                       XmlEscape((logDirectory / std::format("{}.out.log", label)).string()));
    out += std::format("    <key>StandardErrorPath</key>\n    <string>{}</string>\n",
                       XmlEscape((logDirectory / std::format("{}.err.log", label)).string()));
    out += "</dict>\n";
    out += "</plist>\n";

    return out;
}

namespace
{
    /// Whether @p spec already carries an argument introduced by @p flag.
    ///
    /// "The operator set this" is exactly "BuildServiceArgv emitted it", because
    /// that function emits a flag only for one the operator NAMED -- so asking the
    /// argument list is asking the same question the Config test used to, one layer
    /// further along, and since #349 it is asking it of provenance rather than of a
    /// value that might coincide with a default.
    /// @param spec Service being registered.
    /// @param flag Flag prefix including its `=`, e.g. `--config=`.
    /// @return True when the flag is already present.
    [[nodiscard]] bool HasArgument(ServiceSpec const& spec, std::string_view flag)
    {
        return std::ranges::any_of(spec.arguments, [flag](std::string const& arg) { return arg.starts_with(flag); });
    }
} // namespace

namespace
{
/// Root of the macOS package payload, e.g. `/opt/fastcached`.
///
/// Handed down from FASTCACHED_MACOS_PREFIX by src/FastCache/CMakeLists.txt
/// so the daemon's idea of its own install tree cannot drift from the one
/// the installer actually lays down. The fallback matters for builds that
/// compile this file outside the package configuration (a plain
/// `cmake --install --prefix /usr/local`, or the test binary on a
/// non-Apple CI toolchain), where no prefix has been chosen.
#if defined(FC_MACOS_PREFIX)
    constexpr std::string_view MacOsPrefix = FC_MACOS_PREFIX;
#else
    constexpr std::string_view MacOsPrefix = "/opt/fastcached";
#endif

} // namespace

std::optional<std::string> WindowsLogonName(ServiceSpec const& spec)
{
    // The SCM derives the virtual account from the service name itself and creates
    // it on demand, so this is a spelling rather than a lookup -- and it is why a
    // virtual account needs no password and no installer step. It only resolves
    // AFTER CreateService has run; nothing may ACL against it before then.
    if (spec.windowsLogon == WindowsLogonAccount::VirtualAccount)
        return std::format("NT SERVICE\\{}", spec.serviceName);

    // LocalSystem is spelled by naming nobody. Returning "LocalSystem" would also
    // work, but nullopt keeps the one caller passing the literal nullptr the API
    // documents rather than a string that has to be right.
    return std::nullopt;
}

std::filesystem::path DefaultLogDirectory(std::string_view label, ServiceScope scope, std::filesystem::path const& home)
{
    if (scope == ServiceScope::User)
        return home / "Library/Logs/fastcached";
    return std::filesystem::path { MacOsPrefix } / "var/log" / label;
}

std::optional<std::string_view> SwitchSpellingFor(std::string_view onFlag,
                                                  std::string_view offFlag,
                                                  bool value,
                                                  bool wasTyped) noexcept
{
    if (!wasTyped)
        return std::nullopt;

    auto const spelling = value ? onFlag : offFlag;

    // A one-sided switch asked to say "off" has nothing to say. Unreachable from
    // argv -- `--metrics` and its siblings can only set true -- but it is the one
    // shape this function cannot express, so it answers rather than emitting a
    // bare `--`.
    if (spelling.empty())
        return std::nullopt;
    return spelling;
}

ServiceSpec WithScopeDefaults(ServiceSpec spec,
                              ServiceScope scope,
                              std::filesystem::path const& home,
                              std::filesystem::path const& packagedConfig)
{
    // A service that names no application keeps no files, and must be given no
    // flags about them. This is the whole gate, and it is checked before either
    // default rather than inside them, because both defaults exist only because
    // the daemon has a config file and a cache to default.
    //
    // `fastcache-compile-node` is that service: it is configured entirely from
    // argv and its parser rejects both `--storage=` and `--config=`. Appending
    // either registered a job that refused its own command line at every start --
    // reported installed, and dead at every boot. That is the failure this
    // codebase refuses at install time, produced BY the installer.
    if (spec.applicationName.empty())
        return spec;

    // A LaunchAgent that kept the in-memory default would lose the whole cache
    // on every logout, which for a compile cache is most of the value. launchd
    // expands neither `~` nor `$HOME` in ProgramArguments, so the concrete path
    // has to be resolved here, at install time, by the process that knows it.
    //
    // The default is applied only when the caller named **no** config file. A
    // CLI value baked into ProgramArguments outranks YAML in Merge, so
    // injecting one alongside `--config` would override the `storage_path` in
    // that very file for the life of the registration -- the operator edits it,
    // kickstarts the job, and nothing changes, with no error anywhere. Whoever
    // passes a config file owns the storage path in it.
    //
    // System scope gets no storage default at all: its config file is always
    // the package's `<prefix>/etc/fastcached.yaml`, so there is nothing to
    // default.
    if (scope == ServiceScope::User && !HasArgument(spec, "--storage=") && !HasArgument(spec, "--config="))
    {
        // One appended component, not three. `home / "Library/Caches" / app /
        // "cache"` renders with the platform's separator between each, so the
        // value this bakes into a registration would stop being the one the
        // previous release baked in -- and this is a live path an existing agent
        // is already pointed at. Identical output is worth more than the tidier
        // spelling.
        auto const storage = home / std::format("Library/Caches/{}/cache", spec.applicationName);
        spec.arguments.push_back(std::format("--storage={}", storage.string()));
        spec.ownedPaths.emplace_back(storage);
    }

    // The daemon would find this file on its own -- it is the machine-wide
    // candidate its startup lookup probes -- so registering it is a choice,
    // not a necessity, and it is made for two reasons. ServiceAccountReadDenial
    // validates `configPath`, so leaving it empty would demote "the
    // _fastcached account cannot read the config" from an install-time error
    // to a silent fall-through to built-in defaults. And a system job whose
    // HOME resolves somewhere real would otherwise prefer a per-user file
    // over the machine-wide one, which is backwards for a daemon.
    //
    // System scope only: that file describes the machine-wide daemon (its
    // cache lives under the package prefix, writable by the service account
    // alone), so handing it to a per-user agent would point the agent at a
    // directory it cannot write.
    //
    // An empty packagedConfig means it is not actually there, so a
    // build-from-source install does not point launchd at a missing path.
    if (scope == ServiceScope::System && !HasArgument(spec, "--config=") && !packagedConfig.empty())
    {
        spec.arguments.push_back(std::format("--config={}", packagedConfig.string()));
        spec.configPath = packagedConfig.string();
    }

    return spec;
}

#if defined(_WIN32)

namespace
{
    /// One-line description registered with the SCM (shown in services.msc).
    constexpr std::string_view ServiceDescription = "fastcached — fast cache daemon";

    /// Standard "needs elevation" guidance reused across SCM error paths.
    [[nodiscard]] std::string ElevationHint(std::string_view action)
    {
        return std::format("access denied {}; run from an elevated (Administrator) prompt", action);
    }

    /// Frees a `LocalAlloc`ed block, so the ACL paths below cannot leak one on an
    /// early return. There are two such blocks per call and four ways out.
    struct LocalDeleter
    {
        /// @param block Block to release; null is a no-op, as LocalFree allows.
        void operator()(void* block) const noexcept
        {
            (void) ::LocalFree(block);
        }
    };
    using LocalPtr = std::unique_ptr<void, LocalDeleter>;

    /// Where the Application event log records a provider by this name.
    /// @param serviceName The service, which is also its event source.
    /// @return The registry path, relative to `HKEY_LOCAL_MACHINE`.
    [[nodiscard]] std::string EventSourceKeyPath(std::string const& serviceName)
    {
        return std::format(R"(SYSTEM\CurrentControlSet\Services\EventLog\Application\{})", serviceName);
    }

    /// Register @p serviceName as an event source, so its records render as text.
    ///
    /// Without this the events are still written and still readable, but each one
    /// arrives wrapped in "The description for Event ID 1 ... cannot be found",
    /// because the log resolves message ids against a resource this project does not
    /// ship. Pointing at `EventCreate.exe` borrows the one Windows already has whose
    /// first message is the passthrough `%1` -- the same resource `eventcreate.exe`
    /// itself relies on -- so the single insertion string renders verbatim and no
    /// message DLL has to be built, signed and installed to say `%1`.
    ///
    /// @param serviceName The service, which is also its event source.
    /// @return An explanatory message on failure, else nullopt.
    [[nodiscard]] std::optional<std::string> RegisterEventSourceForService(std::string const& serviceName)
    {
        auto const path = EventSourceKeyPath(serviceName);
        HKEY key = nullptr;
        if (auto const rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                            path.c_str(),
                                            0,
                                            nullptr,
                                            REG_OPTION_NON_VOLATILE,
                                            KEY_SET_VALUE,
                                            nullptr,
                                            &key,
                                            nullptr);
            rc != ERROR_SUCCESS)
            return std::format("could not register the event source (error {}); this service's log lines will be "
                               "written but shown without their text",
                               rc);

        // REG_EXPAND_SZ: the value carries `%SystemRoot%` and the log expands it.
        constexpr std::string_view MessageFile = R"(%SystemRoot%\System32\EventCreate.exe)";
        auto rc = RegSetValueExA(key,
                                 "EventMessageFile",
                                 0,
                                 REG_EXPAND_SZ,
                                 reinterpret_cast<BYTE const*>(MessageFile.data()),
                                 static_cast<DWORD>(MessageFile.size() + 1));

        // Error | Warning | Information -- the three `EventTypeFor` maps onto.
        constexpr DWORD TypesSupported = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
        if (rc == ERROR_SUCCESS)
            rc = RegSetValueExA(key,
                                "TypesSupported",
                                0,
                                REG_DWORD,
                                reinterpret_cast<BYTE const*>(&TypesSupported),
                                static_cast<DWORD>(sizeof(TypesSupported)));
        RegCloseKey(key);

        if (rc != ERROR_SUCCESS)
            return std::format("could not describe the event source (error {}); this service's log lines will be "
                               "written but shown without their text",
                               rc);
        return std::nullopt;
    }

    /// Give @p account full control of @p target, creating it as a directory if
    /// absent.
    ///
    /// The Windows counterpart of the `chown` the launchd path does, and needed for
    /// the same reason: a service that no longer runs as the machine's most
    /// privileged identity cannot write a directory the installer created as an
    /// administrator. LocalSystem never noticed because LocalSystem can write
    /// anywhere.
    ///
    /// Created only when it is absent AND meant to be a directory; whatever is
    /// already there is granted as-is. `SE_FILE_OBJECT` covers a file and a
    /// directory alike, so the grant itself does not care -- the *create* does.
    /// A `ServiceSpec::ownedPaths` entry may be one CoW file (`storage_path` is
    /// allowed to name `cache.cow`), and an unconditional `create_directories`
    /// failed in both directions on it: on a fresh install it made a DIRECTORY the
    /// daemon then read as a directory of shards, and once the file existed it
    /// failed and skipped the grant -- on exactly the upgrade this exists for.
    ///
    /// The entry is ADDED to the existing list rather than replacing it, so an
    /// administrator keeps the access they had -- a replaced list is how a
    /// directory becomes one only the service can repair.
    ///
    /// @param target Path to grant access to; created as a directory when absent
    ///        and not named as a file.
    /// @param account Trustee name, e.g. `NT SERVICE\FastCacheCompileNode`. It
    ///        resolves only once the service exists, so call this after
    ///        CreateService.
    /// @return An explanatory message on failure, else nullopt.
    [[nodiscard]] std::optional<std::string> GrantPathAccess(std::filesystem::path const& target, std::string const& account)
    {
        std::error_code ec;
        if (!std::filesystem::exists(target, ec))
        {
            if (PathNamesAFile(target))
                return std::format("{} does not exist yet and names a file, so it was not created; grant "
                                   "'{}' access to the directory that will hold it once it is there",
                                   target.string(),
                                   account);
            // Its own error_code, because the probe below CLEARS one: a
            // successful "no, it is not there" is a successful status query, so
            // reusing `ec` overwrote the reason with a success and the operator
            // was told "could not create <path>: The operation completed
            // successfully" for an access-denied create.
            std::error_code createEc;
            std::filesystem::create_directories(target, createEc);
            if (std::error_code probeEc; !std::filesystem::exists(target, probeEc))
                return std::format("could not create {}: {}",
                                   target.string(),
                                   createEc ? createEc.message() : std::string { "it is still not there" });
        }

        auto path = target.string();

        PACL current = nullptr;
        PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
        if (auto const rc = ::GetNamedSecurityInfoA(path.c_str(),
                                                    SE_FILE_OBJECT,
                                                    DACL_SECURITY_INFORMATION,
                                                    nullptr,
                                                    nullptr,
                                                    &current,
                                                    nullptr,
                                                    &rawDescriptor);
            rc != ERROR_SUCCESS)
            return std::format("could not read the access list of {} (error {})", path, rc);
        LocalPtr const descriptor { rawDescriptor };

        // A copy: EXPLICIT_ACCESS takes a mutable pointer, and the API is ANSI here
        // to match the rest of this file's SCM calls.
        auto trustee = account;

        EXPLICIT_ACCESS_A entry {};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = GRANT_ACCESS;
        // Inherited by what the service later creates inside, or the grant covers
        // the directory and nothing the service puts in it.
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = trustee.data();

        PACL rawUpdated = nullptr;
        if (auto const rc = ::SetEntriesInAclA(1, &entry, current, &rawUpdated); rc != ERROR_SUCCESS)
            return std::format("could not grant '{}' access to {} (error {})", account, path, rc);
        LocalPtr const updated { rawUpdated };

        if (auto const rc = ::SetNamedSecurityInfoA(
                path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, rawUpdated, nullptr);
            rc != ERROR_SUCCESS)
            return std::format("could not apply the access list of {} (error {})", path, rc);

        return std::nullopt;
    }
} // namespace

ServiceControlResult InstallService(ServiceSpec const& spec, ServiceScope /*scope*/)
{
    if (auto const rejection = ServiceRegistrationRejection(spec))
        return { .exitCode = 1, .message = *rejection };

    auto const exe = CurrentExecutablePath();
    if (exe.empty())
        return { .exitCode = 1, .message = "could not determine the fastcached executable path" };

    auto const commandLine = BuildServiceCommandLine(spec);
    auto const logonName = WindowsLogonName(spec);

    SC_HANDLE const manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr)
    {
        auto const err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("opening the service manager") };
        return { .exitCode = 1, .message = std::format("OpenSCManager failed (error {})", err) };
    }

    SC_HANDLE const service = CreateServiceA(manager,
                                             spec.serviceName.c_str(),
                                             spec.serviceName.c_str(),
                                             SERVICE_ALL_ACCESS,
                                             SERVICE_WIN32_OWN_PROCESS,
                                             SERVICE_AUTO_START,
                                             SERVICE_ERROR_NORMAL,
                                             commandLine.c_str(),
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             // lpServiceStartName. Naming nobody is
                                             // LocalSystem -- the whole machine --
                                             // which fastcache-compile-node must
                                             // not have: it compiles input that
                                             // arrived over the network.
                                             logonName ? logonName->c_str() : nullptr,
                                             // No password. A virtual account has
                                             // none, and LocalSystem takes none.
                                             nullptr);
    if (service == nullptr)
    {
        auto const err = GetLastError();
        CloseServiceHandle(manager);
        if (err == ERROR_SERVICE_EXISTS)
            return { .exitCode = 1,
                     .message = std::format("service '{}' already exists; remove it first with --uninstall-service",
                                            spec.serviceName) };
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("creating the service") };
        return { .exitCode = 1, .message = std::format("CreateService failed (error {})", err) };
    }

    // Best-effort friendly description; failure here does not fail the install.
    std::string description { spec.description };
    SERVICE_DESCRIPTIONA descriptor { .lpDescription = description.data() };
    ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &descriptor);

    // What the other two supervisors already say, said here. Without it a node that
    // crashes stays stopped until somebody notices, on a machine whose entire point
    // is being always-on -- `sc qfailure` reported an empty policy on every
    // registration this project has ever made.
    //
    // The numbers are the other two's, not new ones: systemd's units carry
    // `Restart=on-failure` with `RestartSec=1`, and the launchd job this file
    // generates carries `KeepAlive` with `ThrottleInterval=30`. So the first two
    // attempts are immediate-ish and the third backs off to thirty seconds -- and
    // the third repeats, because Windows applies the LAST action to every failure
    // past the end of the array. An hour of health resets the count, which is the
    // start-limit window the systemd units rely on, spelled the way this API spells
    // it.
    //
    // NOT a `ServiceSpec` field, though the two binaries do express it once: a field
    // there would imply the other two supervisors read it, and they cannot -- theirs
    // live in a checked-in unit file and in the scope table above. One place per
    // supervisor, with each naming the others, is the honest arrangement.
    constexpr DWORD RestartDelayMs = 1'000;
    constexpr DWORD BackoffDelayMs = 30'000;
    constexpr DWORD FailureCountResetSeconds = 3'600;

    std::array<SC_ACTION, 3> actions { SC_ACTION { .Type = SC_ACTION_RESTART, .Delay = RestartDelayMs },
                                       SC_ACTION { .Type = SC_ACTION_RESTART, .Delay = RestartDelayMs },
                                       SC_ACTION { .Type = SC_ACTION_RESTART, .Delay = BackoffDelayMs } };
    SERVICE_FAILURE_ACTIONSA failureActions { .dwResetPeriod = FailureCountResetSeconds,
                                              .lpRebootMsg = nullptr,
                                              .lpCommand = nullptr,
                                              .cActions = static_cast<DWORD>(actions.size()),
                                              .lpsaActions = actions.data() };
    // Checked, not discarded. A policy that was refused and is inert, under an
    // `--install-service` that answered "installed", is the same shape of lie as a
    // service logging to a handle nobody holds -- and `sc qfailure` would show the
    // empty policy while the installer had already said otherwise.
    std::string policyWarnings;
    if (ChangeServiceConfig2A(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions) == 0)
        policyWarnings += std::format("\nwarning: the restart policy could not be set (error {}); this service will "
                                      "stay stopped after it fails",
                                      GetLastError());

    // And they must fire on an ORDINARY non-zero exit, not only on a crash. That is
    // the default this API ships with, and it is the wrong one here: every way this
    // daemon gives up -- a port it cannot bind, a store of the wrong vintage -- is a
    // clean return with a non-zero code, which is precisely the failure worth
    // restarting from. Without this the policy above is configured and inert.
    // Not const: `ChangeServiceConfig2A` takes an untyped mutable pointer.
    SERVICE_FAILURE_ACTIONS_FLAG onNonCrash { .fFailureActionsOnNonCrashFailures = TRUE };
    if (ChangeServiceConfig2A(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &onNonCrash) == 0)
        policyWarnings += std::format("\nwarning: the restart policy could not be applied to ordinary failures (error "
                                      "{}); it will act on a crash only",
                                      GetLastError());

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    // Only now: `NT SERVICE\<name>` does not resolve until the service exists, so
    // a grant attempted before CreateService fails to translate the trustee.
    //
    // Reported rather than fatal, and the registration is left in place. A service
    // that is registered and cannot write one directory is recoverable by an
    // operator with `icacls`; one that was rolled back because of it leaves them
    // nothing to repair. This mirrors the launchd path, where a chown that fails
    // is `(void)`-discarded -- except that this says so.
    std::string warnings { std::move(policyWarnings) };

    // A service with no console reports through the event log (#179), and this is
    // what makes those records legible. Reported rather than fatal for the reason the
    // grant below is: the events are written either way, and a registration an
    // operator can repair beats an install rolled back over presentation.
    if (auto const denial = RegisterEventSourceForService(spec.serviceName))
        warnings += std::format("\nwarning: {}", *denial);

    if (logonName)
        for (auto const& owned: spec.ownedPaths)
            if (auto const denial = GrantPathAccess(owned, *logonName))
                warnings += std::format("\nwarning: {}", *denial);

    if (!warnings.empty())
        return { .exitCode = 0,
                 .message = std::format("installed service '{}' (auto-start); start it now with: sc start {}{}",
                                        spec.serviceName,
                                        spec.serviceName,
                                        warnings) };

    return { .exitCode = 0,
             .message = std::format("installed service '{}' (auto-start); start it now with: sc start {}",
                                    spec.serviceName,
                                    spec.serviceName) };
}

ServiceControlResult UninstallService(ServiceSpec const& spec, ServiceScope /*scope*/)
{
    // The name gates removal too: it selects which registration is addressed,
    // and one that could never have been installed cannot be removed either.
    if (auto const rejection = ServiceNameRejection(spec))
        return { .exitCode = 1, .message = *rejection };

    SC_HANDLE const manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        auto const err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("opening the service manager") };
        return { .exitCode = 1, .message = std::format("OpenSCManager failed (error {})", err) };
    }

    SC_HANDLE const service = OpenServiceA(manager, spec.serviceName.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        auto const err = GetLastError();
        CloseServiceHandle(manager);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST)
            return { .exitCode = 1, .message = std::format("no service named '{}' is installed", spec.serviceName) };
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("opening the service") };
        return { .exitCode = 1, .message = std::format("OpenService failed (error {})", err) };
    }

    // Best-effort stop before deletion; ignore failure (e.g. already stopped).
    SERVICE_STATUS status {};
    ControlService(service, SERVICE_CONTROL_STOP, &status);

    auto const deleted = DeleteService(service) != 0;
    auto const deleteErr = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    if (!deleted)
        return { .exitCode = 1, .message = std::format("DeleteService failed (error {})", deleteErr) };

    // AFTER the deletion succeeded, never before it. A service that is still
    // installed and has lost its provider registration writes records that render as
    // "the description ... cannot be found" -- and nothing short of a reinstall puts
    // it back, so a failed uninstall would leave the service worse than it found it.
    //
    // Best-effort from here, like the stop above: an orphaned registry key is not
    // something an operator can act on, and reporting the uninstall as failed over
    // one would describe a service that is genuinely gone as still present.
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, EventSourceKeyPath(spec.serviceName).c_str());

    return { .exitCode = 0, .message = std::format("uninstalled service '{}'", spec.serviceName) };
}

#elif defined(__APPLE__)

namespace
{
    /// The invoking user's home directory, from the password database rather
    /// than $HOME: this runs under `sudo` and from pkg postinstall scripts,
    /// where $HOME belongs to root or is unset entirely.
    [[nodiscard]] std::filesystem::path CurrentHomeDirectory()
    {
        if (auto const* const pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr)
            return std::filesystem::path { pw->pw_dir };
        return {};
    }

    /// Whether a launchctl invocation's own diagnostics reach the terminal.
    enum class LaunchctlOutput : std::uint8_t
    {
        Show,    ///< Let launchctl print; its message is the useful diagnostic.
        Silence, ///< Discard: this call is expected to fail in normal operation.
    };

    /// Returned by RunLaunchctl when the call had to be killed at its deadline.
    constexpr int LaunchctlTimedOut = -2;

    /// Returned by RunLaunchctl when launchctl could not be started at all.
    constexpr int LaunchctlNotStarted = -1;

    /// How long any single launchctl call may take before it is killed.
    ///
    /// An installer must never be able to hang. A launchctl subcommand that
    /// blocks turns a postinstall into a script the macOS Installer kills
    /// minutes later with "An error occurred while running scripts" — which
    /// names no command, no argument and no reason. Bounding each call trades
    /// an unfalsifiable stall for a message that says which one stopped.
    constexpr int LaunchctlTimeoutSeconds = 60;

    /// Run `launchctl` with @p args, waiting up to LaunchctlTimeoutSeconds.
    ///
    /// posix_spawn with an explicit argv rather than system(): every argument
    /// here is a path or a label that can contain shell metacharacters, and a
    /// shell would interpret them.
    ///
    /// @param args Arguments after argv[0].
    /// @param output Whether to let launchctl write to stdout/stderr.
    /// @return The exit status, or -1 if launchctl could not be started.
    [[nodiscard]] int RunLaunchctl(std::vector<std::string> const& args, LaunchctlOutput output = LaunchctlOutput::Show)
    {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        std::string program = "launchctl";
        argv.push_back(program.data());
        // Safe: posix_spawn does not modify argv, and the strings outlive the call.
        for (auto const& arg: args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        ::posix_spawn_file_actions_t actions {};
        ::posix_spawn_file_actions_t* actionsPtr = nullptr;
        if (output == LaunchctlOutput::Silence && ::posix_spawn_file_actions_init(&actions) == 0)
        {
            ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
            actionsPtr = &actions;
        }

        ::pid_t pid = 0;
        auto const spawned = ::posix_spawn(&pid, "/bin/launchctl", actionsPtr, nullptr, argv.data(), environ);

        if (actionsPtr != nullptr)
            ::posix_spawn_file_actions_destroy(actionsPtr);

        if (spawned != 0)
            return LaunchctlNotStarted;

        // Polled rather than a blocking waitpid, so the deadline is enforceable.
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { LaunchctlTimeoutSeconds };
        while (true)
        {
            int status = 0;
            auto const reaped = ::waitpid(pid, &status, WNOHANG);
            if (reaped == pid)
                return WIFEXITED(status) ? WEXITSTATUS(status) : LaunchctlNotStarted;
            if (reaped < 0)
                return LaunchctlNotStarted;

            if (std::chrono::steady_clock::now() >= deadline)
            {
                (void) ::kill(pid, SIGKILL);
                int discarded = 0;
                (void) ::waitpid(pid, &discarded, 0);
                return LaunchctlTimedOut;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
        }
    }

    /// Human-readable form of a RunLaunchctl status.
    ///
    /// A killed or unstartable call must not surface as a bare negative number
    /// in an operator-facing message; the whole point of bounding the call is
    /// that the failure says what happened.
    /// @param status Value returned by RunLaunchctl.
    /// @return A phrase that reads correctly after "failed (".
    [[nodiscard]] std::string LaunchctlStatusText(int status)
    {
        switch (status)
        {
            case LaunchctlTimedOut:
                return std::format("killed after {}s with no result", LaunchctlTimeoutSeconds);
            case LaunchctlNotStarted:
                return "could not be started";
            default:
                return std::format("status {}", status);
        }
    }

    /// Fill the `{}` placeholder of a domain pattern with the invoking uid.
    /// @param pattern Row value from ScopeTraits::domains.
    /// @return The concrete domain target, e.g. `gui/501`.
    [[nodiscard]] std::string ExpandDomain(std::string_view pattern)
    {
        auto const placeholder = pattern.find("{}");
        if (placeholder == std::string_view::npos)
            return std::string { pattern };
        return std::format("{}{}{}", pattern.substr(0, placeholder), ::getuid(), pattern.substr(placeholder + 2));
    }

    /// Every launchctl domain target @p scope could hold a job in, most
    /// preferred first.
    /// @param scope Domain family to enumerate.
    /// @return Concrete targets, e.g. `{"gui/501", "user/501"}`.
    [[nodiscard]] std::vector<std::string> DomainTargets(ServiceScope scope)
    {
        std::vector<std::string> targets;
        for (auto const& pattern: TraitsOf(scope).domains)
            if (!pattern.empty())
                targets.push_back(ExpandDomain(pattern));
        return targets;
    }

    /// The launchctl domain target to bootstrap a new job into.
    ///
    /// Walks the scope's candidates in preference order and returns the first
    /// launchd actually knows about, so a user-scope install works both on a
    /// desktop and over SSH. Falling back to the most-preferred candidate when
    /// none probes clean keeps the failure message pointing at the domain the
    /// caller most likely meant.
    ///
    /// Only ever used for *bootstrapping*. Teardown must not re-probe — see
    /// BootOutEverywhere.
    ///
    /// @param scope Domain being installed into.
    /// @return A concrete launchctl domain target.
    [[nodiscard]] std::string DomainTarget(ServiceScope scope)
    {
        auto const candidates = DomainTargets(scope);
        for (auto const& candidate: candidates)
            if (RunLaunchctl({ "print", candidate }, LaunchctlOutput::Silence) == 0)
                return candidate;
        return candidates.empty() ? std::string {} : candidates.front();
    }

    /// Block until launchd no longer knows @p serviceTarget.
    ///
    /// `launchctl bootout` returns before the teardown is complete: it signals
    /// the job and leaves the label registered while the process winds down. A
    /// `bootstrap` issued in that window fails with "Bootstrap failed: 5: Input/
    /// output error", which is exactly what a reinstall over a running service
    /// used to hit — the plist was rewritten and then nothing was listening.
    ///
    /// @param serviceTarget Domain-qualified label, e.g. `gui/501/com.example.x`.
    /// @return True if the job is gone, false if it was still present at timeout.
    [[nodiscard]] bool WaitForJobGone(std::string const& serviceTarget)
    {
        // Generous enough to cover a graceful shutdown, short enough that a
        // genuinely stuck job still reports rather than hanging the installer.
        //
        // Considered for `Core/BoundedDrain.hpp`'s `DrainWithin` and deliberately
        // left alone (#452). That defect was a loop STATING a wall-clock ceiling
        // and counting requested polls towards it; this budgets ATTEMPTS and says
        // so, which is an honest budget rather than a false claim. Converting it
        // would trade probes for seconds on a macOS installer path with no
        // coverage, which is a different decision from fixing a lie.
        constexpr auto Attempts = 100;
        constexpr auto Interval = std::chrono::milliseconds { 50 };

        return std::ranges::any_of(std::views::iota(0, Attempts), [&](int) {
            if (RunLaunchctl({ "print", serviceTarget }, LaunchctlOutput::Silence) != 0)
                return true;
            std::this_thread::sleep_for(Interval);
            return false;
        });
    }

    /// Boot @p label out of every domain @p scope could hold it in.
    ///
    /// Every candidate, never the one DomainTarget probes for now: which domain
    /// a job landed in was decided when it was *installed*. An agent registered
    /// over SSH lives in `user/<uid>` because `gui/<uid>` did not exist then, so
    /// a later teardown from a desktop session would probe `gui/<uid>`, boot out
    /// a job that was never there, see `launchctl print` fail for the same
    /// reason, and report removal — while the real agent kept running and kept
    /// holding the port, its plist now deleted so nothing could reach it again.
    ///
    /// @param scope Domain family the job belongs to.
    /// @param label launchd job label.
    /// @return A diagnostic if a job is still registered somewhere, else nullopt.
    [[nodiscard]] std::optional<std::string> BootOutEverywhere(ServiceScope scope, std::string const& label)
    {
        for (auto const& domain: DomainTargets(scope))
        {
            auto const serviceTarget = std::format("{}/{}", domain, label);
            // Silenced: "No such process" is the normal answer for every domain
            // the job is not in, which is all but one of them.
            (void) RunLaunchctl({ "bootout", serviceTarget }, LaunchctlOutput::Silence);
            if (!WaitForJobGone(serviceTarget))
                return std::format("'{}' is still registered after bootout; "
                                   "run `launchctl bootout {}` manually and retry",
                                   label,
                                   serviceTarget);
        }
        return std::nullopt;
    }

    /// Why @p account could not read @p path, if it could not.
    ///
    /// A system daemon drops to @p account before it opens its config, so a
    /// root-owned mode-0600 file — exactly what InlineCredentialRejection sends
    /// the operator off to create — leaves launchd restarting a job that exits
    /// at every start, with the EACCES visible nowhere. Checked here, where the
    /// remedy can be named, rather than discovered from a restart loop.
    ///
    /// @param account The unprivileged account the job runs as.
    /// @param path Config file the job will be pointed at.
    /// @return A diagnostic naming the fix, or nullopt when the file is readable.
    [[nodiscard]] std::optional<std::string> ServiceAccountReadDenial(std::string const& account,
                                                                      std::filesystem::path const& path)
    {
        auto const* const pw = ::getpwnam(account.c_str());
        if (pw == nullptr)
            return std::nullopt; // Reported by its own guard; nothing to add.

        struct stat info = {};
        if (::stat(path.c_str(), &info) != 0)
            return std::nullopt; // Absent or unreadable by root: not this check's business.

        auto const readable = (info.st_mode & S_IROTH) != 0 || (info.st_uid == pw->pw_uid && (info.st_mode & S_IRUSR) != 0)
                              || (info.st_gid == pw->pw_gid && (info.st_mode & S_IRGRP) != 0);
        if (readable)
            return std::nullopt;

        return std::format("{} is not readable by '{}', the account the daemon runs as, so the job would exit at "
                           "every start. Grant it access first: chown root:{} {} && chmod 0640 {}",
                           path.string(),
                           account,
                           account,
                           path.string(),
                           path.string());
    }
} // namespace

ServiceControlResult InstallService(ServiceSpec const& spec, ServiceScope scope)
{
    if (auto const rejection = ServiceRegistrationRejection(spec))
        return { .exitCode = 1, .message = *rejection };

    if (spec.exePath.empty())
        return { .exitCode = 1, .message = "could not determine the executable path to register" };

    if (scope == ServiceScope::System && ::geteuid() != 0)
        return { .exitCode = 1, .message = "installing a system LaunchDaemon requires root; re-run with sudo" };

    // The mirror guard, and not a nicety: a user agent is installed *for the
    // invoking account*, and CurrentHomeDirectory reads the real uid, which
    // sudo sets to 0. Without this, `sudo fastcached --install-service` — the
    // natural reading of a command whose help mentions sudo — writes the plist
    // to /var/root/Library/LaunchAgents and bootstraps it into root's domain.
    // Nothing starts at the operator's own login, and an unprivileged
    // --uninstall-service reports nothing installed, leaving a root-owned agent
    // they can neither see nor remove. Which account they meant is genuinely
    // ambiguous, so ask rather than guess.
    if (scope == ServiceScope::User && ::geteuid() == 0)
        return { .exitCode = 1,
                 .message = "--service-scope=user installs an agent for the invoking account, so it must not run as "
                            "root. Re-run without sudo, or pass --service-scope=system for a machine-wide daemon." };

    auto const home = CurrentHomeDirectory();
    if (scope == ServiceScope::User && home.empty())
        return { .exitCode = 1, .message = "could not determine the invoking user's home directory" };

    // A LaunchDaemon plist naming a UserName launchd cannot resolve is not
    // rejected at bootstrap: the job registers, `launchctl bootstrap` and
    // `kickstart` both return 0, and only the spawn fails — so without this
    // check the tool prints "installed and started" while nothing ever listens.
    // The account is created by the .pkg postinstall, which is the only thing
    // that creates it, so a tarball or from-source install lands here.
    if (scope == ServiceScope::System && !spec.serviceAccount.empty() && ::getpwnam(spec.serviceAccount.c_str()) == nullptr)
        return { .exitCode = 1,
                 .message = std::format("the '{}' service account does not exist. It is created by the macOS "
                                        "installer package; for a manual install, create it first (see "
                                        "docs/operations/deployment.md) or use --service-scope=user.",
                                        spec.serviceAccount) };

    // The two ambient inputs WithScopeDefaults decides from, both resolved here
    // rather than inside it: readability, not mere existence, is the test, for
    // the reason DefaultConfigPath.hpp gives — and trust on top of it, so a
    // registration cannot hand launchd a file the daemon would refuse to obey
    // at every start. Failing the same way in both places keeps the two from
    // disagreeing about which configs count.
    SystemConfigPathProbe const probe;
    //
    // Looked up under the SPEC's application name, not the daemon's. Hardcoding
    // the daemon's meant a worker registration was handed the daemon's config
    // file -- a file it cannot parse, and, once the package tightens that file to
    // 0640 root:_fastcached, one the worker's own account cannot even read, so the
    // install was refused for a reason that had nothing to do with the worker.
    // A service that names no application looks nothing up.
    auto const packagedConfig = [&] {
        if (spec.applicationName.empty())
            return std::filesystem::path {};
        auto const path = SystemConfigPath(probe, spec.applicationName);
        return path.has_value() && probe.IsReadableFile(*path) && probe.IsTrustedSystemLocation(*path)
                   ? *path
                   : std::filesystem::path {};
    }();

    auto const effective = WithScopeDefaults(spec, scope, home, packagedConfig);
    auto const label = LaunchdLabel(effective);
    auto const plistPath = LaunchdPlistPath(effective, scope, home);
    auto const logDirectory = DefaultLogDirectory(label, scope, home);

    if (scope == ServiceScope::System && !effective.configPath.empty())
        if (auto const denial = ServiceAccountReadDenial(effective.serviceAccount, effective.configPath))
            return { .exitCode = 1, .message = *denial };

    std::error_code ec;
    std::filesystem::create_directories(plistPath.parent_path(), ec);
    if (ec)
        return { .exitCode = 1,
                 .message = std::format("could not create {}: {}", plistPath.parent_path().string(), ec.message()) };

    std::filesystem::create_directories(logDirectory, ec);
    if (ec)
        return { .exitCode = 1, .message = std::format("could not create {}: {}", logDirectory.string(), ec.message()) };

    // The daemon drops to its service account, so directories root created for it
    // have to change hands or its first write fails with EACCES — which launchd
    // surfaces only as a job that exits immediately, over and over.
    //
    // The storage directory itself, never its parent. Handing over the parent
    // gave away a directory the operator never named: `--storage=/var/db/fc`
    // reassigned /var/db, shared with other system services, to an unprivileged
    // cache account, and `--storage=/tmp/cache` reassigned /private/tmp — both
    // silently, under a message that said the service had been installed.
    //
    // Created only when it is absent AND meant to be a directory; whatever is
    // already there is chowned as-is. An `ownedPaths` entry may be one CoW file
    // (`storage_path` is allowed to name `cache.cow`), and an unconditional
    // `create_directories` failed in both directions on it: on a fresh install it
    // made a DIRECTORY the daemon then read as a directory of shards, and once the
    // file existed it failed and skipped the chown — on exactly the upgrade this
    // handover is here for.
    std::string warnings;
    if (scope == ServiceScope::System && !effective.serviceAccount.empty())
        if (auto const* const pw = ::getpwnam(effective.serviceAccount.c_str()); pw != nullptr)
        {
            std::vector<std::filesystem::path> owned { logDirectory };
            for (auto const& target: effective.ownedPaths)
            {
                std::error_code probeEc;
                auto const namesAFile = PathNamesAFile(target);
                std::error_code createEc;
                if (!std::filesystem::exists(target, probeEc) && !namesAFile)
                    std::filesystem::create_directories(target, createEc);
                if (std::filesystem::exists(target, probeEc))
                {
                    owned.emplace_back(target);
                    continue;
                }

                // SAID, not skipped -- and the two ways of getting here are
                // different facts, so they get different sentences. A path that
                // NAMES A FILE is correctly not created (making a directory of it
                // is the failure the comment above records) but is then never
                // chowned either, and the service account cannot create it inside a
                // root-owned parent. A path that was meant to be a directory and is
                // still absent failed to be created, and `createEc` is the reason.
                //
                // Left silent, either one let `--install-service` report success
                // while launchd respawned the job forever. The Windows sibling has
                // warned since it was written; this path told an operator nothing.
                warnings += namesAFile
                                ? std::format("\nwarning: {} does not exist yet and names a file, so it was not "
                                              "created; grant '{}' access to the directory that will hold it",
                                              target.string(),
                                              effective.serviceAccount)
                                : std::format("\nwarning: could not create {} for '{}': {}",
                                              target.string(),
                                              effective.serviceAccount,
                                              createEc ? createEc.message() : std::string { "it is still not there" });
            }
            for (auto const& path: owned)
                (void) ::chown(path.c_str(), pw->pw_uid, pw->pw_gid);
        }

    {
        std::ofstream out { plistPath, std::ios::binary | std::ios::trunc };
        if (!out)
            return { .exitCode = 1, .message = std::format("could not write {}", plistPath.string()) };
        out << BuildLaunchdPlist(effective, scope, logDirectory);
        if (!out)
            return { .exitCode = 1, .message = std::format("could not write {}", plistPath.string()) };
    }

    auto const domain = DomainTarget(scope);
    auto const serviceTarget = std::format("{}/{}", domain, label);

    // Boot out any previous incarnation first: launchd caches the job
    // description at bootstrap time, so re-registering without this leaves the
    // old ProgramArguments running and the freshly written plist ignored. Every
    // domain, not just the one being bootstrapped into now — an earlier install
    // may have landed in the other one, and that job still holds the port.
    if (auto const stillRegistered = BootOutEverywhere(scope, label))
        return { .exitCode = 1, .message = *stillRegistered };

    if (auto const rc = RunLaunchctl({ "bootstrap", domain, plistPath.string() }); rc != 0)
        return { .exitCode = 1,
                 .message = std::format("wrote {} but `launchctl bootstrap {}` failed ({})",
                                        plistPath.string(),
                                        domain,
                                        LaunchctlStatusText(rc)) };

    // bootstrap only *loads* the job. Despite RunAtLoad, launchd records the
    // spawn as "pended nondemand spawn = speculative" and can leave it unstarted
    // indefinitely — this command reported success while `launchctl print` said
    // "state = not running" and nothing was listening on the port. kickstart
    // forces the spawn, so the service really is up when this returns.
    if (auto const rc = RunLaunchctl({ "kickstart", "-k", serviceTarget }); rc != 0)
        return { .exitCode = 1,
                 .message = std::format("registered '{}' but `launchctl kickstart` failed ({}); "
                                        "it will start at the next login or boot",
                                        label,
                                        LaunchctlStatusText(rc)) };

    return { .exitCode = 0,
             .message = std::format("installed and started launchd job '{}' ({} scope, {}){}",
                                    label,
                                    ServiceScopeName(scope),
                                    plistPath.string(),
                                    warnings) };
}

ServiceControlResult UninstallService(ServiceSpec const& spec, ServiceScope scope)
{
    if (auto const rejection = ServiceNameRejection(spec))
        return { .exitCode = 1, .message = *rejection };

    if (scope == ServiceScope::System && ::geteuid() != 0)
        return { .exitCode = 1, .message = "removing a system LaunchDaemon requires root; re-run with sudo" };

    // Symmetric with the install guard: under sudo the home directory resolves
    // to root's, so this would look for an agent in /var/root and report the
    // operator's own as absent while leaving it running.
    if (scope == ServiceScope::User && ::geteuid() == 0)
        return { .exitCode = 1,
                 .message = "--service-scope=user removes the invoking account's agent, so it must not run as root. "
                            "Re-run without sudo, or pass --service-scope=system." };

    auto const home = CurrentHomeDirectory();
    if (scope == ServiceScope::User && home.empty())
        return { .exitCode = 1, .message = "could not determine the invoking user's home directory" };

    auto const label = LaunchdLabel(spec);
    auto const plistPath = LaunchdPlistPath(spec, scope, home);

    // Best-effort stop; a job that was never loaded is not an error here, so
    // launchctl's "No such process" complaint is suppressed. Waited on for the
    // same reason as the install path: bootout returns while the job is still
    // winding down, and reporting removal before launchd has let go would make
    // "uninstalled" a claim the very next command could contradict.
    if (auto const stillRegistered = BootOutEverywhere(scope, label))
        return { .exitCode = 1, .message = *stillRegistered };

    std::error_code ec;
    auto const removed = std::filesystem::remove(plistPath, ec);
    if (ec)
        return { .exitCode = 1, .message = std::format("could not remove {}: {}", plistPath.string(), ec.message()) };
    if (!removed)
        return { .exitCode = 1, .message = std::format("no launchd job installed at {}", plistPath.string()) };

    return { .exitCode = 0, .message = std::format("removed launchd job '{}' ({})", label, plistPath.string()) };
}

#else

ServiceControlResult InstallService(ServiceSpec const& /*spec*/, ServiceScope /*scope*/)
{
    return { .exitCode = 1, .message = "service control is only available on Windows (SCM) and macOS (launchd)" };
}

ServiceControlResult UninstallService(ServiceSpec const& /*spec*/, ServiceScope /*scope*/)
{
    return { .exitCode = 1, .message = "service control is only available on Windows (SCM) and macOS (launchd)" };
}

#endif // _WIN32 / __APPLE__

} // namespace FastCache
