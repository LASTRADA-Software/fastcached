// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/ByteAppender.hpp>
#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace FastCache
{
namespace
{

    /// The fewest wire bytes one encoded text region can occupy: its grammar tag and
    /// its length prefix, with empty text. Read straight off `EncodeCompileValue`'s
    /// loop below, and pinned against it by a test that encodes one empty region and
    /// measures the difference -- so a field added to that loop fails a test rather
    /// than quietly weakening the guard this feeds.
    ///
    /// Spelled with `FieldPrefixSize` rather than a literal 4, which would restate the
    /// framing contract beside the one place it is defined.
    constexpr std::size_t MinRegionBytes = sizeof(std::uint8_t) + WireFields::FieldPrefixSize;

    /// The grammar tags that DecodeCompileValue accepts. Kept in sync with
    /// PathCanon::Grammar; an out-of-range tag is a malformed frame.
    [[nodiscard]] bool IsKnownGrammar(std::uint8_t tag) noexcept
    {
        switch (static_cast<PathCanon::Grammar>(tag))
        {
            case PathCanon::Grammar::ShowIncludes:
            case PathCanon::Grammar::MsvcDiagnostics:
            case PathCanon::Grammar::GccDepfile:
            case PathCanon::Grammar::GccDiagnostics:
                return true;
        }
        return false;
    }

    /// One typed refusal from this decoder.
    /// @param code    Which kind of refusal this is.
    /// @param context What was wrong with the bytes, for a log.
    /// @return The refusal.
    [[nodiscard]] std::unexpected<ProtocolError> Refuse(ProtocolErrorCode code, std::string context)
    {
        return std::unexpected(ProtocolError { .code = code, .context = std::move(context) });
    }

    /// The refusal for bytes that are damaged or mis-framed.
    /// @param context What was wrong.
    /// @return The refusal.
    [[nodiscard]] std::unexpected<ProtocolError> Malformed(std::string context)
    {
        return Refuse(ProtocolErrorCode::MalformedFrame, std::move(context));
    }

    /// The refusal for a value whose leading byte names a generation this build does
    /// not implement -- deliberately NOT `Malformed`, because the bytes are not
    /// damaged and a caller that cannot tell those two apart applies one policy to
    /// both. `IsForeignGeneration` is how a caller reads it back.
    /// @param generation The leading byte that was read.
    /// @return The typed refusal, naming both generations.
    [[nodiscard]] std::unexpected<ProtocolError> ForeignGenerationRefusal(std::uint8_t generation)
    {
        return Refuse(ProtocolErrorCode::UnsupportedFeature, ForeignGenerationMessage(generation));
    }

} // namespace

std::optional<std::uint8_t> DeclaredGeneration(std::span<std::byte const> bytes) noexcept
{
    if (bytes.size() <= CompileValueGenerationOffset)
        return std::nullopt;
    return static_cast<std::uint8_t>(bytes[CompileValueGenerationOffset]);
}

bool IsForeignGeneration(ProtocolError const& error) noexcept
{
    return error.code == ProtocolErrorCode::UnsupportedFeature;
}

std::string ForeignGenerationMessage(std::uint8_t generation)
{
    return std::format("stored value is generation {}; this build implements {}", generation, CompileValueVersion);
}

std::vector<std::byte> EncodeCompileValue(CompileValue const& value)
{
    std::vector<std::byte> blob;
    ByteAppender out { blob };
    out.AppendByte(static_cast<std::byte>(CompileValueVersion));
    out.AppendField(value.objectBlob);
    out.AppendCount(value.textRegions.size());
    for (auto const& region: value.textRegions)
    {
        out.AppendByte(static_cast<std::byte>(region.grammar));
        out.AppendField(region.bytes);
    }
    return blob;
}

namespace
{
    /// Everything after the generation byte, which is the whole of the layout.
    ///
    /// Split out because it is run TWICE, and the second run is what tells a stored
    /// value of another generation from bytes that are not a stored value at all: the
    /// generation byte alone cannot, since almost no opaque blob happens to begin with
    /// this build's. `DecodeCompileValue` reads the leading byte and hands the rest
    /// here either way.
    ///
    /// @param cursor Positioned immediately after the generation byte.
    /// @return The decoded value, or why the layout did not hold.
    [[nodiscard]] std::expected<CompileValue, ProtocolError> DecodeAfterGeneration(ByteCursor& cursor)
    {
        CompileValue value;

        // One call rather than a length read and a sized read: `ReadFieldBytes` checks
        // the length against the bytes present before copying any, which is the
        // guarantee the two-step spelling had to remember to provide.
        //
        // The short-buffer case is separated back out first, because folding the two
        // reads also folds their REFUSALS, and a log reader diagnoses a frame that
        // stopped before the length from one that declared more than it carried in
        // different places. `CapacityFor(1)` is the count of bytes still present.
        if (cursor.CapacityFor(1) < sizeof(std::uint32_t))
            return Malformed("truncated object length");
        if (!cursor.ReadFieldBytes(value.objectBlob))
            return Malformed("truncated object blob");

        // The count is a claim about bytes this frame must already carry, checked before
        // anything is sized from it (issue #267). `ReadCount` is the only way to obtain
        // one, and it cannot be called without stating what an element costs.
        std::uint32_t regionCount {};
        if (cursor.CapacityFor(1) < sizeof(std::uint32_t))
            return Malformed("truncated region count");
        if (!cursor.ReadCount(regionCount, MinRegionBytes))
            return Malformed("region count exceeds what the remaining bytes can supply");

        // No `reserve(regionCount)`: a validated count is still an amplifier, and the
        // realistic count here is one per grammar, so growing from the regions actually
        // decoded costs nothing measurable beside the object blob copied just above.
        for ([[maybe_unused]] auto const _: std::views::iota(std::uint32_t { 0 }, regionCount))
        {
            std::uint8_t grammarTag {};
            if (!cursor.ReadU8(grammarTag))
                return Malformed("truncated region grammar");
            if (!IsKnownGrammar(grammarTag))
                return Malformed("unknown region grammar tag");

            TextRegion region { .grammar = static_cast<PathCanon::Grammar>(grammarTag), .bytes = {} };
            if (cursor.CapacityFor(1) < sizeof(std::uint32_t))
                return Malformed("truncated region text length");
            if (!cursor.ReadField(region.bytes))
                return Malformed("truncated region text");
            value.textRegions.push_back(std::move(region));
        }

        // `AtEnd` rather than a remaining-count comparison: a FAILED cursor also has zero
        // remaining, so the subtraction spelling reports a malformed frame as a clean one.
        if (!cursor.AtEnd())
            return Malformed("trailing bytes after compile-value frame");

        return value;
    }

} // namespace

std::expected<CompileValue, ProtocolError> DecodeCompileValue(std::span<std::byte const> bytes)
{
    ByteCursor cursor { bytes };

    std::uint8_t version {};
    if (!cursor.ReadU8(version))
        return Malformed("empty compile-value frame");

    // The layout is parsed BEFORE the generation is judged, because a leading byte
    // that is not ours is on its own no evidence of another generation. Reading it
    // that way was a real defect rather than a conservative one: almost no opaque
    // blob begins with 0x01, so every opaque value was called foreign and REFUSED --
    // destroying the node cache tier's documented policy of storing an opaque value
    // verbatim, which is a policy this layer has no business overturning. Two of that
    // tier's tests said so.
    //
    // So a foreign generation is reported only for a frame that HOLDS TOGETHER behind
    // the byte: positive evidence, rather than the absence of ours.
    //
    // A frame that HOLDS TOGETHER behind a foreign byte is one kind of positive
    // evidence. `MaxCompileValueGeneration` is the other, and it is what closed the
    // residual this comment used to describe (#552): a future generation that moves
    // the FRAMING as well parses as junk, so evidence-from-layout cannot see it, and
    // a node's tier stores what it cannot decode VERBATIM -- putting the producing
    // checkout's absolute paths under a key every machine computes, which is #229
    // arriving through the very door #483 closed. The class of bump most likely to
    // cause it, too, since `CompileValueVersion` names the framing and nothing
    // couples it to an `objkey-v*` bump.
    //
    // So the reserved leading-byte range decides that case, and the ORDER of the two
    // tests below is the whole of it: a byte inside the range is a stored value of
    // some generation whether or not this build can parse the rest, and only a byte
    // OUTSIDE it is the opaque blob the tier's verbatim policy is for. The contract
    // and what it costs are on `MaxCompileValueGeneration`.
    //
    // Our OWN generation byte is answered first and separately. A frame stamped with
    // it that does not parse is either damage or an opaque blob that happens to start
    // with this number -- never a foreign generation, and reporting one would be a
    // lie about which build wrote it.
    auto decoded = DecodeAfterGeneration(cursor);
    if (version == CompileValueVersion)
        return decoded;
    if (decoded.has_value())
        return ForeignGenerationRefusal(version);
    if (version >= 1 && version <= MaxCompileValueGeneration)
        return ForeignGenerationRefusal(version);
    return Malformed(std::format("leading byte {} is outside the reserved compile-value generation range 1-{}, so "
                                 "these bytes are not a stored value",
                                 version,
                                 MaxCompileValueGeneration));
}

StoredValueCanonicalization CanonicalStoredValue(std::span<std::byte const> value,
                                                 std::string_view sourceRoot,
                                                 std::string_view buildTree)
{
    auto decoded = DecodeCompileValue(value);
    if (!decoded.has_value())
    {
        // The one place the two absent cases are told apart, and it asks the shared
        // predicate rather than comparing the code itself -- two spellings of one
        // rule are two places for it to drift, which is the argument this file makes
        // about the canonicalization recipe one paragraph up.
        if (!IsForeignGeneration(decoded.error()))
            return { .bytes = {}, .outcome = CanonicalizationOutcome::NotACompileValue, .generation = 0 };

        return { .bytes = {},
                 .outcome = CanonicalizationOutcome::ForeignGeneration,
                 // Cannot be absent: that refusal is only produced after the leading
                 // byte was read. `value_or` rather than a dereference so the
                 // impossible case is a zero rather than undefined behaviour.
                 .generation = DeclaredGeneration(value).value_or(0) };
    }

    PathCanon::Layout const producer { .sourceRoot = std::string { sourceRoot }, .buildTree = std::string { buildTree } };

    // The object blob is never a region and is never rewritten: it is machine code,
    // and a byte sequence inside it that happens to look like a path is not one.
    for (auto& region: decoded->textRegions)
        region.bytes = PathCanon::CanonicalizeRegion(region.bytes, region.grammar, producer);

    return { .bytes = EncodeCompileValue(*decoded), .outcome = CanonicalizationOutcome::Canonicalized, .generation = 0 };
}

} // namespace FastCache
