// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>

    #include <fcntl.h>
    #include <io.h>
    #include <share.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Consensus
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

gsl::owner<std::FILE*> OpenBinary(std::filesystem::path const& path, char const* mode)
{
#if defined(_WIN32)
    auto wide = std::wstring {};
    for (char const symbol: std::string_view { mode })
        wide.push_back(static_cast<wchar_t>(symbol));

    // `_wfsopen` with `_SH_DENYNO` is `_wfopen` exactly -- the same share mode -- without the
    // deprecation. `_wfopen_s`, the replacement the deprecation names, opens the file
    // non-shareable for writing, which would change who else may open the file while it is held.
    return ::_wfsopen(path.wstring().c_str(), wide.c_str(), _SH_DENYNO);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::expected<ReadStream, std::error_code> OpenForReading(std::filesystem::path const& path)
{
#if defined(_WIN32)
    // Delete sharing is what lets a replace rename over this file while it is open: `_wfopen` shares
    // read and write and never delete.
    auto* const handle = ::CreateFileW(path.wstring().c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::unexpected { std::error_code { static_cast<int>(::GetLastError()), std::system_category() } };
    auto const descriptor = ::_open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1)
    {
        auto const failure = std::error_code { errno, std::generic_category() };
        ::CloseHandle(handle);
        return std::unexpected { failure };
    }
    // From here the descriptor owns the handle, and the stream the descriptor.
    auto stream = ReadStream { ::_fdopen(descriptor, "rb"), &std::fclose };
    if (!stream)
    {
        auto const failure = std::error_code { errno, std::generic_category() };
        ::_close(descriptor);
        return std::unexpected { failure };
    }
    return stream;
#else
    errno = 0;
    auto stream = ReadStream { std::fopen(path.c_str(), "rb"), &std::fclose };
    if (!stream)
        return std::unexpected { std::error_code { errno, std::generic_category() } };
    return stream;
#endif
}

bool FlushToDisk(std::FILE* file) noexcept
{
    if (std::fflush(file) != 0)
        return false;

#if defined(_WIN32)
    return ::_commit(::_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

std::expected<std::optional<std::vector<std::byte>>, ConsensusError> ReadFileIfPresent(std::filesystem::path const& path)
{
    auto error = std::error_code {};
    auto const present = std::filesystem::exists(path, error);
    if (error)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot stat {}: {}", path.string(), error.message())) };

    // A file that is not there is not a failure, and not an empty file either: which of
    // the two it is belongs to the caller, who knows what an absent file means.
    if (!present)
        return std::optional<std::vector<std::byte>> {};

    auto const size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot size {}: {}", path.string(), error.message())) };

    auto opened = OpenForReading(path);
    if (!opened.has_value())
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot open {}: {}", path.string(), opened.error().message())) };
    auto file = *std::move(opened);

    auto into = std::vector<std::byte>(static_cast<std::size_t>(size));

    errno = 0;
    auto const read = into.empty() ? std::size_t { 0 } : std::fread(into.data(), 1, into.size(), file.get());
    auto const failure = errno;
    auto const truncated = std::ferror(file.get()) != 0 || read != into.size();
    file.reset();

    if (truncated)
    {
        // A short read with no errno is a file that shrank between the size
        // call and the read, which is a different fault from an I/O error and
        // is worth saying so rather than reporting errno 0 as a cause.
        return std::unexpected { FastCache::StorageFailure(
            failure != 0 ? std::format("cannot read {}: {}", path.string(), std::generic_category().message(failure))
                         : std::format("{} is shorter than its reported size of {} bytes", path.string(), size)) };
    }

    return std::optional { std::move(into) };
}

std::expected<ReplacedBy, ConsensusError> ReplaceFileWith(std::filesystem::path const& path,
                                                          std::span<std::byte const> body,
                                                          StateFile which,
                                                          IReplacingRename const& rename)
{
    auto const temporary = std::filesystem::path { path }.concat(ReplacementSuffix);

    // Created exclusively and unshared, so no handle another account opened on a temporary it
    // could reach keeps writing to the file after the rename -- and with the access its row
    // gives it (`CreateStateFile`): integrity, not secrecy, for every file written this way, so
    // on Windows the directory's list, which carries a service account's grant when an elevated
    // operator wrote it, and on POSIX exactly the row's mode. A temporary a crash left behind is
    // cleared first -- it holds nothing anybody trusts.
    auto stale = std::error_code {};
    std::filesystem::remove(temporary, stale);
    auto created = CreateStateFile(temporary, which);
    if (!created.has_value())
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot open {}: {}", temporary.string(), created.error().message())) };
    auto stream = *std::move(created);

    errno = 0;
    auto const wrote = body.empty() || std::fwrite(body.data(), 1, body.size(), stream.get()) == body.size();
    auto const flushed = wrote && FlushToDisk(stream.get());
    auto const failure = errno;
    stream.reset();

    if (!flushed)
    {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot write {}: {}", temporary.string(), std::generic_category().message(failure))) };
    }

    auto replaced = ReplacedBy {};
    auto error = rename.RenameReplacing(temporary, path);
    if (MeansNoPosixRename(error))
    {
        replaced = ReplacedBy { .route = ReplaceRoute::Classic, .posixRefusal = error };
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error)
    {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot replace {}: {}", path.string(), error.message())) };
    }
    return replaced;
}

std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path,
                                                          std::span<std::byte const> body,
                                                          StateFile which)
{
    auto const rename = SystemReplacingRename {};
    return ReplaceFileWith(path, body, which, rename).transform([](ReplacedBy const&) {});
}

std::expected<ReplacedBy, ConsensusError> ProbeReplaceRoute(std::filesystem::path const& directory,
                                                            IReplacingRename const& rename)
{
    auto const probe = directory / ReplaceProbeFileName;
    // Twice: the first may CREATE the file, and only a rename over one that exists is the replace
    // every state file there goes through.
    auto const first = ReplaceFileWith(probe, std::span<std::byte const> {}, StateFile::Formation, rename);
    auto const second = first.and_then([&](ReplacedBy const&) {
        return ReplaceFileWith(probe, std::span<std::byte const> {}, StateFile::Formation, rename);
    });
    auto discard = std::error_code {};
    std::filesystem::remove(probe, discard);
    return second;
}

} // namespace FastCache::Consensus
