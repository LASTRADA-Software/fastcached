// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <memory>
#include <type_traits>

namespace FastCache
{

/// Everything a connection is handed other than its socket.
///
/// **One type rather than four parameters, because the rule that governs the SET is a
/// compile-time one and a rule spread over a parameter list is a rule each new
/// parameter sits outside of**
/// ([#1051](https://github.com/LASTRADA-Software/fastcached/issues/1051)).
///
/// The rule: a connection's coroutine frame can be DESTROYED rather than resumed. A
/// reactor that stops with a chain parked on it frees that chain from its own
/// destructor (#1025), and a destructor body runs before the members declared beside
/// it -- so by the time such a frame unwinds, every local declared after the reactor
/// in `ReactorServerLoop`'s serving functions is already gone. The listeners, the
/// servers, the expiry pool, the reaper and the per-bind `TlsContext` are all in that
/// set.
///
/// **A destroyed coroutine frame runs no user code except destructors.** That is what
/// makes trivial destructibility the exact test here rather than a proxy for one: a
/// member that is only ever READ cannot be read by a frame nobody resumes, so a
/// pointer into a dead sibling is harmless in this specific window, while a member
/// whose destructor does anything at all is the one way the window can bite. So the
/// requirement is exactly *anything handed to a connection whose destructor does more
/// than nothing must outlive the reactor*, and the `static_assert` below is that
/// sentence with a reader.
///
/// The socket is the deliberate exemption and is a SEPARATE parameter so it cannot
/// hide inside the checked set. `~EpollSocket` / `~IocpSocket` call back into the
/// reactor, which is alive precisely because it is the object being destroyed.
///
/// What this does NOT cover, stated rather than left to be discovered: `Connection::Run`
/// builds its own locals -- a `SourceLogger`, a protocol handler, a `std::string` --
/// and holds them across suspension, and several have non-trivial destructors. They
/// are safe for a reason one level up rather than by this check: an object the
/// coroutine constructs itself can only own its own storage or reach what it was
/// GIVEN, and what it was given is this struct. Constraining the root set is therefore
/// what bounds the whole frame. A third constructor parameter would sit outside it,
/// which is why there are two and why widening the signature has to be a deliberate,
/// visible act.
struct ConnectionHoldings
{
    CacheEngine& engine; ///< Shared cache engine; outlives every connection.
    ILogger& logger;     ///< Shared logger; outlives every connection.
    /// Per-server session context (auth policy, pinned reactor, metrics sink).
    ///
    /// Copied by value into the frame, which is what puts it under the rule above: a
    /// field added here with a non-trivial destructor is one the frame destroys, at
    /// the one moment its neighbours are already gone.
    SessionContext session {};
    /// When `Yes`, this connection's log lines are prefixed with the client IP.
    LogSource logSource { LogSource::No };
};

/// The guard, and the reason there is no argument to pass a bare collaborator to.
///
/// Planting a member with a non-trivial destructor in `ConnectionHoldings` -- or in
/// `SessionContext`, which it carries by value -- fails the build here rather than
/// producing a use-after-free during teardown that nothing would report. A member
/// with a trivial destructor in the same position is not refused, which is the other
/// direction and the one that keeps this from firing only when nothing is wrong.
static_assert(std::is_trivially_destructible_v<ConnectionHoldings>,
              "Everything handed to a connection must be destructible without touching anything, because an "
              "abandoned connection frame is destroyed AFTER the reactor's siblings are gone (#1051). Give the new "
              "collaborator a trivial destructor, or make the reactor outlive it and record why here.");

/// One client connection. Owns the socket, detects the protocol, hands off
/// to the matching protocol handler, and ends when that handler returns.
///
/// The Connection itself does not own a per-connection BufferPool yet —
/// allocations go through the global allocator. A later pass can add the
/// pool without changing this interface.
class Connection
{
  public:
    /// Construct over a freshly-accepted socket.
    /// @param socket Owned socket; closed on connection end. The one collaborator whose
    ///        destructor does something, exempted with its reason on `ConnectionHoldings`.
    /// @param held Everything else this connection is given, as one checked bundle.
    Connection(std::unique_ptr<ISocket> socket, ConnectionHoldings held) noexcept;

    /// Run the connection's protocol loop to completion.
    /// @return Task that resolves when the connection closes.
    [[nodiscard]] Task<void> Run();

  private:
    std::unique_ptr<ISocket> _socket;
    ConnectionHoldings _held;
};

} // namespace FastCache
