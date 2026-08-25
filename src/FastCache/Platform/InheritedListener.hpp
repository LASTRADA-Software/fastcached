// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IListener.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Socket activation: adopting listening sockets a supervisor already bound.
///
/// It lives in `Platform/` and not in `Net/`, where it used to sit, because what
/// it integrates with is a service supervisor rather than a network: it reads the
/// process environment, checks a pid, and applies close-on-exec. That it hands
/// back `IListener`s does not make it a network primitive -- `Platform/` depending
/// on `Net/` is the direction that was always intended, while the reverse put
/// `Net/`'s only reach into `Platform/Environment.hpp` across the boundary `Net/`
/// has to be severable along if it is ever lifted out of this tree.
///
/// A socket-activated service does not bind its own port. The supervisor binds
/// and listens first, then starts the service only when a connection arrives, and
/// hands the already-listening descriptors over. Two things follow that are worth
/// having: the port is reachable from boot even though nothing is running yet, so
/// a client never races the daemon's startup; and an idle service costs nothing at
/// all, which is the right shape for a compile worker, since misses on a warm
/// shared cache are bursty and rare.
///
/// systemd's protocol is the one implemented here, because it is the one the
/// packaged `.socket` unit uses. launchd has an equivalent
/// (`launch_activate_socket`) and Windows has no equivalent at all; both report
/// "nothing was handed over" and the caller binds for itself, which is the same
/// path a systemd machine takes when the unit is started directly.

/// The descriptors a supervisor handed this process.
struct ActivationHandoff
{
    /// First descriptor, or 0 when there was no handoff. Never a valid fd here,
    /// so a caller cannot mistake "none" for "descriptor zero".
    int firstDescriptor { 0 };
    /// How many consecutive descriptors follow, 0 when there was no handoff.
    int count { 0 };

    /// @return True when a supervisor handed over at least one listener.
    [[nodiscard]] constexpr bool Any() const noexcept
    {
        return count > 0;
    }
};

/// First descriptor systemd uses for a handoff, fixed by its protocol.
inline constexpr int ActivationFirstDescriptor = 3;

/// Interpret the socket-activation environment.
///
/// **Pure**, so every rule below is a unit test rather than something only a
/// systemd machine could exercise.
///
/// The `LISTEN_PID` check is the security-relevant one and is not optional.
/// systemd sets it to the pid it is starting; a process that inherited these
/// variables from a parent WITHOUT being the intended recipient would otherwise
/// adopt descriptors 3..3+n as listening sockets when they are whatever that
/// parent happened to leave open -- a log file, a database connection, the other
/// end of a pipe. Checking it is what makes "is fd 3 a listener?" answerable.
///
/// @param listenPid The `LISTEN_PID` value, if present.
/// @param listenFds The `LISTEN_FDS` value, if present.
/// @param currentPid This process's id.
/// @return What was handed over; `count == 0` when nothing was, for any reason.
[[nodiscard]] ActivationHandoff ParseSocketActivation(std::optional<std::string_view> listenPid,
                                                      std::optional<std::string_view> listenFds,
                                                      std::uint64_t currentPid) noexcept;

/// Adopt every listening socket a supervisor handed this process.
///
/// Reads the environment, adopts the descriptors, and then **clears the
/// variables**, which is not tidiness: this process goes on to spawn children --
/// a compile worker spawns a compiler for every job -- and a child that inherited
/// `LISTEN_FDS` would believe it too was socket-activated. Clearing them once,
/// here, is what keeps that from being every spawn site's problem.
///
/// The adopted descriptors are also marked close-on-exec. systemd deliberately
/// passes them without it, so without this every compiler the worker spawns
/// inherits the listening socket -- and the port then stays held by a stray child
/// after the worker exits, so a restart cannot bind and reports "address already
/// in use" with nothing visibly holding it.
///
/// The timeouts are PARAMETERS rather than the caller's job afterwards, and that
/// is a lesson rather than a preference. An accept loop on POSIX can only be
/// woken by a receive timeout on the listening socket -- closing it does not
/// unblock a parked `accept()` -- so a listener without one cannot be shut down
/// at all, and the way that presents is a `systemctl stop` that hangs until the
/// supervisor escalates to SIGKILL. That bug shipped once here already, on the
/// ordinary bind path; requiring the values to adopt a socket is what stops it
/// arriving a second time through this one.
///
/// @param acceptPoll How often a parked accept returns so a shutdown is noticed.
/// @param ioTimeout Receive/send timeout for each accepted connection.
/// @return One listener per handed-over descriptor, in the supervisor's order;
///         empty when there was no handoff, which is not an error.
[[nodiscard]] std::vector<std::unique_ptr<IListener>> AdoptInheritedListeners(std::chrono::milliseconds acceptPoll,
                                                                              std::chrono::milliseconds ioTimeout);

} // namespace FastCache
