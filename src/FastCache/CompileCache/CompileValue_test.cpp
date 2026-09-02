// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;
using PathCanon::Grammar;

TEST_CASE("CompileValue encode/decode round-trips object blob and regions")
{
    CompileValue v;
    v.objectBlob = { std::byte { 0x00 }, std::byte { 0xFF }, std::byte { 0x10 } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes, .bytes = "Note: including file: <SRCROOT>/a.h\r\n" });
    v.textRegions.push_back({ .grammar = Grammar::MsvcDiagnostics, .bytes = "<SRCROOT>/a.cpp(1): warning\r\n" });

    auto const bytes = EncodeCompileValue(v);
    auto const back = DecodeCompileValue(bytes);
    REQUIRE(back.has_value());
    CHECK(back->objectBlob == v.objectBlob);
    REQUIRE(back->textRegions.size() == 2);
    CHECK(back->textRegions[0].grammar == Grammar::ShowIncludes);
    CHECK(back->textRegions[0].bytes == v.textRegions[0].bytes);
    CHECK(back->textRegions[1].grammar == Grammar::MsvcDiagnostics);
    CHECK(back->textRegions[1].bytes == v.textRegions[1].bytes);
}

TEST_CASE("CompileValue with an empty object and no regions round-trips")
{
    CompileValue const v {};
    auto const bytes = EncodeCompileValue(v);
    auto const back = DecodeCompileValue(bytes);
    REQUIRE(back.has_value());
    CHECK(back->objectBlob.empty());
    CHECK(back->textRegions.empty());
}

TEST_CASE("DecodeCompileValue rejects truncated input")
{
    auto const bytes = EncodeCompileValue({ .objectBlob = { std::byte { 1 } }, .textRegions = {} });
    auto const truncated = std::span<std::byte const> { bytes.data(), bytes.size() - 1 };
    CHECK_FALSE(DecodeCompileValue(truncated).has_value());
}

TEST_CASE("DecodeCompileValue refuses another generation, and does not call it malformed")
{
    auto bytes = EncodeCompileValue({ .objectBlob = {}, .textRegions = {} });
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(static_cast<std::uint8_t>(bytes[0]) == CompileValueVersion);
    bytes[0] = std::byte { CompileValueVersion + 1 }; // what a newer build writes

    auto const decoded = DecodeCompileValue(bytes);
    REQUIRE_FALSE(decoded.has_value());

    // The code, not merely the refusal. Damaged bytes and a value written by another
    // build are different facts and the servers on this wire apply opposite policies
    // to them, so a decoder that answers `MalformedFrame` for both leaves every
    // caller unable to tell them apart -- which is how a node came to store a
    // foreign-generation value verbatim (#483).
    CHECK(decoded.error().code == ProtocolErrorCode::UnsupportedFeature);
    CHECK(decoded.error().context.contains("generation"));
}

TEST_CASE("DecodeCompileValue rejects empty input")
{
    CHECK_FALSE(DecodeCompileValue(std::span<std::byte const> {}).has_value());
}

namespace
{
/// A compile-value frame declaring `regionCount` regions and carrying
/// `trailingBytes` bytes after the header for them to be decoded from.
[[nodiscard]] std::vector<std::byte> FrameDeclaring(std::uint32_t regionCount, std::size_t trailingBytes)
{
    std::vector<std::byte> frame;
    frame.push_back(std::byte { 1 }); // version
    for ([[maybe_unused]] auto const i: { 0, 1, 2, 3 })
        frame.push_back(std::byte { 0 }); // objectLen = 0
    for (auto const shift: { 24, 16, 8, 0 })
        frame.push_back(static_cast<std::byte>((regionCount >> shift) & 0xFFU));
    frame.insert(frame.end(), trailingBytes, std::byte { 0 });
    return frame;
}
} // namespace

TEST_CASE("DecodeCompileValue refuses a region count the frame cannot supply")
{
    // Issue #267. `regionCount` is a `u32` read off the wire, and this reserved from
    // it before a single region had been read. A region costs five wire bytes at the
    // very least -- a grammar tag and a length prefix -- so the nine-byte frame below
    // declared four billion of them and asked for roughly 172 GB. Measured rather than
    // estimated: `sizeof(TextRegion)` is 40, and 0xFFFFFFFF * 40 is 171.8e9.
    //
    // Reachable from the daemon's STORE path, so `fastcached` itself was exposed and
    // not only the compile fleet, and from a worker's reply to the launcher. Run under
    // a 2 GiB address-space cap, the pre-fix decoder aborts on `std::bad_alloc`.
    auto const frame = FrameDeclaring(0xFFFFFFFFU, 0);
    REQUIRE(frame.size() == 9);

    auto const decoded = DecodeCompileValue(frame);
    // The refusal itself is the assertion, not merely "it did not crash": a decoder
    // that survived by luck would pass a crash test and still be reserving.
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ProtocolErrorCode::MalformedFrame);
    CHECK(decoded.error().context.contains("region count"));
}

TEST_CASE("DecodeCompileValue bounds a region count by the bytes actually left")
{
    // The boundary from both sides, so the guard is neither off by one nor a constant
    // somebody picked. With `n` bytes left and five needed per region, `n / 5` regions
    // is the most the frame could possibly carry.

    // Ten bytes left could carry two regions. Three is impossible, and is refused on
    // the count alone before anything is decoded.
    auto const impossible = DecodeCompileValue(FrameDeclaring(3, 10));
    REQUIRE_FALSE(impossible.has_value());
    CHECK(impossible.error().context.contains("region count exceeds"));

    // Two is exactly achievable, and those ten zero bytes really are two regions --
    // grammar tag 0 is `ShowIncludes` and a zero length is empty text, five bytes
    // each. So the guard admits precisely what the frame can carry: it refuses
    // impossible claims, and is not merely a ceiling on large ones.
    auto const achievable = DecodeCompileValue(FrameDeclaring(2, 10));
    REQUIRE(achievable.has_value());
    CHECK(achievable->textRegions.size() == 2);
    CHECK(achievable->textRegions[0].grammar == Grammar::ShowIncludes);
    CHECK(achievable->textRegions[0].bytes.empty());

    // And zero regions with nothing left is the ordinary empty value.
    CHECK(DecodeCompileValue(FrameDeclaring(0, 0)).has_value());
}

TEST_CASE("MinRegionBytes tracks the encoder rather than a comment")
{
    // The guard's per-element minimum is derived by hand from `EncodeCompileValue`'s
    // loop, which is comment discipline. This pins it structurally: the cost of one
    // EMPTY region is measured from the encoder itself, so a field added to that loop
    // fails here rather than quietly leaving the guard weaker than the format it
    // guards.
    CompileValue const empty {};
    CompileValue one;
    one.textRegions.push_back(TextRegion { .grammar = Grammar::ShowIncludes, .bytes = {} });

    CHECK(EncodeCompileValue(one).size() - EncodeCompileValue(empty).size() == 5);
}

TEST_CASE("CanonicalStoredValue tells a foreign generation from bytes that are not a value")
{
    // What a launcher sends: absolute paths plus the roots to rewrite them against.
    CompileValue produced;
    produced.objectBlob = { std::byte { 0x01 } };
    produced.textRegions.push_back(
        TextRegion { .grammar = Grammar::ShowIncludes, .bytes = "Note: including file: /home/dev/proj/inc/a.hpp\n" });
    auto const wire = EncodeCompileValue(produced);

    SECTION("this generation is canonicalized")
    {
        auto const canonical = CanonicalStoredValue(wire, "/home/dev/proj", "/home/dev/proj/build");
        REQUIRE(canonical.outcome == CanonicalizationOutcome::Canonicalized);
        auto const stored = DecodeCompileValue(canonical.bytes);
        REQUIRE(stored.has_value());
        REQUIRE(stored->textRegions.size() == 1);
        CHECK(stored->textRegions[0].bytes == "Note: including file: <SRCROOT>/inc/a.hpp\n");
    }

    SECTION("another generation is named, and carries nothing to store")
    {
        // The whole of the mixed-version hazard in one arrangement: byte-identical
        // content, one leading byte apart. A server holding this cannot rewrite the
        // region, so the paths in it are the PRODUCER's -- and a server that stored
        // it anyway would put `/home/dev/proj/inc/a.hpp` into a shared cache under a
        // key every machine computes. Before this outcome existed the two SECTIONs
        // below were one `std::nullopt` and no caller could separate them.
        auto foreign = wire;
        foreign[0] = std::byte { CompileValueVersion + 1 };

        auto const canonical = CanonicalStoredValue(foreign, "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::ForeignGeneration);
        CHECK(canonical.generation == CompileValueVersion + 1);
        // Nothing to store: a server has no bytes it may write, which is what makes
        // "store it verbatim" unavailable rather than merely discouraged.
        CHECK(canonical.bytes.empty());
    }

    SECTION("bytes that are not a value at all stay the caller's own policy")
    {
        auto truncated = std::span<std::byte const> { wire.data(), wire.size() - 1 };
        auto const canonical = CanonicalStoredValue(truncated, "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::NotACompileValue);
        CHECK(canonical.generation == 0);
        CHECK(canonical.bytes.empty());
    }

    SECTION("an empty value is not a foreign generation")
    {
        // There is no leading byte to have read, so this cannot be a generation
        // claim -- and reporting one would name a generation nobody wrote.
        auto const canonical = CanonicalStoredValue({}, "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::NotACompileValue);
        CHECK(canonical.generation == 0);
    }
}

TEST_CASE("Canonicalizing a stored value twice is not a second rewrite")
{
    // A node with an upstream relies on this: it stores canonically and forwards,
    // and the daemon behind it runs the same recipe again.
    CompileValue produced;
    produced.textRegions.push_back(
        TextRegion { .grammar = Grammar::ShowIncludes, .bytes = "Note: including file: /home/dev/proj/inc/a.hpp\n" });

    auto const once = CanonicalStoredValue(EncodeCompileValue(produced), "/home/dev/proj", "/home/dev/proj/build");
    REQUIRE(once.outcome == CanonicalizationOutcome::Canonicalized);
    auto const twice = CanonicalStoredValue(once.bytes, "/home/dev/proj", "/home/dev/proj/build");
    REQUIRE(twice.outcome == CanonicalizationOutcome::Canonicalized);
    CHECK(twice.bytes == once.bytes);
}

// ---------------------------------------------------------------------------
// The canonicalization conformance vector -- what makes `CompileValueVersion` a
// statement about BEHAVIOUR rather than about a byte layout.
// ---------------------------------------------------------------------------

namespace
{

/// One case of the conformance corpus: what a producer captured, the roots it
/// captured it under, and the roots a consumer localizes it back into.
///
/// The roots are `string_view`s rather than a `PathCanon::Layout`, so the whole
/// corpus is a `constexpr` table and adding a case is adding a row. Layouts are
/// built from them at the point of use.
struct ConformanceCase
{
    std::string_view name;               ///< What this row is here to pin.
    std::string_view producerSourceRoot; ///< `<SRCROOT>` on the producing machine.
    std::string_view producerBuildTree;  ///< `<BUILDTREE>` on the producing machine.
    std::string_view consumerSourceRoot; ///< `<SRCROOT>` on the consuming machine.
    std::string_view consumerBuildTree;  ///< `<BUILDTREE>` on the consuming machine.
    std::string_view text;               ///< The captured region, exactly as a driver wrote it.
    Grammar grammar;                     ///< Which grammar locates path spans in `text`.
};

/// The corpus the stored-value contract is digested over.
///
/// Chosen so that every rule the canonicalization spec actually has moves the
/// digest if it moves: each grammar, both separator styles, a nested build tree, a
/// drive-relative root, a UNC root, a bare root, case folding, a path under neither
/// root (which must survive verbatim), text that already carries tokens (which must
/// be left alone, or canonicalizing twice is a second rewrite), both line endings,
/// a final line without one, and an empty region.
///
/// A row is cheap and a rule nothing covers is invisible here, so err towards
/// adding one. What the corpus must NOT do is depend on the host: every entry point
/// in `PathCanon` derives its conventions from the layout rather than from the
/// running binary, so this digest is the same on Windows, Linux and macOS -- and a
/// change that broke that would make a Windows and a POSIX server disagree about a
/// value they both hold, which is the whole failure this vector exists to catch.
/// The corpus, in the order it is digested.
constexpr std::array ConformanceCorpus {
    ConformanceCase { .name = "posix showIncludes under both roots",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "Note: including file: /home/dev/proj/inc/a.hpp\n"
                              "Note: including file:  /home/dev/proj/build/gen/cfg.hpp\n"
                              "Note: including file: /usr/include/stdio.h\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "windows showIncludes, mixed case, CRLF",
                      .producerSourceRoot = R"(C:\src\Proj)",
                      .producerBuildTree = R"(C:\src\Proj\out)",
                      .consumerSourceRoot = R"(D:\work\proj)",
                      .consumerBuildTree = R"(D:\work\proj\build)",
                      .text = "Note: including file: c:\\SRC\\proj\\inc\\A.hpp\r\n"
                              "Note: including file: C:\\src\\Proj\\out\\gen\\cfg.hpp\r\n"
                              "Note: including file: C:\\Program Files\\MSVC\\include\\vector\r\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "showIncludes final line with no newline",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "Note: including file: /home/dev/proj/z.h",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "showIncludes already carrying tokens is left alone",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "Note: including file: <SRCROOT>/inc/a.hpp\n"
                              "Note: including file: <BUILDTREE>/gen/cfg.hpp\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "msvc diagnostics, line and column",
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(E:\ci\proj)",
                      .consumerBuildTree = R"(E:\ci\proj\out)",
                      .text = "C:\\src\\proj\\a.cpp(17): warning C4100: unreferenced\r\n"
                              "C:\\src\\proj\\a.cpp(17,9): note: see reference\r\n"
                              "cl : Command line warning D9002\r\n",
                      .grammar = Grammar::MsvcDiagnostics },
    ConformanceCase { .name = "gcc depfile, target, continuations and escaped space",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "/home/dev/proj/build/a.o: /home/dev/proj/a.cpp \\\n"
                              "  /home/dev/proj/inc/a\\ b.hpp \\\n"
                              "  /usr/include/stdio.h\n"
                              "\n"
                              "/home/dev/proj/inc/a\\ b.hpp:\n",
                      .grammar = Grammar::GccDepfile },
    ConformanceCase { .name = "depfile under a drive-relative root",
                      .producerSourceRoot = R"(C:src\proj)",
                      .producerBuildTree = R"(C:src\proj\out)",
                      .consumerSourceRoot = R"(D:\work\proj)",
                      .consumerBuildTree = R"(D:\work\proj\out)",
                      .text = "C:src\\proj\\out\\a.obj: C:src\\proj\\a.cpp\n",
                      .grammar = Grammar::GccDepfile },
    ConformanceCase { .name = "showIncludes under a UNC root",
                      .producerSourceRoot = R"(\\build01\share\proj)",
                      .producerBuildTree = R"(\\build01\share\proj\out)",
                      .consumerSourceRoot = R"(C:\local\proj)",
                      .consumerBuildTree = R"(C:\local\proj\out)",
                      .text = "Note: including file: \\\\build01\\share\\proj\\inc\\a.hpp\r\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "a bare root produces, and a bare root consumes",
                      // A bare root IS its own trailing separator, so it is the
                      // one shape where the join has a second separator to think
                      // about. It reaches the digest on BOTH sides deliberately:
                      // producing was covered and consuming was not, and a
                      // plausible "collapse the double separator" edit to
                      // `JoinLocalized` therefore passed the entire suite while
                      // changing what every consumer replays. That is the drift
                      // this vector exists to see, and a corpus that only
                      // produces cannot see it.
                      .producerSourceRoot = "/",
                      .producerBuildTree = "/build",
                      .consumerSourceRoot = "/",
                      .consumerBuildTree = "/out",
                      .text = "Note: including file: /inc/a.hpp\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "a bare drive root consumes",
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(D:\)",
                      .consumerBuildTree = R"(D:\out)",
                      .text = "Note: including file: C:\\src\\proj\\inc\\a.hpp\r\n",
                      .grammar = Grammar::ShowIncludes },
    ConformanceCase { .name = "empty region",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "",
                      .grammar = Grammar::MsvcDiagnostics },
};

/// One generation of the stored-value contract: the `CompileValueVersion` it was
/// written under, and the digest the corpus above produces under it.
///
/// The table is what makes the version byte a claim about behaviour. Change how a
/// path span is found or rewritten, or how a value is framed, and the live digest
/// stops matching this generation's row -- the only way back to green is to bump
/// `CompileValueVersion` and add a row, which is exactly what a mixed-version fleet
/// needs to happen. A digest alone would not do it: the behaviour and the golden
/// value are one edit two hunks apart, so moving both leaves the suite green and
/// every server still calling itself generation N. Pairing the digest with the
/// version is what closes that, and it is why the rows for retired generations
/// stay -- the live digest must reproduce none of them, so putting an old version
/// byte back is caught whatever the golden says.
struct StoredValueGeneration
{
    std::string_view digest; ///< What the live corpus yields under that generation.
    std::uint8_t version;    ///< The `CompileValueVersion` it was written under.
};

/// Every generation of the stored-value contract this build knows about.
///
/// Generation 1 is the first, so there is nothing retired yet. That is not a
/// standing property: it holds only until somebody bumps the byte, and the rows
/// below are where the retired ones then live.
/// @return The table, newest last.
constexpr std::array StoredValueGenerations {
    StoredValueGeneration { .digest = "be1728170060f3f786faa7084585a7035385b9a6ab888cb386bbb63c89c72f5c", .version = 1 },
};

/// The generation table's row for @p version, if it has one.
///
/// Returns the ROW rather than an iterator into the table, because a
/// `std::array` iterator is a raw pointer on libstdc++ and libc++ and a class type
/// on MSVC's standard library -- so the `const auto *const` spelling the analyser
/// asks for on one host does not compile on another. A value is the one shape
/// every standard library agrees about.
///
/// @param version A `CompileValueVersion` value.
/// @return The row, or none when the table has no generation of that number.
[[nodiscard]] std::optional<StoredValueGeneration> GenerationRow(std::uint8_t version)
{
    for (auto const& row: StoredValueGenerations)
        if (row.version == version)
            return row;
    return std::nullopt;
}

/// Digest the whole stored-value contract over the conformance corpus.
///
/// Each field is length-prefixed through `WireFields`, which is this tree's one
/// definition of that framing — and it needs framing for the reason recorded at the
/// compiler identity: concatenated, `("ab", "c")` and `("a", "bc")` digest alike, so
/// a change that moved a byte from one field to its neighbour would not be seen.
///
/// A row's `name` is deliberately NOT hashed. It is a label for a human reading a
/// failure, so hashing it would make renaming one — a purely editorial change — fail
/// this test with a message telling the author to bump `CompileValueVersion`, which
/// is the one action this design exists to make hard.
///
/// @return The hex digest.
[[nodiscard]] std::string ConformanceDigest()
{
    std::vector<std::byte> material;

    auto const append = [&material](std::span<std::byte const> field) {
        auto const framed = WireFields::Encode({ field });
        material.insert(material.end(), framed.begin(), framed.end());
    };
    auto const appendText = [&append](std::string_view text) {
        append(AsBytes(text));
    };

    for (auto const& row: ConformanceCorpus)
    {
        // A real object blob, because the contract includes leaving it untouched.
        CompileValue produced;
        produced.objectBlob = { std::byte { 0x7F }, std::byte { 0x45 }, std::byte { 0x4C }, std::byte { 0x46 } };
        produced.textRegions.push_back(TextRegion { .grammar = row.grammar, .bytes = std::string { row.text } });

        auto const canonical =
            CanonicalStoredValue(EncodeCompileValue(produced), row.producerSourceRoot, row.producerBuildTree);
        REQUIRE(canonical.outcome == CanonicalizationOutcome::Canonicalized);

        // The STORED bytes, framing included: what one server writes and another
        // reads is this exact byte string, so this is the thing two generations
        // have to agree about.
        append(canonical.bytes);

        auto const stored = DecodeCompileValue(canonical.bytes);
        REQUIRE(stored.has_value());

        // And the consumer's half. Localize is the inverse the producer's rewrite is
        // only useful through, so a change to it splits a fleet exactly as a change
        // to Canonicalize does.
        PathCanon::Layout const consumer { .sourceRoot = std::string { row.consumerSourceRoot },
                                           .buildTree = std::string { row.consumerBuildTree } };
        for (auto const& region: stored->textRegions)
            appendText(PathCanon::LocalizeRegion(region.bytes, region.grammar, consumer));
    }

    return HexDigest(Sha256::Hash(material));
}

} // namespace

TEST_CASE("The canonicalization spec is pinned to the generation byte that names it")
{
    auto const& rows = StoredValueGenerations;
    // An emptied table would pass every check below vacuously, and read exactly like
    // one that found nothing wrong.
    REQUIRE_FALSE(rows.empty());

    auto const live = ConformanceDigest();

    auto const current = GenerationRow(CompileValueVersion);
    {
        // Scoped, so this note appears only when it is the thing that failed.
        INFO("CompileValueVersion is " << static_cast<unsigned>(CompileValueVersion)
                                       << " and StoredValueGenerations has no row for it -- a bump adds a row");
        REQUIRE(current.has_value());
    }
    auto const& pinned = Unwrap(current);

    // The message names BOTH ways of arriving here, because they call for opposite
    // actions and only one of them is the interesting one. Editing the CORPUS moves
    // the digest without moving any behaviour, and the answer there is to repin this
    // generation's row. Editing the canonicalization or the framing moves what two
    // servers do with one value, and the answer there is a new generation. A message
    // naming only the second teaches whoever meets the first to bump the byte for
    // nothing, which retires every entry in the fleet to buy exactly that.
    INFO("the stored-value contract produced "
         << live << ", but generation " << static_cast<unsigned>(CompileValueVersion) << " is pinned to " << pinned.digest
         << ".\nIf you widened ConformanceCorpus to cover behaviour that was ALREADY in this generation, repin "
            "generation "
         << static_cast<unsigned>(CompileValueVersion) << " to " << live
         << ".\nAdding a PathCanon::Grammar is NOT that -- see the grammar-coverage case, which sends you here "
            "with a row to add and a bump to take"
         << ".\nIf you changed how a path span is found, rewritten, localized or framed, that is a new "
            "canonicalization spec: two servers on one wire at different builds must not both call themselves "
            "generation "
         << static_cast<unsigned>(CompileValueVersion)
         << " while rewriting a value differently. Bump CompileValueVersion and add a row carrying " << live << ".");
    CHECK(live == pinned.digest);

    // A version and its digest move together, so no two generations may carry
    // either -- which is what catches a bump being put back afterwards, the golden
    // re-pasted with it.
    for (auto const& row: rows)
    {
        if (row.version == CompileValueVersion)
            continue;
        INFO("the live contract reproduces retired generation " << static_cast<unsigned>(row.version)
                                                                << " -- its bump was reverted");
        CHECK(live != row.digest);
    }
}

namespace
{

/// Every grammar tag this build's decoder accepts, asked OF the decoder.
///
/// Derived rather than restated, and that is the whole point: a hand-kept list of
/// grammars would be exact about the ones it knows and silent about the one just
/// added, which is the shape of failure the corpus check below exists to prevent.
/// So each of the 256 possible tag bytes is offered to `DecodeCompileValue` in an
/// otherwise-valid frame, and the ones it takes are the answer.
///
/// @return The accepted tags, ascending.
[[nodiscard]] std::vector<std::uint8_t> AcceptedGrammarTags()
{
    std::vector<std::uint8_t> accepted;
    for (auto const tag: std::views::iota(0, 256))
    {
        // One empty region carrying this tag, built by the encoder so the frame is
        // valid in every respect except possibly the tag.
        CompileValue probe;
        probe.textRegions.push_back(
            TextRegion { .grammar = static_cast<Grammar>(static_cast<std::uint8_t>(tag)), .bytes = {} });
        if (DecodeCompileValue(EncodeCompileValue(probe)).has_value())
            accepted.push_back(static_cast<std::uint8_t>(tag));
    }
    return accepted;
}

} // namespace

TEST_CASE("The conformance corpus covers every grammar the decoder accepts")
{
    // A new `PathCanon::Grammar` IS a canonicalization-spec change, and one of the
    // nastier kinds: an older build meets the new tag, `IsKnownGrammar` refuses it,
    // the decoder calls that a malformed frame, and a server whose policy for
    // malformed bytes is "store it verbatim" then stores the producer's absolute
    // paths -- #483's own defect, arriving through a door the generation byte cannot
    // see. The digest only moves for behaviour the corpus exercises, so a grammar no
    // row covers is a spec change that ships under the same generation.
    //
    // `-Wswitch` on `IsKnownGrammar` already forces a DECODE decision for a new
    // enumerator. Nothing forced a CORPUS row, and this is that.
    auto const accepted = AcceptedGrammarTags();
    // Two empty sets agree perfectly: if the probe stopped producing decodable
    // frames this would pass while checking nothing.
    REQUIRE_FALSE(accepted.empty());

    for (auto const tag: accepted)
    {
        INFO("grammar tag "
             << static_cast<unsigned>(tag)
             << " decodes, but no ConformanceCorpus row exercises it -- so a change to how that grammar finds or "
                "rewrites path spans would not move the digest, and would ship under the current "
                "CompileValueVersion.\nAdd a row AND bump CompileValueVersion. Both: a new grammar is a new "
                "canonicalization spec, because an older build meets the tag and refuses it, so the row alone "
                "would repin the CURRENT generation and ship the change under a number that already means "
                "something else. The generation case's repin branch does not apply here.");
        CHECK(std::ranges::any_of(
            ConformanceCorpus, [tag](ConformanceCase const& row) { return static_cast<std::uint8_t>(row.grammar) == tag; }));
    }
}
