// SPDX-License-Identifier: Apache-2.0
#pragma once

// A do-nothing signal handler, installed for the length of a scope, so a test can interrupt a
// thread parked in a system call the way a daemon's own SIGHUP handler does.
//
// POSIX only: Windows delivers no signal to a thread, and every caller skips there.

#if !defined(_WIN32)

    #include <csignal>

    #include <pthread.h>

namespace FastCache::Testing
{

/// Installs a handler that does nothing for one signal, and restores the previous disposition when
/// it goes out of scope.
///
/// What matters is that a handler is INSTALLED: a caught signal interrupts a parked `poll()` with
/// `EINTR`, where an ignored one does nothing and a default one ends the process. `SA_RESTART` is
/// left off: `poll()` is never restarted either way (glibc's `std::signal` sets it, and a SIGHUP
/// still interrupts the admin poll), and without it an interrupted `accept()` reports `EINTR` too,
/// which is the stricter case.
class ScopedSignalHandler
{
  public:
    /// @param signal The signal to catch, e.g. `SIGUSR2`. Pick one no production code installs.
    explicit ScopedSignalHandler(int signal) noexcept:
        _signal { signal }
    {
        struct sigaction action {};
        action.sa_handler = &Ignore;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        _installed = ::sigaction(_signal, &action, &_previous) == 0;
    }

    ScopedSignalHandler(ScopedSignalHandler const&) = delete;
    ScopedSignalHandler(ScopedSignalHandler&&) = delete;
    ScopedSignalHandler& operator=(ScopedSignalHandler const&) = delete;
    ScopedSignalHandler& operator=(ScopedSignalHandler&&) = delete;

    ~ScopedSignalHandler()
    {
        if (_installed)
            ::sigaction(_signal, &_previous, nullptr);
    }

    /// @return Whether the handler was installed; a case asserts it before relying on it.
    [[nodiscard]] bool Installed() const noexcept
    {
        return _installed;
    }

    /// Deliver the signal to one thread.
    /// @param thread The thread to interrupt.
    void Interrupt(pthread_t thread) const noexcept
    {
        ::pthread_kill(thread, _signal);
    }

  private:
    static void Ignore(int /*signal*/) noexcept {}

    int _signal;
    struct sigaction _previous {};
    bool _installed { false };
};

} // namespace FastCache::Testing

#endif
