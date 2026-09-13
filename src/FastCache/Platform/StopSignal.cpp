// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <array>
#include <atomic>
#include <format>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
#if defined(_WIN32)

    /// The event the console control handler sets; null when no stop signal is installed.
    ///
    /// Process-wide because the handler receives nothing but the control code: this is the
    /// one piece of state it can find.
    std::atomic<HANDLE> stopEvent { nullptr };

    /// The whole handler: set the event, and claim Ctrl-C and Ctrl-Break so the console does
    /// not also end the process. Every other control is left to the next handler.
    /// @param control What the console reported.
    /// @return TRUE when the control was handled here.
    BOOL WINAPI OnConsoleControl(DWORD control) noexcept
    {
        if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT)
            return FALSE;
        auto const event = stopEvent.load(std::memory_order_acquire);
        if (event == nullptr)
            return FALSE;
        (void) ::SetEvent(event);
        return TRUE;
    }

    /// A manual-reset event, closed on destruction. Manual reset is what makes both
    /// outcomes sticky: nothing a wait does can clear it.
    class Event
    {
      public:
        Event() = default;
        Event(Event const&) = delete;
        Event(Event&&) = delete;
        Event& operator=(Event const&) = delete;
        Event& operator=(Event&&) = delete;

        ~Event()
        {
            if (_handle != nullptr)
                (void) ::CloseHandle(_handle);
        }

        /// Create the event.
        /// @return Nothing, or why it could not be created.
        [[nodiscard]] std::expected<void, std::string> Open()
        {
            _handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (_handle == nullptr)
                return std::unexpected(std::format("cannot create an event: {}",
                                                   std::system_category().message(static_cast<int>(::GetLastError()))));
            return {};
        }

        [[nodiscard]] HANDLE Handle() const noexcept
        {
            return _handle;
        }

      private:
        HANDLE _handle { nullptr };
    };

    class WindowsStopSignal final: public IStopSignal
    {
      public:
        WindowsStopSignal() = default;
        WindowsStopSignal(WindowsStopSignal const&) = delete;
        WindowsStopSignal(WindowsStopSignal&&) = delete;
        WindowsStopSignal& operator=(WindowsStopSignal const&) = delete;
        WindowsStopSignal& operator=(WindowsStopSignal&&) = delete;

        ~WindowsStopSignal() override
        {
            if (!_installed)
                return;
            // Unregistered before the event is forgotten, so a Ctrl-C from here on reaches
            // whichever handler was there before, never an event about to be closed.
            (void) ::SetConsoleCtrlHandler(&OnConsoleControl, FALSE);
            stopEvent.store(nullptr, std::memory_order_release);
        }

        /// Create both events and register the console handler.
        /// @return Nothing, or why it could not be installed.
        [[nodiscard]] std::expected<void, std::string> Install()
        {
            if (auto opened = _stop.Open(); !opened.has_value())
                return opened;
            if (auto opened = _cancel.Open(); !opened.has_value())
                return opened;

            auto expected = HANDLE { nullptr };
            if (!stopEvent.compare_exchange_strong(expected, _stop.Handle(), std::memory_order_acq_rel))
                return std::unexpected(std::string { "a stop signal is already installed in this process" });

            if (::SetConsoleCtrlHandler(&OnConsoleControl, TRUE) == FALSE)
            {
                stopEvent.store(nullptr, std::memory_order_release);
                return std::unexpected(std::format("cannot register a console control handler: {}",
                                                   std::system_category().message(static_cast<int>(::GetLastError()))));
            }
            _installed = true;
            return {};
        }

        [[nodiscard]] Task<StopWake> Stopped(IExecutor* waiter, IExecutor* resumeOn) override
        {
            co_await ResumeOn { *waiter };
            auto const wake = WaitBlocking();
            co_await ResumeOn { *resumeOn };
            co_return wake;
        }

        void Cancel() noexcept override
        {
            (void) ::SetEvent(_cancel.Handle());
        }

      private:
        /// Block until either event is set.
        /// @return Why the wait ended.
        [[nodiscard]] StopWake WaitBlocking() const
        {
            // Cancel FIRST: `WaitForMultipleObjects` names the lowest index when both are set,
            // and a session that is already closing has decided how it ends.
            auto const handles = std::array<HANDLE, 2> { _cancel.Handle(), _stop.Handle() };
            switch (::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE))
            {
                case WAIT_OBJECT_0:
                    return StopWake::Cancelled;
                case WAIT_OBJECT_0 + 1:
                    return StopWake::Stopped;
                default:
                    return StopWake::Failed;
            }
        }

        Event _stop;
        Event _cancel;
        bool _installed { false };
    };

    using PlatformStopSignal = WindowsStopSignal;

#else

    /// The write end the handler writes to; -1 when no stop signal is installed.
    ///
    /// Process-wide because a signal handler receives nothing but the signal number: this is
    /// the one piece of state it can find. Lock-free, which is what makes reading it from a
    /// handler async-signal-safe.
    std::atomic<int> stopPipeWrite { -1 };

    static_assert(std::atomic<int>::is_always_lock_free, "the stop handler reads this from a signal handler");

    /// The whole handler: one `write(2)`, which is the one async-signal-safe way to say
    /// something. A full pipe drops the byte, and that is fine -- one byte already says it.
    /// @param number The signal number; unused, since only SIGINT is routed here.
    void OnStopSignal(int number) noexcept
    {
        static_cast<void>(number);
        auto const savedErrno = errno;
        if (auto const fd = stopPipeWrite.load(std::memory_order_acquire); fd >= 0)
        {
            auto const byte = char { 1 };
            static_cast<void>(::write(fd, &byte, 1));
        }
        errno = savedErrno;
    }

    /// Both ends of a pipe, closed on destruction.
    class Pipe
    {
      public:
        Pipe() = default;
        Pipe(Pipe const&) = delete;
        Pipe(Pipe&&) = delete;
        Pipe& operator=(Pipe const&) = delete;
        Pipe& operator=(Pipe&&) = delete;

        ~Pipe()
        {
            for (auto const fd: _ends)
                if (fd >= 0)
                    static_cast<void>(::close(fd));
        }

        /// Open the pipe: close-on-exec at both ends, so a child this process spawns
        /// inherits neither, and a write end that never blocks, so the handler cannot.
        /// @return Nothing, or why it could not be opened.
        [[nodiscard]] std::expected<void, std::string> Open()
        {
            if (::pipe(_ends.data()) != 0)
                return std::unexpected(std::format("cannot open a pipe: {}", std::generic_category().message(errno)));
            for (auto const fd: _ends)
                if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
                    return std::unexpected(
                        std::format("cannot mark a pipe close-on-exec: {}", std::generic_category().message(errno)));
            if (auto const flags = ::fcntl(Writer(), F_GETFL);
                flags < 0 || ::fcntl(Writer(), F_SETFL, flags | O_NONBLOCK) != 0)
                return std::unexpected(
                    std::format("cannot make a pipe non-blocking: {}", std::generic_category().message(errno)));
            return {};
        }

        [[nodiscard]] int Reader() const noexcept
        {
            return _ends[0];
        }

        [[nodiscard]] int Writer() const noexcept
        {
            return _ends[1];
        }

      private:
        std::array<int, 2> _ends { -1, -1 };
    };

    class PosixStopSignal final: public IStopSignal
    {
      public:
        PosixStopSignal() = default;
        PosixStopSignal(PosixStopSignal const&) = delete;
        PosixStopSignal(PosixStopSignal&&) = delete;
        PosixStopSignal& operator=(PosixStopSignal const&) = delete;
        PosixStopSignal& operator=(PosixStopSignal&&) = delete;

        ~PosixStopSignal() override
        {
            if (!_installed)
                return;
            // The disposition first and the pipe after, so a SIGINT from here on finds the
            // previous handler rather than a descriptor about to be closed.
            static_cast<void>(::sigaction(SIGINT, &_previous, nullptr));
            stopPipeWrite.store(-1, std::memory_order_release);
        }

        /// Open both pipes and route SIGINT to the stop pipe.
        /// @return Nothing, or why it could not be installed.
        [[nodiscard]] std::expected<void, std::string> Install()
        {
            if (auto opened = _stop.Open(); !opened.has_value())
                return opened;
            if (auto opened = _cancel.Open(); !opened.has_value())
                return opened;

            auto expected = -1;
            if (!stopPipeWrite.compare_exchange_strong(expected, _stop.Writer(), std::memory_order_acq_rel))
                return std::unexpected(std::string { "a stop signal is already installed in this process" });

            struct sigaction ours {};
            ours.sa_handler = &OnStopSignal;
            static_cast<void>(::sigemptyset(&ours.sa_mask));
            // Restarted, so a SIGINT landing on some other thread's blocking call is not
            // turned into that call failing with EINTR.
            ours.sa_flags = SA_RESTART;
            if (::sigaction(SIGINT, &ours, &_previous) != 0)
            {
                stopPipeWrite.store(-1, std::memory_order_release);
                return std::unexpected(
                    std::format("cannot install a SIGINT handler: {}", std::generic_category().message(errno)));
            }
            _installed = true;
            return {};
        }

        [[nodiscard]] Task<StopWake> Stopped(IExecutor* waiter, IExecutor* resumeOn) override
        {
            co_await ResumeOn { *waiter };
            auto const wake = WaitBlocking();
            co_await ResumeOn { *resumeOn };
            co_return wake;
        }

        void Cancel() noexcept override
        {
            auto const byte = char { 1 };
            static_cast<void>(::write(_cancel.Writer(), &byte, 1));
        }

      private:
        /// Block until either pipe is readable.
        ///
        /// Neither byte is ever read, which is what keeps both outcomes sticky: a readable
        /// pipe stays readable for every later wait.
        /// @return Why the wait ended.
        [[nodiscard]] StopWake WaitBlocking() const
        {
            auto fds = std::array<pollfd, 2> {
                pollfd { .fd = _cancel.Reader(), .events = POLLIN, .revents = 0 },
                pollfd { .fd = _stop.Reader(), .events = POLLIN, .revents = 0 },
            };
            auto ready = ::poll(fds.data(), fds.size(), -1);
            // A signal landed on THIS thread, and the SIGINT it may have been has already
            // written its byte: look again rather than report a failure.
            while (ready < 0 && errno == EINTR)
                ready = ::poll(fds.data(), fds.size(), -1);
            if (ready < 0)
                return StopWake::Failed;
            // Cancel first when both are ready: a session that is already closing has
            // decided how it ends.
            if ((fds[0].revents & POLLIN) != 0)
                return StopWake::Cancelled;
            if ((fds[1].revents & POLLIN) != 0)
                return StopWake::Stopped;
            return StopWake::Failed;
        }

        Pipe _stop;
        Pipe _cancel;
        struct sigaction _previous {};
        bool _installed { false };
    };

    using PlatformStopSignal = PosixStopSignal;

#endif
} // namespace

std::expected<std::unique_ptr<IStopSignal>, std::string> InstallStopSignal()
{
    auto installed = std::make_unique<PlatformStopSignal>();
    if (auto outcome = installed->Install(); !outcome.has_value())
        return std::unexpected(std::move(outcome).error());
    return installed;
}

} // namespace FastCache
