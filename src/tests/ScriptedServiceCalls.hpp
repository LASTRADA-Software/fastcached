// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/ServiceControl.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// What each of the service manager's calls answers. Every field defaults to the call
/// succeeding, so a case states only the answer it is about.
struct ScmScript
{
    std::filesystem::path executable { "C:/Program Files/fastcached/bin/fastcached.exe" }; ///< Empty: unknown.
    std::optional<std::uint32_t> openManagerError {}; ///< `OpenSCManager`'s error, if it fails.
    std::optional<std::uint32_t> createError {};      ///< `CreateService`'s error, if it fails.
    std::optional<std::uint32_t> reapplyError {};     ///< Re-applying an existing registration's error, if it fails.
    std::optional<std::uint32_t> openError {};        ///< `OpenService`'s error, if it fails.
    std::optional<std::uint32_t> recoveryError {};    ///< The restart policy's error, if refused.
    std::optional<std::uint32_t> nonCrashError {};    ///< The ordinary-failure flag's error, if refused.
    std::optional<std::uint32_t> sidTypeError {};     ///< The service SID type's error, if refused.
    std::size_t runningPolls { 0 };                   ///< How many status reads still answer "running".
    std::optional<std::uint32_t> deleteError {};      ///< `DeleteService`'s error, if it fails.
    std::optional<std::string> eventSourceDenial {};  ///< Why the event source could not be added.
    std::optional<std::string> grantDenial {};        ///< Why a path could not be granted.
    std::optional<std::string> secludeDenial {};      ///< Why a path could not be secluded.
};

/// The Windows service manager, scripted: every call answers what `ScmScript` says and is
/// recorded, and every handle it hands out is tracked until it is closed.
///
/// `src/tests/` rather than beside `ServiceControl_test.cpp`: it implements a library interface,
/// and any binary whose registration goes through `ScmInstall` is a caller.
class ScriptedScmCalls final: public IScmCalls
{
  public:
    /// @param script What each call answers.
    explicit ScriptedScmCalls(ScmScript script):
        _script { std::move(script) }
    {
    }

    [[nodiscard]] std::filesystem::path ExecutablePath() override
    {
        _calls.emplace_back("ExecutablePath");
        return _script.executable;
    }

    [[nodiscard]] std::expected<ScmHandle, std::uint32_t> OpenManager(ScmManagerAccess access) override
    {
        _calls.emplace_back(access == ScmManagerAccess::Create ? "OpenManager(Create)" : "OpenManager(Connect)");
        return Opened(_script.openManagerError);
    }

    [[nodiscard]] std::expected<ScmHandle, std::uint32_t> Create(ScmHandle manager,
                                                                 ScmServiceRequest const& request) override
    {
        _calls.emplace_back("Create");
        _requested = request;
        return Usable(manager) ? Opened(_script.createError) : std::unexpected { UnknownHandle };
    }

    [[nodiscard]] std::expected<ScmHandle, std::uint32_t> Reapply(ScmHandle manager,
                                                                  ScmServiceRequest const& request) override
    {
        _calls.emplace_back("Reapply");
        _requested = request;
        return Usable(manager) ? Opened(_script.reapplyError) : std::unexpected { UnknownHandle };
    }

    [[nodiscard]] std::expected<ScmHandle, std::uint32_t> Open(ScmHandle manager, std::string const& /*name*/) override
    {
        _calls.emplace_back("Open");
        return Usable(manager) ? Opened(_script.openError) : std::unexpected { UnknownHandle };
    }

    void Describe(ScmHandle service, std::string const& /*description*/) override
    {
        _calls.emplace_back(Usable(service) ? "Describe" : "Describe(unknown handle)");
    }

    [[nodiscard]] std::optional<std::uint32_t> SetRecovery(ScmHandle service,
                                                           std::span<ScmRecoveryAction const> /*actions*/) override
    {
        _calls.emplace_back(Usable(service) ? "SetRecovery" : "SetRecovery(unknown handle)");
        return _script.recoveryError;
    }

    [[nodiscard]] std::optional<std::uint32_t> RecoverOnNonCrashFailures(ScmHandle service) override
    {
        _calls.emplace_back(Usable(service) ? "RecoverOnNonCrashFailures" : "RecoverOnNonCrashFailures(unknown handle)");
        return _script.nonCrashError;
    }

    [[nodiscard]] std::optional<std::uint32_t> SetSidType(ScmHandle service) override
    {
        _calls.emplace_back(Usable(service) ? "SetSidType" : "SetSidType(unknown handle)");
        return _script.sidTypeError;
    }

    void Stop(ScmHandle service) override
    {
        _calls.emplace_back(Usable(service) ? "Stop" : "Stop(unknown handle)");
    }

    [[nodiscard]] bool StillRunning(ScmHandle service) override
    {
        _calls.emplace_back(Usable(service) ? "StillRunning" : "StillRunning(unknown handle)");
        if (_script.runningPolls == 0)
            return false;
        --_script.runningPolls;
        return true;
    }

    [[nodiscard]] std::optional<std::uint32_t> Delete(ScmHandle service) override
    {
        _calls.emplace_back(Usable(service) ? "Delete" : "Delete(unknown handle)");
        return _script.deleteError;
    }

    void Close(ScmHandle handle) override
    {
        _calls.emplace_back(_open.erase(handle.value) == 1 ? "Close" : "Close(unknown handle)");
    }

    [[nodiscard]] std::optional<std::string> AddEventSource(std::string const& /*serviceName*/) override
    {
        _calls.emplace_back("AddEventSource");
        return _script.eventSourceDenial;
    }

    void RemoveEventSource(std::string const& /*serviceName*/) override
    {
        _calls.emplace_back("RemoveEventSource");
    }

    [[nodiscard]] std::optional<std::string> GrantPathAccess(std::filesystem::path const& /*target*/,
                                                             std::string const& /*account*/) override
    {
        _calls.emplace_back("GrantPathAccess");
        return _script.grantDenial;
    }

    [[nodiscard]] std::optional<std::string> SecludePath(
        std::filesystem::path const& /*target*/,
        std::string const& /*account*/,
        std::span<std::filesystem::path const> /*credentialLeaves*/) override
    {
        _calls.emplace_back("SecludePath");
        return _script.secludeDenial;
    }

    /// @return Every call made, in order, by name.
    [[nodiscard]] std::vector<std::string> const& Calls() const noexcept
    {
        return _calls;
    }

    /// @return How many handles were handed out and not closed.
    [[nodiscard]] std::size_t OpenHandles() const noexcept
    {
        return _open.size();
    }

    /// @return The service `Create` or `Reapply` was last asked for, if it was.
    [[nodiscard]] std::optional<ScmServiceRequest> const& Requested() const noexcept
    {
        return _requested;
    }

  private:
    /// Answered for a call on a handle this never handed out, or already closed: no real code.
    static constexpr std::uint32_t UnknownHandle = 0xFFFF'FFFFU;

    /// @param error The scripted failure, if any.
    /// @return A fresh handle, tracked as open, or @p error.
    [[nodiscard]] std::expected<ScmHandle, std::uint32_t> Opened(std::optional<std::uint32_t> error)
    {
        if (error.has_value())
            return std::unexpected { *error };
        auto const handle = ScmHandle { .value = ++_issued };
        _open.insert(handle.value);
        return handle;
    }

    /// @param handle A handle a registration passed back.
    /// @return Whether it is one this handed out and has not seen closed.
    [[nodiscard]] bool Usable(ScmHandle handle) const
    {
        return _open.contains(handle.value);
    }

    ScmScript _script;
    std::vector<std::string> _calls;
    std::set<std::uintptr_t> _open;
    std::uintptr_t _issued { 0 };
    std::optional<ScmServiceRequest> _requested;
};

/// What launchd and the system around it answer. Every field defaults to the call succeeding
/// for an ordinary user, so a case states only the answer it is about.
struct LaunchdScript
{
    bool root { false };                                                ///< Whether the effective user is root.
    std::uint32_t userId { 501 };                                       ///< The real user id.
    std::filesystem::path home { "/Users/operator" };                   ///< Empty: unknown.
    bool accountExists { true };                                        ///< Whether the service account resolves.
    std::filesystem::path packagedConfig {};                            ///< The packaged configuration that counts.
    std::optional<std::string> readDenial {};                           ///< Why the account cannot read its config.
    std::optional<std::error_code> createError {};                      ///< `create_directories`' error, if it fails.
    std::string handOverWarnings {};                                    ///< What handing paths over warned.
    std::optional<std::string> handOverRefusal {};                      ///< Why a `Private` path refused the install.
    bool jobFileExists { false };                                       ///< Whether a job file is there already.
    bool writes { true };                                               ///< Whether the job file is written.
    std::vector<std::string> known { "gui/501", "user/501", "system" }; ///< What `launchctl print` finds.
    std::optional<int> bootstrapStatus {};                              ///< `bootstrap`'s non-zero status, if it fails.
    std::optional<int> kickstartStatus {};                              ///< `kickstart`'s non-zero status, if it fails.
    std::expected<bool, std::error_code> removal { true };              ///< Removing the job file.
};

/// launchd, scripted: every call answers what `LaunchdScript` says and is recorded.
class ScriptedLaunchdCalls final: public ILaunchdCalls
{
  public:
    /// @param script What each call answers.
    explicit ScriptedLaunchdCalls(LaunchdScript script):
        _script { std::move(script) }
    {
    }

    [[nodiscard]] bool RunsAsRoot() override
    {
        return _script.root;
    }

    [[nodiscard]] std::uint32_t UserId() override
    {
        return _script.userId;
    }

    [[nodiscard]] std::filesystem::path HomeDirectory() override
    {
        _calls.emplace_back("HomeDirectory");
        return _script.home;
    }

    [[nodiscard]] bool AccountExists(std::string const& /*account*/) override
    {
        return _script.accountExists;
    }

    [[nodiscard]] std::filesystem::path PackagedConfig(std::string_view /*applicationName*/) override
    {
        return _script.packagedConfig;
    }

    [[nodiscard]] std::optional<std::string> AccountReadDenial(std::string const& /*account*/,
                                                               std::filesystem::path const& /*path*/) override
    {
        return _script.readDenial;
    }

    [[nodiscard]] std::error_code CreateDirectories(std::filesystem::path const& path) override
    {
        _calls.emplace_back("CreateDirectories " + path.generic_string());
        return _script.createError.value_or(std::error_code {});
    }

    [[nodiscard]] OwnedPathsHandedOver HandOver(ServiceSpec const& /*effective*/,
                                                std::filesystem::path const& /*logDirectory*/) override
    {
        _calls.emplace_back("HandOver");
        return OwnedPathsHandedOver { .warnings = _script.handOverWarnings, .refusal = _script.handOverRefusal };
    }

    [[nodiscard]] bool JobFileExists(std::filesystem::path const& /*path*/) override
    {
        return _script.jobFileExists;
    }

    [[nodiscard]] bool WriteJobFile(std::filesystem::path const& path, std::string const& /*text*/) override
    {
        _calls.emplace_back("WriteJobFile " + path.generic_string());
        return _script.writes;
    }

    [[nodiscard]] LaunchctlReadings Launchctl(std::vector<std::string> const& args, LaunchctlOutput /*output*/) override
    {
        auto line = std::string { "launchctl" };
        for (auto const& arg: args)
            line += " " + arg;
        _calls.push_back(line);

        auto const verb = args.empty() ? std::string_view {} : std::string_view { args.front() };
        if (verb == "print")
            return Exited(args.size() > 1 && std::ranges::count(_script.known, args[1]) != 0 ? 0 : 113);
        if (verb == "bootstrap")
            return Exited(_script.bootstrapStatus.value_or(0));
        if (verb == "kickstart")
            return Exited(_script.kickstartStatus.value_or(0));
        return Exited(0);
    }

    void Pause(std::chrono::milliseconds /*interval*/) override
    {
        _calls.emplace_back("Pause");
    }

    [[nodiscard]] std::expected<bool, std::error_code> RemoveJobFile(std::filesystem::path const& path) override
    {
        _calls.emplace_back("RemoveJobFile " + path.generic_string());
        return _script.removal;
    }

    /// @return Every call made that a case may ask about, in order.
    [[nodiscard]] std::vector<std::string> const& Calls() const noexcept
    {
        return _calls;
    }

  private:
    /// @param status The exit status.
    /// @return A call that ran and exited with @p status.
    [[nodiscard]] static LaunchctlReadings Exited(int status)
    {
        auto readings = LaunchctlReadings {};
        readings.outcome = LaunchctlOutcome::Exited;
        readings.exitStatus = status;
        return readings;
    }

    LaunchdScript _script;
    std::vector<std::string> _calls;
};

} // namespace FastCache::Testing
