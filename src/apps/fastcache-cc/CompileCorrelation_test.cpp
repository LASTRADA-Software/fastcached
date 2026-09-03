// SPDX-License-Identifier: Apache-2.0
#include "CompileCorrelation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// The six covered inputs, so a case can vary exactly one of them.
struct Job
{
    std::string preprocessed { "int main(){return 0;}" };
    std::vector<std::string> args { "-O2", "-DNDEBUG" };
    std::string fingerprint { "gcc-14-x86_64-linux-gnu" };
    std::string sourceName { "tu.cpp" };
    std::string compileDir { "/home/ci/build" };
    std::string compileDirReplacement { "." };

    [[nodiscard]] std::string Digest() const
    {
        return CompileCorrelation(preprocessed, args, fingerprint, sourceName, compileDir, compileDirReplacement);
    }
};

} // namespace

TEST_CASE("A correlation is stable for the same compile", "[correlation]")
{
    // Deterministic, or the client's recomputation refuses every honest reply. This is
    // the property the whole mechanism sits on, so it is asserted rather than assumed.
    CHECK(Job {}.Digest() == Job {}.Digest());
}

TEST_CASE("Each covered input changes the correlation", "[correlation]")
{
    // One case per field, because a digest that ignores one of them cannot separate
    // two jobs differing only there -- and each of these four is a pair of jobs whose
    // CORRECT objects differ, which is what makes crossing them a wrong-object bug
    // rather than a harmless swap.
    auto const base = Job {}.Digest();

    SECTION("the preprocessed text")
    {
        Job other;
        other.preprocessed = "int main(){return 1;}";
        CHECK(other.Digest() != base);
    }

    SECTION("the argument vector")
    {
        // Two jobs over identical text at different optimisation levels produce
        // different objects, and nothing upstream of the reply distinguishes them.
        Job other;
        other.args = { "-O0", "-DNDEBUG" };
        CHECK(other.Digest() != base);
    }

    SECTION("the fingerprint")
    {
        // The subtle one. Same text, same flags, same name, different toolchain --
        // so the objects differ and a crossed reply is otherwise invisible. The
        // worker's RESOLVED compiler path is deliberately not covered, because the
        // client cannot know it and could not then verify anything.
        Job other;
        other.fingerprint = "clang-20-x86_64-linux-gnu";
        CHECK(other.Digest() != base);
    }

    SECTION("the source name")
    {
        // A compiler records the name of the file it was handed -- clang-cl and gcc
        // in the COFF/ELF `.file` symbol -- so two otherwise identical jobs really do
        // produce different objects. Seven bytes, per `CompileRequest`'s own note.
        Job other;
        other.sourceName = "other.cpp";
        CHECK(other.Digest() != base);
    }

    SECTION("the compilation directory")
    {
        // It becomes the left-hand side of a `-fdebug-prefix-map` argument on the line
        // the worker spawns, so two jobs differing only here produce objects whose
        // `DW_AT_comp_dir` differs -- #506's own defect, and crossing the replies would
        // reintroduce it underneath the fix.
        Job other;
        other.compileDir = "/home/ci/other";
        CHECK(other.Digest() != base);
    }

    SECTION("its replacement")
    {
        Job other;
        other.compileDirReplacement = "./sub";
        CHECK(other.Digest() != base);
    }

    SECTION("mapping nothing, against mapping to the current directory")
    {
        // The pair that matters most: a client that asked for no mapping and one that
        // asked for `.` get objects recording different directories, and the empty
        // spelling must not digest as though the fields were absent.
        Job other;
        other.compileDir = "";
        other.compileDirReplacement = "";
        CHECK(other.Digest() != base);
    }
}

TEST_CASE("An argument list is framed injectively, not joined", "[correlation]")
{
    // Issue #63's collision, reached through this digest. A grammar that ran the
    // pieces together -- or terminated them with a byte that can occur inside one --
    // gives these two the same digest, and then two unrelated compiles correlate
    // against each other's replies and the mechanism silently passes them.
    //
    // These are exactly the inputs #63 records, which is why they are the ones here:
    // the same total bytes, split at a different place.
    Job left;
    left.args = { "-Da", "b" };

    Job right;
    right.args = { "-D", "ab" };

    CHECK(left.Digest() != right.Digest());
}

TEST_CASE("A field boundary cannot be shifted into a neighbouring field", "[correlation]")
{
    // The same argument one level up: the boundary between two SCALARS must be as
    // firm as the one between two list items, or a name that ends where a fingerprint
    // begins collides with the pair that splits the other way.
    Job left;
    left.fingerprint = "gcc";
    left.sourceName = "14tu.cpp";

    Job right;
    right.fingerprint = "gcc14";
    right.sourceName = "tu.cpp";

    CHECK(left.Digest() != right.Digest());
}

TEST_CASE("An empty argument list is not the same as one empty argument", "[correlation]")
{
    // A build system can pass an empty argument, and a grammar that framed it as
    // nothing would make the two compiles correlate. `KeyDigest` emits a
    // length-prefixed piece per item, so an empty item is still an item.
    Job none;
    none.args = {};

    Job one;
    one.args = { "" };

    CHECK(none.Digest() != one.Digest());
}

TEST_CASE("A correlation is 32 hex characters", "[correlation]")
{
    // The wire carries it as a field, and the client compares it as text. Pinned so a
    // change to the underlying hash is a deliberate wire decision rather than a
    // surprise at a peer that still expects the old width.
    auto const digest = Job {}.Digest();
    REQUIRE(digest.size() == KeyDigest::HexLength);
    CHECK(std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}

TEST_CASE("A correlation shares no domain with a cache key", "[correlation]")
{
    // The schema tag is the domain separation, exactly as it is between `objkey-v*`
    // and `manifest-v*`. Folding the identical material under another tag must not
    // produce the same blob, or a value from one key space could be presented as a
    // correlation from the other.
    Job job;

    KeyDigest impostor { "objkey-v6" };
    impostor.Field(job.fingerprint);
    impostor.Field(job.sourceName);
    for (auto const& arg: job.args)
        impostor.Item(arg);
    impostor.Field(job.preprocessed);

    CHECK(impostor.ToHex() != job.Digest());
}
