// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/MurmurHash3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// What a piece of a key is. The byte is folded into the digest ahead of the
/// piece's length, so a value fed as a `Field` can never digest the same as the
/// identical value fed as an `Item` or a `Path`.
///
/// The two lists these distinguish are adjacent and both hold path-shaped
/// strings, so a dependency path must not be able to read as a trailing
/// argument.
enum class KeyPiece : std::uint8_t
{
    Field = 0x00,    ///< A standalone field.
    ListItem = 0x01, ///< One item of an argument list.
    Path = 0x02,     ///< One path.
};

/// The launcher's key grammar: a schema tag, then length-prefixed pieces,
/// folded straight into a 128-bit digest.
///
/// **The schema tag is a constructor parameter and there is no other way to
/// start a digest.** That is deliberate. Every key this launcher computes has to
/// carry a tag naming the schema it was written under, because nothing else in a
/// key describes how the stored value is framed or canonicalized: without one, a
/// change to either would leave old entries matching new keys and being served
/// under rules they were not written by. Making the tag unskippable turns that
/// from a convention three call sites have to remember into something the type
/// system asks for.
///
/// **It is also the whole of the domain separation between the key spaces**,
/// now that the four salt bytes that used to distinguish them are gone (issue
/// #63 — the salts never separated anything, they were four runs of one CRC).
/// `objkey-v4`, `manifest-v4` and `header-state-v1` are different lengths and
/// different bytes, so no blob in one domain can equal a blob in another.
///
/// **Every piece is length-prefixed, not separator-terminated**, and that is
/// what makes the whole encoding injective rather than merely usually
/// unambiguous. Terminating a value with a byte that can occur *inside* a value
/// is not a framing: with `Field` writing `value` then NUL, the inputs
///
///     { compilerId = "cc\0d", preprocessed = "x"   }
///     { compilerId = "cc",     preprocessed = "d\0x" }
///
/// produce byte-identical blobs and therefore the same key — two unrelated
/// translation units sharing a cache entry, which is exactly the mis-serve
/// issue #63 is about, reached by a different route. Preprocessed text can carry
/// a raw NUL and a build system can pass an argument containing 0x01, so this
/// was reachable rather than theoretical. Writing `kind`, then the length as a
/// big-endian `u64`, then the bytes makes the blob parseable in principle and so
/// injective in fact, for every possible field content.
///
/// Big-endian for the length because that is the byte order everything else in
/// this protocol writes (`AppendU32` in DirectManifest.cpp), and because a
/// *host*-order length would make the key differ between a big-endian and a
/// little-endian machine — the same cross-machine hazard `Core/MurmurHash3.hpp`
/// closes for its block loads.
///
/// It also removes the need for the tag to avoid any particular byte: the old
/// scheme rested domain separation on tags being NUL-free, a precondition
/// nothing enforced.
///
/// It streams. `ComputeKey` used to materialize its entire blob — a second copy
/// of a preprocessed text running to megabytes — purely in order to hash it. The
/// digest is a pure function of the concatenation, so feeding the pieces in
/// order produces the identical key with that peak allocation gone, on the path
/// where the machine is already busiest. `MurmurHash3_test.cpp` pins the
/// streaming/one-shot equivalence that makes the two spellings interchangeable.
class KeyDigest
{
  public:
    /// Begin a digest in the domain named by `schemaTag`.
    /// @param schemaTag Schema label, e.g. `objkey-v4`.
    explicit KeyDigest(std::string_view schemaTag)
    {
        Field(schemaTag);
    }

    /// Fold in one field.
    /// @param value Field contents.
    void Field(std::string_view value)
    {
        Emit(KeyPiece::Field, value);
    }

    /// Fold in one item of an argument list.
    /// @param value Item contents.
    void Item(std::string_view value)
    {
        Emit(KeyPiece::ListItem, value);
    }

    /// Fold in one path.
    /// @param value Path contents.
    void Path(std::string_view value)
    {
        Emit(KeyPiece::Path, value);
    }

    /// @return The key as 32 lowercase hex characters.
    [[nodiscard]] std::string ToHex() const
    {
        return _digest.ToHex();
    }

    /// Width of every key this grammar produces, in characters.
    static constexpr std::size_t HexLength = MurmurHash3::HexLength;

  private:
    /// Fold one length-prefixed piece into the digest.
    /// @param kind What this piece is.
    /// @param value The piece's bytes.
    void Emit(KeyPiece kind, std::string_view value)
    {
        std::array<std::byte, 1 + sizeof(std::uint64_t)> header {};
        header[0] = static_cast<std::byte>(kind);
        WriteBigEndian<std::uint64_t>(std::span<std::byte> { header }.subspan(1), value.size());
        _digest.Update(BytesView { header });
        _digest.Update(value);
    }

    MurmurHash3 _digest;
};

} // namespace FastCache::Cc
