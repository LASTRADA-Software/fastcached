// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/WireFields.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Testing
{

/// A wire blob assembled field by field, so a case can declare a count the blob
/// cannot possibly supply and stop right there.
///
/// This exists because of ONE recurring defect class -- a declared element count
/// reserved from before the bytes behind it were counted (#241, #267, #269, #271) --
/// and the test for each instance has the same shape: build a header, write a
/// hostile count, leave some trailing bytes, and check the decoder refuses rather
/// than reserving. Three files had written that shape independently, down to an
/// identical `for (auto const shift: { 24, 16, 8, 0 })` loop, and the class is not
/// finished: #304 and the encoder ticket still have to write it again. A test idiom
/// that reappears with each instance of a recurring bug class is the one worth
/// lifting, because its whole job is to make the next instance cheap to guard (#306).
///
/// **No format is baked in.** The copy this generalises put `StreamCodec::Magic` and
/// `TypeStream` in its constructor, which is exactly why neither of the other two
/// sites could adopt it -- a builder that knows one format is a builder for one file.
/// Every prefix is a STEP, so a caller spells its own header and this class stays
/// ignorant of what it is building.
///
/// The width conversions are `WireFields::ToBigEndian`, not a fourth shift loop. That
/// is not tidiness: a hand-rolled loop in a test is a second implementation of the
/// convention under test, so a blob built wrong the same way the decoder reads wrong
/// would still round-trip and the case would pass.
class DeclaredCountBlob
{
  public:
    /// One raw byte -- a version, a type tag, a discriminator.
    DeclaredCountBlob& Byte(std::uint8_t value)
    {
        _out.push_back(static_cast<std::byte>(value));
        return *this;
    }

    /// A big-endian `u32`: a length, or the hostile count itself.
    DeclaredCountBlob& U32(std::uint32_t value)
    {
        return Append(WireFields::ToBigEndian<std::uint32_t>(value));
    }

    /// A big-endian `u64`.
    DeclaredCountBlob& U64(std::uint64_t value)
    {
        return Append(WireFields::ToBigEndian<std::uint64_t>(value));
    }

    /// A length-prefixed empty string: the cheapest a field can be on the wire.
    DeclaredCountBlob& EmptyField()
    {
        return U32(0);
    }

    /// Trailing bytes for the declared elements to be decoded from.
    ///
    /// Deliberately usable with a NON-zero count. The clamp these cases were written
    /// against was `min(count, remainingBytes)`, which with nothing trailing clamps to
    /// zero and looks like a refusal -- so it is the bytes being PRESENT that exposes
    /// a one-byte-per-element assumption.
    DeclaredCountBlob& Pad(std::size_t count)
    {
        _out.insert(_out.end(), count, std::byte { 0 });
        return *this;
    }

    /// @return The bytes, borrowed. Valid while this builder is.
    [[nodiscard]] std::span<std::byte const> Bytes() const noexcept
    {
        return _out;
    }

    /// @return The bytes, owned. For a decoder taking a `vector`.
    [[nodiscard]] std::vector<std::byte> const& Vector() const noexcept
    {
        return _out;
    }

    /// @return The bytes as a `std::string`, for a decoder that takes one.
    ///
    /// Both spellings exist because the three call sites genuinely differ: the
    /// manifest decoder takes a `std::string` and the compile-value one takes bytes.
    /// Converting at the call site instead would put a `reinterpret_cast` in three
    /// test files, which is the thing a shared fixture is for.
    [[nodiscard]] std::string String() const
    {
        std::string out;
        out.reserve(_out.size());
        for (auto const byte: _out)
            out.push_back(static_cast<char>(byte));
        return out;
    }

  private:
    template <std::size_t N>
    DeclaredCountBlob& Append(std::array<std::byte, N> const& bytes)
    {
        _out.insert(_out.end(), bytes.begin(), bytes.end());
        return *this;
    }

    std::vector<std::byte> _out;
};

/// The largest count a `u32` field can hold -- the shape every instance of this
/// defect class was reported with.
inline constexpr std::uint32_t ImpossibleCount = 0xFFFFFFFFU;

} // namespace FastCache::Testing
