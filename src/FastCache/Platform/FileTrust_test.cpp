// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <tests/ScratchPath.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>
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

TEST_CASE("FileTrust: the POSIX acquisition reports what the mode bits say", "[platform][filetrust][secret]")
{
    // The real acquisition, on the platform that has it. The verdict is asserted
    // through the composed `SecretFileExposure`, because what an operator gets is
    // the composition and a record that is right while the composition is not
    // would be a test passing for the wrong reason.
    FastCache::Testing::ScratchDirectory const scratch { "fastcached-secret-mode" };

    auto const modeIs = [&scratch](char const* stem, ::mode_t mode) {
        scratch.Write(stem, "requirepass: hunter2\n");
        auto const path = scratch / stem;
        REQUIRE(::chmod(path.c_str(), mode) == 0);
        return path;
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

    SECTION("0640 owned by this test's own account is the owner's own group")
    {
        // Not owned by root -- these run unprivileged -- so the group grant is the
        // owner's own. The root-owned counterpart cannot be constructed without
        // privileges, which is why the delegation clause is asserted through
        // `ClassifySecretFile` above rather than only here.
        CHECK(FastCache::SecretFileExposure(modeIs("group.yaml", S_IRUSR | S_IWUSR | S_IRGRP))
              == SecretExposure::OwnersOwnGroup);
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

#endif
