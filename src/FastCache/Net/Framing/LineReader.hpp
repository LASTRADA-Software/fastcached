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

    /// Drain (consume and discard) exactly `count` bytes from the stream
    /// without buffering them all at once. Used by protocols that need to
    /// step over a frame body — e.g. the memcached binary auth gate, which
    /// must skip past the body of a request from an unauthenticated client
    /// without buffering up to MaxBodyBytes (16 MiB) of attacker-supplied
    /// data on the daemon. Reads up to the internal chunk size at a time.
    /// @param count Number of bytes to skip.
    /// @return Task resolving to success or a ProtocolError.
    [[nodiscard]] Task<std::expected<void, ProtocolError>> Skip(std::size_t count);

    /// Prepend `bytes` to the internal buffer so subsequent reads see them
    /// first. Used by the protocol-autodetect layer to replay the bytes it
    /// peeked at the head of the stream.
    /// @param bytes Bytes to prepend.
    void PrimeWith(std::span<std::byte const> bytes);

    /// @return true if the underlying socket returned EOF and the buffer is drained.
    [[nodiscard]] bool Eof() const noexcept
    {
        return _eof && _buffer.size() == _consumed;
    }

    /// @return Read-only view over currently-buffered (unconsumed) bytes.
    [[nodiscard]] std::span<std::byte const> Buffered() const noexcept
    {
        return std::span<std::byte const> { _buffer.data() + _consumed, _buffer.size() - _consumed };
    }

  private:
    /// Try to extract a CRLF-delimited line from the unconsumed region of
    /// _buffer. On success the line bytes and the CRLF are consumed (the read
    /// cursor advances; no bytes are moved).
    [[nodiscard]] std::expected<std::string, ProtocolError> TryExtractLine();

    /// Pull one chunk from the socket into _buffer; sets _eof on EOF.
    /// @return Task resolving to bytes pulled, or ProtocolError on socket failure.
    [[nodiscard]] Task<std::expected<std::size_t, ProtocolError>> PullChunk();

    /// @return Count of unconsumed bytes available in _buffer.
    [[nodiscard]] std::size_t Available() const noexcept
    {
        return _buffer.size() - _consumed;
    }

    /// Drop the consumed prefix: if the read cursor has advanced, move the
    /// unconsumed tail to the front and reset the cursor. Called before a
    /// socket read so the buffer doesn't grow unboundedly, and amortised so
    /// the common "consume a whole line, buffer now empty" case is O(1).
    void Compact();

    ISocket& _socket;
    std::size_t _maxLineBytes;
    std::size_t _maxPayloadBytes;
    std::size_t _readChunkBytes;
    std::vector<std::byte> _buffer;
    /// Read cursor: index of the first unconsumed byte in _buffer. Consuming a
    /// line or payload advances this instead of erasing from the front, so a
    /// request does not memmove the buffer tail on every command.
    std::size_t _consumed { 0 };
    /// Reusable per-read scratch buffer. Allocated once (lazily, to chunk
    /// size) and reused for every PullChunk, so the request hot path does not
    /// allocate + zero-fill a fresh chunk on each socket read — that churn was
    /// ~40% of instructions on a tight get loop.
    std::vector<std::byte> _scratch;
    bool _eof { false };
};

} // namespace FastCache
