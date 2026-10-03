// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/StateFiles.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace FastCache
{

/// Is this process running with the rights the machine-wide daemon has?
///
/// The question that decides whether a machine-wide configuration is *this*
/// process's business at all. A file at `/etc/fastcached` or
/// `%ProgramData%\fastcached` describes the system service — its cache lives
/// where only the service account can write — so a per-user instance that
/// adopted it would be configured for a daemon it is not: pointed at a state
/// directory it cannot open, or at a port the real daemon already holds.
///
/// On Windows this asks whether `BUILTIN\Administrators` is *enabled* in the
/// effective token, which is false for the unelevated half of an
/// administrator's split token — correctly, since that process could not have
/// written the machine-wide config either. LocalSystem, and therefore the
/// service, answers true.
///
/// @return true when the caller is root (POSIX) or an elevated administrator
///         or LocalSystem (Windows). False when it cannot be determined, which
///         keeps an undecidable case out of the machine-wide config.
[[nodiscard]] bool IsPrivilegedProcess();

/// Can only an administrator have put the file that is at @p path there?
///
/// The question a machine-wide configuration has to answer before it is
/// obeyed. A daemon running as LocalSystem (Windows) or root (POSIX) takes its
/// `storage_path`, `bind` and `requirepass` from that file, so a file an
/// unprivileged account could have written is an unprivileged account telling a
/// privileged process what to do.
///
/// **The test is the containing directory, not the owner.** Ownership is the
/// obvious rule and it is the wrong one: on Windows the default owner of a new
/// object is its *creator*, so a config seeded by hand from an elevated shell is
/// owned by that administrator's own account rather than by
/// `BUILTIN\Administrators`, and an owner whitelist would reject it. Scanning
/// the file's ACL for broadly-granted write is wrong for the opposite reason: a
/// file planted by user `bob` grants full control to *bob's own* SID through the
/// inherited `CREATOR OWNER` entry, which is not a broad principal at all. What
/// actually settles it is who could have created the entry: if nothing outside
/// the administrative accounts may add or replace a file in the directory, then
/// whatever is in it was put there by an administrator.
///
/// Both the file and its **immediate** parent are checked, and deliberately no
/// further up. A file nobody may write is still replaceable when its directory
/// is, which is precisely the planted-config case; but walking the whole chain
/// would reach `C:\ProgramData` itself, which grants every local user
/// create-file by design and would therefore condemn every path beneath it.
/// Replacing an intermediate directory needs `DELETE` on it, which that same
/// design does not grant.
///
/// @param path An existing file.
/// @return true when no non-administrative principal can create or replace a
///         file at @p path. False when that cannot be determined at all — an
///         unreadable security descriptor, a filesystem with no ACLs — because
///         "I cannot tell" and "it is not safe" have to lead to the same place.
[[nodiscard]] bool IsAdministratorOnlyWritable(std::filesystem::path const& path);

/// Why a file holding a secret is not fit to hold one, or nothing.
///
/// A **reason** rather than a `bool`, because the caller has to say WHICH
/// exposure it found: "group- and world-readable" and "world-readable" have
/// different remedies (`chmod g-r` against `chmod o-r`), and an operator handed
/// neither goes looking at the wrong thing. Empty means the file is fit.
enum class SecretExposure : std::uint8_t
{
    /// Nothing else on this machine can read it.
    None,

    /// Any account on the machine can read it. `Everyone`, `Authenticated
    /// Users`, `BUILTIN\Users` and their kin on Windows; the `other` bits on
    /// POSIX.
    AnyLocalAccount,

    /// A group can read it, and the file is not an administrator's to delegate
    /// with -- so the group grant is the file owner's own, over accounts they do
    /// not answer for.
    ///
    /// POSIX only, and the distinction is load-bearing: `0640 root:_fastcached`
    /// is how this project TELLS operators to hold a secret, and the macOS
    /// package ships exactly that, so a rule that condemned every group-readable
    /// file would condemn the documented arrangement. See
    /// `SecretFileExposure`.
    OwnersOwnGroup,

    /// The question could not be answered -- an unreadable security descriptor,
    /// a filesystem with no permissions to inspect, a `stat` that failed.
    ///
    /// Its own answer rather than folded into `None`, because "nothing else can
    /// read it" and "I could not tell" are different claims and a caller that
    /// cannot tell them apart reports the safe one. Reported, never refused on:
    /// a filesystem with no modes to read is an ordinary deployment, not an
    /// exposure.
    Undetermined,

    Last
};

/// What a platform observed about who may read a file.
///
/// **Acquisition is per platform; the DECISION over it is not.** Reading POSIX
/// mode bits and walking a Windows access list have nothing in common and neither
/// can be executed on the other's host -- so the rule that turns either
/// observation into a verdict is a pure function over this record, and the branch
/// a developer cannot run is still exercised against a constructed input. That is
/// the most a single-platform host can honestly give: "tested against a
/// synthesised record" rather than "untested".
struct SecretFileFacts
{
    /// Whether the platform answered at all. False for a failed `stat`, an
    /// unreadable security descriptor, a filesystem with no permissions to
    /// inspect -- and it is its own field rather than a sentinel in the others,
    /// because "nobody else may read it" and "I could not tell" are different
    /// claims.
    bool determined { false };

    /// Any account on the machine may read it: an `other` read bit on POSIX, a
    /// broad principal granted read in a Windows access list.
    bool readableByAnyAccount { false };

    /// A group may read it.
    ///
    /// **POSIX only, and Windows leaves it false deliberately.** A DACL does not
    /// separate "a group" from "everyone" in the way the delegation rule below
    /// needs: `BUILTIN\Users` IS the broad principal, and a narrow group grant is
    /// spelled with a SID this code has no policy for. Inventing the distinction
    /// there would be a claim no access list supports.
    bool readableByGroup { false };

    /// The file is owned by the platform's administrative identity -- uid 0 on
    /// POSIX. Decides whether a group grant is a delegation or an exposure.
    bool administrativelyOwned { false };
};

/// Turn what a platform observed into a verdict.
///
/// The rule, in one place, over a record either platform can produce. See
/// `SecretFileExposure` for what the rule IS and why the group clause is
/// conditional.
///
/// @param facts What the platform reported.
/// @return Why the file is unfit to hold a secret, or `None`.
[[nodiscard]] constexpr SecretExposure ClassifySecretFile(SecretFileFacts const& facts) noexcept
{
    if (!facts.determined)
        return SecretExposure::Undetermined;

    // World before group, because the two are not alternatives: a 0644 file is
    // both, and the world grant is the one worth naming -- `chmod o-r` is its
    // remedy, and reporting the group grant instead sends an operator to tighten
    // something that was not the exposure.
    if (facts.readableByAnyAccount)
        return SecretExposure::AnyLocalAccount;

    // A group grant is an exposure only when the owner is not administrative.
    // Owned by root it is an ADMINISTRATOR delegating read to a service account,
    // which is what `InlineCredentialRejection` instructs in so many words and
    // what the macOS package ships as `0640 root:_fastcached`; owned by a user it
    // is that user's own group, over accounts they do not answer for.
    if (facts.readableByGroup && !facts.administrativelyOwned)
        return SecretExposure::OwnersOwnGroup;

    return SecretExposure::None;
}

/// Can anything other than this file's owner read it?
///
/// **The readability half of file trust, and it is a different question from
/// `IsAdministratorOnlyWritable` above.** That one answers *integrity* -- could an
/// unprivileged account have written what a privileged process is about to obey.
/// This one answers *secrecy*: `--requirepass` may come from a configuration
/// file, and the whole reason to put it there is that a command line is visible in
/// `ps`, so an operator who moves it into a mode-0644 file has undone the point of
/// the exercise and had no signal at all
/// ([#384](https://github.com/LASTRADA-Software/fastcached/issues/384)).
///
/// **The POSIX rule is "not world-readable, and group-readable only when owned by
/// root", and the second clause is what stops this becoming an alarm nobody
/// reads.** `InlineCredentialRejection` tells operators in so many words to put
/// the secret in a file of "mode 0640, readable by the account the service runs
/// as", and the macOS package ships `0640 root:_fastcached` for exactly that
/// reason. A rule refusing every group-readable file would refuse the documented
/// arrangement, which is the failure #384's own acceptance criteria are built
/// around. Owned by root, a group grant is an administrator delegating read to a
/// service account; owned by a user, it is that user's own group -- accounts they
/// do not answer for. This is PostgreSQL's rule for its server key, arrived at for
/// the same reason and cited rather than reinvented.
///
/// **What it therefore does NOT catch, stated rather than implied:** `0640
/// root:staff` passes, because nothing here can know that `staff` is broad while
/// `_fastcached` is not. Group membership is a policy question about a particular
/// machine; a predicate that guessed would be wrong in whichever direction the
/// machine disagreed with.
///
/// **On Windows this fires on the PACKAGED machine-wide config today, and that is
/// a true positive rather than the check being wrong.**
/// `packaging/windows/service-actions.xml` locks `%ProgramData%\fastcached` with an
/// access list granting `BUILTIN\Users` `FILE_GENERIC_READ`, inherited by files --
/// deliberately, because the service's virtual account has to read its own
/// configuration. So the file `InlineCredentialRejection` tells operators to put
/// `requirepass:` in is readable by every local account, which means the advice
/// moves the secret from one world-readable place to another. Two correct-looking
/// decisions in conflict, tracked as
/// [#741](https://github.com/LASTRADA-Software/fastcached/issues/741) against
/// packaging (seed the live file with its own tighter list rather than letting it
/// inherit). There is deliberately **no suppression for that path here**: a check
/// that goes silent about a real exposure because the fix belongs to someone else
/// is a wrong signal removed without a right one added, and it would blind this on
/// the platform where the exposure is worst.
///
/// @param path An existing file.
/// @return Why it is unfit to hold a secret, `None` when it is fit, or
///         `Undetermined` when the platform would not say.
[[nodiscard]] SecretExposure SecretFileExposure(std::filesystem::path const& path);

/// What this platform reports about who may read @p path.
///
/// The acquisition half, published so the seam is visible rather than implied:
/// `SecretFileExposure` is this composed with `ClassifySecretFile`, and only this
/// half touches the filesystem.
/// @param path An existing file.
/// @return The observation; `determined` false when the platform would not say.
[[nodiscard]] SecretFileFacts ObserveSecretFile(std::filesystem::path const& path);

/// What to tell an operator about @p exposure, and how to fix it.
///
/// Beside the predicate so the advice cannot drift from the rule that produced
/// it, and spelled for the platform this build targets -- `chmod` says nothing
/// useful on Windows.
///
/// @param path The file the exposure was found on.
/// @param exposure What was found; `None` yields an empty string.
/// @return A sentence naming the exposure and the remedy, or empty.
[[nodiscard]] std::string SecretExposureHint(std::filesystem::path const& path, SecretExposure exposure);

/// What to tell an operator about @p exposure on a secret ONE account holds, and how to
/// restrict it to that account.
///
/// `SecretExposureHint`'s remedy grants every service on the machine read on Windows, which
/// is right for a configuration a service reads and wrong for a node's identity key -- and
/// worse than wrong for a node run by a user, whom that list does not name, so following it
/// would leave the file unreadable to the one process that needs it. This remedy sets the
/// list `SecureSecretFileForOwner` sets: SYSTEM, Administrators and OWNER RIGHTS, with every
/// principal `SecretFileExposure` scans for removed and nothing inherited. On POSIX it takes
/// every group and other bit away.
///
/// @param path The file the exposure was found on.
/// @param exposure What was found; `None` yields an empty string, and `Undetermined` the
///        sentence `SecretExposureHint` gives, since there is nothing to restrict.
/// @return A sentence naming the exposure and the remedy, or empty.
[[nodiscard]] std::string OwnerOnlySecretExposureHint(std::filesystem::path const& path, SecretExposure exposure);

/// Make @p directory administrator-only writable.
///
/// The companion to IsAdministratorOnlyWritable, for the one place that creates
/// a machine-wide config directory outside the installer: `--seed-config` run
/// by hand. A directory created there inherits its parent's permissions, and on
/// Windows that parent is `%ProgramData%`, which lets every standard account
/// create files in the new subdirectory. Without this, seeding by hand would
/// produce a configuration the daemon then refuses — a tool defeating itself.
///
/// Does not create the directory: the caller has already done that, and both
/// halves need it to exist.
///
/// @param directory An existing directory.
/// @return true when the directory is administrator-only writable afterwards.
[[nodiscard]] bool SecureDirectoryForAdministrators(std::filesystem::path const& directory);

/// Make @p file readable by nobody but the administrative accounts and the
/// machine's own services.
///
/// The secrecy counterpart of `SecureDirectoryForAdministrators`, and it exists
/// because the directory's list cannot answer this question. A machine-wide
/// config directory has to grant read broadly -- the daemon runs as a virtual
/// account, which is an ordinary `BUILTIN\Users` member -- and that grant is
/// inherited by everything created inside it, including the one file
/// `InlineCredentialRejection` tells operators to put `requirepass:` in
/// ([#741](https://github.com/LASTRADA-Software/fastcached/issues/741)). So the
/// FILE carries a list of its own, protected against that inheritance, rather
/// than the directory being narrowed for every reader at once.
///
/// **The Windows grant is `NT AUTHORITY\SERVICE` (S-1-5-6) and deliberately not
/// `NT SERVICE\<service>`.** The per-service trustee resolves only once the
/// service exists (see `Platform/ServiceControl`'s `GrantPathAccess`), and the
/// MSI seeds the config BEFORE it registers anything -- so naming it would grant
/// nothing at all on the one path this exists for. `SERVICE` is every principal
/// logged on as a service, which needs an administrator to arrange and excludes
/// every interactive account; it also survives `--service-name`, which the
/// per-service SID would not. It is deliberately not one of the broad principals
/// `SecretFileExposure` scans for, so a file secured here reports `None` rather
/// than the check being taught an exception.
///
/// On POSIX it removes every group and other bit, leaving the owner's. A package
/// that wants to delegate read to a service account chowns and chmods afterwards
/// -- which is what the macOS postinstall does to reach `0640 root:_fastcached`,
/// and it stays that package's decision rather than this function guessing a
/// group name.
///
/// **State the consequence rather than leave it to be discovered:** a
/// `sudo fastcached --seed-config=...` run by hand on Linux therefore leaves
/// `0600 root:root`, which the packaged unit's `User=fastcached` cannot read --
/// and an unreadable machine-wide candidate is skipped in SILENCE by
/// `ResolveDefaultConfigPath`, so the daemon would start on built-in defaults
/// with nothing said. Nothing in the shipped Linux packaging reaches this (the
/// config is a dpkg conffile / rpm `%config`, written by the package manager and
/// never by this call), so the exposure is to hand-seeding only; whoever does it
/// owes the `chown root:<service group>` and `chmod 0640` the documentation asks
/// for.
///
/// Does not create the file: the caller has just written it, or found it.
///
/// @param file An existing file.
/// @return true when nothing outside those accounts can read it afterwards --
///         the property, asked back through `SecretFileExposure`, rather than
///         whether the call that set it returned success.
[[nodiscard]] bool SecureSecretFileForServices(std::filesystem::path const& file);

/// Make @p file readable by its owner and the administrative accounts alone.
///
/// For a secret ONE process holds -- a node's identity key -- where
/// `SecureSecretFileForServices`'s grant to every service on the machine would
/// hand read to principals with no business with it. On Windows a protected
/// access list of SYSTEM, Administrators and the file's OWNER (OWNER RIGHTS, so
/// it follows whoever created the file rather than naming an account); on POSIX
/// every group and other bit removed.
///
/// **It answers with the exposure it READ BACK, not with a `bool`**, because the
/// caller has three outcomes to tell apart and only it can decide between them:
/// `None` is secured, an exposure is a list that did not take -- or that somebody
/// widened -- and `Undetermined` is a filesystem that keeps no permissions at
/// all, which `SecretExposure` defines as reported and never refused. Whether the
/// apply call returned success is not consulted: the property is asked through
/// the same predicate that would otherwise warn about this file.
///
/// Does not create the file, and says nothing useful about one that is absent
/// (`Undetermined`).
///
/// @param file An existing file.
/// @return Who other than its owner and the administrators may read it now.
[[nodiscard]] SecretExposure SecureSecretFileForOwner(std::filesystem::path const& file);

/// An open stream this process owns, closed by the handle.
using SecretFileStream = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

/// Create the state file @p which at @p file NEW, with the access its row of the state-file table
/// gives it (`StateFileAccessOf`), and open it for writing with nothing else able to open it while
/// it is.
///
/// **Access is decided at OPEN, never per read**, so a file protected after it was created is
/// protected against every open that comes AFTER -- and a handle somebody opened in between keeps
/// reading whatever is written through it later. So the access is part of the create:
/// - `OwnerOnly` (the key): on Windows a security descriptor carrying `SecureSecretFileForOwner`'s
///   list, protected against the directory's;
/// - `OthersRead`: on Windows the directory's inherited list, which carries every grant the
///   directory holds -- a service account's among them, whenever it was made -- so a file an
///   elevated operator wrote into a service's directory stays readable by the service;
/// - on POSIX the row's mode, EXACTLY (`StateFilePosixMode`): set on the new descriptor after the
///   create, so a permissive umask cannot widen it (under umask 000, `0666` would let every account
///   rewrite what the node acts on) and a strict one cannot narrow it.
///
/// Share mode 0 on Windows, which refuses every other open for as long as this one lasts.
/// Exclusive, with nothing in front of it (`CREATE_NEW`, `O_CREAT | O_EXCL | O_NOFOLLOW`): a file
/// that is already there is `std::errc::file_exists`, never truncated or written through.
/// @param file The file to create.
/// @param which Which state file it is, and so who may read it.
/// @return The stream, or why the file could not be created.
[[nodiscard]] std::expected<SecretFileStream, std::error_code> CreateStateFile(std::filesystem::path const& file,
                                                                               StateFile which);

/// Who owns a file, as far as trusting what is in it goes.
///
/// **Private: never transmitted or persisted.**
enum class FileOwnerStanding : std::uint8_t
{
    ThisProcess,    ///< The account this process runs as.
    Administrative, ///< SYSTEM or Administrators on Windows; root on POSIX.
    Another,        ///< Any other account: somebody else put this file here.
    Undetermined,   ///< The platform would not say.
    Last,
};

/// A file's owner, and what to call it in a sentence.
struct FileOwner
{
    FileOwnerStanding standing { FileOwnerStanding::Undetermined }; ///< Whose it is.
    std::string name; ///< The owner as an operator reads it: `DOMAIN\name`, a SID, or `uid N`.
};

/// Who owns @p path.
///
/// The integrity half of a secret this process holds itself: a file nobody else can READ may
/// still be one somebody else WROTE, holding a secret they chose -- and a process that adopts
/// it proves itself with a key its author holds too. `SecretFileExposure` deliberately asks
/// nothing about the owner, so this is the question beside it.
///
/// **The entry itself, never what it points at**: `lstat`, and a reparse point opened as itself
/// on Windows. A link planted beside the files a process trusts, pointing at a file this account
/// owns elsewhere, would otherwise answer `ThisProcess` for a file the process never wrote.
/// @param path An existing file or directory.
/// @return Its owner, `Undetermined` when the platform would not say.
[[nodiscard]] FileOwner FileOwnerOf(std::filesystem::path const& path);

/// Whether @p path is a link rather than a file or directory of its own: a symbolic link on
/// POSIX, any reparse point (a symbolic link or a junction) on Windows.
/// @param path An entry, which need not resolve.
/// @return True for a link; false for anything else, and for nothing there.
[[nodiscard]] bool IsLinkEntry(std::filesystem::path const& path);

/// Whether an account other than @p path's owner and the administrators may WRITE it.
///
/// A file somebody else can rewrite is one whose contents they choose, whoever owns it. On Windows
/// a broad principal (`Users`, `Everyone`, ...) granted any right that writes or re-grants the
/// entry; on POSIX a group or other write bit, read with `lstat` -- the entry itself, never what a
/// link points at.
/// @param path An existing entry.
/// @return True when others may write it, false when nobody but its owner and the administrators
///         may, and the platform's own error when it would not say -- which is an outcome of its
///         own, and never folded into either answer here: an integrity check that cannot tell
///         must not read as one that found nothing, and each caller decides what it means.
[[nodiscard]] std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& path);

/// The commands that make an entry others may write its owner's alone, in the order they must run:
/// `chmod go-w` on POSIX; on Windows inheritance converted and then the broad principals removed,
/// two commands for `DirectoryWritersRemedy`'s reason.
/// @param path The entry.
/// @return The command lines.
[[nodiscard]] std::vector<std::string> OthersMayWriteRemedy(std::filesystem::path const& path);

/// Why `OpenRegularFile` did not open a file.
struct RegularFileRefusal
{
    std::error_code error;     ///< What the platform said; `no_such_file_or_directory` for an absent one.
    bool notRegular { false }; ///< True when there IS an entry and it is not a regular file.
};

/// Open @p path for reading only if it is a regular file of its own.
///
/// **Never blocks, and never follows a link.** A FIFO planted where a file is expected would
/// block an ordinary open until somebody writes to it -- a start that never ends, before any
/// judgement it was about to make. So on POSIX `O_NONBLOCK | O_NOFOLLOW`, then `fstat` on the
/// descriptor; on Windows `FILE_FLAG_OPEN_REPARSE_POINT`, then the handle's attributes and type.
/// Anything but a regular file is refused (`notRegular`) rather than read.
/// @param path The file.
/// @return The stream, or why it was not opened.
[[nodiscard]] std::expected<SecretFileStream, RegularFileRefusal> OpenRegularFile(std::filesystem::path const& path);

/// Whether accounts other than a directory's owner and the administrators can add or remove
/// what is in it.
///
/// **Private: never transmitted or persisted.**
enum class DirectoryWriters : std::uint8_t
{
    OwnerOnly,    ///< Only its owner and the administrators can.
    Others,       ///< Another account can create or delete entries in it.
    ForeignOwner, ///< Another account owns it, and so can grant itself whatever it likes.
    Undetermined, ///< The platform would not say.
    Last,
};

/// Who can add or remove entries in @p directory.
///
/// For a directory a process keeps its OWN secrets and state in, which is a different rule from
/// `IsAdministratorOnlyWritable`'s machine-wide config directory: this one may belong to the
/// account the process runs as. On Windows a broad principal (`Users`, `Everyone`, ...) granted
/// add-file, add-subdirectory, delete-child or the rights to take the list over is `Others`; on
/// POSIX a group or other write bit is -- the sticky bit included, which stops others removing
/// or renaming an entry and not CREATING a name that is not there yet: a state file before its
/// first write, or a temporary. A state directory has no reason to be shared. An owner that is
/// neither this process's account nor an administrative one is `ForeignOwner` on both.
/// @param directory An existing directory.
/// @return Who can write in it.
[[nodiscard]] DirectoryWriters DirectoryWritersOf(std::filesystem::path const& directory);

/// Create @p directory, and every missing parent, with the directory itself readable, writable
/// and listable by its owner and the administrators alone from its first instant.
///
/// On Windows a protected list of SYSTEM, Administrators and OWNER RIGHTS, inherited by what is
/// created in it; on POSIX mode 0700. Parents are created as `create_directories` creates them:
/// they are not this directory, and a directory's own list is what decides who may add to it.
/// A directory that is already there is left exactly as it is -- `DirectoryWritersOf` judges it.
/// @param directory The directory.
/// @return Nothing, or why it could not be created.
[[nodiscard]] std::expected<void, std::error_code> CreateOwnerOnlyDirectory(std::filesystem::path const& directory);

/// The commands that make a directory other accounts may write in its owner's alone, in the order
/// they must run.
///
/// **Two on Windows, and neither is enough alone.** A directory under `%ProgramData%` INHERITS its
/// broad grants, and `icacls /inheritance:d /remove:g ...` as ONE command runs the removal against
/// the inherited entries before converting them -- it reports success and removes nothing,
/// measured. So inheritance is converted first, and the broad principals removed second. Both
/// keep every other grant, the service account's among them, and the removal propagates to what
/// the directory holds -- which is why a caller whose directory holds a secret must say what that
/// secret was exposed to BEFORE it hands these out.
/// @param directory The directory.
/// @param writers What `DirectoryWritersOf` found.
/// @return The command lines, empty for anything but `Others`.
[[nodiscard]] std::vector<std::string> DirectoryWritersRemedy(std::filesystem::path const& directory,
                                                              DirectoryWriters writers);

/// What to tell an operator about a directory that is not its owner's alone, and how to fix it.
/// @param directory The directory.
/// @param writers What `DirectoryWritersOf` found; `OwnerOnly` yields an empty string.
/// @return A sentence naming what was found and the remedy, or empty.
[[nodiscard]] std::string DirectoryWritersHint(std::filesystem::path const& directory, DirectoryWriters writers);

/// Give @p directory a protected access list of its own: SYSTEM and Administrators in full,
/// @p account Modify, inherited by everything created inside; nothing inherited from its
/// parent, its OWNER set to Administrators, and any owner held to reading the list.
///
/// For a directory holding a credential a SERVICE mints -- the node's identity key --
/// where `SecureSecretFileForServices`' per-file list cannot help, because the file does
/// not exist yet when the installer runs. Adding the account to what the directory
/// inherits is not enough either: `%ProgramData%` grants `BUILTIN\Users` read
/// inheritably, and an added entry leaves that in place.
///
/// It refuses rather than trusting an existing directory as-is, because a standard account
/// can arrange one before the installer runs, with no privilege:
///  - a reparse point (a junction or symlink) on the directory or any entry below it, which
///    would redirect the service's key to a directory the planter controls;
///  - an entry owned by anyone but SYSTEM, Administrators or @p account, which a planter
///    still knows the contents of;
///  - a non-directory entry with more than one hard link, which shares its access list with a
///    file outside the directory the apply would then rewrite;
///  - an entry that keeps an explicit broad grant of its own through the new list.
///
/// The owner is set to Administrators, and what is already inside is covered too: on Windows
/// the directory's new list replaces every INHERITED entry below it. The structure checks run
/// in a read-only pre-pass AND again after the list is applied: once the protected list and the
/// Administrators owner are on, no NEW open by anybody else can create or change anything there.
/// A handle opened BEFORE the apply keeps the access it was granted, since an access list is
/// checked at open and not at each write, so a file that was broadly writable until then can
/// still be written through such a handle; that residual is accepted, because refusing while
/// other handles are open would refuse every re-apply with the service holding its key open. On
/// POSIX it removes every group and other bit from the directory, which hides what is inside
/// without visiting it.
///
/// @param directory An existing directory.
/// @param account The service's account (`NT SERVICE\<name>`), which resolves only once
///        the service exists; empty for a service running as LocalSystem, which the list
///        names already. Unused on POSIX, where the caller has made it the owner.
/// @param credentialLeaves Leaf names of files in @p directory that hold a credential this
///        node minted (the identity key). An exposed one is refused with a DELETE remedy --
///        following an `icacls /reset` would keep a disclosed key in service -- where any
///        other state file is told to inherit the directory's list. Empty on POSIX.
/// @return Nothing when nothing outside those accounts can read the directory or what it
///         holds afterwards -- the property, read back -- else why not, naming the entry.
[[nodiscard]] std::expected<void, std::string> SecureDirectoryForService(
    std::filesystem::path const& directory,
    std::string const& account,
    std::span<std::filesystem::path const> credentialLeaves = {});

/// The command that would make @p directory administrator-only writable,
/// spelled for the platform this build targets.
///
/// Lives beside the check so a caller that has to explain a rejection does not
/// have to know which platform it is on, and so the advice cannot drift from
/// the rule that produced it.
///
/// @param directory The directory to secure.
/// @return A ready-to-paste shell command.
[[nodiscard]] std::string SecureDirectoryHint(std::filesystem::path const& directory);

} // namespace FastCache
