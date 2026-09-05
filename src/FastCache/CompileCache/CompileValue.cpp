// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <array>
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
                return true;
        }
        return false;
    }

    /// Append a big-endian u32 to `out`.
    /// @param out Destination byte vector.
    /// @param n   Value to append.
    void AppendU32(std::vector<std::byte>& out, std::uint32_t n)
    {
        std::array<std::byte, sizeof(std::uint32_t)> buf {};
        WriteBigEndian<std::uint32_t>(buf, n);
        out.insert(out.end(), buf.begin(), buf.end());
    }

    /// Append raw bytes of a string to `out`.
    /// @param out Destination byte vector.
    /// @param s   Source string.
    void AppendBytes(std::vector<std::byte>& out, std::string_view s)
    {
        auto const* p = reinterpret_cast<std::byte const*>(s.data());
        out.insert(out.end(), p, p + s.size());
    }

    /// A cursor that reads from a byte span with bounds checking. Every read
    /// returns false (via the caller) rather than over-reading a short buffer.
    class Cursor
    {
      public:
        explicit Cursor(std::span<std::byte const> bytes) noexcept:
            _bytes(bytes)
        {
        }

        /// @return Bytes not yet consumed.
        [[nodiscard]] std::size_t Remaining() const noexcept
        {
            return _bytes.size() - _pos;
        }

        /// Read one byte. @param out [out] the byte. @return false if none left.
        [[nodiscard]] bool ReadU8(std::uint8_t& out) noexcept
        {
            if (Remaining() < 1)
                return false;
            out = static_cast<std::uint8_t>(_bytes[_pos]);
            _pos += 1;
            return true;
        }

        /// Read a big-endian u32. @param out [out] value. @return false if short.
        [[nodiscard]] bool ReadU32(std::uint32_t& out) noexcept
        {
            if (Remaining() < sizeof(std::uint32_t))
                return false;
            out = ReadBigEndian<std::uint32_t>(_bytes.subspan(_pos, sizeof(std::uint32_t)));
            _pos += sizeof(std::uint32_t);
            return true;
        }

        /// Read `n` bytes into a fresh vector. @return false if fewer than `n` left.
        [[nodiscard]] bool ReadBytes(std::size_t n, std::vector<std::byte>& out)
        {
            if (Remaining() < n)
                return false;
            auto const chunk = _bytes.subspan(_pos, n);
            out.assign(chunk.begin(), chunk.end());
            _pos += n;
            return true;
        }

        /// Read `n` bytes into a fresh string. @return false if fewer than `n` left.
        [[nodiscard]] bool ReadString(std::size_t n, std::string& out)
        {
            if (Remaining() < n)
                return false;
            auto const chunk = _bytes.subspan(_pos, n);
            out.assign(reinterpret_cast<char const*>(chunk.data()), n);
            _pos += n;
            return true;
        }

      private:
        std::span<std::byte const> _bytes;
        std::size_t _pos { 0 };
    };

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

    /// Where the generation sits in an encoded value: its leading byte.
    ///
    /// The named form of a position `DecodeCompileValue` also reads, through its
    /// `Cursor`'s first `ReadU8`. One fact in two syntaxes, and they must move
    /// together -- a field added ahead of the generation changes both. It exists so
    /// that `CanonicalStoredValue`, which needs the number for a refusal that names
    /// it, asks a question with a name rather than writing `front()` a hundred lines
    /// from the code that guarantees the answer.
    ///
    /// @param bytes An encoded value, possibly empty.
    /// @return The declared generation, or none when there is no leading byte to
    ///         declare one -- which is not the same as declaring generation zero.
    [[nodiscard]] std::optional<std::uint8_t> DeclaredGeneration(std::span<std::byte const> bytes) noexcept
    {
        if (bytes.empty())
            return std::nullopt;
        return static_cast<std::uint8_t>(bytes.front());
    }

} // namespace

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
    std::vector<std::byte> out;
    // Reserved before the first insert, which is what the object blob costs and is
    // the bulk of any real value. It also keeps GCC 16 from reasoning about the
    // five-byte buffer the header alone would leave: it deduced the insertion point
    // as "offset 5 out of the bounds [0, 5]" and reported -Warray-bounds= against
    // the reallocation that was about to happen, fatal under WERROR (#805).
    out.reserve(1 + sizeof(std::uint32_t) + value.objectBlob.size());
    out.push_back(static_cast<std::byte>(CompileValueVersion));

    AppendU32(out, static_cast<std::uint32_t>(value.objectBlob.size()));
    out.insert(out.end(), value.objectBlob.begin(), value.objectBlob.end());

    AppendU32(out, static_cast<std::uint32_t>(value.textRegions.size()));
    for (auto const& region: value.textRegions)
    {
        out.push_back(static_cast<std::byte>(region.grammar));
        AppendU32(out, static_cast<std::uint32_t>(region.bytes.size()));
        AppendBytes(out, region.bytes);
    }
    return out;
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
    [[nodiscard]] std::expected<CompileValue, ProtocolError> DecodeAfterGeneration(Cursor& cursor)
    {
        CompileValue value;

        std::uint32_t objectLen {};
        if (!cursor.ReadU32(objectLen))
            return Malformed("truncated object length");
        if (!cursor.ReadBytes(objectLen, value.objectBlob))
            return Malformed("truncated object blob");

        std::uint32_t regionCount {};
        if (!cursor.ReadU32(regionCount))
            return Malformed("truncated region count");

        // The count is a claim about bytes this frame must already carry -- see
        // `WireFields::DeclaredCountFits` for why that is checkable and why it is checked
        // before anything is sized from it (issue #267).
        if (!WireFields::DeclaredCountFits(regionCount, MinRegionBytes, cursor.Remaining()))
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

            std::uint32_t textLen {};
            if (!cursor.ReadU32(textLen))
                return Malformed("truncated region text length");

            TextRegion region { .grammar = static_cast<PathCanon::Grammar>(grammarTag), .bytes = {} };
            if (!cursor.ReadString(textLen, region.bytes))
                return Malformed("truncated region text");
            value.textRegions.push_back(std::move(region));
        }

        if (cursor.Remaining() != 0)
            return Malformed("trailing bytes after compile-value frame");

        return value;
    }

} // namespace

std::expected<CompileValue, ProtocolError> DecodeCompileValue(std::span<std::byte const> bytes)
{
    Cursor cursor { bytes };

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
