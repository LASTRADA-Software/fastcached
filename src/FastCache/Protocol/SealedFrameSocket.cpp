// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/SealedFrameSocket.hpp>

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <string>
#include <utility>

namespace FastCache
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// How much one raw read asks for. A sealed connection carries the verbs a joining machine
    /// sends, which are small; this bounds a read, not a frame.
    constexpr std::size_t ScratchBytes = 16U * 1024U;

    /// One row of `SealFaults`.
    struct SealFaultRow
    {
        SealFault fault;      ///< The fault.
        std::string_view why; ///< What an operator reads.
    };

    constexpr EnumTable<SealFault, SealFaultRow> SealFaults { {
        { .fault = SealFault::BadTag,
          .why = "a frame's seal did not verify: it was injected, altered, replayed or reordered by something between "
                 "the two ends, or a frame before it was dropped" },
        { .fault = SealFault::Oversized,
          .why = "a sealed frame declared more payload than this end holds before it can check a seal" },
        { .fault = SealFault::Unframed, .why = "a sealed connection carried bytes that are not a frame of this protocol" },
    } };

    static_assert(RowsInEnumeratorOrder(SealFaults, &SealFaultRow::fault),
                  "SealFaults must hold one row per SealFault, in enumerator order");

    /// The payload length a header declares, when it is this protocol's.
    /// @param header Exactly the header's bytes.
    /// @param isRequest Whether it is a request header rather than a reply's.
    /// @return The length, or nothing for a header that does not decode.
    [[nodiscard]] std::optional<std::uint32_t> DeclaredLength(std::span<std::byte const> header, bool isRequest)
    {
        if (isRequest)
            return Wire::DecodeRequestHeader(header).transform([](Wire::RequestHeader const& h) { return h.payloadLength; });
        return Wire::DecodeReplyHeader(header).transform([](Wire::ReplyHeader const& h) { return h.payloadLength; });
    }

    /// @param end This end.
    /// @return Whether what it READS is a request.
    [[nodiscard]] constexpr bool ReadsRequests(SealedFrameEnd end) noexcept
    {
        return end == SealedFrameEnd::Server;
    }

    /// @param isRequest Whether the frame is a request.
    /// @return Its header's width.
    [[nodiscard]] constexpr std::size_t HeaderBytes(bool isRequest) noexcept
    {
        return isRequest ? Wire::RequestHeaderSize : Wire::ReplyHeaderSize;
    }
} // namespace

std::string_view DescribeSealFault(SealFault fault) noexcept
{
    return SealFaults[static_cast<std::size_t>(fault)].why;
}

SealedFrameSocket::SealedFrameSocket(std::unique_ptr<core::net::ISocket> raw, SealedFrameEnd end, std::size_t maxPayload):
    _raw { std::move(raw) },
    _end { end },
    _maxPayload { maxPayload }
{
}

void SealedFrameSocket::SealReceiving(SessionKey key)
{
    _opener.emplace(std::move(key));
    _inScratch.resize(ScratchBytes);
}

void SealedFrameSocket::SealSending(SessionKey key)
{
    _sealer.emplace(std::move(key));
}

bool SealedFrameSocket::Sealed() const noexcept
{
    return _opener.has_value() && _sealer.has_value();
}

std::optional<SealFault> SealedFrameSocket::Fault() const noexcept
{
    return _fault;
}

core::net::NetError SealedFrameSocket::FaultError() const
{
    return core::net::NetError { .code = core::net::NetErrorCode::ConnReset,
                                 .systemCode = 0,
                                 .context = std::string { DescribeSealFault(_fault.value_or(SealFault::BadTag)) } };
}

std::size_t SealedFrameSocket::ReleasedBytes() const noexcept
{
    return _released.size() - _releasedOffset;
}

std::size_t SealedFrameSocket::TakeReleased(std::span<std::byte> out)
{
    auto const n = std::min(out.size(), ReleasedBytes());
    std::ranges::copy(std::span<std::byte const> { _released }.subspan(_releasedOffset, n), out.begin());
    _releasedOffset += n;
    if (_releasedOffset == _released.size())
    {
        _released.clear();
        _releasedOffset = 0;
    }
    return n;
}

std::optional<SealFault> SealedFrameSocket::ReleaseWholeFrames()
{
    // Reached only once a proof has engaged the opener: `Read` passes bytes straight through before
    // that. Asked anyway, and answered CLOSED, because a frame no opener can check is one nothing
    // may hand out.
    if (!_opener.has_value())
        return SealFault::BadTag;
    auto& opener = *_opener;
    auto const isRequest = ReadsRequests(_end);
    auto const headerBytes = HeaderBytes(isRequest);
    auto consumed = std::size_t { 0 };
    auto fault = std::optional<SealFault> {};

    // A `while`: each pass consumes one whole frame of a length only its header states, which no
    // `for` head can express, and stops at the first frame not yet whole.
    while (!fault.has_value())
    {
        auto const rest = std::span<std::byte const> { _pending }.subspan(consumed);
        if (rest.size() < headerBytes)
            break;
        auto const declared = DeclaredLength(rest.first(headerBytes), isRequest);
        if (!declared.has_value())
        {
            fault = SealFault::Unframed;
            break;
        }
        if (*declared > _maxPayload)
        {
            fault = SealFault::Oversized;
            break;
        }
        auto const frameBytes = headerBytes + *declared;
        if (rest.size() < frameBytes + SessionTagBytes)
            break;

        auto tag = SessionTag {};
        std::ranges::copy(rest.subspan(frameBytes, SessionTagBytes), tag.begin());
        if (!opener.Open(rest.first(headerBytes), rest.subspan(headerBytes, *declared), tag))
        {
            fault = SealFault::BadTag;
            break;
        }
        _released.insert(_released.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(frameBytes));
        consumed += frameBytes + SessionTagBytes;
    }

    _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(consumed));
    return fault;
}

core::async::Task<core::net::IoResult> SealedFrameSocket::PumpRead(std::span<std::byte> out)
{
    // A `while`: each pass reads whatever the raw socket has, and the loop ends on a verified
    // byte, an error, a fault or EOF -- none of which a `for` head can state.
    while (true)
    {
        auto const got = co_await _raw->read(std::span<std::byte> { _inScratch });
        if (!got.has_value())
            co_return std::unexpected(got.error());
        if (*got == 0)
            // The peer has finished sending. A frame it left unfinished was never verified and is
            // never handed out; what the reader sees is the goodbye.
            co_return core::net::IoResult { std::size_t { 0 } };

        _pending.insert(_pending.end(), _inScratch.begin(), _inScratch.begin() + static_cast<std::ptrdiff_t>(*got));
        _fault = ReleaseWholeFrames();

        // Verified frames ahead of a fault are handed out first: they are the peer's own, and the
        // read after them meets the fault.
        if (auto const n = TakeReleased(out); n > 0)
            co_return core::net::IoResult { n };
        if (_fault.has_value())
            co_return std::unexpected(FaultError());
    }
}

core::net::IoAwaitable SealedFrameSocket::read(std::span<std::byte> buffer)
{
    core::net::contract::requireReadBuffer(buffer);
    if (!_opener.has_value())
        return _raw->read(buffer);

    if (auto const n = TakeReleased(buffer); n > 0)
        return core::net::IoAwaitable { core::net::IoResult { n } };
    if (_fault.has_value())
        return core::net::IoAwaitable { core::net::IoResult { std::unexpected(FaultError()) } };

    return core::net::IoAwaitable { PumpRead(buffer) };
}

core::net::IoResult SealedFrameSocket::SealWhatIsWhole(std::span<std::span<std::byte const> const> segments)
{
    auto written = std::size_t { 0 };
    for (auto const segment: segments)
    {
        _outPending.insert(_outPending.end(), segment.begin(), segment.end());
        written += segment.size();
    }

    // Reached only once a proof has engaged the sealer, and answered closed without one, for
    // `ReleaseWholeFrames`' reason: bytes this layer cannot seal are bytes it may not send.
    if (!_sealer.has_value())
        return std::unexpected(core::net::NetError { .code = core::net::NetErrorCode::ConnReset,
                                                     .systemCode = 0,
                                                     .context = "a sealing layer was asked to seal with no key agreed" });
    auto& sealer = *_sealer;
    auto const isRequest = !ReadsRequests(_end);
    auto const headerBytes = HeaderBytes(isRequest);
    auto consumed = std::size_t { 0 };
    // A `while` for `ReleaseWholeFrames`' reason: one whole frame per pass, of a length only its
    // header states.
    while (true)
    {
        auto const rest = std::span<std::byte const> { _outPending }.subspan(consumed);
        if (rest.size() < headerBytes)
            break;
        auto const declared = DeclaredLength(rest.first(headerBytes), isRequest);
        if (!declared.has_value())
            // This end's own writer framed something that is not a frame. Nothing after it can be
            // located, so nothing after it can be sealed; the connection is over.
            return std::unexpected(
                core::net::NetError { .code = core::net::NetErrorCode::ConnReset,
                                      .systemCode = 0,
                                      .context = std::string { DescribeSealFault(SealFault::Unframed) } });
        auto const frameBytes = headerBytes + *declared;
        if (rest.size() < frameBytes)
            break;
        auto const tag = sealer.Seal(rest.first(headerBytes), rest.subspan(headerBytes, *declared));
        _outReady.insert(_outReady.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(frameBytes));
        _outReady.insert(_outReady.end(), tag.begin(), tag.end());
        consumed += frameBytes;
    }
    _outPending.erase(_outPending.begin(), _outPending.begin() + static_cast<std::ptrdiff_t>(consumed));
    return core::net::IoResult { written };
}

core::async::Task<core::net::IoResult> SealedFrameSocket::PumpWrite(std::size_t reported)
{
    // A `while`: the raw socket may take a part of what is ready, and each pass sends the rest.
    while (!_outReady.empty())
    {
        auto const sent = co_await _raw->write(std::span<std::byte const> { _outReady });
        if (!sent.has_value())
            co_return std::unexpected(sent.error());
        if (*sent == 0)
            co_return std::unexpected(core::net::NetError {
                .code = core::net::NetErrorCode::ConnReset, .systemCode = 0, .context = "a sealed write sent nothing" });
        _outReady.erase(_outReady.begin(), _outReady.begin() + static_cast<std::ptrdiff_t>(*sent));
    }
    co_return core::net::IoResult { reported };
}

core::net::IoAwaitable SealedFrameSocket::StartWrite(std::size_t reported)
{
    if (_outReady.empty())
        // Only part of a frame so far: nothing can be sealed until its payload is whole, and the
        // caller has handed over every byte it wrote.
        return core::net::IoAwaitable { core::net::IoResult { reported } };

    return core::net::IoAwaitable { PumpWrite(reported) };
}

core::net::IoAwaitable SealedFrameSocket::write(std::span<std::byte const> buffer)
{
    if (!_sealer.has_value())
        return _raw->write(buffer);
    auto const segments = std::array { buffer };
    auto const taken = SealWhatIsWhole(segments);
    if (!taken.has_value())
        return core::net::IoAwaitable { taken };
    return StartWrite(*taken);
}

core::net::IoAwaitable SealedFrameSocket::writeVectored(std::span<std::span<std::byte const> const> segments,
                                                        std::shared_ptr<void const> keepAlive)
{
    if (!_sealer.has_value())
        return _raw->writeVectored(segments, std::move(keepAlive));
    // Copied synchronously into the frame being sealed, so neither the segments nor `keepAlive`
    // need outlive this call -- `TlsSocket`'s reason for the same.
    static_cast<void>(keepAlive);
    auto const taken = SealWhatIsWhole(segments);
    if (!taken.has_value())
        return core::net::IoAwaitable { taken };
    return StartWrite(*taken);
}

core::net::ResultAwaitable<void> SealedFrameSocket::handshakeIfNeeded()
{
    return _raw->handshakeIfNeeded();
}

core::net::IoAwaitable SealedFrameSocket::waitReadable()
{
    if (!_opener.has_value())
        return _raw->waitReadable();
    if (auto const available = ReleasedBytes(); available > 0)
        return core::net::IoAwaitable { core::net::IoResult { available } };
    if (_fault.has_value())
        return core::net::IoAwaitable { core::net::IoResult { std::unexpected(FaultError()) } };
    return _raw->waitReadable();
}

void SealedFrameSocket::cancelRead() noexcept
{
    _raw->cancelRead();
}

std::string SealedFrameSocket::peerAddress() const
{
    return _raw->peerAddress();
}

void SealedFrameSocket::close() noexcept
{
    _raw->close();
}

core::net::ResultAwaitable<void> SealedFrameSocket::shutdownWrite()
{
    return _raw->shutdownWrite();
}

void SealedFrameSocket::setReceiveDeadline(std::chrono::milliseconds deadline) noexcept
{
    _raw->setReceiveDeadline(deadline);
}

bool SealedFrameSocket::isClosed() const noexcept
{
    return _raw->isClosed();
}

} // namespace FastCache
