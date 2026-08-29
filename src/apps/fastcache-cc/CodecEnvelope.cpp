// SPDX-License-Identifier: Apache-2.0
#include "CodecEnvelope.hpp"

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/EnumTable.hpp>

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
} // namespace

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
    auto const envelope = Wire::DecodeCodecEnvelope(field);
    if (!envelope.has_value())
        return std::unexpected(EnvelopeError::Malformed);

    // FIRST, before the codec is even looked up, and certainly before any byte is
    // decompressed. Everything below this line can allocate what the field says.
    if (envelope->rawLength > maxRawBytes)
        return std::unexpected(EnvelopeError::DeclaredTooLarge);

    if (envelope->codec == Wire::IdentityCodec)
    {
        if (envelope->rawLength != envelope->bytes.size())
            return std::unexpected(EnvelopeError::Malformed);
        return std::vector<std::byte> { envelope->bytes.begin(), envelope->bytes.end() };
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
    return std::move(*decoded);
}

} // namespace FastCache::Cc
