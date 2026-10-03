// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NetworkChangeWatcher.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #include <winsock2.h>
// clang-format off
    // ws2ipdef.h and iphlpapi.h both require winsock2.h to have been seen first,
    // which is what this ordering is; clang-format sorts includes alphabetically
    // and would break the build silently.
    #include <ws2ipdef.h>

    #include <iphlpapi.h>
    #include <netioapi.h>
// clang-format on
#endif

namespace FastCache
{

NetworkChangeRelay::NetworkChangeRelay(IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound):
    _sink { sink },
    _clock { clock },
    _debouncer { bound },
    _thread { [this](std::stop_token const& stop) { Run(stop); } }
{
}

NetworkChangeRelay::~NetworkChangeRelay() = default;

void NetworkChangeRelay::Notify() noexcept
{
    std::scoped_lock const lock { _mutex };
    _debouncer.Observe(_clock.now());
    _changed.notify_all();
}

void NetworkChangeRelay::Run(std::stop_token const& stop)
{
    std::unique_lock lock { _mutex };
    while (!stop.stop_requested())
    {
        auto const due = _debouncer.DueAt();
        if (!due.has_value())
        {
            (void) _changed.wait(lock, stop, [this] { return _debouncer.DueAt().has_value(); });
            continue;
        }
        if (!_debouncer.TakeIfDue(_clock.now()))
        {
            // A notification only ever postpones the due instant, so nothing but reaching it --
            // or the stop -- is a reason to look again sooner.
            (void) _changed.wait_for(lock, stop, *due - _clock.now(), [] { return false; });
            continue;
        }
        lock.unlock();
        // Uncaught on purpose: a sink must not throw, and one that does ends the process here.
        _sink.OnHostEvent(HostEvent::NetworkChanged);
        lock.lock();
    }
}

#if !defined(_WIN32)

std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> StartNetworkChangeWatcher(
    IHostEventSink& /*sink*/, core::platform::IClock const& /*clock*/, NetworkDebounce /*bound*/)
{
    return nullptr;
}

#else

namespace
{
    /// Both callbacks only notify the relay: an OS callback that did more would be where a
    /// cancellation could be reached from, and `CancelMibChangeNotify2` waits for the callbacks
    /// in flight, so it must never run inside one.
    VOID NETIOAPI_API_ OnInterface(PVOID context, PMIB_IPINTERFACE_ROW /*row*/, MIB_NOTIFICATION_TYPE /*type*/)
    {
        static_cast<NetworkChangeRelay*>(context)->Notify();
    }

    VOID NETIOAPI_API_ OnAddress(PVOID context, PMIB_UNICASTIPADDRESS_ROW /*row*/, MIB_NOTIFICATION_TYPE /*type*/)
    {
        static_cast<NetworkChangeRelay*>(context)->Notify();
    }

    /// One OS notification the watcher registers for.
    struct NotificationRow
    {
        std::string_view name;                                 ///< The Win32 function, for a refusal.
        DWORD (*registerFor)(NetworkChangeRelay&, HANDLE&) {}; ///< Registers it; NO_ERROR or why not.
    };

    constexpr auto Notifications = std::to_array<NotificationRow>({
        { .name = "NotifyIpInterfaceChange",
          .registerFor = [](NetworkChangeRelay& relay, HANDLE& handle) -> DWORD {
              return NotifyIpInterfaceChange(AF_UNSPEC, &OnInterface, &relay, FALSE, &handle);
          } },
        { .name = "NotifyUnicastIpAddressChange",
          .registerFor = [](NetworkChangeRelay& relay, HANDLE& handle) -> DWORD {
              return NotifyUnicastIpAddressChange(AF_UNSPEC, &OnAddress, &relay, FALSE, &handle);
          } },
    });

    class WindowsNetworkChangeWatcher final: public NetworkChangeWatcher
    {
      public:
        WindowsNetworkChangeWatcher(IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound):
            _relay { sink, clock, bound }
        {
        }

        /// Cancelled in the destructor's BODY, so before `_relay` is destroyed: no callback is
        /// running once `CancelMibChangeNotify2` has returned, and none can reach a relay that
        /// is gone.
        ~WindowsNetworkChangeWatcher() override
        {
            for (HANDLE const handle: _handles)
                if (handle != nullptr)
                    (void) CancelMibChangeNotify2(handle);
        }

        WindowsNetworkChangeWatcher(WindowsNetworkChangeWatcher const&) = delete;
        WindowsNetworkChangeWatcher(WindowsNetworkChangeWatcher&&) = delete;
        WindowsNetworkChangeWatcher& operator=(WindowsNetworkChangeWatcher const&) = delete;
        WindowsNetworkChangeWatcher& operator=(WindowsNetworkChangeWatcher&&) = delete;

        /// Register every notification, or none: a refusal leaves those already registered to
        /// the destructor.
        /// @return Nothing, or which registration the OS refused and why.
        [[nodiscard]] std::expected<void, std::string> Register()
        {
            for (auto const& [row, handle]: std::views::zip(Notifications, _handles))
                if (auto const status = row.registerFor(_relay, handle); status != NO_ERROR)
                    return std::unexpected { std::format(
                        "{} failed: {}", row.name, std::system_category().message(static_cast<int>(status))) };
            return {};
        }

      private:
        NetworkChangeRelay _relay;
        std::array<HANDLE, Notifications.size()> _handles {};
    };
} // namespace

std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> StartNetworkChangeWatcher(
    IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound)
{
    auto watcher = std::make_unique<WindowsNetworkChangeWatcher>(sink, clock, bound);
    return watcher->Register().transform(
        [&watcher] { return std::unique_ptr<NetworkChangeWatcher> { std::move(watcher) }; });
}

#endif // _WIN32

} // namespace FastCache
