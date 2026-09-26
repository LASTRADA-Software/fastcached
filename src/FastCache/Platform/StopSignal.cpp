// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/StopSignal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <system_error>
#include <utility>

#include <core/async/ResumeOn.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <cstdlib>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
    /// A stop signal whose wait blocks a thread: the two hops every platform takes around it.
    ///
    /// Written once here, so what differs between platforms is only how the thread blocks.
    class BlockingStopSignal: public IStopSignal
    {
      public:
        [[nodiscard]] core::async::Task<StopWake> Stopped(core::async::IExecutor* waiter,
                                                          core::async::IExecutor* resumeOn) final
        {
            co_await core::async::ResumeOn { *waiter };
            auto const wake = WaitBlocking();
            co_await core::async::ResumeOn { *resumeOn };
            co_return wake;
        }

      private:
        /// Block the calling thread until a stop is requested or the signal is cancelled.
        /// @return Why the wait ended; `Cancelled` wins when both have happened.
        [[nodiscard]] virtual StopWake WaitBlocking() const = 0;
    };

    /// Whether a stop signal is installed in this process right now: the one-at-a-time rule.
    ///
    /// Separate from the handler-facing end below, which outlives every install.
    std::atomic<bool> stopClaimed { false };

    /// Claims the one-install slot for as long as it lives, unless released to an install.
    class StopClaim
    {
      public:
        StopClaim() noexcept:
            _held { [] {
                auto expected = false;
                return stopClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
            }() }
        {
        }

        StopClaim(StopClaim const&) = delete;
        StopClaim(StopClaim&&) = delete;
        StopClaim& operator=(StopClaim const&) = delete;
        StopClaim& operator=(StopClaim&&) = delete;

        ~StopClaim()
        {
            if (_held)
                stopClaimed.store(false, std::memory_order_release);
        }

        /// @return Whether the slot was free and is now this claim's.
        [[nodiscard]] bool Held() const noexcept
        {
            return _held;
        }

        /// Hand the slot to an install, which releases it when it is destroyed.
        void Keep() noexcept
        {
            _held = false;
        }

      private:
        bool _held;
    };

#if defined(_WIN32)

    /// The event the console control handler sets, created once and **never closed**.
    ///
    /// `SetConsoleCtrlHandler(..., FALSE)` does not wait for a handler already running on the
    /// console's own thread, so a handler that loaded the handle just before an uninstall could
    /// call `SetEvent` after the event was closed -- on whatever object had reused the value by
    /// then. So the handler-facing handle lives for the process: one handle, reused by every later
    /// install, which resets it first. That is the whole reason it is never closed; it is not a
    /// leak, and closing it on uninstall reopens exactly that race.
    std::atomic<HANDLE> stopEvent { nullptr };

    /// The whole handler: set the event, and claim Ctrl-C and Ctrl-Break so the console does
    /// not also end the process. Every other control, and any control while nothing is installed,
    /// is left to the next handler.
    /// @param control What the console reported.
    /// @return TRUE when the control was handled here.
    BOOL WINAPI OnConsoleControl(DWORD control) noexcept
    {
        if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT)
            return FALSE;
        auto const event = stopEvent.load(std::memory_order_acquire);
        if (!stopClaimed.load(std::memory_order_acquire) || event == nullptr)
            return FALSE;
        (void) ::SetEvent(event);
        return TRUE;
    }

    /// Create the process's stop event if no install has yet. Only the holder of the claim calls
    /// this, so two installs never race to create it.
    /// @return Nothing, or why it could not be created.
    [[nodiscard]] std::expected<void, std::string> EnsureStopEvent()
    {
        if (stopEvent.load(std::memory_order_acquire) != nullptr)
            return {};
        auto const event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr)
            return std::unexpected(std::format("cannot create an event: {}",
                                               std::system_category().message(static_cast<int>(::GetLastError()))));
        stopEvent.store(event, std::memory_order_release);
        return {};
    }

    /// A manual-reset event, closed on destruction. Manual reset is what makes an outcome
    /// sticky: nothing a wait does can clear it.
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

    class WindowsStopSignal final: public BlockingStopSignal
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
            // Unregistered, then the slot released: a Ctrl-C from here on reaches whichever
            // handler was there before. The event stays open -- see `stopEvent`.
            (void) ::SetConsoleCtrlHandler(&OnConsoleControl, FALSE);
            stopClaimed.store(false, std::memory_order_release);
        }

        /// Create the cancel event, reset the process's stop event, and register the handler.
        /// @return Nothing, or why it could not be installed.
        [[nodiscard]] std::expected<void, std::string> Install()
        {
            if (auto opened = _cancel.Open(); !opened.has_value())
                return opened;

            auto claim = StopClaim {};
            if (!claim.Held())
                return std::unexpected(std::string { "a stop signal is already installed in this process" });
            if (auto created = EnsureStopEvent(); !created.has_value())
                return created;
            // A stop an earlier install heard is that install's, not this one's.
            (void) ::ResetEvent(stopEvent.load(std::memory_order_acquire));

            if (::SetConsoleCtrlHandler(&OnConsoleControl, TRUE) == FALSE)
                return std::unexpected(std::format("cannot register a console control handler: {}",
                                                   std::system_category().message(static_cast<int>(::GetLastError()))));
            claim.Keep();
            _installed = true;
            return {};
        }

        void Cancel() noexcept override
        {
            (void) ::SetEvent(_cancel.Handle());
        }

      private:
        /// Block until either event is set.
        /// @return Why the wait ended.
        [[nodiscard]] StopWake WaitBlocking() const override
        {
            // Cancel FIRST: `WaitForMultipleObjects` names the lowest index when both are set,
            // and a session that is already closing has decided how it ends.
            auto const handles = std::array<HANDLE, 2> { _cancel.Handle(), stopEvent.load(std::memory_order_acquire) };
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

        Event _cancel;
        bool _installed { false };
    };

    /// Install the process's stop signal.
    /// @return The signal, or why it could not be installed.
    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> InstallPlatformStopSignal()
    {
        // No inherited-ignore arm here, deliberately. A console process started with Ctrl-C
        // disabled (`CREATE_NEW_PROCESS_GROUP`, or a parent that called
        // `SetConsoleCtrlHandler(nullptr, TRUE)`) inherits an ignore flag, and Windows offers no
        // call that reads it back -- so rather than guess, this installs as usual, and a console
        // that never delivers Ctrl-C simply never fires the signal.
        auto installed = std::make_unique<WindowsStopSignal>();
        if (auto outcome = installed->Install(); !outcome.has_value())
            return std::unexpected(std::move(outcome).error());
        return installed;
    }

#else

    /// The self-pipe the handler writes to, created once and **never closed**, read end included.
    ///
    /// A handler already running on another thread may have loaded the write end just before an
    /// uninstall; closed then, its `write(2)` would land in whatever descriptor reused the number.
    /// So the handler-facing pipe lives for the process: two descriptors, reused by every later
    /// install, which drains the pipe first. The READ end stays open too, and not for symmetry: a
    /// pipe whose reader was closed answers the handler's write with SIGPIPE, which ends the
    /// process -- the very death this seam exists not to change. That is the whole reason neither
    /// end is closed; it is not a leak, and closing either on uninstall reopens a race.
    std::atomic<int> stopPipeRead { -1 };

    /// See `stopPipeRead`. Lock-free, which is what makes reading it from a handler
    /// async-signal-safe.
    std::atomic<int> stopPipeWrite { -1 };

    static_assert(std::atomic<int>::is_always_lock_free, "the stop handler reads this from a signal handler");

    /// What writing one wake byte to a non-blocking pipe came to.
    ///
    /// TRANSMITTED/PERSISTED: no. Private to this file; enumerators may be inserted.
    enum class WakeWrite : std::uint8_t
    {
        Written,        ///< The byte is in the pipe.
        AlreadyPending, ///< The pipe is full, so a wake is already waiting to be seen.
        Failed,         ///< Anything else: the pipe cannot carry a wake.
    };

    /// The `errno` values a non-blocking write answers for a full pipe. Two spellings that are one
    /// value on most hosts and need not be, so they are listed rather than compared in one `||`.
    constexpr auto FullPipeErrors = std::array { EAGAIN, EWOULDBLOCK };

    /// Write one wake byte to @p fd, a non-blocking pipe whose bytes are never consumed by a wait.
    ///
    /// **Async-signal-safe**: `write(2)` and `errno`, no allocation, no logging -- the stop handler
    /// calls it. Three answers, because the failures are not alike:
    ///   - `EINTR` is retried: a signal landing mid-write is not an answer about the pipe.
    ///   - A FULL pipe (`EAGAIN`) is `AlreadyPending`, and benign: a wait only polls these pipes
    ///     and never reads them, so the bytes filling it stay readable and a wake is already due.
    ///     One more byte would say nothing the pipe does not already say.
    ///   - Anything else is `Failed`. Neither pipe is closed while a writer can reach it, so no
    ///     other answer should happen -- and a wake that could not be written leaves a wait that
    ///     will never end.
    /// @param fd The write end.
    /// @return What the write came to.
    [[nodiscard]] WakeWrite WriteWakeByte(int fd) noexcept
    {
        auto const byte = char { 1 };
        auto written = ::write(fd, &byte, 1);
        while (written < 0 && errno == EINTR)
            written = ::write(fd, &byte, 1);
        if (written == 1)
            return WakeWrite::Written;
        if (written < 0 && std::ranges::find(FullPipeErrors, errno) != FullPipeErrors.end())
            return WakeWrite::AlreadyPending;
        return WakeWrite::Failed;
    }

    /// Write a wake byte to @p fd, and end the process when the pipe cannot carry one.
    ///
    /// **Ending the process is the answer, not a shortcut past one.** Every caller has nothing to
    /// return a failure through -- a signal handler, and a `noexcept` cancel -- and the waiter it
    /// exists to wake would otherwise block forever: a session the operator can no longer stop, or
    /// one that never finishes closing, with nothing anywhere saying why. `Failed` means the
    /// never-closed invariant on these pipes has been broken, which is a defect in this file; and
    /// `std::abort` is async-signal-safe, so the handler may take this path too.
    /// @param fd The write end.
    void WakeOrAbort(int fd) noexcept
    {
        if (WriteWakeByte(fd) == WakeWrite::Failed)
            std::abort();
    }

    /// The whole handler: one `write(2)`, which is the one async-signal-safe way to say
    /// something. A full pipe drops the byte, and that is fine -- one byte already says it
    /// (`WriteWakeByte`).
    /// A byte written after an uninstall is drained by the next install, never heard as its stop.
    /// @param number The signal number; unused, since only SIGINT is routed here.
    void OnStopSignal(int number) noexcept
    {
        static_cast<void>(number);
        auto const savedErrno = errno;
        if (auto const fd = stopPipeWrite.load(std::memory_order_acquire); fd >= 0)
        {
            WakeOrAbort(fd);
        }
        errno = savedErrno;
    }

    /// Make @p fd close-on-exec and, when asked, non-blocking.
    /// @param fd The descriptor.
    /// @param nonBlocking Whether reads and writes must never block.
    /// @return Nothing, or why it could not be set.
    [[nodiscard]] std::expected<void, std::string> Configure(int fd, bool nonBlocking)
    {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
            return std::unexpected(
                std::format("cannot mark a pipe close-on-exec: {}", std::generic_category().message(errno)));
        if (!nonBlocking)
            return {};
        if (auto const flags = ::fcntl(fd, F_GETFL); flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
            return std::unexpected(
                std::format("cannot make a pipe non-blocking: {}", std::generic_category().message(errno)));
        return {};
    }

    /// Create the process's stop pipe if no install has yet. Only the holder of the claim calls
    /// this, so two installs never race to create it.
    ///
    /// Both ends non-blocking: the handler must never block on a full pipe, and an install drains
    /// the read end without waiting for a byte that is not there.
    /// @return Nothing, or why it could not be created.
    [[nodiscard]] std::expected<void, std::string> EnsureStopPipe()
    {
        if (stopPipeWrite.load(std::memory_order_acquire) >= 0)
            return {};
        auto ends = std::array<int, 2> { -1, -1 };
        if (::pipe(ends.data()) != 0)
            return std::unexpected(std::format("cannot open a pipe: {}", std::generic_category().message(errno)));
        for (auto const fd: ends)
            if (auto configured = Configure(fd, true); !configured.has_value())
            {
                // Never handed to the handler, so these two can still be closed.
                for (auto const end: ends)
                    static_cast<void>(::close(end));
                return configured;
            }
        stopPipeRead.store(ends[0], std::memory_order_release);
        stopPipeWrite.store(ends[1], std::memory_order_release);
        return {};
    }

    /// Discard every byte an earlier install's stop left in the process's stop pipe.
    void DrainStopPipe() noexcept
    {
        auto buffer = std::array<char, 64> {};
        while (::read(stopPipeRead.load(std::memory_order_acquire), buffer.data(), buffer.size()) > 0)
        {
        }
    }

    /// Both ends of a pipe, closed on destruction: the per-install cancel pipe.
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
        /// inherits neither, and a write end that never blocks.
        /// @return Nothing, or why it could not be opened.
        [[nodiscard]] std::expected<void, std::string> Open()
        {
            if (::pipe(_ends.data()) != 0)
                return std::unexpected(std::format("cannot open a pipe: {}", std::generic_category().message(errno)));
            if (auto configured = Configure(Reader(), false); !configured.has_value())
                return configured;
            return Configure(Writer(), true);
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

    /// Wait on a cancel pipe and, when there is one, the stop pipe.
    ///
    /// Neither byte is ever read, which is what keeps both outcomes sticky: a readable pipe stays
    /// readable for every later wait.
    /// @param cancel The per-install cancel pipe's read end.
    /// @param stop The stop pipe's read end, or -1 for a signal that can never fire.
    /// @return Why the wait ended.
    [[nodiscard]] StopWake PollStop(int cancel, int stop)
    {
        auto fds = std::array<pollfd, 2> {
            pollfd { .fd = cancel, .events = POLLIN, .revents = 0 },
            pollfd { .fd = stop, .events = POLLIN, .revents = 0 },
        };
        // A negative descriptor is skipped by `poll(2)`, which is how the never-firing signal
        // waits on its cancel alone.
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

    class PosixStopSignal final: public BlockingStopSignal
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
            // The disposition first, then the slot. The stop pipe stays open -- see
            // `stopPipeRead`.
            static_cast<void>(::sigaction(SIGINT, &_previous, nullptr));
            stopClaimed.store(false, std::memory_order_release);
        }

        /// Open the cancel pipe, drain the process's stop pipe, and route SIGINT to it.
        /// @return Nothing, or why it could not be installed.
        [[nodiscard]] std::expected<void, std::string> Install()
        {
            if (auto opened = _cancel.Open(); !opened.has_value())
                return opened;

            auto claim = StopClaim {};
            if (!claim.Held())
                return std::unexpected(std::string { "a stop signal is already installed in this process" });
            if (auto created = EnsureStopPipe(); !created.has_value())
                return created;
            // A stop an earlier install heard is that install's, not this one's.
            DrainStopPipe();

            struct sigaction ours {};
            ours.sa_handler = &OnStopSignal;
            // Never `::sigemptyset`: the macOS SDK defines it as a MACRO, and a qualified name
            // expanded to an expression does not parse there.
            static_cast<void>(sigemptyset(&ours.sa_mask));
            // Restarted, so a SIGINT landing on some other thread's blocking call is not
            // turned into that call failing with EINTR.
            ours.sa_flags = SA_RESTART;
            if (::sigaction(SIGINT, &ours, &_previous) != 0)
                return std::unexpected(
                    std::format("cannot install a SIGINT handler: {}", std::generic_category().message(errno)));
            claim.Keep();
            _installed = true;
            return {};
        }

        void Cancel() noexcept override
        {
            // A cancel already pending fills the pipe as well as one byte does (`WriteWakeByte`).
            WakeOrAbort(_cancel.Writer());
        }

      private:
        [[nodiscard]] StopWake WaitBlocking() const override
        {
            return PollStop(_cancel.Reader(), stopPipeRead.load(std::memory_order_acquire));
        }

        Pipe _cancel;
        struct sigaction _previous {};
        bool _installed { false };
    };

    /// The signal for a process that inherited SIGINT ignored: it installs nothing and never fires.
    ///
    /// A job started in the background by a non-interactive shell, or under `nohup`, inherits
    /// SIGINT as `SIG_IGN` across exec. Catching it anyway would let a Ctrl-C meant for the
    /// foreground end this watch -- a changed death. So the disposition is left as it is, and only
    /// `Cancel()` ends a wait.
    class IgnoredStopSignal final: public BlockingStopSignal
    {
      public:
        /// Open the cancel pipe.
        /// @return Nothing, or why it could not be opened.
        [[nodiscard]] std::expected<void, std::string> Open()
        {
            return _cancel.Open();
        }

        void Cancel() noexcept override
        {
            // A cancel already pending fills the pipe as well as one byte does (`WriteWakeByte`).
            WakeOrAbort(_cancel.Writer());
        }

      private:
        [[nodiscard]] StopWake WaitBlocking() const override
        {
            return PollStop(_cancel.Reader(), -1);
        }

        Pipe _cancel;
    };

    /// Whether SIGINT is ignored as this process found it.
    /// @return True for `SIG_IGN`; false for anything else, or when the disposition cannot be read.
    [[nodiscard]] bool SigintIgnored() noexcept
    {
        struct sigaction current {};
        return ::sigaction(SIGINT, nullptr, &current) == 0 && current.sa_handler == SIG_IGN;
    }

    /// Install the process's stop signal, or leave an inherited ignore alone.
    /// @return The signal, or why it could not be installed.
    [[nodiscard]] std::expected<std::unique_ptr<IStopSignal>, std::string> InstallPlatformStopSignal()
    {
        if (SigintIgnored())
        {
            auto ignored = std::make_unique<IgnoredStopSignal>();
            if (auto opened = ignored->Open(); !opened.has_value())
                return std::unexpected(std::move(opened).error());
            return ignored;
        }

        auto installed = std::make_unique<PosixStopSignal>();
        if (auto outcome = installed->Install(); !outcome.has_value())
            return std::unexpected(std::move(outcome).error());
        return installed;
    }

#endif
} // namespace

std::expected<std::unique_ptr<IStopSignal>, std::string> InstallStopSignal()
{
    return InstallPlatformStopSignal();
}

std::intptr_t StopSignalHandlerEnd() noexcept
{
#if defined(_WIN32)
    auto const event = stopEvent.load(std::memory_order_acquire);
    return event == nullptr ? -1 : static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(event));
#else
    return stopPipeWrite.load(std::memory_order_acquire);
#endif
}

} // namespace FastCache
