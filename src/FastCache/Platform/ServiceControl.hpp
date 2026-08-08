// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Whether the generated argument list carries the `--daemon` flag.
///
/// The two supervisors disagree about it, so it cannot be a constant. The
/// Windows SCM needs it: it is the hook that hands control to
/// WindowsServiceHost. launchd needs its absence: like systemd it supervises a
/// process it started in the foreground, and a job that forks is reaped
/// immediately as "exited" — the same reason the shipped systemd unit does not
/// pass it (see packaging/linux/fastcached.service).
enum class EmitDaemonFlag : std::uint8_t
{
    No,  ///< Foreground supervision (launchd, systemd).
    Yes, ///< Fork into the background (Windows SCM).
};

/// Build the argument vector a service supervisor launches fastcached with.
///
/// Element 0 is @p exePath; the rest are `--flag=value` tokens, one for every
/// @p cfg field that differs from a default-constructed Config, plus
/// `--service-name=<name>` and — depending on @p daemonFlag — `--daemon`.
/// Path-bearing flags (`--storage`, `--config`) are made absolute because a
/// service does not inherit the installing shell's working directory, so a
/// relative path captured at install time would resolve elsewhere at start.
///
/// Values are returned **unquoted**: quoting is a property of a flat command
/// line, not of an argv array, and launchd's `ProgramArguments` must receive
/// the literal value. BuildServiceCommandLine adds quotes when it joins.
///
/// `--install-service` is deliberately never re-emitted: it is not a Config
/// field, so the service can never recursively re-install itself.
///
/// Pure and platform-independent so it can be unit-tested on every platform.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cfg Effective configuration to embed in the launch arguments.
/// @param daemonFlag Whether to emit `--daemon`.
/// @return Argument vector, `exePath` first.
[[nodiscard]] std::vector<std::string> BuildServiceArgv(std::filesystem::path const& exePath,
                                                        Config const& cfg,
                                                        EmitDaemonFlag daemonFlag);

/// Build the command line the Windows Service Control Manager (SCM) will launch
/// when it starts the registered service.
///
/// BuildServiceArgv joined into one string, with `--daemon` emitted and any
/// value containing whitespace quoted so the SCM's tokenizer keeps it as a
/// single argument. Only the value is quoted, not the whole `--flag=value`
/// token, which is what the SCM expects.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cfg Effective configuration to embed in the service command line.
/// @return Fully-quoted command line string.
[[nodiscard]] std::string BuildServiceCommandLine(std::filesystem::path const& exePath, Config const& cfg);

/// Why @p cfg cannot be handed to a service supervisor, if it cannot.
///
/// The one Config field with no safe representation in launch arguments is
/// `requirePass`: a supervisor records those arguments where every local
/// account can read them, so the shared secret would be published to exactly
/// the accounts it exists to keep out. BuildServiceArgv therefore never emits
/// it, and this reports the omission instead of leaving it silent — an install
/// that quietly drops the password would come up unauthenticated while telling
/// the operator it succeeded.
///
/// Supplying the secret through `--config` is accepted: the file can be mode
/// 0600 and the daemon re-reads it on every start and reload.
///
/// Pure, so the rule is unit-testable on every platform.
///
/// @param cfg Configuration about to be baked into a service registration.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> InlineCredentialRejection(Config const& cfg);

/// Parse the `--service-scope` argument.
/// @param text One of `user` or `system`, lowercase.
/// @return The scope, or a ConfigError naming the accepted spellings.
[[nodiscard]] std::expected<ServiceScope, ConfigError> ParseServiceScope(std::string_view text);

/// CLI spelling of @p scope, the inverse of ParseServiceScope.
/// @param scope Scope to name.
/// @return `"user"` or `"system"`.
[[nodiscard]] std::string_view ServiceScopeName(ServiceScope scope) noexcept;

/// The reverse-DNS launchd job label for @p cfg.
///
/// launchd requires labels to be unique process-wide and conventionally
/// reverse-DNS; it is also the handle every `launchctl` subcommand takes, and
/// the name the job appears under in System Settings → Login Items.
///
/// @param cfg Effective configuration; only `serviceName` is used.
/// @return e.g. `software.lastrada.fastcached`.
[[nodiscard]] std::string LaunchdLabel(Config const& cfg);

/// Absolute path of the plist file backing a job in @p scope.
/// @param cfg Effective configuration; names the plist via LaunchdLabel.
/// @param scope Which domain the job belongs to.
/// @param homeDirectory The user's home directory; used only for
///        ServiceScope::User, ignored otherwise.
/// @return Absolute path, e.g. `/Library/LaunchDaemons/<label>.plist`.
[[nodiscard]] std::filesystem::path LaunchdPlistPath(Config const& cfg,
                                                     ServiceScope scope,
                                                     std::filesystem::path const& homeDirectory);

/// Render the launchd job description for @p cfg as an XML property list.
///
/// Pure and platform-independent so it can be unit-tested on every platform,
/// and so the plists shipped in the package can be generated by the very binary
/// that later registers them — one implementation, no drift.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cfg Effective configuration to embed in `ProgramArguments`.
/// @param scope Which domain the job is for; selects the supervision policy
///        (see the scope table in the implementation).
/// @param logDirectory Directory for the job's stdout/stderr files.
/// @return A complete `<?xml ...?><plist>` document.
[[nodiscard]] std::string BuildLaunchdPlist(std::filesystem::path const& exePath,
                                            Config const& cfg,
                                            ServiceScope scope,
                                            std::filesystem::path const& logDirectory);

/// Outcome of a service-control operation.
struct ServiceControlResult
{
    int exitCode { 0 };     ///< Process exit code (0 = success).
    std::string message {}; ///< Human-readable status / error message.
};

/// Register fastcached with the platform's service supervisor.
///
/// Windows: creates an SCM service with start type `SERVICE_AUTO_START` (it runs
/// on every boot) but leaves it **stopped** — the caller starts it explicitly
/// (`sc start <name>`) for this session.
///
/// macOS: writes a launchd job description (see BuildLaunchdPlist) to the
/// directory @p scope names and bootstraps it, so it is running when this
/// returns and again after every boot or login.
///
/// Either way the launch arguments come from BuildServiceArgv, so every
/// non-default flag passed alongside `--install-service` is baked in and reused
/// on every start.
///
/// On platforms with neither supervisor this is a no-op that reports an error.
///
/// @param cfg Effective configuration; `serviceName` names the service and the
///            remaining fields are embedded in the launch arguments.
/// @param scope Which supervisor domain to register in. Ignored on Windows,
///              which has only one.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, already exists).
[[nodiscard]] ServiceControlResult InstallService(Config const& cfg, ServiceScope scope = ServiceScope::System);

/// Remove a previously-registered fastcached service.
///
/// Best-effort stops (Windows) or boots out (macOS) the service first, then
/// deletes its registration. On platforms with no supervisor this is a no-op
/// that reports an error.
///
/// @param cfg Effective configuration; only `serviceName` is used.
/// @param scope Which supervisor domain to remove from. Ignored on Windows.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, no such service).
[[nodiscard]] ServiceControlResult UninstallService(Config const& cfg, ServiceScope scope = ServiceScope::System);

} // namespace FastCache
