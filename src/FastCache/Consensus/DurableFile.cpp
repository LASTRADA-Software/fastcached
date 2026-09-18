// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Consensus
{

gsl::owner<std::FILE*> OpenBinary(std::filesystem::path const& path, char const* mode)
{
#if defined(_WIN32)
    auto wide = std::wstring {};
    for (char const symbol: std::string_view { mode })
        wide.push_back(static_cast<wchar_t>(symbol));

    return ::_wfopen(path.wstring().c_str(), wide.c_str());
#else
    return std::fopen(path.c_str(), mode);
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

    errno = 0;
    gsl::owner<std::FILE*> const file = OpenBinary(path, "rb");
    if (file == nullptr)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot open {}: {}", path.string(), std::generic_category().message(errno))) };

    auto into = std::vector<std::byte>(static_cast<std::size_t>(size));

    errno = 0;
    auto const read = into.empty() ? std::size_t { 0 } : std::fread(into.data(), 1, into.size(), file);
    auto const failure = errno;
    auto const truncated = std::ferror(file) != 0 || read != into.size();
    (void) std::fclose(file);

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

std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path, std::span<std::byte const> body)
{
    auto const temporary = std::filesystem::path { path }.concat(".tmp");

    errno = 0;
    gsl::owner<std::FILE*> const file = OpenBinary(temporary, "wb");
    if (file == nullptr)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot open {}: {}", temporary.string(), std::generic_category().message(errno))) };

    errno = 0;
    auto const wrote = body.empty() || std::fwrite(body.data(), 1, body.size(), file) == body.size();
    auto const flushed = wrote && FlushToDisk(file);
    auto const failure = errno;
    (void) std::fclose(file);

    if (!flushed)
    {
        auto discard = std::error_code {};
        std::filesystem::remove(temporary, discard);
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot write {}: {}", temporary.string(), std::generic_category().message(failure))) };
    }

    auto error = std::error_code {};
    std::filesystem::rename(temporary, path, error);
    if (error)
        return std::unexpected { FastCache::StorageFailure(
            std::format("cannot replace {}: {}", path.string(), error.message())) };

    return {};
}

} // namespace FastCache::Consensus
