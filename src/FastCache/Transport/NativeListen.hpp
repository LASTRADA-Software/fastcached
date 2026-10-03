// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/UdpSocket.hpp>
#include <core/platform/Types.hpp>

namespace core::net
{
class EventLoop;
} // namespace core::net

namespace FastCache
{

/// @file NativeListen.hpp
/// The one bind sequence this project keeps of its own, beside core-cpp's `core::net::listen`.
///
/// core-cpp owns every socket this project reads and writes (#1596), and every listener a loop
/// drives is `core::net::listen`, bound as `ClientListenOptions` says. What core-cpp does not
/// offer, and this file does, is a listener a thread BLOCKS on: `BlockingListener` for the admin
/// endpoints, and `AcceptRaw` for the Windows multi-reactor's accept thread. Both are candidates
/// for graduation into core-cpp; until then they are this file, and nothing else in this tree
/// binds.
///
/// **Exclusive**, exactly as core-cpp's listeners: `SO_EXCLUSIVEADDRUSE` on Windows, where
/// `SO_REUSEADDR` would let a second process take a port already being served, and `SO_REUSEADDR`
/// on POSIX, where it only steps over a dead socket's TIME_WAIT. A port several loops share is
/// `core::net::PortSharing::Shared`, which only `core::net::listen` binds.

/// The kernel send and receive buffers every client connection asks for, so a reply of up to this
/// size leaves in one write. A request: the kernel caps it at its own maximum.
inline constexpr std::size_t ClientSocketBufferBytes = std::size_t { 1 } << 20;

/// How the daemon binds a client listener on a loop, on every path: @p sharing decides whether
/// other loops' listeners may bind the same port, and every connection it accepts asks for
/// `ClientSocketBufferBytes` each way.
/// @param host The address to bind.
/// @param port The port; 0 lets the OS choose.
/// @param backlog The listen backlog.
/// @param sharing `Shared` for the POSIX multi-reactor's one listener per loop, `Exclusive`
///        otherwise. Windows refuses `Shared` (`core::net::NetErrorCode::Unsupported`).
/// @return The options to hand `core::net::listen`.
[[nodiscard]] core::net::ListenOptions ClientListenOptions(std::string_view host,
                                                           std::uint16_t port,
                                                           int backlog,
                                                           core::net::PortSharing sharing) noexcept;

/// A socket this file bound and set listening.
struct BoundSocket
{
    core::platform::NativeHandle handle { core::platform::InvalidHandle };
    int family { 0 };
};

/// Resolve @p host, then create, claim exclusively, bind and listen on the first candidate that
/// allows it. The socket blocks: it is for a thread that accepts, not for a loop.
/// @param resolver Resolves @p host (a literal or a name) to candidate addresses.
/// @param host The address to bind; `0.0.0.0`, `::` and names all work.
/// @param port The port; 0 lets the OS choose.
/// @param backlog The listen backlog.
/// @return The listening socket, owned by the caller, or why no candidate could be bound.
[[nodiscard]] std::expected<BoundSocket, std::string> BindAndListen(core::net::IAddressResolver& resolver,
                                                                    std::string_view host,
                                                                    std::uint16_t port,
                                                                    int backlog);

/// @return The port @p socket is bound to, or 0 when it cannot be asked.
[[nodiscard]] std::uint16_t BoundPortOf(core::platform::NativeHandle socket) noexcept;

/// @return The address @p socket is bound to as the KERNEL reports it -- "127.0.0.1", "::1",
///         "0.0.0.0" -- in `CanonicalAddressLiteral`'s spelling, or empty when it cannot be asked.
[[nodiscard]] std::string BoundAddressOf(core::platform::NativeHandle socket);

/// The canonical spelling of an IPv4 or IPv6 address literal -- the one `BoundAddressOf` reports,
/// so `::0001` and `::1` compare equal. Pure: nothing is looked up, so a host NAME is not a
/// literal and answers nothing.
/// @param text The text, unbracketed.
/// @return The canonical literal, or nothing when @p text is not an address literal.
[[nodiscard]] std::optional<std::string> CanonicalAddressLiteral(std::string_view text);

/// Close a socket this file handed out, ignoring the result.
void CloseNativeSocket(core::platform::NativeHandle socket) noexcept;

/// The options every socket this project accepts carries: close-on-exec (a spawned compiler must
/// not inherit a client connection), TCP_NODELAY, and 1 MiB send and receive buffers, so a large
/// reply leaves in one write.
/// @param handle An accepted socket.
void ApplyHotSocketOptions(core::platform::NativeHandle handle) noexcept;

/// A connection `AcceptRaw` took, not yet owned by any socket object.
struct AcceptedSocket
{
    core::platform::NativeHandle handle { core::platform::InvalidHandle }; ///< Owned by the caller.
    std::string peer;                                                      ///< The peer's printable address.
};

/// Accept one connection on a BLOCKING listening socket, and hand back its raw handle.
///
/// For a thread that accepts and HANDS the connection to a loop of another thread -- the Windows
/// multi-reactor, where the kernel cannot spread one port across several completion ports -- so
/// the connection must not be bound to a loop here. The handle already carries
/// `ApplyHotSocketOptions`, because `core::net::adoptSocket` leaves options alone.
///
/// **Closing @p listening from another thread is what ends a parked call on Windows, and it says
/// so**: a parked accept answers `Cancelled`, one called after the close `BadHandle`. POSIX does
/// not wake a parked `accept()` on close (measured by `scripts/probes/accept-close-wakeup.cpp`),
/// so there this has no caller.
/// @param listening A bound, listening, blocking socket.
/// @return The connection, or why none was taken.
[[nodiscard]] std::expected<AcceptedSocket, core::net::NetError> AcceptRaw(core::platform::NativeHandle listening);

/// A listener over a descriptor a supervisor handed this process (socket activation), driven by
/// @p loop.
///
/// **@p descriptor is this call's on every path.** `core::net::adoptListener` leaves a handle it
/// refused with its caller; this closes it then, so a caller that hands a descriptor over never
/// closes it itself and cannot close it twice.
/// @param loop The loop the listener belongs to.
/// @param descriptor An already-bound, already-listening descriptor; owned from here on.
/// @return The listener, or why the descriptor could not be served.
[[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, std::string> AdoptInheritedListener(
    core::net::EventLoop& loop, int descriptor);

/// A listener over a socket this process bound and set listening itself, driven by @p loop.
///
/// For a caller that must hold a port from the moment it is chosen until the moment it is
/// served: binding port 0, reading the port and closing the socket so a loop can bind it again
/// leaves the port free in between, and on a host running other processes it is an ephemeral
/// port something else may take. Handing the bound socket over closes that gap.
///
/// **@p handle is this call's on every path**, as `AdoptInheritedListener`'s descriptor is: the
/// listener closes it, and a handle `core::net::adoptListener` refuses is closed here.
/// @param loop The loop the listener belongs to.
/// @param handle A bound, listening socket; owned from here on.
/// @return The listener, or why the socket could not be served.
[[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, std::string> AdoptBoundListener(
    core::net::EventLoop& loop, core::platform::NativeHandle handle);

/// A TCP listener a thread BLOCKS on: `accept()` completes before it returns.
///
/// For a thread that owns no loop -- the admin endpoints, which serve one request at a time on a
/// thread of their own. Its sockets are core-cpp's `core::net::BlockingSocket`, so everything read
/// or written through one is core-cpp's socket contract; only the bind and the accept are here.
///
/// **A parked accept is ended by a poll, not by `close()`, on POSIX.** POSIX does not unblock an
/// `accept()` when another thread closes its socket; Windows does. So `SetTimeouts` arms a poll
/// before each accept, and a caller that stops a thread parked in `accept()` sets one and re-checks
/// its own flag on the `Timeout` it answers.
class BlockingListener final: public core::net::IListener
{
  public:
    /// Bind and listen.
    /// @param bindAddress The address to bind.
    /// @param port The port; 0 lets the OS choose.
    /// @param backlog The listen backlog.
    /// @param resolver Resolves @p bindAddress.
    /// @return A listener; `IsBound()` says whether the bind succeeded and `BindError()` why not.
    [[nodiscard]] static std::unique_ptr<BlockingListener> Bind(
        std::string_view bindAddress,
        std::uint16_t port,
        int backlog = 511,
        core::net::IAddressResolver& resolver = core::net::defaultAddressResolver());

    /// Take over a listening socket this process did not bind -- one inherited through socket
    /// activation.
    /// @param handle The listening socket; owned by the result.
    /// @return The listener.
    [[nodiscard]] static std::unique_ptr<BlockingListener> Adopt(core::platform::NativeHandle handle);

    BlockingListener(BlockingListener const&) = delete;
    BlockingListener(BlockingListener&&) = delete;
    BlockingListener& operator=(BlockingListener const&) = delete;
    BlockingListener& operator=(BlockingListener&&) = delete;
    ~BlockingListener() override;

    /// Accept one connection, blocking the calling thread until one arrives or the poll armed by
    /// `SetTimeouts` runs out (`core::net::NetErrorCode::Timeout`). Completes before it returns.
    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override;

    void close() noexcept override;

    [[nodiscard]] std::uint16_t boundPort() const noexcept override;

    /// @return The address this listener is bound to, as the kernel reports it; empty when it is
    ///         not bound. See `BoundAddressOf`.
    [[nodiscard]] std::string BoundAddress() const;

    /// @param acceptPoll How long one accept waits before answering `Timeout`; zero waits for ever.
    /// @param ioTimeout The send and receive timeout every accepted socket gets; zero for none.
    void SetTimeouts(std::chrono::milliseconds acceptPoll, std::chrono::milliseconds ioTimeout) noexcept;

    /// @return Whether the bind succeeded.
    [[nodiscard]] bool IsBound() const noexcept
    {
        return _handle != core::platform::InvalidHandle;
    }

    /// @return Why the bind failed, or empty.
    [[nodiscard]] std::string_view BindError() const noexcept
    {
        return _bindError;
    }

    /// Hand the listening socket out and own it no longer -- to `AdoptBoundListener`, so a
    /// loop serves the port this bound without it ever being released in between.
    /// @return The socket, or `InvalidHandle` when none is bound; the caller owns it.
    [[nodiscard]] core::platform::NativeHandle Release() noexcept
    {
        return std::exchange(_handle, core::platform::InvalidHandle);
    }

  private:
    BlockingListener() = default;

    /// The accept itself, answered synchronously.
    [[nodiscard]] core::net::AcceptResult AcceptNow();

    core::platform::NativeHandle _handle { core::platform::InvalidHandle };
    std::string _bindError;
    std::chrono::milliseconds _ioTimeout { 0 };
    std::chrono::milliseconds _acceptPoll { 0 };
};

} // namespace FastCache
