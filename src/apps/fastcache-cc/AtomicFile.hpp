// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// One file being written: its bytes, then a CHECKED close.
///
/// The close is its own operation because it is where a buffered write first meets the
/// disk: a full volume or a lost network share can accept every `write` into the buffer
/// and refuse the flush the close performs. A write whose close was not asked is a write
/// nobody knows landed.
class IFileSink
{
  public:
    IFileSink() = default;
    IFileSink(IFileSink const&) = delete;
    IFileSink& operator=(IFileSink const&) = delete;
    IFileSink(IFileSink&&) = delete;
    IFileSink& operator=(IFileSink&&) = delete;
    virtual ~IFileSink() = default;

    /// Append @p bytes.
    /// @param bytes What to write.
    /// @return False when they could not all be written.
    [[nodiscard]] virtual bool Write(std::span<std::byte const> bytes) = 0;

    /// Flush and close. Called once, before the file is renamed or removed.
    /// @return False when the close failed: what was written is not known to be stored.
    [[nodiscard]] virtual bool Close() = 0;
};

/// The filesystem operations an atomic replace is made of: the seam `WriteFileAtomically`
/// is tested through, so each step can be failed without a full disk.
class IAtomicWriteFiles
{
  public:
    IAtomicWriteFiles() = default;
    IAtomicWriteFiles(IAtomicWriteFiles const&) = delete;
    IAtomicWriteFiles& operator=(IAtomicWriteFiles const&) = delete;
    IAtomicWriteFiles(IAtomicWriteFiles&&) = delete;
    IAtomicWriteFiles& operator=(IAtomicWriteFiles&&) = delete;
    virtual ~IAtomicWriteFiles() = default;

    /// Create @p path empty, replacing any file there.
    /// @param path The file to create.
    /// @return A sink over it, or nullptr when it could not be created.
    [[nodiscard]] virtual std::unique_ptr<IFileSink> Create(std::filesystem::path const& path) = 0;

    /// Rename @p from over @p to in one step, replacing a file already at @p to.
    /// @param from The file to move. @param to Where it goes.
    /// @return False when the rename failed, which leaves @p to as it was.
    [[nodiscard]] virtual bool Replace(std::filesystem::path const& from, std::filesystem::path const& to) = 0;

    /// Remove @p path if it exists; a failure is not reported, since nothing can act on it.
    /// @param path The file to remove.
    virtual void Remove(std::filesystem::path const& path) noexcept = 0;
};

/// This machine's files: `std::ofstream` with a checked `close()`, and a rename that replaces an
/// existing file in one step -- `rename(2)` on POSIX. On Windows it is a POSIX-semantics rename
/// (`SetFileInformationByHandle`, `FileRenameInfoEx`), which replaces a file another handle holds
/// open as long as that reader shares delete, as `SharedReadFile` does; on a filesystem with no
/// such rename it falls back to `MoveFileExW`'s, which any open reader refuses.
/// @return The implementation.
[[nodiscard]] std::unique_ptr<IAtomicWriteFiles> MakeDiskFiles();

/// The step an atomic write failed at. Private: never transmitted or persisted, so its
/// enumerators carry no values; `Last` sizes the name table.
enum class AtomicWriteStep : std::uint8_t
{
    Create,  ///< The temp file could not be created.
    Write,   ///< The bytes could not all be written to it.
    Close,   ///< Its close failed, so what was written is not known to be stored.
    Replace, ///< It could not be renamed over the target.
    Last
};

/// @param step A step. @return Its name, for a diagnostic.
[[nodiscard]] std::string_view AtomicWriteStepName(AtomicWriteStep step) noexcept;

/// The temp file an atomic write of @p target goes through: BESIDE it, because a rename
/// replaces in one step only within one volume, and named for @p writerId AND @p sequence, so
/// no two writes in flight share one temp file and interleave into it -- not two processes,
/// and not two threads of one process either, which a process id alone let collide.
/// @param target The file being written.
/// @param writerId What distinguishes this writer from a concurrent one -- the process id.
/// @param sequence Which of this process's writes it is.
/// @return The temp path, in @p target's directory.
[[nodiscard]] std::filesystem::path TempPathBeside(std::filesystem::path const& target,
                                                   std::uint64_t writerId,
                                                   std::uint64_t sequence);

/// Replace @p target with @p bytes so that a reader -- a build system, a linker, the next
/// compile -- sees the old file or the whole new one, never a prefix of it.
///
/// The bytes go to `TempPathBeside` -- this process's id and the next of its per-process write
/// sequence -- which is closed with its close CHECKED and only then renamed over @p target.
/// Every way out that is not the rename removes the temp file, so a failure leaves @p target
/// exactly as it was and nothing beside it. Written for a cache HIT, which writes an object and
/// a depfile no compiler produced: a launcher killed mid-write, or a disk filling mid-write, used
/// to leave a truncated object with a FRESH timestamp, which the build system then took as up to
/// date. And the ONE writer of the launcher's per-user state files -- the toolchain fingerprint
/// cache, the reachability memo -- which sixteen launchers on a cold cache replace at once.
///
/// **A reader holding @p target open is the case to know about**, and it is never a torn or empty
/// file. A reader that opened it through `SharedReadFile` -- every launcher reader of a replaced
/// file does -- does not stop the replace: the reader goes on reading the bytes it opened, and the
/// next open sees the new file. A reader that does NOT share delete (another program, or a
/// filesystem with no POSIX-semantics rename) makes Windows REFUSE the replace: that is
/// `AtomicWriteStep::Replace`, with @p target exactly as it was -- a stale answer, never a wrong
/// one, which costs the fingerprint cache a rewalk and the memo a dial. POSIX never refuses.
///
/// @param target The file to replace.
/// @param bytes Its new contents.
/// @param writerId What distinguishes this writer from a concurrent one -- the process id.
/// @param files The filesystem.
/// @return Nothing, or the step that failed.
[[nodiscard]] std::expected<void, AtomicWriteStep> WriteFileAtomically(std::filesystem::path const& target,
                                                                       std::span<std::byte const> bytes,
                                                                       std::uint64_t writerId,
                                                                       IAtomicWriteFiles& files);

/// Which of a compile's outputs a restored file is. Private: never transmitted or
/// persisted; `Last` sizes the name table.
enum class OutputRole : std::uint8_t
{
    DependencyRecord, ///< The depfile the build names.
    Object,           ///< The object file.
    Last
};

/// @param role A role. @return What a diagnostic calls it.
[[nodiscard]] std::string_view OutputRoleName(OutputRole role) noexcept;

/// One file a restore writes.
struct RestoredFile
{
    std::filesystem::path path;       ///< Where it goes.
    std::span<std::byte const> bytes; ///< What it holds.
};

/// Which file a restore failed on, and at which step.
struct RestoreFailure
{
    OutputRole role;      ///< The file.
    AtomicWriteStep step; ///< The step.
};

/// Restore a compile's outputs, the dependency record FIRST and the object LAST.
///
/// Each file is atomic by `WriteFileAtomically`; the PAIR is ordered, and the order is
/// the point. The object's rename is the commit: a failure or a kill before it leaves the
/// OLD object, whose timestamp still marks it out of date against the edited source, so
/// the build recompiles -- beside a depfile that is new, which only ever costs a rebuild.
/// Object first was the other way round: a new object with a fresh timestamp beside the
/// PREVIOUS build's dependency record, which the build system then trusts.
///
/// The object is its own parameter rather than one entry of a list, so no caller can put
/// it anywhere but last.
/// @param object The object file.
/// @param dependencyRecord The depfile, when the build names one.
/// @param writerId What distinguishes this writer from a concurrent one -- the process id.
/// @param files The filesystem.
/// @return Nothing, or which file failed and at which step; the object is not written
///         after a failure of the dependency record.
[[nodiscard]] std::expected<void, RestoreFailure> RestoreOutputs(RestoredFile const& object,
                                                                 std::optional<RestoredFile> const& dependencyRecord,
                                                                 std::uint64_t writerId,
                                                                 IAtomicWriteFiles& files);

/// A file opened for reading the way every launcher reader of an atomically replaced file opens
/// it: sharing DELETE on Windows, so a concurrent `WriteFileAtomically` can rename over it while it
/// is open (`MakeDiskFiles`); the open file goes on reading what it opened. On POSIX, any open file
/// already allows that.
class SharedReadFile
{
  public:
    /// @param path The file.
    /// @return The open file, or nothing when it is absent or cannot be opened.
    [[nodiscard]] static std::optional<SharedReadFile> Open(std::filesystem::path const& path);

    SharedReadFile(SharedReadFile const&) = delete;
    SharedReadFile& operator=(SharedReadFile const&) = delete;
    /// Takes @p other's file; @p other holds none after.
    SharedReadFile(SharedReadFile&& other) noexcept;
    /// Closes this file and takes @p other's.
    SharedReadFile& operator=(SharedReadFile&& other) noexcept;
    ~SharedReadFile();

    /// Const because the position it advances is the open FILE's, which this object only names.
    /// @return Everything from the current position to the end, or nothing when a read failed.
    [[nodiscard]] std::optional<std::string> ReadAll() const;

  private:
    /// @param handle An open handle (Windows) or descriptor (POSIX), owned from here.
    explicit SharedReadFile(std::intptr_t handle) noexcept;

    /// Close the file, if one is held.
    void Release() noexcept;

    std::intptr_t _handle; ///< The open handle or descriptor; `InvalidHandle` when none.
};

/// The whole of @p path, read through `SharedReadFile`.
/// @param path The file.
/// @return Its contents, or nothing when it is absent or unreadable.
[[nodiscard]] std::optional<std::string> ReadFileShared(std::filesystem::path const& path);

/// This process's id: the `writerId` a real writer passes.
/// @return The id.
[[nodiscard]] std::uint64_t CurrentProcessId() noexcept;

} // namespace FastCache::Cc
