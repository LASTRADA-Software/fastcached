// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
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

std::vector<std::string> BuildServiceArgv(std::filesystem::path const& exePath, Config const& cfg, EmitDaemonFlag daemonFlag)
{
    Config const defaults {};
    std::vector<std::string> argv { exePath.string() };

    // Emit `--flag=value` only when the field differs from its default, so the
    // recorded launch arguments stay minimal and self-documenting. The rule is
    // written once here; the table below is pure data.
    auto const emitIfSet = [&argv](std::string_view flag, auto const& value, auto const& fallback) {
        if (value != fallback)
            argv.push_back(std::format("--{}={}", flag, FlagValue(value)));
    };

    // Path flags test the *captured* value for emptiness and absolutize only
    // what survives. Absolutizing first would turn an unset path into the
    // installing process's working directory, silently pinning the service to
    // whatever shell happened to register it.
    auto const emitPathIfSet = [&argv](std::string_view flag, std::string const& value) {
        if (!value.empty())
            argv.push_back(std::format("--{}={}", flag, AbsolutePathArg(value)));
    };

    // Valueless switches: present or absent, never `--flag=true`, because that
    // is not a spelling CliParser accepts.
    auto const emitSwitchIfSet = [&argv](std::string_view flag, bool value, bool fallback) {
        if (value != fallback)
            argv.emplace_back(std::format("--{}", flag));
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
    //        flag                    current value              emitted unless equal to
    emitIfSet("bind", cfg.bindAddress, defaults.bindAddress);
    emitIfSet("port", cfg.port, defaults.port);
    emitIfSet("max-memory", cfg.maxMemoryBytes, defaults.maxMemoryBytes);
    emitIfSet("log-level", cfg.logLevel, defaults.logLevel);
    emitSwitchIfSet("log-timestamps", cfg.logTimestamps, defaults.logTimestamps);
    emitSwitchIfSet("log-source", cfg.logSource, defaults.logSource);
    emitSwitchIfSet("log-everything", cfg.logEverything, defaults.logEverything);
    emitPathIfSet("storage", cfg.storagePath);
    emitIfSet("storage-durability", cfg.storageDurability, defaults.storageDurability);
    emitIfSet("storage-max-value", cfg.storageMaxValueBytes, defaults.storageMaxValueBytes);
    emitIfSet("storage-max-disk", cfg.storageMaxDiskBytes, defaults.storageMaxDiskBytes);
    emitIfSet("storage-shards", cfg.storageShards, defaults.storageShards);
    emitIfSet("threads", cfg.workerThreads, defaults.workerThreads);
    emitIfSet("cpu-affinity", cfg.cpuAffinity, defaults.cpuAffinity);
    emitIfSet("lru-mode", cfg.lruRecency, defaults.lruRecency);
    emitIfSet("listen-backlog", cfg.listenBacklog, defaults.listenBacklog);
    emitIfSet("compression", cfg.compression, defaults.compression);
    emitIfSet("compression-level", cfg.compressionLevel, defaults.compressionLevel);
    emitIfSet("compression-min-bytes", cfg.compressionMinBytes, defaults.compressionMinBytes);
    emitIfSet("auth-username", cfg.authUsername, defaults.authUsername);
    emitSwitchIfSet("tls", cfg.tlsEnabled, defaults.tlsEnabled);
    emitPathIfSet("tls-cert", cfg.tlsCertPath);
    emitPathIfSet("tls-key", cfg.tlsKeyPath);
    emitSwitchIfSet("metrics", cfg.metricsEnabled, defaults.metricsEnabled);
    emitIfSet("metrics-bind", cfg.metricsBindAddress, defaults.metricsBindAddress);
    emitIfSet("metrics-port", cfg.metricsPort, defaults.metricsPort);
    emitIfSet("notify-keyspace-events", cfg.notifyKeyspaceEvents, defaults.notifyKeyspaceEvents);
    emitPathIfSet("pidfile", cfg.pidfile);
    emitPathIfSet("config", cfg.configPath);

    // Repeated flags, one token per listener, so a multi-endpoint daemon comes
    // back with the same listener set it was installed with. The address goes
    // through FormatListenHost so what is emitted is what ParseListenSpec
    // accepts — these tokens are re-parsed by the daemon at every start.
    for (auto const& bind: cfg.binds)
        argv.push_back(
            std::format("--{}={}:{}", bind.tls ? "listen-tls" : "listen", FormatListenHost(bind.address), bind.port));

    // requirePass is deliberately NOT here, and its omission is reported rather
    // than silent: see InlineCredentialRejection.
    return argv;
}

std::optional<std::string> InlineCredentialRejection(Config const& cfg)
{
    if (cfg.requirePass.empty())
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

    if (cfg.configPath.empty())
        return std::string { Preamble };

    // --config does not make the combination safe. Nothing here can tell
    // whether the named file carries `requirepass:` — the installer-seeded YAML
    // does not — so accepting it was the silent drop wearing a different hat:
    // the daemon came up unauthenticated while the operator was told the
    // password had been registered.
    return std::format("{} You passed --config={}, so put it there and drop --requirepass from this command line.",
                       Preamble,
                       cfg.configPath);
}

std::optional<std::string> ServiceNameRejection(Config const& cfg)
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
    if (cfg.serviceName.empty())
        return "--service-name must not be empty";

    if (cfg.serviceName.find_first_of("/\\") != std::string::npos)
        return std::format("--service-name must not contain a path separator: '{}'", cfg.serviceName);

    // A leading dot covers `..` — the traversal that needs no separator — and a
    // dotfile, which launchd's directory scan skips, so the job would silently
    // never come back after a reboot.
    if (cfg.serviceName.starts_with('.'))
        return std::format("--service-name must not start with '.': '{}'", cfg.serviceName);

    if (std::ranges::any_of(cfg.serviceName, [](unsigned char ch) { return std::iscntrl(ch) != 0; }))
        return std::format("--service-name must not contain control characters: '{}'", cfg.serviceName);

    return std::nullopt;
}

std::optional<std::string> ServiceRegistrationRejection(Config const& cfg)
{
    // A table, so a new rule is a new row rather than another `if` threaded
    // through both platforms' InstallService.
    using Validator = std::optional<std::string> (*)(Config const&);
    constexpr auto Validators = std::to_array<Validator>({ &ServiceNameRejection, &InlineCredentialRejection });

    for (auto const validator: Validators)
        if (auto rejection = validator(cfg))
            return rejection;

    return std::nullopt;
}

std::string BuildServiceCommandLine(std::filesystem::path const& exePath, Config const& cfg)
{
    auto const argv = BuildServiceArgv(exePath, cfg, EmitDaemonFlag::Yes);

    // The executable path is always quoted so an install directory containing
    // spaces (e.g. "C:\Program Files\fastcached") tokenizes correctly.
    std::string out = std::format("\"{}\"", argv.front());

    for (auto const& arg: argv | std::views::drop(1))
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

    /// The dedicated account a system-wide job runs as, created by the package's
    /// postinstall. Mirrors the `fastcached` user in packaging/linux/fastcached.sysusers.
    constexpr std::string_view ServiceAccount = "_fastcached";

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
        bool runsAsServiceAccount; ///< Emit UserName/GroupName.
    };

    constexpr auto ScopeTable = std::to_array<ScopeTraits>({
        // A user agent uses KeepAlive={Crashed:true} rather than <true/>: a second
        // logged-in user's agent loses the race for 127.0.0.1:11211 and exits
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

std::string LaunchdLabel(Config const& cfg)
{
    auto lowered = cfg.serviceName;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return std::format("{}{}", LaunchdLabelPrefix, lowered);
}

std::filesystem::path LaunchdPlistPath(Config const& cfg, ServiceScope scope, std::filesystem::path const& homeDirectory)
{
    auto const& traits = TraitsOf(scope);
    auto const fileName = std::format("{}.plist", LaunchdLabel(cfg));

    if (traits.homeRelative)
        return homeDirectory / traits.plistDirectory / fileName;
    return std::filesystem::path { traits.plistDirectory } / fileName;
}

std::string BuildLaunchdPlist(std::filesystem::path const& exePath,
                              Config const& cfg,
                              ServiceScope scope,
                              std::filesystem::path const& logDirectory)
{
    auto const& traits = TraitsOf(scope);
    auto const label = LaunchdLabel(cfg);

    // EmitDaemonFlag::No is the whole point: launchd supervises the process it
    // started, and a job that double-forks is reaped instantly as "exited".
    auto const argv = BuildServiceArgv(exePath, cfg, EmitDaemonFlag::No);

    std::string arguments;
    for (auto const& arg: argv)
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

    if (traits.runsAsServiceAccount)
    {
        out += std::format("    <key>UserName</key>\n    <string>{}</string>\n", ServiceAccount);
        out += std::format("    <key>GroupName</key>\n    <string>{}</string>\n", ServiceAccount);
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

#if defined(_WIN32)

namespace
{
    /// One-line description registered with the SCM (shown in services.msc).
    constexpr std::string_view ServiceDescription = "fastcached — fast cache daemon";

    /// Resolve the absolute path of the running executable.
    /// @return Path on success; empty path on failure.
    [[nodiscard]] std::filesystem::path CurrentExecutablePath()
    {
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
    }

    /// Standard "needs elevation" guidance reused across SCM error paths.
    [[nodiscard]] std::string ElevationHint(std::string_view action)
    {
        return std::format("access denied {}; run from an elevated (Administrator) prompt", action);
    }
} // namespace

ServiceControlResult InstallService(Config const& cfg, ServiceScope /*scope*/)
{
    if (auto const rejection = ServiceRegistrationRejection(cfg))
        return { .exitCode = 1, .message = *rejection };

    auto const exe = CurrentExecutablePath();
    if (exe.empty())
        return { .exitCode = 1, .message = "could not determine the fastcached executable path" };

    auto const commandLine = BuildServiceCommandLine(exe, cfg);

    SC_HANDLE const manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr)
    {
        auto const err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("opening the service manager") };
        return { .exitCode = 1, .message = std::format("OpenSCManager failed (error {})", err) };
    }

    SC_HANDLE const service = CreateServiceA(manager,
                                             cfg.serviceName.c_str(),
                                             cfg.serviceName.c_str(),
                                             SERVICE_ALL_ACCESS,
                                             SERVICE_WIN32_OWN_PROCESS,
                                             SERVICE_AUTO_START,
                                             SERVICE_ERROR_NORMAL,
                                             commandLine.c_str(),
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr);
    if (service == nullptr)
    {
        auto const err = GetLastError();
        CloseServiceHandle(manager);
        if (err == ERROR_SERVICE_EXISTS)
            return { .exitCode = 1,
                     .message = std::format("service '{}' already exists; remove it first with --uninstall-service",
                                            cfg.serviceName) };
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("creating the service") };
        return { .exitCode = 1, .message = std::format("CreateService failed (error {})", err) };
    }

    // Best-effort friendly description; failure here does not fail the install.
    std::string description { ServiceDescription };
    SERVICE_DESCRIPTIONA descriptor { .lpDescription = description.data() };
    ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &descriptor);

    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    return { .exitCode = 0,
             .message = std::format(
                 "installed service '{}' (auto-start); start it now with: sc start {}", cfg.serviceName, cfg.serviceName) };
}

ServiceControlResult UninstallService(Config const& cfg, ServiceScope /*scope*/)
{
    // The name gates removal too: it selects which registration is addressed,
    // and one that could never have been installed cannot be removed either.
    if (auto const rejection = ServiceNameRejection(cfg))
        return { .exitCode = 1, .message = *rejection };

    SC_HANDLE const manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        auto const err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return { .exitCode = 1, .message = ElevationHint("opening the service manager") };
        return { .exitCode = 1, .message = std::format("OpenSCManager failed (error {})", err) };
    }

    SC_HANDLE const service = OpenServiceA(manager, cfg.serviceName.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        auto const err = GetLastError();
        CloseServiceHandle(manager);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST)
            return { .exitCode = 1, .message = std::format("no service named '{}' is installed", cfg.serviceName) };
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
    return { .exitCode = 0, .message = std::format("uninstalled service '{}'", cfg.serviceName) };
}

#elif defined(__APPLE__)

namespace
{
    /// Resolve the absolute path of the running executable.
    /// @return Path on success; empty path on failure.
    [[nodiscard]] std::filesystem::path CurrentExecutablePath()
    {
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
    }

    /// The invoking user's home directory, from the password database rather
    /// than $HOME: this runs under `sudo` and from pkg postinstall scripts,
    /// where $HOME belongs to root or is unset entirely.
    [[nodiscard]] std::filesystem::path CurrentHomeDirectory()
    {
        if (auto const* const pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr)
            return std::filesystem::path { pw->pw_dir };
        return {};
    }

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

    /// Where a job in @p scope writes its stdout/stderr.
    [[nodiscard]] std::filesystem::path DefaultLogDirectory(ServiceScope scope, std::filesystem::path const& home)
    {
        if (scope == ServiceScope::User)
            return home / "Library/Logs/fastcached";
        return std::filesystem::path { MacOsPrefix } / "var/log";
    }

    /// Apply the scope's per-user path defaults to @p cfg.
    ///
    /// A LaunchAgent that kept the in-memory default would lose the whole cache
    /// on every logout, which for a compile cache is most of the value. launchd
    /// expands neither `~` nor `$HOME` in ProgramArguments, so the concrete path
    /// has to be resolved here, at install time, by the process that knows it.
    ///
    /// The default is applied only when the caller named **no** config file. A
    /// CLI value baked into ProgramArguments outranks YAML in Merge, so
    /// injecting one alongside `--config` would override the `storage_path` in
    /// that very file for the life of the registration — the operator edits it,
    /// kickstarts the job, and nothing changes, with no error anywhere. Whoever
    /// passes a config file owns the storage path in it.
    ///
    /// System scope gets no storage default at all: its config file is always
    /// the package's `<prefix>/etc/fastcached.yaml`, so there is nothing to
    /// default.
    ///
    /// @param cfg Configuration as parsed from the command line.
    /// @param scope Domain being installed into.
    /// @param home The invoking user's home directory.
    /// @return @p cfg with `storagePath` filled in when the caller left it unset.
    [[nodiscard]] Config WithScopeDefaults(Config cfg, ServiceScope scope, std::filesystem::path const& home)
    {
        if (scope == ServiceScope::User && cfg.storagePath.empty() && cfg.configPath.empty())
            cfg.storagePath = (home / "Library/Caches/fastcached/cache").string();

        // Without --config the job reads no YAML at all: there is no default
        // search path, so an operator editing <prefix>/etc/fastcached.yaml and
        // kickstarting the job would see nothing change, with no error anywhere.
        // Only adopt the packaged file when it actually exists, so a
        // build-from-source install does not point launchd at a missing path.
        //
        // System scope only: that file describes the machine-wide daemon (its
        // cache lives under the package prefix, writable by the service account
        // alone), so handing it to a per-user agent would point the agent at a
        // directory it cannot write.
        if (scope == ServiceScope::System && cfg.configPath.empty())
        {
            auto const packaged = std::filesystem::path { MacOsPrefix } / "etc/fastcached.yaml";
            std::error_code ec;
            if (std::filesystem::exists(packaged, ec) && !ec)
                cfg.configPath = packaged.string();
        }
        return cfg;
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

    /// Why ServiceAccount could not read @p path, if it could not.
    ///
    /// A system daemon drops to ServiceAccount before it opens its config, so a
    /// root-owned mode-0600 file — exactly what InlineCredentialRejection sends
    /// the operator off to create — leaves launchd restarting a job that exits
    /// at every start, with the EACCES visible nowhere. Checked here, where the
    /// remedy can be named, rather than discovered from a restart loop.
    ///
    /// @param path Config file the job will be pointed at.
    /// @return A diagnostic naming the fix, or nullopt when the file is readable.
    [[nodiscard]] std::optional<std::string> ServiceAccountReadDenial(std::filesystem::path const& path)
    {
        auto const* const pw = ::getpwnam(std::string { ServiceAccount }.c_str());
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
                           ServiceAccount,
                           ServiceAccount,
                           path.string(),
                           path.string());
    }
} // namespace

ServiceControlResult InstallService(Config const& cfg, ServiceScope scope)
{
    if (auto const rejection = ServiceRegistrationRejection(cfg))
        return { .exitCode = 1, .message = *rejection };

    auto const exe = CurrentExecutablePath();
    if (exe.empty())
        return { .exitCode = 1, .message = "could not determine the fastcached executable path" };

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
    if (scope == ServiceScope::System && ::getpwnam(std::string { ServiceAccount }.c_str()) == nullptr)
        return { .exitCode = 1,
                 .message = std::format("the '{}' service account does not exist. It is created by the macOS "
                                        "installer package; for a manual install, create it first (see "
                                        "docs/operations/deployment.md) or use --service-scope=user.",
                                        ServiceAccount) };

    auto const effective = WithScopeDefaults(cfg, scope, home);
    auto const label = LaunchdLabel(effective);
    auto const plistPath = LaunchdPlistPath(effective, scope, home);
    auto const logDirectory = DefaultLogDirectory(scope, home);

    if (scope == ServiceScope::System && !effective.configPath.empty())
        if (auto const denial = ServiceAccountReadDenial(effective.configPath))
            return { .exitCode = 1, .message = *denial };

    std::error_code ec;
    std::filesystem::create_directories(plistPath.parent_path(), ec);
    if (ec)
        return { .exitCode = 1,
                 .message = std::format("could not create {}: {}", plistPath.parent_path().string(), ec.message()) };

    std::filesystem::create_directories(logDirectory, ec);
    if (ec)
        return { .exitCode = 1, .message = std::format("could not create {}: {}", logDirectory.string(), ec.message()) };

    // The daemon drops to ServiceAccount, so directories root created for it
    // have to change hands or its first write fails with EACCES — which launchd
    // surfaces only as a job that exits immediately, over and over.
    //
    // The storage directory itself, never its parent. Handing over the parent
    // gave away a directory the operator never named: `--storage=/var/db/fc`
    // reassigned /var/db, shared with other system services, to an unprivileged
    // cache account, and `--storage=/tmp/cache` reassigned /private/tmp — both
    // silently, under a message that said the service had been installed.
    if (scope == ServiceScope::System)
        if (auto const* const pw = ::getpwnam(std::string { ServiceAccount }.c_str()); pw != nullptr)
        {
            std::vector<std::filesystem::path> owned { logDirectory };
            if (!effective.storagePath.empty())
            {
                std::error_code storageEc;
                std::filesystem::create_directories(effective.storagePath, storageEc);
                if (!storageEc)
                    owned.emplace_back(effective.storagePath);
            }
            for (auto const& path: owned)
                (void) ::chown(path.c_str(), pw->pw_uid, pw->pw_gid);
        }

    {
        std::ofstream out { plistPath, std::ios::binary | std::ios::trunc };
        if (!out)
            return { .exitCode = 1, .message = std::format("could not write {}", plistPath.string()) };
        out << BuildLaunchdPlist(exe, effective, scope, logDirectory);
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
             .message = std::format("installed and started launchd job '{}' ({} scope, {})",
                                    label,
                                    ServiceScopeName(scope),
                                    plistPath.string()) };
}

ServiceControlResult UninstallService(Config const& cfg, ServiceScope scope)
{
    if (auto const rejection = ServiceNameRejection(cfg))
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

    auto const label = LaunchdLabel(cfg);
    auto const plistPath = LaunchdPlistPath(cfg, scope, home);

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

ServiceControlResult InstallService(Config const& /*cfg*/, ServiceScope /*scope*/)
{
    return { .exitCode = 1, .message = "service control is only available on Windows (SCM) and macOS (launchd)" };
}

ServiceControlResult UninstallService(Config const& /*cfg*/, ServiceScope /*scope*/)
{
    return { .exitCode = 1, .message = "service control is only available on Windows (SCM) and macOS (launchd)" };
}

#endif // _WIN32 / __APPLE__

} // namespace FastCache
