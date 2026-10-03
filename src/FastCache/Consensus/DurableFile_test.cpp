// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Platform/ReplacingRename.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;

TEST_CASE("A missing file and an empty one are different answers", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file" };

    auto const absent = ReadFileIfPresent(scratch / "absent");
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->has_value());

    scratch.Write("empty");
    auto const empty = ReadFileIfPresent(scratch / "empty");
    REQUIRE(empty.has_value());
    REQUIRE(Testing::Unwrap(empty).has_value());
    CHECK(Testing::Unwrap(Testing::Unwrap(empty)).empty());
}

TEST_CASE("A replaced file reads back whole, and replacing it again leaves only the new bytes", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file-replace" };
    auto const path = scratch / "state";

    auto const first = std::vector<std::byte>(64, std::byte { 0xAA });
    REQUIRE(ReplaceFileAtomically(path, first, StateFile::Formation).has_value());
    auto const second = WireFields::AsBytes("short");
    REQUIRE(ReplaceFileAtomically(path, second, StateFile::Formation).has_value());

    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    REQUIRE(Testing::Unwrap(read).has_value());
    CHECK(WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) == "short");
}

TEST_CASE("A file a reader holds open is still replaced, and the reader keeps what it opened", "[consensus][storage]")
{
    // A node's record is replaced on the formation's beat thread; a reader holding it open at that
    // moment must not make the replace fail. On Windows that needs the reader's delete sharing AND the
    // replace's POSIX-semantics rename -- measured, either alone refuses -- and this case holds both.
    auto const scratch = Testing::ScratchDirectory { "durable-file-held" };
    auto const path = scratch / "formation";
    REQUIRE(ReplaceFileAtomically(path, WireFields::AsBytes("old"), StateFile::Formation).has_value());

    auto held = OpenForReading(path);
    REQUIRE(held.has_value());
    auto const replaced = ReplaceFileAtomically(path, WireFields::AsBytes("new"), StateFile::Formation);
    INFO((replaced.has_value() ? std::string { "(replaced)" } : replaced.error().context));
    CHECK(replaced.has_value());

    auto kept = std::array<char, 8> {};
    auto const n = std::fread(kept.data(), 1, kept.size(), held->get());
    CHECK(std::string_view { kept.data(), n } == "old");
    held->reset();

    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    REQUIRE(Testing::Unwrap(read).has_value());
    CHECK(WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) == "new");
}

TEST_CASE("A directory where a file belongs is a failure to read, never an absent file", "[consensus][storage]")
{
    auto const scratch = Testing::ScratchDirectory { "durable-file-dir" };
    scratch.Write("dir/inside");

    auto const read = ReadFileIfPresent(scratch / "dir");
    CHECK_FALSE(read.has_value());
}

namespace
{
/// Which step of a replace's temporary a `RecordingDurableFiles` fails.
///
/// Private to this file: never transmitted or persisted.
enum class FailedStep : std::uint8_t
{
    None,
    Write,
    Sync,
    Close,
    DirectorySync,
};

/// The system's files, with every step a replace asks of its temporary recorded in order, and one
/// of them failed on request: the seam each durability property is asserted through.
class RecordingDurableFiles final: public IDurableFiles
{
  public:
    /// @param fail The step to fail, or none.
    explicit RecordingDurableFiles(FailedStep fail) noexcept:
        _fail { fail }
    {
    }

    std::expected<std::unique_ptr<IDurableSink>, std::error_code> Create(std::filesystem::path const& path,
                                                                         StateFile which) override
    {
        _calls.emplace_back("create");
        return _system.Create(path, which).transform([this](std::unique_ptr<IDurableSink> real) {
            return std::unique_ptr<IDurableSink> { std::make_unique<Sink>(*this, std::move(real)) };
        });
    }

    std::error_code SyncDirectory(std::filesystem::path const& directory) override
    {
        _calls.emplace_back("sync-directory");
        return _fail == FailedStep::DirectorySync ? std::make_error_code(std::errc::io_error)
                                                  : _system.SyncDirectory(directory);
    }

    /// Record a step another seam took, in the same order as this one's.
    /// @param step What was asked.
    void Note(std::string step)
    {
        _calls.push_back(std::move(step));
    }

    /// @return Every step asked, in order.
    [[nodiscard]] std::vector<std::string> const& Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::vector<std::string> _calls;

    /// The real temporary, each step recorded and the chosen one failed.
    class Sink final: public IDurableSink
    {
      public:
        Sink(RecordingDurableFiles& owner, std::unique_ptr<IDurableSink> real) noexcept:
            _owner { owner },
            _real { std::move(real) }
        {
        }

        std::error_code Write(std::span<std::byte const> bytes) override
        {
            _owner._calls.emplace_back("write");
            return _owner._fail == FailedStep::Write ? Refused() : _real->Write(bytes);
        }

        std::error_code Sync() override
        {
            _owner._calls.emplace_back("sync");
            return _owner._fail == FailedStep::Sync ? Refused() : _real->Sync();
        }

        std::error_code Close() override
        {
            _owner._calls.emplace_back("close");
            // The real file is closed either way, so the replace can remove it.
            auto const closed = _real->Close();
            return _owner._fail == FailedStep::Close ? Refused() : closed;
        }

      private:
        /// @return What a full volume says.
        [[nodiscard]] static std::error_code Refused() noexcept
        {
            return std::make_error_code(std::errc::no_space_on_device);
        }

        RecordingDurableFiles& _owner;
        std::unique_ptr<IDurableSink> _real;
    };

    FailedStep _fail;
    SystemDurableFiles _system;
};

/// The system's POSIX-semantics rename, recorded in a `RecordingDurableFiles`'s order.
class RecordingRename final: public Platform::IReplacingRename
{
  public:
    /// @param log Where the rename is recorded.
    explicit RecordingRename(RecordingDurableFiles& log) noexcept:
        _log { &log }
    {
    }

    [[nodiscard]] std::error_code RenameReplacing(std::filesystem::path const& from,
                                                  std::filesystem::path const& to) const override
    {
        _log->Note("rename");
        return _system.RenameReplacing(from, to);
    }

  private:
    RecordingDurableFiles* _log;
    Platform::SystemReplacingRename _system;
};

/// @param path A file.
/// @return Its contents as text; empty when it is absent.
[[nodiscard]] std::string TextAt(std::filesystem::path const& path)
{
    auto const read = ReadFileIfPresent(path);
    REQUIRE(read.has_value());
    return Testing::Unwrap(read).has_value()
               ? std::string { WireFields::AsStringView(Testing::Unwrap(Testing::Unwrap(read))) }
               : std::string {};
}
} // namespace

TEST_CASE("A replace writes, syncs to the platter and closes its temporary, renames it, then syncs the directory",
          "[consensus][storage][durable]")
{
    // The order is the property: a rename before the sync can land a file a power loss empties, a
    // rename before a CHECKED close can land one a full volume truncated, and a replace reported
    // before its DIRECTORY is flushed is one a power loss can take back -- the old entry returns.
    auto const scratch = Testing::ScratchDirectory { "durable-file-order" };
    auto const path = scratch / "formation";
    RecordingDurableFiles files { FailedStep::None };
    auto const rename = RecordingRename { files };

    auto const replaced = ReplaceFileWith(path, WireFields::AsBytes("new"), StateFile::Formation, files, rename);
    REQUIRE(replaced.has_value());
    CHECK(files.Calls() == std::vector<std::string> { "create", "write", "sync", "close", "rename", "sync-directory" });
    CHECK(TextAt(path) == "new");
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path { path }.concat(ReplacementSuffix)));
}

TEST_CASE("A replace whose write, sync or close fails leaves the file it replaces as it was, and names the step",
          "[consensus][storage][durable]")
{
    // A close is a step of its own: a full volume can accept every write into the buffer and refuse the
    // flush the close performs. A failed sync or close used to be read as a landed file.
    struct Row
    {
        FailedStep fail;
        std::string_view says;
        std::vector<std::string> calls;
    };
    auto const rows = std::to_array<Row>({
        { .fail = FailedStep::Write, .says = "cannot write", .calls = { "create", "write", "close" } },
        { .fail = FailedStep::Sync, .says = "cannot sync", .calls = { "create", "write", "sync", "close" } },
        { .fail = FailedStep::Close, .says = "cannot close", .calls = { "create", "write", "sync", "close" } },
    });
    for (auto const& row: rows)
    {
        CAPTURE(row.says);
        auto const scratch = Testing::ScratchDirectory { "durable-file-step" };
        auto const path = scratch / "formation";
        REQUIRE(ReplaceFileAtomically(path, WireFields::AsBytes("old"), StateFile::Formation).has_value());

        RecordingDurableFiles files { row.fail };
        auto const rename = Platform::SystemReplacingRename {};
        auto const refused = ReplaceFileWith(path, WireFields::AsBytes("new"), StateFile::Formation, files, rename);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().context.contains(row.says));
        // Closed whatever failed, so the temporary could be removed -- and it was.
        CHECK(files.Calls() == row.calls);
        CHECK(TextAt(path) == "old");
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path { path }.concat(ReplacementSuffix)));
    }
}

TEST_CASE("A replace whose directory cannot be synced is reported, never taken as durable", "[consensus][storage][durable]")
{
    // The rename has happened -- the new file is in place -- but nothing flushed the entry naming
    // it, so a power loss may still bring the old one back. A caller told "replaced" would act on
    // that: a vote cast on a term the disk may forget. So the replace fails, and says why.
    auto const scratch = Testing::ScratchDirectory { "durable-file-dirsync" };
    auto const path = scratch / "formation";
    REQUIRE(ReplaceFileAtomically(path, WireFields::AsBytes("old"), StateFile::Formation).has_value());

    RecordingDurableFiles files { FailedStep::DirectorySync };
    auto const rename = RecordingRename { files };
    auto const refused = ReplaceFileWith(path, WireFields::AsBytes("new"), StateFile::Formation, files, rename);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.contains("cannot sync its directory"));
    CHECK(files.Calls() == std::vector<std::string> { "create", "write", "sync", "close", "rename", "sync-directory" });
    // What is on the disk is the new file: the failure is about whether it survives, not whether it landed.
    CHECK(TextAt(path) == "new");
}

TEST_CASE("A filesystem that cannot sync a directory degrades the replace, and every other sync failure refuses it",
          "[consensus][storage][durable]")
{
    // Each of this platform's "not supported here" answers lands the replace and carries the answer back;
    // an answer outside the table is a sync that failed, and fails the replace (the case above).
    REQUIRE_FALSE(UnsupportedDirectorySyncAnswers().empty());
    for (auto const answer: UnsupportedDirectorySyncAnswers())
    {
        INFO(answer.message());
        CHECK(MeansDirectorySyncUnsupported(answer));
        auto const scratch = Testing::ScratchDirectory { "durable-file-unsynced" };
        auto const path = scratch / "formation";

        /// This machine's files, with a directory sync that answers `answer`.
        class Unsynced final: public IDurableFiles
        {
          public:
            explicit Unsynced(std::error_code said) noexcept:
                _said { said }
            {
            }

            std::expected<std::unique_ptr<IDurableSink>, std::error_code> Create(std::filesystem::path const& file,
                                                                                 StateFile which) override
            {
                return _system.Create(file, which);
            }

            std::error_code SyncDirectory(std::filesystem::path const& /*directory*/) override
            {
                return _said;
            }

          private:
            std::error_code _said;
            SystemDurableFiles _system;
        } files { answer };
        auto const rename = Platform::SystemReplacingRename {};
        auto const replaced = ReplaceFileWith(path, WireFields::AsBytes("new"), StateFile::Formation, files, rename);
        REQUIRE(replaced.has_value());
        CHECK(Testing::Unwrap(replaced).directoryUnsynced == answer);
        CHECK(TextAt(path) == "new");
    }
    CHECK_FALSE(MeansDirectorySyncUnsupported(std::make_error_code(std::errc::io_error)));
    CHECK_FALSE(MeansDirectorySyncUnsupported(std::error_code {}));
}

TEST_CASE("This machine's directory sync succeeds on a directory and is refused for one that is not there",
          "[consensus][storage][durable]")
{
    // The measurement the header states, taken on every platform the suite runs on: a refused flush
    // here would fail every replace a node makes. And the refusal path answers rather than passes.
    auto const scratch = Testing::ScratchDirectory { "durable-file-dirsync-real" };
    auto const synced = SyncDirectoryToDisk(scratch.Path());
    INFO(synced.message());
    CHECK_FALSE(synced);
    CHECK(SyncDirectoryToDisk(scratch / "absent"));
}
