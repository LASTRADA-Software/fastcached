// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/FileTrust.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include <array>
    #include <cstddef>
    #include <memory>
    #include <ranges>
    #include <span>

    #include <windows.h>
    // After windows.h: both depend on its types, and WIN32_LEAN_AND_MEAN (set
    // on the target) keeps them from arriving on their own.
    #include <aclapi.h>
    #include <sddl.h>
#else
    #include <sys/stat.h>

    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{

    /// The directory an entry lives in.
    /// @param path Entry to look at.
    /// @return Its parent, or `.` for a bare relative name, which has no parent
    ///         to name but does have one to check.
    [[nodiscard]] std::filesystem::path ParentOf(std::filesystem::path const& path)
    {
        auto parent = path.parent_path();
        return parent.empty() ? std::filesystem::path { "." } : parent;
    }

#if defined(_WIN32)

    /// Frees the single LocalAlloc'd block `GetNamedSecurityInfoW` hands back.
    /// The ACL it also yields points into that block, so this is the only
    /// release the caller owes.
    struct LocalFreeDeleter
    {
        void operator()(void* block) const noexcept
        {
            ::LocalFree(block);
        }
    };

    using LocalBlock = std::unique_ptr<void, LocalFreeDeleter>;

    /// Groups every local account is a member of simply by existing. A write
    /// granted to one of these is a write granted to everybody, whatever the
    /// object's owner happens to be.
    ///
    /// Well-known SID *types* rather than SDDL strings: `CreateWellKnownSid`
    /// builds them into a caller-supplied buffer, so the comparison needs no
    /// allocation, no parsing, and no second spelling to keep in step.
    constexpr auto BroadPrincipals = std::to_array<WELL_KNOWN_SID_TYPE>({
        WinWorldSid,             // Everyone
        WinAuthenticatedUserSid, // Authenticated Users
        WinBuiltinUsersSid,      // BUILTIN\Users
        WinInteractiveSid,       // INTERACTIVE
        WinBuiltinGuestsSid,     // BUILTIN\Guests
    });

    /// The rights that let a principal put a *different* file at a path:
    /// overwrite the contents, add an entry to the directory, delete what is
    /// there, or take control and grant itself the rest.
    ///
    /// The same bits carry both meanings, which is why one constant covers a
    /// file and its directory: `FILE_WRITE_DATA` is `FILE_ADD_FILE` on a
    /// directory, and `FILE_APPEND_DATA` is `FILE_ADD_SUBDIRECTORY`.
    constexpr DWORD PlantingRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

    /// The access control list a machine-wide config directory should carry:
    /// SYSTEM and Administrators in full, everyone else read and traverse only,
    /// and `P` for protected so the permissive inheritance from %ProgramData%
    /// cannot leak back in.
    ///
    /// The MSI fragment spells the same policy, because an installer cannot
    /// call into this. The two are deliberately not required to be identical
    /// strings: IsAdministratorOnlyWritable above is the single arbiter both
    /// are judged by, so a drift between them shows up as a startup refusal
    /// naming the directory, not as a silently weaker ACL.
    constexpr auto AdministratorOnlyDacl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

    /// The principals a machine-wide directory may belong to. An owner keeps
    /// WRITE_DAC whatever the access list says, so a directory owned by a
    /// standard account is one that account can re-open at will — which makes
    /// the owner as load-bearing as the entries, and a check of the entries
    /// alone easy to walk past: create the directory, then tighten it.
    constexpr auto AdministrativeOwners = std::to_array<WELL_KNOWN_SID_TYPE>({
        WinLocalSystemSid,          // NT AUTHORITY\SYSTEM
        WinBuiltinAdministratorsSid // BUILTIN\Administrators
    });

    /// TrustedInstaller owns most of what Windows itself installs (including
    /// `%SystemRoot%\System32\drivers\etc`) and has no WELL_KNOWN_SID_TYPE, so
    /// it is the one owner that has to be spelled out.
    constexpr auto TrustedInstallerSid = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    /// @param sid Security identifier to test.
    /// @param wellKnown Types to compare it against.
    /// @return true when @p sid is any of them.
    [[nodiscard]] bool MatchesWellKnownSid(PSID sid, std::span<WELL_KNOWN_SID_TYPE const> wellKnown)
    {
        for (auto const type: wellKnown)
        {
            std::array<std::byte, SECURITY_MAX_SID_SIZE> buffer {};
            auto size = static_cast<DWORD>(buffer.size());
            if (::CreateWellKnownSid(type, nullptr, buffer.data(), &size) == FALSE)
                continue;
            if (::EqualSid(sid, buffer.data()) == TRUE)
                return true;
        }
        return false;
    }

    /// @param sid Security identifier from an access-allowed entry.
    /// @return true when it names one of BroadPrincipals.
    [[nodiscard]] bool IsBroadPrincipal(PSID sid)
    {
        return MatchesWellKnownSid(sid, BroadPrincipals);
    }

    /// @param path Entry to inspect.
    /// @return true when it is owned by SYSTEM, Administrators or
    ///         TrustedInstaller.
    [[nodiscard]] bool IsAdministrativelyOwned(std::filesystem::path const& path)
    {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor)
            != ERROR_SUCCESS)
            return false;

        auto const owned = LocalBlock { descriptor };
        if (owner == nullptr)
            return false;

        if (MatchesWellKnownSid(owner, AdministrativeOwners))
            return true;

        PSID trustedInstaller = nullptr;
        if (::ConvertStringSidToSidW(TrustedInstallerSid, &trustedInstaller) == FALSE)
            return false;

        auto const ownedSid = LocalBlock { trustedInstaller };
        return ::EqualSid(owner, trustedInstaller) == TRUE;
    }

    /// @param path Entry to inspect.
    /// @return true when nothing in BroadPrincipals is granted any of
    ///         PlantingRights on it.
    [[nodiscard]] bool NoBroadPrincipalMayWrite(std::filesystem::path const& path)
    {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor)
            != ERROR_SUCCESS)
            return false;

        auto const owned = LocalBlock { descriptor };

        // An absent DACL is not an empty one: it grants everyone everything.
        if (dacl == nullptr)
            return false;

        ACL_SIZE_INFORMATION size {};
        if (::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) == FALSE)
            return false;

        for (auto const index: std::views::iota(DWORD { 0 }, DWORD { size.AceCount }))
        {
            void* entry = nullptr;
            if (::GetAce(dacl, index, &entry) == FALSE)
                return false;

            // Only allow entries carry a grant; deny entries and the auditing
            // types can subtract from one but never add.
            if (static_cast<ACE_HEADER const*>(entry)->AceType != ACCESS_ALLOWED_ACE_TYPE)
                continue;

            auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            if ((allowed->Mask & PlantingRights) == 0)
                continue;

            // The SID begins at SidStart and runs past it; the member is the
            // documented handle on it, not a value.
            if (IsBroadPrincipal(&allowed->SidStart))
                return false;
        }

        return true;
    }

#else

    /// @param path Entry to inspect.
    /// @return true when it is owned by root and writable by neither group nor
    ///         world — the rule sshd and sudo apply to their own configuration,
    ///         for the same reason.
    [[nodiscard]] bool RootOwnedAndUnwritableByOthers(std::filesystem::path const& path)
    {
        struct ::stat info {};

        if (::stat(path.c_str(), &info) != 0)
            return false;

        return info.st_uid == 0 && (info.st_mode & static_cast<::mode_t>(S_IWGRP | S_IWOTH)) == 0;
    }

#endif

    /// Can only an administrator add or replace an entry in @p directory?
    ///
    /// The load-bearing half of both public functions: the one they ask about a
    /// containing directory, and the one SecureDirectoryForAdministrators has
    /// to be able to establish before it may claim success.
    ///
    /// @param directory Directory to inspect.
    /// @return true when no non-administrative principal can write in it.
    [[nodiscard]] bool IsAdministrativeContainer(std::filesystem::path const& directory)
    {
#if defined(_WIN32)
        // Owner as well as entries: an owner keeps WRITE_DAC whatever the
        // entries say, so a directory a standard account owns is one it can
        // re-open whenever it likes.
        return IsAdministrativelyOwned(directory) && NoBroadPrincipalMayWrite(directory);
#else
        return RootOwnedAndUnwritableByOthers(directory);
#endif
    }

} // namespace

bool IsPrivilegedProcess()
{
#if defined(_WIN32)
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators {};
    auto size = static_cast<DWORD>(administrators.size());
    if (::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size) == FALSE)
        return false;

    // A null token means the effective one. Membership here is *enabled*
    // membership, so the unelevated half of a split administrator token answers
    // false — which is the right answer: that process could not have written
    // the machine-wide config either.
    BOOL member = FALSE;
    if (::CheckTokenMembership(nullptr, administrators.data(), &member) == FALSE)
        return false;

    return member == TRUE;
#else
    return ::geteuid() == 0;
#endif
}

bool IsAdministratorOnlyWritable(std::filesystem::path const& path)
{
    if (!IsAdministrativeContainer(ParentOf(path)))
        return false;

    // The file itself is checked for a permissive entry somebody added after
    // the fact — but on Windows not for its owner, because a config written by
    // an elevated administrator is owned by that person's own account and
    // demanding otherwise would reject the ordinary hand-edited file. Only an
    // administrator could have created it in a directory that just passed.
#if defined(_WIN32)
    return NoBroadPrincipalMayWrite(path);
#else
    return RootOwnedAndUnwritableByOthers(path);
#endif
}

bool SecureDirectoryForAdministrators(std::filesystem::path const& directory)
{
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(AdministratorOnlyDacl, SDDL_REVISION_1, &descriptor, nullptr)
        == FALSE)
        return false;

    auto const owned = LocalBlock { descriptor };

    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE || present == FALSE)
        return false;

    // The owner goes with the entries, and it is the half that cannot be
    // faked: Windows lets a caller hand ownership to a group its own token
    // carries, so an elevated administrator can do this and a standard account
    // cannot. That refusal is a feature — it is what stops an unprivileged
    // `--seed-config` from planting a machine-wide config that would otherwise
    // pass the startup check, since the planter would still own the directory
    // and keep WRITE_DAC over it.
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators {};
    auto size = static_cast<DWORD>(administrators.size());
    if (::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size) == FALSE)
        return false;

    // PROTECTED_DACL_SECURITY_INFORMATION is what actually severs inheritance.
    // The `P` in the SDDL only marks the descriptor being built here, not the
    // object it ends up on, so leaving the flag out would apply the three
    // entries and then let %ProgramData%'s permissive ones flow in beside them.
    //
    // SetNamedSecurityInfoW takes a mutable name, hence the owned copy.
    auto name = directory.wstring();
    if (::SetNamedSecurityInfoW(name.data(),
                                SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                administrators.data(),
                                nullptr,
                                dacl,
                                nullptr)
        != ERROR_SUCCESS)
        return false;
#else
    // Ownership is not something chmod can fix, and it does not need fixing:
    // only root can create the machine-wide config directory. What is left is
    // the umask, which can leave a fresh directory group-writable.
    std::error_code ec;
    std::filesystem::permissions(directory,
                                 std::filesystem::perms::group_write | std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove,
                                 ec);
    if (ec)
        return false;
#endif

    // Report the property, not the syscall. What the caller needs to know is
    // whether the directory *is* administrator-only afterwards, and on POSIX a
    // successful chmod does not establish that — a non-root caller ends up with
    // a tidy directory it still owns. Asking the same question the startup
    // check asks is also what keeps the two from ever disagreeing.
    return IsAdministrativeContainer(directory);
}

std::string SecureDirectoryHint(std::filesystem::path const& directory)
{
#if defined(_WIN32)
    // SIDs rather than account names, so the advice is not itself wrong on a
    // non-English Windows: LocalSystem, Administrators, Users.
    //
    // /setowner comes first and is not optional: an owner keeps WRITE_DAC
    // however the entries are set, so repairing only the entries of a directory
    // somebody else created would leave them able to undo the repair.
    return std::format(R"(icacls "{}" /setowner *S-1-5-32-544 /inheritance:r /grant *S-1-5-18:(OI)(CI)F )"
                       R"(/grant *S-1-5-32-544:(OI)(CI)F /grant *S-1-5-32-545:(OI)(CI)RX)",
                       directory.string());
#else
    return std::format("sudo chown root '{0}' && sudo chmod go-w '{0}'", directory.string());
#endif
}

} // namespace FastCache
