// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Buffered byte reader on top of an ISocket. Supplies both line-delimited
/// (`ReadLine`) and length-prefixed (`ReadExactly`) reads while sharing a
/// single internal buffer — so bytes that landed in the same TCP segment as
/// a header line can be consumed by the subsequent body read without an
/// extra socket round-trip.
///
/// Used by every protocol handler:
///   - memcached text: ReadLine for the command, ReadExactly for the value
///   - memcached binary: ReadExactly for the 24-byte header + extras/key/value
///   - Redis RESP: ReadLine for `*N\r\n` / `$M\r\n`, ReadExactly for bulk strings
class ByteReader
{
  public:
    using LineResult = std::expected<std::string, ProtocolError>;
    using BytesResult = std::expected<std::vector<std::byte>, ProtocolError>;

    /// Construct over the given socket.
    /// @param socket Source socket; lifetime must exceed this reader.
    /// @param maxLineBytes Hard cap on a single line's length (CRLF stripped).
    /// @param maxPayloadBytes Hard cap on a single length-prefixed payload.
    /// @param readChunkBytes Size of each underlying socket read.
    ByteReader(ISocket& socket,
               std::size_t maxLineBytes,
               std::size_t maxPayloadBytes,
               std::size_t readChunkBytes = 4096) noexcept;

    /// Read the next CRLF-delimited line; returns the line without the CRLF.
    /// @return Task resolving to the line, or a ProtocolError.
    [[nodiscard]] Task<LineResult> ReadLine();

    /// Read exactly `count` bytes from the stream.
    /// @param count Number of bytes to read.
    /// @return Task resolving to the byte vector or a ProtocolError.
    [[nodiscard]] Task<BytesResult> ReadExactly(std::size_t count);

    /// Prepend `bytes` to the internal buffer so subsequent reads see them
    /// first. Used by the protocol-autodetect layer to replay the bytes it
    /// peeked at the head of the stream.
    /// @param bytes Bytes to prepend.
    void PrimeWith(std::span<std::byte const> bytes);

    /// @return true if the underlying socket returned EOF and the buffer is drained.
    [[nodiscard]] bool Eof() const noexcept { return _eof && _buffer.empty(); }

    /// @return Read-only view over currently-buffered bytes.
    [[nodiscard]] std::span<std::byte const> Buffered() const noexcept
    {
        return std::span<std::byte const> { _buffer.data(), _buffer.size() };
    }

  private:
    /// Try to extract a CRLF-delimited line from _buffer. On success the
    /// line bytes and the CRLF are removed from _buffer.
    [[nodiscard]] std::expected<std::string, ProtocolError> TryExtractLine();

    /// Pull one chunk from the socket into _buffer; sets _eof on EOF.
    /// @return Task resolving to bytes pulled, or ProtocolError on socket failure.
    [[nodiscard]] Task<std::expected<std::size_t, ProtocolError>> PullChunk();

    ISocket& _socket;
    std::size_t _maxLineBytes;
    std::size_t _maxPayloadBytes;
    std::size_t _readChunkBytes;
    std::vector<std::byte> _buffer;
    bool _eof { false };
};

} // namespace FastCache
