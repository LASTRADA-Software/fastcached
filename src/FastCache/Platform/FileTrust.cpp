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
    #include <optional>
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

    /// The rights that let a principal READ a file's contents.
    ///
    /// `FILE_READ_DATA` is the one that matters; the two generic masks include it
    /// and are what an inherited entry is usually spelled with -- the packaged
    /// `%ProgramData%\fastcached` list grants `BUILTIN\Users` `0x1200a9`, which
    /// is `FILE_GENERIC_READ|FILE_GENERIC_EXECUTE`, so a scan for the bare bit
    /// alone would miss the very entry this exists to find.
    constexpr DWORD ReadingRights = FILE_READ_DATA | GENERIC_READ | GENERIC_ALL;

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

    /// Is nothing in BroadPrincipals granted any of @p rights on @p path?
    ///
    /// **The rights are a parameter and not a constant**, because integrity and
    /// secrecy are the same walk over a different mask: `PlantingRights` answers
    /// "could an unprivileged account have replaced this file", `ReadingRights`
    /// answers "can an unprivileged account read what is in it"
    /// ([#384](https://github.com/LASTRADA-Software/fastcached/issues/384)). Two
    /// scans differing only in a constant is the repetition a parameter exists to
    /// remove -- and a copy is the one that would never have learned about
    /// `GENERIC_ALL`.
    ///
    /// `std::optional` rather than `bool`, because the two callers want opposite
    /// things from "I could not tell". Writability must treat it as unsafe -- an
    /// unreadable security descriptor is exactly what a planted config would
    /// present -- while secrecy must report it as its own outcome rather than
    /// claim an exposure nobody established. Folding the two into `false` is what
    /// made this a `bool` in the first place, and it is only right for one of them.
    ///
    /// @param path Entry to inspect.
    /// @param rights The mask an entry must grant to count.
    /// @return true when no broad principal is granted any of them, false when
    ///         one is, nullopt when the access list would not say.
    [[nodiscard]] std::optional<bool> NoBroadPrincipalMay(std::filesystem::path const& path, DWORD rights)
    {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor)
            != ERROR_SUCCESS)
            return std::nullopt;

        auto const owned = LocalBlock { descriptor };

        // An absent DACL is not an empty one: it grants everyone everything. So
        // this is a determinate answer -- everybody may -- and not a failure to
        // read one.
        if (dacl == nullptr)
            return false;

        ACL_SIZE_INFORMATION size {};
        if (::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) == FALSE)
            return std::nullopt;

        for (auto const index: std::views::iota(DWORD { 0 }, DWORD { size.AceCount }))
        {
            void* entry = nullptr;
            if (::GetAce(dacl, index, &entry) == FALSE)
                return std::nullopt;

            // Only allow entries carry a grant; deny entries and the auditing
            // types can subtract from one but never add.
            if (static_cast<ACE_HEADER const*>(entry)->AceType != ACCESS_ALLOWED_ACE_TYPE)
                continue;

            auto* const allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            if ((allowed->Mask & rights) == 0)
                continue;

            // The SID begins at SidStart and runs past it; the member is the
            // documented handle on it, not a value.
            if (IsBroadPrincipal(&allowed->SidStart))
                return false;
        }

        return true;
    }

    /// @param path Entry to inspect.
    /// @return true when nothing in BroadPrincipals is granted any of
    ///         PlantingRights on it. An undeterminable list answers false, for
    ///         the reason `NoBroadPrincipalMay` gives: "I cannot tell" and "it is
    ///         not safe" have to lead to the same place here.
    [[nodiscard]] bool NoBroadPrincipalMayWrite(std::filesystem::path const& path)
    {
        return NoBroadPrincipalMay(path, PlantingRights).value_or(false);
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

    /// The readability facts, from the same `stat` the writability half reads.
    ///
    /// Reports what the mode bits SAY and decides nothing: the rule is
    /// `ClassifySecretFile`, which is where the delegation clause lives and where
    /// both platforms' records meet one implementation.
    ///
    /// @param path Entry to inspect.
    /// @return What POSIX reports about who may read it.
    [[nodiscard]] SecretFileFacts PosixSecretFileFacts(std::filesystem::path const& path)
    {
        struct ::stat info {};

        if (::stat(path.c_str(), &info) != 0)
            return SecretFileFacts {};

        return SecretFileFacts {
            .determined = true,
            .readableByAnyAccount = (info.st_mode & static_cast<::mode_t>(S_IROTH)) != 0,
            .readableByGroup = (info.st_mode & static_cast<::mode_t>(S_IRGRP)) != 0,
            .administrativelyOwned = info.st_uid == 0,
        };
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

SecretFileFacts ObserveSecretFile(std::filesystem::path const& path)
{
#if defined(_WIN32)
    // The same access-list walk the writability half does, over a different
    // rights mask -- which is why `NoBroadPrincipalMayWrite` became
    // `NoBroadPrincipalMay(path, rights)` rather than being copied. Two scans
    // differing only in a constant is the repetition a parameter exists to
    // remove, and a copy would have been the one that never learned about
    // `GENERIC_ALL`.
    //
    // No owner test and no parent test, unlike the writability half. An owner can
    // always read their own file, which is not an exposure; and a directory
    // nobody else may TRAVERSE cannot hide a file whose own list grants read,
    // because the check that matters is on the file the daemon opens.
    //
    // `readableByGroup` stays false and `administrativelyOwned` with it: a DACL
    // does not separate a narrow group grant from a broad one in the way the
    // delegation clause needs, so there is nothing here to report against it. See
    // `SecretFileFacts`.
    auto const answer = NoBroadPrincipalMay(path, ReadingRights);
    if (!answer.has_value())
        return SecretFileFacts {};
    return SecretFileFacts { .determined = true, .readableByAnyAccount = !*answer };
#else
    return PosixSecretFileFacts(path);
#endif
}

SecretExposure SecretFileExposure(std::filesystem::path const& path)
{
    return ClassifySecretFile(ObserveSecretFile(path));
}

std::string SecretExposureHint(std::filesystem::path const& path, SecretExposure exposure)
{
    switch (exposure)
    {
        case SecretExposure::None:
        case SecretExposure::Last:
            return {};
        case SecretExposure::Undetermined:
            return std::format("could not determine who may read {}, so whether the secret in it is protected is "
                               "unknown",
                               path.string());
        case SecretExposure::AnyLocalAccount:
#if defined(_WIN32)
            return std::format("{} is readable by every account on this machine, so the secret in it is not protected; "
                               "restrict it with: icacls \"{}\" /inheritance:r /grant *S-1-5-18:R /grant "
                               "\"NT SERVICE\\fastcached\":R",
                               path.string(),
                               path.string());
#else
            return std::format("{} is readable by every account on this machine, so the secret in it is not protected; "
                               "restrict it with: chmod o-r {}",
                               path.string(),
                               path.string());
#endif
        case SecretExposure::OwnersOwnGroup:
            return std::format("{} is readable by its group and is not owned by root, so the secret in it is exposed to "
                               "accounts its owner does not answer for; restrict it with: chmod g-r {}",
                               path.string(),
                               path.string());
    }
    return {};
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
