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
#include <memory>
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
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastCache::Consensus
{

namespace
{
    /// A state file created by `CreateStateFile`, written through its stream.
    class SystemDurableSink final: public IDurableSink
    {
      public:
        /// @param stream The open temporary, owned from here.
        explicit SystemDurableSink(SecretFileStream stream) noexcept:
            _stream { std::move(stream) }
        {
        }

        std::error_code Write(std::span<std::byte const> bytes) override
        {
            errno = 0;
            if (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), _stream.get()) == bytes.size())
                return {};
            return Failure();
        }

        std::error_code Sync() override
        {
            errno = 0;
            return FlushToDisk(_stream.get()) ? std::error_code {} : Failure();
        }

        std::error_code Close() override
        {
            // Released rather than reset: `fclose`'s answer is the close this asks, and the stream's
            // own deleter would discard it.
            errno = 0;
            return std::fclose(_stream.release()) == 0 ? std::error_code {} : Failure();
        }

      private:
        /// @return What the C library said, or an I/O error when it set nothing.
        [[nodiscard]] static std::error_code Failure() noexcept
        {
            return std::error_code { errno != 0 ? errno : EIO, std::generic_category() };
        }

        SecretFileStream _stream;
    };
} // namespace

std::expected<std::unique_ptr<IDurableSink>, std::error_code> SystemDurableFiles::Create(std::filesystem::path const& path,
                                                                                         StateFile which)
{
    return CreateStateFile(path, which).transform([](SecretFileStream stream) -> std::unique_ptr<IDurableSink> {
        return std::make_unique<SystemDurableSink>(std::move(stream));
    });
}

std::expected<void, DirectorySyncFailure> SystemDurableFiles::SyncDirectory(std::filesystem::path const& directory)
{
    return SyncDirectoryToDisk(directory);
}

std::expected<void, DirectorySyncFailure> SyncDirectoryToDisk(std::filesystem::path const& directory)
{
    auto const refused = [](DirectorySyncStep step, std::error_code code) {
        return std::unexpected { DirectorySyncFailure { .step = step, .code = code } };
    };
#if defined(_WIN32)
    // FILE_WRITE_DATA is a directory's add-file right, which a replace into it already needs; read
    // access alone is refused the flush (measured, see the header).
    auto* const handle = ::CreateFileW(directory.wstring().c_str(),
                                       FILE_WRITE_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return refused(DirectorySyncStep::Open,
                       std::error_code { static_cast<int>(::GetLastError()), std::system_category() });
    auto const flushed = ::FlushFileBuffers(handle) != FALSE
                             ? std::error_code {}
                             : std::error_code { static_cast<int>(::GetLastError()), std::system_category() };
    ::CloseHandle(handle);
    if (flushed)
        return refused(DirectorySyncStep::Flush, flushed);
    return {};
#else
    auto const descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return refused(DirectorySyncStep::Open, std::error_code { errno, std::generic_category() });
    auto const synced = ::fsync(descriptor) == 0 ? std::error_code {} : std::error_code { errno, std::generic_category() };
    auto const closed = ::close(descriptor) == 0 ? std::error_code {} : std::error_code { errno, std::generic_category() };
    if (synced)
        return refused(DirectorySyncStep::Flush, synced);
    if (closed)
        return refused(DirectorySyncStep::Close, closed);
    return {};
#endif
}

std::span<std::error_code const> UnsupportedDirectorySyncAnswers()
{
#if defined(_WIN32)
    static auto const answers = std::to_array<std::error_code>({
        std::error_code { ERROR_INVALID_FUNCTION, std::system_category() },
        std::error_code { ERROR_NOT_SUPPORTED, std::system_category() },
        std::error_code { ERROR_INVALID_PARAMETER, std::system_category() },
    });
#else
    // `ENOTSUP` and `EOPNOTSUPP` are one value on Linux and two on some other systems; a duplicate
    // row costs nothing.
    static auto const answers = std::to_array<std::error_code>({
        std::error_code { EINVAL, std::generic_category() },
        std::error_code { ENOTSUP, std::generic_category() },
        std::error_code { EOPNOTSUPP, std::generic_category() },
        std::error_code { EBADF, std::generic_category() },
    });
#endif
    return answers;
}

bool MeansDirectorySyncUnsupported(DirectorySyncFailure const& failure) noexcept
{
    return failure.step == DirectorySyncStep::Flush
           && std::ranges::contains(UnsupportedDirectorySyncAnswers(), failure.code);
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

std::expected<Platform::ReplacedBy, ConsensusError> ReplaceFileWith(std::filesystem::path const& path,
                                                                    std::span<std::byte const> body,
                                                                    StateFile which,
                                                                    IDurableFiles& files,
                                                                    Platform::IReplacingRename const& rename)
{
    auto const temporary = std::filesystem::path { path }.concat(ReplacementSuffix);
    // Every way out but the rename removes the temporary, AFTER its sink is gone: Windows refuses to
    // delete a file that is still open.
    auto const abandon = [&temporary](std::string_view step, std::error_code failure) {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot {} {}: {}", step, temporary.string(), failure.message())) };
    };

    // Created exclusively and unshared, so no handle another account opened on a temporary it
    // could reach keeps writing to the file after the rename -- and with the access its row
    // gives it (`CreateStateFile`): integrity, not secrecy, for every file written this way, so
    // on Windows the directory's list, which carries a service account's grant when an elevated
    // operator wrote it, and on POSIX exactly the row's mode. A temporary a crash left behind is
    // cleared first -- it holds nothing anybody trusts.
    auto stale = std::error_code {};
    std::filesystem::remove(temporary, stale);
    auto created = files.Create(temporary, which);
    if (!created.has_value())
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot open {}: {}", temporary.string(), created.error().message())) };
    auto sink = *std::move(created);

    // In order, each only once the one before it held: a close is still asked after a failed write or
    // sync, to release the file, and its own answer is then not the one reported.
    auto failure = sink->Write(body);
    auto step = std::string_view { "write" };
    if (!failure)
    {
        failure = sink->Sync();
        step = "sync";
    }
    auto const closed = sink->Close();
    sink.reset();
    if (failure)
        return abandon(step, failure);
    if (closed)
        return abandon("close", closed);

    auto replaced = Platform::RenameIntoPlace(temporary, path, rename);
    if (!replaced.has_value())
    {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot replace {}: {}", path.string(), replaced.error().message())) };
    }

    // The rename changed the directory, and only a flush of the directory makes that survive a
    // power loss. Reported rather than swallowed: the new file is in place, but a caller told it is
    // durable could act on a write a power cut takes back -- a vote, for one. Except where the
    // FILESYSTEM cannot sync a directory at all: that is degraded and carried back, never refused, or
    // such a volume refuses every state write forever.
    auto const directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path { "." };
    auto const synced = files.SyncDirectory(directory);
    if (!synced.has_value() && MeansDirectorySyncUnsupported(synced.error()))
    {
        auto degraded = *replaced;
        degraded.directoryUnsynced = synced.error().code;
        return degraded;
    }
    if (!synced.has_value())
        return std::unexpected { FastCache::StorageFailure(
            std::format("replaced {}, but cannot sync its directory {}: {}; the replace is not known to survive a "
                        "power loss",
                        path.string(),
                        directory.string(),
                        synced.error().code.message())) };
    return *replaced;
}

std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path,
                                                          std::span<std::byte const> body,
                                                          StateFile which)
{
    auto files = SystemDurableFiles {};
    auto const rename = Platform::SystemReplacingRename {};
    return ReplaceFileWith(path, body, which, files, rename).transform([](Platform::ReplacedBy const&) {});
}

std::expected<Platform::ReplacedBy, ConsensusError> ProbeReplaceRoute(std::filesystem::path const& directory,
                                                                      IDurableFiles& files,
                                                                      Platform::IReplacingRename const& rename)
{
    auto const probe = directory / ReplaceProbeFileName;
    // Twice: the first may CREATE the file, and only a rename over one that exists is the replace
    // every state file there goes through.
    auto const first = ReplaceFileWith(probe, std::span<std::byte const> {}, StateFile::Formation, files, rename);
    auto const second = first.and_then([&](Platform::ReplacedBy const&) {
        return ReplaceFileWith(probe, std::span<std::byte const> {}, StateFile::Formation, files, rename);
    });
    auto discard = std::error_code {};
    std::filesystem::remove(probe, discard);
    return second;
}

} // namespace FastCache::Consensus
