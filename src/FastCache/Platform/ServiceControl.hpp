// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
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

/// Which identity the Windows SCM logs a service on as.
///
/// The Windows counterpart of `ServiceSpec::serviceAccount`, and separate from it
/// because the two supervisors take different *kinds* of answer: launchd wants an
/// account name that must already exist, while the SCM derives a per-service
/// identity from the service's own name and needs no account created at all. One
/// string field could not mean both, and a spec has to answer both supervisors.
enum class WindowsLogonAccount : std::uint8_t
{
    /// `lpServiceStartName = nullptr`: the SCM default, **LocalSystem** -- the
    /// most privileged identity on the machine. Correct only for a service that
    /// genuinely needs it.
    LocalSystem,

    /// `NT SERVICE\<serviceName>`: a virtual account the SCM creates on demand for
    /// a `SERVICE_WIN32_OWN_PROCESS` service. It has a per-service SID, no
    /// password, no group membership and no machine credentials on the network --
    /// the Windows answer to "this process should not have the whole machine".
    VirtualAccount,
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
/// value back out, so the emission cannot be written once generically -- each
/// binary walks its own table and hands the result over. That is why
/// `BuildServiceArgv` stays hand-written per binary and is guarded by a test that
/// walks the option table and requires every non-excluded flag to be emitted.
///
/// One installer-supplied default `WithScopeDefaults` may fill in.
///
/// **An enumerator each, because the two are independent questions and one bit
/// could not say so.** `ServiceSpec::applicationName` used to decide both at once:
/// a service that named an application was handed a `--config` *and* a
/// `--storage`, and one that named none got neither. That was exactly right while
/// `fastcached` was the only file-configured service, and it left
/// `fastcache-compile-node` -- which reads a configuration file and has no
/// `--storage` flag at all -- with no way to say so
/// ([#396](https://github.com/LASTRADA-Software/fastcached/issues/396)). Its spec
/// had to name no application, which is the SAFE direction (a registration
/// carrying `--storage=` produces a job that answers its own command line with
/// "unrecognised argument" at every start, reported installed and dead at every
/// boot) and costs it both the system-scope `--config` default and the
/// install-time `ServiceAccountReadDenial` on that path.
enum class ScopeDefault : std::uint8_t
{
    /// `--config=<the packaged machine-wide file>`. System scope only: that file
    /// describes the machine-wide service, so handing it to a per-user agent
    /// points the agent at a directory it cannot write.
    ConfigPath,

    /// `--storage=<the per-user cache directory>`. User scope only: a system
    /// job's cache lives under the package prefix, named by its config file, so
    /// there is nothing to default.
    StoragePath,

    Last
};

/// Which installer-supplied defaults a service accepts, indexed by the default.
///
/// A table indexed by the enumerator rather than one `bool` per default, so a
/// third default is a new enumerator plus a new row -- and every guard that walks
/// the table covers it without anybody remembering to widen a struct.
using ScopeDefaultSet = EnumTable<ScopeDefault, bool>;

/// The set naming exactly @p accepted.
///
/// Spelled as a call at each spec (`ScopeDefaults({ ScopeDefault::ConfigPath })`)
/// rather than as an aggregate of bools, because a positional `{ true, false }`
/// says which default it means only by counting.
/// @param accepted The defaults this service's parser will take.
/// @return The set; every default not named is refused.
[[nodiscard]] constexpr ScopeDefaultSet ScopeDefaults(std::initializer_list<ScopeDefault> accepted) noexcept
{
    ScopeDefaultSet set {};
    for (auto const which: accepted)
        set[static_cast<std::size_t>(which)] = true;
    return set;
}

/// Whether @p set names @p which.
/// @param set The service's accepted set.
/// @param which The default in question.
/// @return True when this service's parser takes that flag.
[[nodiscard]] constexpr bool AcceptsScopeDefault(ScopeDefaultSet const& set, ScopeDefault which) noexcept
{
    return set[static_cast<std::size_t>(which)];
}

/// What one installer-supplied default is called, and where it applies.
struct ScopeDefaultRow
{
    /// The default this row describes. Its position in the table, guarded by
    /// `RowsInEnumeratorOrder`.
    ScopeDefault which {};

    /// The flag prefix, `=` included, e.g. `--config=`. Both tested against the
    /// arguments already present and emitted, so the spelling exists once.
    std::string_view flag {};

    /// The one scope this default applies in.
    ServiceScope scope {};
};

/// Every installer-supplied default, one row per enumerator, in enumerator order.
///
/// Published so a guard can walk it. That is the point rather than a convenience:
/// a test naming the two flags by hand passes under the very defect #396 was --
/// two defaults decided together still yield both flags for the spec that accepts
/// both -- while a walk that requires a spec accepting ONE row to receive that
/// row's flag and **no other row's** does not, and covers a third default with no
/// edit.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<ScopeDefaultRow const> ScopeDefaultTable() noexcept;

/// The flag prefix @p which fills in, `=` included.
/// @param which The default in question.
/// @return Its flag prefix, e.g. `--storage=`.
[[nodiscard]] std::string_view ScopeDefaultFlag(ScopeDefault which) noexcept;

/// **That obstacle is a missing column, not an impossibility**, and the honest
/// version of this paragraph says so: `OptionSpec::same` already carries a
/// member-pointer column that reads a field back out (`FieldEq<&Config::x>()`),
/// so a symmetric renderer would make the emission table pure data and delete
/// thirty hand-copied lines per binary. It is not written yet, and the reason is
/// scope rather than principle --
/// [#349](https://github.com/LASTRADA-Software/fastcached/issues/349) moved the
/// emission rule and left the shape alone. What keeps the hand-written table
/// honest meanwhile is the guard, which drives ONE row at a time and asserts the
/// emitted token MOVES when that row's own applier runs -- so a line naming
/// another row's field, or another row's bit, fails rather than being covered by
/// a neighbour's emission. Presence alone would not catch either: a wrong field
/// still emits a token under the right spelling.
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

    /// Paths `serviceAccount` must own before the job first runs.
    ///
    /// Only ever paths the operator actually named. Handing over a **parent**
    /// gives away a directory nobody asked about: `--storage=/var/db/fc` would
    /// reassign `/var/db`, shared with other system services, to an unprivileged
    /// cache account -- silently, under a message saying the service had been
    /// installed. The daemon drops to `serviceAccount`, so a directory root
    /// created for it has to change hands or the first write fails with EACCES,
    /// which launchd surfaces only as a job that exits over and over.
    ///
    /// Usually a directory, which the handover creates when it is absent -- but
    /// never a path that `Core/PathKind`'s `PathNamesAFile` says is a file, and
    /// never over something already there. `storage_path` may name one CoW file,
    /// and a `create_directories` over `cache.cow` would put a directory where the
    /// daemon wants a file. Hence `ownedPaths` rather than `ownedDirectories`:
    /// `chown` and a DACL apply to a file just as well, so only the *create* has
    /// to care, and it is refused in the handover rather than in each producer.
    std::vector<std::filesystem::path> ownedPaths;

    /// Whether a secret was typed on the installing command line.
    InlineCredential inlineCredential { InlineCredential::Absent };

    /// Which installer-supplied defaults this service's parser will accept.
    ///
    /// The other half of the split described on `ScopeDefault`, and it answers a
    /// question about the **parser**: which flags this binary would understand if
    /// the installer filled one in. `applicationName` below answers the other, a
    /// question about the **value**. Empty -- the default -- means every default is
    /// refused, which is what a service configured entirely from argv wants and
    /// what `fastcache-compile-node` had to get by naming no application at all.
    ///
    /// Declared here, immediately after the other byte-wide member, rather than
    /// beside `applicationName` where it reads better: every `bool` and byte-wide
    /// enum in a struct belongs in one run, and this run had seven bytes of padding
    /// after it, so the field costs nothing where it is and eight bytes where it
    /// reads best. The two are cross-referenced instead.
    ScopeDefaultSet acceptedScopeDefaults {};

    /// The `--config` path the operator named, or empty.
    ///
    /// Used only to make the credential refusal say where the secret should go
    /// instead. It is not a hint that the combination is safe -- see
    /// `InlineCredentialRejection`.
    std::string configPath;

    /// The application name this service's *files* are looked up under -- its
    /// machine-wide configuration and its per-user cache -- or **empty** when the
    /// service keeps neither.
    ///
    /// Empty is a real answer, not a missing one. It is also **only** this question
    /// since #396: which defaults the installer may fill in is
    /// `acceptedScopeDefaults` above, and the two are independent.
    ///
    /// **What this still decides, and why one bit looked sufficient for as long as
    /// it did.** Both defaults are *values looked up under this name* -- where the
    /// packaged machine-wide config lives, and where a per-user cache goes -- so a
    /// service naming none has nothing to derive either default from, whatever its
    /// parser accepts. That is a property of the VALUE, not of the parser, and it
    /// is exactly the overlap that made a single bit read as adequate: for the one
    /// service that existed, "has files" and "takes both flags" happened to
    /// coincide. They are not the same fact, and the day a second service arrived
    /// the bit could only answer one of them.
    ///
    /// So the name still gates both defaults -- `ScopeDefaultApplies` asks it
    /// first -- and it no longer says anything about which flags a parser takes.
    /// `fastcache-compile-node` names one and accepts only `ConfigPath`.
    std::string applicationName;

    /// Which identity the Windows SCM should log this service on as.
    ///
    /// Defaults to `LocalSystem` because that is what the SCM does when told
    /// nothing, so the default states the platform's behaviour rather than this
    /// project's preference. **Both** of this project's services override it:
    /// neither has any use for unrestricted access to the machine.
    ///
    /// `serviceAccount` does not answer this: it holds a POSIX account name that
    /// must already exist, which is not a thing the SCM takes.
    WindowsLogonAccount windowsLogon { WindowsLogonAccount::LocalSystem };
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
/// The daemon's half of the seam: it owns `CliResult`, so it is what turns one
/// into a `ServiceSpec`. `fastcache-compile-node` has its own equivalent over its
/// own configuration type.
///
/// Takes the **parse**, not the configuration, for the reason `BuildServiceArgv`
/// does: which flags a registration carries is decided by what the operator
/// typed, and only a `CliResult` knows that.
///
/// @param exePath Absolute path to the fastcached executable.
/// @param cli The command-line parse to embed in the launch arguments.
/// @return The spec a supervisor is registered from.
[[nodiscard]] ServiceSpec MakeDaemonServiceSpec(std::filesystem::path const& exePath, CliResult const& cli);

/// Build the argument vector a service supervisor launches fastcached with.
///
/// Element 0 is @p exePath; the rest are `--flag=value` tokens, one for every
/// flag @p cli records the operator as having **named**, plus
/// `--service-name=<name>` and — depending on @p daemonFlag — `--daemon`.
/// Path-bearing flags (`--storage`, `--config`) are made absolute because a
/// service does not inherit the installing shell's working directory, so a
/// relative path captured at install time would resolve elsewhere at start.
///
/// **Provenance, never value.** This took a `Config` and emitted a flag whose
/// value differed from a default-constructed one, which drops any pin that
/// happens to equal the default — and a default derived from the HOST is not a
/// constant. `--max-memory` defaults to a quarter of RAM, so on a 32 GiB machine
/// `--install-service --max-memory=8g` registered no `--max-memory` at all, and
/// the service re-derived its budget from RAM at every start: add memory and the
/// pinned budget silently moved, for precisely the operator who bothered to pin
/// it ([#349](https://github.com/LASTRADA-Software/fastcached/issues/349)). Every
/// other row was safe only because its default is a compile-time constant, which
/// `logTimestamps` also was until #496 made it platform-dependent — so the rule
/// is applied to the whole table rather than to the row that was caught.
///
/// A `CliResult` and not a `Config` because that is where the provenance lives,
/// and the signature is the guard: the rule that a registration replays the
/// **command line** and never the merged file cannot be obeyed by a function
/// that was only ever handed the merge's output.
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
/// @param cli The command-line parse to embed in the launch arguments.
/// @param daemonFlag Whether to emit `--daemon`.
/// @return Argument vector, `exePath` first.
[[nodiscard]] std::vector<std::string> BuildServiceArgv(std::filesystem::path const& exePath,
                                                        CliResult const& cli,
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

/// The account name to hand `CreateService` as `lpServiceStartName`.
///
/// Pure and platform-independent so it is asserted everywhere rather than only on
/// a Windows runner -- the same reason BuildLaunchdPlist is. The alternative was a
/// derivation visible only inside `#if defined(_WIN32)`, and this file has already
/// paid for that once.
///
/// @param spec Service to name; `serviceName` is what a virtual account derives
///        from, so the two cannot disagree.
/// @return `NT SERVICE\<serviceName>` for a virtual account; `nullopt` for
///         LocalSystem, which is spelled by passing no name at all.
[[nodiscard]] std::optional<std::string> WindowsLogonName(ServiceSpec const& spec);

/// Where a launchd job writes its stdout/stderr.
///
/// System scope gets a directory of its **own**, named by the job's label,
/// because `InstallService` hands that directory to the account the job runs as
/// -- and a machine may run more than one of this project's services system-wide.
/// While it was one shared directory each install chowned the other's to its own
/// account, so registering the compile worker reassigned the daemon's and a
/// package reinstall reassigned it back.
///
/// User scope keeps the flat directory: nothing is chowned there and the files
/// inside are already named by the label, so two agents cannot collide. Giving it
/// a subdirectory would move a path an operator already has open in `tail -f` and
/// buy nothing.
///
/// Declared here rather than kept private to the launchd implementation for the
/// reason WithScopeDefaults below is: it decides a path that ends up in a plist
/// and in a chown, so it has to be assertable on every platform.
///
/// @param label The job's launchd label, from LaunchdLabel.
/// @param scope Which domain the job belongs to.
/// @param home The invoking user's home directory; used only for User scope.
/// @return Directory the job's `.out.log` and `.err.log` live in.
[[nodiscard]] std::filesystem::path DefaultLogDirectory(std::string_view label,
                                                        ServiceScope scope,
                                                        std::filesystem::path const& home);

/// Which spelling of a switch a registration must carry, if any.
///
/// **Extracted so the decision is assertable on every platform**, which is the only
/// way to see the defect it was written for. It used to compare @p value against the
/// platform default and emit the POSITIVE flag when they differed, which says "on" --
/// correct while the default is false everywhere, and inverted once it is not. With
/// `logTimestamps` defaulting true under macOS (#496), an operator's explicit
/// `--no-log-timestamps` differed from that default and was registered as
/// `--log-timestamps`: the thing they turned off, turned back on, at every boot and
/// silently, because a registration replays its command line forever.
///
/// It now asks **provenance** instead (#349), which is a stronger answer to the same
/// question: an operator who names a switch gets the spelling that produces the value
/// they asked for, whatever the platform would otherwise have done. A switch nobody
/// named is not registered, so the next start re-derives it exactly as this one did.
///
/// This is the one place both kinds of switch are decided. The old comment here said
/// not to unify them, and it was right while the rule was a value comparison: the
/// two-sided switch needed a platform default that the one-sided switches did not.
/// Provenance consults no default at all, so the only thing left separating them is
/// *whether a negative spelling exists* -- an argument, not a second rule.
///
/// @param onFlag Flag name, without dashes, that sets the value true.
/// @param offFlag Flag name, without dashes, that sets it false; empty when the
///        switch has no negative spelling.
/// @param value What the operator asked for.
/// @param wasTyped Whether the operator named this switch at all.
/// @return The flag name to emit, or nullopt when nothing was named -- or when
///         @p value is false and there is no spelling that says so.
[[nodiscard]] std::optional<std::string_view> SwitchSpellingFor(std::string_view onFlag,
                                                                std::string_view offFlag,
                                                                bool value,
                                                                bool wasTyped) noexcept;

/// Fill in the per-scope path arguments the operator left unset.
///
/// Pure, and declared here rather than kept private to the launchd
/// implementation, for the reason BuildLaunchdPlist is: it decides what goes into
/// a registration, so it has to be assertable on every platform. While it was
/// private to the `__APPLE__` block no test could reach it, and it spent that
/// time handing every service the *daemon's* defaults -- including one whose
/// parser rejects them.
///
/// Decides nothing about the environment: `home` and `packagedConfig` are probed
/// by the composition root and passed in, so the policy is testable against
/// values that need not exist.
///
/// @param spec Service as described from the command line.
/// @param scope Domain being installed into.
/// @param home The invoking user's home directory.
/// @param packagedConfig The machine-wide config file, empty when it is absent,
///        unreadable or untrusted.
/// @return @p spec with each default filled in where `ScopeDefaultApplies` says
///         it may be -- per default, never both from one answer.
[[nodiscard]] ServiceSpec WithScopeDefaults(ServiceSpec spec,
                                            ServiceScope scope,
                                            std::filesystem::path const& home,
                                            std::filesystem::path const& packagedConfig);

/// Whether @p which may be filled in for @p spec at @p scope.
///
/// The clauses that are the same for **every** default, asked once here rather
/// than re-spelled per block: this service has files to derive a value from at
/// all, its parser accepts this particular flag, this is the scope the default
/// applies in, and the operator did not name the flag themselves.
///
/// Exposed so the guard can drive it directly. A per-default question that only
/// existed inside `WithScopeDefaults` could be asserted only through the whole
/// registration, which is how "two decided from one bit" survived: the spec that
/// accepted both was the only one anybody drove.
///
/// What is deliberately NOT here is each default's own extra condition, because
/// they are not the same condition and folding them in would need a column that
/// means "and whatever else that one wants". `StoragePath` additionally requires
/// that no `--config` was named (whoever passes a config file owns the storage
/// path in it); `ConfigPath` additionally requires that a packaged config was
/// actually found. Both stay at their block, with their reasons.
///
/// @param spec Service as described from the command line.
/// @param scope Domain being installed into.
/// @param which The default in question.
/// @return True when the shared clauses all hold.
[[nodiscard]] bool ScopeDefaultApplies(ServiceSpec const& spec, ServiceScope scope, ScopeDefault which);

// ---------------------------------------------------------------------------
// What a bounded `launchctl` call cost (#535)
//
// The bound itself was never in doubt; what it REPORTED about itself was. It
// rendered the configured ceiling rather than the measured elapsed, carried no
// evidence about the process it killed, and the caller then told an operator the
// job "will start at the next login or boot" -- advice that is true when the host
// was merely busy and false when the binary wedges, with nothing able to tell
// those apart.
//
// The readings and the verdict live HERE, outside the `__APPLE__` branch that
// acquires them, and that placement is the point rather than tidiness.
//
// The reason is the absence of a SEAM, not the absence of a machine -- an earlier
// draft of this comment said "testable on no machine this project builds on" and
// that is false: CI builds and runs `ctest` on macOS. What there is no way to
// reach is a decision buried in a file-local function that spawns a real process
// and waits on it. Lifting the decision out over a record gives it one, and it is
// the same shape `_e2e_verdict` takes in `scripts/lib/e2e-common.sh`, for the
// same reason.
// ---------------------------------------------------------------------------

/// How a bounded `launchctl` call ended.
enum class LaunchctlOutcome : std::uint8_t
{
    Exited,     ///< It returned a status of its own, zero or not.
    NotStarted, ///< It could not be spawned, or could not be waited for.
    Signalled,  ///< It ran and was killed by a signal rather than exiting.
    TimedOut,   ///< The deadline passed and WE killed it.
};

/// What one bounded `launchctl` call actually cost.
///
/// `elapsed` is MEASURED. The message this feeds used to interpolate the
/// configured ceiling, so it read `60s` whatever the call really took -- and a
/// polled wait overshoots its deadline by up to one poll interval, on a loaded
/// host by more. A number that cannot be wrong is not a measurement.
struct LaunchctlReadings
{
    /// How it ended.
    LaunchctlOutcome outcome { LaunchctlOutcome::Exited };

    /// The call's own exit status. Meaningful only for `Exited`.
    int exitStatus { 0 };

    /// The signal that killed it. Meaningful only for `Signalled`.
    ///
    /// Its own field rather than folded into `exitStatus` as `128 + signal`:
    /// that encoding is a shell convention, and a struct that has room to say
    /// which fact it holds should say it. `NotStarted` used to cover this case,
    /// so a `launchctl` that started and crashed was reported as one that
    /// "could not be started" -- a diagnostic naming the wrong cause, which is
    /// the class of defect this whole change is about.
    int terminatingSignal { 0 };

    /// Measured wall time, never the configured ceiling.
    std::chrono::milliseconds elapsed { 0 };

    /// The ceiling it was allowed, so a message can report both.
    std::chrono::milliseconds budget { 0 };

    /// CPU the killed child consumed, or `nullopt` when nothing could read it.
    ///
    /// A disengaged optional is "no reading", which is NOT the same fact as a
    /// reading of zero -- zero says the process never ran, and that is the one
    /// calibrated point on this axis. Collapsing them would let a platform that
    /// cannot sample CPU report every timeout as "it never ran".
    std::optional<std::chrono::microseconds> cpu {};
};

/// What a timed-out `launchctl` call's readings do and do not establish.
///
/// **Calibrated on measurements, and the first draft of this enum was wrong.** It
/// read zero cpu as "it never ran", on the reasoning that zero is the one point
/// that does not vary. Measured by driving the acquisition loop over a parked
/// process and a spinning one: a `sleep` that does nothing at all still consumes
/// **665us** -- exec, dynamic linking and entering the sleep are not free -- so
/// zero essentially never occurs and the interesting difference is a RATIO. The
/// same run put a busy loop at **846448us** against 1012ms of ELAPSED: 0.07% duty
/// against 83.6%, three orders apart. Elapsed rather than budget throughout, in a
/// header whose whole subject is that those two differ.
enum class LaunchctlFinding : std::uint8_t
{
    /// Not a timeout at all; there is nothing here to diagnose.
    NotATimeout,

    /// It was burning cpu when the budget ran out. Whatever it was doing, it will
    /// do again -- this is the one reading that rules a slow host OUT.
    BurningCpu,

    /// It consumed barely any cpu: it was waiting on something. That covers a
    /// loaded host AND a lock it will never get, which these readings cannot
    /// separate -- so this names what was seen rather than what caused it.
    Waiting,

    /// The duty cycle fell between the two, or no reading could be taken. Said
    /// out loud rather than resolved towards whichever is convenient.
    Inconclusive,
};

/// Whether a launchctl call did what was asked.
///
/// One spelling, because "exited zero" is the only success and four call sites
/// comparing fields for themselves is four places for the answer to drift apart.
/// No site ever got this wrong -- the previous code compared an int against 0 and
/// every sentinel was negative -- so this preserves a correct answer rather than
/// repairing a broken one.
/// @param readings What the call cost.
/// @return True when it exited with status zero.
[[nodiscard]] bool LaunchctlSucceeded(LaunchctlReadings const& readings) noexcept;

/// How to name a launchctl call's failure in a sentence.
///
/// "failed" only where something actually refused. A wait that ran out refused
/// nothing, and calling it a failure sends an operator looking for a rejection
/// that never happened (#535). BOTH failing call sites use this rather than the
/// kickstart one alone: they are byte-identical defects, and the one that was not
/// observed is the one that survives a fix aimed at the one that was.
/// @param readings What the call cost.
/// @return "timed out" or "failed".
[[nodiscard]] std::string_view LaunchctlFailureVerb(LaunchctlReadings const& readings) noexcept;

/// Read a launchctl timeout's readings as a finding.
///
/// Two bands with a deliberate gap, because the gap is the honest part. The
/// anchors are measured (0.07% parked, 83.6% spinning) and everything between is
/// `Inconclusive` rather than assigned to the nearer edge -- which is the rule
/// `.agent/rules/testing.md` states for the scratch-isolation classifier, where
/// "no magnitude bar calibrates" and the band between idle and clearly-working is
/// reported as neither.
/// @param readings What the call cost.
/// @return The finding.
[[nodiscard]] LaunchctlFinding LaunchctlFindingOf(LaunchctlReadings const& readings) noexcept;

/// Render a launchctl outcome for an operator, naming the KIND of failure and
/// what its readings do and do not establish.
///
/// It deliberately makes no claim about what happens next. The sentence this
/// replaces ended "it will start at the next login or boot", emitted for every
/// failure -- a prediction true only when the host was busy, and these readings
/// cannot establish that case (#535).
/// @param readings What the call cost.
/// @return A phrase that reads correctly after "kickstart timed out (".
[[nodiscard]] std::string LaunchctlStatusText(LaunchctlReadings const& readings);

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
