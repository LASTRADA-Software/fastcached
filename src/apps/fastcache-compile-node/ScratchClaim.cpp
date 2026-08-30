// SPDX-License-Identifier: Apache-2.0
#include "ScratchClaim.hpp"

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/file.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastCache::Node
{

namespace
{
    /// What a failure to take the lock means.
    enum class ClaimFailure : std::uint8_t
    {
        Contended, ///< Somebody else holds it. Try the next root.
        Fatal,     ///< This root cannot be claimed at all, and nor can any other.
    };

    /// The first line of a claim file: what wrote it, and in what format.
    ///
    /// Diagnostics only. **The lock decides ownership and this never does** -- on a
    /// root whose lock we hold, an unreadable or unknown-version record is stale
    /// data from a previous owner, to be overwritten rather than refused. That is
    /// `FleetHistory`'s rule ("no state of a history file may keep a node from
    /// starting") for a different file: making the version load-bearing for
    /// ownership would mean a version bump left every root unclaimable and no node
    /// able to start at all.
    ///
    /// Versioned all the same, so a mismatch is *detected and named* by anything
    /// reading it rather than silently misread as the current shape.
    constexpr std::string_view ClaimRecordMagic = "fastcache-scratch-claim";

    /// The record layout this build writes.
    constexpr int ClaimRecordVersion = 1;

    /// The claim file for the root at @p root.
    ///
    /// **Beside the directory, never inside it, and that is load-bearing twice.**
    /// Reclaiming empties the root, and so does the ordinary success path --
    /// `CompileJobRunner`'s `ScratchGuard` removes each job directory beneath it. A
    /// lock file inside would therefore be destroyed by normal operation, not merely
    /// by reclamation. Worse on POSIX: deleting an `flock`'d file is perfectly
    /// legal, and a second process may then create a FRESH file at the same path and
    /// lock that -- giving two live owners of one root, which is the exact condition
    /// this whole file exists to prevent.
    /// @param root The scratch root.
    /// @return The path of its claim file.
    [[nodiscard]] std::filesystem::path ClaimFileFor(std::filesystem::path const& root)
    {
        auto beside = root;
        beside += ".claim";
        return beside;
    }

    /// Empty @p root of everything a previous owner left, without removing it.
    /// @param root The directory to empty.
    /// @return True when anything was actually removed.
    [[nodiscard]] bool EmptyDirectory(std::filesystem::path const& root)
    {
        std::error_code ec;
        auto removed = false;
        for (auto iterator = std::filesystem::directory_iterator { root, ec };
             !ec && iterator != std::filesystem::directory_iterator {};
             iterator.increment(ec))
        {
            std::error_code inner;
            if (std::filesystem::remove_all(iterator->path(), inner) > 0)
                removed = true;
        }
        return removed;
    }

    /// One system error, and what it means for the claim.
    struct ClaimErrorRow
    {
        int systemError;      ///< `errno`, or `GetLastError()`.
        ClaimFailure failure; ///< What the caller should do about it.
    };

#if defined(_WIN32)
    /// `CreateFileW` refuses a second holder because of the share mode the first
    /// one passed; both spellings of that refusal reach us here.
    constexpr std::array ClaimErrorRows {
        ClaimErrorRow { .systemError = ERROR_SHARING_VIOLATION, .failure = ClaimFailure::Contended },
        ClaimErrorRow { .systemError = ERROR_LOCK_VIOLATION, .failure = ClaimFailure::Contended },
    };
#else
    /// EWOULDBLOCK and EAGAIN are one value on Linux and permitted to differ by
    /// POSIX; the duplicate row costs a comparison and keeps the table honest on a
    /// platform where they split.
    constexpr std::array ClaimErrorRows {
        ClaimErrorRow { .systemError = EWOULDBLOCK, .failure = ClaimFailure::Contended },
        ClaimErrorRow { .systemError = EAGAIN, .failure = ClaimFailure::Contended },
    };
#endif

    /// Classify the system error a failed claim produced.
    ///
    /// Anything the table does not name is `Fatal`, and on POSIX that INCLUDES
    /// `ENOLCK` and `EOPNOTSUPP` -- a filesystem that cannot lock. `FilePageStore`
    /// treats that case as "open unguarded and say so"; this one refuses, because an
    /// unclaimed scratch root is not a weakened guard but the defect itself.
    /// @param systemError POSIX `errno`, or Windows `GetLastError()`.
    /// @return What the caller should do about it.
    [[nodiscard]] ClaimFailure ClassifyClaimFailure(int systemError) noexcept
    {
        for (auto const& row: ClaimErrorRows)
            if (row.systemError == systemError)
                return row.failure;
        return ClaimFailure::Fatal;
    }

    /// A claim held by an OS handle, released when this object dies.
    class LockFileScratchClaim final: public IScratchClaim
    {
      public:
        // Spelled out rather than inherited. The destructor releases an OS handle, so
        // a copy would release it twice -- freeing a root this process is still
        // compiling into -- and a move would leave a released handle behind.
        LockFileScratchClaim(LockFileScratchClaim const&) = delete;
        LockFileScratchClaim(LockFileScratchClaim&&) = delete;
        LockFileScratchClaim& operator=(LockFileScratchClaim const&) = delete;
        LockFileScratchClaim& operator=(LockFileScratchClaim&&) = delete;

        /// @param root The claimed directory.
        /// @param reclaimed Whether it held a dead owner's leftovers.
        /// @param handle The OS handle carrying the claim.
#if defined(_WIN32)
        LockFileScratchClaim(std::filesystem::path root, bool reclaimed, HANDLE handle) noexcept:
            _root { std::move(root) },
            _reclaimed { reclaimed },
            _handle { handle }
        {
        }

        ~LockFileScratchClaim() override
        {
            if (_handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(_handle);
        }
#else
        LockFileScratchClaim(std::filesystem::path root, bool reclaimed, int descriptor) noexcept:
            _root { std::move(root) },
            _reclaimed { reclaimed },
            _descriptor { descriptor }
        {
        }

        ~LockFileScratchClaim() override
        {
            // Closing releases the `flock`, which is the whole release mechanism.
            if (_descriptor >= 0)
                ::close(_descriptor);
        }
#endif

        [[nodiscard]] std::filesystem::path const& Root() const noexcept override
        {
            return _root;
        }

        [[nodiscard]] bool Reclaimed() const noexcept override
        {
            return _reclaimed;
        }

      private:
        std::filesystem::path _root;
        bool _reclaimed { false };
#if defined(_WIN32)
        HANDLE _handle { INVALID_HANDLE_VALUE };
#else
        int _descriptor { -1 };
#endif
    };

    /// Take the OS-level claim on @p claimFile.
    /// @param claimFile The lock file beside the root.
    /// @return The held handle, or why it was not taken.
#if defined(_WIN32)
    [[nodiscard]] std::expected<HANDLE, ClaimFailure> TakeClaim(std::filesystem::path const& claimFile)
    {
        auto const path = claimFile.native();
        // `dwShareMode = 0` IS the claim: no other handle to this path can be opened
        // at all while this one lives, so there is no separate lock call and no
        // window in which the file is open but unclaimed. A null security attribute
        // makes the handle non-inheritable, which matters because this process
        // spawns compilers -- an inherited copy would keep the root claimed after
        // this process is gone. That is the same hazard `O_CLOEXEC` answers below,
        // and `FilePageStore` records it having been bitten by exactly this node.
        auto* handle = ::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::unexpected(ClassifyClaimFailure(static_cast<int>(::GetLastError())));

        // Prove the share mode is enforced rather than assuming it. A share mode is
        // honoured by the filesystem driver, and some network redirectors and
        // user-mode filesystems accept one and ignore it -- on those the claim above
        // is decoration and two nodes would share a root with no diagnostic at all.
        // Asking for a second handle asks exactly that question: if THIS process can
        // reopen the file it just claimed, so can another one.
        //
        // `OPEN_EXISTING` so the probe can only ever observe, and it is closed
        // immediately -- nothing is written through it. A successful probe is
        // `Fatal` and NOT a quiet carry-on: an unclaimed root is the defect.
        auto* const probe = ::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(probe);
            ::CloseHandle(handle);
            return std::unexpected(ClaimFailure::Fatal);
        }
        return handle;
    }
#else
    [[nodiscard]] std::expected<int, ClaimFailure> TakeClaim(std::filesystem::path const& claimFile)
    {
        // O_CLOEXEC is load-bearing because this process spawns compilers. A `flock`
        // lives on the open file DESCRIPTION and is released only when the LAST
        // descriptor referring to it closes, so an inherited copy in a child keeps
        // the root claimed after this node is gone -- and the next node then refuses
        // to start, blaming a second one that does not exist. `FilePageStore` records
        // having been bitten by precisely this, in precisely this program.
        constexpr int Mode = 0644;
        auto const descriptor = ::open(claimFile.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, Mode);
        if (descriptor < 0)
            return std::unexpected(ClaimFailure::Fatal);

        // `flock`, never `fcntl`. An fcntl lock is per PROCESS, so a second claimant
        // inside ONE process would take it again and report success -- which is not
        // merely a weaker guard but the in-process form of this very defect, the one
        // a22e056 fixed. It would also pass the two-runner model this fix was
        // reproduced with, and a guard its own test cannot fail is worse than none.
        while (::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
        {
            if (errno == EINTR)
                continue;
            auto const failure = ClassifyClaimFailure(errno);
            ::close(descriptor);
            return std::unexpected(failure);
        }
        return descriptor;
    }
#endif

    /// The production claimant: one lock file per candidate root.
    class LockFileScratchClaimant final: public IScratchClaimant
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<IScratchClaim>, ScratchClaimRefusal> Claim(
            std::filesystem::path const& base, std::size_t maxRoots) override
        {
            std::error_code ec;
            std::filesystem::create_directories(base, ec);
            if (ec && !std::filesystem::is_directory(base))
                return std::unexpected(ScratchClaimRefusal::Unavailable);

            for (std::size_t index = 0; index < maxRoots; ++index)
            {
                auto const root = base / std::format("node-{}", index);
                auto held = TakeClaim(ClaimFileFor(root));
                if (!held.has_value())
                {
                    if (held.error() == ClaimFailure::Contended)
                        continue; // A live owner. Try the next root.
                    return std::unexpected(ScratchClaimRefusal::Unavailable);
                }

                // The lock is held, so whoever wrote anything here is gone: the OS
                // released their claim however they died, `_Exit` included. There is
                // no staleness to reason about and no pid to inspect.
                std::error_code made;
                auto const existed = std::filesystem::exists(root, made);
                std::filesystem::create_directories(root, made);
                if (made && !std::filesystem::is_directory(root))
                {
                    CloseClaim(*held);
                    return std::unexpected(ScratchClaimRefusal::Unavailable);
                }
                auto const reclaimed = existed && EmptyDirectory(root);

                // Discarded deliberately, and named so rather than left as an
                // unchecked write. The record is DIAGNOSTICS: the lock decides
                // ownership and the record never does, exactly as its version field
                // never does -- so a claim whose record could not be written is still
                // a claim, and this root is still exclusively ours.
                //
                // Stated because the tempting repairs are both wrong. Turning it into
                // a refusal makes a node that cannot write one line fail to start,
                // which is a diagnostics failure escalated into an outage. Leaving the
                // return unchecked makes it look like nobody thought about it.
                (void) RecordOwnership(*held);
                return std::make_unique<LockFileScratchClaim>(root, reclaimed, *held);
            }
            return std::unexpected(ScratchClaimRefusal::InUse);
        }

      private:
#if defined(_WIN32)
        /// Release a claim taken but not handed out.
        /// @param handle The held handle.
        static void CloseClaim(HANDLE handle) noexcept
        {
            ::CloseHandle(handle);
        }

        /// Write who owns this root, for an operator reading the directory.
        ///
        /// Diagnostics only, and a failure here is deliberately NOT fatal -- see the
        /// call site, which states the decision rather than leaving it to an
        /// unchecked return.
        /// @param handle The held handle.
        /// @return Whether the record was actually written.
        [[nodiscard]] static bool RecordOwnership(HANDLE handle)
        {
            auto const text = OwnershipRecord(static_cast<unsigned long long>(::GetCurrentProcessId()));
            if (::SetFilePointer(handle, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
                return false;
            DWORD written = 0;
            if (::WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) == 0)
                return false;
            if (::SetEndOfFile(handle) == 0)
                return false;
            return written == static_cast<DWORD>(text.size());
        }
#else
        /// Release a claim taken but not handed out.
        /// @param descriptor The held descriptor.
        static void CloseClaim(int descriptor) noexcept
        {
            ::close(descriptor);
        }

        /// Write who owns this root, for an operator reading the directory.
        ///
        /// Diagnostics only, and a failure here is deliberately NOT fatal -- see the
        /// call site, which states the decision rather than leaving it to an
        /// unchecked return.
        ///
        /// The results are checked rather than cast to `void`: glibc marks
        /// `ftruncate` and `write` `warn_unused_result`, and GCC deliberately ignores
        /// a `(void)` cast on those -- so the cast would not even silence it, which is
        /// the compiler being right. An ignored write here is not a decision.
        /// @param descriptor The held descriptor.
        /// @return Whether the record was actually written.
        [[nodiscard]] static bool RecordOwnership(int descriptor)
        {
            auto const text = OwnershipRecord(static_cast<unsigned long long>(::getpid()));
            if (::ftruncate(descriptor, 0) != 0)
                return false;
            if (::lseek(descriptor, 0, SEEK_SET) < 0)
                return false;
            auto const written = ::write(descriptor, text.data(), text.size());
            return written >= 0 && static_cast<std::size_t>(written) == text.size();
        }
#endif

        /// The diagnostic line a claim file carries.
        /// @param pid This process's identifier.
        /// @return The record text.
        [[nodiscard]] static std::string OwnershipRecord(unsigned long long pid)
        {
            auto const now = std::chrono::system_clock::now();
            return std::format("{} v{} pid={} since={:%FT%TZ}\n",
                               ClaimRecordMagic,
                               ClaimRecordVersion,
                               pid,
                               std::chrono::floor<std::chrono::seconds>(now));
        }
    };
} // namespace

std::unique_ptr<IScratchClaimant> MakeLockFileScratchClaimant()
{
    return std::make_unique<LockFileScratchClaimant>();
}

} // namespace FastCache::Node
