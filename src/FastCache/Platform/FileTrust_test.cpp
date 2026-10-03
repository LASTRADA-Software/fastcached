// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <tests/ScopedUmask.hpp>
#include <tests/ScratchPath.hpp>

#if defined(_WIN32)
    #include <tests/AccessList.hpp>
#else
    #include <sys/stat.h>

    #include <unistd.h>
#endif

using FastCache::ClassifySecretFile;
using FastCache::SecretExposure;
using FastCache::SecretExposureHint;
using FastCache::SecretFileFacts;

// --- The rule, as a pure function over a synthesised record -----------------
//
// **This is how the Windows branch is covered on a host that is not Windows.**
// Reading POSIX mode bits and walking a DACL have nothing in common and neither
// executes on the other's platform, so the DECISION is split out and driven
// against constructed records. That is not the same as running the real
// acquisition and it is not claimed to be: it converts "untested" into "tested
// against a constructed input", which is the most this host can honestly give,
// and it is stated as reasoned rather than measured.

TEST_CASE("FileTrust: a secret file's verdict follows what the platform reported", "[platform][filetrust][secret]")
{
    SECTION("nothing else can read it")
    {
        CHECK(ClassifySecretFile({ .determined = true }) == SecretExposure::None);
        // Owned by root and readable by nobody else is the same answer -- the
        // owner reading their own file is not an exposure.
        CHECK(ClassifySecretFile({ .determined = true, .administrativelyOwned = true }) == SecretExposure::None);
    }

    SECTION("any account on the machine can read it")
    {
        CHECK(ClassifySecretFile({ .determined = true, .readableByAnyAccount = true }) == SecretExposure::AnyLocalAccount);

        // **World before group, and it is not arbitrary.** A 0644 file is both,
        // and the world grant is the one worth naming: `chmod o-r` is its remedy,
        // where reporting the group grant would send an operator to tighten
        // something that was not the exposure.
        CHECK(ClassifySecretFile({ .determined = true, .readableByAnyAccount = true, .readableByGroup = true })
              == SecretExposure::AnyLocalAccount);

        // And root ownership does not excuse a world grant. Only the GROUP clause
        // is conditional on it; folding the two would make `0644 root:root` pass.
        CHECK(ClassifySecretFile({ .determined = true, .readableByAnyAccount = true, .administrativelyOwned = true })
              == SecretExposure::AnyLocalAccount);
    }

    SECTION("a group grant is an exposure only when the owner is not administrative")
    {
        // The half that keeps this from becoming an alarm nobody reads.
        // `InlineCredentialRejection` tells operators to use "mode 0640, readable
        // by the account the service runs as", and the macOS package ships
        // `0640 root:_fastcached`. A rule condemning every group-readable file
        // would condemn the documented arrangement.
        CHECK(ClassifySecretFile({ .determined = true, .readableByGroup = true, .administrativelyOwned = true })
              == SecretExposure::None);

        // Owned by a user, the group is that user's own -- accounts they do not
        // answer for.
        CHECK(ClassifySecretFile({ .determined = true, .readableByGroup = true }) == SecretExposure::OwnersOwnGroup);
    }

    SECTION("a platform that would not answer says so")
    {
        // Its own outcome rather than folded into `None`. A record that reports
        // nothing must not read as "nobody else can read it" -- and the other
        // fields are deliberately set here, because a `determined == false` record
        // with contents is exactly what a half-filled acquisition would produce.
        CHECK(ClassifySecretFile({}) == SecretExposure::Undetermined);
        CHECK(ClassifySecretFile({ .determined = false, .readableByAnyAccount = true }) == SecretExposure::Undetermined);
    }

    SECTION("the Windows record shape is the one its acquisition produces")
    {
        // `ObserveSecretFile` on Windows can only ever report `determined` plus
        // `readableByAnyAccount`: a DACL does not separate a narrow group grant
        // from a broad one in the way the delegation clause needs, so it leaves
        // `readableByGroup` and `administrativelyOwned` false. Driving exactly
        // that shape is what covers the Windows verdict here.
        CHECK(ClassifySecretFile({ .determined = true, .readableByAnyAccount = false }) == SecretExposure::None);
        CHECK(ClassifySecretFile({ .determined = true, .readableByAnyAccount = true }) == SecretExposure::AnyLocalAccount);
    }
}

TEST_CASE("FileTrust: every exposure names its own remedy", "[platform][filetrust][secret]")
{
    // An alarm nobody can act on is the one that gets ignored, so each outcome
    // has to say what to DO -- and each remedy has to differ, or two different
    // mistakes arrive as one sentence describing neither.
    std::filesystem::path const path { "/etc/fastcached/fastcached.yaml" };

    auto const world = SecretExposureHint(path, SecretExposure::AnyLocalAccount);
    auto const group = SecretExposureHint(path, SecretExposure::OwnersOwnGroup);
    auto const unknown = SecretExposureHint(path, SecretExposure::Undetermined);

    for (auto const& text: { world, group, unknown })
    {
        INFO("hint: " << text);
        CHECK_FALSE(text.empty());
        // Names the file, or an operator with several cannot tell which.
        CHECK(text.contains(path.string()));
    }

    // Different remedies, and asserted to DIFFER: a widened message covering both
    // would pass a per-outcome check individually while telling an operator to fix
    // the wrong thing.
    CHECK(world != group);
    CHECK(world != unknown);
    CHECK(group != unknown);

#if !defined(_WIN32)
    CHECK(world.contains("chmod o-r"));
    CHECK(group.contains("chmod g-r"));
#else
    CHECK(world.contains("icacls"));
#endif

    // Nothing to say about a file that is fit.
    CHECK(SecretExposureHint(path, SecretExposure::None).empty());
}

TEST_CASE("FileTrust: a secret one account holds is restricted to that account, not to every service",
          "[platform][filetrust][secret]")
{
    // The remedy for an identity key. `SecretExposureHint`'s Windows line names no owner, so a
    // node run by a user that followed it could no longer read its own key: the remedy has to
    // set the list `SecureSecretFileForOwner` sets, and name the file.
    std::filesystem::path const path { "/var/lib/fastcache-node/node-key" };

    for (auto const exposure: { SecretExposure::AnyLocalAccount, SecretExposure::OwnersOwnGroup })
    {
        CAPTURE(exposure);
        auto const hint = FastCache::OwnerOnlySecretExposureHint(path, exposure);
        INFO("hint: " << hint);
        CHECK(hint.contains(path.string()));
        CHECK(hint.contains("restrict it to its owner"));
#if defined(_WIN32)
        // OWNER RIGHTS granted, the service grant absent, and inheritance cut.
        CHECK(hint.contains("*S-1-3-4:F"));
        CHECK_FALSE(hint.contains("S-1-5-6"));
        CHECK(hint.contains("/inheritance:r"));
#else
        CHECK(hint.contains("chmod go-rwx"));
#endif
    }

    CHECK(FastCache::OwnerOnlySecretExposureHint(path, SecretExposure::None).empty());
    CHECK(FastCache::OwnerOnlySecretExposureHint(path, SecretExposure::Undetermined)
          == SecretExposureHint(path, SecretExposure::Undetermined));
}

#if !defined(_WIN32)

namespace
{
/// Write a secret-bearing file into @p scratch at a given mode.
///
/// File scope rather than a lambda inside one case, because both POSIX cases
/// below need the same three lines and a third copy of them is how the content
/// string and the mode drift apart.
///
/// @param scratch Directory to write into.
/// @param stem File name inside it.
/// @param mode Mode to chmod it to.
/// @return The path written.
[[nodiscard]] std::filesystem::path SecretFileAtMode(FastCache::Testing::ScratchDirectory const& scratch,
                                                     char const* stem,
                                                     ::mode_t mode)
{
    scratch.Write(stem, "requirepass: hunter2\n");
    auto const path = scratch / stem;
    REQUIRE(::chmod(path.c_str(), mode) == 0);
    return path;
}
} // namespace

TEST_CASE("FileTrust: the POSIX acquisition reports what the mode bits say", "[platform][filetrust][secret]")
{
    // The real acquisition, on the platform that has it. The verdict is asserted
    // through the composed `SecretFileExposure`, because what an operator gets is
    // the composition and a record that is right while the composition is not
    // would be a test passing for the wrong reason.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-mode" };

    auto const modeIs = [&scratch](char const* stem, ::mode_t mode) {
        return SecretFileAtMode(scratch, stem, mode);
    };

    SECTION("0600 is fit")
    {
        CHECK(FastCache::SecretFileExposure(modeIs("private.yaml", S_IRUSR | S_IWUSR)) == SecretExposure::None);
    }

    SECTION("0644 is readable by any account")
    {
        // The ticket's own example: `--config /tmp/anything.yaml` carrying
        // `requirepass:`, accepted in silence whatever its mode.
        CHECK(FastCache::SecretFileExposure(modeIs("world.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))
              == SecretExposure::AnyLocalAccount);
    }

    SECTION("0640 is classified by WHO owns it, which is whoever ran this suite")
    {
        // The one section in this file whose SUBJECT changes with the runner, and
        // #870. The fixture creates `0640 <runner>:<group>`, so an unprivileged run
        // produces the owner's own group and a root run produces the administrator
        // delegation. BOTH verdicts are correct for the file that actually exists;
        // what was wrong was asserting one of them unconditionally, which made the
        // case pass or fail on who typed the command with nothing in the run saying
        // which.
        //
        // Root is not an exotic mode here. The suite runs as root in most CI
        // containers -- the trust case further down says so in its own comment --
        // and `SeedConfigFile`'s privileged branch is reachable no other way.
        //
        // ASSERTED in both modes rather than skipped in one, because as root this
        // is the only place the delegation clause meets a REAL root-owned file;
        // everywhere else it is driven from a synthesised `SecretFileFacts`. So a
        // root run covers strictly more than an unprivileged one, where a skip
        // would have made it cover less.
        //
        // This section used to say the root-owned counterpart "cannot be
        // constructed without privileges". That is false on Linux -- an
        // unprivileged user namespace (`unshare -r`) gives euid 0 and a file that
        // `stat` reports as `uid=0`, which is the only fact the clause reads, and
        // it is how #870 was reproduced on an unprivileged host. Reaching for it
        // HERE is #1127, because it has to degrade to an honest skip wherever the
        // facility is absent (macOS, hardened kernels) and that is a different
        // change from making this case root-safe.
        auto const path = modeIs("group.yaml", S_IRUSR | S_IWUSR | S_IRGRP);

        // The control that makes this root-SAFE rather than merely root-aware.
        //
        // The branch below is chosen from the CALLER's euid, while the verdict is
        // decided by the FILE's owner (`administrativelyOwned` is `st_uid == 0`).
        // Those are two different facts and nothing else here ties them together,
        // so without this a fixture that guessed its mode wrong would assert
        // against the wrong expectation in silence -- which is the failure this
        // ticket is about, one level up.
        struct ::stat info {};

        REQUIRE(::stat(path.c_str(), &info) == 0);
        REQUIRE((info.st_uid == 0) == (::geteuid() == 0));

        if (::geteuid() == 0)
        {
            // `0640 root:root`: an administrator delegating read to a service
            // account, which is what `InlineCredentialRejection` instructs and what
            // the macOS package ships. Fit, not exposed.
            CHECK(FastCache::SecretFileExposure(path) == SecretExposure::None);
        }
        else
        {
            // `0640 <user>:<group>`: the owner's own group, over accounts they do
            // not answer for.
            CHECK(FastCache::SecretFileExposure(path) == SecretExposure::OwnersOwnGroup);
        }
    }

    SECTION("a file that is not there is undetermined, not fit")
    {
        CHECK(FastCache::SecretFileExposure(scratch / "absent.yaml") == SecretExposure::Undetermined);
    }

    SECTION("the record and the verdict agree")
    {
        auto const path = modeIs("observed.yaml", S_IRUSR | S_IWUSR | S_IROTH);
        auto const facts = FastCache::ObserveSecretFile(path);
        CHECK(facts.determined);
        CHECK(facts.readableByAnyAccount);
        CHECK(ClassifySecretFile(facts) == FastCache::SecretFileExposure(path));
    }
}

TEST_CASE("FileTrust: securing a secret file takes read away from everyone but its owner", "[platform][filetrust][secret]")
{
    // #741, on the platform this host can actually run. The Windows arm applies a
    // protected access list instead and is asserted against a real installed MSI
    // by the `package-windows` CI job -- there is no way to exercise a DACL here,
    // and pretending otherwise is what a synthesised record is for elsewhere in
    // this file.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secure-secret" };

    auto const path = SecretFileAtMode(scratch, "exposed.yaml", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    // The state a packaged config arrives in, and the reason this function exists.
    // Asserted first so a later green cannot come from a file that was already fit.
    REQUIRE(FastCache::SecretFileExposure(path) == SecretExposure::AnyLocalAccount);

    REQUIRE(FastCache::SecureSecretFileForServices(path));

    // The MODE, not only the verdict. `SecureSecretFileForServices` REPORTS through
    // `SecretFileExposure`, so asserting only that is one function agreeing with
    // itself -- it would pass just as well if the call had changed nothing and the
    // predicate had been broken instead.
    struct ::stat info {};

    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & static_cast<::mode_t>(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) == 0);

    // And the owner still reads it, which is the half that distinguishes SECURED
    // from BROKEN: a daemon that cannot open its own configuration falls back to
    // built-in defaults in silence.
    CHECK((info.st_mode & static_cast<::mode_t>(S_IRUSR)) != 0);
    std::ifstream probe { path };
    CHECK(probe.is_open());

    CHECK(FastCache::SecretFileExposure(path) == SecretExposure::None);
}

TEST_CASE("FileTrust: securing a file that is not there fails rather than claiming success", "[platform][filetrust][secret]")
{
    // "I could not tell" and "nothing else can read it" have to lead to different
    // places, and this is the caller-facing end of that: a seed that could not
    // restrict what it wrote must not report that it did.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secure-absent" };
    CHECK_FALSE(FastCache::SecureSecretFileForServices(scratch / "absent.yaml"));
}

TEST_CASE("FileTrust: securing a secret for its owner takes read away from group and other", "[platform][filetrust][secret]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secure-owner" };

    // Group- AND world-readable, so the case cannot pass on a function that removed only one.
    auto const path = SecretFileAtMode(scratch, "node-key", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    REQUIRE(FastCache::SecretFileExposure(path) == SecretExposure::AnyLocalAccount);

    CHECK(FastCache::SecureSecretFileForOwner(path) == SecretExposure::None);

    // The MODE, for the reason the services case gives: the verdict alone is the function
    // agreeing with itself.
    struct ::stat info {};

    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & static_cast<::mode_t>(S_IRWXG | S_IRWXO)) == 0);
    CHECK((info.st_mode & static_cast<::mode_t>(S_IRUSR | S_IWUSR)) == static_cast<::mode_t>(S_IRUSR | S_IWUSR));
}

#endif

TEST_CASE("FileTrust: securing an absent secret for its owner answers that it could not tell",
          "[platform][filetrust][secret]")
{
    // `Undetermined`, never `None`: a caller that minted nothing must not be told it secured something.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secure-owner-absent" };
    CHECK(FastCache::SecureSecretFileForOwner(scratch / "absent") == SecretExposure::Undetermined);
}

#if defined(_WIN32)

TEST_CASE("FileTrust: securing a secret for its owner replaces a broad access list with an owner-only one",
          "[platform][filetrust][secret]")
{
    // The real access-list walk on the platform it is for. A test process owns what it creates,
    // so it may rewrite the list -- which is also why OWNER RIGHTS is what keeps it able to read
    // the file back afterwards.
    //
    // The file starts UNPROTECTED, carrying entries INHERITED from its directory -- `BUILTIN\Users`
    // read, which is what a file inherits under `%ProgramData%` -- so only a list applied WITH
    // protection drops them: one applied without it keeps every inherited entry beside its own.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secure-owner-dacl" };
    auto const directory = scratch / "inheriting";
    std::filesystem::create_directories(directory);
    REQUIRE(FastCache::Testing::ApplyAccessList(directory, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)"));
    {
        std::ofstream { directory / "node-key" } << "secret";
    }
    auto const path = directory / "node-key";
    REQUIRE(FastCache::Testing::AccessListOf(path).contains("ID;"));
    REQUIRE(FastCache::SecretFileExposure(path) == SecretExposure::AnyLocalAccount);

    CHECK(FastCache::SecureSecretFileForOwner(path) == SecretExposure::None);

    // The LIST, not only the verdict: the verdict asks only whether a broad principal may read,
    // and the services list would answer `None` too while granting every service on the machine.
    // `P` is the protection against the parent's entries; `AI` is what `SetNamedSecurityInfo`
    // stamps on every list it applies.
    CHECK(FastCache::Testing::AccessListOf(path) == "D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)");

    // And the owner still reads it, the half that tells SECURED from BROKEN.
    std::ifstream probe { path };
    CHECK(probe.is_open());
}

#endif

TEST_CASE("FileTrust: a file others may write is told apart from one only its owner may", "[platform][filetrust][secret]")
{
    // The question the state directory's walk asks of every entry: whoever OWNS a file, one another
    // account can rewrite holds contents that account chose. Both directions, on the real
    // filesystem, and the remedy names the entry.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-others-may-write" };
    auto const path = scratch / "formation";
    auto created = FastCache::CreateStateFile(path, FastCache::StateFile::Formation);
    REQUIRE(created.has_value());
    created->reset();
#if defined(_WIN32)
    REQUIRE(FastCache::Testing::ApplyAccessList(path, L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)"));
    CHECK(FastCache::OthersMayWrite(path) == std::expected<bool, std::error_code> { false });
    REQUIRE(FastCache::Testing::ApplyAccessList(path, L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)(A;;FW;;;BU)"));
    CHECK(FastCache::OthersMayWrite(path) == std::expected<bool, std::error_code> { true });
    CHECK(FastCache::OthersMayWriteRemedy(path).back().contains("/remove:g"));
#else
    CHECK(FastCache::OthersMayWrite(path) == std::expected<bool, std::error_code> { false });
    std::filesystem::permissions(path, std::filesystem::perms::group_write, std::filesystem::perm_options::add);
    CHECK(FastCache::OthersMayWrite(path) == std::expected<bool, std::error_code> { true });
    std::filesystem::permissions(path, std::filesystem::perms::group_write, std::filesystem::perm_options::remove);
    std::filesystem::permissions(path, std::filesystem::perms::others_write, std::filesystem::perm_options::add);
    CHECK(FastCache::OthersMayWrite(path) == std::expected<bool, std::error_code> { true });
    CHECK(FastCache::OthersMayWriteRemedy(path)
          == std::vector<std::string> { std::format("chmod go-w '{}'", path.string()) });
#endif
    CHECK(FastCache::OthersMayWriteRemedy(path).front().contains(path.string()));
}

TEST_CASE("FileTrust: a regular file is opened, and anything else is refused without blocking",
          "[platform][filetrust][secret]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-open-regular" };
    auto const file = scratch / "node-key";
    {
        auto stream = std::ofstream { file, std::ios::binary };
        stream << "key";
    }
    auto const opened = FastCache::OpenRegularFile(file);
    REQUIRE(opened.has_value());
    auto buffer = std::array<char, 4> {};
    CHECK(std::fread(buffer.data(), 1, buffer.size(), opened->get()) == 3);

    // Absent is its own answer, which is the one a caller may mint on.
    auto const absent = FastCache::OpenRegularFile(scratch / "absent");
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error().error == std::errc::no_such_file_or_directory);
    CHECK_FALSE(absent.error().notRegular);

    // An entry that is not a file is refused as that, never read.
    auto const directory = FastCache::OpenRegularFile(scratch.Path());
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().notRegular);
}

#if defined(_WIN32)

TEST_CASE("FileTrust: a secret created owner-only is protected from its first instant and opened by nobody else",
          "[platform][filetrust][secret]")
{
    // Access is decided at OPEN: a list applied after the create would leave readable every handle
    // opened in between. So the list is part of the create, and on Windows so is share mode 0.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-owner-only-create" };
    auto const directory = scratch / "inheriting";
    std::filesystem::create_directories(directory);
    REQUIRE(FastCache::Testing::ApplyAccessList(directory, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)"));
    auto const path = directory / "node-key";

    auto created = FastCache::CreateStateFile(path, FastCache::StateFile::Key);
    REQUIRE(created.has_value());
    // Before a byte is written, and while the creating handle is held: nothing inherited, and a
    // second open refused outright.
    auto const list = FastCache::Testing::AccessListOf(path);
    CHECK(list.starts_with("D:P"));
    CHECK_FALSE(list.contains("ID;"));
    CHECK_FALSE(list.contains("BU"));
    CHECK(FastCache::SecretFileExposure(path) == SecretExposure::None);
    {
        std::ifstream racer { path };
        CHECK_FALSE(racer.is_open());
    }
    created->reset();

    // Exclusive: a file that is there is refused, never truncated.
    auto const again = FastCache::CreateStateFile(path, FastCache::StateFile::Key);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == std::errc::file_exists);
}

TEST_CASE("FileTrust: a directory other accounts may add to is not its owner's alone", "[platform][filetrust][secret]")
{
    // `BUILTIN\Users` add-file and add-subdirectory: the list measured on this project's own
    // `%ProgramData%\fastcache-node`, and exactly the directory a planted key would be put in.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-directory-writers" };
    auto const directory = scratch / "state";
    std::filesystem::create_directories(directory);
    REQUIRE(FastCache::Testing::ApplyAccessList(directory, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;0x1200af;;;BU)"));
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::Others);

    // And the remedy names the directory in TWO commands, the conversion first, and prints them in
    // that order; every principal `DirectoryWritersOf` scans for is in the removal. Whether they
    // WORK is the next case's, on the inherited list where one command did not.
    auto const hint = FastCache::DirectoryWritersHint(directory, FastCache::DirectoryWriters::Others);
    auto const remedy = FastCache::DirectoryWritersRemedy(directory, FastCache::DirectoryWriters::Others);
    REQUIRE(remedy.size() == 2);
    CHECK(remedy[0] == std::format(R"(icacls "{}" /inheritance:d)", directory.string()));
    CHECK(remedy[1].starts_with(std::format(R"(icacls "{}" /remove:g)", directory.string())));
    CHECK(hint.find(remedy[0]) < hint.find(remedy[1]));
    CHECK(hint.contains(remedy[1]));
    for (auto const* const broad: { "*S-1-1-0", "*S-1-5-11", "*S-1-5-32-545", "*S-1-5-4", "*S-1-5-32-546" })
        CHECK(remedy[1].contains(broad));
}

TEST_CASE("FileTrust: the printed directory remedy, run as printed, restricts a directory that INHERITS its grants",
          "[platform][filetrust][secret]")
{
    // The live shape: `C:\ProgramData\fastcache-node\cluster` holds no grant of its own and
    // inherits `%ProgramData%`'s -- `Users` read and execute, and `Users` create-files and
    // create-folders on the containers. One `icacls /inheritance:d /remove:g` reported success
    // there and removed nothing (the removal ran before the conversion made the entries
    // explicit), so the next start refused with the same line: a loop. Built here the same way,
    // the remedy is RUN exactly as printed, and the directory judged afterwards.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-inherited-remedy" };
    auto const programData = scratch / "programdata";
    std::filesystem::create_directories(programData);
    REQUIRE(FastCache::Testing::ApplyAccessList(programData,
                                                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)"
                                                L"(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU)(A;CI;0x116;;;BU)"));
    auto const directory = programData / "cluster";
    std::filesystem::create_directories(directory);
    scratch.Write("programdata/cluster/node-key", "x");
    REQUIRE(FastCache::Testing::AccessListOf(directory).contains("ID;"));
    REQUIRE(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::Others);
    REQUIRE(FastCache::SecretFileExposure(directory / "node-key") != FastCache::SecretExposure::None);

    for (auto const& command: FastCache::DirectoryWritersRemedy(directory, FastCache::DirectoryWriters::Others))
    {
        CAPTURE(command);
        CHECK(FastCache::Testing::RunCommandLine(command) == std::optional<DWORD> { 0 });
    }
    CAPTURE(FastCache::Testing::AccessListOf(directory));
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::OwnerOnly);
    CHECK_FALSE(FastCache::Testing::AccessListOf(directory).contains("BU"));

    // And WHY a caller holding a secret there must speak first: the removal propagates, and takes
    // the read away from the file too -- so after the remedy nothing shows it was ever exposed.
    CHECK(FastCache::SecretFileExposure(directory / "node-key") == FastCache::SecretExposure::None);
}

TEST_CASE("FileTrust: a directory that lets other accounts only DELETE its entries is not its owner's alone",
          "[platform][filetrust][secret]")
{
    // The deletion half of planting: `FILE_DELETE_CHILD` alone lets `BUILTIN\Users` remove the key,
    // and a node that then finds none mints a new identity and falls out of its cluster, saying
    // only `minted`. No add-file right is granted, so only delete-child can make this `Others`.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-directory-delete-child" };
    auto const directory = scratch / "state";
    std::filesystem::create_directories(directory);
    REQUIRE(FastCache::Testing::ApplyAccessList(directory, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;;0x40;;;BU)"));
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::Others);

    // The control: the same list without the delete-child grant is the owner's alone.
    REQUIRE(FastCache::Testing::ApplyAccessList(directory, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)"));
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::OwnerOnly);
}

#endif

TEST_CASE("FileTrust: a file this process created is its own, and so is its scratch directory",
          "[platform][filetrust][secret]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-file-owner" };
    scratch.Write("mine", "x");
    CHECK(FastCache::FileOwnerOf(scratch / "mine").standing == FastCache::FileOwnerStanding::ThisProcess);
    CHECK_FALSE(FastCache::FileOwnerOf(scratch / "mine").name.empty());
    CHECK(FastCache::FileOwnerOf(scratch / "absent").standing == FastCache::FileOwnerStanding::Undetermined);
    CHECK(FastCache::DirectoryWritersOf(scratch.Path()) == FastCache::DirectoryWriters::OwnerOnly);
    CHECK(FastCache::DirectoryWritersOf(scratch / "absent") == FastCache::DirectoryWriters::Undetermined);
}

TEST_CASE("FileTrust: a link is judged as itself, never as what it points at", "[platform][filetrust][secret]")
{
    // A DANGLING link is where the two readings disagree: following it finds nothing and answers
    // `Undetermined`, while the entry itself is this process's -- so an owner read through the
    // link is caught here even though this account owns both ends of any link it can make.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-link-entry" };
    scratch.Write("file", "x");
    auto const link = scratch / "link";
    auto failure = std::error_code {};
    std::filesystem::create_symlink(scratch / "absent-target", link, failure);
    if (failure)
        SKIP("this account cannot create a symbolic link here (" << failure.message() << ")");

    CHECK(FastCache::IsLinkEntry(link));
    CHECK(FastCache::FileOwnerOf(link).standing == FastCache::FileOwnerStanding::ThisProcess);
    CHECK_FALSE(FastCache::IsLinkEntry(scratch / "file"));
    CHECK_FALSE(FastCache::IsLinkEntry(scratch / "absent"));
}

TEST_CASE("FileTrust: a directory created owner-only is, and one already there is left alone",
          "[platform][filetrust][secret]")
{
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-owner-only-directory" };
    auto const directory = scratch / "parent" / "state";
    REQUIRE(FastCache::CreateOwnerOnlyDirectory(directory).has_value());
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::OwnerOnly);
#if defined(_WIN32)
    CHECK(FastCache::Testing::AccessListOf(directory) == "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)");
#else
    CHECK((std::filesystem::status(directory).permissions() & std::filesystem::perms::all)
          == std::filesystem::perms::owner_all);

    // Already there: not re-created and not re-permissioned -- judging it is `DirectoryWritersOf`'s.
    std::filesystem::permissions(directory, std::filesystem::perms::all);
    REQUIRE(FastCache::CreateOwnerOnlyDirectory(directory).has_value());
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::Others);

    // The sticky bit is no exception: it stops others removing an entry, not creating one that is
    // not there yet -- a state file before its first write, or a temporary.
    std::filesystem::permissions(directory, std::filesystem::perms::all | std::filesystem::perms::sticky_bit);
    CHECK(FastCache::DirectoryWritersOf(directory) == FastCache::DirectoryWriters::Others);
    std::filesystem::permissions(directory, std::filesystem::perms::all);
    CHECK(FastCache::DirectoryWritersHint(directory, FastCache::DirectoryWriters::Others).contains("chmod go-w"));
#endif
}

#if !defined(_WIN32)
namespace
{
/// The permission bits of @p path.
/// @param path An existing entry.
/// @return Its mode's low nine bits.
[[nodiscard]] unsigned ModeOf(std::filesystem::path const& path)
{
    struct ::stat info {};

    REQUIRE(::lstat(path.c_str(), &info) == 0);
    return static_cast<unsigned>(info.st_mode) & 0777U;
}
} // namespace

TEST_CASE("FileTrust: every state file is created with exactly its row's mode, whatever the umask",
          "[platform][filetrust][secret]")
{
    // A permissive umask must not WIDEN a state file -- under umask 000 a create's 0666 would let
    // every account rewrite what the node acts on -- and a strict one must not narrow one the
    // service's account has to read. So the mode is the row's, set on the descriptor, under each.
    for (auto const mask: { 0000U, 0002U, 0022U, 0077U })
    {
        CAPTURE(mask);
        FastCache::Testing::ScopedUmask const scoped { static_cast<::mode_t>(mask) };
        FastCache::Testing::ScratchDirectory const scratch { "fastcached-state-file-modes" };
        for (auto const file: FastCache::Enumerators<FastCache::StateFile>())
        {
            auto const path = scratch / FastCache::StateFileName(file);
            CAPTURE(path.string());
            auto created = FastCache::CreateStateFile(path, file);
            REQUIRE(created.has_value());
            CHECK(ModeOf(path) == FastCache::StateFilePosixMode(file));
            created->reset();
        }
    }
    // The key alone is its owner's; nothing may be written by anybody else.
    CHECK(FastCache::StateFilePosixMode(FastCache::StateFile::Key) == 0600U);
    for (auto const file: FastCache::Enumerators<FastCache::StateFile>())
        CHECK((FastCache::StateFilePosixMode(file) & 0022U) == 0U);
}

TEST_CASE("FileTrust: a link where a file is expected is refused as not a file, never followed",
          "[platform][filetrust][secret]")
{
    // The FIFO half of the same rule -- an entry that would BLOCK an ordinary open -- is the node
    // key case's, which bounds its wait and releases a blocked reader; asserted here it would hang
    // the whole binary the day the open blocks again.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-open-link" };
    auto const target = scratch / "target";
    {
        auto stream = std::ofstream { target };
        stream << "x";
    }
    auto const link = scratch / "link";
    std::filesystem::create_symlink(target, link);
    auto const followed = FastCache::OpenRegularFile(link);
    REQUIRE_FALSE(followed.has_value());
    CHECK(followed.error().notRegular);
}
#endif
