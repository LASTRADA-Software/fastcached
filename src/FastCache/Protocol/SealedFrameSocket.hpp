// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Which end of a `0xFC` connection a sealing socket sits at, which decides the header shape of
/// what it reads and of what it writes: a server reads requests and writes replies, a caller the
/// reverse.
///
/// **PRIVATE: persisted and transmitted nowhere.**
enum class SealedFrameEnd : std::uint8_t
{
    Server, ///< Reads request frames, writes reply frames.
    Caller, ///< Writes request frames, reads reply frames.
};

/// Why a sealed connection stopped reading.
///
/// **PRIVATE: persisted and transmitted nowhere.** Three answers rather than one, because only the
/// first is the event the seal exists to catch; the other two are a peer that is not speaking this
/// grammar at all, which an operator reads differently.
enum class SealFault : std::uint8_t
{
    BadTag,    ///< A frame's tag did not verify: injected, altered, replayed, reordered or dropped.
    Oversized, ///< A frame declared more payload than this end will hold before it can check a tag.
    Unframed,  ///< A header that is not this protocol's: nothing after it can be located.
    Last,      ///< Not a fault, and has no row: the length of a table keyed by one.
};

/// @param fault A fault.
/// @return What an operator reads about it.
[[nodiscard]] std::string_view DescribeSealFault(SealFault fault) noexcept;

/// An `ISocket` that seals every `0xFC` frame it writes and checks every one it reads, once a
/// connection has proved its identity (#178).
///
/// ## Why a decorator
///
/// Every frame after `ProveNode`, both ways, carries a `SealedFrameTagBytes` tag after its payload
/// under a per-direction session key (`Distributed::NodeProof`). The endpoint has five sanctioned
/// writers and one reader, and the client exchanges through `Cc::Exchange`; a seal applied at each
/// of those would be a second code path per writer, and the one that forgot would put an unsealed
/// frame on a sealed connection. So the socket seals, and nothing above it changes: a connection
/// is wrapped when it opens, passes bytes straight through until a proof engages it, and from then
/// on hands its reader only frames whose tag has verified.
///
/// ## Engaged per direction
///
/// `SealReceiving` and `SealSending` are separate because the two ends switch at different points:
/// the server engages both after it verifies the proof and then writes the `Ok` sealed, while the
/// caller must READ that `Ok` sealed although it wrote `ProveNode` in the clear. A caller therefore
/// engages receiving before it sends the proof, and sending once the `Ok` arrives.
///
/// ## What a reader is handed
///
/// Header and payload, exactly the bytes an unsealed peer would have sent -- the tag is consumed
/// here -- and nothing of a frame until the WHOLE frame has verified. A frame whose declared payload
/// exceeds `maxPayload` is a fault rather than a refusal: it cannot be verified without being held,
/// and a proven peer that sends one is not running this build's caps. A fault ends the stream: the
/// read that meets it, and every read after it, fails, and the owner closes the connection -- never
/// answers it, which is what makes an injected verb worth nothing.
///
/// ## Contracts it keeps
///
/// `Read`'s buffer must be non-empty (`Detail::RequireReadBuffer`, first statement). `Read` and
/// `WaitReadable` share the raw socket's one read operation, as every transport's do, and
/// `CancelRead` retires a parked read by forwarding, exactly as `TlsSocket`'s does. At most one
/// read and one write may be in flight at once, which the endpoint's one-reader and one-writer
/// rules already guarantee.
class SealedFrameSocket final: public ISocket
{
  public:
    /// @param raw The connection; owned.
    /// @param end Which end this is, which decides the header shape read and written.
    /// @param maxPayload The largest payload a sealed frame read here may declare.
    SealedFrameSocket(std::unique_ptr<ISocket> raw, SealedFrameEnd end, std::size_t maxPayload);

    SealedFrameSocket(SealedFrameSocket const&) = delete;
    SealedFrameSocket(SealedFrameSocket&&) = delete;
    SealedFrameSocket& operator=(SealedFrameSocket const&) = delete;
    SealedFrameSocket& operator=(SealedFrameSocket&&) = delete;
    ~SealedFrameSocket() override = default;

    /// Check every frame read from now on under @p key. Call with no read in flight.
    /// @param key The key the PEER seals under.
    void SealReceiving(SessionKey key);

    /// Seal every frame written from now on under @p key. Call with no write in flight.
    /// @param key The key this end seals under.
    void SealSending(SessionKey key);

    /// @return Whether both directions are sealed.
    [[nodiscard]] bool Sealed() const noexcept;

    /// @return Why reading stopped, or nothing while it has not.
    [[nodiscard]] std::optional<SealFault> Fault() const noexcept;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;
    [[nodiscard]] Task<std::expected<void, NetError>> HandshakeIfNeeded() override;

    /// @copydoc ISocket::WaitReadable
    ///
    /// Verified bytes not yet handed out answer at once; otherwise the raw socket answers. The tag
    /// changes nothing about how a peer says goodbye -- its FIN is the raw EOF -- so delegating
    /// does not reopen #712's TLS problem.
    [[nodiscard]] IoAwaitable WaitReadable() override;

    /// @copydoc ISocket::CancelRead
    ///
    /// Forwarded, which retires a parked pump too: its raw read completes `Cancelled`, and the
    /// pump hands that to whoever awaited this socket.
    void CancelRead() noexcept override;

    [[nodiscard]] std::string PeerAddress() const override;
    void Close() noexcept override;
    void ShutdownWrite() noexcept override;
    void SetReceiveDeadline(std::chrono::milliseconds deadline) noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept override;

  private:
    /// Move every whole, verified frame out of `_pending` into `_released`.
    /// @return The fault that stopped it, or nothing.
    [[nodiscard]] std::optional<SealFault> ReleaseWholeFrames();

    /// Copy verified bytes into @p out.
    /// @param out Where to.
    /// @return How many.
    [[nodiscard]] std::size_t TakeReleased(std::span<std::byte> out);

    /// @return How many verified bytes wait to be handed out.
    [[nodiscard]] std::size_t ReleasedBytes() const noexcept;

    /// Append @p segments to what this end has written, and seal every frame now whole.
    /// @param segments What the caller wrote.
    /// @return How many bytes that was, or the error when a header this end wrote is not a frame.
    [[nodiscard]] IoResult SealWhatIsWhole(std::span<std::span<std::byte const> const> segments);

    /// The error every read reports once a fault has stopped the stream.
    /// @return It.
    [[nodiscard]] NetError FaultError() const;

    Task<IoResult> PumpRead(std::span<std::byte> out);
    Task<IoResult> PumpWrite(std::size_t reported);
    DetachedTask DriveRead(IoAwaitable* awaitable, std::span<std::byte> out);
    DetachedTask DriveWrite(IoAwaitable* awaitable, std::size_t reported);

    /// Start a pump for a sealed write of @p reported caller bytes whose frames wait in `_outReady`.
    /// @param reported What the caller is told was written.
    /// @return The awaitable.
    [[nodiscard]] IoAwaitable StartWrite(std::size_t reported);

    std::unique_ptr<ISocket> _raw;
    SealedFrameEnd _end;
    std::size_t _maxPayload;

    std::optional<FrameOpener> _opener;
    std::optional<FrameSealer> _sealer;

    std::vector<std::byte> _inScratch; ///< One raw read's worth, allocated when receiving is sealed.
    std::vector<std::byte> _pending;   ///< Raw bytes read and not yet a whole frame.
    std::vector<std::byte> _released;  ///< Verified header and payload bytes not yet handed out.
    std::size_t _releasedOffset { 0 }; ///< How much of `_released` has been handed out.
    std::optional<SealFault> _fault;   ///< Why reading stopped.
    std::span<std::byte> _readView;    ///< The parked read's destination.

    std::vector<std::byte> _outPending; ///< Bytes written that do not yet make a whole frame.
    std::vector<std::byte> _outReady;   ///< Sealed frames not yet on the wire.
    std::size_t _writeReported { 0 };   ///< What the parked write reports.
};

} // namespace FastCache
