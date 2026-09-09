// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/BlockingSocket.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

namespace FastCache::Testing
{

/// A loopback client that leaves by RESET rather than by FIN.
///
/// **`ISocket` cannot express an abortive close, and should not.** The two
/// departures are different bytes on the wire -- FIN says *I have finished
/// sending*, RST says *this connection is gone* -- and a server tells them apart
/// through different arms: `WaitReadable` answers `0` for the first and an ERROR
/// for the second, on all three reactors. `Net/WaitReadable_test.cpp` is the
/// measurement of that, one abortive close per platform.
///
/// A fake that can be told to return an error proves nothing about either, because
/// the whole question is which of the two the KERNEL sends. So the close has to be
/// real, which means a raw handle: there is no `ISocket` verb for it, and adding one
/// would have exactly the consumers `ShutdownWrite` was added to avoid.
/// `ShutdownWrite` earned its place by making a rule about half-closing statable in
/// PRODUCTION; nothing in production wants to reset its own peer, since the shape of
/// a close belongs to the client.
///
/// **Shared because a second consumer arrived, and because drift here is SILENT.**
/// Two files need an abortive close as of this change --
/// `Net/WaitReadable_test.cpp`, which measures what the primitive reports, and
/// `apps/fastcache-compile-node/FrameEndpoint_test.cpp`, which needs the surface to
/// notice a reset peer. The hazard is not maintenance, and it is a property of the
/// code rather than a story about its history: **a consumer's case stays GREEN when
/// this stops working.** A `SO_LINGER` that stops taking degrades the close to an
/// ordinary FIN; the EOF arm answers instead of the error arm; the departure is
/// still counted; the assertion still passes. `FrameEndpoint_test.cpp` cannot
/// distinguish those by construction -- both of `WatchPeer`'s arms reach the same
/// `RecordDeparture`, which is #817, closed as refuted. One implementation is what
/// makes `WaitReadable_test.cpp`'s assertion -- that this really does report an
/// ERROR rather than a `0` -- the guard for every other user.
///
/// It builds on `Net::Detail` rather than its own platform aliases: `NativeSocket`,
/// `InvalidSocket`, `CloseNativeSocket`, `EnsureNetworkInitialised` and
/// `ArmNoSigPipe` are that header's answers to the same questions, and two of them
/// are not optional.
///
/// **`EnsureNetworkInitialised` because Winsock** returns `INVALID_SOCKET` with
/// `WSANOTINITIALISED` for a `::socket` call that precedes it, so a client that
/// omits it works only for as long as something else happens to have bound a
/// listener first.
///
/// **`ArmNoSigPipe` because macOS**, where `SO_NOSIGPIPE` is defined and
/// `NoSignalSendFlags()` is therefore `0` -- the two are alternatives, not
/// belt-and-braces, and on that platform the suppression lives ENTIRELY in the
/// socket option. Sending to a peer that has already gone would raise SIGPIPE and
/// abort the whole Catch2 process: a crash rather than a failed assertion, on the
/// one platform where this client's own subject makes a dead peer likely.
class AbortiveClient
{
  public:
    /// Dial loopback.
    /// @param port The port to connect to.
    explicit AbortiveClient(std::uint16_t port):
        _fd { Dial(port) }
    {
    }

    AbortiveClient(AbortiveClient const&) = delete;
    AbortiveClient(AbortiveClient&&) = delete;
    AbortiveClient& operator=(AbortiveClient const&) = delete;
    AbortiveClient& operator=(AbortiveClient&&) = delete;

    /// Closes gracefully if `Reset` was never called, so an abandoned client is still
    /// tidied. The guard is needed BECAUSE `Reset` invalidates the handle.
    ~AbortiveClient()
    {
        if (_fd != Detail::InvalidSocket)
            Detail::CloseNativeSocket(_fd);
    }

    /// @return True when the connect succeeded.
    [[nodiscard]] bool Connected() const noexcept
    {
        return _fd != Detail::InvalidSocket;
    }

    /// Write a whole frame, looping over short sends.
    ///
    /// `EINTR` is retried rather than reported: a signal arriving mid-send is not a
    /// broken connection, and sanitizer runs and profilers deliver them.
    /// @param frame The bytes to send, header included.
    /// @return True when every byte reached the kernel.
    [[nodiscard]] bool Send(std::span<std::byte const> frame) const
    {
        std::size_t sent = 0;
        while (sent < frame.size())
        {
            // `SendLen` rather than a literal cast: Winsock's `send` takes an `int`
            // length and POSIX's takes a `size_t`, so one spelling is a sign
            // conversion on whichever platform it is not written for.
            auto const chunk = ::send(static_cast<RawHandle>(_fd),
                                      reinterpret_cast<char const*>(frame.data()) + sent,
                                      static_cast<SendLen>(frame.size() - sent),
                                      Detail::NoSignalSendFlags());
            if (chunk > 0)
            {
                sent += static_cast<std::size_t>(chunk);
                continue;
            }
#if !defined(_WIN32)
            if (chunk < 0 && errno == EINTR)
                continue;
#endif
            return false;
        }
        return true;
    }

    /// Leave abruptly: RST rather than FIN.
    ///
    /// Zero linger on close is what turns one into the other, on both stacks.
    ///
    /// **The `setsockopt` result is REPORTED rather than ignored**, because silently
    /// failing to arm it is exactly the degradation this class's own doc names: the
    /// close becomes an ordinary FIN and every consumer stays green. A caller that
    /// discards this is asserting about a departure shape it did not get.
    /// @return True when the socket was armed for an abortive close and shut.
    [[nodiscard]] bool Reset() noexcept
    {
        if (_fd == Detail::InvalidSocket)
            return false;
        linger const abortive { .l_onoff = 1, .l_linger = 0 };
        // `reinterpret_cast`, not a hop through `void const*`: the two-step is what
        // `bugprone-casting-through-void` refuses, and setsockopt's `char const*` on
        // Winsock against `void const*` on POSIX is exactly the shape that invites it.
        auto const armed = ::setsockopt(static_cast<RawHandle>(_fd),
                                        SOL_SOCKET,
                                        SO_LINGER,
                                        reinterpret_cast<OptValue>(&abortive),
                                        static_cast<int>(sizeof(abortive)))
                           == 0;
        Detail::CloseNativeSocket(_fd);
        _fd = Detail::InvalidSocket;
        return armed;
    }

  private:
#if defined(_WIN32)
    using RawHandle = SOCKET;
    using OptValue = char const*;
    using SendLen = int;
#else
    using RawHandle = int;
    using OptValue = void const*;
    using SendLen = std::size_t;
#endif

    /// Connect to loopback, or answer `InvalidSocket`.
    ///
    /// Static, so the handle is a member INITIALIZER rather than an assignment in a
    /// constructor body.
    /// @param port The port to dial.
    /// @return The connected handle, or `Detail::InvalidSocket` when a step failed.
    [[nodiscard]] static Detail::NativeSocket Dial(std::uint16_t port) noexcept
    {
        Detail::EnsureNetworkInitialised();
        auto const fd = static_cast<Detail::NativeSocket>(::socket(AF_INET, SOCK_STREAM, 0));
        if (fd == Detail::InvalidSocket)
            return Detail::InvalidSocket;
        Detail::ArmNoSigPipe(fd);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(static_cast<RawHandle>(fd), reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) == 0)
            return fd;
        Detail::CloseNativeSocket(fd);
        return Detail::InvalidSocket;
    }

    Detail::NativeSocket _fd { Detail::InvalidSocket };
};

} // namespace FastCache::Testing
