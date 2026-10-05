// SPDX-License-Identifier: Apache-2.0
#include "AtomicFile.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/ReplacingRename.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>

namespace FastCache::Cc
{

namespace
{

    /// The next of this process's atomic writes: what tells two of its writes in flight apart in
    /// their temp names. A counter rather than anything random -- `std::random_device` answers 0
    /// on one host here -- and per process, since the process id already separates processes.
    /// @return The sequence number, never the same twice in one process.
    [[nodiscard]] std::uint64_t NextWriteSequence() noexcept
    {
        static std::atomic<std::uint64_t> next { 0 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    /// No file held, in either representation.
    constexpr std::intptr_t InvalidHandle = -1;

    /// A file on disk, written through `std::ofstream`.
    class DiskFileSink final: public IFileSink
    {
      public:
        /// @param out An open stream over the file.
        explicit DiskFileSink(std::ofstream out):
            _out { std::move(out) }
        {
        }

        bool Write(std::span<std::byte const> bytes) override
        {
            _out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return _out.good();
        }

        bool Close() override
        {
            // `basic_filebuf::close` fails when the flush or the `fclose` under it does,
            // and `basic_ofstream::close` turns that into `failbit`: the checked close.
            _out.close();
            return !_out.fail();
        }

      private:
        std::ofstream _out;
    };

    /// This machine's filesystem.
    class DiskFiles final: public IAtomicWriteFiles
    {
      public:
        std::unique_ptr<IFileSink> Create(std::filesystem::path const& path) override
        {
            std::ofstream out { path, std::ios::binary | std::ios::trunc };
            if (!out)
                return nullptr;
            return std::make_unique<DiskFileSink>(std::move(out));
        }

        bool Replace(std::filesystem::path const& from, std::filesystem::path const& to) override
        {
            // The one rename the node's durable writer moves its files with, compiled in rather than
            // linked (`Platform/ReplacingRename`). Refused by the files -- a reader that does not
            // share delete is the one this writer expects -- is the answer; refused because the
            // filesystem has no such rename, the classic one is asked instead, which any open reader
            // refuses.
            return Platform::RenameIntoPlace(from, to, Platform::SystemReplacingRename {}).has_value();
        }

        void Remove(std::filesystem::path const& path) noexcept override
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    /// One step's name.
    struct AtomicWriteStepRow
    {
        AtomicWriteStep step;
        std::string_view name;
    };

    constexpr EnumTable<AtomicWriteStep, AtomicWriteStepRow> AtomicWriteStepTable { {
        { .step = AtomicWriteStep::Create, .name = "create" },
        { .step = AtomicWriteStep::Write, .name = "write" },
        { .step = AtomicWriteStep::Close, .name = "close" },
        { .step = AtomicWriteStep::Replace, .name = "replace" },
    } };
    static_assert(RowsInEnumeratorOrder(AtomicWriteStepTable, &AtomicWriteStepRow::step));

    /// One output role's name.
    struct OutputRoleRow
    {
        OutputRole role;
        std::string_view name;
    };

    constexpr EnumTable<OutputRole, OutputRoleRow> OutputRoleTable { {
        { .role = OutputRole::DependencyRecord, .name = "depfile" },
        { .role = OutputRole::Object, .name = "object" },
    } };
    static_assert(RowsInEnumeratorOrder(OutputRoleTable, &OutputRoleRow::role));

} // namespace

std::unique_ptr<IAtomicWriteFiles> MakeDiskFiles()
{
    return std::make_unique<DiskFiles>();
}

std::string_view AtomicWriteStepName(AtomicWriteStep step) noexcept
{
    return AtomicWriteStepTable[static_cast<std::size_t>(step)].name;
}

std::string_view OutputRoleName(OutputRole role) noexcept
{
    return OutputRoleTable[static_cast<std::size_t>(role)].name;
}

std::expected<void, RestoreFailure> RestoreOutputs(RestoredFile const& object,
                                                   std::optional<RestoredFile> const& dependencyRecord,
                                                   std::uint64_t writerId,
                                                   IAtomicWriteFiles& files)
{
    auto const restore = [writerId, &files](RestoredFile const& file,
                                            OutputRole role) -> std::expected<void, RestoreFailure> {
        return WriteFileAtomically(file.path, file.bytes, writerId, files).transform_error([role](AtomicWriteStep step) {
            return RestoreFailure { .role = role, .step = step };
        });
    };
    if (dependencyRecord.has_value())
        if (auto written = restore(*dependencyRecord, OutputRole::DependencyRecord); !written.has_value())
            return written;
    return restore(object, OutputRole::Object);
}

std::filesystem::path TempPathBeside(std::filesystem::path const& target, std::uint64_t writerId, std::uint64_t sequence)
{
    auto name = target.filename();
    name += "." + std::to_string(writerId) + "." + std::to_string(sequence) + ".tmp";
    return target.parent_path() / name;
}

std::expected<void, AtomicWriteStep> WriteFileAtomically(std::filesystem::path const& target,
                                                         std::span<std::byte const> bytes,
                                                         std::uint64_t writerId,
                                                         IAtomicWriteFiles& files)
{
    auto const temp = TempPathBeside(target, writerId, NextWriteSequence());
    // Every way out but the rename removes the temp file, AFTER its sink is gone: Windows
    // refuses to delete a file that is still open, and a temp file left per failure sits
    // in the build tree for good.
    auto const abandon = [&files, &temp](AtomicWriteStep step) {
        files.Remove(temp);
        return std::unexpected { step };
    };

    auto sink = files.Create(temp);
    if (sink == nullptr)
        return abandon(AtomicWriteStep::Create);
    if (!sink->Write(bytes))
    {
        // Its close is asked only to release the file; the write already failed.
        [[maybe_unused]] auto const closed = sink->Close();
        sink.reset();
        return abandon(AtomicWriteStep::Write);
    }
    auto const closed = sink->Close();
    sink.reset();
    if (!closed)
        return abandon(AtomicWriteStep::Close);
    if (!files.Replace(temp, target))
        return abandon(AtomicWriteStep::Replace);
    return {};
}

std::optional<SharedReadFile> SharedReadFile::Open(std::filesystem::path const& path)
{
#if defined(_WIN32)
    // Delete sharing is what lets a replace rename over this file while it is open: the
    // standard streams share read and write and never delete.
    auto* const handle = ::CreateFileW(path.wstring().c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return SharedReadFile { reinterpret_cast<std::intptr_t>(handle) };
#else
    auto const descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return std::nullopt;
    return SharedReadFile { static_cast<std::intptr_t>(descriptor) };
#endif
}

SharedReadFile::SharedReadFile(std::intptr_t handle) noexcept:
    _handle { handle }
{
}

SharedReadFile::SharedReadFile(SharedReadFile&& other) noexcept:
    _handle { std::exchange(other._handle, InvalidHandle) }
{
}

SharedReadFile& SharedReadFile::operator=(SharedReadFile&& other) noexcept
{
    if (this != &other)
    {
        Release();
        _handle = std::exchange(other._handle, InvalidHandle);
    }
    return *this;
}

SharedReadFile::~SharedReadFile()
{
    Release();
}

void SharedReadFile::Release() noexcept
{
    if (_handle == InvalidHandle)
        return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(_handle));
#else
    ::close(static_cast<int>(_handle));
#endif
    _handle = InvalidHandle;
}

std::optional<std::string> SharedReadFile::ReadAll() const
{
    if (_handle == InvalidHandle)
        return std::nullopt;
    std::string text;
    auto chunk = std::array<char, 16384> {};
    // Until a read answers nothing, which is the end; a failed read is no answer at all, since a
    // prefix of a state file is exactly what its reader must never act on.
    while (true)
    {
#if defined(_WIN32)
        DWORD got = 0;
        if (::ReadFile(reinterpret_cast<HANDLE>(_handle), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)
            == 0)
            return std::nullopt;
#else
        auto const got = ::read(static_cast<int>(_handle), chunk.data(), chunk.size());
        if (got < 0)
            return std::nullopt;
#endif
        if (got == 0)
            return text;
        text.append(chunk.data(), static_cast<std::size_t>(got));
    }
}

std::optional<std::string> ReadFileShared(std::filesystem::path const& path)
{
    auto file = SharedReadFile::Open(path);
    if (!file.has_value())
        return std::nullopt;
    return file->ReadAll();
}

std::uint64_t CurrentProcessId() noexcept
{
    // A `#if` because the OSes provide this differently rather than spelling one call two
    // ways. Used only to make a temp name unique, which is why it needs no seam.
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace FastCache::Cc
