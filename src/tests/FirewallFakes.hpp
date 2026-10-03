// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/Firewall.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// An `IFirewall` holding its rules in a vector, for every case that asks what an install or an
/// uninstall did to a firewall. ONE fake for both test binaries, for `ScriptedSocket.hpp`'s
/// reason: a copy per file drifts from the interface in silence.
///
/// The Windows Firewall removes by NAME and does not keep names unique, so a removal refuses when
/// a rule outside the group carries the name of a rule inside it or of one about to be added.
/// This fake removes by GROUP, but models that refusal the same way -- names compared ignoring
/// case, through `FirewallNameKey` -- so a case can assert it.
class RecordingFirewall final: public IFirewall
{
  public:
    /// @copydoc IFirewall::AddInboundAllow
    [[nodiscard]] std::expected<void, std::string> AddInboundAllow(FirewallRule const& rule) override
    {
        auto const attempt = adds++;
        if (refuseAddAt.has_value() && attempt == *refuseAddAt)
            return std::unexpected(std::string { "scripted refusal" });
        rules.push_back(rule);
        return {};
    }

    /// @copydoc IFirewall::RemoveGroup
    [[nodiscard]] std::expected<std::size_t, std::string> RemoveGroup(std::string_view group,
                                                                      std::span<std::string const> incoming) override
    {
        if (refuseRemove.has_value())
            return std::unexpected(*refuseRemove);

        // (key, spelling): the group's names first, then the incoming ones, as the Windows walk
        // checks them, so the refusal names our spelling as well as the outside rule's.
        using Named = std::pair<std::string, std::string>;
        std::vector<Named> ours;
        for (auto const& rule: rules)
            if (rule.group == group)
                ours.emplace_back(FirewallNameKey(rule.name), rule.name);
        for (auto const& name: incoming)
            ours.emplace_back(FirewallNameKey(name), name);
        for (auto const& rule: rules)
        {
            if (rule.group == group)
                continue;
            if (auto const match = std::ranges::find(ours, FirewallNameKey(rule.name), &Named::first); match != ours.end())
                return std::unexpected(FirewallNameCollision(group, match->second, rule.name));
        }

        if (refuseRemoveAt.has_value())
        {
            // A removal that stops part-way: the group's first `refuseRemoveAt` rules go, as the
            // Windows removal takes them one name at a time, and then the firewall refuses.
            auto const total = static_cast<std::size_t>(
                std::ranges::count(rules, group, [](FirewallRule const& rule) -> std::string_view { return rule.group; }));
            auto taken = std::size_t { 0 };
            std::erase_if(rules, [group, limit = *refuseRemoveAt, &taken](FirewallRule const& rule) {
                if (rule.group != group || taken == limit)
                    return false;
                ++taken;
                return true;
            });
            return std::unexpected(std::format("scripted refusal after {} of {} removed", taken, total));
        }

        return std::erase_if(rules, [group](FirewallRule const& rule) { return rule.group == group; });
    }

    /// @copydoc IFirewall::NamesInGroup
    [[nodiscard]] std::expected<std::vector<std::string>, std::string> NamesInGroup(std::string_view group) override
    {
        if (refuseList.has_value())
            return std::unexpected(*refuseList);
        std::vector<std::string> names;
        for (auto const& rule: rules)
            if (rule.group == group)
                names.push_back(rule.name);
        return names;
    }

    /// @param group A group name.
    /// @return The rules filed under it, in the order they were added.
    [[nodiscard]] std::vector<FirewallRule> InGroup(std::string_view group) const
    {
        std::vector<FirewallRule> found;
        std::ranges::copy_if(
            rules, std::back_inserter(found), [group](FirewallRule const& rule) { return rule.group == group; });
        return found;
    }

    std::vector<FirewallRule> rules;           ///< What the firewall holds now.
    std::size_t adds { 0 };                    ///< Every add attempted, refused ones included.
    std::optional<std::size_t> refuseAddAt;    ///< The add (counting from zero) this fake refuses.
    std::optional<std::string> refuseRemove;   ///< When set, every removal is refused with this reason.
    std::optional<std::string> refuseList;     ///< When set, every `NamesInGroup` is refused with this reason.
    std::optional<std::size_t> refuseRemoveAt; ///< When set, a removal takes this many of the group's rules, then refuses.
};

} // namespace FastCache::Testing
