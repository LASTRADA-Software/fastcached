// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
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
/// core-cpp owns every socket this project reads and writes (#1596). What it does not have yet,
/// and this file does, is two ways of standing up a listener that `core::net::listen` does not
/// offer: a port several event loops share (SO_REUSEPORT, the daemon's multi-reactor path), and a
/// listener a thread BLOCKS on (`BlockingListener`, the admin endpoints). Both are candidates for
/// graduation into core-cpp; until then they are this file, and nothing else in this tree binds.
///
/// **Exclusive by default**, exactly as core-cpp's listeners: `SO_EXCLUSIVEADDRUSE` on Windows,
/// where `SO_REUSEADDR` would let a second process take a port already being served, and
/// `SO_REUSEADDR` on POSIX, where it only steps over a dead socket's TIME_WAIT. Sharing a port is
/// `ReusePort::Yes`, and only that.

/// Whether a listening socket shares its port with other listeners of this process.
enum class ReusePort : bool
{
    No,  ///< The port is this socket's alone.
    Yes, ///< SO_REUSEPORT: several listeners bind one port and the kernel spreads connections.
};

/// A socket this file bound and set listening.
struct BoundSocket
{
    core::platform::NativeHandle handle { core::platform::InvalidHandle };
    int family { 0 };
};

/// Resolve @p host, then create, claim, bind and listen on the first candidate that allows it.
/// @param resolver Resolves @p host (a literal or a name) to candidate addresses.
/// @param host The address to bind; `0.0.0.0`, `::` and names all work.
/// @param port The port; 0 lets the OS choose.
/// @param backlog The listen backlog.
/// @param reusePort Whether the port is shared with other listeners of this process.
/// @return The listening socket, owned by the caller, or why no candidate could be bound.
[[nodiscard]] std::expected<BoundSocket, std::string> BindAndListen(
    core::net::IAddressResolver& resolver, std::string_view host, std::uint16_t port, int backlog, ReusePort reusePort);

/// @return The port @p socket is bound to, or 0 when it cannot be asked.
[[nodiscard]] std::uint16_t BoundPortOf(core::platform::NativeHandle socket) noexcept;

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

/// A listener on a port other listeners of this process share, driven by @p loop.
///
/// The daemon's POSIX multi-reactor path binds one of these per reactor on the same port, and the
/// kernel spreads connections across them. `core::net::listen` in core-cpp v0.1.0 has no way to
/// ask for that, so the socket is bound here with `ReusePort::Yes` and handed to
/// `core::net::adoptListener`. **This function is the seam that goes**: core-cpp v0.1.1 adds
/// `ListenOptions::sharing`, and the body becomes one `core::net::listen` call when the pin moves.
/// @param loop The loop the listener belongs to.
/// @param host The address to bind.
/// @param port The port.
/// @param backlog The listen backlog.
/// @param resolver Resolves @p host.
/// @return The listener, or why it could not be made.
[[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, std::string> ListenOnSharedPort(
    core::net::EventLoop& loop,
    std::string_view host,
    std::uint16_t port,
    int backlog,
    core::net::IAddressResolver& resolver = core::net::defaultAddressResolver());

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
