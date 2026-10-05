// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostEvents.hpp>

#include <algorithm>
#include <array>

#include <core/Ranges.hpp>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

namespace
{
    constexpr auto PowerBroadcasts = std::to_array<PowerBroadcastRow>({
        { .broadcast = 0x4, .event = HostEvent::Suspending, .name = "PBT_APMSUSPEND" },
        { .broadcast = 0x7, .event = HostEvent::Resumed, .name = "PBT_APMRESUMESUSPEND" },
        { .broadcast = 0x12, .event = HostEvent::Resumed, .name = "PBT_APMRESUMEAUTOMATIC" },
    });
} // namespace

#if defined(_WIN32)
static_assert(PBT_APMSUSPEND == 0x4 && PBT_APMRESUMESUSPEND == 0x7 && PBT_APMRESUMEAUTOMATIC == 0x12,
              "PowerBroadcasts spells the PBT_* values as numbers so the table is portable");
#endif

HostEventSubscription::HostEventSubscription(IHostEvents& events, IHostEventSink& sink):
    _events { events },
    _sink { sink }
{
    _events.Subscribe(_sink);
}

HostEventSubscription::~HostEventSubscription()
{
    _events.Unsubscribe(_sink);
}

/// The delivery turn, held for one walk over the sinks: taken once no other thread holds it, and
/// re-entered freely by the thread that does. Released on every path out, a throwing sink included.
class HostEventHub::Turn
{
  public:
    /// @param hub The hub whose turn to take; its lock is not held by the caller.
    explicit Turn(HostEventHub& hub):
        _hub { hub }
    {
        auto const self = std::this_thread::get_id();
        std::unique_lock lock { _hub._mutex };
        _hub._settled.wait(lock, [this, self] { return _hub._turnDepth == 0 || _hub._turnHolder == self; });
        _hub._turnHolder = self;
        ++_hub._turnDepth;
    }

    /// Notifies under the lock, as `Delivery` does, and for the same reason.
    ~Turn()
    {
        std::scoped_lock const lock { _hub._mutex };
        if (--_hub._turnDepth == 0)
            _hub._turnHolder = std::thread::id {};
        _hub._settled.notify_all();
    }

    Turn(Turn const&) = delete;
    Turn(Turn&&) = delete;
    Turn& operator=(Turn const&) = delete;
    Turn& operator=(Turn&&) = delete;

  private:
    HostEventHub& _hub;
};

/// One delivery to one sink, recorded in the hub's in-flight list for as long as the sink runs --
/// including when it throws, which is why this is an object rather than two calls.
class HostEventHub::Delivery
{
  public:
    /// @param hub The hub, whose lock the caller holds.
    /// @param sink The sink about to be called.
    Delivery(HostEventHub& hub, IHostEventSink* sink):
        _hub { hub },
        _sink { sink }
    {
        _hub._inFlight.push_back(_sink);
    }

    /// Notifies UNDER the lock, so an `Unsubscribe` this releases cannot return -- and let its
    /// caller tear anything down -- before this has finished touching the hub.
    ~Delivery()
    {
        std::scoped_lock const lock { _hub._mutex };
        if (auto const at = std::ranges::find(_hub._inFlight, _sink); at != _hub._inFlight.end())
            _hub._inFlight.erase(at);
        _hub._settled.notify_all();
    }

    Delivery(Delivery const&) = delete;
    Delivery(Delivery&&) = delete;
    Delivery& operator=(Delivery const&) = delete;
    Delivery& operator=(Delivery&&) = delete;

  private:
    HostEventHub& _hub;
    IHostEventSink* _sink;
};

void HostEventHub::Subscribe(IHostEventSink& sink)
{
    std::scoped_lock const lock { _mutex };
    _sinks.push_back(&sink);
}

void HostEventHub::Unsubscribe(IHostEventSink& sink) noexcept
{
    std::unique_lock lock { _mutex };
    std::erase(_sinks, &sink);
    // The holder is the only thread inside any sink, so what it has in flight is its own.
    if (_turnDepth != 0 && _turnHolder == std::this_thread::get_id())
        return;
    _settled.wait(lock, [this, &sink] { return std::ranges::find(_inFlight, &sink) == _inFlight.end(); });
}

void HostEventHub::OnHostEvent(HostEvent event)
{
    Turn const turn { *this };
    auto const snapshot = [this] {
        std::scoped_lock const lock { _mutex };
        return _sinks;
    }();
    for (auto* const sink: snapshot)
    {
        std::unique_lock lock { _mutex };
        // Re-asked under the lock: a sink that left since the snapshot -- from inside an
        // earlier sink, or on another thread -- has been told it hears nothing more.
        if (std::ranges::find(_sinks, sink) == _sinks.end())
            continue;
        Delivery const delivery { *this, sink };
        lock.unlock();
        sink->OnHostEvent(event);
    }
}

std::size_t HostEventHub::SubscriberCount() const
{
    std::scoped_lock const lock { _mutex };
    return _sinks.size();
}

std::span<PowerBroadcastRow const> PowerBroadcastTable() noexcept
{
    return PowerBroadcasts;
}

std::optional<HostEvent> HostEventForPowerBroadcast(std::uint32_t broadcast) noexcept
{
    if (auto const* const row = core::findOrNull(PowerBroadcasts, broadcast, &PowerBroadcastRow::broadcast))
        return row->event;
    return std::nullopt;
}

NetworkChangeDebouncer::NetworkChangeDebouncer(NetworkDebounce bound) noexcept:
    _bound { bound }
{
}

void NetworkChangeDebouncer::Observe(core::platform::SteadyTimePoint now) noexcept
{
    if (!_first.has_value())
        _first = now;
    _last = now;
}

std::optional<core::platform::SteadyTimePoint> NetworkChangeDebouncer::DueAt() const noexcept
{
    if (!_first.has_value())
        return std::nullopt;
    return std::min(_last + _bound.quiet, *_first + _bound.ceiling);
}

bool NetworkChangeDebouncer::TakeIfDue(core::platform::SteadyTimePoint now) noexcept
{
    auto const due = DueAt();
    if (!due.has_value() || now < *due)
        return false;
    _first.reset();
    return true;
}

} // namespace FastCache
