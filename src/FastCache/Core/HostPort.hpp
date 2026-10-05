// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
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

/// A host with the IPv4-mapped IPv6 prefix taken off, if it had one.
///
/// A dual-stack listener reports an IPv4 client as `::ffff:10.0.0.1`, so one
/// machine has two spellings and which one a caller sees depends on how the
/// listener was bound rather than on anything about the peer. Every comparison
/// against a peer's host therefore has to fold them together first, or the answer
/// differs between two nodes that are configured identically.
///
/// Spelled here rather than at each comparison, on the header's own argument: a
/// rule with two authors is a defect rather than a coincidence.
///
/// Returns a view **into** @p host, so the argument must outlive the result.
/// @param host The peer's host, without a port or brackets.
/// @return The unmapped host, or @p host unchanged when it was not mapped.
[[nodiscard]] inline std::string_view UnmappedHost(std::string_view host) noexcept
{
    constexpr std::string_view MappedPrefix = "::ffff:";
    return host.starts_with(MappedPrefix) ? host.substr(MappedPrefix.size()) : host;
}

/// The host part of an endpoint, keeping a bare host whole.
///
/// The other half of comparing an advertised endpoint against a peer, and the same
/// argument puts it here: this rule already had three authors before it was named
/// -- a host-list admission oracle since retired, `AdvertisesWildcard`, and the scheduler's
/// endpoint check (#242) -- each re-deriving that an endpoint which will not split
/// is a legitimate bare host rather than a parse failure. Dropping such a host
/// instead is how a member the set cannot represent silently stops being one.
///
/// A **view**, unlike `SplitHostPort`, which materialises two `std::string`s: a
/// caller that only wants the host was allocating twice per call to read one of
/// them back out, and the port it paid for is the one thing a peer's address never
/// carries.
///
/// Returns a view **into** @p endpoint, so the argument must outlive the result.
/// @param endpoint `host:port`, `[v6]:port`, or a bare host.
/// @return The host, or the whole of @p endpoint when there is no port to split off.
[[nodiscard]] inline std::string_view HostOfEndpoint(std::string_view endpoint) noexcept
{
    if (!endpoint.empty() && endpoint.front() == '[')
    {
        auto const close = endpoint.find(']');
        if (close != std::string_view::npos)
            return endpoint.substr(1, close - 1);
        return endpoint;
    }

    auto const colon = endpoint.rfind(':');
    // An IPv6 literal is all colons and no brackets, so a bare `2001:db8::1` must not
    // be split at its last one -- that yields a "host" of `2001:db8:` and a "port" of
    // `1`, which is a plausible-looking wrong answer rather than a failure.
    if (colon == std::string_view::npos || endpoint.find(':') != colon)
        return endpoint;
    return endpoint.substr(0, colon);
}

/// Whether two hosts name the same machine.
///
/// One spelling, because the alternative is what this codebase keeps paying for: a
/// comparison that folds the IPv4-mapped form beside one that does not answers
/// differently about the same machine at the same instant, and which surface a peer
/// meets then decides what it is.
///
/// An empty host matches **nothing**, the empty host included. That is the direction
/// an unidentifiable peer has to fail in -- it is what `core::net::formatPeerAddress` answers
/// for a peer whose family is unknown or whose `getpeername` failed, and two
/// unanswerable questions are not a match.
///
/// The emptiness test is on the **folded** host rather than the argument, or a bare
/// `::ffff:` -- which unmaps to nothing -- would match the empty host and hand an
/// unnameable peer the answer the rule above exists to deny it.
/// @param left One host, without a port or brackets.
/// @param right The other.
/// @return True when they name the same machine.
[[nodiscard]] inline bool SameHost(std::string_view left, std::string_view right) noexcept
{
    auto const bare = UnmappedHost(left);
    return !bare.empty() && bare == UnmappedHost(right);
}

namespace Detail
{
    /// @param text One dotted-quad octet.
    /// @return Its value, or nullopt when it is not one to three decimal digits naming 0..255.
    [[nodiscard]] inline std::optional<unsigned> ParseOctet(std::string_view text) noexcept
    {
        constexpr std::size_t MaxDigits = 3;
        constexpr unsigned MaxOctet = 255;
        if (text.empty() || text.size() > MaxDigits)
            return std::nullopt;
        auto value = 0U;
        for (auto const c: text)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            value = (value * 10U) + static_cast<unsigned>(c - '0');
        }
        return value <= MaxOctet ? std::optional { value } : std::nullopt;
    }

    /// @param text A host.
    /// @return Whether it is an IPv4 LITERAL -- exactly four decimal octets -- inside 127.0.0.0/8.
    [[nodiscard]] inline bool IsIpv4LoopbackLiteral(std::string_view text) noexcept
    {
        constexpr std::size_t Octets = 4;
        constexpr unsigned LoopbackNet = 127;
        auto seen = std::size_t { 0 };
        for (auto const part: std::views::split(text, '.'))
        {
            auto const octet = ParseOctet(std::string_view { part.begin(), part.end() });
            if (!octet.has_value() || (seen == 0 && *octet != LoopbackNet))
                return false;
            ++seen;
        }
        return seen == Octets;
    }
} // namespace Detail

/// Whether a host names this machine over the loopback interface.
///
/// The one test for "is this caller on the same machine as me", spelled once
/// because two layers now ask it and they must not be able to disagree: a peer the
/// membership oracle counts as local while the cache surface does not would be
/// admitted to the fleet and refused its objects, or the reverse.
///
/// Textual rather than a `SocketAddress` comparison, because what a caller has at
/// these two sites is what `core::net::ISocket::PeerAddress()` reports — a host string. The
/// spellings recognised are the ones a kernel actually produces for a loopback
/// connection: `127.0.0.0/8` in any of its forms, IPv6 `::1`, and the
/// IPv4-mapped `::ffff:127.x.x.x` a dual-stack listener reports for an IPv4 client.
/// The literal name `localhost` is **not** among them: it is whatever a resolver
/// says it is, and a resolver is not something a security decision may depend on.
///
/// **An IP LITERAL, parsed, and never a NAME.** This matched `127.` as a PREFIX, which a
/// name satisfies too: `127.cache.example.com` read as loopback, so a bind spelled that
/// way was judged unreachable from the network whatever it resolved to -- the fail-OPEN
/// direction for every rule asking whether a port faces other machines. Now the host
/// is four decimal octets inside `127.0.0.0/8`, or `::1`, or the mapped form of the
/// first; anything else, every name included, is not loopback, so every decision this
/// answers fails CLOSED.
///
/// An **empty** host is not local either, and that direction is deliberate. It is
/// what `core::net::formatPeerAddress` answers for a peer it could not identify — a family it
/// does not know, or a `getpeername` that failed — and a caller this machine cannot
/// name must not be handed its CPU. The one shape that is genuinely local and
/// reports nothing is a Unix-domain socket, which is unreachable here: the node
/// listens on TCP, and a socket-activated one is *required* to `--advertise` a
/// host:port, so a Unix socket would already be a worker no client could dial.
/// @param host The peer's host, without a port or brackets.
/// @return True when the peer is on this machine.
[[nodiscard]] inline bool IsLoopbackHost(std::string_view host) noexcept
{
    // Unmapped first, so `::ffff:127.0.0.1` and `127.0.0.1` take the same branch
    // rather than each needing one. `::1` is not a mapped form and survives it
    // unchanged, which is why the equality still holds. Any address in 127.0.0.0/8,
    // not 127.0.0.1 alone: the whole /8 is loopback, and a client bound to 127.0.0.2
    // is no less local for it.
    auto const bare = UnmappedHost(host);
    return bare == "::1" || Detail::IsIpv4LoopbackLiteral(bare);
}

/// @param left One name.
/// @param right The other.
/// @return True when they differ in ASCII case at most, which is how DNS compares names.
///         Locale-free on purpose.
[[nodiscard]] constexpr bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right) noexcept
{
    constexpr auto lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return std::ranges::equal(left, right, [lower](char a, char b) { return lower(a) == lower(b); });
}

/// Whether a host NAME reaches this machine and no other, wherever it is resolved.
///
/// **A different question from `IsLoopbackHost`, and deliberately a second predicate rather than
/// a wider first one.** That one decides who is ADMITTED, from what a kernel reports, and must not
/// take a name a resolver answers. This one decides what this node may TELL A PEER to dial, and
/// there a name matters precisely because the peer resolves it: `localhost` and every name under
/// `.localhost` resolve to the loopback address on every machine (RFC 6761, section 6.3), so a
/// peer told to dial one reaches ITSELF -- confidently, with no error at either end. The loopback
/// literals `IsLoopbackHost` knows are included.
///
/// Compared without regard to ASCII case, and a single trailing root dot is ignored.
/// @param host A host, without a port or brackets.
/// @return True when every machine resolving @p host reaches itself.
[[nodiscard]] inline bool NamesOnlyThisMachine(std::string_view host) noexcept
{
    if (IsLoopbackHost(host))
        return true;
    if (host.ends_with('.'))
        host.remove_suffix(1);

    constexpr std::string_view Localhost = "localhost";
    if (EqualsIgnoringAsciiCase(host, Localhost))
        return true;
    return host.size() > Localhost.size() + 1 && host[host.size() - Localhost.size() - 1] == '.'
           && EqualsIgnoringAsciiCase(host.substr(host.size() - Localhost.size()), Localhost);
}

/// Whether @p host names NO ONE machine, because every machine answers to it: a host
/// `NamesOnlyThisMachine` answers for, or a wildcard.
///
/// **The one rule for "an audience a ticket may name"**, asked by the node that mints a ticket, the
/// node that spends one (`Distributed::AudienceNamesOneMachine`) and the launcher deciding whether
/// to ask for one at all (`Cc::ChooseCredential`). Every node is its own loopback and no node is the
/// wildcard, so a ticket naming one would be spendable at any node that heard it presented -- the
/// minter refuses it, and a launcher that asked anyway would move the minter's refusal counter for
/// every exchange. Here, header-only, because the launcher does not link the library.
///
/// The loopback NAMES count though `IsLoopbackHost` does not: this is not a question about where a
/// caller IS, which a name must never answer, but about whether a name could single out one machine,
/// and RFC 6761 reserves `localhost` and every name under `.localhost` to resolve to loopback
/// everywhere -- the same reading `NamesOnlyThisMachine` gives an advertised endpoint, so the two
/// questions cannot disagree about a name.
/// @param host A host, without a port or brackets.
/// @return True when every machine would answer to it.
[[nodiscard]] inline bool NamesNoOneMachine(std::string_view host) noexcept
{
    auto const unmapped = UnmappedHost(host);
    return NamesOnlyThisMachine(unmapped) || unmapped == "0.0.0.0" || unmapped == "::";
}

/// Whether a host is link-local: scoped to the interface it was observed on, not to any
/// particular machine.
///
/// A link-local address is zone-less text once it leaves the socket it came from -- `fe80::1`
/// names a different machine on every link -- so it is scoped to the SCHEDULER's own link, never
/// to the client dialling the hint. Handing it out anyway would spend a lease's dial-hint budget
/// on an address the client cannot reach, which is exactly the failure mode a stale DNS record
/// was supposed to avoid. It shows up more than the range alone would suggest: mDNS `.local`
/// names commonly resolve to one, and a NAT'd or bridged host often has no other address on the
/// interface the scheduler heard it from.
///
/// IPv4's link-local range is `169.254.0.0/16`, unmapped first as `IsLoopbackHost` does. IPv6's
/// is `fe80::/10` -- the ten most significant bits fixed -- which is not a textual prefix a
/// `starts_with` can spell: the range covers every address whose first 16-bit group, read as a
/// number, falls in `[0xfe80, 0xfebf]`, so that group is parsed and compared numerically rather
/// than pattern-matched. A zone id (`fe80::1%eth0`) sits after the address and never inside its
/// first group, so it does not need stripping first.
/// @param host A host, without a port or brackets; a zone id, if any, is ignored.
/// @return True for an IPv4 or IPv6 link-local address.
[[nodiscard]] inline bool IsLinkLocalHost(std::string_view host) noexcept
{
    constexpr std::string_view V4Prefix = "169.254.";
    auto const bare = UnmappedHost(host);
    if (bare.starts_with(V4Prefix))
        return true;

    auto const colon = bare.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 4)
        return false;
    auto const firstGroup = bare.substr(0, colon);
    unsigned value = 0;
    auto const [end, error] = std::from_chars(firstGroup.data(), firstGroup.data() + firstGroup.size(), value, 16);
    if (error != std::errc {} || end != firstGroup.data() + firstGroup.size())
        return false;
    return value >= 0xfe80 && value <= 0xfebf;
}

/// Whether a host is an IP address written out rather than a name to resolve.
///
/// TEXTUAL and pure, deliberately not `inet_pton`: it decides whether a DNS record could
/// be stale, which is a question about the SPELLING. A colon means an IPv6 literal -- a
/// DNS name never carries one -- and otherwise four dot-separated decimal octets, each
/// 0-255 in at most three digits. The IPv4-mapped form is unmapped first.
/// @param host A host, without a port or brackets.
/// @return True for an IPv4 or IPv6 literal.
[[nodiscard]] inline bool IsIpLiteralHost(std::string_view host) noexcept
{
    auto const bare = UnmappedHost(host);
    if (bare.contains(':'))
        return true;
    std::size_t octets = 0;
    for (auto const part: std::views::split(bare, '.'))
    {
        auto const text = std::string_view { part.begin(), part.end() };
        unsigned value = 0;
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || text.size() > 3 || error != std::errc {} || end != text.data() + text.size() || value > 255)
            return false;
        ++octets;
    }
    return octets == 4;
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

/// Parse an endpoint that is about to be DIALLED.
///
/// Stricter than `SplitHostPort` and narrower than `ParseEndpoint`, and each of the
/// three refusals has already been a bug somewhere in this tree:
///
/// - **Splitting is not parsing.** `SplitHostPort` takes the LAST colon and hands
///   back whatever follows it, so `no leader: try again` splits contentedly into a
///   host and a port of `" try again"`. That matters because a `NotLeader` refusal
///   carries the leader's endpoint *as its message*, and a client follows it (#237)
///   -- so a sentence that merely splits would be dialled as an address.
/// - **An empty host is the same misconfiguration by a different spelling.**
///   `:6674` and `[]:6674` both split cleanly and name nobody.
/// - **A bare port is not an endpoint here.** `ParseEndpoint` supplies a default
///   host, which is right for a *bind* address an operator typed and wrong for text
///   naming somewhere else to ask: a scheduler answering `6675` would send the
///   client back to itself.
///
/// One author rather than two, which is this header's own argument. "May I dial
/// this?" (`Cc::DialEndpoint`) and "is this refusal an instruction?"
/// (`Cc::RedirectTarget`) must answer identically: a redirect the second accepts and
/// the first refuses spends a hop on a guaranteed transport failure, and the reverse
/// is a leader the client can reach and declines to.
/// @param text `host:port`, or `[v6]:port`.
/// @return `(host, port)`, or nullopt when this is not something to dial.
[[nodiscard]] inline std::optional<std::pair<std::string, std::uint16_t>> ParseDialEndpoint(std::string_view text)
{
    auto const split = SplitHostPort(text);
    if (!split.has_value() || split->first.empty())
        return std::nullopt;
    auto const port = ParseTcpPort(split->second);
    if (!port.has_value())
        return std::nullopt;
    return std::pair { split->first, *port };
}

/// Whether @p host names every interface rather than a machine.
///
/// The two spellings of "every interface", plus the empty host -- which reaches `getaddrinfo`
/// as nullptr under AI_PASSIVE and is therefore the wildcard as well, the case
/// `--listen-node=:6674` is refused for. Brackets are the caller's to strip (`HostOfEndpoint`
/// does), so `[::]` arrives here as `::`.
/// @param host A host, unbracketed.
/// @return True for a wildcard.
[[nodiscard]] constexpr bool IsWildcardHost(std::string_view host) noexcept
{
    return host.empty() || host == "0.0.0.0" || host == "::";
}

/// Whether @p endpoint is one ANOTHER machine may be told to dial to reach this one: an endpoint
/// `ParseDialEndpoint` accepts, whose host singles out one machine -- neither a name that reaches
/// only the dialler's own machine nor a wildcard, which is `NamesNoOneMachine`'s question, asked
/// rather than restated so a ticket's audience and a member's record cannot disagree about a host.
///
/// **The one rule for an endpoint that is RECORDED or STATED for peers**, asked wherever one is
/// produced -- what a joiner states in `Enroll`, what a member announces, what a leader records for
/// itself -- and again where the record is decided (`Cluster::Validate`), so no route into a member
/// record accepts what another refuses. A peer told to dial `127.0.0.1:6674`, `localhost:6674` or
/// `0.0.0.0:6674` reaches ITSELF, confidently and with no error at either end, which is worse than
/// being told nothing.
/// @param endpoint `host:port`, or `[v6]:port`.
/// @return True when a peer dialling it could reach this machine.
[[nodiscard]] inline bool IsPeerDialableEndpoint(std::string_view endpoint)
{
    auto const dial = ParseDialEndpoint(endpoint);
    return dial.has_value() && !NamesNoOneMachine(dial->first);
}

/// @p endpoint when another machine may be told to dial it (`IsPeerDialableEndpoint`), else empty.
///
/// Empty rather than the spelling, because a record's empty endpoint is the stated "has none" and
/// every reader treats it so, while a loopback or wildcard one is dialled.
/// @param endpoint What this node would state.
/// @return The endpoint to state or record, or empty.
[[nodiscard]] inline std::string PeerDialableOrNone(std::string_view endpoint)
{
    return IsPeerDialableEndpoint(endpoint) ? std::string { endpoint } : std::string {};
}

/// Join a host and a port into text `SplitHostPort` reads back.
///
/// The inverse of the parser, and it lives beside it for the reason the parser
/// lives here at all: a v6 host has to be bracketed or the next `rfind(':')` takes
/// the wrong colon, and a rule spelled at each caller is a rule that comes to
/// disagree with itself. Two places already spelled it -- `UdpSocket` for what a
/// datagram's sender is called, `ServiceControl` for what goes into a registered
/// command line -- which is precisely the count at which this file's own header
/// comment says a private helper has become a rule with two authors.
///
/// A host that is already bracketed is left alone, so joining what `SplitHostPort`
/// produced and joining what an operator typed give the same answer.
/// @param host The host, bracketed or not.
/// @param port The port, as text.
/// @return `host:port`, with the host bracketed when it needs to be.
[[nodiscard]] inline std::string FormatHostPort(std::string_view host, std::string_view port)
{
    auto const bracketed = host.contains(':') && !host.starts_with('[');
    return bracketed ? std::format("[{}]:{}", host, port) : std::format("{}:{}", host, port);
}

/// Join a host and a numeric port.
/// @param host The host, bracketed or not.
/// @param port The port.
/// @return `host:port`, with the host bracketed when it needs to be.
[[nodiscard]] inline std::string FormatHostPort(std::string_view host, std::uint16_t port)
{
    return FormatHostPort(host, std::format("{}", port));
}

} // namespace FastCache
