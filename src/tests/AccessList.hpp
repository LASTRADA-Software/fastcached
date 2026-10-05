// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file AccessList.hpp
/// Set and read a Windows access list the way an operator's `icacls` would, for the cases that
/// assert what `Platform/FileTrust` does to a real one.
///
/// Shared rather than written per file, because two cases asserting one list must spell the
/// setting of it one way: a copy that forgot `PROTECTED_DACL_SECURITY_INFORMATION` would let the
/// parent's entries flow in and make the "broad" list the case starts from narrower than it reads.
/// Windows only, and empty elsewhere: a POSIX case asserts mode bits instead.

#if defined(_WIN32)

    #include <cstddef>
    #include <filesystem>
    #include <memory>
    #include <optional>
    #include <string>
    #include <string_view>

    #include <windows.h>

    #include <aclapi.h>
    #include <sddl.h>

namespace FastCache::Testing
{

/// Frees what a security API allocated with `LocalAlloc`.
struct LocalFreeDeleter
{
    /// @param block What to free.
    void operator()(void* block) const noexcept
    {
        ::LocalFree(block);
    }
};

/// The owning handle for such a block.
using LocalBlock = std::unique_ptr<void, LocalFreeDeleter>;

/// Replace @p path's access list with @p sddl, protected against its parent's.
/// @param path An existing file or directory.
/// @param sddl The access list, in SDDL.
/// @return True when it was applied.
[[nodiscard]] inline bool ApplyAccessList(std::filesystem::path const& path, wchar_t const* sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr) == FALSE)
        return false;
    auto const owned = LocalBlock { descriptor };
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) == FALSE || present == FALSE)
        return false;
    auto name = path.wstring();
    return ::SetNamedSecurityInfoW(name.data(),
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr,
                                   nullptr,
                                   dacl,
                                   nullptr)
           == ERROR_SUCCESS;
}

/// @p path's access list, as SDDL.
///
/// Narrowed, so a failed comparison prints the list rather than a row of `{?}`: SDDL is
/// ASCII by grammar, SIDs and flags alike.
/// @param path An existing file or directory.
/// @return The list, or empty when it could not be read.
[[nodiscard]] inline std::string AccessListOf(std::filesystem::path const& path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(path.wstring().c_str(),
                                SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return {};
    auto const owned = LocalBlock { descriptor };
    wchar_t* text = nullptr;
    if (::ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr)
        == FALSE)
        return {};
    auto const ownedText = LocalBlock { text };
    auto narrow = std::string {};
    for (auto const character: std::wstring_view { text })
        narrow.push_back(static_cast<char>(character));
    return narrow;
}

/// How long `RunCommandLine` waits for the program it started.
inline constexpr DWORD CommandLineBoundMilliseconds = 60'000;

/// Run @p commandLine exactly as an operator would type it -- one program and its arguments, no
/// command processor -- and wait for it, BOUNDED.
///
/// For the remedies a refusal prints: a case that asserts only the TEXT of an `icacls` line cannot
/// tell a command that works from one that reports success and changes nothing, which is how the
/// one-command directory remedy failed on an inherited list.
/// @param commandLine The whole line, program first, as printed.
/// @return Its exit code, or nothing when it could not be started or did not end within
///         `CommandLineBoundMilliseconds` (it is then terminated).
[[nodiscard]] inline std::optional<DWORD> RunCommandLine(std::string_view commandLine)
{
    auto wide = std::wstring(static_cast<std::size_t>(::MultiByteToWideChar(
                                 CP_UTF8, 0, commandLine.data(), static_cast<int>(commandLine.size()), nullptr, 0)),
                             L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, commandLine.data(), static_cast<int>(commandLine.size()), wide.data(), static_cast<int>(wide.size()));
    auto startup = STARTUPINFOW {};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION {};
    if (::CreateProcessW(
            nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)
        == FALSE)
        return std::nullopt;
    ::CloseHandle(process.hThread);
    auto exitCode = std::optional<DWORD> {};
    if (::WaitForSingleObject(process.hProcess, CommandLineBoundMilliseconds) == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        if (::GetExitCodeProcess(process.hProcess, &code) != FALSE)
            exitCode = code;
    }
    else
        ::TerminateProcess(process.hProcess, 1);
    ::CloseHandle(process.hProcess);
    return exitCode;
}

} // namespace FastCache::Testing

#endif
