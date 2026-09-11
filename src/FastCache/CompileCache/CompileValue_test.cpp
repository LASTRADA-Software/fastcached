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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <tests/DeclaredCountBlob.hpp>
#include <tests/ForeignGenerationValue.hpp>
#include <tests/RetiredGenerations.hpp>

using namespace FastCache;
using PathCanon::Grammar;

TEST_CASE("EncodeCompileValue writes the layout its header documents, byte for byte")
{
    // **The assertion that distinguishes.** The round-trip case below agrees with
    // whatever layout and byte order `EncodeCompileValue` and `DecodeCompileValue`
    // happen to share, so it passes under the hand-rolled `AppendU32`/`AppendBytes`
    // this encoder used to carry and under `ByteAppender` alike (#305). These bytes are
    // spelled field by field through a builder sharing no code with the encoder,
    // against the layout `CompileValue.hpp` writes out:
    // `[u8 generation][u32 objectLen][object][u32 regionCount]{[u8 grammar][u32 len][text]}`.
    //
    // The generation byte is read from `CompileValueVersion` rather than typed, because
    // a literal here would have to be edited at every bump and would then be asserting
    // its own edit. The GRAMMAR tags are typed, because those ARE a wire contract: an
    // enum whose ordinal is written to a stored value states `= N`, and a literal is
    // what makes a renumbering show up here as a changed number rather than as an
    // invisible consequence of one inserted enumerator.
    CompileValue value;
    value.objectBlob = { std::byte { 0x01 }, std::byte { 0xFE } };
    value.textRegions.push_back({ .grammar = Grammar::ShowIncludes, .bytes = "note" });
    value.textRegions.push_back({ .grammar = Grammar::GccDepfile, .bytes = "" });

    auto const expected = Testing::DeclaredCountBlob {}
                              .Byte(CompileValueVersion)
                              .U32(2)
                              .Byte(0x01)
                              .Byte(0xFE) // the object blob, length-prefixed
                              .U32(2)     // regionCount
                              .Byte(0)    // Grammar::ShowIncludes
                              .Field("note")
                              .Byte(2) // Grammar::GccDepfile
                              .Field("")
                              .Vector();

    CHECK(EncodeCompileValue(value) == expected);

    // And the tags above really are those grammars, so a renumbering fails the CHECK
    // rather than being absorbed by two literals moving together.
    CHECK(static_cast<std::uint8_t>(Grammar::ShowIncludes) == 0);
    CHECK(static_cast<std::uint8_t>(Grammar::GccDepfile) == 2);
}

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
    // An EMPTY value on purpose -- this case is about the DECODER, which reaches its
    // generation check before there is anything else to read. `StampGeneration` is what
    // knows the generation is the leading byte, here and in the three other places this
    // was hand-rolled (#649); it refuses an encoder that produced nothing rather than
    // reading off the end.
    auto const bytes =
        Testing::StampGeneration(EncodeCompileValue({ .objectBlob = {}, .textRegions = {} }), Testing::ForeignGeneration);

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

TEST_CASE("The shared foreign-generation helper refuses to stamp a value it does not recognise")
{
    // It lives here rather than beside an implementation because there is no
    // implementation: `src/tests/ForeignGenerationValue.hpp` is a builder for the four
    // cases that need a value from a generation this build does not implement, and the
    // property under test is a fact about THIS file's encoder (#649).
    //
    // The guard is the whole point of consolidating them. All four hand-rolled copies
    // wrote the leading byte unconditionally, so a generation that moved off byte 0 would
    // have them stamping some other field: the value is then damaged rather than foreign,
    // every case still asserts a refusal, and every case still passes.
    //
    // Both directions, because a guard nobody has watched ACCEPT is not known to work --
    // a helper that threw unconditionally would satisfy the refusing half alone.
    CHECK_NOTHROW(Testing::ForeignGenerationValue());

    // Already carrying a generation this build does not implement, which is what a
    // second stamp of the same bytes looks like and what a moved generation byte looks
    // like from here: the helper cannot tell the difference and must not guess.
    CHECK_THROWS_AS(Testing::StampGeneration(Testing::ForeignGenerationValue(), Testing::ForeignGeneration),
                    std::logic_error);

    // Nothing to stamp is its own arm: indexing it is the read off the end that the
    // hand-rolled copies each had to guard against on their own.
    CHECK_THROWS_AS(Testing::StampGeneration({}, Testing::ForeignGeneration), std::logic_error);
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
    // The constant, not a literal: this helper exists to exercise the region-count
    // guard and needs a frame of the CURRENT generation to do it. The literal that
    // used to sit here was the byte's only anchor by accident, and a generation
    // bump then failed two tests that have nothing to do with versioning. The byte
    // is pinned deliberately instead, in its own case below.
    //
    // The assembly is shared (#306); which fields a compile-value header has is not.
    return FastCache::Testing::DeclaredCountBlob {}
        .Byte(CompileValueVersion)
        .U32(0) // objectLen
        .U32(regionCount)
        .Pad(trailingBytes)
        .Vector();
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
    // Generation 3 since #879 / #891.
    auto const encoded = EncodeCompileValue(CompileValue {});
    REQUIRE_FALSE(encoded.empty());
    CHECK(encoded.front() == std::byte { 4 });
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
    auto const frame = FrameDeclaring(FastCache::Testing::ImpossibleCount, 0);
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
        // `wire` is encoded above, so it is never empty; GCC cannot see that through the
        // copy and read the subscript as a null dereference (#805). `StampGeneration`
        // refuses an empty vector rather than indexing one, which answers that and keeps
        // the SECTION honest if the encoder above ever returns nothing (#649).
        auto const foreign = Testing::StampGeneration(wire, Testing::ForeignGeneration);

        auto const canonical = CanonicalStoredValue(foreign, "/home/dev/proj", "/home/dev/proj/build");
        CHECK(canonical.outcome == CanonicalizationOutcome::ForeignGeneration);
        CHECK(canonical.generation == Testing::ForeignGeneration);
        // Nothing to store: a server has no bytes it may write, which is what makes
        // "store it verbatim" unavailable rather than merely discouraged.
        CHECK(canonical.bytes.empty());
    }

    SECTION("bytes that are not a value at all stay the caller's own policy")
    {
        // The same emptiness the SECTION above states, and here it matters more:
        // `wire.size() - 1` on an empty vector is a size_t UNDERFLOW, so the span
        // would be 2^64-1 bytes long and `CanonicalStoredValue` would read off the
        // end of the world rather than answering `NotACompileValue`. Stated, not
        // assumed, because the encoder is what the whole case is built on.
        REQUIRE_FALSE(wire.empty());
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
    /// The prefix the PRODUCER's `/showIncludes` notes carry, i.e. its
    /// `msvc_deps_prefix`. `PathCanon::IncludeNoteMarker` for a machine whose
    /// toolchain speaks English, which is every row that predates #879.
    std::string_view producerMarker { PathCanon::IncludeNoteMarker };
    /// The prefix the CONSUMER's build matches. A row where this differs from
    /// `producerMarker` is two machines in different UI languages sharing a cache,
    /// which is the case #700 introduced and #879 closes.
    std::string_view consumerMarker { PathCanon::IncludeNoteMarker };
    std::string_view text; ///< The captured region, exactly as a driver wrote it.
    // The byte-wide members sit together at the end, which is this tree's layout rule
    // wherever a struct mixes them with wider ones.
    Grammar grammar; ///< Which grammar locates path spans in `text`.
    /// What normalizing the producer's marker must do to `text`. Defaulted to
    /// `Preserves` beside `producerMarker`'s own default, and the pair is the point:
    /// a row that says nothing about markers is CLAIMING to be about something else,
    /// and this column is what holds it to that rather than letting it pass silently.
    RegionEffect storeMarkerEffect { RegionEffect::Preserves };
    RegionEffect producerEffect; ///< What canonicalization must do to the normalized text.
    RegionEffect consumerEffect; ///< What localization must do to the canonical form.
    /// What restoring the consumer's marker must do to the localized text. Its own
    /// column rather than sharing `storeMarkerEffect`, because the two sides are
    /// independent — a value produced in German and replayed in English rewrites on
    /// one side only, and that asymmetry IS #700's peer edge.
    RegionEffect replayMarkerEffect { RegionEffect::Preserves };
};

/// One row driven through the whole stored-value pipeline, stage by stage.
///
/// Both the digest and the per-row effect assertions need these, and they need the
/// SAME ones: a digest taken over one pipeline while the effects are asserted against
/// another is two claims about two things wearing one name. Four stages rather than
/// two since #879, because the marker is a canonical form as much as `<SRCROOT>` is
/// and the launcher's two rewrites are where a fleet agrees about it.
struct PipelineTrace
{
    std::string normalized;             ///< Producer's marker rewritten to the canonical one.
    std::vector<std::byte> storedBytes; ///< What a server holds: the canonical value, framing included.
    std::string canonical;              ///< The stored region's text alone.
    std::string localized;              ///< Paths localized into the consumer's roots.
    std::string replayed;               ///< Canonical marker rewritten to the consumer's own.
};

/// Run one row through producer-normalize, canonicalize, localize and
/// replay-restore.
/// @param row The corpus row.
/// @return Every stage's output.
[[nodiscard]] PipelineTrace RunConformanceRow(ConformanceCase const& row)
{
    PipelineTrace trace;
    trace.normalized = PathCanon::RewriteIncludeNoteMarker(row.text, row.producerMarker, PathCanon::IncludeNoteMarker);

    CompileValue produced;
    // A real object blob, because the contract includes leaving it untouched.
    produced.objectBlob = { std::byte { 0x7F }, std::byte { 0x45 }, std::byte { 0x4C }, std::byte { 0x46 } };
    produced.textRegions.push_back(TextRegion { .grammar = row.grammar, .bytes = trace.normalized });

    auto const canonical = CanonicalStoredValue(EncodeCompileValue(produced), row.producerSourceRoot, row.producerBuildTree);
    REQUIRE(canonical.outcome == CanonicalizationOutcome::Canonicalized);
    trace.storedBytes = canonical.bytes;

    auto const stored = DecodeCompileValue(canonical.bytes);
    REQUIRE(stored.has_value());
    REQUIRE(stored->textRegions.size() == 1);
    trace.canonical = stored->textRegions.front().bytes;

    PathCanon::Layout const consumer { .sourceRoot = std::string { row.consumerSourceRoot },
                                       .buildTree = std::string { row.consumerBuildTree } };
    trace.localized = PathCanon::LocalizeRegion(trace.canonical, row.grammar, consumer);
    trace.replayed = PathCanon::RewriteIncludeNoteMarker(trace.localized, PathCanon::IncludeNoteMarker, row.consumerMarker);
    return trace;
}

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
    ConformanceCase { .name = "a localized producer normalizes its marker, and an English consumer matches it",
                      // #879's first half, and the shape #229 records. Before the
                      // marker was a canonical form the grammar found no note here at
                      // all, so the region was stored with `/home/dev/proj` still in
                      // it and every consumer replayed the producing checkout.
                      //
                      // The consumer is deliberately ENGLISH rather than a mirror of
                      // the producer: that is #700's peer edge, where a machine
                      // setting FASTCACHE_MSVC_DEPS_PREFIX made previously-healthy
                      // differently-localized peers under-rebuild. Both marker
                      // columns are asserted, so a row that stopped rewriting on
                      // EITHER side is a failure and not a quieter pass.
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .producerMarker = "Hinweis: Einlesen der Datei:",
                      .text = "Hinweis: Einlesen der Datei: /home/dev/proj/inc/a.hpp\r\n"
                              "Hinweis: Einlesen der Datei: /usr/include/stdio.h\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .storeMarkerEffect = RegionEffect::Rewrites,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites,
                      .replayMarkerEffect = RegionEffect::Preserves },
    ConformanceCase { .name = "an English producer's value replays under a localized consumer's marker",
                      // The mirror, and it is not decoration: the two rewrites are
                      // separate calls in separate binaries -- the store side runs
                      // before the value leaves the launcher, the replay side after
                      // the stale-hit guard -- so a fix to one says nothing about the
                      // other. Without this row `replayMarkerEffect` would be
                      // `Preserves` everywhere and could never fail.
                      .producerSourceRoot = R"(C:\src\proj)",
                      .producerBuildTree = R"(C:\src\proj\out)",
                      .consumerSourceRoot = R"(D:\work\proj)",
                      .consumerBuildTree = R"(D:\work\proj\build)",
                      .consumerMarker = "Hinweis: Einlesen der Datei:",
                      .text = "Note: including file: C:\\src\\proj\\inc\\a.hpp\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .storeMarkerEffect = RegionEffect::Preserves,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites,
                      .replayMarkerEffect = RegionEffect::Rewrites },
    ConformanceCase { .name = "an indented note canonicalizes, and its depth survives",
                      // #891, and it needs no language pack: `cl` indents a note by
                      // inclusion depth, so this is what a note for anything a header
                      // pulls in transitively looks like. The grammar demanded the
                      // marker at column zero while `IncludeNotePath` already skipped
                      // blanks, so the launcher's reader found these paths and the
                      // canonicalizer did not.
                      //
                      // The first line is at depth zero deliberately -- the control,
                      // in the same region as the thing it controls, so "recognise an
                      // indented note" and "recognise a note at all" cannot be one
                      // passing assertion. Both must rewrite, and the tabs and spaces
                      // between them must come through untouched.
                      .producerSourceRoot = R"(C:\ci\deep\src)",
                      .producerBuildTree = R"(C:\ci\deep\build)",
                      .consumerSourceRoot = R"(D:\project)",
                      .consumerBuildTree = R"(D:\project\build)",
                      .text = "Note: including file: "
                              R"(C:\ci\deep\src\a.h)"
                              "\r\n"
                              " Note: including file: "
                              R"(C:\ci\deep\src\b.h)"
                              "\r\n"
                              "\t  Note: including file: "
                              R"(C:\ci\deep\build\gen\cfg.h)"
                              "\r\n",
                      .grammar = Grammar::ShowIncludes,
                      .storeMarkerEffect = RegionEffect::Preserves,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites,
                      .replayMarkerEffect = RegionEffect::Preserves },
    ConformanceCase { .name = "a diagnostic quoting the marker mid-line is not a note",
                      // The anchor, on the side that matters. Both regions a launcher
                      // stores are tagged `ShowIncludes` and one of them is the
                      // DIAGNOSTIC stream, so a marker rule matching anywhere in a
                      // line would rewrite a path a compiler merely quoted -- and on
                      // the launcher's side of the same rule it would delete an
                      // ordinary source line from the bytes the key is hashed over.
                      //
                      // Preserves on every side, and that is the assertion: the path
                      // in it lies under the producer's source root, so a grammar
                      // that matched would canonicalize it and this row would fail on
                      // `producerEffect` rather than merely not fire.
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "warning: the string \"Note: including file: /home/dev/proj/inc/a.hpp\" is unused\n",
                      .grammar = Grammar::ShowIncludes,
                      .storeMarkerEffect = RegionEffect::Preserves,
                      .producerEffect = RegionEffect::Preserves,
                      .consumerEffect = RegionEffect::Preserves,
                      .replayMarkerEffect = RegionEffect::Preserves },
    ConformanceCase { .name = "empty region",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "",
                      .grammar = Grammar::MsvcDiagnostics,
                      .producerEffect = RegionEffect::Preserves,
                      .consumerEffect = RegionEffect::Preserves },
    // #202: GCC/Clang diagnostics. A replayed warning used to name the checkout
    // that stored it, because stderr was tagged `ShowIncludes` -- a grammar that
    // cannot see a diagnostic's path -- so nothing was ever rewritten.
    ConformanceCase { .name = "gcc diagnostic under the source root",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "/home/dev/proj/src/a.cpp:12:5: warning: unused variable 'x'\n",
                      .grammar = Grammar::GccDiagnostics,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    // The include chain: its header and its indented continuations, both anchored.
    ConformanceCase { .name = "gcc include chain, header and continuation",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "In file included from /home/dev/proj/inc/a.hpp:3,\n"
                              "                 from /home/dev/proj/src/a.cpp:1:\n"
                              "/home/dev/proj/build/gen/cfg.hpp:9:1: note: expanded here\n",
                      .grammar = Grammar::GccDiagnostics,
                      .producerEffect = RegionEffect::Rewrites,
                      .consumerEffect = RegionEffect::Rewrites },
    // A path OUTSIDE both roots is left standing, as every other grammar leaves it.
    ConformanceCase { .name = "gcc diagnostic in a toolchain header is untouched",
                      .producerSourceRoot = "/home/dev/proj",
                      .producerBuildTree = "/home/dev/proj/build",
                      .consumerSourceRoot = "/srv/ci/checkout",
                      .consumerBuildTree = "/srv/ci/checkout/out",
                      .text = "/usr/include/c++/16/vector:120:7: note: candidate\n",
                      .grammar = Grammar::GccDiagnostics,
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
/// A generation of the stored-value contract, keyed on its `CompileValueVersion`.
///
/// The shape and its guards are shared with the key space's schema-tag table through
/// `<tests/RetiredGenerations.hpp>` (#548). The two tables assert DIFFERENTLY and the
/// header says why: a retired digest is only re-derivable while the digested inputs
/// are frozen, and this corpus grows.
using StoredValueGeneration = FastCache::Testing::Generation<std::uint8_t>;

/// Every generation of the stored-value contract this build knows about.
///
/// The live one is LAST and that ordering is load-bearing -- see the case below.
/// @return The table, newest last.
constexpr std::array StoredValueGenerations {
    // Generation 1 is RETIRED (#547): a bare root canonicalized nothing on the
    // producer side and doubled its separator on the consumer side.
    StoredValueGeneration { .key = 1, .digest = "be1728170060f3f786faa7084585a7035385b9a6ab888cb386bbb63c89c72f5c" },
    // Generation 2 is RETIRED (#879, #891): `Grammar::ShowIncludes` matched only the
    // literal English marker, at column zero, so a localized `cl` and an indented
    // note both canonicalized to nothing and their regions kept the producing
    // checkout's absolute paths.
    StoredValueGeneration { .key = 2, .digest = "04e18f13a5e1004d23f5f6b738609ff17d3d23a44b0bd39437084df531727af4" },
    // Generation 3 is RETIRED (#202): stderr was tagged `Grammar::ShowIncludes`, a
    // grammar that cannot see a diagnostic's path, so a replayed warning named the
    // checkout that STORED it -- on a machine with several checkouts, a path that
    // resolves to a different tree at a different revision, so the line number
    // lands on unrelated code.
    StoredValueGeneration { .key = 3, .digest = "01678295e5663e51cda388e8334ed57e01ab9326b8b2754811b53afd8ed9a90e" },
    StoredValueGeneration { .key = 4, .digest = "53089e7f32b6881cb354df3af4850c75f7fee50fed240c7ca5eaf3b448597edf" },
};

/// One corpus row's own contribution to a generation's conformance digest.
///
/// The aggregate digest is a FOLD, and a fold destroys precisely what
/// [#583](https://github.com/LASTRADA-Software/fastcached/issues/583) needs: it
/// answers *the contract's behaviour moved* and cannot answer WHICH rows moved, so it
/// cannot check the one claim a bump actually makes.
struct RowDigest
{
    std::string_view row;    ///< The `ConformanceCase::name` this covers.
    std::string_view digest; ///< That row's covered stages, digested alone.
};

/// Generation 4's rows, frozen as this build produces them.
///
/// **The freeze starts HERE and does not run backwards.** Generations 1 to 3 keep
/// only their aggregate digest and stay DATED RECORDS, for a reason worth stating
/// rather than leaving as an omission: re-deriving one means running THAT
/// generation's behaviour, which is gone, and running today's behaviour over its
/// corpus yields a confident number that is neither its digest nor anything a reader
/// can interpret -- worse than no number, because it would look like a re-derivation.
/// Recovering them needs old trees built, which is not what this mechanism is for.
constexpr std::array Generation4Rows {
    RowDigest { .row = "posix showIncludes under both roots",
                .digest = "14df5a11169bc099b908969bfa598dbd1bd5af12f4042fa66d3fa8c7ce239c09" },
    RowDigest { .row = "windows showIncludes, mixed case, CRLF",
                .digest = "9d6cb862c0a9f2eb9d51d2336fcea8b6734073fed8bf7850fd3be41646847026" },
    RowDigest { .row = "showIncludes final line with no newline",
                .digest = "0d6e5bab4b4ea85cd1225b1591e0a325c313a1b06dfc75dd30564bb9b78f79d5" },
    RowDigest { .row = "showIncludes already carrying tokens is left alone",
                .digest = "e3cfd9ba878c360606dfbdb5ee25cece0268dd048034d35631b863c25fd5ba0f" },
    RowDigest { .row = "msvc diagnostics, line and column",
                .digest = "39f4621416681297a2d93a856cea4b53a7af0b23409995fcc8d00b3f15351e2e" },
    RowDigest { .row = "gcc depfile, target, continuations and escaped space",
                .digest = "7f982a9fdc5f6c1d0478f370f9726f08bd823a28d6f31baf8fd04ec66c84a222" },
    RowDigest { .row = "depfile under a drive-relative root",
                .digest = "106c4c40b209bc9a686c7cfd3d7baf79df4f236d229ea688dc5e7a8665639c83" },
    RowDigest { .row = "showIncludes under a UNC root",
                .digest = "acfd322dbf79226e1090352581d7d5964f6a5dde5aa5767a7b10edc41fbde8cb" },
    RowDigest { .row = "a bare root produces, and a bare root consumes",
                .digest = "4b4691413029e66261df20c6075e1d6e3cd4612d4943015450ceda30f1435e61" },
    // This digest and "a bare drive root consumes" below are EQUAL, and that is the
    // canonicalization working rather than a redundant row. The two differ only on the
    // PRODUCER side -- a bare `C:\` root against a nested `C:\src\proj` one -- and
    // erasing exactly that difference is what the stored form is for, so identical
    // stored bytes is the property this cache exists to have. They are not
    // interchangeable: a defect in the bare-drive-root producer path moves this row
    // and leaves the other standing, which is the whole reason both are here. Equal
    // today is not redundant.
    RowDigest { .row = "a bare drive root produces, and a bare drive root consumes",
                .digest = "62f9934c4822d8bd526b0e24bb8697b1c13226f734e28aed21c90a64c08e47ae" },
    RowDigest { .row = "an untrimmed root produces, and an untrimmed root consumes",
                .digest = "e6b6b9c572cf0ebe03bbbf9cd0635b42941308e584f8b4cc6a47b4ef8acb3311" },
    RowDigest { .row = "a drive-relative root consumes",
                .digest = "763f779e22bcdb69025f0c75c43e3f3dcb3ca9516cfa72845bdbfd42be175b0d" },
    RowDigest { .row = "a UNC root consumes", .digest = "1f4cbb8b5b91b84a0470524f64c959e4929679ba3e5c24e7bc2f7cfaee981713" },
    RowDigest { .row = "a bare drive root consumes",
                .digest = "62f9934c4822d8bd526b0e24bb8697b1c13226f734e28aed21c90a64c08e47ae" },
    RowDigest { .row = "a localized producer normalizes its marker, and an English consumer matches it",
                .digest = "7fc2e9b7e11fc499187b98cd570200b2ff1244bb3b68610df8e779738a68daf9" },
    RowDigest { .row = "an English producer's value replays under a localized consumer's marker",
                .digest = "920b678d7d302237bbac8322218492f3bb5914c003b4311fce054ce0adf08968" },
    RowDigest { .row = "an indented note canonicalizes, and its depth survives",
                .digest = "697f9fdbcfb486cc563d9c5919ea4714e60b1e5b56abca9faaba092dd22b4344" },
    RowDigest { .row = "a diagnostic quoting the marker mid-line is not a note",
                .digest = "d41d57eafffe2a9a78b4ca327db1c822a55bef0ae56434677f1f8ccf31e2a1ce" },
    RowDigest { .row = "empty region", .digest = "ff7acbd998d5e32f2ba3a47c779137140e018b16b2a9875b6d4945b86975469b" },
    RowDigest { .row = "gcc diagnostic under the source root",
                .digest = "e6bbb746df126c4529546cc65c85e61c1b8676e11b9b05269c201fa7ee5dbc05" },
    RowDigest { .row = "gcc include chain, header and continuation",
                .digest = "b11f9576be6f2394a2aa132c633cf62ab15bf72c4899d508b1c9f9cfa71e3361" },
    RowDigest { .row = "gcc diagnostic in a toolchain header is untouched",
                .digest = "28a4d31422770fbb7a6a0926830fb4006cfaba358a0b62524684e68d2c974bfa" },
};

/// A generation whose per-row output this build has frozen.
struct FrozenGeneration
{
    std::span<RowDigest const> rows; ///< Its rows, one per corpus row.
    std::uint8_t generation;         ///< The `CompileValueVersion` they were taken under.
};

/// Every generation frozen per row, oldest first.
constexpr std::array FrozenGenerations {
    FrozenGeneration { .rows = Generation4Rows, .generation = 4 },
};

/// What a bump CLAIMS it changed.
///
/// The claim is the `changed` set alone -- rows present in both generations whose
/// output moved. Rows ADDED or REMOVED by the same commit are reported to the reader
/// and deliberately excluded from the claim: widening the corpus is a repin rather
/// than a behaviour change, and #547 did both in one commit, so demanding one list
/// for two different acts would make the honest case unstatable.
struct GenerationBump
{
    std::span<std::string_view const> changedRows; ///< Rows this bump says it moved.
    std::uint8_t from;                             ///< The generation left behind.
    std::uint8_t to;                               ///< The generation adopted.
};

/// Every bump between adjacent frozen generations.
///
/// Empty while only one generation is frozen, which is not a licence to forget: the
/// `static_assert` below fails the BUILD the moment a second generation is frozen
/// without the row saying what it moved. A runtime check would let that ship and
/// report it afterwards, and this is a *do something* obligation rather than a *say
/// why* one, so it belongs to the type system.
constexpr std::array<GenerationBump, 0> GenerationBumps {};

static_assert(GenerationBumps.size() + 1 == FrozenGenerations.size(),
              "Every adjacent pair of frozen generations needs a bump row saying which corpus rows it "
              "moved. Freeze the new generation's rows and add that row; a bump that moved nothing "
              "spells an empty changedRows, which is a statement rather than an omission.");

/// How two frozen generations differ.
struct RowSetDelta
{
    std::vector<std::string_view> changed; ///< Present in both, output moved.
    std::vector<std::string_view> added;   ///< Only in the newer generation.
    std::vector<std::string_view> removed; ///< Only in the older generation.
};

/// Compare two frozen generations row by row.
/// @param before The older generation's rows.
/// @param after The newer generation's rows.
/// @return Which rows changed, arrived and left, each in its own table's order.
[[nodiscard]] RowSetDelta CompareFrozenRows(std::span<RowDigest const> before, std::span<RowDigest const> after)
{
    RowSetDelta delta;
    for (auto const& row: after)
    {
        auto const* const earlier = FindOrNull(before, row.row, &RowDigest::row);
        if (earlier == nullptr)
            delta.added.push_back(row.row);
        else if (earlier->digest != row.digest)
            delta.changed.push_back(row.row);
    }
    for (auto const& row: before)
        if (FindOrNull(after, row.row, &RowDigest::row) == nullptr)
            delta.removed.push_back(row.row);
    return delta;
}

/// Render a row list for a failure message.
/// @param rows The row names.
/// @return Them, comma-separated, or `(none)` -- which a reader must be able to tell
/// from a list, since an empty claim is a real thing to say.
[[nodiscard]] std::string RenderRows(std::span<std::string_view const> rows)
{
    if (rows.empty())
        return "(none)";
    std::string out;
    for (auto const& row: rows)
        out += (out.empty() ? "" : ", ") + std::string { row };
    return out;
}

/// Append the stages a generation's contract covers, framed, to `material`.
///
/// Shared by the aggregate digest and the per-row one (#583) so the two cannot come to
/// cover different stages, which is what two spellings of one list eventually do. They
/// must NOT share a byte LAYOUT: the aggregate's value is pinned to
/// `CompileValueVersion`, so re-folding it -- even into something that looks
/// equivalent -- would move a published number with no behaviour changing, which is
/// the exact failure the pin exists to catch, arriving from the instrument side.
///
/// Each field is length-prefixed through `WireFields`, which is this tree's one
/// definition of that framing — and it needs framing for the reason recorded at the
/// compiler identity: concatenated, `("ab", "c")` and `("a", "bc")` digest alike, so
/// a change that moved a byte from one field to its neighbour would not be seen.
///
/// A row's `name` is deliberately NOT hashed. It is a label for a human reading a
/// failure, so hashing it would make renaming one — a purely editorial change — fail
/// this test with a message telling the author to bump `CompileValueVersion`, which
/// is the one action this design exists to make hard. The per-row table keys ON that
/// name, which is not a contradiction: a rename there is caught as a row that left
/// and a row that arrived, and neither is a claim about behaviour.
///
/// @param trace One row's pipeline output.
/// @param material The digest material to append to.
void AppendCoveredStages(PipelineTrace const& trace, std::vector<std::byte>& material)
{
    auto const append = [&material](std::span<std::byte const> field) {
        auto const framed = WireFields::Encode({ field });
        material.insert(material.end(), framed.begin(), framed.end());
    };
    auto const appendText = [&append](std::string_view text) {
        append(AsBytes(text));
    };

    // The STORED bytes, framing included: what one server writes and another
    // reads is this exact byte string, so this is the thing two generations
    // have to agree about. It is downstream of the producer's marker
    // normalization, which is why that rewrite is inside the digest at all --
    // two builds normalizing differently would put different bytes here under
    // one generation, which is the failure this vector exists for.
    append(trace.storedBytes);

    // And the consumer's half, all the way to what the build system actually
    // reads. Localize is the inverse the producer's rewrite is only useful
    // through, so a change to it splits a fleet exactly as a change to
    // Canonicalize does -- and the marker restore after it is the same argument
    // one field over.
    appendText(trace.replayed);
}

/// Digest the whole stored-value contract over the conformance corpus.
/// @return The hex digest, folded exactly as every earlier generation took it.
[[nodiscard]] std::string ConformanceDigest()
{
    std::vector<std::byte> material;
    for (auto const& row: ConformanceCorpus)
        AppendCoveredStages(RunConformanceRow(row), material);
    return HexDigest(Sha256::Hash(material));
}

/// Digest ONE corpus row's contribution.
/// @param row The corpus row.
/// @return Its covered stages, digested alone.
[[nodiscard]] std::string ConformanceRowDigest(ConformanceCase const& row)
{
    std::vector<std::byte> material;
    AppendCoveredStages(RunConformanceRow(row), material);
    return HexDigest(Sha256::Hash(material));
}

} // namespace

TEST_CASE("The canonicalization spec is pinned to the generation byte that names it")
{
    auto const& rows = StoredValueGenerations;
    // An emptied table would pass every check below vacuously, and read exactly like
    // one that found nothing wrong. Each shared assertion re-asks this for itself, so
    // it holds even for a case that reaches only one of them.
    FastCache::Testing::RequireGenerationsPopulated<std::uint8_t>(rows);

    auto const live = ConformanceDigest();

    // `FindOrNull` rather than an iterator: a `std::array` iterator is a raw pointer
    // on libstdc++ and libc++ and a class type on MSVC's, so the spelling the
    // analyser asks for on one host does not compile on another. That argument is
    // the helper's own, which is why this calls it instead of restating it.
    auto const* const pinnedRow = FindOrNull(StoredValueGenerations, CompileValueVersion, &StoredValueGeneration::key);
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
    FastCache::Testing::RequireGenerationsAscending<std::uint8_t>(rows);
    FastCache::Testing::RequireLiveGenerationIsLast<std::uint8_t>(CompileValueVersion, rows);
}

TEST_CASE("Every corpus row is frozen under the live generation")
{
    auto const* const frozen = FindOrNull(FrozenGenerations, CompileValueVersion, &FrozenGeneration::generation);
    {
        INFO("CompileValueVersion is " << static_cast<unsigned>(CompileValueVersion)
                                       << " and no frozen row table names it. A bump freezes the new "
                                          "generation's rows beside the old ones and adds the bump row that "
                                          "says which rows it moved.");
        REQUIRE(frozen != nullptr);
    }

    // Every digest is computed BEFORE any view is taken of one. A `std::string_view`
    // into `digests.back()` would dangle the moment the vector reallocated, and a
    // 64-character digest is past every standard library's inline capacity, so the
    // bug would be invisible until somebody shortened the digest.
    std::vector<std::string> digests;
    for (auto const& row: ConformanceCorpus)
        digests.push_back(ConformanceRowDigest(row));

    std::vector<RowDigest> live;
    for (auto const i: std::views::iota(std::size_t { 0 }, ConformanceCorpus.size()))
        live.push_back(RowDigest { .row = ConformanceCorpus[i].name, .digest = digests[i] });

    auto const delta = CompareFrozenRows(frozen->rows, live);

    // The guard's remedy is part of the guard, so it says where each answer leads
    // rather than only that something is wrong -- and the two directions call for
    // OPPOSITE actions, which is the whole reason the aggregate check spells both.
    std::string paste;
    for (auto const& row: live)
    {
        paste += "    RowDigest { .row = \"";
        paste.append(row.row);
        paste += "\", .digest = \"";
        paste.append(row.digest);
        paste += "\" },\n";
    }

    {
        INFO("the corpus has rows this generation's frozen table does not: "
             << RenderRows(delta.added)
             << ".\nAdding a corpus row is a REPIN and not a bump -- it widens what the generation is "
                "measured over without changing what any server does -- so repin the aggregate digest "
                "and paste this table over Generation"
             << static_cast<unsigned>(CompileValueVersion) << "Rows:\n"
             << paste);
        CHECK(delta.added.empty());
    }
    {
        INFO("this generation's frozen table has rows the corpus no longer holds: "
             << RenderRows(delta.removed)
             << ".\nA row RENAMED reads as one that left and one that arrived, and neither is a claim "
                "about behaviour. Paste this table over Generation"
             << static_cast<unsigned>(CompileValueVersion) << "Rows:\n"
             << paste);
        CHECK(delta.removed.empty());
    }
    {
        INFO("these rows produce something other than what generation "
             << static_cast<unsigned>(CompileValueVersion) << " was frozen with: " << RenderRows(delta.changed)
             << ".\nThat is a BEHAVIOUR change and it is not repinnable: two servers on one wire at "
                "different builds would stamp this same generation on text they rewrote differently. "
                "Bump CompileValueVersion, freeze the new generation's rows, and add the bump row naming "
                "exactly these.\nWhat this adds over the aggregate digest is only WHICH rows -- the "
                "aggregate already fails, since it folds these same stages.");
        CHECK(delta.changed.empty());
    }
}

TEST_CASE("A generation bump names the rows it moved")
{
    // Every real bump against its claim. That loop is EMPTY today -- one generation is
    // frozen, so there is no bump to check -- and a case whose only content is an empty
    // loop passes vacuously, which is what this file refuses everywhere else. So the
    // mechanism is driven below against staged tables, and the static_assert beside
    // `GenerationBumps` is what makes the real loop non-empty the moment a second
    // generation is frozen.
    for (auto const& bump: GenerationBumps)
    {
        auto const* const before = FindOrNull(FrozenGenerations, bump.from, &FrozenGeneration::generation);
        auto const* const after = FindOrNull(FrozenGenerations, bump.to, &FrozenGeneration::generation);
        INFO("bump " << static_cast<unsigned>(bump.from) << " to " << static_cast<unsigned>(bump.to)
                     << " names a generation with no frozen rows");
        REQUIRE(before != nullptr);
        REQUIRE(after != nullptr);

        auto const delta = CompareFrozenRows(before->rows, after->rows);
        INFO("bump " << static_cast<unsigned>(bump.from) << " to " << static_cast<unsigned>(bump.to) << " claims it moved "
                     << RenderRows(bump.changedRows) << " and actually moved " << RenderRows(delta.changed)
                     << ".\nRows that arrived (" << RenderRows(delta.added) << ") and left (" << RenderRows(delta.removed)
                     << ") are corpus width rather than behaviour and are not part of the claim.\nThe "
                        "order is the corpus's own, so list them as they appear there.");
        CHECK(RenderRows(delta.changed) == RenderRows(bump.changedRows));
    }

    SECTION("a moved row is told apart from one that arrived and one that left")
    {
        constexpr auto Before = std::to_array<RowDigest>({
            { .row = "stays", .digest = "aa" },
            { .row = "moves", .digest = "bb" },
            { .row = "leaves", .digest = "cc" },
        });
        constexpr auto After = std::to_array<RowDigest>({
            { .row = "stays", .digest = "aa" },
            { .row = "moves", .digest = "b2" },
            { .row = "arrives", .digest = "dd" },
        });
        auto const delta = CompareFrozenRows(Before, After);
        CHECK(RenderRows(delta.changed) == "moves");
        CHECK(RenderRows(delta.added) == "arrives");
        CHECK(RenderRows(delta.removed) == "leaves");
    }

    SECTION("two identical generations report nothing at all")
    {
        constexpr auto Rows = std::to_array<RowDigest>({
            { .row = "stays", .digest = "aa" },
            { .row = "also stays", .digest = "bb" },
        });
        auto const delta = CompareFrozenRows(Rows, Rows);
        CHECK(RenderRows(delta.changed) == "(none)");
        CHECK(RenderRows(delta.added) == "(none)");
        CHECK(RenderRows(delta.removed) == "(none)");
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
    // Four stages, four columns, and each is asserted where it happens. The two
    // marker stages are separate calls in separate binaries -- the store side runs in
    // the launcher before a value is sent, the replay side after the stale-hit guard
    // has read the canonical marker -- so one column could not honestly cover both.
    for (auto const& row: ConformanceCorpus)
    {
        auto const trace = RunConformanceRow(row);

        auto const observed = [](bool changed) {
            return changed ? RegionEffect::Rewrites : RegionEffect::Preserves;
        };

        INFO("row \"" << row.name << "\" declares storeMarkerEffect=" << NameOf(row.storeMarkerEffect)
                      << " but normalizing the producer's marker "
                      << (trace.normalized != row.text ? "CHANGED" : "did not change") << " the text.");
        CHECK(observed(trace.normalized != row.text) == row.storeMarkerEffect);

        INFO("row \"" << row.name << "\" declares producerEffect=" << NameOf(row.producerEffect) << " but canonicalization "
                      << (trace.canonical != trace.normalized ? "CHANGED" : "did not change")
                      << " the text. A row that rewrites nothing asserts nothing, and the digest cannot see the "
                         "difference.");
        CHECK(observed(trace.canonical != trace.normalized) == row.producerEffect);

        INFO("row \"" << row.name << "\" declares consumerEffect=" << NameOf(row.consumerEffect) << " but localization "
                      << (trace.localized != trace.canonical ? "CHANGED" : "did not change") << " the canonical text.");
        CHECK(observed(trace.localized != trace.canonical) == row.consumerEffect);

        INFO("row \"" << row.name << "\" declares replayMarkerEffect=" << NameOf(row.replayMarkerEffect)
                      << " but restoring the consumer's marker "
                      << (trace.replayed != trace.localized ? "CHANGED" : "did not change") << " the localized text.");
        CHECK(observed(trace.replayed != trace.localized) == row.replayMarkerEffect);
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
