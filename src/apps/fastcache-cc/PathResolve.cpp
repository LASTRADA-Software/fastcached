// SPDX-License-Identifier: Apache-2.0
#include "PathResolve.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache::Cc
{

namespace
{

#if defined(_WIN32)

    /// Owns a Win32 handle for the duration of one resolution step.
    class ScopedHandle
    {
      public:
        explicit ScopedHandle(HANDLE handle) noexcept:
            _handle { handle }
        {
        }

        ~ScopedHandle()
        {
            if (_handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(_handle);
        }

        ScopedHandle(ScopedHandle const&) = delete;
        ScopedHandle& operator=(ScopedHandle const&) = delete;
        ScopedHandle(ScopedHandle&&) = delete;
        ScopedHandle& operator=(ScopedHandle&&) = delete;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return _handle != INVALID_HANDLE_VALUE;
        }
        [[nodiscard]] HANDLE Get() const noexcept
        {
            return _handle;
        }

      private:
        HANDLE _handle;
    };

    /// Remove the extended-length prefix `GetFinalPathNameByHandleW` returns.
    ///
    /// `\\?\` disables path parsing for the API that receives it, so a path
    /// carrying it is not interchangeable with one that does not — and every
    /// consumer here is a string prefix comparison against a root the build system
    /// spelled without it. The UNC form is its own row because the prefix is not
    /// simply dropped there: `\\?\UNC\host\share` is `\\host\share`.
    /// @param text A path as the API returned it.
    /// @return The ordinary form.
    [[nodiscard]] std::string StripExtendedPrefix(std::string text)
    {
        constexpr std::string_view UncPrefix = R"(\\?\UNC\)";
        constexpr std::string_view DosPrefix = R"(\\?\)";
        if (text.starts_with(UncPrefix))
            return R"(\\)" + text.substr(UncPrefix.size());
        if (text.starts_with(DosPrefix))
            return text.substr(DosPrefix.size());
        return text;
    }

    /// Call a `(buffer, length) -> needed-length` Win32 path API, sizing the
    /// buffer from its own answer. Both APIs used below share that protocol, so
    /// the two-call dance is written once.
    /// @param call The API, invoked as `call(buffer, size)`.
    /// @return The path it produced, or empty on any failure.
    template <class Call>
    [[nodiscard]] std::string CallPathApi(Call const& call)
    {
        auto const needed = call(nullptr, 0U);
        if (needed == 0)
            return {};
        std::wstring buffer(needed, L'\0');
        auto const written = call(buffer.data(), needed);
        // A second call that needs MORE room lost a race with a rename; refuse it
        // rather than return a truncated path, which would be a wrong prefix test.
        if (written == 0 || written >= needed)
            return {};
        buffer.resize(written);
        return StripExtendedPrefix(std::filesystem::path { buffer }.string());
    }

    /// Resolve through an open handle: normalizes 8.3 short components, symlinks,
    /// junctions, `subst` drives and case in one call. The strongest step, and the
    /// only one that answers the `subst`/junction half of issue #66.
    /// @param path The path to resolve.
    /// @return The resolved path, or empty when no handle could be opened.
    [[nodiscard]] std::string ByFinalName(std::filesystem::path const& path)
    {
        // FILE_FLAG_BACKUP_SEMANTICS is what makes this work for a DIRECTORY,
        // which is the case that matters most here — the memo resolves parents.
        // Sharing everything, because a header the compiler still holds open must
        // not fail to resolve.
        ScopedHandle const handle { ::CreateFileW(path.wstring().c_str(),
                                                  0,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                  nullptr,
                                                  OPEN_EXISTING,
                                                  FILE_FLAG_BACKUP_SEMANTICS,
                                                  nullptr) };
        if (!handle.IsValid())
            return {};
        return CallPathApi([&handle](wchar_t* buffer, DWORD size) {
            return ::GetFinalPathNameByHandleW(handle.Get(), buffer, size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        });
    }

    /// Expand 8.3 short components without opening a handle.
    ///
    /// Kept as its own row because it succeeds where the step above cannot: a file
    /// opened for exclusive access refuses a second handle, and a short-named path
    /// that fails to expand is precisely the silent misconfiguration this module
    /// exists to end.
    /// @param path The path to resolve.
    /// @return The long form, or empty on failure.
    [[nodiscard]] std::string ByLongName(std::filesystem::path const& path)
    {
        auto const wide = path.wstring();
        return CallPathApi([&wide](wchar_t* buffer, DWORD size) { return ::GetLongPathNameW(wide.c_str(), buffer, size); });
    }

#else

    /// Resolve through the filesystem: follows symlinks and removes `.`/`..`.
    /// @param path The path to resolve.
    /// @return The canonical path, or empty when it does not exist.
    [[nodiscard]] std::string ByCanonical(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto resolved = std::filesystem::canonical(path, ec);
        // The error_code overload throughout: a launcher whose whole contract is
        // that a cache problem never breaks a build must not throw on a stat.
        return ec ? std::string {} : resolved.string();
    }

#endif

    /// Last resort: lexical normalization against the current directory, with no
    /// requirement that the path exist. Resolves nothing about spelling, so it
    /// leaves a mismatched root exactly as mismatched as it was — which is the
    /// correct outcome, since the alternative is to resolve one side only.
    /// @param path The path to resolve.
    /// @return The normalized path, or empty on failure.
    [[nodiscard]] std::string ByWeaklyCanonical(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto resolved = std::filesystem::weakly_canonical(path, ec);
        return ec ? std::string {} : resolved.make_preferred().string();
    }

    /// One way of asking the filesystem what a path really is. An empty result
    /// means "I could not answer; try the next row".
    using ResolveStep = std::string (*)(std::filesystem::path const&);

    /// The steps, strongest first; the first non-empty answer wins. Adding a
    /// reconciliation rule is adding a row, and the order is the whole policy:
    /// a weaker step must never pre-empt a stronger one, or a `subst` drive would
    /// be normalized to itself and reported as resolved.
#if defined(_WIN32)
    constexpr std::array<ResolveStep, 3> ResolveSteps {
        &ByFinalName,
        &ByLongName,
        &ByWeaklyCanonical,
    };
#else
    constexpr std::array<ResolveStep, 2> ResolveSteps {
        &ByCanonical,
        &ByWeaklyCanonical,
    };
#endif

    /// The real resolver: the step table above, memoized per directory.
    ///
    /// Not thread-safe, and it does not need to be — one launcher process handles
    /// exactly one compile, on one thread.
    class MemoizingPathResolver final: public IPathResolver
    {
      public:
        // Both entry points are thin guards over the real work, and the guard has
        // to sit HERE rather than around the filesystem calls inside.
        //
        // "Never throws, worst case the input comes back" is the contract every
        // caller relies on — a launcher whose whole promise is that a cache problem
        // cannot break a build, called from a main() with no catch of its own. The
        // std::filesystem calls all take an error_code, so it is tempting to guard
        // only the step table; but on Windows std::filesystem::path STORES a
        // wstring, so merely constructing one from a narrow string converts through
        // the active code page, and path::string() converts back. Either direction
        // throws on a character the code page cannot represent. Those conversions
        // happen on the first line of each function, before any step runs, and in
        // parent.string() between them — outside a guard placed further in.

        std::string Resolve(std::string_view path) override
        {
            try
            {
                return ResolveFile(path);
            }
            catch (std::exception const&)
            {
                return std::string { path };
            }
        }

        std::string ResolveDirectory(std::string_view path) override
        {
            try
            {
                return ResolveWhole(path);
            }
            catch (std::exception const&)
            {
                return std::string { path };
            }
        }

        [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
        {
            return _filesystemCalls;
        }

      private:
        /// Resolve a path naming a FILE: the parent through the memo, the leaf
        /// appended as spelled. May throw; Resolve() is the guard.
        /// @param path An absolute file path.
        /// @return The resolved spelling, or `path` when no answer is available.
        std::string ResolveFile(std::string_view path)
        {
            std::filesystem::path const input { path };
            if (!IsResolvable(input))
                return std::string { path };

            auto const parent = input.parent_path();
            auto const filename = input.filename();
            // Nothing to split (a bare root, or a trailing separator): resolve the
            // whole thing, which is also what a directory wants.
            if (parent.empty() || filename.empty() || parent == input)
                return ResolveWhole(path);

            auto const resolvedParent = ResolveWhole(parent.string());
            if (resolvedParent.empty())
                return std::string { path };

            // Appended rather than resolved: see MakePathResolver's contract for
            // why the leaf is left as the `#include` directive spelled it.
            auto joined = std::filesystem::path { resolvedParent } / filename;
            return joined.make_preferred().string();
        }

        /// Resolve a whole path through the step table, memoized. May throw;
        /// ResolveDirectory() is the guard.
        /// @param path An absolute path.
        /// @return The resolved spelling, or `path` when no step could answer.
        std::string ResolveWhole(std::string_view path)
        {
            std::filesystem::path const input { path };
            if (!IsResolvable(input))
                return std::string { path };

            // Keyed on the spelling as given, not on a folded form. Two spellings
            // of one directory then cost two entries and two filesystem calls but
            // resolve to the same answer, which is the safe direction — a folded
            // key would have to reproduce the very comparison rule whose
            // insufficiency is the bug being fixed here.
            std::string key { path };
            if (auto const found = _memo.find(key); found != _memo.end())
                return found->second;

            std::string resolved;
            for (ResolveStep const step: ResolveSteps)
            {
                ++_filesystemCalls;
                resolved = step(input);
                if (!resolved.empty())
                    break;
            }
            if (resolved.empty())
                resolved = std::string { path };

            return _memo.emplace(std::move(key), std::move(resolved)).first->second;
        }

        /// Whether asking the filesystem about this path can help.
        ///
        /// A relative path is returned verbatim by contract, and an empty one has
        /// nothing to resolve. Both tests live here so the two entry points cannot
        /// disagree about them.
        /// @param path The candidate.
        /// @return True when the step table should be run.
        [[nodiscard]] static bool IsResolvable(std::filesystem::path const& path)
        {
            return !path.empty() && path.is_absolute();
        }

        std::unordered_map<std::string, std::string> _memo;
        std::size_t _filesystemCalls { 0 };
    };

} // namespace

std::unique_ptr<IPathResolver> MakePathResolver()
{
    return std::make_unique<MemoizingPathResolver>();
}

} // namespace FastCache::Cc
