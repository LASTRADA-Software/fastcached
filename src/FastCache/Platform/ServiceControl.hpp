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

/// Whether the command line that asked for a registration carried a secret.
///
/// An `enum class` and not a `bool` because the call site reads as a claim about
/// the invocation rather than as a flag: `InlineCredential::Present` says what is
/// true, where `true` would say nothing at all.
enum class InlineCredential : std::uint8_t
{
    Absent,  ///< Nothing secret would reach the supervisor.
    Present, ///< A credential was typed alongside `--install-service`.
};

/// A service to register, described independently of which binary runs it.
///
/// This is the seam that lets one implementation of "install this as a service"
/// serve more than one executable. Every function below used to take
/// `Config const&` -- the *daemon's* configuration type -- so a second binary
/// could not reach any of it without either depending on the daemon's config or
/// growing a parallel copy of this entire file, and `fastcache-compile-node` had
/// no service integration on macOS or Windows for exactly that reason.
///
/// What is deliberately **not** here is how `arguments` was derived. An
/// `OptionSpec` describes how to *parse* a flag and carries no way to read a
/// value back out, so "emit every field that differs from a default" cannot be
/// written once generically -- each binary walks its own table and hands the
/// result over. That is why `BuildServiceArgv` stays hand-written per binary and
/// is guarded by a test that walks the option table and requires every
/// non-excluded flag to be emitted.
struct ServiceSpec
{
    /// The supervisor's key: the SCM service name, and the stem of the launchd
    /// label and plist file. Reaches the filesystem, so `ServiceNameRejection`
    /// has rules about it.
    std::string serviceName;

    /// Absolute path to the executable the supervisor launches.
    std::filesystem::path exePath;

    /// `argv[1..]`, unquoted, and **without** any backgrounding flag.
    ///
    /// Values are unquoted because quoting is a property of a flat command line
    /// rather than of an argv array, and launchd's `ProgramArguments` must
    /// receive the literal value. `BuildServiceCommandLine` adds quotes when it
    /// joins.
    std::vector<std::string> arguments;

    /// The token that makes this binary fork into the background, empty when it
    /// has none.
    ///
    /// Held apart from `arguments` rather than baked into them, because the two
    /// supervisors disagree and one spec has to answer both. The Windows SCM
    /// needs it -- it is the hook that hands control to `WindowsServiceHost` --
    /// and launchd needs its absence: like systemd it supervises the process it
    /// started, so a job that double-forks is reaped instantly as "exited".
    std::string daemonFlag;

    /// Name shown by `services.msc` and in Login Items.
    std::string displayName;

    /// One-line description registered with the SCM.
    std::string description;

    /// The unprivileged account a system-scope job runs as, empty to run as the
    /// supervisor's default (root / LocalSystem).
    ///
    /// A field rather than the constant it used to be, because it is the answer
    /// to "who owns this service's files" and a second binary may well want a
    /// different one.
    std::string serviceAccount;

    /// Directories `serviceAccount` must own before the job first runs.
    ///
    /// Only ever directories the operator actually named. Handing over a
    /// **parent** gives away a directory nobody asked about: `--storage=/var/db/fc`
    /// would reassign `/var/db`, shared with other system services, to an
    /// unprivileged cache account -- silently, under a message saying the service
    /// had been installed. The daemon drops to `serviceAccount`, so a directory
    /// root created for it has to change hands or the first write fails with
    /// EACCES, which launchd surfaces only as a job that exits over and over.
    std::vector<std::filesystem::path> ownedDirectories;

    /// Whether a secret was typed on the installing command line.
    InlineCredential inlineCredential { InlineCredential::Absent };

    /// The `--config` path the operator named, or empty.
    ///
    /// Used only to make the credential refusal say where the secret should go
    /// instead. It is not a hint that the combination is safe -- see
    /// `InlineCredentialRejection`.
    std::string configPath;
};

/// Resolve the absolute path of the running executable.
///
/// Published because a `ServiceSpec` states what the supervisor should launch,
/// so every binary that builds one has to be able to answer it. Resolved through
/// symlinks where the platform can: whatever records this path keeps it verbatim,
/// and a path through a symlink that later moves pins a service to nothing.
/// @return Path on success; an empty path when it cannot be determined.
[[nodiscard]] std::filesystem::path CurrentExecutablePath();

/// Describe the running daemon as a service to register.
///
/// The daemon's half of the seam: it owns `Config`, so it is what turns one into
/// a `ServiceSpec`. `fastcache-compile-node` has its own equivalent over its own
/// configuration type.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cfg Effective configuration to embed in the launch arguments.
/// @return The spec a supervisor is registered from.
[[nodiscard]] ServiceSpec MakeDaemonServiceSpec(std::filesystem::path const& exePath, Config const& cfg);

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
[[nodiscard]] std::string BuildServiceCommandLine(ServiceSpec const& spec);

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
/// `--config` does not make the combination acceptable: nothing can tell from
/// here whether the named file actually carries `requirepass:`, so waving it
/// through was the silent drop under another name — the operator was told their
/// password had been registered and got an unauthenticated daemon. The secret
/// belongs in the config file *instead of* on the command line.
///
/// Pure, so the rule is unit-testable on every platform.
///
/// @param spec Service about to be registered.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> InlineCredentialRejection(ServiceSpec const& spec);

/// Why @p cfg's `serviceName` cannot name a service, if it cannot.
///
/// The name reaches the filesystem: LaunchdPlistPath concatenates it into the
/// directory launchd scans, and the SCM keys its registry entry on it. A
/// separator or a `..` therefore escapes that directory — writing a root-owned
/// file somewhere no uninstall path knows about — and a merely misplaced
/// character puts the plist where the supervisor never looks, while the install
/// still reports success.
///
/// Pure, so the rule is unit-testable on every platform.
///
/// @param spec Service whose `serviceName` is to be validated.
/// @return An explanatory message when the name must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> ServiceNameRejection(ServiceSpec const& spec);

/// Why @p cfg cannot be handed to a service supervisor at all, if it cannot.
///
/// Runs every registration rule in turn — currently ServiceNameRejection and
/// InlineCredentialRejection — and reports the first that objects, so both
/// platforms' InstallService share one gate and a new rule is a new table row.
///
/// @param spec Service about to be registered.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> ServiceRegistrationRejection(ServiceSpec const& spec);

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
/// @param spec Service to label; only `serviceName` is used.
/// @return e.g. `software.lastrada.fastcached`.
[[nodiscard]] std::string LaunchdLabel(ServiceSpec const& spec);

/// Absolute path of the plist file backing a job in @p scope.
/// @param spec Service to locate; names the plist via LaunchdLabel.
/// @param scope Which domain the job belongs to.
/// @param homeDirectory The user's home directory; used only for
///        ServiceScope::User, ignored otherwise.
/// @return Absolute path, e.g. `/Library/LaunchDaemons/<label>.plist`.
[[nodiscard]] std::filesystem::path LaunchdPlistPath(ServiceSpec const& spec,
                                                     ServiceScope scope,
                                                     std::filesystem::path const& homeDirectory);

/// Render the launchd job description for @p cfg as an XML property list.
///
/// Pure and platform-independent so it can be unit-tested on every platform,
/// and so the plists shipped in the package can be generated by the very binary
/// that later registers them — one implementation, no drift.
///
/// @param spec Service to describe; `exePath` and `arguments` become
///        `ProgramArguments`, and `daemonFlag` is deliberately not emitted.
/// @param scope Which domain the job is for; selects the supervision policy
///        (see the scope table in the implementation).
/// @param logDirectory Directory for the job's stdout/stderr files.
/// @return A complete `<?xml ...?><plist>` document.
[[nodiscard]] std::string BuildLaunchdPlist(ServiceSpec const& spec,
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
/// @param spec Service to register: what to launch, with which arguments, under
///             which name.
/// @param scope Which supervisor domain to register in. Ignored on Windows,
///              which has only one.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, already exists).
[[nodiscard]] ServiceControlResult InstallService(ServiceSpec const& spec, ServiceScope scope = ServiceScope::System);

/// Remove a previously-registered fastcached service.
///
/// Best-effort stops (Windows) or boots out (macOS) the service first, then
/// deletes its registration. On platforms with no supervisor this is a no-op
/// that reports an error.
///
/// @param spec Service to remove; only `serviceName` is used.
/// @param scope Which supervisor domain to remove from. Ignored on Windows.
/// @return ServiceControlResult with exit code 0 and a success message, or a
///         non-zero code and a diagnostic (e.g. needs elevation, no such service).
[[nodiscard]] ServiceControlResult UninstallService(ServiceSpec const& spec, ServiceScope scope = ServiceScope::System);

} // namespace FastCache
