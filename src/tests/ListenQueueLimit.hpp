// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#if defined(__linux__)
    #include <sys/socket.h>

    #include <charconv>
    #include <filesystem>
    #include <fstream>
    #include <string>
    #include <system_error>

    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

namespace FastCache::Testing
{

/// The accept-queue limits the KERNEL recorded for every listening TCP socket this process holds
/// on @p port -- what a listener actually asked `::listen` for, read back after the fact, so a case
/// asserts the production opener's wiring rather than the value a helper says it would pass.
///
/// Linux answers it: a listening socket's `TCP_INFO` aliases `tcpi_sacked` to `sk_max_ack_backlog`,
/// the limit `::listen` set after the kernel's own clamp to `net.core.somaxconn`. The sockets are
/// found by walking `/proc/self/fd` and asking each socket descriptor for its port and whether it
/// listens, since the openers under test keep their listener to themselves.
///
/// Several sockets can answer one port: a dual-stack bind is one, a `PortSharing::Shared` listener
/// per loop is several. Every one is reported, so a case can assert they ALL took the depth.
/// @param port The port the listener is bound to, as it reports it.
/// @return One limit per listening socket on @p port (empty when none is found), or nothing on a
///         platform that cannot be asked -- which is not the same as a socket that took no depth.
[[nodiscard]] inline std::optional<std::vector<int>> ListenQueueLimitsOnPort(std::uint16_t port)
{
#if defined(__linux__)
    std::vector<int> limits;
    std::error_code ignored;
    for (auto const& entry: std::filesystem::directory_iterator { "/proc/self/fd", ignored })
    {
        auto const name = entry.path().filename().string();
        int fd = -1;
        if (std::from_chars(name.data(), name.data() + name.size(), fd).ec != std::errc {})
            continue;

        int listening = 0;
        auto length = static_cast<socklen_t>(sizeof(listening));
        if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length) != 0 || listening == 0)
            continue;

        sockaddr_storage address {};
        auto addressLength = static_cast<socklen_t>(sizeof(address));
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0)
            continue;
        if (address.ss_family != AF_INET && address.ss_family != AF_INET6)
            continue;
        auto const boundPort = address.ss_family == AF_INET6
                                   ? ntohs(reinterpret_cast<sockaddr_in6 const*>(&address)->sin6_port)
                                   : ntohs(reinterpret_cast<sockaddr_in const*>(&address)->sin_port);
        if (boundPort != port)
            continue;

        tcp_info info {};
        auto infoLength = static_cast<socklen_t>(sizeof(info));
        if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &infoLength) != 0)
            continue;
        limits.push_back(static_cast<int>(info.tcpi_sacked));
    }
    return limits;
#else
    static_cast<void>(port);
    return std::nullopt;
#endif
}

/// The ceiling the kernel clamps every `::listen` backlog to, or nothing where it cannot be read.
///
/// A case that asserts a DEEP queue has to know it: on a kernel whose ceiling is at or below
/// core-cpp's default backlog of 128, a listener that asked for depth and one that asked for
/// nothing record the same limit, and the case cannot tell them apart.
/// @return `net.core.somaxconn` on Linux; nothing elsewhere or when it does not parse.
[[nodiscard]] inline std::optional<int> KernelListenCeiling()
{
#if defined(__linux__)
    std::ifstream in { "/proc/sys/net/core/somaxconn" };
    int ceiling = 0;
    if (in >> ceiling && ceiling > 0)
        return ceiling;
#endif
    return std::nullopt;
}

} // namespace FastCache::Testing
