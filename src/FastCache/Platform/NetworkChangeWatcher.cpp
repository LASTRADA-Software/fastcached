// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NetworkChangeWatcher.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
    #include <FastCache/Platform/NetworkChangeMessages.hpp>

    #include <sys/socket.h>

    #include <cerrno>
    #include <cstddef>
    #include <cstdint>
    #include <optional>
    #include <span>
    #include <vector>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>

    #if defined(__linux__)
        #include <linux/netlink.h>
        #include <linux/rtnetlink.h>
    #elif defined(__APPLE__)
        #include <net/route.h>
    #endif
#endif

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

namespace
{
    /// How much one read takes: a netlink or route-socket datagram reporting a change is far
    /// smaller, and one larger is read truncated rather than lost.
    constexpr std::size_t ReadBufferSize = 16 * 1024;

    /// @param what The call that failed.
    /// @return A refusal naming @p what and `errno`'s reason.
    [[nodiscard]] std::string Refusal(std::string_view what)
    {
        return std::format("{} failed: {}", what, std::system_category().message(errno));
    }

    /// A descriptor closed when it goes out of scope.
    class OwnedDescriptor
    {
      public:
        OwnedDescriptor() = default;

        /// @param descriptor Owned from here on; negative for none.
        explicit OwnedDescriptor(int descriptor) noexcept:
            _descriptor { descriptor }
        {
        }

        ~OwnedDescriptor()
        {
            if (_descriptor >= 0)
                (void) ::close(_descriptor);
        }

        OwnedDescriptor(OwnedDescriptor&& other) noexcept:
            _descriptor { std::exchange(other._descriptor, -1) }
        {
        }
        OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept
        {
            OwnedDescriptor { std::move(other) }.Swap(*this);
            return *this;
        }
        OwnedDescriptor(OwnedDescriptor const&) = delete;
        OwnedDescriptor& operator=(OwnedDescriptor const&) = delete;

        /// @return The descriptor, still owned; negative for none.
        [[nodiscard]] int Get() const noexcept
        {
            return _descriptor;
        }

      private:
        /// @param other Exchanges descriptors with this one.
        void Swap(OwnedDescriptor& other) noexcept
        {
            std::swap(_descriptor, other._descriptor);
        }

        int _descriptor = -1;
    };

    /// @param descriptor Marked close-on-exec, so a compiler the node spawns does not inherit it.
    /// @return Whether it was.
    [[nodiscard]] bool SetCloseOnExec(int descriptor) noexcept
    {
        return ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
    }

    /// The pipe the destructor wakes the reading thread through.
    struct StopPipe
    {
        OwnedDescriptor readEnd;  ///< Polled by the thread.
        OwnedDescriptor writeEnd; ///< Written once, by the destructor.
    };

    /// @return Both ends, close-on-exec; or why not.
    [[nodiscard]] std::expected<StopPipe, std::string> MakeStopPipe()
    {
        std::array<int, 2> ends { -1, -1 };
        if (::pipe(ends.data()) != 0)
            return std::unexpected { Refusal("stop pipe") };
        auto stop = StopPipe { .readEnd = OwnedDescriptor { ends[0] }, .writeEnd = OwnedDescriptor { ends[1] } };
        if (!SetCloseOnExec(stop.readEnd.Get()) || !SetCloseOnExec(stop.writeEnd.Get()))
            return std::unexpected { Refusal("stop pipe fcntl") };
        return stop;
    }

    /// Reads one change socket on a thread of its own, into one relay.
    class PosixNetworkChangeWatcher final: public NetworkChangeWatcher
    {
      public:
        /// @param socket What is read; owned.
        /// @param stop Wakes the thread; owned.
        /// @param classify Reads one buffer.
        /// @param sink Where the event goes.
        /// @param clock What the debouncer reads.
        /// @param bound The debounce.
        PosixNetworkChangeWatcher(OwnedDescriptor socket,
                                  StopPipe stop,
                                  NetworkChangeClassifier classify,
                                  IHostEventSink& sink,
                                  core::platform::IClock const& clock,
                                  NetworkDebounce bound):
            _relay { sink, clock, bound },
            _socket { std::move(socket) },
            _stop { std::move(stop) },
            _classify { classify },
            _thread { [this] { Run(); } }
        {
        }

        /// Wakes the thread in the destructor's BODY, so it has returned -- `_thread` is destroyed
        /// first, joining it -- before any descriptor it polls is closed or the relay it notifies
        /// is gone.
        ~PosixNetworkChangeWatcher() override
        {
            constexpr auto wake = std::byte { 1 };
            while (::write(_stop.writeEnd.Get(), &wake, sizeof(wake)) < 0 && errno == EINTR)
                continue;
        }

        PosixNetworkChangeWatcher(PosixNetworkChangeWatcher const&) = delete;
        PosixNetworkChangeWatcher(PosixNetworkChangeWatcher&&) = delete;
        PosixNetworkChangeWatcher& operator=(PosixNetworkChangeWatcher const&) = delete;
        PosixNetworkChangeWatcher& operator=(PosixNetworkChangeWatcher&&) = delete;

      private:
        /// What one wake of the thread found, and so what it does next.
        enum class Step : std::uint8_t
        {
            Continue, ///< Poll again.
            Stop,     ///< Return: stopped, at end of file, or on an error that would repeat.
        };

        /// The thread: poll the socket and the stop pipe until the stop pipe is readable.
        void Run()
        {
            std::vector<std::byte> buffer(ReadBufferSize);
            while (Poll(buffer) == Step::Continue)
                continue;
        }

        /// Wait for either descriptor, then read the socket if it is the one ready.
        /// @param buffer Where a read lands.
        /// @return What to do next.
        [[nodiscard]] Step Poll(std::span<std::byte> buffer)
        {
            auto fds = std::to_array<pollfd>({
                { .fd = _socket.Get(), .events = POLLIN, .revents = 0 },
                { .fd = _stop.readEnd.Get(), .events = POLLIN, .revents = 0 },
            });
            if (::poll(fds.data(), fds.size(), -1) < 0)
                return errno == EINTR ? Step::Continue : Step::Stop;
            if (fds[1].revents != 0)
                return Step::Stop;
            if (fds[0].revents == 0)
                return Step::Continue;
            return Read(buffer);
        }

        /// Read what the socket holds and notify the relay when it reports a change.
        /// @param buffer Where the read lands.
        /// @return What to do next.
        [[nodiscard]] Step Read(std::span<std::byte> buffer)
        {
            auto const got = ::read(_socket.Get(), buffer.data(), buffer.size());
            if (got > 0)
            {
                if (_classify(buffer.first(static_cast<std::size_t>(got))))
                    _relay.Notify();
                return Step::Continue;
            }
            if (got == 0)
                return Step::Stop;
            switch (errno)
            {
                case EINTR:
                case EAGAIN:
                    return Step::Continue;
                case ENOBUFS:
                    // The kernel dropped notifications it had no room for: a change was among them.
                    _relay.Notify();
                    return Step::Continue;
                default:
                    return Step::Stop;
            }
        }

        NetworkChangeRelay _relay;
        OwnedDescriptor _socket;
        StopPipe _stop;
        NetworkChangeClassifier _classify;
        /// LAST, so every member it reads is built before it starts and freed after it joins.
        std::jthread _thread;
    };

    /// One platform's change socket: how it is opened, and how what it delivers is read.
    struct ChangeSocketRow
    {
        std::expected<OwnedDescriptor, std::string> (*open)() {}; ///< The socket, or why not.
        NetworkChangeClassifier classify {};                      ///< Reads one buffer from it.
    };

    #if defined(__linux__)
    /// @return A `NETLINK_ROUTE` socket bound to the link, address and route groups; or why not.
    [[nodiscard]] std::expected<OwnedDescriptor, std::string> OpenNetlinkSocket()
    {
        auto socket = OwnedDescriptor { ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE) };
        if (socket.Get() < 0)
            return std::unexpected { Refusal("netlink socket") };
        sockaddr_nl address {};
        address.nl_family = AF_NETLINK;
        address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
        if (::bind(socket.Get(), reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
            return std::unexpected { Refusal("netlink bind") };
        return socket;
    }

    constexpr auto PlatformChangeSocket =
        std::optional { ChangeSocketRow { .open = &OpenNetlinkSocket, .classify = &NetlinkReportsChange } };
    #elif defined(__APPLE__)
    /// @return A `PF_ROUTE` socket, which hears every family's routing messages; or why not.
    [[nodiscard]] std::expected<OwnedDescriptor, std::string> OpenRouteSocket()
    {
        auto socket = OwnedDescriptor { ::socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC) };
        if (socket.Get() < 0)
            return std::unexpected { Refusal("route socket") };
        if (!SetCloseOnExec(socket.Get()))
            return std::unexpected { Refusal("route socket fcntl") };
        return socket;
    }

    constexpr auto PlatformChangeSocket =
        std::optional { ChangeSocketRow { .open = &OpenRouteSocket, .classify = &RouteSocketReportsChange } };
    #else
    /// This platform offers nothing this project watches.
    constexpr auto PlatformChangeSocket = std::optional<ChangeSocketRow> {};
    #endif

    /// @param socket What the watcher reads; owned.
    /// @param classify Reads one buffer.
    /// @param sink Where the event goes.
    /// @param clock What the debouncer reads.
    /// @param bound The debounce.
    /// @return The running watcher; or why its stop pipe could not be made.
    [[nodiscard]] std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> Watch(
        OwnedDescriptor socket,
        NetworkChangeClassifier classify,
        IHostEventSink& sink,
        core::platform::IClock const& clock,
        NetworkDebounce bound)
    {
        return MakeStopPipe().transform([&](StopPipe stop) {
            return std::unique_ptr<NetworkChangeWatcher> { std::make_unique<PosixNetworkChangeWatcher>(
                std::move(socket), std::move(stop), classify, sink, clock, bound) };
        });
    }
} // namespace

std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> WatchNetworkChangeDescriptor(
    int descriptor,
    NetworkChangeClassifier classify,
    IHostEventSink& sink,
    core::platform::IClock const& clock,
    NetworkDebounce bound)
{
    return Watch(OwnedDescriptor { descriptor }, classify, sink, clock, bound);
}

std::expected<std::unique_ptr<NetworkChangeWatcher>, std::string> StartNetworkChangeWatcher(
    IHostEventSink& sink, core::platform::IClock const& clock, NetworkDebounce bound)
{
    if (!PlatformChangeSocket.has_value())
        return nullptr;
    return PlatformChangeSocket->open().and_then([&](OwnedDescriptor socket) {
        return Watch(std::move(socket), PlatformChangeSocket->classify, sink, clock, bound);
    });
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
