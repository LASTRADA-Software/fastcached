// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

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
