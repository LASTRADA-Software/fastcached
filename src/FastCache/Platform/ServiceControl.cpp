// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/ServiceControl.hpp>

#include <algorithm>
#include <array>
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
    #include <sys/wait.h>

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

    /// Wrap @p value in double quotes when it contains whitespace, so the SCM's
    /// command-line tokenizer keeps it as a single argument. Quote-free values
    /// pass through unchanged for a stable, readable command line.
    [[nodiscard]] std::string MaybeQuote(std::string_view value)
    {
        if (value.contains(' '))
            return std::format("\"{}\"", value);
        return std::string { value };
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

    if (daemonFlag == EmitDaemonFlag::Yes)
        argv.emplace_back("--daemon");

    // Unconditional: it lets the running service identify itself, and on launchd
    // it is what LaunchdLabel derives the job label from.
    argv.push_back(std::format("--service-name={}", cfg.serviceName));

    //        flag                    current value              emitted unless equal to
    emitIfSet("bind", cfg.bindAddress, defaults.bindAddress);
    emitIfSet("port", cfg.port, defaults.port);
    emitIfSet("max-memory", cfg.maxMemoryBytes, defaults.maxMemoryBytes);
    emitIfSet("log-level", cfg.logLevel, defaults.logLevel);
    emitPathIfSet("storage", cfg.storagePath);
    emitIfSet("storage-durability", cfg.storageDurability, defaults.storageDurability);
    emitIfSet("storage-max-value", cfg.storageMaxValueBytes, defaults.storageMaxValueBytes);
    emitIfSet("threads", cfg.workerThreads, defaults.workerThreads);
    emitIfSet("storage-shards", cfg.storageShards, defaults.storageShards);
    emitPathIfSet("config", cfg.configPath);

    return argv;
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
        std::string_view domain;         ///< launchctl domain target, `{}` filled with the uid.
        bool homeRelative;               ///< plistDirectory is relative to the user's home.
        bool alwaysKeepAlive;            ///< KeepAlive=<true/> vs {Crashed:true}; see below.
        bool runsAsServiceAccount;       ///< Emit UserName/GroupName.
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
                      .domain = "gui/{}",
                      .homeRelative = true,
                      .alwaysKeepAlive = false,
                      .runsAsServiceAccount = false },
        // The system daemon is the singleton owner of the port, so an unexpected
        // exit really should be restarted.
        ScopeTraits { .scope = ServiceScope::System,
                      .name = "system",
                      .plistDirectory = "/Library/LaunchDaemons",
                      .domain = "system",
                      .homeRelative = false,
                      .alwaysKeepAlive = true,
                      .runsAsServiceAccount = true },
    });

    /// The row describing @p scope.
    [[nodiscard]] constexpr ScopeTraits const& TraitsOf(ServiceScope scope) noexcept
    {
        auto const* const it = std::ranges::find(ScopeTable, scope, &ScopeTraits::scope);
        return it != ScopeTable.end() ? *it : ScopeTable.front();
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
    auto const* const it = std::ranges::find(ScopeTable, text, &ScopeTraits::name);
    if (it != ScopeTable.end())
        return it->scope;

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

    /// Where a job in @p scope writes its stdout/stderr.
    [[nodiscard]] std::filesystem::path DefaultLogDirectory(ServiceScope scope, std::filesystem::path const& home)
    {
        if (scope == ServiceScope::User)
            return home / "Library/Logs/fastcached";
        return "/opt/fastcached/var/log";
    }

    /// Apply the scope's per-user path defaults to @p cfg.
    ///
    /// A LaunchAgent that kept the in-memory default would lose the whole cache
    /// on every logout, which for a compile cache is most of the value. launchd
    /// expands neither `~` nor `$HOME` in ProgramArguments, so the concrete path
    /// has to be resolved here, at install time, by the process that knows it.
    ///
    /// @param cfg Configuration as parsed from the command line.
    /// @param scope Domain being installed into.
    /// @param home The invoking user's home directory.
    /// @return @p cfg with `storagePath` filled in when the caller left it unset.
    [[nodiscard]] Config WithScopeDefaults(Config cfg, ServiceScope scope, std::filesystem::path const& home)
    {
        if (cfg.storagePath.empty())
            cfg.storagePath = scope == ServiceScope::User ? (home / "Library/Caches/fastcached/cache").string()
                                                          : "/opt/fastcached/var/cache";
        return cfg;
    }

    /// Whether a launchctl invocation's own diagnostics reach the terminal.
    enum class LaunchctlOutput : std::uint8_t
    {
        Show,    ///< Let launchctl print; its message is the useful diagnostic.
        Silence, ///< Discard: this call is expected to fail in normal operation.
    };

    /// Run `launchctl` with @p args, waiting for it to finish.
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
            return -1;

        int status = 0;
        if (::waitpid(pid, &status, 0) < 0)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    /// The launchctl domain target for @p scope, e.g. `gui/501` or `system`.
    [[nodiscard]] std::string DomainTarget(ServiceScope scope)
    {
        auto const& traits = TraitsOf(scope);
        if (traits.domain.contains("{}"))
            return std::format("gui/{}", ::getuid());
        return std::string { traits.domain };
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
} // namespace

ServiceControlResult InstallService(Config const& cfg, ServiceScope scope)
{
    auto const exe = CurrentExecutablePath();
    if (exe.empty())
        return { .exitCode = 1, .message = "could not determine the fastcached executable path" };

    auto const home = CurrentHomeDirectory();
    if (scope == ServiceScope::User && home.empty())
        return { .exitCode = 1, .message = "could not determine the invoking user's home directory" };

    if (scope == ServiceScope::System && ::geteuid() != 0)
        return { .exitCode = 1, .message = "installing a system LaunchDaemon requires root; re-run with sudo" };

    auto const effective = WithScopeDefaults(cfg, scope, home);
    auto const label = LaunchdLabel(effective);
    auto const plistPath = LaunchdPlistPath(effective, scope, home);
    auto const logDirectory = DefaultLogDirectory(scope, home);

    std::error_code ec;
    std::filesystem::create_directories(plistPath.parent_path(), ec);
    if (ec)
        return { .exitCode = 1,
                 .message = std::format("could not create {}: {}", plistPath.parent_path().string(), ec.message()) };

    std::filesystem::create_directories(logDirectory, ec);
    if (ec)
        return { .exitCode = 1, .message = std::format("could not create {}: {}", logDirectory.string(), ec.message()) };

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
    // old ProgramArguments running and the freshly written plist ignored.
    // Silenced because "No such process" is the normal case on a first install.
    (void) RunLaunchctl({ "bootout", serviceTarget }, LaunchctlOutput::Silence);
    if (!WaitForJobGone(serviceTarget))
        return { .exitCode = 1,
                 .message = std::format("'{}' is still registered after bootout; "
                                        "run `launchctl bootout {}` manually and retry",
                                        label,
                                        serviceTarget) };

    if (auto const rc = RunLaunchctl({ "bootstrap", domain, plistPath.string() }); rc != 0)
        return { .exitCode = 1,
                 .message = std::format(
                     "wrote {} but `launchctl bootstrap {}` failed (status {})", plistPath.string(), domain, rc) };

    // bootstrap only *loads* the job. Despite RunAtLoad, launchd records the
    // spawn as "pended nondemand spawn = speculative" and can leave it unstarted
    // indefinitely — this command reported success while `launchctl print` said
    // "state = not running" and nothing was listening on the port. kickstart
    // forces the spawn, so the service really is up when this returns.
    if (auto const rc = RunLaunchctl({ "kickstart", "-k", serviceTarget }); rc != 0)
        return { .exitCode = 1,
                 .message = std::format("registered '{}' but `launchctl kickstart` failed (status {}); "
                                        "it will start at the next login or boot",
                                        label,
                                        rc) };

    return { .exitCode = 0,
             .message = std::format("installed and started launchd job '{}' ({} scope, {})",
                                    label,
                                    ServiceScopeName(scope),
                                    plistPath.string()) };
}

ServiceControlResult UninstallService(Config const& cfg, ServiceScope scope)
{
    auto const home = CurrentHomeDirectory();
    if (scope == ServiceScope::User && home.empty())
        return { .exitCode = 1, .message = "could not determine the invoking user's home directory" };

    if (scope == ServiceScope::System && ::geteuid() != 0)
        return { .exitCode = 1, .message = "removing a system LaunchDaemon requires root; re-run with sudo" };

    auto const label = LaunchdLabel(cfg);
    auto const plistPath = LaunchdPlistPath(cfg, scope, home);

    // Best-effort stop; a job that was never loaded is not an error here, so
    // launchctl's "No such process" complaint is suppressed. Waited on for the
    // same reason as the install path: bootout returns while the job is still
    // winding down, and reporting removal before launchd has let go would make
    // "uninstalled" a claim the very next command could contradict.
    auto const serviceTarget = std::format("{}/{}", DomainTarget(scope), label);
    (void) RunLaunchctl({ "bootout", serviceTarget }, LaunchctlOutput::Silence);
    if (!WaitForJobGone(serviceTarget))
        return { .exitCode = 1,
                 .message = std::format("'{}' is still registered after bootout; "
                                        "run `launchctl bootout {}` manually and retry",
                                        label,
                                        serviceTarget) };

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
