// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/InheritedListener.hpp>

#include <charconv>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
    /// Variables systemd sets on a socket-activated service.
    constexpr std::string_view ListenPidVariable = "LISTEN_PID";
    constexpr std::string_view ListenFdsVariable = "LISTEN_FDS";
    /// Optional, and read by nobody here -- cleared alongside the others so a
    /// child cannot see a name list without the descriptors it refers to.
    constexpr std::string_view ListenFdNamesVariable = "LISTEN_FDNAMES";

    /// Parse a whole non-negative integer, rejecting trailing junk.
    ///
    /// Whole-string, not a prefix: `from_chars` stopping early would read "3x" as
    /// 3, and a value that is not exactly a number means the environment is not
    /// what this code thinks it is -- which is the moment to hand back nothing
    /// rather than to guess.
    [[nodiscard]] std::optional<std::uint64_t> ParseWholeNumber(std::string_view text) noexcept
    {
        if (text.empty())
            return std::nullopt;
        std::uint64_t value = 0;
        auto const* const begin = text.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(text.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::nullopt;
        return value;
    }

    /// Stop a descriptor from surviving an exec.
    ///
    /// systemd hands its descriptors over WITHOUT close-on-exec, deliberately, so
    /// that a service which re-execs itself keeps them. This process does the
    /// opposite kind of exec -- a compile worker spawns a compiler per job -- and
    /// a compiler holding the listening socket keeps the port alive after the
    /// worker exits, so the restart cannot bind and blames an address in use that
    /// nothing visible is using.
    void MarkCloseOnExec([[maybe_unused]] int descriptor) noexcept
    {
#if !defined(_WIN32)
        auto const flags = ::fcntl(descriptor, F_GETFD);
        if (flags >= 0)
            static_cast<void>(::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC));
#endif
    }

    /// Remove the activation variables from this process's environment.
    void ClearActivationEnvironment() noexcept
    {
#if defined(_WIN32)
        for (auto const name: { ListenPidVariable, ListenFdsVariable, ListenFdNamesVariable })
            static_cast<void>(::_putenv_s(std::string { name }.c_str(), ""));
#else
        for (auto const name: { ListenPidVariable, ListenFdsVariable, ListenFdNamesVariable })
            static_cast<void>(::unsetenv(std::string { name }.c_str()));
#endif
    }
} // namespace

ActivationHandoff ParseSocketActivation(std::optional<std::string_view> listenPid,
                                        std::optional<std::string_view> listenFds,
                                        std::uint64_t currentPid) noexcept
{
    if (!listenPid.has_value() || !listenFds.has_value())
        return {};

    auto const target = ParseWholeNumber(*listenPid);
    // Addressed to somebody else. The variables are inherited across fork and
    // exec, so this is the ordinary case for any grandchild of an activated
    // service -- and adopting on the strength of the variables alone would mean
    // treating whatever the parent left on descriptor 3 as a listening socket.
    if (!target.has_value() || *target != currentPid)
        return {};

    auto const count = ParseWholeNumber(*listenFds);
    if (!count.has_value() || *count == 0)
        return {};

    // A sanity bound rather than a real limit. systemd passes a handful; a value
    // in the thousands means the variable is not what it claims, and adopting
    // that many descriptors would close whatever this process legitimately had
    // open above fd 3.
    constexpr std::uint64_t MaxHandoff = 1024;
    if (*count > MaxHandoff)
        return {};

    return ActivationHandoff { .firstDescriptor = ActivationFirstDescriptor, .count = static_cast<int>(*count) };
}

std::vector<std::unique_ptr<IListener>> AdoptInheritedListeners(std::chrono::milliseconds acceptPoll,
                                                                std::chrono::milliseconds ioTimeout)
{
    std::vector<std::unique_ptr<IListener>> listeners;

#if !defined(_WIN32)
    auto const pid = ReadEnvironmentVariable(std::string { ListenPidVariable });
    auto const fds = ReadEnvironmentVariable(std::string { ListenFdsVariable });

    auto const handoff = ParseSocketActivation(pid.has_value() ? std::optional { std::string_view { *pid } } : std::nullopt,
                                               fds.has_value() ? std::optional { std::string_view { *fds } } : std::nullopt,
                                               static_cast<std::uint64_t>(::getpid()));

    for (int offset = 0; offset < handoff.count; ++offset)
    {
        int const descriptor = handoff.firstDescriptor + offset;
        MarkCloseOnExec(descriptor);
        auto adopted = BlockingListener::Adopt(static_cast<Detail::NativeSocket>(descriptor));
        // Not optional -- see the header. Without it this listener's accept loop
        // has no way to observe a shutdown on POSIX.
        adopted->SetTimeouts(acceptPoll, ioTimeout);
        listeners.emplace_back(std::move(adopted));
    }
#endif

    // Cleared unconditionally, including when nothing was adopted. A process that
    // decided the variables were not addressed to IT must still not pass them on:
    // the next child down would make the same wrong decision with a pid that now
    // matches by coincidence of reuse.
    ClearActivationEnvironment();

    return listeners;
}

} // namespace FastCache
