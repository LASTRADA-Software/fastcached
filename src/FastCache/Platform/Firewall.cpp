// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/Firewall.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache
{

namespace
{
    constexpr auto ProtocolRows = EnumTable<FirewallProtocol, FirewallProtocolRow> { {
        { .protocol = FirewallProtocol::Tcp, .name = "tcp", .ianaNumber = 6 },
        { .protocol = FirewallProtocol::Udp, .name = "udp", .ianaNumber = 17 },
    } };

    static_assert(RowsInEnumeratorOrder(ProtocolRows, [](FirewallProtocolRow const& row) { return row.protocol; }),
                  "every FirewallProtocol needs a row, at its own index");

    constexpr auto PortKindRows = EnumTable<FirewallPortKind, FirewallPortKindRow> { {
        { .kind = FirewallPortKind::Fixed,
          .nameText = [](std::uint16_t number) { return std::format("{}", number); },
          .firewallText = [](std::uint16_t number) { return std::format("{}", number); },
          .fault = [](std::uint16_t number) -> std::optional<std::string> {
              if (number == 0)
                  return std::string { "its port is 0" };
              return std::nullopt;
          } },
        { .kind = FirewallPortKind::Any,
          .nameText = [](std::uint16_t /*number*/) { return std::string { "any" }; },
          .firewallText = [](std::uint16_t /*number*/) { return std::string { "*" }; },
          // Which of the two was meant is not for the firewall to guess: opening every port where
          // one was intended is the direction that admits somebody else.
          .fault = [](std::uint16_t number) -> std::optional<std::string> {
              if (number != 0)
                  return std::format("it admits any local port and also names port {}", number);
              return std::nullopt;
          },
          .onlyProtocol = FirewallProtocol::Udp },
    } };

    static_assert(RowsInEnumeratorOrder(PortKindRows, [](FirewallPortKindRow const& row) { return row.kind; }),
                  "every FirewallPortKind needs a row, at its own index");
    static_assert(std::ranges::all_of(PortKindRows,
                                      [](FirewallPortKindRow const& row) {
                                          return row.nameText != nullptr && row.firewallText != nullptr
                                                 && row.fault != nullptr;
                                      }),
                  "every FirewallPortKind row spells its ports and says what it refuses");

    /// @p digits as a whole decimal number, or nullopt when it is not one.
    [[nodiscard]] std::optional<unsigned> DecimalOf(std::string_view digits) noexcept
    {
        auto value = 0U;
        auto const [stopped, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (digits.empty() || error != std::errc {} || stopped != digits.data() + digits.size())
            return std::nullopt;
        return value;
    }

    /// @p text cut at every @p separator; empty pieces are kept, since they are what a grammar refuses.
    [[nodiscard]] std::vector<std::string_view> Pieces(std::string_view text, char separator)
    {
        std::vector<std::string_view> pieces;
        auto rest = text;
        auto cut = rest.find(separator);
        while (cut != std::string_view::npos)
        {
            pieces.push_back(rest.substr(0, cut));
            rest = rest.substr(cut + 1);
            cut = rest.find(separator);
        }
        pieces.push_back(rest);
        return pieces;
    }

    /// The bytes of an address, most significant first; an IPv4 address fills the first four.
    using AddressBytes = std::array<std::uint8_t, 16>;

    /// A decimal number from 0 to 255 with no leading zero, or nullopt.
    [[nodiscard]] std::optional<std::uint8_t> OctetOf(std::string_view part) noexcept
    {
        auto const value = DecimalOf(part);
        if (!value.has_value() || *value > 255 || (part.size() > 1 && part.front() == '0'))
            return std::nullopt;
        return static_cast<std::uint8_t>(*value);
    }

    /// Four decimal numbers from 0 to 255, separated by dots, none with a leading zero.
    [[nodiscard]] std::optional<AddressBytes> ParseIpv4(std::string_view text)
    {
        auto const parts = Pieces(text, '.');
        if (parts.size() != 4)
            return std::nullopt;
        AddressBytes bytes {};
        for (auto const index: std::views::iota(std::size_t { 0 }, parts.size()))
        {
            auto const octet = OctetOf(parts[index]);
            if (!octet.has_value())
                return std::nullopt;
            bytes[index] = *octet;
        }
        return bytes;
    }

    /// One to four hexadecimal digits.
    [[nodiscard]] bool IsHexGroup(std::string_view group) noexcept
    {
        return !group.empty() && group.size() <= 4 && std::ranges::all_of(group, [](char digit) {
            return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
        });
    }

    /// The 16-bit groups one side of an IPv6 address spells, or nullopt when it is malformed.
    /// @param side The text on one side of `::` (or the whole address).
    /// @param tailMayBeIpv4 Whether the last piece may be a dotted IPv4 address (two groups).
    [[nodiscard]] std::optional<std::vector<std::uint16_t>> GroupsIn(std::string_view side, bool tailMayBeIpv4)
    {
        std::vector<std::uint16_t> groups;
        if (side.empty())
            return groups;
        auto const pieces = Pieces(side, ':');
        for (auto const index: std::views::iota(std::size_t { 0 }, pieces.size()))
        {
            auto const piece = pieces[index];
            if (tailMayBeIpv4 && index + 1 == pieces.size() && piece.contains('.'))
            {
                auto const embedded = ParseIpv4(piece);
                if (!embedded.has_value())
                    return std::nullopt;
                groups.push_back(static_cast<std::uint16_t>(((*embedded)[0] << 8U) | (*embedded)[1]));
                groups.push_back(static_cast<std::uint16_t>(((*embedded)[2] << 8U) | (*embedded)[3]));
                continue;
            }
            auto value = 0U;
            if (!IsHexGroup(piece)
                || std::from_chars(piece.data(), piece.data() + piece.size(), value, 16).ec != std::errc {})
                return std::nullopt;
            groups.push_back(static_cast<std::uint16_t>(value));
        }
        return groups;
    }

    /// Eight groups, or fewer around exactly one `::`; the last may be a dotted IPv4 address.
    [[nodiscard]] std::optional<AddressBytes> ParseIpv6(std::string_view text)
    {
        std::vector<std::uint16_t> groups;
        auto const gap = text.find("::");
        if (gap == std::string_view::npos)
        {
            auto whole = GroupsIn(text, true);
            if (!whole.has_value() || whole->size() != 8)
                return std::nullopt;
            groups = std::move(*whole);
        }
        else
        {
            if (text.find("::", gap + 1) != std::string_view::npos)
                return std::nullopt;
            auto const head = GroupsIn(text.substr(0, gap), false);
            auto const tail = GroupsIn(text.substr(gap + 2), true);
            if (!head.has_value() || !tail.has_value() || head->size() + tail->size() > 7)
                return std::nullopt;
            groups = *head;
            groups.resize(8 - tail->size(), 0);
            groups.insert(groups.end(), tail->begin(), tail->end());
        }
        AddressBytes bytes {};
        for (auto const index: std::views::iota(std::size_t { 0 }, groups.size()))
        {
            bytes[2 * index] = static_cast<std::uint8_t>(groups[index] >> 8U);
            bytes[(2 * index) + 1] = static_cast<std::uint8_t>(groups[index] & 0xFFU);
        }
        return bytes;
    }

    /// @return @p bytes' first four as a dotted IPv4 address.
    [[nodiscard]] std::string FormatIpv4(AddressBytes const& bytes)
    {
        return std::format("{}.{}.{}.{}", bytes[0], bytes[1], bytes[2], bytes[3]);
    }

    /// @return @p bytes as an IPv6 address in RFC 5952's canonical text: lowercase, no leading
    ///         zeros, and the longest run of two or more zero groups (the first on a tie) as `::`.
    [[nodiscard]] std::string FormatIpv6(AddressBytes const& bytes)
    {
        constexpr auto GroupCount = std::size_t { 8 };
        std::array<std::uint16_t, GroupCount> groups {};
        for (auto const index: std::views::iota(std::size_t { 0 }, GroupCount))
            groups[index] = static_cast<std::uint16_t>((bytes[2 * index] << 8U) | bytes[(2 * index) + 1]);

        auto gapStart = GroupCount;
        auto gapLength = std::size_t { 1 };
        auto runLength = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, GroupCount))
        {
            runLength = groups[index] == 0 ? runLength + 1 : 0;
            if (runLength > gapLength)
            {
                gapLength = runLength;
                gapStart = index + 1 - runLength;
            }
        }

        std::string text;
        auto index = std::size_t { 0 };
        while (index < GroupCount)
        {
            if (index == gapStart)
            {
                text += "::";
                index += gapLength;
                continue;
            }
            if (!text.empty() && !text.ends_with(':'))
                text += ':';
            text += std::format("{:x}", groups[index]);
            ++index;
        }
        return text;
    }

    /// Which grammar an address is read with. **Private**: never transmitted or persisted.
    enum class AddressFamily : std::uint8_t
    {
        V4,
        V6,
        Last
    };

    /// One address family, described once.
    struct AddressFamilyRow
    {
        AddressFamily family {};                                   ///< Which family; its own index.
        std::string_view name;                                     ///< How a refusal names it.
        unsigned maxPrefix {};                                     ///< The longest prefix length.
        std::optional<AddressBytes> (*parse)(std::string_view) {}; ///< The address grammar.
        std::string (*format)(AddressBytes const&) {};             ///< The address's canonical text.
        std::string_view malformed;                                ///< What a malformed address is told.
    };

    constexpr auto AddressFamilies = EnumTable<AddressFamily, AddressFamilyRow> { {
        { .family = AddressFamily::V4,
          .name = "IPv4",
          .maxPrefix = 32,
          .parse = &ParseIpv4,
          .format = &FormatIpv4,
          .malformed = "an IPv4 address is four numbers from 0 to 255, separated by dots, none with a leading zero" },
        { .family = AddressFamily::V6,
          .name = "IPv6",
          .maxPrefix = 128,
          .parse = &ParseIpv6,
          .format = &FormatIpv6,
          .malformed = "an IPv6 address is up to eight groups of one to four hex digits, with at most one '::'" },
    } };

    static_assert(RowsInEnumeratorOrder(AddressFamilies, [](AddressFamilyRow const& row) { return row.family; }),
                  "every AddressFamily needs a row, at its own index");

    /// @p address with every bit from @p prefix on cleared.
    /// @return The network address, and whether any bit had to be cleared.
    [[nodiscard]] std::pair<AddressBytes, bool> NetworkOf(AddressBytes address, unsigned prefix, unsigned maxPrefix)
    {
        auto hostBits = false;
        for (auto const bit: std::views::iota(prefix, maxPrefix))
        {
            auto& byte = address[bit / 8];
            auto const mask = static_cast<std::uint8_t>(0x80U >> (bit % 8));
            hostBits = hostBits || (byte & mask) != 0;
            byte = static_cast<std::uint8_t>(byte & ~mask);
        }
        return { address, hostBits };
    }
} // namespace

FirewallProtocolRow const& FirewallProtocolRowOf(FirewallProtocol protocol) noexcept
{
    return ProtocolRows[static_cast<std::size_t>(protocol)];
}

FirewallPortKindRow const& FirewallPortKindRowOf(FirewallPortKind kind) noexcept
{
    return PortKindRows[static_cast<std::size_t>(kind)];
}

std::string FirewallGroupFor(std::string_view serviceName)
{
    return std::format("fastcached: {}", serviceName);
}

std::string FirewallRuleName(std::string_view serviceName,
                             std::string_view surface,
                             FirewallProtocol protocol,
                             FirewallLocalPort localPort)
{
    return std::format("{} {} {}/{}",
                       serviceName,
                       surface,
                       FirewallProtocolRowOf(protocol).name,
                       FirewallPortKindRowOf(localPort.kind).nameText(localPort.number));
}

std::string FirewallNameKey(std::string_view name)
{
    std::string key { name };
    std::ranges::transform(key, key.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    return key;
}

std::string FirewallNameCollision(std::string_view group, std::string_view ours, std::string_view theirs)
{
    return std::format("our rule '{}' (group '{}') and the rule '{}' outside it have the same name ignoring case, and "
                       "the firewall removes rules by name, so the other rule could be removed instead; nothing was "
                       "changed -- rename the other rule, then run this again",
                       ours,
                       group,
                       theirs);
}

std::expected<std::string, ConfigError> ParseFirewallScope(std::string_view text)
{
    auto const refuse = [text](std::string_view why) {
        // `source` and `field` stay empty on purpose: a value parser cannot know which flag
        // reached it, so `ApplyOneOption` stamps the row's own spelling.
        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::ParseError,
            .source = {},
            .line = 0,
            .field = {},
            .context = std::format(
                "'{}' is not a scope: {}; expected an IPv4 or IPv6 address, or one with a /prefix", text, why) });
    };

    if (text.empty())
        return refuse("it is empty");

    auto const slash = text.find('/');
    auto const address = text.substr(0, slash);
    // The family is read off the text: only IPv6 spells a colon.
    auto const& family =
        AddressFamilies[static_cast<std::size_t>(address.contains(':') ? AddressFamily::V6 : AddressFamily::V4)];
    auto const bytes = family.parse(address);
    if (!bytes.has_value())
        return refuse(family.malformed);
    if (slash == std::string_view::npos)
        return std::string { text };

    auto const prefix = DecimalOf(text.substr(slash + 1));
    if (prefix == std::optional { 0U })
        return refuse("a /0 prefix is every address; omit --firewall-allow to allow any address");
    if (!prefix.has_value() || *prefix > family.maxPrefix)
        return refuse(
            std::format("the prefix length of an {} address is a number from 1 to {}", family.name, family.maxPrefix));
    // A prefix with host bits set names one network while spelling another address, and which
    // one a firewall keeps is its own business -- so the operator is asked which was meant.
    if (auto const [network, hostBits] = NetworkOf(*bytes, *prefix, family.maxPrefix); hostBits)
        return refuse(std::format("host bits are set below the /{0} prefix; did you mean {1}/{0} or {2}/{3}",
                                  *prefix,
                                  family.format(network),
                                  address,
                                  family.maxPrefix));
    return std::string { text };
}

namespace
{
    /// What is wrong with one rule, or nullopt when nothing is.
    using RuleFault = std::optional<std::string> (*)(FirewallRule const&);

    /// @return Why @p rule cannot be told apart from another, if it has no name.
    [[nodiscard]] std::optional<std::string> NameFault(FirewallRule const& rule)
    {
        if (rule.name.empty())
            return std::string { "it has no name" };
        return std::nullopt;
    }

    /// @return Why @p rule would not be scoped to a service, if it names none.
    [[nodiscard]] std::optional<std::string> ServiceFault(FirewallRule const& rule)
    {
        if (rule.serviceName.empty())
            return std::string { "it names no service, and a rule is scoped to one" };
        return std::nullopt;
    }

    /// @return Why @p rule's program is ambiguous, if it is not an absolute path.
    [[nodiscard]] std::optional<std::string> ProgramFault(FirewallRule const& rule)
    {
        if (!rule.program.is_absolute())
            return std::format("its program '{}' is not an absolute path", rule.program.string());
        return std::nullopt;
    }

    /// @return Why @p rule's local ports are refused, if its number disagrees with its kind.
    [[nodiscard]] std::optional<std::string> PortFault(FirewallRule const& rule)
    {
        return FirewallPortKindRowOf(rule.localPort.kind).fault(rule.localPort.number);
    }

    /// @return Why @p rule's port kind may not admit its protocol, if its row names another.
    [[nodiscard]] std::optional<std::string> PortProtocolFault(FirewallRule const& rule)
    {
        auto const& kind = FirewallPortKindRowOf(rule.localPort.kind);
        if (!kind.onlyProtocol.has_value() || *kind.onlyProtocol == rule.protocol)
            return std::nullopt;
        return std::format("it admits any local port on {}, and only a {} socket the kernel binds needs that -- "
                           "every {} surface is bound to a port the configuration names",
                           FirewallProtocolRowOf(rule.protocol).name,
                           FirewallProtocolRowOf(*kind.onlyProtocol).name,
                           FirewallProtocolRowOf(rule.protocol).name);
    }

    /// @return What is wrong with the first of @p rule's remote addresses `ParseFirewallScope` refuses.
    [[nodiscard]] std::optional<std::string> ScopeFault(FirewallRule const& rule)
    {
        for (auto const& scope: rule.remoteAddresses)
            if (auto const parsed = ParseFirewallScope(scope); !parsed.has_value())
                return std::format("a remote address is refused: {}", parsed.error().context);
        return std::nullopt;
    }

    /// Every property a rule must have before the firewall is touched, one row each. Checked
    /// BEFORE the group is emptied: the new rules share the old ones' group and names, so they
    /// cannot be added first, and a rule refused after the removal leaves the service with fewer
    /// rules than it had.
    constexpr auto RuleFaults = std::to_array<RuleFault>({
        &NameFault,
        &ServiceFault,
        &ProgramFault,
        &PortFault,
        &PortProtocolFault,
        &ScopeFault,
    });

    /// @return Nothing when every rule of @p rules may be applied as the whole of @p group, or
    ///         what is wrong with the first that may not.
    [[nodiscard]] std::expected<void, std::string> CheckRules(std::string_view group, std::span<FirewallRule const> rules)
    {
        if (auto const* const stray =
                core::findIfOrNull(rules, [group](FirewallRule const& rule) { return rule.group != group; }))
            return std::unexpected(std::format("rule '{}' is filed under '{}', not '{}'", stray->name, stray->group, group));

        for (auto const index: std::views::iota(std::size_t { 0 }, rules.size()))
            for (auto const fault: RuleFaults)
                if (auto const why = fault(rules[index]); why.has_value())
                    return std::unexpected(std::format("rule {} ('{}') is refused: {}", index + 1, rules[index].name, *why));

        // Keyed ignoring case: two names the firewall may treat as one must not both be added.
        using Keyed = std::pair<std::string, std::string_view>;
        std::vector<Keyed> keyed;
        keyed.reserve(rules.size());
        for (auto const& rule: rules)
            keyed.emplace_back(FirewallNameKey(rule.name), rule.name);
        std::ranges::sort(keyed);
        if (auto const twice = std::ranges::adjacent_find(keyed, std::ranges::equal_to {}, &Keyed::first);
            twice != keyed.end())
            return std::unexpected(
                std::format("rules '{}' and '{}' have the same name ignoring case, and the firewall removes rules by name",
                            twice->second,
                            std::next(twice)->second));
        return {};
    }
} // namespace

std::expected<FirewallOutcome, std::string> ApplyServiceFirewall(IFirewall& firewall,
                                                                 std::string_view group,
                                                                 std::span<FirewallRule const> rules)
{
    if (auto const checked = CheckRules(group, rules); !checked.has_value())
        return std::unexpected(std::format("{}; the firewall was not changed", checked.error()));

    // The names about to be added travel with the removal, so a rule outside the group already
    // carrying one refuses the apply in the same walk, before anything is removed.
    std::vector<std::string> incoming;
    incoming.reserve(rules.size());
    std::ranges::transform(rules, std::back_inserter(incoming), [](FirewallRule const& rule) { return rule.name; });
    auto const removed = firewall.RemoveGroup(group, incoming);
    if (!removed.has_value())
        return std::unexpected(removed.error());

    auto added = std::size_t { 0 };
    for (auto const& rule: rules)
    {
        if (auto const done = firewall.AddInboundAllow(rule); !done.has_value())
            return std::unexpected(
                std::format("{} of {} rule(s) created, then '{}' was refused: {}; the {} rule(s) the group held "
                            "before had already been removed",
                            added,
                            rules.size(),
                            rule.name,
                            done.error(),
                            *removed));
        ++added;
    }
    return FirewallOutcome { .removed = *removed, .added = added };
}

std::expected<std::size_t, std::string> RemoveServiceFirewall(IFirewall& firewall, std::string_view group)
{
    return firewall.RemoveGroup(group, {});
}

namespace
{
    /// @param names Rule names.
    /// @return The names, comma-separated.
    [[nodiscard]] std::string JoinedNames(std::span<std::string const> names)
    {
        std::string joined;
        for (auto const& name: names)
            joined += std::format("{}{}", joined.empty() ? "" : ", ", name);
        return joined;
    }

    /// @param text Any text.
    /// @return @p text as a PowerShell single-quoted literal, embedded quotes doubled.
    [[nodiscard]] std::string PowerShellQuoted(std::string_view text)
    {
        std::string quoted { "'" };
        for (auto const letter: text)
            quoted += letter == '\'' ? std::string_view { "''" } : std::string_view { &letter, 1 };
        return quoted + "'";
    }

    /// What a refused removal of @p group left behind, and how to remove it.
    /// @param firewall The firewall that refused.
    /// @param group The group it was asked to empty.
    /// @return Text to append (leading newline).
    [[nodiscard]] std::string LeftInPlace(IFirewall& firewall, std::string_view group)
    {
        // By GROUP, never by name: a name may also be carried by a rule outside the group, which
        // is one of the reasons a removal refuses in the first place. Safe to offer only because
        // no registration remains by now -- see `RemovalFirewallNote`.
        auto const remedy = std::format("to remove them, run from an elevated PowerShell: Remove-NetFirewallRule -Group "
                                        "{} (it removes by group, never by name), or clear the cause above and run "
                                        "--uninstall-service again, which removes them now that the service is gone",
                                        PowerShellQuoted(group));
        auto const left = firewall.NamesInGroup(group);
        if (!left.has_value())
            return std::format(
                "\nwhich of its rules remain could not be listed ({}), so every one may still be in place; {}",
                left.error(),
                remedy);
        if (left->empty())
            return "\nno rule of the group remains";
        return std::format("\nstill in place: {}\n{}", JoinedNames(*left), remedy);
    }

    /// @param host A configured bind host.
    /// @return Whether it is the NAME `localhost`, compared ignoring ASCII case.
    [[nodiscard]] bool NamesLocalhost(std::string_view host)
    {
        return FirewallNameKey(host) == "localhost";
    }

    /// Why a rule was opened for each surface bound to the name `localhost`.
    ///
    /// `IsLoopbackHost` deliberately does not take the name to be loopback -- a name is whatever
    /// the resolver answers -- so a surface bound to it gets a rule like any other, and the
    /// operator is told why and how to need none.
    /// @param rules The rules being opened.
    /// @return One line per such rule (each with a leading newline); empty when there is none.
    [[nodiscard]] std::string LocalhostNotes(std::span<FirewallRule const> rules)
    {
        std::string notes;
        for (auto const& rule: rules)
            if (NamesLocalhost(rule.bindHost))
                notes += std::format("\nnote: {} is opened for a surface bound to '{}': a name is whatever the "
                                     "resolver answers, so it is not taken to be loopback; bind 127.0.0.1 or ::1 "
                                     "to need no rule",
                                     rule.name,
                                     rule.bindHost);
        return notes;
    }
} // namespace

std::string RegistrationFirewallNote(IFirewall* firewall, std::string_view serviceName, std::span<FirewallRule const> rules)
{
    std::vector<std::string> names;
    names.reserve(rules.size());
    std::ranges::transform(rules, std::back_inserter(names), &FirewallRule::name);

    if (firewall == nullptr)
        return rules.empty() ? std::string {}
                             : std::format("\nnote: this platform's firewall is not managed here; these surfaces of the "
                                           "service face the network and need a rule of your own: {}{}",
                                           JoinedNames(names),
                                           LocalhostNotes(rules));
    auto const applied = ApplyServiceFirewall(*firewall, FirewallGroupFor(serviceName), rules);
    if (!applied.has_value())
        return std::format("\nwarning: the firewall rules were not all created ({}); other machines may not reach this "
                           "service until --install-service is run again",
                           applied.error());
    if (rules.empty())
        return applied->removed == 0
                   ? std::string { "\nfirewall: no rule needed; every surface this configuration opens is loopback" }
                   : std::format("\nfirewall: no rule needed; every surface this configuration opens is loopback, so "
                                 "the {} rule(s) an earlier install opened were removed",
                                 applied->removed);
    return std::format(
        "\nfirewall: allowed inbound {} (every network profile){}", JoinedNames(names), LocalhostNotes(rules));
}

std::string RemovalFirewallNote(IFirewall* firewall, std::string_view serviceName)
{
    if (firewall == nullptr)
        return {};
    auto const group = FirewallGroupFor(serviceName);
    auto const removed = RemoveServiceFirewall(*firewall, group);
    if (removed.has_value())
        return std::format("\nfirewall: removed {} rule(s)", *removed);
    return std::format("\nwarning: the firewall rules of '{}' were not removed: {}{}",
                       group,
                       removed.error(),
                       LeftInPlace(*firewall, group));
}

} // namespace FastCache
