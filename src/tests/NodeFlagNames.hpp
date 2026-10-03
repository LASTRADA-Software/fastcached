// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <apps/fastcache-compile-node/NodeConfig.hpp>

namespace FastCache::Testing
{

/// Every `--flag` spelled in @p text that no row of `Node::NodeOptions()` accepts.
///
/// **What an operator reads is a flag they will type**, so text this binary prints -- its help, a
/// refusal, what enrollment reports -- may name only a flag it parses. A retired flag named there
/// sends somebody after a spelling the parser refuses, and nothing else would notice: a string is
/// not a call site.
///
/// A token is `--` and a run of lower-case letters, digits and hyphens, not preceded by a letter,
/// a digit or a hyphen; its trailing hyphens are dropped. One followed by `*` names a FAMILY
/// (`--cluster-*`) and is accepted when some row's spelling starts with it.
/// @param text What the binary prints.
/// @return The tokens no row accepts, in the order they appear; empty when every one parses.
[[nodiscard]] inline std::vector<std::string> FlagsNoNodeRowAccepts(std::string_view text)
{
    auto const isFlagChar = [](char c) {
        auto const u = static_cast<unsigned char>(c);
        return std::islower(u) != 0 || std::isdigit(u) != 0 || c == '-';
    };
    auto const accepted = [](std::string_view token, bool family) {
        return std::ranges::any_of(Node::NodeOptions(), [token, family](auto const& row) {
            auto const names = [token, family](std::string_view spelling) {
                return !spelling.empty() && (family ? spelling.starts_with(token) : spelling == token);
            };
            return names(row.primary) || names(row.alias);
        });
    };

    auto unknown = std::vector<std::string> {};
    auto at = text.find("--");
    while (at != std::string_view::npos)
    {
        auto end = at + 2;
        while (end < text.size() && isFlagChar(text[end]))
            ++end;
        auto const standsAlone = at == 0 || !isFlagChar(text[at - 1]);
        auto token = text.substr(at, end - at);
        auto const family = end < text.size() && text[end] == '*';
        if (!family)
            while (token.ends_with('-'))
                token.remove_suffix(1);
        if (standsAlone && token.size() > 2 && std::islower(static_cast<unsigned char>(token[2])) != 0
            && !accepted(token, family))
            unknown.emplace_back(token);
        at = text.find("--", end);
    }
    return unknown;
}

} // namespace FastCache::Testing
