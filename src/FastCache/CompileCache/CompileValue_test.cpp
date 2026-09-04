// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
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
    // The constant, not a literal: this helper exists to exercise the region-count
    // guard and needs a frame of the CURRENT generation to do it. The literal that
    // used to sit here was the byte's only anchor by accident, and a generation
    // bump then failed two tests that have nothing to do with versioning. The byte
    // is pinned deliberately instead, in its own case below.
    frame.push_back(std::byte { CompileValueVersion });
    for ([[maybe_unused]] auto const i: { 0, 1, 2, 3 })
        frame.push_back(std::byte { 0 }); // objectLen = 0
    for (auto const shift: { 24, 16, 8, 0 })
        frame.push_back(static_cast<std::byte>((regionCount >> shift) & 0xFFU));
    frame.insert(frame.end(), trailingBytes, std::byte { 0 });
    return frame;
}
} // namespace

TEST_CASE("The generation byte is pinned by value, not only by name")
{
    // A wire constant has TWO facts -- its name and its value -- and a symbol both
    // ends spell can only test the first. Every other case in this file compares
    // against `CompileValueVersion` or against `CompileValueVersion + 1`, so all of
    // them would still agree if the constant moved, while every already-deployed
    // launcher disagreed and nobody here could recompile them.
    //
    // So one case reads the raw byte. It is the anchor, not the code smell it looks
    // like, and it is expected to be edited by a deliberate generation bump -- which
    // is exactly the moment somebody should have to think about it.
    //
    // Generation 2 since #547.
    auto const encoded = EncodeCompileValue(CompileValue {});
    REQUIRE_FALSE(encoded.empty());
    CHECK(encoded.front() == std::byte { 2 });
}

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

    SECTION("opaque bytes are not a foreign generation just for starting with one")
    {
        // The regression for the defect the NODE's tests caught, not this file's.
        // Classifying on the leading byte alone made every opaque value foreign --
        // almost none begins with this build's generation -- and the node's cache
        // tier answers `NotACompileValue` by storing the bytes verbatim, a documented
        // policy this layer does not own. So a foreign generation has to be PROVED by
        // the layout behind the byte.
        //
        // `"not-a-value"` is the node's own fixture string, kept verbatim: its
        // leading `n` is no generation this build implements, and the four bytes an
        // object length would
        // occupy read as ~1.87 billion, which the frame cannot supply.
        constexpr std::string_view opaque = "not-a-value";
        auto const bytes = AsBytes(opaque);
        REQUIRE(static_cast<std::uint8_t>(bytes.front()) != CompileValueVersion);

        auto const canonical = CanonicalStoredValue(bytes, "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::NotACompileValue);
        CHECK(canonical.generation == 0);
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

TEST_CASE("A generation that moved the framing is refused, not stored verbatim")
{
    // #552, the residual #483 left behind. #483 refuses a foreign generation whose
    // frame still HOLDS TOGETHER, because a parseable layout is positive evidence
    // that these bytes are a stored value. A future generation that moves the
    // FRAMING as well leaves no such evidence: it parses as junk, comes back
    // `NotACompileValue`, and a node's cache tier stores what it cannot decode
    // VERBATIM -- so the producing checkout's absolute paths land under a key every
    // machine computes, which is #229 arriving through the door #483 closed.
    //
    // The tests here could not reach this, and that is why it survived #483: every
    // foreign-generation case above re-stamps a frame THIS build encoded, so the
    // layout always holds and the evidence path always fires. The value below is
    // synthesised instead.
    //
    // The framing bump modelled is the cheapest realistic one -- the frame gains a
    // field, a `u16` flags word directly after the generation byte -- which is
    // squarely what `CompileValueVersion` is allowed to mean. Nothing couples it to
    // an `objkey-v*` bump, so this is an ordinary future event, not a hypothetical.
    auto const futureFramed = [](std::uint8_t generation) {
        std::vector<std::byte> v;
        v.push_back(static_cast<std::byte>(generation));
        v.push_back(std::byte { 0x00 }); // the new field, high byte
        v.push_back(std::byte { 0x07 }); // the new field, low byte
        constexpr std::string_view obj = "OBJECT";
        auto const len = static_cast<std::uint32_t>(obj.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            v.push_back(static_cast<std::byte>((len >> shift) & 0xFFU));
        for (char const c: obj)
            v.push_back(static_cast<std::byte>(c));
        for (int i = 0; i < 4; ++i)
            v.push_back(std::byte { 0x00 }); // region count 0
        return v;
    };

    SECTION("the frame does not parse here, which is the whole point")
    {
        // Asserted rather than assumed: if this ever started parsing, the case would
        // be exercising #483's evidence-from-layout path and would say nothing about
        // #552 while staying green.
        auto const decoded = DecodeCompileValue(futureFramed(CompileValueVersion + 1));
        REQUIRE_FALSE(decoded.has_value());
    }

    SECTION("and it is still named as a foreign generation")
    {
        for (auto const bump: { 1, 2, 3 })
        {
            auto const generation = static_cast<std::uint8_t>(CompileValueVersion + bump);
            CAPTURE(generation);
            auto const canonical = CanonicalStoredValue(futureFramed(generation), "/home/dev/proj", "/home/dev/proj/build");
            CHECK(canonical.outcome == CanonicalizationOutcome::ForeignGeneration);
            CHECK(canonical.generation == generation);
            // Nothing to store is what makes "store it verbatim" unavailable to a
            // caller rather than merely discouraged -- the same property #483 relies
            // on, now reached without a parseable layout.
            CHECK(canonical.bytes.empty());
        }
    }
}

TEST_CASE("The reserved generation range is the boundary between a stored value and an opaque blob")
{
    // The contract `MaxCompileValueGeneration` states, tested at both edges rather
    // than in the middle: an off-by-one either way is the difference between
    // reopening #552 and re-committing the defect the node's tests caught, where
    // classifying on the leading byte alone refused EVERY opaque value.
    //
    // These frames deliberately do not parse, so the leading byte is the only thing
    // that can decide the answer.
    auto const unparseable = [](std::uint8_t leading) {
        std::vector<std::byte> v { static_cast<std::byte>(leading) };
        for (int i = 0; i < 6; ++i)
            v.push_back(std::byte { 0xFF }); // a length no frame this short can supply
        return v;
    };

    SECTION("the last reserved byte is a generation")
    {
        auto const canonical =
            CanonicalStoredValue(unparseable(MaxCompileValueGeneration), "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::ForeignGeneration);
        CHECK(canonical.generation == MaxCompileValueGeneration);
    }

    SECTION("the first byte past it is not")
    {
        auto const canonical =
            CanonicalStoredValue(unparseable(MaxCompileValueGeneration + 1), "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::NotACompileValue);
        CHECK(canonical.generation == 0);
    }

    SECTION("and neither is any real object file, which is what the range costs")
    {
        // The cost of the range is that an opaque value starting inside it is
        // refused. These are the leading bytes of the formats this cache actually
        // stores, and none is inside it -- so the cost is a fact rather than a hope,
        // and a future widening of the range has to argue with this list.
        struct Row
        {
            std::uint8_t leading;  ///< First byte of the format.
            std::string_view what; ///< Which format, for `INFO`.
        };
        auto const rows = std::vector<Row> {
            { .leading = 0x7F, .what = "ELF" },
            { .leading = 0x4D, .what = "PE/COFF, 'MZ'" },
            { .leading = 0x64, .what = "COFF, x86-64 machine type" },
            { .leading = 0xCE, .what = "Mach-O 32-bit" },
            { .leading = 0xCF, .what = "Mach-O 64-bit" },
            { .leading = 0xFE, .what = "Mach-O universal" },
            { .leading = 0x21, .what = "ar archive, '!'" },
        };
        for (auto const& row: rows)
        {
            INFO(row.what);
            CHECK(row.leading > MaxCompileValueGeneration);
            auto const canonical = CanonicalStoredValue(unparseable(row.leading), "/home/dev/proj", "/home/dev/proj/build");
            CHECK(canonical.outcome == CanonicalizationOutcome::NotACompileValue);
        }
    }

    SECTION("a zero leading byte is not a generation either")
    {
        // Generations are allocated from 1, so 0 names none -- and reporting one
        // would name a generation nobody wrote, which is the argument the
        // empty-value case above makes for the same reason.
        auto const canonical = CanonicalStoredValue(unparseable(0), "/home/dev/proj", "/home/dev/proj/build");
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

/// What a row claims one side of the transformation does to its text.
///
/// **A row that exercises nothing is byte-identical, inside a digest, to one that
/// does real work** — both just contribute their bytes — so a corpus can grow, read
/// as thorough, and assert progressively less. That is not a flaw in the digest; it
/// is the class of thing a digest structurally cannot check, and this column is what
/// covers it (issue #547).
///
/// It is a declaration rather than a blanket "every row must change something",
/// because two rows here are *deliberately* inert and a guard that has to be waived
/// twice is a guard on its way to becoming decoration: canonicalizing a region that
/// already carries tokens must be a no-op, or canonicalizing twice would be a second
/// rewrite, and the empty region exists to prove the framing survives with no text
/// at all. Saying which it is per side keeps both honest.
enum class RegionEffect : std::uint8_t
{
    Rewrites,  ///< This side must CHANGE the text. A row that stops doing so is inert.
    Preserves, ///< This side must leave the text byte-identical, deliberately.
};

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
    // The byte-wide members sit together at the end, which is this tree's layout rule
    // wherever a struct mixes them with wider ones.
    Grammar grammar;             ///< Which grammar locates path spans in `text`.
    RegionEffect producerEffect; ///< What canonicalization must do to `text`.
    RegionEffect consumerEffect; ///< What localization must do to the canonical form.
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
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "windows showIncludes, mixed case, CRLF",
                      .producerSourceRoot = R"(C:\src\Proj)",
                      .producerBuildTree = R"(C:\src\Proj\out)",
                      .consumerSourceRoot = R"(D:\work\proj)",
                      .consumerBuildTree = R"(D:\work\proj\build)",
                      .text = "Note: including file: c:\\SRC\\proj\\inc\\A.hpp\r\n"
                              "Note: including file: C:\\src\\Proj\\out\\gen\\cfg.hpp\r\n"
                              "Note: including file: C:\\Program Files\\MSVC\\include\\vector\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "showIncludes final line with no newline",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "Note: including file: /home/dev/proj/z.h",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "showIncludes already carrying tokens is left alone",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "Note: including file: <SRCROOT>/inc/a.hpp\n"
                              "Note: including file: <BUILDTREE>/gen/cfg.hpp\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Preserves,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "msvc diagnostics, line and column",
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(E:\ci\proj)",
                      .consumerBuildTree = R"(E:\ci\proj\out)",
                      .text = "C:\\src\\proj\\a.cpp(17): warning C4100: unreferenced\r\n"
                              "C:\\src\\proj\\a.cpp(17,9): note: see reference\r\n"
                              "cl : Command line warning D9002\r\n",
                      .grammar = Grammar::MsvcDiagnostics,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
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
                      .grammar = Grammar::GccDepfile,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "depfile under a drive-relative root",
                      .producerSourceRoot = R"(C:src\proj)",
                      .producerBuildTree = R"(C:src\proj\out)",
                      .consumerSourceRoot = R"(D:\work\proj)",
                      .consumerBuildTree = R"(D:\work\proj\out)",
                      .text = "C:src\\proj\\out\\a.obj: C:src\\proj\\a.cpp\n",
                      .grammar = Grammar::GccDepfile,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "showIncludes under a UNC root",
                      .producerSourceRoot = R"(\\build01\share\proj)",
                      .producerBuildTree = R"(\\build01\share\proj\out)",
                      .consumerSourceRoot = R"(C:\local\proj)",
                      .consumerBuildTree = R"(C:\local\proj\out)",
                      .text = "Note: including file: \\\\build01\\share\\proj\\inc\\a.hpp\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "a bare root produces, and a bare root consumes",
                      // A bare root IS its own trailing separator, which is the
                      // whole of issue #547 on both sides at once: the producer
                      // matched nothing (so the value kept absolute paths) and
                      // the consumer emitted `//inc/a.hpp`.
                      //
                      // This row is why the `RegionEffect` columns exist. It was
                      // added for #483 with a comment claiming it covered both
                      // sides, and it covered NEITHER -- with nothing
                      // canonicalized there was no token, so localization had
                      // nothing to localize. A digest cannot tell that from a row
                      // doing real work; both just contribute bytes. The columns
                      // below are the assertion the digest structurally cannot
                      // make.
                      .producerSourceRoot = "/",
                      .producerBuildTree = "/build",
                      .consumerSourceRoot = "/",
                      .consumerBuildTree = "/out",
                      .text = "Note: including file: /inc/a.hpp\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "a bare drive root produces, and a bare drive root consumes",
                      // The Windows half of the row above. It was absent from the
                      // corpus in ANY form -- not inert, missing -- so neither the
                      // inertness check nor the one-sided-variation check would
                      // have found it on its own. Only enumerating every shape
                      // against both sides did.
                      .producerSourceRoot = R"(C:\)",
                      .producerBuildTree = R"(C:\out)",
                      .consumerSourceRoot = R"(D:\)",
                      .consumerBuildTree = R"(D:\out)",
                      .text = R"(Note: including file: C:\inc\a.hpp)"
                              "\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "an untrimmed root produces, and an untrimmed root consumes",
                      // The other shape that ends in a separator, and the one the
                      // bare-root fix claimed in a comment to have picked up on the
                      // way past. A claim in a comment is not coverage, so it gets a
                      // row -- and the row pays for itself, because the claim was
                      // half true: untrimmed roots now MATCH, and until `RootDepth`
                      // the trailing byte also made the build tree beat the source
                      // root on length alone. Both roots untrimmed on both sides, and
                      // the second line sits under the build tree so the tie-break is
                      // what decides it.
                      //
                      // `RootReconciler` trims these, so only a client that does not
                      // reaches here -- which the daemon and the node are, taking the
                      // roots straight off a STORE frame.
                      .producerSourceRoot = "/home/dev/proj/",
                      .producerBuildTree = "/home/dev/proj/build/",
                      .consumerSourceRoot = R"(C:\work\proj\)",
                      .consumerBuildTree = R"(C:\work\proj\out\)",
                      .text = "Note: including file: /home/dev/proj/inc/a.hpp\n"
                              "Note: including file: /home/dev/proj/build/gen/cfg.hpp\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "a drive-relative root consumes",
                      // The mirror of the drive-relative PRODUCER row above. It
                      // passes, and it is here for that reason: nothing
                      // distinguished it from the bare roots before it was run, so
                      // a row documenting a shape as sound is worth as much as one
                      // that catches a defect. "We checked once by hand" is not
                      // coverage.
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(C:src\proj)",
                      .consumerBuildTree = R"(C:src\proj\out)",
                      .text = R"(Note: including file: C:\src\proj\inc\a.hpp)"
                              "\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "a UNC root consumes",
                      // The mirror of the UNC PRODUCER row above, and here for the
                      // same reason. It also pins the one shape where a leading
                      // double separator is CORRECT, so a future "collapse the
                      // double separator" fix cannot quietly break it.
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(\\host\share\proj)",
                      .consumerBuildTree = R"(\\host\share\proj\out)",
                      .text = R"(Note: including file: C:\src\proj\inc\a.hpp)"
                              "\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "a bare drive root consumes",
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(D:\)",
                      .consumerBuildTree = R"(D:\out)",
                      .text = "Note: including file: C:\\src\\proj\\inc\\a.hpp\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "empty region",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "",
                      .grammar = Grammar::MsvcDiagnostics,
                      .producerEffect = RegionEffect::Preserves,
                      .consumerEffect = RegionEffect::Preserves },
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
/// version is what closes that.
///
/// A retired row's digest is a DATED RECORD and not a second guard, which is worth
/// saying out loud because the obvious reading is the wrong one. It describes the
/// corpus as that generation met it -- this table's first retirement added three
/// rows in the same commit, so `be1728...` no longer describes anything the live
/// corpus can produce. And the live digest could not reproduce a retired one even
/// if the corpus had stood still: `ConformanceDigest` frames `canonical.bytes`,
/// whose FIRST BYTE is `CompileValueVersion`, so comparing across generations is
/// unequal by construction. What catches a bump being put back is structural and
/// lives in the case below.
struct StoredValueGeneration
{
    std::string_view digest; ///< What the corpus of its day yielded under that generation.
    std::uint8_t version;    ///< The `CompileValueVersion` it was written under.
};

/// Every generation of the stored-value contract this build knows about.
///
/// The live one is LAST and that ordering is load-bearing -- see the case below.
/// @return The table, newest last.
constexpr std::array StoredValueGenerations {
    // Generation 1 is RETIRED (#547): a bare root canonicalized nothing on the
    // producer side and doubled its separator on the consumer side.
    StoredValueGeneration { .digest = "be1728170060f3f786faa7084585a7035385b9a6ab888cb386bbb63c89c72f5c", .version = 1 },
    StoredValueGeneration { .digest = "04e18f13a5e1004d23f5f6b738609ff17d3d23a44b0bd39437084df531727af4", .version = 2 },
};

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

    // `FindOrNull` rather than an iterator: a `std::array` iterator is a raw pointer
    // on libstdc++ and libc++ and a class type on MSVC's, so the spelling the
    // analyser asks for on one host does not compile on another. That argument is
    // the helper's own, which is why this calls it instead of restating it.
    auto const* const pinnedRow = FindOrNull(StoredValueGenerations, CompileValueVersion, &StoredValueGeneration::version);
    {
        // Scoped, so this note appears only when it is the thing that failed.
        INFO("CompileValueVersion is " << static_cast<unsigned>(CompileValueVersion)
                                       << " and StoredValueGenerations has no row for it -- a bump adds a row");
        REQUIRE(pinnedRow != nullptr);
    }
    auto const& pinned = *pinnedRow;

    // The message names BOTH ways of arriving here, because they call for opposite
    // actions and only one of them is the interesting one. Editing the CORPUS moves
    // the digest without moving any behaviour, and the answer there is to repin this
    // generation's row. Editing the canonicalization or the framing moves what two
    // servers do with one value, and the answer there is a new generation. A message
    // naming only the second teaches whoever meets the first to bump the byte for
    // nothing, which retires every entry in the fleet to buy exactly that.
    // Scoped, like the note above it: unscoped, this whole paragraph was reprinted
    // under the structural failure below, which is a different fact with a different
    // remedy -- and the reader meets the digest advice first.
    {
        INFO("the stored-value contract produced "
             << live << ", but generation " << static_cast<unsigned>(CompileValueVersion) << " is pinned to "
             << pinned.digest
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
    }

    // What catches a bump being put back afterwards with a freshly pasted golden.
    //
    // This used to be `CHECK(live != row.digest)` over the retired rows, which reads
    // like that guard and cannot be that guard: `ConformanceDigest` frames
    // `canonical.bytes`, whose leading byte IS `CompileValueVersion`, so a digest
    // taken under one generation can never equal one taken under another. The check
    // could not fail, and a check that cannot fail is indistinguishable from one that
    // holds -- which is the same thing `RegionEffect` exists to say one table over.
    //
    // The structure carries it instead, and structure is what the reverting author
    // actually has to defeat. A bump ADDS a row, so the live byte names the LAST one;
    // putting the byte back names an earlier row however good its digest is.
    for (auto const i: std::views::iota(std::size_t { 1 }, rows.size()))
    {
        INFO("StoredValueGenerations is out of order at row " << i << ": generations are unique and ascending, so a "
                                                              << "bump appends");
        REQUIRE(rows[i].version > rows[i - 1].version);
    }

    INFO("CompileValueVersion is " << static_cast<unsigned>(CompileValueVersion) << " but the newest generation in "
                                   << "the table is " << static_cast<unsigned>(rows.back().version)
                                   << ". A bump appends a row, so the live byte is the last one -- naming an earlier "
                                   << "generation is a bump that was reverted, whatever digest was pasted with it.");
    CHECK(CompileValueVersion == rows.back().version);
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

/// Render a `RegionEffect` for a failure message.
/// @param effect What a row declared.
/// @return Its spelling.
[[nodiscard]] std::string_view NameOf(RegionEffect effect)
{
    switch (effect)
    {
        case RegionEffect::Rewrites:
            return "Rewrites";
        case RegionEffect::Preserves:
            return "Preserves";
    }
    return "?";
}

} // namespace

TEST_CASE("Every conformance row does what it says on each side")
{
    // The assertion a digest structurally cannot make. A row that rewrites nothing
    // contributes bytes exactly as one that rewrites everything does, so without
    // this the corpus can grow, read as thorough, and assert progressively less --
    // which is what happened to the bare-root row added in #483 and repaired in
    // #547. Declaring the effect per side, rather than requiring every row to change
    // something, is what keeps the two DELIBERATE no-ops honest instead of waived.
    for (auto const& row: ConformanceCorpus)
    {
        CompileValue produced;
        produced.objectBlob = { std::byte { 0x7F } };
        produced.textRegions.push_back(TextRegion { .grammar = row.grammar, .bytes = std::string { row.text } });

        auto const canonical =
            CanonicalStoredValue(EncodeCompileValue(produced), row.producerSourceRoot, row.producerBuildTree);
        REQUIRE(canonical.outcome == CanonicalizationOutcome::Canonicalized);
        auto const stored = DecodeCompileValue(canonical.bytes);
        REQUIRE(stored.has_value());
        REQUIRE(stored->textRegions.size() == 1);
        auto const& canonText = stored->textRegions.front().bytes;

        PathCanon::Layout const consumer { .sourceRoot = std::string { row.consumerSourceRoot },
                                           .buildTree = std::string { row.consumerBuildTree } };
        auto const localized = PathCanon::LocalizeRegion(canonText, row.grammar, consumer);

        auto const observed = [](bool changed) {
            return changed ? RegionEffect::Rewrites : RegionEffect::Preserves;
        };

        INFO("row \"" << row.name << "\" declares producerEffect=" << NameOf(row.producerEffect) << " but canonicalization "
                      << (canonText != row.text ? "CHANGED" : "did not change")
                      << " the text. A row that rewrites nothing asserts nothing, and the digest cannot see the "
                         "difference.");
        CHECK(observed(canonText != row.text) == row.producerEffect);

        INFO("row \"" << row.name << "\" declares consumerEffect=" << NameOf(row.consumerEffect) << " but localization "
                      << (localized != canonText ? "CHANGED" : "did not change") << " the canonical text.");
        CHECK(observed(localized != canonText) == row.consumerEffect);
    }
}

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
