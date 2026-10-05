// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache
{

/// Something the host told this process about itself.
///
/// **Every event is a HINT that lets a consumer act sooner, never the only way it acts.** A
/// Modern Standby machine can sleep and wake without reporting either, a node run in the
/// foreground has no service control handler and hears no power event at all, and a resume can
/// be reported twice (`PBT_APMRESUMEAUTOMATIC`, then `PBT_APMRESUMESUSPEND` once a user is
/// present). So a consumer recovers on its own schedule whatever it hears here, and treats a
/// repeated event as the same event.
///
/// **Private**: never transmitted and never persisted, so its enumerators carry no values.
enum class HostEvent : std::uint8_t
{
    Suspending,     ///< The machine is about to sleep; what must be said before it does is said now.
    Resumed,        ///< The machine woke up.
    NetworkChanged, ///< An interface or an address came or went; debounced.
    Last
};

/// Where a host event is delivered.
class IHostEventSink
{
  public:
    IHostEventSink() = default;
    IHostEventSink(IHostEventSink const&) = delete;
    IHostEventSink(IHostEventSink&&) = delete;
    IHostEventSink& operator=(IHostEventSink const&) = delete;
    IHostEventSink& operator=(IHostEventSink&&) = delete;
    virtual ~IHostEventSink() = default;

    /// Called on the thread the host delivered on -- the SCM's control handler for a power
    /// event, the network watcher's own thread for a network change. A sink that blocks holds
    /// that thread. **A sink must not throw**: neither of those threads has a caller to throw to, so an
    /// exception escaping here ends the process. **A sink returns promptly** and hands long work --
    /// network I/O above all -- to a thread of its own: the delivery turn is held for the whole
    /// call, and the SCM handler is also the one thread serving STOP and SHUTDOWN. The single
    /// blocking this contract allows is a `Suspending` sink's own bounded withdrawal, which is
    /// the point of delivering that event synchronously.
    /// @param event What happened.
    virtual void OnHostEvent(HostEvent event) = 0;
};

/// Where a consumer listens for host events.
class IHostEvents
{
  public:
    IHostEvents() = default;
    IHostEvents(IHostEvents const&) = delete;
    IHostEvents(IHostEvents&&) = delete;
    IHostEvents& operator=(IHostEvents const&) = delete;
    IHostEvents& operator=(IHostEvents&&) = delete;
    virtual ~IHostEvents() = default;

    /// @param sink Receives every event from now on; must outlive its subscription, and is
    ///        subscribed at most once.
    virtual void Subscribe(IHostEventSink& sink) = 0;

    /// @param sink Receives nothing once this returns -- a delivery to it already in flight on
    ///        another thread has finished by then.
    virtual void Unsubscribe(IHostEventSink& sink) noexcept = 0;
};

/// A subscription, ended when this is destroyed. Declared AFTER the sink it names, so the sink
/// outlives it.
class HostEventSubscription
{
  public:
    /// @param events Where to listen.
    /// @param sink What listens.
    HostEventSubscription(IHostEvents& events, IHostEventSink& sink);
    ~HostEventSubscription();
    HostEventSubscription(HostEventSubscription const&) = delete;
    HostEventSubscription(HostEventSubscription&&) = delete;
    HostEventSubscription& operator=(HostEventSubscription const&) = delete;
    HostEventSubscription& operator=(HostEventSubscription&&) = delete;

  private:
    IHostEvents& _events;
    IHostEventSink& _sink;
};

/// The process's host events: producers deliver into it as a sink, consumers subscribe to it.
///
/// **One thread delivers at a time.** `OnHostEvent` holds a delivery TURN for its whole walk,
/// which the holding thread may re-enter (a sink delivering again) and every other producer
/// waits for. So at most one thread is ever inside a sink, and a consumer never hears two events
/// at once: a power event on the SCM's handler thread waits while the network watcher's thread
/// delivers. A sink must therefore not wait for another producer's thread, which may be waiting
/// for the turn it holds.
///
/// **A sink is called with no lock held**, so it may subscribe, unsubscribe -- itself or another
/// -- or deliver again from inside `OnHostEvent`. A sink that left after a delivery began and
/// before that delivery reached it is not called by it. That is asked by ADDRESS, so a sink
/// subscribed at the address of one that left can hear an event whose delivery began before it
/// subscribed: one it would have heard a moment later anyway, which is benign.
///
/// **`Unsubscribe` from any thread but the turn's holder waits until the holder has left that
/// sink**, which is what makes destroying a sink right after its subscription safe. From the
/// holder -- a sink removing itself or another from inside a delivery -- it does not wait, since
/// every delivery in flight is that thread's own. Since only the holder is ever inside a sink,
/// two sinks removing each other from two producers' threads cannot wait for each other. A caller
/// outside any delivery must not hold what the sink it removes is waiting for.
class HostEventHub final: public IHostEvents, public IHostEventSink
{
  public:
    HostEventHub() = default;
    ~HostEventHub() override = default;
    HostEventHub(HostEventHub const&) = delete;
    HostEventHub(HostEventHub&&) = delete;
    HostEventHub& operator=(HostEventHub const&) = delete;
    HostEventHub& operator=(HostEventHub&&) = delete;

    void Subscribe(IHostEventSink& sink) override;
    void Unsubscribe(IHostEventSink& sink) noexcept override;

    /// Deliver @p event to every subscriber, in subscription order, on the calling thread, once
    /// no other thread holds the delivery turn.
    /// @param event What the host said.
    void OnHostEvent(HostEvent event) override;

    /// @return How many sinks listen now -- what a wiring case asserts.
    [[nodiscard]] std::size_t SubscriberCount() const;

  private:
    class Turn;
    class Delivery;

    mutable std::mutex _mutex;
    std::condition_variable _settled; ///< Notified whenever a delivery leaves its sink or the turn is released.
    std::vector<IHostEventSink*> _sinks;
    std::vector<IHostEventSink*> _inFlight; ///< The sinks the turn's holder is inside.
    std::thread::id _turnHolder;            ///< The thread delivering; meaningful while `_turnDepth` is not zero.
    std::size_t _turnDepth {};              ///< How deeply that thread has entered `OnHostEvent`.
};

/// One `PBT_*` power broadcast this process reacts to.
struct PowerBroadcastRow
{
    std::uint32_t broadcast {}; ///< The `dwEventType` of `SERVICE_CONTROL_POWEREVENT`.
    HostEvent event {};         ///< What it means here.
    std::string_view name;      ///< Its Win32 name, for the reader.
};

/// @return Every power broadcast this process reacts to.
[[nodiscard]] std::span<PowerBroadcastRow const> PowerBroadcastTable() noexcept;

/// @param broadcast A `PBT_*` value.
/// @return Its event, or nullopt for one the table does not name.
[[nodiscard]] std::optional<HostEvent> HostEventForPowerBroadcast(std::uint32_t broadcast) noexcept;

/// How a burst of network notifications becomes one event.
struct NetworkDebounce
{
    std::chrono::milliseconds quiet { 2'000 };    ///< Report once nothing new arrived for this long...
    std::chrono::milliseconds ceiling { 10'000 }; ///< ...or once the burst has lasted this long.
};

/// The decision half of the network watcher: when a burst is reported. Pure over instants, so
/// every rule is a test; the watcher's thread only carries it out.
class NetworkChangeDebouncer
{
  public:
    /// @param bound The quiet period and the ceiling.
    explicit NetworkChangeDebouncer(NetworkDebounce bound) noexcept;

    /// A raw notification arrived at @p now.
    /// @param now When it arrived.
    void Observe(core::platform::SteadyTimePoint now) noexcept;

    /// @return When the pending burst is due, or nullopt when none is pending. Never earlier
    ///         than it was before the latest `Observe`: a notification only postpones.
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> DueAt() const noexcept;

    /// @param now The current instant.
    /// @return True exactly once per burst, at or after its due instant; the burst is then spent.
    [[nodiscard]] bool TakeIfDue(core::platform::SteadyTimePoint now) noexcept;

  private:
    NetworkDebounce _bound;
    std::optional<core::platform::SteadyTimePoint> _first;
    core::platform::SteadyTimePoint _last {};
};

} // namespace FastCache
