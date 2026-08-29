// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <array>
#include <ranges>
#include <utility>

namespace FastCache
{
namespace
{

    constexpr std::uint8_t CompileValueVersion = 1;

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

    [[nodiscard]] std::unexpected<ProtocolError> Malformed(std::string context)
    {
        return std::unexpected(ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = std::move(context) });
    }

} // namespace

std::vector<std::byte> EncodeCompileValue(CompileValue const& value)
{
    std::vector<std::byte> out;
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

std::expected<CompileValue, ProtocolError> DecodeCompileValue(std::span<std::byte const> bytes)
{
    Cursor cursor { bytes };

    std::uint8_t version {};
    if (!cursor.ReadU8(version))
        return Malformed("empty compile-value frame");
    if (version != CompileValueVersion)
        return Malformed("unknown compile-value version");

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

} // namespace FastCache
