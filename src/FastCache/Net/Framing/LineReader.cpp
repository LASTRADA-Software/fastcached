// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>

#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace FastCache
{

namespace
{

    constexpr std::byte CarriageReturn { 0x0d };
    constexpr std::byte LineFeed { 0x0a };

    [[nodiscard]] ProtocolError MakeTruncated(std::string context)
    {
        return ProtocolError { .code = ProtocolErrorCode::Truncated, .context = std::move(context) };
    }

    [[nodiscard]] ProtocolError MakeLineTooLong()
    {
        return ProtocolError { .code = ProtocolErrorCode::LineTooLong, .context = "line exceeds cap" };
    }

    [[nodiscard]] ProtocolError MakePayloadTooLarge(std::size_t requested, std::size_t cap)
    {
        return ProtocolError { .code = ProtocolErrorCode::PayloadTooLarge,
                               .context = std::format("requested {} bytes, cap is {}", requested, cap) };
    }

} // namespace

ByteReader::ByteReader(ISocket& socket,
                       std::size_t maxLineBytes,
                       std::size_t maxPayloadBytes,
                       std::size_t readChunkBytes) noexcept:
    _socket { socket },
    _maxLineBytes { maxLineBytes },
    _maxPayloadBytes { maxPayloadBytes },
    _readChunkBytes { readChunkBytes }
{
}

void ByteReader::Compact()
{
    if (_consumed == 0)
        return;
    auto const remaining = Available();
    if (remaining != 0)
        std::memmove(_buffer.data(), _buffer.data() + _consumed, remaining);
    _buffer.resize(remaining);
    _consumed = 0;
}

std::expected<std::string, ProtocolError> ByteReader::TryExtractLine()
{
    FC_ZONE_SCOPED_N("LineReader.TryExtractLine");
    auto const* const base = _buffer.data() + _consumed;
    auto const avail = Available();
    if (avail < 2)
        return std::unexpected(ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "no CRLF yet" });

    for (std::size_t i = 0; i + 1 < avail; ++i)
    {
        if (base[i] == CarriageReturn && base[i + 1] == LineFeed)
        {
            if (i > _maxLineBytes)
                return std::unexpected(MakeLineTooLong());
            // Bulk-copy the line bytes (no per-byte loop), then consume the
            // line + its CRLF by advancing the read cursor (no buffer memmove).
            std::string line { reinterpret_cast<char const*>(base), i };
            _consumed += i + 2;
            return line;
        }
    }
    return std::unexpected(ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "no CRLF yet" });
}

Task<std::expected<std::size_t, ProtocolError>> ByteReader::PullChunk()
{
    // Reuse a persistent scratch buffer across reads. Sizing it with resize()
    // only grows the allocation once (and the one-time zero-fill is irrelevant
    // — we overwrite exactly `got` bytes from the socket and copy only those);
    // a fresh `std::vector<std::byte>(_readChunkBytes)` per call would malloc +
    // zero-fill + free on every request, which dominated the get hot path.
    // Drop already-consumed bytes before growing the buffer, so a long-lived
    // connection's buffer stays bounded by the in-flight (unconsumed) data.
    Compact();
    if (_scratch.size() < _readChunkBytes)
        _scratch.resize(_readChunkBytes);
    auto const result = co_await _socket.Read(std::span<std::byte> { _scratch.data(), _readChunkBytes });
    if (!result.has_value())
        co_return std::unexpected(MakeTruncated("socket read failed"));

    auto const got = *result;
    if (got == 0)
    {
        _eof = true;
        co_return std::size_t { 0 };
    }
    _buffer.insert(_buffer.end(), _scratch.begin(), _scratch.begin() + static_cast<std::ptrdiff_t>(got));
    co_return got;
}

Task<ByteReader::LineResult> ByteReader::ReadLine()
{
    if (auto existing = TryExtractLine(); existing.has_value())
        co_return std::move(*existing);

    while (true)
    {
        if (Available() > _maxLineBytes)
            co_return std::unexpected(MakeLineTooLong());

        auto const pulled = co_await PullChunk();
        if (!pulled.has_value())
            co_return std::unexpected(pulled.error());

        if (*pulled == 0)
        {
            if (Available() == 0)
                co_return std::unexpected(MakeTruncated("clean EOF before line"));
            co_return std::unexpected(MakeTruncated("EOF mid-line"));
        }

        if (auto extracted = TryExtractLine(); extracted.has_value())
            co_return std::move(*extracted);

        if (Available() > _maxLineBytes)
            co_return std::unexpected(MakeLineTooLong());
    }
}

void ByteReader::PrimeWith(std::span<std::byte const> bytes)
{
    if (bytes.empty())
        return;
    // Drop the consumed prefix first so "insert at front" prepends ahead of
    // only the unconsumed data (and the cursor stays at 0).
    Compact();
    _buffer.insert(_buffer.begin(), bytes.begin(), bytes.end());
}

Task<ByteReader::BytesResult> ByteReader::ReadExactly(std::size_t count)
{
    if (count > _maxPayloadBytes)
        co_return std::unexpected(MakePayloadTooLarge(count, _maxPayloadBytes));

    while (Available() < count)
    {
        auto const pulled = co_await PullChunk();
        if (!pulled.has_value())
            co_return std::unexpected(pulled.error());
        if (*pulled == 0)
            co_return std::unexpected(MakeTruncated("EOF before payload satisfied"));
    }

    auto const* const base = _buffer.data() + _consumed;
    std::vector<std::byte> out { base, base + count };
    _consumed += count; // consume via the cursor; no buffer memmove
    co_return out;
}

} // namespace FastCache
