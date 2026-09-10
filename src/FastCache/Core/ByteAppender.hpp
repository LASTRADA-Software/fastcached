// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace FastCache
{

/// A growable buffer of byte-sized elements: what an encoder here writes into.
///
/// Two spellings occur in this tree and both satisfy it -- `std::vector<std::byte>` for
/// a stored value blob, `std::string` for a manifest or a history file -- which is why
/// `ByteAppender` is a template over the buffer rather than fixed to one of them. A
/// second appender written for the other spelling is the drift this whole exercise
/// removes.
///
/// The element must be one byte and trivially copyable, which is what makes
/// `AppendRaw`'s pointer cast legal: `std::byte`, `char` and `unsigned char` may each
/// alias any object.
template <typename Buffer>
concept ByteBuffer = requires(Buffer& out, Buffer::value_type element, Buffer::value_type const* first) {
    requires sizeof(element) == 1;
    requires std::is_trivially_copyable_v<typename Buffer::value_type>;
    out.push_back(element);
    out.insert(out.end(), first, first);
};

/// The writer half of the wire grammar `ByteCursor` reads.
///
/// ## Why this exists
///
/// `ByteCursor::ReadField` gave `[u32 length][bytes]` a name on the reader's side, and
/// the writer's side went on being spelled by hand in six encoders -- four of them
/// building the length prefix with `WriteBigEndian` into a local array, one with a shift
/// chain, each then inserting the payload itself
/// ([#305](https://github.com/LASTRADA-Software/fastcached/issues/305)). Naming one
/// direction and not the other is the worse of the two states: the hand-rolled writer
/// stops reading as leftover and starts reading as deliberate, and an encoder and a
/// decoder that do not share a statement of the grammar are how the two drift.
///
/// ## What it borrows
///
/// It holds a **reference** to the caller's buffer and appends to it, exactly as
/// `ByteCursor` holds a span over the caller's bytes. It is a walk, not a value: the
/// buffer outlives it, and nothing here is returned by value to a caller that might
/// outlive what it points at. See `.agent/rules/wire-and-protocol.md` on why a decoded
/// struct returned by value must not borrow -- this is the other shape, and the
/// distinction is that this one never leaves the expression that made it.
///
/// ## Where it lives
///
/// Beside `ByteCursor` and under the same constraint, which `Core/WireFields.hpp`
/// argues in full: it must be includable from a header `fastcache-cc` compiles
/// **without linking `FastCache`**, so it stays header-only and reaches nothing but
/// `Core/Bytes.hpp`, `Core/Endian.hpp`, `Core/WireFields.hpp` and the standard library.
/// Anything added here keeps both properties.
///
/// ## No bare `u32` append
///
/// The public surface has none, mirroring `ByteCursor::ReadU32` being private and for
/// the same reason turned around: a bare `u32` on this wire is either a field's length
/// prefix or an element count, and each of those has a member that enforces something a
/// bare append cannot. `AppendField` refuses a payload whose length cannot be declared;
/// `AppendCount` refuses a count that cannot be written. Both refuse rather than
/// truncate, the way `WireFields::RequireEncodable` already did for a whole field list,
/// and every site this replaced cast the size down instead.
///
/// The surface is otherwise deliberately only what a caller reaches today, as
/// `ByteCursor`'s is. A member that would pull in a new dependency belongs elsewhere.
///
/// @tparam Buffer The buffer being appended to.
template <ByteBuffer Buffer>
class ByteAppender
{
  public:
    /// One element of the buffer -- `std::byte` for a value blob, `char` for a manifest.
    /// Named once because both the byte append and the raw insert have to spell it, and
    /// a second spelling is how the two would come to disagree about what a byte is.
    using Element = Buffer::value_type;

    /// @param out The buffer to append to; it must outlive this appender.
    explicit constexpr ByteAppender(Buffer& out) noexcept:
        _out { out }
    {
    }

    /// Append one byte: a magic marker, a type tag, a format version.
    ///
    /// Takes `std::byte` where `ByteCursor::ReadU8` yields `std::uint8_t`, because every
    /// caller here already holds one -- a `constexpr std::byte Magic`, a grammar
    /// enumerator cast for the wire -- and a `std::string` buffer would otherwise need a
    /// second cast to `char` at the call site to say the same thing.
    /// @param value The byte to append.
    void AppendByte(std::byte value)
    {
        _out.push_back(static_cast<Element>(value));
    }

    /// Append a big-endian `u64`, as `ByteCursor::ReadU64` reads one.
    /// @param value Host-order value.
    void AppendU64(std::uint64_t value)
    {
        AppendRaw(WireFields::ToBigEndian<std::uint64_t>(value));
    }

    /// Append an element count as a big-endian `u32`.
    ///
    /// The counterpart to `ByteCursor::ReadCount`, and deliberately a member here rather
    /// than a function in `WireFields`: a count is not part of that grammar. `WireFields`
    /// knows `[u32 length][bytes]` and nothing else, and each protocol owns what its own
    /// counts mean -- which is why `StreamCodec` spells `CountBytes` apart from
    /// `FieldPrefixSize` even though both are four.
    ///
    /// The reader's side of a count carries the security bound (`minBytesEach`); the
    /// writer's side carries the ceiling, because a `std::size_t` container size is what
    /// every caller passes and a cast down to `u32` would declare a count the payload
    /// does not hold.
    /// @param count How many elements follow.
    /// @throws std::length_error When the count exceeds what a `u32` field can carry.
    void AppendCount(std::size_t count)
    {
        if (std::uint64_t { count } > WireFields::MaxPayload)
            throw std::length_error("wire element count exceeds the u32 count field");
        AppendU32(static_cast<std::uint32_t>(count));
    }

    /// Append one length-prefixed field: a `u32` length, then those bytes.
    ///
    /// The grammar `WireFields` describes, written one field at a time --
    /// `ByteCursor::ReadField` and `ByteCursor::ReadFieldBytes` are what read it back.
    /// @param field The field's bytes.
    /// @throws std::length_error When the field exceeds the u32 ceiling.
    void AppendField(std::span<std::byte const> field)
    {
        AppendU32(WireFields::RequireFieldLength(field.size()));
        AppendRaw(field);
    }

    /// Append one length-prefixed field holding text.
    /// @param field The field's text.
    /// @throws std::length_error When the field exceeds the u32 ceiling.
    void AppendField(std::string_view field)
    {
        AppendField(AsBytes(field));
    }

    /// Append bytes with no length prefix in front of them.
    ///
    /// For a format whose **own** grammar supplies the framing: `FleetHistory`'s file is
    /// `[u64 length][bytes]`, which is not this grammar and must not be forced into it.
    /// A `[u32 length][bytes]` field is `AppendField`'s, and spelling one as an
    /// `AppendCount` followed by an `AppendRaw` is the hand-rolling this type removes.
    /// @param bytes The bytes to append.
    void AppendRaw(std::span<std::byte const> bytes)
    {
        // Returned early rather than left to an insert of an empty range, because
        // `bytes.data()` may legally be null for an empty span and the pointer cast and
        // arithmetic below would then be performed on it. The same defensiveness
        // `WireFields::Encode` documents at its own zero-size return.
        if (bytes.empty())
            return;
        auto const* const first = reinterpret_cast<Element const*>(bytes.data());
        _out.insert(_out.end(), first, first + bytes.size());
    }

    /// Append text with no length prefix in front of it.
    /// @param text The text to append.
    void AppendRaw(std::string_view text)
    {
        AppendRaw(AsBytes(text));
    }

  private:
    /// Append a big-endian `u32`. Private: a bare `u32` append is how a length prefix
    /// or a count gets written without the check that makes it honest, which is the
    /// defect this type exists to make unwritable.
    /// @param value Host-order value.
    void AppendU32(std::uint32_t value)
    {
        AppendRaw(WireFields::ToBigEndian<std::uint32_t>(value));
    }

    Buffer& _out;
};

} // namespace FastCache
