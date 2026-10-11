// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "InputWitness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Cc;

namespace
{
/// Identities and contents a case sets by hand, and a clock it moves by hand.
///
/// A path with an identity SEQUENCE answers `Identify` from it instead of from `identities`: each
/// call takes the next answer, and the last one repeats forever. That is how a case places a
/// change BETWEEN two looks the witness takes in one step. An EMPTY sequence answers like an
/// unsequenced path, from `identities`, rather than reading the front of an empty deque.
///
/// `hashCalls` counts every `Hash`, so a case can assert that nothing was hashed.
class FakeInputFiles final: public IInputFiles
{
  public:
    std::map<std::string, FileIdentity, std::less<>> identities;
    std::map<std::string, std::string, std::less<>> hashes;
    mutable std::map<std::string, std::deque<std::optional<FileIdentity>>, std::less<>> sequences;
    mutable std::size_t hashCalls = 0;
    FileInstant now { 1'000 };

    [[nodiscard]] std::optional<FileIdentity> Identify(std::string_view path) const override
    {
        if (sequences.contains(path) && !sequences.find(path)->second.empty())
        {
            auto& queue = sequences.find(path)->second;
            auto answer = queue.front();
            if (queue.size() > 1)
                queue.pop_front();
            return answer;
        }
        if (!identities.contains(path))
            return std::nullopt;
        return identities.at(std::string { path });
    }
    [[nodiscard]] std::string Hash(std::string_view path) const override
    {
        ++hashCalls;
        return hashes.contains(path) ? hashes.at(std::string { path }) : std::string {};
    }
    [[nodiscard]] FileInstant Now() const override
    {
        return now;
    }
    /// `now` plus `FenceOffset`: a scripted clock has no tick to cross, so the fenced instant is the
    /// reading, offset by a value no case uses as a change time so that T0 taken from `Now()` instead
    /// of from `Fence()` is a different instant and a case can tell (`Begin takes T0 from the fence`).
    [[nodiscard]] FileInstant Fence() const override
    {
        return now + FenceOffset;
    }

    /// How far the fake's fence lies past its clock.
    static constexpr FileInstant FenceOffset = 7;

    /// Put @p path at content @p hash, changed at @p changeTime, with id @p id.
    void Set(std::string const& path, std::string hash, FileInstant changeTime, std::uint64_t id)
    {
        identities[path] =
            FileIdentity { .changeTime = changeTime, .size = hash.size(), .volumeId = 1, .fileIdHigh = 0, .fileIdLow = id };
        hashes[path] = std::move(hash);
    }
};

constexpr std::string_view Tu = "/r/src/a.cpp";
constexpr std::string_view Header = "/r/src/h.hpp";

/// A quiet TU and header, both last changed long before T0 (the fence, 1007).
FakeInputFiles QuietFiles()
{
    FakeInputFiles files;
    files.Set(std::string { Tu }, "tu-v1", 10, 1);
    files.Set(std::string { Header }, "h-v1", 10, 2);
    return files;
}

std::vector<std::string> const HeaderOnly { std::string { Header } };

/// An identity changed long before T0 (the fence, 1007), with file id @p id.
constexpr FileIdentity OldIdentity(std::uint64_t id)
{
    return FileIdentity { .changeTime = 10, .size = 5, .volumeId = 1, .fileIdHigh = 0, .fileIdLow = id };
}
} // namespace

TEST_CASE("Nothing moved is Stable, at the key and after the compile", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Stable, .path = "" });
    files.now = 3'000;
    CHECK(witness.AfterCompile({}, files) == StabilityFinding { .stability = InputStability::Stable, .path = "" });
    CHECK(witness.KeyedHash(Tu) == std::optional<std::string> { "tu-v1" });
    CHECK(witness.KeyedHash(Header) == std::optional<std::string> { "h-v1" });
}

TEST_CASE("Begin takes T0 from the fence, not the clock", "[input-witness][1650]")
{
    // The fence is what puts a save made just before the invocation on the near side of T0; a
    // Begin that read `Now()` instead would pass every other case here, whose times sit far apart.
    auto const files = QuietFiles();
    auto const witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    CHECK(witness.Started() == files.Fence());
    CHECK(witness.Started() != files.Now());
}

TEST_CASE("A TU saved during the compile is MovedDuringCompile, and names the TU", "[input-witness][1650]")
{
    // The incident: the save landed after the key and before the launcher returned.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Tu }, "tu-v2", 2'500, 1);
    CHECK(witness.AfterCompile({}, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = std::string { Tu } });
    // The manifest still gets the keyed content, not the saved one.
    CHECK(witness.KeyedHash(Tu) == std::optional<std::string> { "tu-v1" });
}

TEST_CASE("Same bytes, new identity is still moved", "[input-witness][1650]")
{
    // Review focus 4. A content-only bracket would call this stable; an interleaving (V2 read by
    // the compiler, then V1 restored) cannot be excluded, so it is not.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Header }, "h-v1", 10, 99); // same bytes, same old change time, new id
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedDuringCompile);
}

TEST_CASE("A header deleted during the compile is MovedDuringCompile", "[input-witness][1650]")
{
    // Review focus 3.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.identities.erase(std::string { Header });
    CHECK(witness.AfterCompile({}, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = std::string { Header } });
}

TEST_CASE("A same-size, same-id rewrite after the snapshot is seen through its change time", "[input-witness][1650]")
{
    // Size and file id unchanged, so only the change time can tell. It must: every entry's
    // T1' change time is before T0, and any rewrite after T1' is stamped at or after T0.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.identities[std::string { Header }].changeTime = 2'000;
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedDuringCompile);
}

TEST_CASE("A header changed while the key was taken is MovedBeforeKey", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.Set(std::string { Header }, "h-v2", 1'500, 2); // after T0 = 1007, before the snapshot
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey()
          == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
    CHECK(witness.AfterCompile({}, files).stability == InputStability::MovedBeforeKey);
}

TEST_CASE("A TU whose content moved between T0 and the snapshot is MovedBeforeKey, without a clock", "[input-witness][1650]")
{
    // A filesystem whose clock lags this machine's (a network share) stamps the change BEFORE T0;
    // the TU's own T0/T1' comparison still sees it.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.Set(std::string { Tu }, "tu-v2", 10, 1); // same size, change time and id: only the hash differs
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Tu } });
}

TEST_CASE("A change time equal to T0 counts as moved, one tick earlier does not", "[input-witness][1650]")
{
    // Review focus 5: the boundary is >=, so a change in T0's own coarse tick fails closed.
    // T0 is the fence, 1007: neither it nor 1006 lies on a coarser stamp grid, so both are compared
    // at one-tick granularity and the boundary is exactly T0.
    for (auto const [changeTime, expected]: { std::pair { FileInstant { 1'007 }, InputStability::MovedBeforeKey },
                                              std::pair { FileInstant { 1'006 }, InputStability::Stable } })
    {
        auto files = QuietFiles();
        files.Set(std::string { Header }, "h-v1", changeTime, 2);
        auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
        files.now = 2'000;
        witness.Snapshot(HeaderOnly, files);
        CHECK(witness.AtKey().stability == expected);
    }
}

TEST_CASE("A file that cannot be read at the snapshot is Unwitnessed", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.hashes.erase(std::string { Header });
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Header } });
}

TEST_CASE("With hashing off nothing is hashed and an unreadable hash is not Unwitnessed", "[input-witness][1650]")
{
    // Direct mode off: no manifest, so the witness guards the STORE by identity alone.
    auto files = QuietFiles();
    files.hashes.clear();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::No);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey().stability == InputStability::Stable);
    CHECK_FALSE(witness.KeyedHash(Header).has_value());
    CHECK(files.hashCalls == 0);
}

TEST_CASE("A late dependency changed after T0 is MovedDuringCompile; one unreadable is Unwitnessed", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot({}, files);
    files.Set("/r/src/late.hpp", "late", 1'500, 7);
    std::vector<std::string> const late { "/r/src/late.hpp" };
    CHECK(witness.AfterCompile(late, files)
          == StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = "/r/src/late.hpp" });
    std::vector<std::string> const missing { "/r/src/missing.hpp" };
    CHECK(witness.AfterCompile(missing, files)
          == StabilityFinding { .stability = InputStability::Unwitnessed, .path = "/r/src/missing.hpp" });
}

TEST_CASE("A whole-second stamp in T0's own second counts as moved", "[input-witness][1650]")
{
    // A filesystem that stamps whole seconds (HFS+, ext4 with 128-byte inodes) stamps an edit made
    // at 5.9 s as 5.0 s; with T0 = 5.7 s a raw >= would call it older than the invocation. The
    // comparison is ChangedAtOrAfter, which floors T0 to the stamp's own granularity (Ruling 5).
    FakeInputFiles files;
    files.now = 5'700'000'000;
    files.Set(std::string { Tu }, "tu-v1", 10, 1);
    files.Set(std::string { Header }, "h-v2", 5'000'000'000, 2); // an odd whole second: 1 s granular
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 6'000'000'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey()
          == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
}

TEST_CASE("Every verdict decides the STORE and the manifest exactly", "[input-witness][1650]")
{
    CHECK(RowOf(InputStability::Stable).store == StoreObject::Yes);
    CHECK(RowOf(InputStability::Stable).manifest == KeepManifest::Yes);
    CHECK(RowOf(InputStability::MovedDuringCompile).store == StoreObject::No);
    CHECK(RowOf(InputStability::MovedDuringCompile).manifest == KeepManifest::Yes);
    CHECK(RowOf(InputStability::MovedBeforeKey).store == StoreObject::No);
    CHECK(RowOf(InputStability::MovedBeforeKey).manifest == KeepManifest::No);
    CHECK(RowOf(InputStability::Unwitnessed).store == StoreObject::No);
    CHECK(RowOf(InputStability::Unwitnessed).manifest == KeepManifest::No);
    // And what the statistics record when the object is not stored: a race with an editor is
    // INPUTS-MOVED, while an input that could not be observed is usually a lasting condition
    // (an unreadable file), which the next build would meet again, so it is UNCACHEABLE.
    CHECK(RowOf(InputStability::Stable).outcome == Outcome::Miss);
    CHECK(RowOf(InputStability::MovedDuringCompile).outcome == Outcome::InputsMoved);
    CHECK(RowOf(InputStability::MovedBeforeKey).outcome == Outcome::InputsMoved);
    CHECK(RowOf(InputStability::Unwitnessed).outcome == Outcome::Uncacheable);
    CHECK(DescribeInstability({ .stability = InputStability::MovedDuringCompile, .path = "/r/a.cpp" })
          == "an input changed during the compile: /r/a.cpp");
    CHECK(DescribeInstability({ .stability = InputStability::Stable, .path = "" }).empty());
}

TEST_CASE("The witnessed content answers keyed hashes, and guards a late path by the clock", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.Set(std::string { Header }, "h-v2", 2'500, 2); // saved after the snapshot
    files.Set("/r/src/old.hpp", "old", 10, 8);           // late, untouched
    files.Set("/r/src/new.hpp", "new", 1'500, 9);        // late, changed after T0
    WitnessedManifestContent const content { witness, files };
    CHECK(content.HashAsKeyed(Header) == std::expected<std::string, ManifestFault> { "h-v1" });
    CHECK(content.HashAsKeyed("/r/src/old.hpp") == std::expected<std::string, ManifestFault> { "old" });
    CHECK(content.HashAsKeyed("/r/src/new.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::ChangedDuringInvocation) });
    CHECK(content.HashAsKeyed("/r/src/none.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::Unreadable) });
}

TEST_CASE("Two checkouts with equal content record byte-identical manifests and keys", "[input-witness][1650]")
{
    // Spec invariant 1, at the seam: identities differ in every field between the two
    // checkouts (and between two machines), and nothing of them may reach the record.
    FastCache::Testing::ScratchDirectory const scratchA { "fc-witness-share-a" };
    FastCache::Testing::ScratchDirectory const scratchB { "fc-witness-share-b-deeper" };
    std::array<std::string, 2> encoded;
    std::array<std::string, 2> manifestKeys;
    for (auto const index: { std::size_t { 0 }, std::size_t { 1 } })
    {
        auto const& scratch = index == 0 ? scratchA : scratchB;
        auto const shift = static_cast<FileInstant>(index); // a change time is signed
        scratch.Write("src/h.hpp", "#pragma once\nint h();\n");
        scratch.Write("src/a.cpp", "#include \"h.hpp\"\nint a() { return h(); }\n");
        auto const& root = scratch.Path();
        FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
        auto const source = (root / "src" / "a.cpp").string();
        auto const header = (root / "src" / "h.hpp").string();
        auto const resolvedSource = ResolveAgainst(source, root.string(), layout);
        auto const resolvedHeader = ResolveAgainst(header, root.string(), layout);

        FakeInputFiles files;
        auto const disk = MakeDiskInputFiles();
        files.identities[resolvedSource] = FileIdentity {
            .changeTime = 10 + shift, .size = 1, .volumeId = 100U + index, .fileIdHigh = 0, .fileIdLow = 1U + index
        };
        files.identities[resolvedHeader] = FileIdentity {
            .changeTime = 20 + shift, .size = 2, .volumeId = 100U + index, .fileIdHigh = 0, .fileIdLow = 3U + index
        };
        files.hashes[resolvedSource] = disk->Hash(resolvedSource);
        files.hashes[resolvedHeader] = disk->Hash(resolvedHeader);

        auto witness = InputWitness::Begin(resolvedSource, files, ContentHashing::Yes);
        files.now = 2'000;
        witness.Snapshot(ProjectDependencyPaths(std::vector<std::string> { header }, root.string(), layout), files);
        REQUIRE(witness.AtKey().stability == InputStability::Stable);
        WitnessedManifestContent const content { witness, files };
        auto const manifest = BuildManifest({ .sourcePath = source,
                                              .reportedDependencies = ReportedDependencies::Observed({ header }),
                                              .workingDirectory = root.string(),
                                              .toolchainStamp = "stamp",
                                              .objectKey = "objkey",
                                              .content = content },
                                            layout);
        REQUIRE(manifest.has_value());
        encoded[index] = EncodeManifest(*manifest);
        auto const token = CanonicalSourceToken(source, layout, root.string());
        REQUIRE(token.has_value());
        manifestKeys[index] = ComputeManifestKey(*token, { "-c", *token }, "stamp");
    }
    CHECK(encoded[0] == encoded[1]);
    CHECK(manifestKeys[0] == manifestKeys[1]);
}

TEST_CASE("A header whose identity moves while it is hashed is MovedBeforeKey", "[input-witness][1650]")
{
    // Snapshot rule 2: both identities are old, so only before != after can tell.
    auto files = QuietFiles();
    files.sequences[std::string { Header }] = { OldIdentity(2), OldIdentity(22) };
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey()
          == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
}

TEST_CASE("A TU absent at T0 and present at the snapshot is Unwitnessed", "[input-witness][1650]")
{
    // Snapshot rule 4a: nothing to compare the T1' identity with, and nothing dereferenced.
    auto files = QuietFiles();
    files.sequences[std::string { Tu }] = { std::nullopt, OldIdentity(1) };
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Tu } });
}

TEST_CASE("A TU replaced between T0 and the snapshot by the same bytes is MovedBeforeKey", "[input-witness][1650]")
{
    // Snapshot rule 4b: same hash, old change time; only the file id moved between T0 and T1'.
    auto files = QuietFiles();
    files.sequences[std::string { Tu }] = { OldIdentity(1), OldIdentity(11) };
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Tu } });
}

TEST_CASE("The witnessed content refuses a late path it can identify but not hash", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.identities["/r/src/late.hpp"] = OldIdentity(7); // identified, but no hash
    WitnessedManifestContent const content { witness, files };
    CHECK(content.HashAsKeyed("/r/src/late.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::Unreadable) });
}

TEST_CASE("The witnessed content refuses a late path whose identity moves while it is hashed", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    files.hashes["/r/src/late.hpp"] = "late";
    files.sequences["/r/src/late.hpp"] = { OldIdentity(7), OldIdentity(77) };
    WitnessedManifestContent const content { witness, files };
    CHECK(content.HashAsKeyed("/r/src/late.hpp")
          == std::expected<std::string, ManifestFault> { std::unexpected(ManifestFault::ChangedDuringInvocation) });
}

TEST_CASE("A value that is not a verdict answers its own row and never the nearest", "[input-witness][1650]")
{
    auto const& row = RowOf(InputStability::Last);
    CHECK(&row == &UnknownStabilityRow);
    CHECK(row.stability == InputStability::Last);
    CHECK(row.store == StoreObject::No);
    CHECK(row.manifest == KeepManifest::No);
    CHECK(row.sentence == "an unknown input verdict");
    CHECK(row.outcome == Outcome::Uncacheable);
    CHECK(DescribeInstability({ .stability = InputStability::Last, .path = "/r/a.cpp" })
          == "an unknown input verdict: /r/a.cpp");
}

TEST_CASE("A header that cannot be identified at the snapshot is Unwitnessed though it can be hashed",
          "[input-witness][1650]")
{
    // Snapshot rule 1, both identity arms at once: the hash is there, so only the identity arms can tell.
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.identities.erase(std::string { Header });
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Header } });
}

TEST_CASE("A header that vanishes while it is hashed is Unwitnessed with hashing on or off", "[input-witness][1650]")
{
    // Snapshot rule 1's `!after` arm alone. With hashing off the empty-hash arm cannot stand in.
    for (auto const hashing: { ContentHashing::Yes, ContentHashing::No })
    {
        auto files = QuietFiles();
        files.sequences[std::string { Header }] = { OldIdentity(2), std::nullopt };
        auto witness = InputWitness::Begin(std::string { Tu }, files, hashing);
        files.now = 2'000;
        witness.Snapshot(HeaderOnly, files);
        CHECK(witness.AtKey()
              == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Header } });
    }
}

TEST_CASE("A header that appears while it is hashed is Unwitnessed with hashing on or off", "[input-witness][1650]")
{
    // Snapshot rule 1's `!before` arm alone; the TU is unsequenced, so the header's queue is untouched
    // until the snapshot.
    for (auto const hashing: { ContentHashing::Yes, ContentHashing::No })
    {
        auto files = QuietFiles();
        files.sequences[std::string { Header }] = { std::nullopt, OldIdentity(2) };
        auto witness = InputWitness::Begin(std::string { Tu }, files, hashing);
        files.now = 2'000;
        witness.Snapshot(HeaderOnly, files);
        CHECK(witness.AtKey()
              == StabilityFinding { .stability = InputStability::Unwitnessed, .path = std::string { Header } });
    }
}

TEST_CASE("A second snapshot decides afresh and does not inherit the first one's finding", "[input-witness][1650]")
{
    auto files = QuietFiles();
    auto witness = InputWitness::Begin(std::string { Tu }, files, ContentHashing::Yes);
    files.Set(std::string { Header }, "h-v2", 1'500, 2); // after T0
    files.now = 2'000;
    witness.Snapshot(HeaderOnly, files);
    REQUIRE(witness.AtKey()
            == StabilityFinding { .stability = InputStability::MovedBeforeKey, .path = std::string { Header } });
    files.Set(std::string { Header }, "h-v1", 10, 2); // quiet again
    witness.Snapshot(HeaderOnly, files);
    CHECK(witness.AtKey() == StabilityFinding { .stability = InputStability::Stable, .path = "" });
    CHECK(witness.KeyedHash(Header) == std::optional<std::string> { "h-v1" });
}
