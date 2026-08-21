// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/MurmurHash3.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Separator ending one field. Cannot occur inside a schema tag, which is what
/// makes the tag a prefix-free domain label — see KeyDigest.
inline constexpr std::byte FieldEnd { 0x00 };

/// Separator ending one item of an argument list.
inline constexpr std::byte ListItemEnd { 0x01 };

/// Separator ending one path.
///
/// A separator of its own, so a dependency path can never be read as a trailing
/// argument: the two lists are adjacent and both hold path-shaped strings.
inline constexpr std::byte PathEnd { 0x02 };

/// The launcher's key grammar: a schema tag, then separator-terminated fields,
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
/// **It is also the whole of the domain separation between the key spaces**, now
/// that the four salt bytes that used to distinguish them are gone (issue #63 —
/// the salts never separated anything, they were four runs of one CRC). A tag is
/// NUL-free and is followed by `FieldEnd`, so tag+NUL is a prefix-free code:
/// `objkey-v3`, `manifest-v3` and `header-state-v1` differ in their first byte,
/// so no blob in one domain can equal a blob in another. That is *stronger* than
/// the salts were — a salt made a cross-domain collision improbable, whereas
/// disjoint inputs reduce it to an ordinary collision, now at 2^-128 rather than
/// 2^-32. Any future domain gets the same separation for free provided it
/// follows the same rule.
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
    /// @param schemaTag Schema label, e.g. `objkey-v3`. Must not contain a NUL.
    explicit KeyDigest(std::string_view schemaTag)
    {
        Field(schemaTag);
    }

    /// Fold in one field.
    /// @param value Field contents.
    void Field(std::string_view value)
    {
        _digest.Update(value);
        _digest.Update(FieldEnd);
    }

    /// Fold in one item of an argument list.
    /// @param value Item contents.
    void Item(std::string_view value)
    {
        _digest.Update(value);
        _digest.Update(ListItemEnd);
    }

    /// Fold in one path.
    /// @param value Path contents.
    void Path(std::string_view value)
    {
        _digest.Update(value);
        _digest.Update(PathEnd);
    }

    /// @return The key as 32 lowercase hex characters.
    [[nodiscard]] std::string ToHex() const
    {
        return _digest.ToHex();
    }

    /// Width of every key this grammar produces, in characters.
    static constexpr std::size_t HexLength = MurmurHash3::HexLength;

  private:
    MurmurHash3 _digest;
};

} // namespace FastCache::Cc
