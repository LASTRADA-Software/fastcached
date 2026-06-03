// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>

#include <filesystem>
#include <string>

namespace FastCache
{

/// Build the command line the Windows Service Control Manager (SCM) will launch
/// when it starts the registered service.
///
/// The result is the quoted executable path followed by `--daemon` (the SCM
/// runtime hook, see WindowsServiceHost), `--service-name=<name>`, and one
/// `--flag=value` token for every @p cfg field that differs from a
/// default-constructed Config. Path-bearing flags (`--storage`, `--config`) are
/// made absolute because a Windows service runs with its current directory set
/// to `C:\Windows\System32`, so a relative path captured at install time would
/// resolve to the wrong location at service start.
///
/// `--install-service` is deliberately never re-emitted: it is not a Config
/// field, so the service can never recursively re-install itself.
///
/// Pure and platform-independent so it can be unit-tested on every platform.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cfg Effective configuration to embed in the service command line.
/// @return Fully-quoted command line string.
[[nodiscard]] std::string BuildServiceCommandLine(std::filesystem::path const& exePath, Config const& cfg);

/// Outcome of a service-control operation.
struct ServiceControlResult
{
    int exitCode { 0 };     ///< Process exit code (0 = success).
    std::string message {}; ///< Human-readable status / error message.
};

/// Register fastcached as a Windows service with the SCM.
///
/// The service is created with start type `SERVICE_AUTO_START` (it runs on every
/// boot) but is left **stopped** after registration — the caller must start it
/// explicitly (`sc start <name>`) for this session. The launch command line is
/// produced by BuildServiceCommandLine, so every non-default flag passed
/// alongside `--install-service` is baked in and reused on every start.
///
/// On non-Windows platforms this is a no-op that reports an error.
///
/// @param cfg Effective configuration; `serviceName` names the service and the
///            remaining fields are embedded in the launch command line.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, already exists).
[[nodiscard]] ServiceControlResult InstallWindowsService(Config const& cfg);

/// Remove a previously-registered fastcached Windows service.
///
/// Best-effort stops the service first, then deletes it from the SCM. On
/// non-Windows platforms this is a no-op that reports an error.
///
/// @param cfg Effective configuration; only `serviceName` is used.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, no such service).
[[nodiscard]] ServiceControlResult UninstallWindowsService(Config const& cfg);

} // namespace FastCache
