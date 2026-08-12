// SPDX-License-Identifier: Apache-2.0
#pragma once

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
