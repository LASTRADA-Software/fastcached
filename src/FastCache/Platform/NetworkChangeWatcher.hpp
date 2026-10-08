// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/HostEvents.hpp>

#include <condition_variable>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

#include <core/platform/Clock.hpp>

namespace FastCache
{

/// The network watcher's portable half: raw notifications in, one debounced
/// `HostEvent::NetworkChanged` out, delivered on a thread of its own.
///
/// **Every decision reads the injected clock** -- when a burst began, when it went quiet, when it
/// is due -- and is `NetworkChangeDebouncer`'s. What real time decides is only how long the thread
/// sleeps before it looks again (`due - now`, on that clock), so a `ManualClock` holds a burst back
/// for as long as it stands still.
///
/// **The sink is called with no lock held**, so it may call `Notify` from inside `OnHostEvent`.
/// **It must not throw**: the relay's thread has nobody to hand an exception to, so one escaping
/// the sink ends the process. The relay does not catch it, because it has nowhere to report it
/// and swallowing it would hide a consumer's defect behind a watcher that silently goes on.
/// Destroying the relay stops its thread whatever it is waiting for and returns once a delivery in
/// flight has finished, so no delivery reaches the sink after that -- and it must therefore not be
/// destroyed from inside its own sink's `OnHostEvent`, which would join the thread it runs on.
class NetworkChangeRelay
{
  public:
    /// @param sink Where the debounced event goes; must outlive the relay.
    /// @param clock What every instant is read from; must outlive the relay.
    /// @param bound The debounce.
    NetworkChangeRelay(IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound);
    ~NetworkChangeRelay();
    NetworkChangeRelay(NetworkChangeRelay const&) = delete;
    NetworkChangeRelay(NetworkChangeRelay&&) = delete;
    NetworkChangeRelay& operator=(NetworkChangeRelay const&) = delete;
    NetworkChangeRelay& operator=(NetworkChangeRelay&&) = delete;

    /// A raw notification arrived. Any thread, an OS callback's included: it records the instant
    /// and wakes the relay's thread, and never calls the sink itself.
    void Notify() noexcept;

  private:
    /// The relay's thread: waits for a burst, then for it to be due, then delivers it.
    /// @param stop Requested by the destructor.
    void Run(std::stop_token const& stop);

    IHostEventSink& _sink;
    core::platform::IClock const& _clock;
    std::mutex _mutex;
    std::condition_variable_any _changed;
    NetworkChangeDebouncer _debouncer;
    /// LAST, so every member it reads is built before it starts and freed after it joins.
    std::jthread _thread;
};

/// Watches this machine's interfaces and addresses while it lives, and stops when destroyed.
class NetworkChangeWatcher
{
  public:
    NetworkChangeWatcher() = default;
    NetworkChangeWatcher(NetworkChangeWatcher const&) = delete;
    NetworkChangeWatcher(NetworkChangeWatcher&&) = delete;
    NetworkChangeWatcher& operator=(NetworkChangeWatcher const&) = delete;
    NetworkChangeWatcher& operator=(NetworkChangeWatcher&&) = delete;
    virtual ~NetworkChangeWatcher() = default;
};

/// Start delivering debounced `HostEvent::NetworkChanged` into @p sink.
///
/// Windows: `NotifyIpInterfaceChange` and `NotifyUnicastIpAddressChange`, into one
/// `NetworkChangeRelay` -- a VPN can hand out a new address without an interface event. The OS
/// callback only notifies the relay, so the watcher is never cancelled from inside it.
///
/// Linux: a `NETLINK_ROUTE` socket subscribed to the link, IPv4/IPv6 address and IPv4/IPv6 route
/// groups. macOS: a `PF_ROUTE` socket. Either is read by a thread of the watcher's own, through
/// `WatchNetworkChangeDescriptor`. **A thread does not survive `fork()`**, so a POSIX caller that
/// daemonizes starts the watcher in the process that goes on to serve, after the fork. Other POSIX
/// platforms offer nothing this project watches.
///
/// Three answers, and a caller tells them apart: a watcher; NO watcher, where this platform offers
/// nothing this project watches, which is not an error; and a refusal, where the OS would not
/// register one, which is.
/// @param sink Where the event goes; must outlive the watcher.
/// @param clock What the debouncer's instants are read from; must outlive the watcher.
/// @param bound The debounce.
/// @return The running watcher, or null where the platform offers nothing; or which registration
///         the OS refused, and why.
[[nodiscard]] std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> StartNetworkChangeWatcher(
    IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound);

#if !defined(_WIN32)

/// Which messages read from a change socket report a change: `NetlinkReportsChange` or
/// `RouteSocketReportsChange` (`NetworkChangeMessages.hpp`).
using NetworkChangeClassifier = bool (*)(std::span<std::byte const> buffer) noexcept;

/// Watch an already-open change socket: a thread of the watcher's own polls it, reads what arrives
/// and notifies one `NetworkChangeRelay` whenever @p classify says a read reports a change -- or
/// when the kernel says notifications were dropped (`ENOBUFS`), since a lost change is still one.
/// The thread stops at end of file or a read error, and always when the watcher is destroyed.
///
/// The seam `StartNetworkChangeWatcher` opens the OS socket in front of, so the read loop is driven
/// by a test through a pipe rather than by changing this machine's addresses.
/// @param descriptor The socket (or any readable descriptor). Owned from here on: closed by the
///                   watcher, or before returning when no watcher is made.
/// @param classify Reads one buffer.
/// @param sink Where the event goes; must outlive the watcher.
/// @param clock What the debouncer's instants are read from; must outlive the watcher.
/// @param bound The debounce.
/// @return The running watcher; or why its stop pipe could not be made.
[[nodiscard]] std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> WatchNetworkChangeDescriptor(
    int descriptor,
    NetworkChangeClassifier classify,
    IHostEventSink& sink,
    core::platform::IClock const& clock,
    NetworkDebounce bound);

#endif

} // namespace FastCache
