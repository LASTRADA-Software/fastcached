// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache
{

/// Splitting `host:port` text, and the one subtlety in it.
///
/// ## Why this is shared rather than written where it is needed
///
/// An IPv6 literal contains colons, so the obvious `rfind(':')` splits `::1:6674`
/// at the wrong one and hands back a host of `::1:` — or, worse, succeeds with a
/// plausible-looking wrong answer. The bracketed form `[::1]:6674` is what the
/// grammar exists for, and getting it right is four lines that every caller would
/// otherwise write for itself.
///
/// `fastcache-cc`'s `TcpClient.cpp` had the only correct copy, file-local. A
/// second caller — the worker's `--admin-listen` — is what turned "a private
/// helper" into "a rule with two authors", which is the shape this codebase
/// treats as a defect rather than a coincidence.
///
/// ## Why `Core/` and header-only
///
/// The same constraint `WireFields` and `Cli/Options` are under: `fastcache-cc`
/// compiles against these without linking `FastCache`, so anything needing a
/// translation unit would break the launcher's *link* rather than merely its
/// build. Nothing here touches a socket — it is text in, text out — so there is
/// nothing to link.

/// Split `host:port` into its parts, honouring the bracketed IPv6 form.
///
/// @param hostPort The endpoint text, e.g. `127.0.0.1:6674` or `[::1]:6674`.
/// @return `(host, port)` as text, or nullopt when no port is present.
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>> SplitHostPort(std::string_view hostPort)
{
    if (!hostPort.empty() && hostPort.front() == '[')
    {
        auto const close = hostPort.find(']');
        if (close == std::string_view::npos || close + 1 >= hostPort.size() || hostPort[close + 1] != ':')
            return std::nullopt;
        return std::pair { std::string { hostPort.substr(1, close - 1) }, std::string { hostPort.substr(close + 2) } };
    }

    auto const colon = hostPort.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= hostPort.size())
        return std::nullopt;
    return std::pair { std::string { hostPort.substr(0, colon) }, std::string { hostPort.substr(colon + 1) } };
}

/// Parse a TCP port from text.
///
/// Named `ParseTcpPort` and not `ParsePort` because `Config/CliParser` already
/// owns that name in this namespace, with the same parameter list and a different
/// return type (`std::expected<std::uint16_t, ConfigError>`, which the CLI needs
/// so it can tell "not a number" from "out of range"). That is **not an overload**
/// and the compiler cannot say so: each translation unit sees only one of the two
/// declarations, and the Itanium ABI does not encode a return type in a free
/// function's mangled name -- so both definitions claim the identical symbol, the
/// linker keeps the strong one, and every caller of the header's `inline` version
/// silently reaches the other. It reads an `expected` as an `optional` and
/// segfaults. **MSVC hides this**: its mangling does include the return type, so
/// the same tree links and passes on Windows and crashes on Linux, which is how it
/// was found -- 1730 green MSVC tests and two ASan SIGSEGVs.
///
/// Rejects a trailing remainder rather than stopping at it, so `6674x` is an
/// error instead of port 6674 — a listener silently bound somewhere other than
/// where the operator wrote is the kind of no-op this codebase keeps recording.
/// Port 0 is refused too: it means "any free port" to the kernel, which for a
/// scrape or dispatch endpoint is an address nobody can be told in advance.
/// @param text The port digits.
/// @return The port, or nullopt when it is not a usable one.
[[nodiscard]] inline std::optional<std::uint16_t> ParseTcpPort(std::string_view text)
{
    auto value = 0U;
    // Spelled inline rather than through a hoisted `end`, which is both this
    // codebase's own idiom everywhere else and what keeps the size visibly paired
    // with the pointer -- `bugprone-suspicious-stringview-data-usage` reads a
    // hoisted one as a `data()` handed off with no length at all.
    auto const parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    if (value == 0 || value > 65535U)
        return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

/// Whether a host names this machine over the loopback interface.
///
/// The one test for "is this caller on the same machine as me", spelled once
/// because two layers now ask it and they must not be able to disagree: a peer the
/// membership oracle counts as local while the cache surface does not would be
/// admitted to the fleet and refused its objects, or the reverse.
///
/// Textual rather than a `SocketAddress` comparison, because what a caller has at
/// these two sites is what `ISocket::PeerAddress()` reports — a host string. The
/// spellings recognised are the ones a kernel actually produces for a loopback
/// connection: `127.0.0.0/8` in any of its forms, IPv6 `::1`, and the
/// IPv4-mapped `::ffff:127.x.x.x` a dual-stack listener reports for an IPv4 client.
/// The literal name `localhost` is **not** among them: it is whatever a resolver
/// says it is, and a resolver is not something a security decision may depend on.
/// @param host The peer's host, without a port or brackets.
/// @return True when the peer is on this machine.
[[nodiscard]] inline bool IsLoopbackHost(std::string_view host) noexcept
{
    // Any 127.x.x.x, not 127.0.0.1 alone: the whole /8 is loopback, and a client
    // bound to 127.0.0.2 is no less local for it.
    constexpr std::string_view V4Prefix = "127.";
    constexpr std::string_view MappedPrefix = "::ffff:";

    if (host == "::1")
        return true;
    if (host.starts_with(V4Prefix))
        return true;
    if (host.starts_with(MappedPrefix))
        return host.substr(MappedPrefix.size()).starts_with(V4Prefix);
    return false;
}

/// Split an endpoint that may name only a port.
///
/// A bare port means `defaultHost`, which callers set to loopback: an endpoint
/// reachable from the network is an operator's decision, and defaulting to the
/// wildcard address would make it an accident.
/// @param text `port`, `host:port`, or `[v6]:port`.
/// @param defaultHost What a bare port binds to.
/// @return `(host, port)`, or nullopt when the text names no usable port.
[[nodiscard]] inline std::optional<std::pair<std::string, std::uint16_t>> ParseEndpoint(std::string_view text,
                                                                                        std::string_view defaultHost)
{
    if (auto const split = SplitHostPort(text); split.has_value())
    {
        auto const port = ParseTcpPort(split->second);
        if (!port.has_value())
            return std::nullopt;
        return std::pair { split->first, *port };
    }

    auto const port = ParseTcpPort(text);
    if (!port.has_value())
        return std::nullopt;
    return std::pair { std::string { defaultHost }, *port };
}

} // namespace FastCache
