// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>

    #include <unistd.h>
#endif

#if defined(__linux__)
    #include <sys/wait.h>

    #include <cerrno>
    #include <cstdlib>

    #include <spawn.h>

extern char** environ;
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

    // --- The delegation clause against a REAL root-owned file, unprivileged -----
    //
    // #1127. The case above asserts the delegation clause through the real
    // acquisition only when the suite runs AS root; unprivileged, it takes the
    // other branch and the clause is exercised nowhere but a synthesised
    // `SecretFileFacts`. That covers the DECISION and says nothing about whether
    // `ObserveSecretFile` actually reports `administrativelyOwned` for a file the
    // kernel calls root-owned.
    //
    // An unprivileged user namespace closes that without privilege: `unshare -r`
    // maps the caller to uid 0, and a file created inside is genuinely `st_uid == 0`
    // -- which is the one fact the clause reads (`FileTrust.cpp`,
    // `.administrativelyOwned = info.st_uid == 0`). It is NOT real root, and that is
    // sufficient here for exactly that reason and would not be for a case needing
    // real privilege, such as `SeedConfigFile`'s privileged branch.
    //
    // Linux only, and deliberately not `!defined(_WIN32)`: macOS has no `unshare(1)`
    // and no user namespaces, so on that platform this is not a skip to report but a
    // case that should not exist.
    #if defined(__linux__)

namespace
{
/// Names the file the re-executed child writes what it measured into.
constexpr char const* NsReportEnv = "FASTCACHED_FILETRUST_NS_REPORT";

/// Leads the child's one-line report, so the parent can tell the child's own
/// output from anything else that could occupy that path.
constexpr char const* NsReportToken = "filetrust-ns";

/// Run @p argv to completion.
///
/// @param argv Argument vector; `argv[0]` is looked up on `PATH`.
/// @return The child's exit status; `128 + signal` for a child a signal ended; and
///         -1 only when it could not be started or could not be reaped at all.
///         Those are different outcomes: a missing `unshare(1)` is a skip and a
///         child that ran and failed is not. A signalled child gets its own band
///         rather than sharing -1, because "the facility is absent" and "it died"
///         send a reader to different places.
///
/// `waitpid` is restarted on `EINTR`. Without that a signal delivered to this
/// process -- a profiler's timer, a job-control stop -- reads as "could not be
/// started", which the caller turns into a SKIP: a host that CAN run this case
/// reporting that it cannot, which is the state collapse this case exists to avoid.
[[nodiscard]] int SpawnAndWait(std::vector<std::string> argv)
{
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (auto& argument: argv)
        raw.push_back(argument.data());
    raw.push_back(nullptr);

    ::pid_t child = 0;
    if (::posix_spawnp(&child, raw[0], nullptr, nullptr, raw.data(), environ) != 0)
        return -1;

    int status = 0;
    ::pid_t reaped = 0;
    do
        reaped = ::waitpid(child, &status, 0);
    while (reaped == -1 && errno == EINTR);

    if (reaped != child)
        return -1;
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace

// Spelled ONCE, and the second spelling is DERIVED rather than agreed: the parent
// passes this name to the re-executed child as a Catch2 test spec, and a drifted
// copy would select no case at all -- which arrives as an absent report and would
// read as "this host cannot", the one confusion this case is built to avoid. That
// used to be a macro; a macro makes the two spellings one token and makes the name
// invisible to `check-test-names`, which extracts the FIRST STRING LITERAL on the
// `TEST_CASE` line and would have taken the TAGS for the name. Asking Catch2 for the
// running case's own name gives the same guarantee and keeps the literal readable.
TEST_CASE("FileTrust: the delegation clause meets a real root-owned file", "[platform][filetrust][secret]")
{
    // --- the re-executed half, running inside the namespace ------------------
    //
    // It MEASURES and reports; it asserts nothing. One place decides, and it is
    // the parent -- a child that also asserted would spend its exit status on a
    // failure count the parent then has to interpret alongside the report, which
    // is two channels disagreeing about one fact.
    if (char const* const reportPath = std::getenv(NsReportEnv); reportPath != nullptr)
    {
        FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-ns" };
        auto const path = SecretFileAtMode(scratch, "delegated.yaml", S_IRUSR | S_IWUSR | S_IRGRP);

        struct ::stat info {};

        REQUIRE(::stat(path.c_str(), &info) == 0);

        // Written in ONE line at the end rather than incrementally: a half-written
        // report and a missing one would otherwise be different states the parent
        // has no way to separate.
        // A leading token and four integers. Parsed with `operator>>` rather than
        // `sscanf` not because varargs are banned -- `.clang-tidy` disables that
        // check deliberately for the POSIX calls this tree has to make -- but
        // because the stream gives two things a field count does not: the token
        // says the child wrote this rather than something else occupying the path,
        // and `fail()` separates a truncated record from a complete one.
        //
        // The MODE travels beside the verdict, and it is what makes the parent's
        // assertion DISCRIMINATE. `SecretExposure::None` is what the delegation
        // clause produces (`readableByGroup && administrativelyOwned`) AND what
        // "nothing else can read it" produces -- so a fixture that yielded `0600`
        // here would satisfy the parent while exercising no delegation at all,
        // which is a case that cannot fail for the reason it exists. The owner uid
        // is already carried for the mirror-image reason; the group grant is the
        // clause's other operand and was not.
        std::ofstream report { reportPath, std::ios::binary | std::ios::trunc };
        report << NsReportToken << ' ' << info.st_uid << ' ' << ::geteuid() << ' '
               << (info.st_mode & static_cast<::mode_t>(07777)) << ' '
               << static_cast<int>(FastCache::SecretFileExposure(path)) << '\n';

        // Closed and checked HERE rather than left to the destructor, whose failure
        // nothing observes. A write that failed would otherwise reach the parent as
        // a missing or truncated report and be named a broken test, which sends a
        // reader to the wrong file.
        report.close();
        REQUIRE(report.good());
        return;
    }

    // --- the parent half -----------------------------------------------------
    if (::geteuid() == 0)
        SKIP("already root, so the acquisition case above already meets a real root-owned file");

    // The PRECONDITION, measured in this run rather than inferred from the child's
    // silence. Without it an absent report has two causes -- the facility is
    // unavailable, or the child never selected the case -- and reporting the second
    // as a skip is a pass for a case that never ran, which is the defect this
    // ticket is about one level up.
    if (SpawnAndWait({ "unshare", "-r", "true" }) != 0)
        SKIP("unprivileged user namespaces are unavailable here (no unshare(1), a hardened kernel with "
             "kernel.unprivileged_userns_clone=0 or user.max_user_namespaces=0, or a container runtime "
             "refusing to nest), so the real acquisition cannot be shown a root-owned file on this host");

    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-ns-parent" };
    auto const reportPath = scratch / "report.txt";

    // `/proc/self/exe` rather than `argv[0]`, which Catch2 does not hand to a case
    // and which a test runner is free to spell relatively.
    std::error_code ec;
    auto const self = std::filesystem::read_symlink("/proc/self/exe", ec);
    REQUIRE_FALSE(ec);

    REQUIRE(::setenv(NsReportEnv, reportPath.c_str(), 1) == 0);
    auto const caseName = Catch::getResultCapture().getCurrentTestName();
    auto const childStatus = SpawnAndWait({ "unshare", "-r", self.string(), caseName });
    REQUIRE(::unsetenv(NsReportEnv) == 0);

    // BEFORE the first assertion that can fire, because a Catch2 `INFO` attaches
    // only to the assertions that FOLLOW it -- and the next one is the assertion
    // whose entire diagnosis is whether the child ran and died or ran and wrote
    // nothing. Stated after the `ifstream`, as it was, the one number that
    // separates those two was absent from exactly the failure it explains.
    INFO("child exit status " << childStatus);

    // The facility works -- the probe above said so -- so an absent report is a
    // broken test rather than an unsupported host, and it is named as one.
    std::ifstream reportFile { reportPath, std::ios::binary };
    REQUIRE_FALSE(reportFile.fail());

    std::string report;
    std::getline(reportFile, report);
    INFO("report: " << report);
    REQUIRE_FALSE(report.empty());

    std::istringstream fields { report };
    std::string token;
    unsigned long ownerUid = 0;
    unsigned long effectiveUid = 0;
    unsigned long mode = 0;
    int exposure = -1;
    fields >> token >> ownerUid >> effectiveUid >> mode >> exposure;

    // The token is what separates "the child wrote this" from any other content
    // that could end up at that path, and the stream state is what separates a
    // complete record from a truncated one.
    REQUIRE(token == NsReportToken);
    REQUIRE_FALSE(fields.fail());

    // The namespace mapped, or it did not. Distinguished rather than folded into
    // the assertion, because a run where the mapping silently failed would
    // otherwise report the OwnersOwnGroup verdict as a delegation-clause failure.
    if (ownerUid != 0 || effectiveUid != 0)
        SKIP("the user namespace did not map this caller to uid 0, so no root-owned file was created");

    // The file the child actually made, asserted before any verdict is drawn from
    // it. Without this the case passes over a `0600` file, where `None` says
    // nothing at all about the delegation clause -- the control the sibling 0640
    // section applies to the OWNER, applied here to the GROUP grant, which is the
    // clause's other operand.
    CHECK((mode & static_cast<unsigned long>(S_IRGRP)) != 0);
    CHECK((mode & static_cast<unsigned long>(S_IROTH)) == 0);

    // The coverage this ticket exists for: the REAL acquisition, over a real
    // `0640` file the kernel reports as root-owned, reaching the delegation clause.
    CHECK(exposure == static_cast<int>(SecretExposure::None));
}

    #endif

#endif
