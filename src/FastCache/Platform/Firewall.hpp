// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// The transport a firewall rule admits.
///
/// **Private**: never transmitted or persisted by this project; the number the firewall is
/// given is the row's `ianaNumber`, never the enumerator's value.
enum class FirewallProtocol : std::uint8_t
{
    Tcp,
    Udp,
    Last
};

/// One protocol, described once.
struct FirewallProtocolRow
{
    FirewallProtocol protocol {}; ///< Which protocol; its own index.
    std::string_view name;        ///< How a rule name spells it.
    std::int32_t ianaNumber {};   ///< What the firewall is given: 6 for TCP, 17 for UDP.
};

/// @param protocol A protocol.
/// @return Its row.
[[nodiscard]] FirewallProtocolRow const& FirewallProtocolRowOf(FirewallProtocol protocol) noexcept;

/// Which local ports a rule admits.
///
/// **Private**: never transmitted or persisted by this project; what the firewall is given is
/// the row's `firewallText`, never the enumerator's value.
enum class FirewallPortKind : std::uint8_t
{
    /// Exactly `FirewallLocalPort::number`, which is never 0.
    Fixed,

    /// Every local port, for a socket whose port the KERNEL chooses when it binds -- which no
    /// port-scoped rule can name, however it is configured. The rule is still scoped to the
    /// program and the service, so what it exposes is only the sockets of its protocol that
    /// program opens: a program-scoped rule, not an open port range.
    Any,

    Last
};

/// The local ports of one rule: a kind AND a number, never a number whose 0 means "any".
///
/// A 0 read as "any" by one layer and as port 0 by the next is a rule that admits everything
/// where it was meant to admit nothing, or the reverse -- so "any" is its own kind, and a rule
/// whose number disagrees with its kind is refused before the firewall is touched.
struct FirewallLocalPort
{
    FirewallPortKind kind { FirewallPortKind::Fixed }; ///< Which ports.
    std::uint16_t number {};                           ///< The port for `Fixed`; 0 for `Any`.

    /// Member-wise equality.
    friend bool operator==(FirewallLocalPort const&, FirewallLocalPort const&) = default;
};

/// One port kind, described once.
struct FirewallPortKindRow
{
    FirewallPortKind kind {}; ///< Which kind; its own index.

    /// How a rule name spells the ports: the number for `Fixed`, `any` for `Any`.
    std::string (*nameText)(std::uint16_t number) = nullptr;

    /// What the firewall is given as the rule's local ports: the number for `Fixed`, `*` for
    /// `Any` -- the Windows Firewall's own spelling of every port, and what a new rule of a
    /// port-bearing protocol reads back before any is set.
    std::string (*firewallText)(std::uint16_t number) = nullptr;

    /// Why a rule of this kind carrying @p number cannot be applied, or nullopt.
    std::optional<std::string> (*fault)(std::uint16_t number) = nullptr;

    /// The one protocol a rule of this kind may admit, or nullopt for any.
    ///
    /// `Any` is UDP's alone: it exists for discovery's reply socket, whose port the kernel
    /// chooses, and every TCP surface this project opens is bound to a port the configuration
    /// names. An any-port TCP rule would admit every listener the program ever opens -- the
    /// cache, the compile port, consensus -- whether or not its own rule was meant to.
    std::optional<FirewallProtocol> onlyProtocol {};
};

/// @param kind A port kind.
/// @return Its row.
[[nodiscard]] FirewallPortKindRow const& FirewallPortKindRowOf(FirewallPortKind kind) noexcept;

/// One inbound allow rule, scoped to a program AND a service.
///
/// Profile is always "every profile": a VPN adapter is usually classified Public, and a rule
/// scoped to Domain or Private never reaches it -- which is the office's whole VPN population.
struct FirewallRule
{
    std::string name;                                    ///< Unique within the firewall; `FirewallRuleName`.
    std::string group;                                   ///< What an uninstall removes by; `FirewallGroupFor`.
    std::filesystem::path program;                       ///< The executable the rule admits.
    std::string serviceName;                             ///< The service whose SID the rule is scoped to.
    FirewallProtocol protocol { FirewallProtocol::Tcp }; ///< The transport the rule admits.
    FirewallLocalPort localPort {};                      ///< The local ports.
    std::vector<std::string> remoteAddresses;            ///< `--firewall-allow` scopes; empty means any address.

    /// The host the surface binds, as configured. Never given to the firewall: an install's note
    /// reads it, to say when a rule was opened for a NAME such as `localhost`, which
    /// `IsLoopbackHost` deliberately does not take to be loopback.
    std::string bindHost;

    /// Member-wise equality.
    friend bool operator==(FirewallRule const&, FirewallRule const&) = default;
};

/// A host firewall, as far as a service registration needs one.
class IFirewall
{
  public:
    IFirewall() = default;
    IFirewall(IFirewall const&) = delete;
    IFirewall(IFirewall&&) = delete;
    IFirewall& operator=(IFirewall const&) = delete;
    IFirewall& operator=(IFirewall&&) = delete;
    virtual ~IFirewall() = default;

    /// Add @p rule, enabled, inbound, allowing.
    /// @param rule The rule.
    /// @return Nothing, or why the firewall refused it.
    [[nodiscard]] virtual std::expected<void, std::string> AddInboundAllow(FirewallRule const& rule) = 0;

    /// Remove every rule filed under @p group, unless a name would be shared.
    ///
    /// A firewall removes rules by NAME, and names need not be unique: removing a name a rule
    /// outside @p group also carries could take that rule instead, and adding one gives the NEXT
    /// removal the same problem. So the removal refuses, before removing anything, when a rule
    /// outside @p group carries the name of a rule inside it OR one of @p incoming -- the names
    /// about to be added -- which lets an apply refuse a collision up front, in the same walk.
    /// @param group The group.
    /// @param incoming The names of the rules about to be added; empty for a plain removal.
    /// @return How many were removed, or why the firewall could not be asked or refused.
    [[nodiscard]] virtual std::expected<std::size_t, std::string> RemoveGroup(std::string_view group,
                                                                              std::span<std::string const> incoming) = 0;

    /// The names of the rules filed under @p group, changing nothing.
    ///
    /// What a refused or interrupted removal reports it left behind: the refusal says why, and
    /// only the firewall can say which rules are still there afterwards.
    /// @param group The group.
    /// @return The names, in the order the firewall lists them, or why it could not be asked.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, std::string> NamesInGroup(std::string_view group) = 0;
};

/// @param serviceName A registered service.
/// @return The group its rules are filed under.
[[nodiscard]] std::string FirewallGroupFor(std::string_view serviceName);

/// @param serviceName The service.
/// @param surface The surface the rule admits, e.g. `node` or `discovery-beacon`.
/// @param protocol Its protocol.
/// @param localPort Its local ports; `Any` spells them `any`, so the name says the rule is not
///        one port's.
/// @return The rule's name.
[[nodiscard]] std::string FirewallRuleName(std::string_view serviceName,
                                           std::string_view surface,
                                           FirewallProtocol protocol,
                                           FirewallLocalPort localPort);

/// The key two rule names are compared by: the name with ASCII letters folded to lower case.
///
/// Rule names are compared without regard to case wherever this project asks whether two collide
/// -- treating two names as different when the firewall may not is the direction that removes
/// somebody else's rule. The Windows implementation compares with `CompareStringOrdinal`,
/// ignoring case, which folds beyond ASCII; this portable key folds ASCII only, so it can miss a
/// non-ASCII case difference (which that implementation still refuses) but never invents one.
/// @param name A rule name.
/// @return Its comparison key.
[[nodiscard]] std::string FirewallNameKey(std::string_view name);

/// The refusal every `IFirewall::RemoveGroup` gives for a shared name, worded in one place so
/// every implementation names the same two rules the same way.
/// @param group The group being replaced or removed.
/// @param ours Our rule's name, as this project spells it (held by the group, or about to be added).
/// @param theirs The name as the rule OUTSIDE the group spells it; equal to @p ours ignoring case.
/// @return One sentence naming both spellings and what to do.
[[nodiscard]] std::string FirewallNameCollision(std::string_view group, std::string_view ours, std::string_view theirs);

/// Parse one `--firewall-allow` value: an IPv4 or IPv6 address, optionally with a prefix length.
///
/// A pure grammar, one row per address family (`AddressFamilyRow` in Firewall.cpp), because a
/// scope the firewall reads differently from what the operator typed is a rule admitting
/// somebody else. So an IPv4 number with a leading zero is refused: some readers take `010` as
/// octal 8. `/0` is refused by name: "any address" is spelled by omitting the flag, so an
/// explicit every-address scope is a mistake rather than an intention (`--fleet-open`'s rule).
/// Zone indices (`fe80::1%eth0`) are refused: a firewall remote address has no interface.
/// A prefix with host bits set (`10.0.0.5/24`) is refused, offering both readings -- the network
/// (`10.0.0.0/24`) and the single address (`10.0.0.5/32`) -- since which one the operator meant
/// is not something a parser can decide for them.
/// @param text The value.
/// @return The value as typed, or a `ConfigError` naming the fault. Its `field` and `source`
///         are left empty: a value parser cannot know which flag reached it, so
///         `ApplyOneOption` stamps the row's own spelling.
[[nodiscard]] std::expected<std::string, ConfigError> ParseFirewallScope(std::string_view text);

/// What applying a service's rules did.
struct FirewallOutcome
{
    std::size_t removed {}; ///< Rules of the group that were there before.
    std::size_t added {};   ///< Rules created.
};

/// Make @p rules the whole of @p group: check every rule, remove what the group holds, then add
/// each rule.
///
/// Replace, never append -- a repair or an upgrade runs the install again, and a surface that
/// has since become loopback-only must lose its rule. The new rules share the old ones' group and
/// names, so they cannot be added before the old are removed; instead every rule is checked
/// first, and one that would be refused -- no name, no service, a program that is not an absolute
/// path, a `Fixed` port 0, an `Any` port carrying a number or on a protocol its row does not
/// admit, a remote address `ParseFirewallScope` refuses, or a name another rule of the batch
/// also has, ignoring case (`FirewallNameKey`) -- refuses the call before the firewall is
/// touched.
/// @param firewall The firewall.
/// @param group Every rule must be filed under it; one that is not refuses the call untouched.
/// @param rules The rules; may be empty, which clears the group.
/// @return What changed, or why it stopped (naming how many rules landed first and how many the
///         group held before).
[[nodiscard]] std::expected<FirewallOutcome, std::string> ApplyServiceFirewall(IFirewall& firewall,
                                                                               std::string_view group,
                                                                               std::span<FirewallRule const> rules);

/// Remove every rule of @p group.
/// @param firewall The firewall.
/// @param group The group.
/// @return How many were removed, or why the firewall could not be asked.
[[nodiscard]] std::expected<std::size_t, std::string> RemoveServiceFirewall(IFirewall& firewall, std::string_view group);

/// Apply @p rules for @p serviceName and say what happened, for the line an install prints.
///
/// A rule whose `bindHost` is the name `localhost` gets a line of its own saying why it was
/// opened at all: the name is not taken to be loopback, and binding an address avoids the rule.
/// @param firewall The machine's firewall, or null where none is managed.
/// @param serviceName The service registered.
/// @param rules What it needs; empty clears what an earlier install opened.
/// @return Text to append to the install's message (leading newline); empty when there is nothing
///         to say, which is only where no firewall is managed and nothing faces the network.
[[nodiscard]] std::string RegistrationFirewallNote(IFirewall* firewall,
                                                   std::string_view serviceName,
                                                   std::span<FirewallRule const> rules);

/// Remove @p serviceName's rules and say so, for the line an uninstall prints.
///
/// Only for a service that is gone -- deleted, or never registered: its remedy tells the operator
/// to remove the group by hand, which on a service still installed closes its ports.
/// `WithRemovalFirewall` is the caller that holds to that.
///
/// A removal that is refused, or stops part-way, names every rule the firewall still files under
/// the service's group -- asked of the firewall afterwards, since only it knows which remain --
/// and how to remove them.
/// @param firewall The machine's firewall, or null where none is managed.
/// @param serviceName The service removed.
/// @return Text to append (leading newline); empty where no firewall is managed.
[[nodiscard]] std::string RemovalFirewallNote(IFirewall* firewall, std::string_view serviceName);

/// The machine's firewall.
/// @return The Windows Firewall (`INetFwPolicy2`) on Windows; null everywhere else, where this
///         project manages no firewall and says so rather than pretending.
[[nodiscard]] std::unique_ptr<IFirewall> MakeSystemFirewall();

} // namespace FastCache
