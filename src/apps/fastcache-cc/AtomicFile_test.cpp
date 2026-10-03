// SPDX-License-Identifier: Apache-2.0
#include "AtomicFile.hpp"
#include "FileBytes.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// @param text ASCII text. @return Its bytes.
[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> bytes;
    std::ranges::transform(text, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// A filesystem in memory that fails the step it is told to.
///
/// A failed WRITE lands a prefix first -- `failingWriteLands` bytes of it -- because that
/// is what a full disk does, and a prefix is exactly what must never reach the target.
/// It also records a removal made while a sink is still open, which Windows refuses.
class FakeDisk final: public IAtomicWriteFiles
{
  public:
    std::map<std::filesystem::path, std::string> files;
    std::optional<AtomicWriteStep> failAt;
    /// A rename onto this target fails, and only that one: a failure BETWEEN two writes.
    std::optional<std::filesystem::path> failReplaceOnto;
    /// Every target a rename landed on, in order, and every file created.
    std::vector<std::filesystem::path> replaced;
    std::vector<std::filesystem::path> created;
    std::size_t failingWriteLands { 3 };
    int openSinks { 0 };
    bool removedWhileOpen { false };

    std::unique_ptr<IFileSink> Create(std::filesystem::path const& path) override
    {
        if (failAt == AtomicWriteStep::Create)
            return nullptr;
        created.push_back(path);
        files[path].clear();
        return std::make_unique<Sink>(*this, path);
    }

    bool Replace(std::filesystem::path const& from, std::filesystem::path const& to) override
    {
        if (failAt == AtomicWriteStep::Replace || failReplaceOnto == to || !files.contains(from))
            return false;
        files[to] = files.at(from);
        files.erase(from);
        replaced.push_back(to);
        return true;
    }

    void Remove(std::filesystem::path const& path) noexcept override
    {
        removedWhileOpen = removedWhileOpen || openSinks != 0;
        files.erase(path);
    }

  private:
    class Sink final: public IFileSink
    {
      public:
        Sink(FakeDisk& disk, std::filesystem::path path):
            _disk { disk },
            _path { std::move(path) }
        {
            ++_disk.openSinks;
        }

        Sink(Sink const&) = delete;
        Sink& operator=(Sink const&) = delete;
        Sink(Sink&&) = delete;
        Sink& operator=(Sink&&) = delete;

        ~Sink() override
        {
            if (!_closed)
                --_disk.openSinks;
        }

        bool Write(std::span<std::byte const> bytes) override
        {
            auto const failing = _disk.failAt == AtomicWriteStep::Write;
            auto const lands = failing ? std::min(bytes.size(), _disk.failingWriteLands) : bytes.size();
            auto& file = _disk.files[_path];
            std::ranges::transform(
                bytes.first(lands), std::back_inserter(file), [](std::byte b) { return static_cast<char>(b); });
            return !failing;
        }

        bool Close() override
        {
            _closed = true;
            --_disk.openSinks;
            return _disk.failAt != AtomicWriteStep::Close;
        }

      private:
        FakeDisk& _disk;
        std::filesystem::path _path;
        bool _closed { false };
    };
};

constexpr std::uint64_t Writer = 4242;

/// @param directory A directory. @return The names of what is in it, sorted.
[[nodiscard]] std::vector<std::string> NamesIn(std::filesystem::path const& directory)
{
    std::vector<std::string> names;
    for (auto const& item: std::filesystem::directory_iterator { directory })
        names.push_back(item.path().filename().string());
    std::ranges::sort(names);
    return names;
}

/// Whether a reader that does NOT share delete -- a standard stream, another program -- makes
/// this platform refuse a rename over the file it holds. Windows' answer, measured by lane 2a;
/// POSIX lets any rename through.
#if defined(_WIN32)
constexpr bool UnsharedReaderRefusesReplace = true;
#else
constexpr bool UnsharedReaderRefusesReplace = false;
#endif

} // namespace

TEST_CASE("An atomic write replaces its target whole", "[launcher][atomicfile]")
{
    FakeDisk disk;
    std::filesystem::path const target = "build/u.obj";
    disk.files[target] = "AN OLDER, LONGER OBJECT";
    auto const written = WriteFileAtomically(target, Bytes("NEW"), Writer, disk);
    CHECK(written.has_value());
    // The whole new content and nothing else: no temp file left beside it.
    CHECK(disk.files == std::map<std::filesystem::path, std::string> { { target, "NEW" } });
    CHECK_FALSE(disk.removedWhileOpen);
}

TEST_CASE("An atomic write that fails at any step leaves its target as it was", "[launcher][atomicfile]")
{
    // A target that existed keeps its old bytes, one that did not stays absent -- and in
    // neither case does it hold a PREFIX of the new bytes, which is what a direct write
    // leaves when it is interrupted, under a fresh timestamp the build system believes.
    std::filesystem::path const target = "build/u.obj";
    for (auto const step: Enumerators<AtomicWriteStep>())
    {
        for (auto const existed: { true, false })
        {
            INFO("failing at " << AtomicWriteStepName(step) << (existed ? ", over an existing target" : ", no target"));
            FakeDisk disk;
            disk.failAt = step;
            if (existed)
                disk.files[target] = "THE OLD OBJECT";
            auto const before = disk.files;

            auto const written = WriteFileAtomically(target, Bytes("THE NEW OBJECT"), Writer, disk);
            REQUIRE_FALSE(written.has_value());
            CHECK(written.error() == step);
            // Exactly as it was: the target untouched and no temp file left beside it.
            CHECK(disk.files == before);
            // The temp file was released before it was removed, which Windows requires.
            CHECK_FALSE(disk.removedWhileOpen);
            CHECK(disk.openSinks == 0);
        }
    }
}

TEST_CASE("A close that fails is a failed write, whatever the writes said", "[launcher][atomicfile]")
{
    // Every byte was accepted into the buffer; the flush the close performs is where a full
    // volume says no. A write that did not ask its close would rename a short file into place.
    FakeDisk disk;
    disk.failAt = AtomicWriteStep::Close;
    std::filesystem::path const target = "u.d";
    auto const written = WriteFileAtomically(target, Bytes("u.obj: u.cpp\n"), Writer, disk);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == AtomicWriteStep::Close);
    CHECK_FALSE(disk.files.contains(target));
}

TEST_CASE("A restore commits the depfile first and the object last", "[launcher][atomicfile]")
{
    // The object's rename is the commit (the combined re-review's M2): a failure or a kill
    // before it leaves the OLD object, out of date against the edited source.
    FakeDisk disk;
    std::filesystem::path const object = "u.obj";
    std::filesystem::path const depfile = "u.d";
    disk.files[object] = "OLD OBJECT";
    disk.files[depfile] = "OLD DEPS";
    auto const objectBytes = Bytes("NEW OBJECT");
    auto const depBytes = Bytes("NEW DEPS");
    auto const restored = RestoreOutputs(RestoredFile { .path = object, .bytes = objectBytes },
                                         RestoredFile { .path = depfile, .bytes = depBytes },
                                         Writer,
                                         disk);
    CHECK(restored.has_value());
    CHECK(disk.replaced == std::vector<std::filesystem::path> { depfile, object });
    CHECK(disk.files == std::map<std::filesystem::path, std::string> { { object, "NEW OBJECT" }, { depfile, "NEW DEPS" } });
}

TEST_CASE("A failure between a restore's two writes leaves the old object beside the new depfile", "[launcher][atomicfile]")
{
    // The crash window, injected: the depfile is renamed and the object's rename fails.
    // What is left is an object that LOOKS stale -- never a new object beside the previous
    // build's dependency record, which a build system would trust.
    FakeDisk disk;
    std::filesystem::path const object = "u.obj";
    std::filesystem::path const depfile = "u.d";
    disk.files[object] = "OLD OBJECT";
    disk.files[depfile] = "OLD DEPS";
    disk.failReplaceOnto = object;
    auto const objectBytes = Bytes("NEW OBJECT");
    auto const depBytes = Bytes("NEW DEPS");
    auto const restored = RestoreOutputs(RestoredFile { .path = object, .bytes = objectBytes },
                                         RestoredFile { .path = depfile, .bytes = depBytes },
                                         Writer,
                                         disk);
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error().role == OutputRole::Object);
    CHECK(restored.error().step == AtomicWriteStep::Replace);
    CHECK(disk.files == std::map<std::filesystem::path, std::string> { { object, "OLD OBJECT" }, { depfile, "NEW DEPS" } });
}

TEST_CASE("A depfile that cannot be restored leaves the object untouched and names the depfile", "[launcher][atomicfile]")
{
    FakeDisk disk;
    std::filesystem::path const object = "u.obj";
    std::filesystem::path const depfile = "u.d";
    disk.files[object] = "OLD OBJECT";
    disk.failReplaceOnto = depfile;
    auto const objectBytes = Bytes("NEW OBJECT");
    auto const depBytes = Bytes("NEW DEPS");
    auto const restored = RestoreOutputs(RestoredFile { .path = object, .bytes = objectBytes },
                                         RestoredFile { .path = depfile, .bytes = depBytes },
                                         Writer,
                                         disk);
    REQUIRE_FALSE(restored.has_value());
    CHECK(restored.error().role == OutputRole::DependencyRecord);
    CHECK(OutputRoleName(restored.error().role) == "depfile");
    // The object was not even begun: no temp file beside it was ever created.
    CHECK(std::ranges::none_of(disk.created, [&object](auto const& path) {
        return path.filename().string().starts_with(object.filename().string() + ".");
    }));
    CHECK(disk.files == std::map<std::filesystem::path, std::string> { { object, "OLD OBJECT" } });
}

TEST_CASE("A restore with no depfile writes the object alone", "[launcher][atomicfile]")
{
    FakeDisk disk;
    std::filesystem::path const object = "u.obj";
    auto const objectBytes = Bytes("NEW OBJECT");
    CHECK(RestoreOutputs(RestoredFile { .path = object, .bytes = objectBytes }, std::nullopt, Writer, disk).has_value());
    CHECK(disk.replaced == std::vector<std::filesystem::path> { object });
}

TEST_CASE("The temp file of an atomic write sits beside its target", "[launcher][atomicfile]")
{
    // Beside, because a rename replaces in one step only within one volume.
    std::filesystem::path const target = std::filesystem::path { "out" } / "build" / "u.obj";
    auto const temp = TempPathBeside(target, Writer, 7);
    CHECK(temp.parent_path() == target.parent_path());
    CHECK(temp != target);
    CHECK(temp.filename().string().contains("4242"));
    // Another process, and another write of the SAME process, each get a name of their own.
    CHECK(temp != TempPathBeside(target, Writer + 1, 7));
    CHECK(temp != TempPathBeside(target, Writer, 8));
}

TEST_CASE("On disk, an atomic write replaces a longer file whole and leaves no temp file", "[launcher][atomicfile]")
{
    Testing::ScratchDirectory const scratch { "fc-cc-atomic" };
    scratch.Write("u.obj", "AN OLDER OBJECT, LONGER THAN THE NEW ONE");
    auto const files = MakeDiskFiles();
    auto const written = WriteFileAtomically(scratch / "u.obj", Bytes("NEW OBJECT"), Writer, *files);
    REQUIRE(written.has_value());
    CHECK(ReadFileBytes(scratch / "u.obj") == std::optional { Bytes("NEW OBJECT") });
    auto const entries = std::ranges::distance(std::filesystem::directory_iterator { scratch.Path() },
                                               std::filesystem::directory_iterator {});
    CHECK(entries == 1);
}

TEST_CASE("On disk, a rename that cannot happen leaves the target and removes the temp file", "[launcher][atomicfile]")
{
    // A non-empty DIRECTORY where the object should be: no rename replaces it, on any
    // platform, so the write fails at its last step -- after the temp file was written.
    Testing::ScratchDirectory const scratch { "fc-cc-atomic-replace" };
    scratch.Write("u.obj/keep", "kept");
    auto const files = MakeDiskFiles();
    auto const written = WriteFileAtomically(scratch / "u.obj", Bytes("NEW OBJECT"), Writer, *files);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == AtomicWriteStep::Replace);
    CHECK(ReadFileBytes(scratch / "u.obj/keep") == std::optional { Bytes("kept") });
    CHECK(NamesIn(scratch.Path()) == std::vector<std::string> { "u.obj" });
}

TEST_CASE("Two threads of one process writing one target each finish whole and leave no temp file",
          "[launcher][atomicfile][concurrency]")
{
    // The per-user state files are written by every launcher, and a launcher writes from more
    // than one thread, so two writes of ONE process to one target meet. A temp name of the
    // process id alone gave both the same temp file: one truncated what the other was writing,
    // and the second rename found its temp file already gone. Many short rounds, each one more
    // chance at the overlap; every observation is taken on this thread, after the joins.
    constexpr auto Rounds = 60;
    constexpr std::size_t PayloadBytes = 256 * 1024;
    Testing::ScratchDirectory const scratch { "fc-cc-atomic-threads" };
    auto const target = scratch / "toolchain.fingerprint";
    auto const payloadA = std::vector<std::byte>(PayloadBytes, std::byte { 'a' });
    auto const payloadB = std::vector<std::byte>(PayloadBytes, std::byte { 'b' });
    std::vector<std::string> wrong;
    for (auto const round: std::views::iota(0, Rounds))
    {
        std::latch start { 2 };
        std::optional<std::expected<void, AtomicWriteStep>> writtenA;
        std::optional<std::expected<void, AtomicWriteStep>> writtenB;
        auto const write = [&start, &target](std::vector<std::byte> const& payload,
                                             std::optional<std::expected<void, AtomicWriteStep>>& result) {
            auto const files = MakeDiskFiles();
            start.arrive_and_wait();
            result = WriteFileAtomically(target, payload, CurrentProcessId(), *files);
        };
        {
            std::jthread const a { write, std::cref(payloadA), std::ref(writtenA) };
            std::jthread const b { write, std::cref(payloadB), std::ref(writtenB) };
        }
        auto const landed = ReadFileBytes(target);
        auto const whole = landed == std::optional { payloadA } || landed == std::optional { payloadB };
        auto const finished = writtenA.has_value() && writtenA->has_value() && writtenB.has_value() && writtenB->has_value();
        auto const names = NamesIn(scratch.Path());
        if (!finished || !whole || names != std::vector<std::string> { "toolchain.fingerprint" })
            wrong.push_back(std::format("round {}: both finished={}, one payload whole={}, {} entr(ies) in the directory",
                                        round,
                                        finished,
                                        whole,
                                        names.size()));
    }
    INFO("rounds that ended wrong: " << wrong.size() << " of " << Rounds
                                     << (wrong.empty() ? "" : ", the first: " + wrong.front()));
    CHECK(wrong.empty());
}

TEST_CASE("A replace over a reader that holds the target open is never torn: it lands or it is refused and changes nothing",
          "[launcher][atomicfile]")
{
    // Launchers read the fingerprint cache and the reachability memo while another launcher
    // replaces them.
    Testing::ScratchDirectory const scratch { "fc-cc-atomic-reader" };
    auto const target = scratch / "reachability.memo";

    SECTION("a reader opened the way every launcher reader opens it: the replace lands, and the reader keeps its bytes")
    {
        scratch.Write("reachability.memo", "OLD");
        auto reader = SharedReadFile::Open(target);
        REQUIRE(reader.has_value());
        auto const files = MakeDiskFiles();
        auto const written = WriteFileAtomically(target, Bytes("NEW"), CurrentProcessId(), *files);
        CHECK(written.has_value());
        CHECK(Testing::Unwrap(reader).ReadAll() == std::optional<std::string> { "OLD" });
        CHECK(ReadFileShared(target) == std::optional<std::string> { "NEW" });
        CHECK(NamesIn(scratch.Path()) == std::vector<std::string> { "reachability.memo" });
    }

    SECTION("a reader that does not share delete: where the platform refuses, a reported failure and the old file intact")
    {
        scratch.Write("reachability.memo", "OLD");
        std::ifstream const unshared { target, std::ios::binary };
        REQUIRE(unshared.is_open());
        auto const files = MakeDiskFiles();
        auto const written = WriteFileAtomically(target, Bytes("NEW"), CurrentProcessId(), *files);
        if (UnsharedReaderRefusesReplace)
        {
            // The safe miss: a stale answer for the next reader, never a torn or empty file.
            REQUIRE_FALSE(written.has_value());
            CHECK(written.error() == AtomicWriteStep::Replace);
            CHECK(ReadFileShared(target) == std::optional<std::string> { "OLD" });
        }
        else
        {
            CHECK(written.has_value());
            CHECK(ReadFileShared(target) == std::optional<std::string> { "NEW" });
        }
        CHECK(NamesIn(scratch.Path()) == std::vector<std::string> { "reachability.memo" });
    }
}
