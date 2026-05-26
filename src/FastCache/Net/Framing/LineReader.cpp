// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/Framing/LineReader.hpp>

#include <algorithm>
#include <cstddef>
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

std::expected<std::string, ProtocolError> ByteReader::TryExtractLine()
{
    if (_buffer.size() < 2)
        return std::unexpected(ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "no CRLF yet" });

    for (std::size_t i = 0; i + 1 < _buffer.size(); ++i)
    {
        if (_buffer[i] == CarriageReturn && _buffer[i + 1] == LineFeed)
        {
            if (i > _maxLineBytes)
                return std::unexpected(MakeLineTooLong());
            std::string line;
            line.reserve(i);
            for (std::size_t j = 0; j < i; ++j)
                line.push_back(static_cast<char>(_buffer[j]));
            _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(i + 2));
            return line;
        }
    }
    return std::unexpected(ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "no CRLF yet" });
}

Task<std::expected<std::size_t, ProtocolError>> ByteReader::PullChunk()
{
    std::vector<std::byte> chunk(_readChunkBytes);
    auto const result = co_await _socket.Read(std::span<std::byte> { chunk.data(), chunk.size() });
    if (!result.has_value())
        co_return std::unexpected(MakeTruncated("socket read failed"));

    auto const got = *result;
    if (got == 0)
    {
        _eof = true;
        co_return std::size_t { 0 };
    }
    chunk.resize(got);
    _buffer.insert(_buffer.end(), chunk.begin(), chunk.end());
    co_return got;
}

Task<ByteReader::LineResult> ByteReader::ReadLine()
{
    if (auto existing = TryExtractLine(); existing.has_value())
        co_return std::move(*existing);

    while (true)
    {
        if (_buffer.size() > _maxLineBytes)
            co_return std::unexpected(MakeLineTooLong());

        auto const pulled = co_await PullChunk();
        if (!pulled.has_value())
            co_return std::unexpected(pulled.error());

        if (*pulled == 0)
        {
            if (_buffer.empty())
                co_return std::unexpected(MakeTruncated("clean EOF before line"));
            co_return std::unexpected(MakeTruncated("EOF mid-line"));
        }

        if (auto extracted = TryExtractLine(); extracted.has_value())
            co_return std::move(*extracted);

        if (_buffer.size() > _maxLineBytes)
            co_return std::unexpected(MakeLineTooLong());
    }
}

void ByteReader::PrimeWith(std::span<std::byte const> bytes)
{
    if (bytes.empty())
        return;
    _buffer.insert(_buffer.begin(), bytes.begin(), bytes.end());
}

Task<ByteReader::BytesResult> ByteReader::ReadExactly(std::size_t count)
{
    if (count > _maxPayloadBytes)
        co_return std::unexpected(MakePayloadTooLarge(count, _maxPayloadBytes));

    while (_buffer.size() < count)
    {
        auto const pulled = co_await PullChunk();
        if (!pulled.has_value())
            co_return std::unexpected(pulled.error());
        if (*pulled == 0)
            co_return std::unexpected(MakeTruncated("EOF before payload satisfied"));
    }

    std::vector<std::byte> out;
    out.reserve(count);
    out.insert(out.end(), _buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(count));
    _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(count));
    co_return out;
}

} // namespace FastCache
