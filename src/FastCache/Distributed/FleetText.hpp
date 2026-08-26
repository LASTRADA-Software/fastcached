// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace FastCache::Distributed
{

/// Escape text for HTML or SVG.
///
/// Every value the fleet surfaces came off a wire: a toolchain fingerprint and an
/// endpoint are whatever a peer sent, and the page and the charts both interpolate
/// them. Shared between the two renderers rather than copied, because two copies
/// are two places for one of them to be forgotten.
///
/// `apps/fastcache-cc/Stats.cpp` keeps its own sibling of this deliberately: that
/// binary does not link this library at all, which is a documented constraint
/// rather than an oversight.
/// @param text Untrusted text.
/// @return The same text, safe to interpolate into markup.
[[nodiscard]] inline std::string EscapeMarkup(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (auto const ch: text)
    {
        switch (ch)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

/// Append one JSON string literal, quotes included.
/// @param out Where to append.
/// @param text The string's contents.
inline void AppendJsonText(std::string& out, std::string_view text)
{
    out += '"';
    for (auto const ch: text)
    {
        switch (ch)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                else
                    out += ch;
                break;
        }
    }
    out += '"';
}

} // namespace FastCache::Distributed
