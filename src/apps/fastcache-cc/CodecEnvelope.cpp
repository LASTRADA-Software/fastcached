// SPDX-License-Identifier: Apache-2.0
#include "CodecEnvelope.hpp"

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <concepts>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What one envelope refusal means on the wire and to a person.
    ///
    /// Both in one row, deliberately, exactly as `WorkerProtocol`'s `RefusalTable`
    /// pairs a code with its counter and for the same reason: they answer the same
    /// question asked by two audiences, and a refusal reported under one code while
    /// being described as another sends an operator to look for something that did
    /// not happen.
    ///
    /// No member carries a default. A row answering only two of the three questions
    /// is not a row, and `ErrorCode` has no zero enumerator to default to anyway.
    struct EnvelopeErrorRow
    {
        EnvelopeError error;   ///< The reason this row describes.
        Wire::ErrorCode code;  ///< What the peer is told.
        std::string_view text; ///< What a person reads.
    };

    /// One row per `EnvelopeError`, in enumerator order.
    constexpr EnumTable<EnvelopeError, EnvelopeErrorRow> ErrorTable { {
        { .error = EnvelopeError::Malformed,
          .code = Wire::ErrorCode::MalformedFrame,
          .text = "the codec envelope is malformed" },
        { .error = EnvelopeError::UnsupportedCodec,
          .code = Wire::ErrorCode::UnsupportedCodec,
          .text = "the payload is in a codec this build cannot decode" },
        { .error = EnvelopeError::DeclaredTooLarge,
          .code = Wire::ErrorCode::PayloadTooLarge,
          .text = "the declared decompressed size exceeds this endpoint's ceiling" },
        { .error = EnvelopeError::Corrupt,
          .code = Wire::ErrorCode::MalformedFrame,
          .text = "the payload does not expand to its declared size" },
    } };

    static_assert(RowsInEnumeratorOrder(ErrorTable, &EnvelopeErrorRow::error),
                  "ErrorTable must hold one row per EnvelopeError, in enumerator order");

    /// The row describing @p error.
    /// @param error The reason, never `Last`.
    /// @return Its descriptor.
    [[nodiscard]] constexpr EnvelopeErrorRow const& RowFor(EnvelopeError error) noexcept
    {
        return ErrorTable[static_cast<std::size_t>(error)];
    }

    /// Undo a codec envelope into @p Out, which is the ONLY thing the two callers
    /// disagree about.
    ///
    /// Internal and `if constexpr`-shaped rather than a templated public API, which is
    /// the distinction that matters: a generic public `Unenvelope` had to
    /// range-construct its result, so the object path paid a second full allocation and
    /// memcpy of a multi-megabyte payload that `Compression::Decompress` had already
    /// sized exactly. Here each output type gets the one cheapest spelling -- the
    /// decompressed buffer is MOVED into a `std::vector<std::byte>`, and an `Identity`
    /// payload is copied straight out of the frame into whichever container asked for
    /// it -- while the guard above them exists exactly once, which is the whole reason
    /// this file exists.
    /// @tparam Out `std::vector<std::byte>` or `std::string`.
    /// @param field The enveloped field, exactly as it arrived.
    /// @param maxRawBytes The caller's own ceiling on the decompressed size.
    /// @return The original bytes as an @p Out, or why they could not be produced.
    template <typename Out>
    [[nodiscard]] std::expected<Out, EnvelopeError> OpenAs(std::span<std::byte const> field, std::size_t maxRawBytes)
    {
        auto const envelope = Wire::DecodeCodecEnvelope(field);
        if (!envelope.has_value())
            return std::unexpected(EnvelopeError::Malformed);

        // FIRST, before the codec is even looked up, and certainly before any byte is
        // decompressed. Everything below this line can allocate what the field says.
        if (envelope->rawLength > maxRawBytes)
            return std::unexpected(EnvelopeError::DeclaredTooLarge);

        // Copy bytes into whichever container this caller wanted, exactly once.
        auto const materialize = [](std::span<std::byte const> bytes) -> Out {
            if constexpr (std::same_as<Out, std::string>)
                return std::string { Wire::AsStringView(bytes) };
            else
                return Out(bytes.begin(), bytes.end());
        };

        if (envelope->codec == Wire::IdentityCodec)
        {
            if (envelope->rawLength != envelope->bytes.size())
                return std::unexpected(EnvelopeError::Malformed);
            // Straight out of the frame. An intermediate `std::vector<std::byte>` here
            // would be a second full copy of a preprocessed translation unit, on the
            // one codec a node actually negotiates.
            return materialize(envelope->bytes);
        }

        auto const codec = static_cast<CompressionCodec>(envelope->codec);
        if (!Compression::IsAvailable(codec))
            return std::unexpected(EnvelopeError::UnsupportedCodec);

        // NOT `auto const`. `Decompress` already produced a buffer of exactly the right
        // size, and moving it out is what keeps this path to one allocation end to end.
        // On a `const` object `*std::move(decoded)` is a `vector const&&`, which binds to
        // the COPY constructor with no diagnostic at all -- a move that reads as applied
        // and is not.
        auto decoded = Compression::Decompress(codec, envelope->bytes, envelope->rawLength);
        if (!decoded.has_value())
            return std::unexpected(EnvelopeError::Corrupt);
        if constexpr (std::same_as<Out, std::vector<std::byte>>)
            return std::move(*decoded);
        else
            return materialize(*decoded);
    }
} // namespace

Wire::CodecList AvailableCodecs()
{
    Wire::CodecList out;
    for (auto const codec: { CompressionCodec::Zstd, CompressionCodec::Lz4 })
        if (Compression::IsAvailable(codec))
            out.push_back(static_cast<std::uint8_t>(codec));
    out.push_back(Wire::IdentityCodec); // always, and always last
    return out;
}

std::vector<std::byte> Envelope(std::span<std::byte const> payload,
                                Wire::CodecList const& peerCodecs,
                                Wire::CodecList const& ownCodecs)
{
    auto const chosen = Wire::ChooseCodec(peerCodecs, ownCodecs);
    // `ownCodecs` is what this end SAID it can produce, which need not be what it
    // can: a worker's list reaches its caller through a registration and a grant, so
    // asking `Compression` itself is the only check that cannot be stale.
    if (chosen != Wire::IdentityCodec && Compression::IsAvailable(static_cast<CompressionCodec>(chosen)))
    {
        auto const compressed = Compression::Compress(static_cast<CompressionCodec>(chosen), payload, /*level=*/1);
        if (compressed.size() < payload.size())
            return Wire::EncodeCodecEnvelope(chosen, static_cast<std::uint32_t>(payload.size()), compressed);
    }
    return Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(payload.size()), payload);
}

Wire::ErrorCode WireCodeFor(EnvelopeError error) noexcept
{
    return RowFor(error).code;
}

std::string_view DescribeEnvelopeError(EnvelopeError error) noexcept
{
    return RowFor(error).text;
}

std::expected<std::vector<std::byte>, EnvelopeError> Unenvelope(std::span<std::byte const> field, std::size_t maxRawBytes)
{
    return OpenAs<std::vector<std::byte>>(field, maxRawBytes);
}

std::expected<std::string, EnvelopeError> UnenvelopeText(std::span<std::byte const> field, std::size_t maxRawBytes)
{
    return OpenAs<std::string>(field, maxRawBytes);
}

} // namespace FastCache::Cc
