// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Platform/FileTrustDetail.hpp>

#include <cerrno>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #include <FastCache/Platform/NarrowText.hpp>

    #include <algorithm>
    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <ranges>
    #include <span>
    #include <type_traits>
    #include <vector>

    #include <windows.h>

    #include <fcntl.h>
    #include <io.h>
    // After windows.h: both depend on its types, and WIN32_LEAN_AND_MEAN (set
    // on the target) keeps them from arriving on their own.
    #include <aclapi.h>
    #include <sddl.h>
#else
    #include <sys/stat.h>

    #include <fcntl.h>
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

    /// The access list a file holding a secret should carry: SYSTEM and
    /// Administrators in full, the machine's services read, and nobody else.
    ///
    /// `P` for protected, and here it is the entire point rather than a
    /// hardening detail. The directory above grants `BU` read *inheritably*, on
    /// purpose -- the daemon runs as a virtual account, which is an ordinary
    /// `BUILTIN\Users` member -- so a file left to inherit is readable by every
    /// local account, which is the file `requirepass:` is told to live in (#741).
    ///
    /// `SU` is `NT AUTHORITY\SERVICE`, S-1-5-6: every principal logged on as a
    /// service. See `SecureSecretFileForServices` for why it is that and not the
    /// per-service SID. No inheritance flags -- a file has nothing to inherit it.
    ///
    /// As with AdministratorOnlyDacl, this and `SecretExposureHint`'s `icacls`
    /// line are not required to be identical strings: `SecretFileExposure` is the
    /// single arbiter both are judged by, so a drift between them shows up as a
    /// warning naming the file rather than as a silently weaker list.
    constexpr auto SecretFileDacl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;SU)";

    /// The access list a secret held by ONE account should carry: SYSTEM and
    /// Administrators in full, the file's owner in full, and nobody else.
    ///
    /// `SecretFileDacl` without its service grant, and with the owner in its place:
    /// a node's identity key is read by the process that minted it and by nothing
    /// else, so a grant to every service on the machine would be read access handed
    /// to principals that have no business with it. `OW` is OWNER RIGHTS
    /// (S-1-3-4), which follows the file's owner rather than naming an account, so
    /// the list is the same whether a user, a virtual service account or SYSTEM
    /// created the file -- and it replaces the owner's implicit rights rather than
    /// adding to them, which `FA` makes moot. Protected, for `SecretFileDacl`'s
    /// reason: the directory's list is not consulted.
    constexpr auto OwnerOnlySecretFileDacl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)";

    /// The access list a directory ONE account keeps its secrets in should carry: the entries of
    /// `OwnerOnlySecretFileDacl`, inherited by every file and subdirectory created in it. Protected,
    /// so the permissive entries a parent like `%ProgramData%` grants never flow in.
    constexpr auto OwnerOnlyDirectoryDacl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)";

    /// The rights that let a principal put a different file in a DIRECTORY, or take one away:
    /// `PlantingRights` (whose `FILE_WRITE_DATA` and `FILE_APPEND_DATA` ARE add-file and
    /// add-subdirectory on a directory) and delete-child, which removes an entry whatever the
    /// entry's own list says.
    constexpr DWORD EntryPlantingRights = PlantingRights | FILE_DELETE_CHILD;

    /// The access list a directory holding a credential a SERVICE mints should carry,
    /// before the service's own entry is appended: SYSTEM and Administrators in full,
    /// inherited by everything created inside, and `P` so `%ProgramData%`'s
    /// `BUILTIN\Users` read cannot flow in.
    ///
    /// **And `OW` (OWNER RIGHTS, S-1-3-4) held to `RC`**, which is not decoration. An
    /// owner keeps `READ_CONTROL` and `WRITE_DAC` whatever the entries say, unless an
    /// OWNER RIGHTS entry names what it keeps instead -- and `%ProgramData%` lets any
    /// standard account CREATE a subdirectory, so the state directory may be owned by
    /// whoever made it first. With `WRITE_DAC` that account re-opens the directory,
    /// deletes the key, and reads the one the node mints to replace it. Held to `RC`,
    /// the owner may read the list and change nothing; every account that should
    /// write here has an entry of its own. Inherited, so a file the service creates
    /// holds its owner -- the service -- to the same, where its own entry grants the rest.
    constexpr auto ServiceDirectoryDacl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;RC;;;OW)";

    /// @param path Entry to inspect.
    /// @return Whether its access list is protected from inheritance, nullopt when the
    ///         descriptor would not say.
    [[nodiscard]] std::optional<bool> DaclIsProtected(std::filesystem::path const& path)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor)
            != ERROR_SUCCESS)
            return std::nullopt;

        auto const owned = LocalBlock { descriptor };
        SECURITY_DESCRIPTOR_CONTROL control {};
        DWORD revision = 0;
        if (::GetSecurityDescriptorControl(descriptor, &control, &revision) == FALSE)
            return std::nullopt;
        return (control & SE_DACL_PROTECTED) != 0;
    }

    /// The SDDL entry granting @p account full control, inheritably.
    /// @param account An account name `LookupAccountName` resolves.
    /// @return The entry, or why the account did not resolve.
    [[nodiscard]] std::expected<std::wstring, std::string> ServiceAccountEntry(std::string const& account)
    {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sid {};
        auto sidSize = static_cast<DWORD>(sid.size());
        // The domain buffer is required by the call and read by nobody.
        std::array<char, 256> domain {};
        auto domainSize = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        if (::LookupAccountNameA(nullptr, account.c_str(), sid.data(), &sidSize, domain.data(), &domainSize, &use) == FALSE)
            return std::unexpected(std::format("the account '{}' did not resolve (error {})", account, ::GetLastError()));

        LPWSTR text = nullptr;
        if (::ConvertSidToStringSidW(sid.data(), &text) == FALSE)
            return std::unexpected(
                std::format("the account '{}' has a SID that could not be spelled (error {})", account, ::GetLastError()));
        auto const owned = LocalBlock { text };
        // `0x1301bf` is Modify: FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE
        // | DELETE, and NOT `WRITE_DAC` or `WRITE_OWNER`. This process compiles input that
        // arrived over the network, so it reads its key and writes its state but may not
        // rewrite the list that protects them -- which `FA` (full control) would have let it,
        // and an owner it does not have would be the only thing then stopping it re-opening
        // the directory to every account.
        return std::wstring { L"(A;OICI;0x1301bf;;;" } + text + L")";
    }

    /// Resolve @p account to its binary SID in @p buffer.
    ///
    /// The owner comparison needs the SID itself, not the string `ServiceAccountEntry`
    /// spells; both come from one `LookupAccountName`, so this is called once and its answer
    /// shared.
    /// @param account The account name.
    /// @param buffer Filled with the SID; large enough for any.
    /// @return true when it resolved.
    [[nodiscard]] bool ResolveAccountSid(std::string const& account, std::span<std::byte> buffer)
    {
        auto size = static_cast<DWORD>(buffer.size());
        std::array<char, 256> domain {};
        auto domainSize = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        return ::LookupAccountNameA(nullptr, account.c_str(), buffer.data(), &size, domain.data(), &domainSize, &use)
               != FALSE;
    }

    /// Is @p path a reparse point -- a junction or a symbolic link?
    ///
    /// Asked WITHOUT following it: `GetFileAttributesW` reports the link's own attributes, not
    /// its target's. A junction needs no privilege to create, and
    /// `SetNamedSecurityInfoW` on one writes the list onto the junction while its target -- a
    /// directory the planter owns -- keeps its own, so a list applied to a junctioned state
    /// directory secures nothing and the service mints its key where the planter can read it.
    /// @param path Entry to inspect.
    /// @return true when it is a reparse point, false when it is not, nullopt when its
    ///         attributes could not be read.
    [[nodiscard]] std::optional<bool> IsReparsePoint(std::filesystem::path const& path)
    {
        auto const attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return std::nullopt;
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    /// Does @p path have more than one hard link -- is it the same file as one elsewhere?
    ///
    /// A hard link is not a reparse point and shares its target's security descriptor, so a key
    /// written into a file the planter can write but SYSTEM or Administrators owns, hard-linked
    /// in as `node-key`, passes the owner and reparse checks -- and the apply then rewrites the
    /// OUTSIDE file's inherited entries. `nNumberOfLinks` above one is the tell, read from a
    /// handle opened WITHOUT following a reparse point.
    /// @param path Entry to inspect.
    /// @return true when it has more than one link, false when it has one, nullopt when the
    ///         information could not be read.
    [[nodiscard]] std::optional<bool> HasMultipleHardLinks(std::filesystem::path const& path)
    {
        HANDLE const handle = ::CreateFileW(path.c_str(),
                                            FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::nullopt;

        BY_HANDLE_FILE_INFORMATION info {};
        auto const read = ::GetFileInformationByHandle(handle, &info);
        ::CloseHandle(handle);
        if (read == FALSE)
            return std::nullopt;
        return info.nNumberOfLinks > 1;
    }

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

    /// Is @p path owned by SYSTEM, Administrators or @p serviceSid?
    ///
    /// The owners a file the service or the installer wrote may have. Anything else -- a file
    /// a standard account planted -- is refused: its owner keeps `READ_CONTROL` and, planted,
    /// knows its contents, so a key adopted from it is a key the planter holds. `TrustedInstaller`
    /// is deliberately NOT allowed here, unlike `IsAdministrativelyOwned`: nothing but this
    /// directory's own service and the administrators should have written what is in it.
    /// @param path Entry to inspect.
    /// @param serviceSid The service's SID, or an empty span for a LocalSystem service.
    /// @return true when an allowed account owns it, false when another does, nullopt when
    ///         the owner could not be read.
    [[nodiscard]] std::optional<bool> IsOwnedByServiceOrAdministrator(std::filesystem::path const& path,
                                                                      std::span<std::byte const> serviceSid)
    {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor)
            != ERROR_SUCCESS)
            return std::nullopt;

        auto const owned = LocalBlock { descriptor };
        if (owner == nullptr)
            return std::nullopt;

        if (MatchesWellKnownSid(owner, AdministrativeOwners))
            return true;
        if (!serviceSid.empty() && ::EqualSid(owner, const_cast<std::byte*>(serviceSid.data())) == TRUE)
            return true;
        return false;
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
    ///         one is, and the error when the access list would not say -- carried
    ///         rather than dropped, so a caller that refuses on it can name why.
    [[nodiscard]] std::expected<bool, std::error_code> NoBroadPrincipalMay(std::filesystem::path const& path, DWORD rights)
    {
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (auto const status = ::GetNamedSecurityInfoW(
                path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
            status != ERROR_SUCCESS)
            return std::unexpected { std::error_code { static_cast<int>(status), std::system_category() } };

        auto const owned = LocalBlock { descriptor };

        // An absent DACL is not an empty one: it grants everyone everything. So
        // this is a determinate answer -- everybody may -- and not a failure to
        // read one.
        if (dacl == nullptr)
            return false;

        ACL_SIZE_INFORMATION size {};
        if (::GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) == FALSE)
            return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };

        for (auto const index: std::views::iota(DWORD { 0 }, size.AceCount))
        {
            void* entry = nullptr;
            if (::GetAce(dacl, index, &entry) == FALSE)
                return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };

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

    /// Closes a kernel handle.
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept
        {
            ::CloseHandle(handle);
        }
    };

    /// The owning handle for a kernel object.
    using OwnedHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

    /// A SID this process's token names, read out of the token-information class that carries it.
    struct TokenSidRow
    {
        TOKEN_INFORMATION_CLASS kind;                ///< The class to ask for.
        PSID (*sidOf)(std::byte const* information); ///< Where that class's answer keeps the SID.
    };

    /// The SIDs that make a file this process's own: the account it runs as, and the OWNER its
    /// token stamps on every object it creates. The two differ under an elevated token, whose
    /// default owner is BUILTIN\Administrators -- so without the second row a file this process
    /// had just created read as `Administrative` on every elevated run, CI's runners included.
    constexpr auto ThisProcessSids = std::array {
        TokenSidRow { .kind = TokenUser,
                      .sidOf = [](std::byte const* information) -> PSID {
                          return reinterpret_cast<TOKEN_USER const*>(information)->User.Sid;
                      } },
        TokenSidRow { .kind = TokenOwner,
                      .sidOf = [](std::byte const* information) -> PSID {
                          return reinterpret_cast<TOKEN_OWNER const*>(information)->Owner;
                      } },
    };

    /// Is @p sid one this process's own files are owned by?
    /// @param sid The SID to test.
    /// @return True when it is the token's user, or the owner the token stamps on what it creates.
    [[nodiscard]] bool IsThisProcessUser(PSID sid)
    {
        HANDLE raw = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == FALSE)
            return false;
        auto const token = OwnedHandle { raw };

        return std::ranges::any_of(ThisProcessSids, [&token, sid](TokenSidRow const& row) {
            DWORD size = 0;
            (void) ::GetTokenInformation(token.get(), row.kind, nullptr, 0, &size);
            if (size == 0)
                return false;
            std::vector<std::byte> buffer(size);
            if (::GetTokenInformation(token.get(), row.kind, buffer.data(), size, &size) == FALSE)
                return false;
            return ::EqualSid(row.sidOf(buffer.data()), sid) == TRUE;
        });
    }

    /// @p sid as an operator reads it: `DOMAIN\name`, or the SID itself when it names no account.
    /// @param sid The SID.
    /// @return Its name.
    [[nodiscard]] std::string AccountNameOf(PSID sid)
    {
        std::array<wchar_t, 256> name {};
        std::array<wchar_t, 256> domain {};
        auto nameSize = static_cast<DWORD>(name.size());
        auto domainSize = static_cast<DWORD>(domain.size());
        SID_NAME_USE use = SidTypeUnknown;
        if (::LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use) != FALSE)
        {
            auto const account =
                domainSize == 0 ? std::wstring { name.data() } : std::format(L"{}\\{}", domain.data(), name.data());
            if (auto utf8 = Utf8FromWideText(account); utf8.has_value())
                return *std::move(utf8);
        }
        wchar_t* text = nullptr;
        if (::ConvertSidToStringSidW(sid, &text) == FALSE)
            return "an account this machine cannot name";
        auto const owned = LocalBlock { text };
        return Utf8FromWideText(text).value_or("an account this machine cannot name");
    }

    /// Build a security descriptor from @p sddl, for a create that applies it from the first instant.
    /// @param sddl The access list.
    /// @return The descriptor, owned, or the error that refused it.
    [[nodiscard]] std::expected<LocalBlock, std::error_code> DescriptorOf(wchar_t const* sddl)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
            return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };
        return LocalBlock { descriptor };
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

#if defined(_WIN32)

    /// Apply @p sddl to @p path as a PROTECTED access list, optionally taking
    /// ownership.
    ///
    /// The apply half of both public functions below, parameterised for the same
    /// reason `NoBroadPrincipalMay` is: integrity and secrecy are the same call
    /// over a different list, and a copy is the one that never learns whatever the
    /// original learns next. `PROTECTED_DACL_SECURITY_INFORMATION` in particular is
    /// load-bearing in both and is written here once — the `P` in an SDDL string
    /// marks only the descriptor being built, not the object it ends up on, so
    /// leaving the flag out applies the entries and then lets the parent's
    /// permissive ones flow in beside them.
    ///
    /// @param path Existing file or directory; `SE_FILE_OBJECT` covers both.
    /// @param sddl The access list to apply, in SDDL.
    /// @param owner Owner to set, or nullptr to leave ownership alone.
    /// @return `ERROR_SUCCESS` when the list was applied, else the failing call's error --
    ///         a code rather than a `bool`, so a caller that must say WHY can.
    [[nodiscard]] DWORD ApplyProtectedDacl(std::filesystem::path const& path, wchar_t const* sddl, PSID owner)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
            return ::GetLastError();

        auto const owned = LocalBlock { descriptor };

        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        if (::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE)
            return ::GetLastError();
        if (present == FALSE)
            return ERROR_INVALID_SECURITY_DESCR;

        auto const what =
            static_cast<SECURITY_INFORMATION>(DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION)
            | (owner != nullptr ? static_cast<SECURITY_INFORMATION>(OWNER_SECURITY_INFORMATION) : SECURITY_INFORMATION {});

        // SetNamedSecurityInfoW takes a mutable name, hence the owned copy.
        auto name = path.wstring();
        return ::SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, what, owner, nullptr, dacl, nullptr);
    }

#else

    /// Take @p permissions away from @p path.
    ///
    /// The POSIX apply half, beside its Windows counterpart for the same reason:
    /// the two public functions differ in the mask and in nothing else.
    ///
    /// @param path Existing file or directory.
    /// @param permissions Bits to clear.
    /// @return true when the change was made.
    [[nodiscard]] bool RemovePermissions(std::filesystem::path const& path, std::filesystem::perms permissions)
    {
        std::error_code ec;
        std::filesystem::permissions(path, permissions, std::filesystem::perm_options::remove, ec);
        return !ec;
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

#if defined(_WIN32)
namespace
{
    /// Is the effective token a member of @p wellKnown?
    ///
    /// A null token means the effective one, and membership here is *enabled*
    /// membership -- so the unelevated half of a split administrator token answers
    /// false, which is the right answer: that process could not have written the
    /// machine-wide config either.
    [[nodiscard]] bool EffectiveTokenIsIn(WELL_KNOWN_SID_TYPE wellKnown) noexcept
    {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sid {};
        auto size = static_cast<DWORD>(sid.size());
        if (::CreateWellKnownSid(wellKnown, nullptr, sid.data(), &size) == FALSE)
            return false;

        BOOL member = FALSE;
        if (::CheckTokenMembership(nullptr, sid.data(), &member) == FALSE)
            return false;
        return member == TRUE;
    }
} // namespace
#endif

bool IsPrivilegedProcess()
{
#if defined(_WIN32)
    // **Two identities, because the question is "am I the machine-wide instance",
    // not "am I an administrator"** and the two parted company on 2026-08-25.
    //
    // `90edf4e0` moved the service off LocalSystem to a VIRTUAL ACCOUNT, whose token
    // is a service principal and not an administrator -- that commit's own comment
    // says so, reasoning about file readability and never noticing this predicate.
    // From then the shipped Windows service answered false here, so
    // `ResolveDefaultConfigPath` skipped every `ConfigScope::System` row and the
    // daemon silently ran on built-in defaults: `requirepass` placed in
    // `%ProgramData%\fastcached\fastcached.yaml` was not in force, and nothing was
    // logged ([#860](https://github.com/LASTRADA-Software/fastcached/issues/860)).
    //
    // **Widening the LOOKUP does not widen TRUST.** The single caller uses this
    // answer for two things -- whether to consider a system row at all, and whether
    // to demand `IsTrustedSystemLocation` of what it finds -- and a service now gets
    // BOTH. So a machine-wide config is still obeyed only when only an administrator
    // could have written it; what changed is that the service is allowed to look.
    return EffectiveTokenIsIn(WinBuiltinAdministratorsSid) || EffectiveTokenIsIn(WinServiceSid);
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
            // Raw SIDs throughout, and no account name anywhere: `NT SERVICE\<x>`
            // resolves only once that service exists, and an advice line that
            // fails on the machine it is pasted into is worse than none. These are
            // SecretFileDacl's three entries in icacls's grammar -- SYSTEM,
            // Administrators, and every principal logged on as a service.
            return std::format("{} is readable by every account on this machine, so the secret in it is not protected; "
                               "restrict it with: icacls \"{}\" /inheritance:r /grant *S-1-5-18:F /grant "
                               "*S-1-5-32-544:F /grant *S-1-5-6:R",
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

std::string OwnerOnlySecretExposureHint(std::filesystem::path const& path, SecretExposure exposure)
{
    if (exposure == SecretExposure::None || exposure == SecretExposure::Undetermined)
        return SecretExposureHint(path, exposure);
#if defined(_WIN32)
    // `OwnerOnlySecretFileDacl` in icacls's grammar, raw SIDs for `SecretExposureHint`'s reason:
    // inheritance cut, `BroadPrincipals` removed in the order they are declared, then SYSTEM,
    // Administrators and OWNER RIGHTS granted in place of whatever they held.
    auto const command = std::format(R"(icacls "{}" /inheritance:r /remove:g *S-1-1-0 *S-1-5-11 *S-1-5-32-545 )"
                                     R"(*S-1-5-4 *S-1-5-32-546 /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-3-4:F)",
                                     path.string());
#else
    auto const command = std::format("chmod go-rwx {}", path.string());
#endif
    return std::format("{} can be read by accounts other than its owner, so the secret in it is not protected; "
                       "restrict it to its owner with: {}",
                       path.string(),
                       command);
}

bool SecureDirectoryForAdministrators(std::filesystem::path const& directory)
{
#if defined(_WIN32)
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

    if (ApplyProtectedDacl(directory, AdministratorOnlyDacl, administrators.data()) != ERROR_SUCCESS)
        return false;
#else
    // Ownership is not something chmod can fix, and it does not need fixing:
    // only root can create the machine-wide config directory. What is left is
    // the umask, which can leave a fresh directory group-writable.
    if (!RemovePermissions(directory, std::filesystem::perms::group_write | std::filesystem::perms::others_write))
        return false;
#endif

    // Report the property, not the syscall. What the caller needs to know is
    // whether the directory *is* administrator-only afterwards, and on POSIX a
    // successful chmod does not establish that — a non-root caller ends up with
    // a tidy directory it still owns. Asking the same question the startup
    // check asks is also what keeps the two from ever disagreeing.
    return IsAdministrativeContainer(directory);
}

bool SecureSecretFileForServices(std::filesystem::path const& file)
{
#if defined(_WIN32)
    // No owner, unlike the directory. An owner keeps WRITE_DAC, so the directory
    // has to name one or a standard account that created it can re-open it — but
    // this file's directory has just been established as administrator-only
    // writable, so only an administrator can have created what is in it, and
    // `IsAdministratorOnlyWritable` deliberately does not test a file's owner for
    // exactly that reason. Setting it here would be a second thing that can fail
    // for no property gained.
    if (ApplyProtectedDacl(file, SecretFileDacl, nullptr) != ERROR_SUCCESS)
        return false;
#else
    // Group as well as other. A group grant is only safe where an administrator
    // chose the group -- which is a packaging decision (the macOS postinstall
    // chowns to `_fastcached` and chmods 0640) and not something this call can
    // guess a name for, so it hands back a file with no delegation at all and
    // lets the package add one.
    if (!RemovePermissions(file, std::filesystem::perms::group_all | std::filesystem::perms::others_all))
        return false;
#endif

    // The property, not the syscall -- the same rule SecureDirectoryForAdministrators
    // reports by, and asked through the very predicate that would otherwise warn
    // about this file at startup, so the two can never disagree about what
    // "secured" means. `Undetermined` fails here, deliberately: a list that would
    // not be read back is not one this claimed to have set.
    return SecretFileExposure(file) == SecretExposure::None;
}

SecretExposure SecureSecretFileForOwner(std::filesystem::path const& file)
{
    // Whether the apply call succeeded is deliberately not the answer: a filesystem
    // with no permissions refuses the call AND reads back `Undetermined`, which the
    // caller decides on, while a call that "succeeded" on a list somebody else then
    // widened is caught only by asking again.
#if defined(_WIN32)
    (void) ApplyProtectedDacl(file, OwnerOnlySecretFileDacl, nullptr);
#else
    (void) RemovePermissions(file, std::filesystem::perms::group_all | std::filesystem::perms::others_all);
#endif
    return SecretFileExposure(file);
}

#if defined(_WIN32)
namespace
{
    /// Create @p file NEW, unshared, with @p attributes' descriptor or -- null -- its directory's.
    /// The one create both public functions share, so they differ in the list and nothing else.
    [[nodiscard]] std::expected<SecretFileStream, std::error_code> CreateNewUnshared(std::filesystem::path const& file,
                                                                                     SECURITY_ATTRIBUTES* attributes)
    {
        // Share mode 0: no other open of this file succeeds while this handle lives, so nothing
        // can be holding one when the contents arrive -- a list alone would refuse only later
        // opens. `CREATE_NEW` is the exclusive create, with nothing in front of it.
        HANDLE const handle =
            ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };

        auto const descriptorNumber = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_WRONLY | _O_BINARY);
        if (descriptorNumber == -1)
        {
            auto const error = errno;
            ::CloseHandle(handle);
            return std::unexpected { std::error_code { error, std::generic_category() } };
        }
        auto* const stream = ::_fdopen(descriptorNumber, "wb");
        if (stream == nullptr)
        {
            auto const error = errno;
            ::_close(descriptorNumber);
            return std::unexpected { std::error_code { error, std::generic_category() } };
        }
        return SecretFileStream { stream, &std::fclose };
    }
} // namespace
#else
namespace
{
    /// Create @p file NEW with EXACTLY @p mode, never through a link.
    ///
    /// The create's own mode is narrowed by the umask and never widened, so it is created owner-only
    /// and then set to @p mode on the descriptor: a permissive umask cannot widen it and a strict one
    /// cannot narrow it. A mode that cannot be set removes the empty file rather than leaving one
    /// with the umask's.
    [[nodiscard]] std::expected<SecretFileStream, std::error_code> CreateNewUnshared(std::filesystem::path const& file,
                                                                                     ::mode_t mode)
    {
        auto const descriptorNumber =
            ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (descriptorNumber < 0)
            return std::unexpected { std::error_code { errno, std::generic_category() } };
        if (::fchmod(descriptorNumber, mode) != 0)
        {
            auto const error = errno;
            ::close(descriptorNumber);
            ::unlink(file.c_str());
            return std::unexpected { std::error_code { error, std::generic_category() } };
        }
        auto* const stream = ::fdopen(descriptorNumber, "wb");
        if (stream == nullptr)
        {
            auto const error = errno;
            ::close(descriptorNumber);
            return std::unexpected { std::error_code { error, std::generic_category() } };
        }
        return SecretFileStream { stream, &std::fclose };
    }
} // namespace
#endif

std::expected<SecretFileStream, std::error_code> CreateStateFile(std::filesystem::path const& file, StateFile which)
{
#if defined(_WIN32)
    if (StateFileAccessOf(which) == StateFileAccess::OthersRead)
        return CreateNewUnshared(file, nullptr);
    auto const descriptor = DescriptorOf(OwnerOnlySecretFileDacl);
    if (!descriptor.has_value())
        return std::unexpected { descriptor.error() };
    auto attributes = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                            .lpSecurityDescriptor = descriptor->get(),
                                            .bInheritHandle = FALSE };
    return CreateNewUnshared(file, &attributes);
#else
    return CreateNewUnshared(file, static_cast<::mode_t>(StateFilePosixMode(which)));
#endif
}

FileOwner FileOwnerOf(std::filesystem::path const& path)
{
#if defined(_WIN32)
    // Opened as ITSELF (`FILE_FLAG_OPEN_REPARSE_POINT`): `GetNamedSecurityInfoW` on a path follows
    // a link to its target. `BACKUP_SEMANTICS` so a directory opens too; READ_CONTROL alone reads
    // the owner, and every share mode so a file somebody holds open is still asked.
    HANDLE const handle = ::CreateFileW(path.c_str(),
                                        READ_CONTROL,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return FileOwner {};
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto const read = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
    ::CloseHandle(handle);
    if (read != ERROR_SUCCESS)
        return FileOwner {};
    auto const owned = LocalBlock { descriptor };
    if (owner == nullptr)
        return FileOwner {};

    auto name = AccountNameOf(owner);
    if (IsThisProcessUser(owner))
        return FileOwner { .standing = FileOwnerStanding::ThisProcess, .name = std::move(name) };
    if (MatchesWellKnownSid(owner, AdministrativeOwners))
        return FileOwner { .standing = FileOwnerStanding::Administrative, .name = std::move(name) };
    return FileOwner { .standing = FileOwnerStanding::Another, .name = std::move(name) };
#else
    struct ::stat info {};

    // `lstat`: the entry, never what a link points at.
    if (::lstat(path.c_str(), &info) != 0)
        return FileOwner {};
    auto name = std::format("uid {}", info.st_uid);
    if (info.st_uid == ::geteuid())
        return FileOwner { .standing = FileOwnerStanding::ThisProcess, .name = std::move(name) };
    if (info.st_uid == 0)
        return FileOwner { .standing = FileOwnerStanding::Administrative, .name = std::move(name) };
    return FileOwner { .standing = FileOwnerStanding::Another, .name = std::move(name) };
#endif
}

bool IsLinkEntry(std::filesystem::path const& path)
{
#if defined(_WIN32)
    auto const attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct ::stat info {};

    return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

DirectoryWriters DirectoryWritersOf(std::filesystem::path const& directory)
{
    // The owner first on both platforms: an owner can re-grant itself anything, so a list that
    // looks narrow says nothing about a directory somebody else owns.
    switch (FileOwnerOf(directory).standing)
    {
        case FileOwnerStanding::Undetermined:
        case FileOwnerStanding::Last:
            return DirectoryWriters::Undetermined;
        case FileOwnerStanding::Another:
            return DirectoryWriters::ForeignOwner;
        case FileOwnerStanding::ThisProcess:
        case FileOwnerStanding::Administrative:
            break;
    }
#if defined(_WIN32)
    auto const nobody = NoBroadPrincipalMay(directory, EntryPlantingRights);
    if (!nobody.has_value())
        return DirectoryWriters::Undetermined;
    return *nobody ? DirectoryWriters::OwnerOnly : DirectoryWriters::Others;
#else
    struct ::stat info {};

    if (::stat(directory.c_str(), &info) != 0)
        return DirectoryWriters::Undetermined;
    // A group or other write bit is others writing, the sticky bit NOT excepted: sticky stops
    // them removing or renaming an entry that is there, and not creating a name that is not --
    // a state file before its first write, or a temporary. `/tmp` is shared on purpose; a
    // directory a process keeps its own state in has no reason to be.
    auto const othersWrite = (info.st_mode & static_cast<::mode_t>(S_IWGRP | S_IWOTH)) != 0;
    return othersWrite ? DirectoryWriters::Others : DirectoryWriters::OwnerOnly;
#endif
}

std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& path)
{
#if defined(_WIN32)
    return NoBroadPrincipalMay(path, PlantingRights).transform([](bool nobody) { return !nobody; });
#else
    struct ::stat info {};

    if (::lstat(path.c_str(), &info) != 0)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
    return (info.st_mode & static_cast<::mode_t>(S_IWGRP | S_IWOTH)) != 0;
#endif
}

std::expected<SecretFileStream, RegularFileRefusal> OpenRegularFile(std::filesystem::path const& path)
{
#if defined(_WIN32)
    HANDLE const handle = ::CreateFileW(path.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        OPEN_EXISTING,
                                        // `BACKUP_SEMANTICS` so a DIRECTORY opens too, and is refused
                                        // below as what it is rather than as a denied open.
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        auto const error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return std::unexpected { RegularFileRefusal {
                .error = std::make_error_code(std::errc::no_such_file_or_directory), .notRegular = false } };
        return std::unexpected { RegularFileRefusal {
            .error = std::error_code { static_cast<int>(error), std::system_category() }, .notRegular = false } };
    }
    auto information = BY_HANDLE_FILE_INFORMATION {};
    auto const regular = ::GetFileType(handle) == FILE_TYPE_DISK
                         && ::GetFileInformationByHandle(handle, &information) != FALSE
                         && (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    if (!regular)
    {
        ::CloseHandle(handle);
        return std::unexpected { RegularFileRefusal { .error = std::make_error_code(std::errc::invalid_argument),
                                                      .notRegular = true } };
    }
    auto const descriptorNumber = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptorNumber == -1)
    {
        auto const error = errno;
        ::CloseHandle(handle);
        return std::unexpected { RegularFileRefusal { .error = std::error_code { error, std::generic_category() },
                                                      .notRegular = false } };
    }
    auto* const stream = ::_fdopen(descriptorNumber, "rb");
#else
    auto const descriptorNumber = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptorNumber < 0)
    {
        // `O_NOFOLLOW` answers a link with `ELOOP`: there IS an entry, and it is not a file.
        auto const error = errno;
        return std::unexpected { RegularFileRefusal { .error = std::error_code { error, std::generic_category() },
                                                      .notRegular = error == ELOOP } };
    }
    struct ::stat info {};

    if (::fstat(descriptorNumber, &info) != 0 || !S_ISREG(info.st_mode))
    {
        auto const error = errno;
        ::close(descriptorNumber);
        return std::unexpected { RegularFileRefusal { .error = S_ISREG(info.st_mode)
                                                                   ? std::error_code { error, std::generic_category() }
                                                                   : std::make_error_code(std::errc::invalid_argument),
                                                      .notRegular = !S_ISREG(info.st_mode) } };
    }
    auto* const stream = ::fdopen(descriptorNumber, "rb");
#endif
    if (stream == nullptr)
    {
        auto const error = errno;
#if defined(_WIN32)
        ::_close(descriptorNumber);
#else
        ::close(descriptorNumber);
#endif
        return std::unexpected { RegularFileRefusal { .error = std::error_code { error, std::generic_category() },
                                                      .notRegular = false } };
    }
    return SecretFileStream { stream, &std::fclose };
}

std::expected<void, std::error_code> CreateOwnerOnlyDirectory(std::filesystem::path const& directory)
{
    if (auto const parent = directory.parent_path(); !parent.empty())
    {
        auto failure = std::error_code {};
        std::filesystem::create_directories(parent, failure);
        if (failure)
            return std::unexpected { failure };
    }
#if defined(_WIN32)
    auto const descriptor = DescriptorOf(OwnerOnlyDirectoryDacl);
    if (!descriptor.has_value())
        return std::unexpected { descriptor.error() };
    auto attributes = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                            .lpSecurityDescriptor = descriptor->get(),
                                            .bInheritHandle = FALSE };
    if (::CreateDirectoryW(directory.c_str(), &attributes) == FALSE && ::GetLastError() != ERROR_ALREADY_EXISTS)
        return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };
#else
    if (::mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
#endif
    return {};
}

std::vector<std::string> OthersMayWriteRemedy(std::filesystem::path const& path)
{
#if defined(_WIN32)
    // Inheritance CONVERTED rather than cut, so every entry that is not a broad principal -- the
    // service account's grant among them -- is kept; then every principal `BroadPrincipals` scans
    // for is removed, in a command of its own (see `DirectoryWritersRemedy` for why one is not
    // enough).
    return { std::format(R"(icacls "{}" /inheritance:d)", path.string()),
             std::format(R"(icacls "{}" /remove:g *S-1-1-0 *S-1-5-11 *S-1-5-32-545 *S-1-5-4 *S-1-5-32-546)",
                         path.string()) };
#else
    return { std::format("chmod go-w '{}'", path.string()) };
#endif
}

std::vector<std::string> DirectoryWritersRemedy(std::filesystem::path const& directory, DirectoryWriters writers)
{
    if (writers != DirectoryWriters::Others)
        return {};
    return OthersMayWriteRemedy(directory);
}

std::string DirectoryWritersHint(std::filesystem::path const& directory, DirectoryWriters writers)
{
    switch (writers)
    {
        case DirectoryWriters::OwnerOnly:
        case DirectoryWriters::Last:
            return {};
        case DirectoryWriters::Undetermined:
            return std::format("could not determine who may create or delete entries in {}", directory.string());
        case DirectoryWriters::ForeignOwner:
            return std::format("{} is owned by an account that is neither this process's nor an administrator's, which "
                               "can grant itself anything in it: another account created it. Remove it, with what is "
                               "in it, and this node creates its own, its owner's alone",
                               directory.string());
        case DirectoryWriters::Others: {
            auto commands = std::string {};
            for (auto const& command: DirectoryWritersRemedy(directory, writers))
                commands += std::format("{}{}", commands.empty() ? "" : " and THEN, as a second command, ", command);
            return std::format("{} lets other accounts on this machine create or delete entries in it, so a file "
                               "this node trusts could have been put there by one of them; restrict it with: {}",
                               directory.string(),
                               commands);
        }
    }
    return {};
}

std::expected<void, std::string> SecureDirectoryForService(std::filesystem::path const& directory,
                                                           std::string const& account,
                                                           std::span<std::filesystem::path const> credentialLeaves)
{
    return Detail::SecureDirectoryForService(directory, account, credentialLeaves, {});
}

std::expected<void, std::string> Detail::SecureDirectoryForService(std::filesystem::path const& directory,
                                                                   std::string const& account,
                                                                   std::span<std::filesystem::path const> credentialLeaves,
                                                                   std::function<void()> const& afterPreCheck)
{
#if defined(_WIN32)
    // The remedy for a credential -- the identity key -- and for anything a planter owns:
    // deletion, then a fresh identity. Following an `icacls /reset` on a disclosed key would
    // leave that key in service; only re-minting it (which needs an ABSENT key file) undoes
    // the disclosure.
    auto const deleteRemedy = [](std::filesystem::path const& entry, std::string_view why) {
        return std::format("{} {}; delete it (or the whole directory) so the node mints a fresh identity, then "
                           "re-admit or re-enroll the node",
                           entry.string(),
                           why);
    };

    // A reparse point is refused before ANYTHING is written. `SetNamedSecurityInfoW` on a
    // junction writes the list onto the junction and leaves its target -- a directory the
    // planter owns and needed no privilege to point here -- untouched, so the service would
    // mint its key there under a report of success. The directory's OWNER is not checked here:
    // it may legitimately be whoever created it first, and the apply below sets it.
    if (auto const reparse = IsReparsePoint(directory); reparse != std::optional { false })
        return std::unexpected(
            reparse.has_value() ? deleteRemedy(directory,
                                               "is a reparse point (a junction or symlink), so a list applied to it "
                                               "secures nothing and the service's state could be redirected elsewhere")
                                : std::format("whether {} is a reparse point could not be determined", directory.string()));

    // The account's SID, resolved once BEFORE anything is applied -- a list whose service
    // entry could not be spelled would lock the service out of its own directory -- and kept
    // to judge who owns what is already inside.
    std::array<std::byte, SECURITY_MAX_SID_SIZE> serviceSidBuffer {};
    std::span<std::byte const> serviceSid;
    auto dacl = std::wstring { ServiceDirectoryDacl };
    if (!account.empty())
    {
        auto const entry = ServiceAccountEntry(account);
        if (!entry)
            return std::unexpected(entry.error());
        if (!ResolveAccountSid(account, serviceSidBuffer))
            return std::unexpected(std::format("the account '{}' did not resolve (error {})", account, ::GetLastError()));
        serviceSid = std::span<std::byte const> { serviceSidBuffer };
        dacl += *entry;
    }

    // The structure of one entry: not a reparse point, owned by SYSTEM/Administrators/the
    // service, and -- for a non-directory -- not a hard link to a file elsewhere. Shared by
    // both passes, because the post-apply pass has to make exactly the pre-apply checks again.
    auto const refuseEntryStructure = [&](std::filesystem::path const& entry) -> std::optional<std::string> {
        if (auto const reparse = IsReparsePoint(entry); reparse != std::optional { false })
            return reparse.has_value()
                       ? deleteRemedy(entry, "is a reparse point (a junction or symlink) inside the state directory")
                       : std::format("whether {} is a reparse point could not be determined", entry.string());

        // Owned by anyone but SYSTEM, Administrators or the service is a plant: its owner
        // keeps READ_CONTROL and, having created it, knows its contents, so a key adopted
        // from it is a key that account holds.
        if (auto const owned = IsOwnedByServiceOrAdministrator(entry, serviceSid); owned != std::optional { true })
            return owned.has_value()
                       ? deleteRemedy(entry, "is owned by an account other than the system, administrators or the service")
                       : std::format("who owns {} could not be determined", entry.string());

        // A hard link shares its target's security descriptor, so a key hard-linked in from an
        // admin-owned file outside passes reparse and owner -- and the apply rewrites the
        // outside file. A directory has no hard links to count (`nNumberOfLinks` counts its
        // subdirectories), so this is asked of non-directories only.
        auto const attributes = ::GetFileAttributesW(entry.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            if (auto const linked = HasMultipleHardLinks(entry); linked != std::optional { false })
                return linked.has_value()
                           ? deleteRemedy(entry,
                                          "is a hard link to a file outside the state directory, which shares "
                                          "its access list")
                           : std::format("how many links {} has could not be determined", entry.string());
        return std::nullopt;
    };

    // A read-only pre-pass over everything already inside, BEFORE the list is written: a
    // reparse point, a foreign-owned entry or a hard link is refused without touching the tree,
    // because the remedy is deletion and applying a list first would be a change to a structure
    // about to be thrown away. The directory's own reparse was checked above; its owner is not,
    // because the apply sets it.
    std::error_code ec;
    auto prePass = std::filesystem::recursive_directory_iterator { directory, ec };
    while (!ec && prePass != std::filesystem::recursive_directory_iterator {})
    {
        if (auto const denial = refuseEntryStructure(prePass->path()))
            return std::unexpected(*denial);
        prePass.increment(ec);
    }
    if (ec)
        return std::unexpected(std::format("what it holds could not be listed: {}", ec.message()));

    // The race window this pass and the post-apply pass exist to close. Empty in production;
    // a test mutates the tree here to prove the second pass catches what the first could not.
    if (afterPreCheck)
        afterPreCheck();

    // The OWNER is set to Administrators, not only the list: an owner keeps `WRITE_DAC`
    // whatever the entries say -- the OWNER RIGHTS entry holds a NAMED owner to reading, but
    // the directory may currently be owned by whoever created it first, and setting the list
    // without the owner would leave that account able to undo it. `AdministrativeOwners` and
    // `SecureDirectoryHint` require the same. This is why the whole call needs the privileges
    // an install has: setting an object's owner to a group is not a right a standard account
    // holds.
    std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators {};
    auto administratorsSize = static_cast<DWORD>(administrators.size());
    if (::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &administratorsSize) == FALSE)
        return std::unexpected("the Administrators SID could not be built");

    // What is already inside is covered by the same call, and that is inheritance doing its
    // job rather than a hope: setting a directory's list recomputes the INHERITED entries of
    // everything under it. A key the service minted under the old list was created with
    // default security, so every entry it carries is inherited -- the upgrade from today's
    // MSI -- and after this it carries the new ones instead.
    if (auto const rc = ApplyProtectedDacl(directory, dacl.c_str(), administrators.data()); rc != ERROR_SUCCESS)
        return std::unexpected(std::format("its access list could not be replaced (error {})", rc));

    // The property, not the syscall. Windows lets every account bypass traverse checking, so
    // a closed directory does not hide a file whose own list lets anybody read it.
    if (NoBroadPrincipalMay(directory, ReadingRights | PlantingRights) != std::optional { true })
        return std::unexpected("it still lets a broad principal list or add to it, or would not say");
    if (DaclIsProtected(directory) != std::optional { true })
        return std::unexpected("its access list is still not protected from inheritance, or would not say");

    auto const isCredential = [credentialLeaves](std::filesystem::path const& entry) {
        return std::ranges::any_of(credentialLeaves,
                                   [&entry](std::filesystem::path const& leaf) { return entry.filename() == leaf; });
    };

    // The FULL check again: between the pre-pass and the apply the planter still owned the
    // directory and could add a child with a non-broad ACE, or rename the directory away and
    // drop a junction in its place. Once the protected list and the Administrators owner are
    // on, no NEW open can create or change anything here, so this pass sees the tree's final
    // shape. One residual it cannot close, stated rather than implied away: an access check is
    // made when a handle is OPENED, so a handle opened before the apply keeps the access it was
    // granted -- a file that was broadly writable until now can still be written through one.
    // Refusing to proceed while any other handle is open would refuse every re-apply with the
    // service running, since it holds its own key open. The directory's own reparse and owner
    // are re-checked first.
    if (auto const denial = refuseEntryStructure(directory))
        return std::unexpected(*denial);

    auto postPass = std::filesystem::recursive_directory_iterator { directory, ec };
    while (!ec && postPass != std::filesystem::recursive_directory_iterator {})
    {
        auto const entry = postPass->path();
        if (auto const denial = refuseEntryStructure(entry))
            return std::unexpected(*denial);

        // An explicit broad grant of its own survives the recomputation above, so this is asked
        // AFTER the apply -- an inherited grant is cured by it, a NAMED one is not. A CREDENTIAL
        // gets the delete remedy -- a disclosed key stays disclosed after an `icacls /reset` --
        // while any other state file is told to inherit the directory's list.
        if (auto const exposure = SecretFileExposure(entry); exposure != SecretExposure::None)
        {
            if (exposure == SecretExposure::Undetermined)
                return std::unexpected(std::format("who may read {} could not be determined", entry.string()));
            if (isCredential(entry))
                return std::unexpected(deleteRemedy(
                    entry, "is a credential this node minted and it is readable by other accounts on this machine"));
            return std::unexpected(
                std::format("{0} keeps an access list of its own that lets every account on this machine read it; "
                            "make it inherit the directory's with: icacls \"{0}\" /reset",
                            entry.string()));
        }
        postPass.increment(ec);
    }
    if (ec)
        return std::unexpected(std::format("what it holds could not be listed: {}", ec.message()));
    return {};
#else
    (void) credentialLeaves;
    (void) afterPreCheck;
    // The account is the directory's owner by now -- the caller hands the path over
    // first -- so the owner's bits are its bits. Nothing inside needs visiting: POSIX
    // has no traverse bypass, so a directory nobody else may search hides every file
    // in it whatever that file's own mode says.
    (void) account;
    if (!RemovePermissions(directory, std::filesystem::perms::group_all | std::filesystem::perms::others_all))
        return std::unexpected(std::string { "its mode could not be changed" });

    std::error_code ec;
    auto const left = std::filesystem::status(directory, ec).permissions();
    if (ec)
        return std::unexpected(std::format("its mode could not be read back: {}", ec.message()));
    if ((left & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) != std::filesystem::perms::none)
        return std::unexpected(std::string { "its group and other bits are still set" });
    return {};
#endif
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
