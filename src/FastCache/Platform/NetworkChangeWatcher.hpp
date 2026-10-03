// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/HostEvents.hpp>

#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
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

} // namespace FastCache
