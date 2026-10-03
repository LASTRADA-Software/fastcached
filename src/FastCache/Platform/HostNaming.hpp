// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>

/// @file HostNaming.hpp
/// This machine's name in DNS, behind a seam. `--raft-self` and the default `--advertise` take
/// the fully qualified name, so a peer across a VPN dials a name its resolver can answer rather
/// than a bare label only the office LAN knows; the primary DNS suffix is where a node looks for
/// the fleet's SRV record (`Platform/SrvResolver.hpp`).
///
/// The decisions -- what a suffix is, and whether a resolver's canonical name is this machine's
/// -- are the two pure helpers below, so every host's answer is testable on any host. The system
/// implementation only asks the platform.
namespace FastCache
{

/// This machine's names in DNS. Asked once at startup; a name that changes later is a restart's to see.
class IHostNaming
{
  public:
    IHostNaming() = default;
    IHostNaming(IHostNaming const&) = delete;
    IHostNaming(IHostNaming&&) = delete;
    IHostNaming& operator=(IHostNaming const&) = delete;
    IHostNaming& operator=(IHostNaming&&) = delete;
    virtual ~IHostNaming() = default;

    /// @return The fully qualified DNS name, or the bare host name when the machine has no domain.
    [[nodiscard]] virtual std::string FullyQualifiedName() const = 0;

    /// @return The primary DNS suffix ("corp.example"), empty when the machine has none.
    [[nodiscard]] virtual std::string PrimaryDnsSuffix() const = 0;

    /// @return The name the platform offered and `JudgeHostNaming` refused as a placeholder, so a
    ///         log line can say what was declined; empty when nothing was.
    [[nodiscard]] virtual std::string DeclinedName() const = 0;
};

/// DNS suffixes that name no machine a peer can resolve: the placeholders an `/etc/hosts` line
/// carries for a machine with no domain. Compared whole and without regard to ASCII case.
inline constexpr std::array PlaceholderDnsSuffixes { std::string_view { "localdomain" }, std::string_view { "localhost" } };

/// This machine's names as a node takes them: judged, never merely read.
struct HostNamingAnswer
{
    std::string fqdn;     ///< The name peers are told to dial.
    std::string suffix;   ///< Where the fleet's SRV record is asked for; empty for none.
    std::string declined; ///< A placeholder name that was refused; empty when none was.
};

/// Refuse a placeholder domain as this machine's name, by name.
///
/// `host.localdomain` is what WSL, and many a Linux install with no domain, answers for itself --
/// and only this machine resolves it. Advertised, it is a CONFIDENT WRONG SIGNAL: the node
/// registers, peers are told to dial it, and every dial fails with nothing at either end saying
/// why. So a name whose suffix is a `PlaceholderDnsSuffixes` row is refused for its first label,
/// the bare host name -- which may or may not resolve on the other machines, the vague right
/// answer -- and the suffix is dropped, so no SRV query is made against a domain that is not one.
/// What was refused is kept, for the one log line that names it and the flags that override it.
/// @param fqdn   The fully qualified name the platform answered.
/// @param suffix The primary DNS suffix it answered.
/// @return The names to use, and what was declined.
[[nodiscard]] HostNamingAnswer JudgeHostNaming(std::string_view fqdn, std::string_view suffix);

/// This machine's naming, read from the platform ONCE, here.
///
/// Both names are taken at construction and handed back unchanged, rather than asked again per
/// call: two derivations a moment apart can disagree, and a suffix that does not belong to the
/// FQDN beside it is worse than either answer alone. Not a cache -- nothing is refreshed, and a
/// renamed machine is a restart's to see, as `IHostNaming` says.
///
/// - Windows: `GetComputerNameExW(ComputerNameDnsFullyQualified)` and `(ComputerNameDnsDomain)`,
///   which read local configuration and touch no network.
/// - POSIX: `gethostname` (through `QueryHostFacts`), then `getaddrinfo(AI_CANONNAME)` for the
///   canonical name, judged by `FullyQualifiedNameFrom`; the suffix is `DnsSuffixOf` that name.
/// - Both: the pair then passes `JudgeHostNaming`, so a placeholder domain is refused alike on
///   every platform.
///   `getaddrinfo` may ask DNS, and nothing here can interrupt it: its own bound is the system
///   resolver's configuration (`resolv.conf`'s `timeout` and `attempts`). A caller that must not
///   wait that long runs this on a thread and stops waiting -- the node's start does, at
///   `HostNamingBound` -- so nothing this read uses after its wait may be borrowed.
/// @return The system naming. Never null; a name the platform would not give is empty.
[[nodiscard]] std::unique_ptr<IHostNaming> MakeSystemHostNaming();

/// The DNS suffix a fully qualified name carries: everything after its first label.
///
/// A single trailing root dot is dropped first, so `office-a.` names no domain rather than an
/// empty one, and `office-a.corp.example.` is `corp.example`.
/// @param fqdn A fully qualified name, or a bare host name.
/// @return The suffix, or empty when @p fqdn is a single label -- no domain, and so no SRV query.
[[nodiscard]] std::string DnsSuffixOf(std::string_view fqdn);

/// Which of a resolver's canonical name and this machine's own host name is its FQDN.
///
/// The canonical name is taken only when it names THIS machine: it equals @p hostName, or it
/// starts with @p hostName's first label followed by a dot, compared without regard to ASCII
/// case (DNS names are case-insensitive). Anything else is refused in favour of the host name --
/// an `/etc/hosts` that lists `localhost` first on the host's own line makes the canonical name
/// `localhost`, and advertising that would send every peer to itself. A single trailing root dot
/// on the canonical name is dropped.
/// @param hostName      What the machine calls itself (`gethostname`).
/// @param canonicalName What the resolver answered for it; empty when it answered nothing.
/// @return The fully qualified name, or @p hostName when the canonical name is not this machine's.
[[nodiscard]] std::string FullyQualifiedNameFrom(std::string_view hostName, std::string_view canonicalName);

} // namespace FastCache
