// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/ServiceControl.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#if defined(_WIN32)
    #include <windows.h>
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

    /// CLI spelling of an ExecutionModel, matching ParseExecutionModel.
    [[nodiscard]] constexpr std::string_view ExecutionModelName(ExecutionModel model) noexcept
    {
        switch (model)
        {
            case ExecutionModel::Auto:
                return "auto";
            case ExecutionModel::Threaded:
                return "threaded";
            case ExecutionModel::Reactor:
                return "reactor";
        }
        return "auto";
    }

    /// Wrap @p value in double quotes when it contains whitespace, so the SCM's
    /// command-line tokenizer keeps it as a single argument. Quote-free values
    /// pass through unchanged for a stable, readable command line.
    [[nodiscard]] std::string MaybeQuote(std::string_view value)
    {
        if (value.find(' ') != std::string_view::npos)
            return std::format("\"{}\"", value);
        return std::string { value };
    }

    /// Absolutize a captured path so it survives the service's
    /// `C:\Windows\System32` working directory, then quote if needed.
    [[nodiscard]] std::string AbsolutePathArg(std::string const& path)
    {
        std::error_code ec;
        auto const abs = std::filesystem::absolute(path, ec);
        return MaybeQuote(ec ? path : abs.string());
    }
} // namespace

std::string BuildServiceCommandLine(std::filesystem::path const& exePath, Config const& cfg)
{
    Config const defaults {};

    // The executable path is always quoted so an install directory containing
    // spaces (e.g. "C:\Program Files\fastcached") tokenizes correctly.
    std::string out = std::format("\"{}\"", exePath.string());

    // --daemon is the SCM runtime hook; --service-name lets the running service
    // identify itself. Both are emitted unconditionally.
    out += " --daemon";
    out += std::format(" --service-name={}", MaybeQuote(cfg.serviceName));

    // Every remaining flag is emitted only when it differs from the default, so
    // the command line stays minimal and self-documenting.
    if (cfg.bindAddress != defaults.bindAddress)
        out += std::format(" --bind={}", MaybeQuote(cfg.bindAddress));
    if (cfg.port != defaults.port)
        out += std::format(" --port={}", cfg.port);
    if (cfg.maxMemoryBytes != defaults.maxMemoryBytes)
        out += std::format(" --max-memory={}", cfg.maxMemoryBytes);
    if (cfg.logLevel != defaults.logLevel)
        out += std::format(" --log-level={}", LogLevelName(cfg.logLevel));
    if (!cfg.storagePath.empty())
        out += std::format(" --storage={}", AbsolutePathArg(cfg.storagePath));
    if (cfg.storageDurability != defaults.storageDurability)
        out += std::format(" --storage-durability={}", DurabilityName(cfg.storageDurability));
    if (cfg.storageMaxValueBytes != defaults.storageMaxValueBytes)
        out += std::format(" --storage-max-value={}", cfg.storageMaxValueBytes);
    if (cfg.executionModel != defaults.executionModel)
        out += std::format(" --execution-model={}", ExecutionModelName(cfg.executionModel));
    if (cfg.workerThreads != defaults.workerThreads)
        out += std::format(" --threads={}", cfg.workerThreads);
    if (cfg.storageShards != defaults.storageShards)
        out += std::format(" --storage-shards={}", cfg.storageShards);
    if (!cfg.configPath.empty())
        out += std::format(" --config={}", AbsolutePathArg(cfg.configPath));

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

ServiceControlResult InstallWindowsService(Config const& cfg)
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

ServiceControlResult UninstallWindowsService(Config const& cfg)
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

#else

ServiceControlResult InstallWindowsService(Config const& /*cfg*/)
{
    return { .exitCode = 1, .message = "Windows service control is only available on Windows" };
}

ServiceControlResult UninstallWindowsService(Config const& /*cfg*/)
{
    return { .exitCode = 1, .message = "Windows service control is only available on Windows" };
}

#endif // _WIN32

} // namespace FastCache
