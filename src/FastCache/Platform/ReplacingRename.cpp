// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/ReplacingRename.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache::Platform
{

#if defined(_WIN32)
namespace
{
    /// What `SetFileInformationByHandle` answers on a filesystem that has no POSIX-semantics rename: the
    /// information class or its flags are not understood there, which says nothing about the files.
    constexpr auto NoPosixRename =
        std::to_array<DWORD>({ ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION });
} // namespace
#endif

bool MeansNoPosixRename(std::error_code refusal) noexcept
{
#if defined(_WIN32)
    return refusal.category() == std::system_category()
           && std::ranges::contains(NoPosixRename, static_cast<DWORD>(refusal.value()));
#else
    // `rename(2)` has the semantics already, so nothing it answers asks for another rename.
    static_cast<void>(refusal);
    return false;
#endif
}

std::error_code SystemReplacingRename::RenameReplacing(std::filesystem::path const& from,
                                                       std::filesystem::path const& to) const
{
#if defined(_WIN32)
    auto* const handle = ::CreateFileW(from.wstring().c_str(),
                                       DELETE | SYNCHRONIZE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::error_code { static_cast<int>(::GetLastError()), std::system_category() };

    // `FILE_RENAME_INFO` ends in the name, so it is laid out in storage of the whole size, held as
    // words so it is aligned for the structure.
    auto const name = to.wstring();
    auto const nameBytes = name.size() * sizeof(wchar_t);
    auto const size = sizeof(FILE_RENAME_INFO) + nameBytes;
    auto storage = std::vector<std::uint64_t>((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    // `Flags` shares a union with the older `ReplaceIfExists`, so it is written as bytes at its offset
    // rather than through the union; the storage starts zeroed, so `RootDirectory` is null.
    auto const flags = DWORD { FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS };
    std::memcpy(std::as_writable_bytes(std::span { storage }).subspan(offsetof(FILE_RENAME_INFO, Flags)).data(),
                &flags,
                sizeof flags);
    auto* const info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(&info->FileName[0], name.data(), nameBytes);

    auto const renamed = ::SetFileInformationByHandle(handle, FileRenameInfoEx, info, static_cast<DWORD>(size)) != 0;
    auto const failure = renamed ? DWORD { 0 } : ::GetLastError();
    ::CloseHandle(handle);
    if (renamed)
        return {};
    return std::error_code { static_cast<int>(failure), std::system_category() };
#else
    auto error = std::error_code {};
    std::filesystem::rename(from, to, error);
    return error;
#endif
}

std::expected<ReplacedBy, std::error_code> RenameIntoPlace(std::filesystem::path const& from,
                                                           std::filesystem::path const& to,
                                                           IReplacingRename const& rename)
{
    auto replaced = ReplacedBy {};
    auto error = rename.RenameReplacing(from, to);
    if (MeansNoPosixRename(error))
    {
        replaced = ReplacedBy { .route = ReplaceRoute::Classic, .posixRefusal = error };
        error.clear();
        std::filesystem::rename(from, to, error);
    }
    if (error)
        return std::unexpected { error };
    return replaced;
}

} // namespace FastCache::Platform
