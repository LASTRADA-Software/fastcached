// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Utf8.hpp>

#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

/// @file PeerText.hpp
/// Bytes another machine sent, made into text a log line or a report may carry.
///
/// **A claim a peer has not proved is printed only once it is TEXT**, and never at whatever length
/// it arrived in. Two steps, one place: every byte that belongs to no UTF-8 sequence is written
/// `\xNN`, and the result is cut on a code-point boundary to a ceiling and marked as cut. Kept apart
/// from `Utf8.hpp`, which stays a `constexpr` leaf without `<format>`.
namespace FastCache
{

/// What a clamped text ends with, so a reader can tell it was cut.
inline constexpr std::string_view ClampedMarker = "...";

/// @p raw as TEXT: every byte that belongs to no UTF-8 sequence written `\xNN`.
/// @param raw What a peer or a component supplied; a path may be in any encoding its host chose.
/// @return Well-formed UTF-8.
[[nodiscard]] inline std::string EscapeNonUtf8(std::string_view raw)
{
    std::string text;
    text.reserve(raw.size());
    while (!raw.empty())
    {
        auto const length = Utf8SequenceLength(raw);
        if (length == 0)
        {
            text += std::format("\\x{:02X}", static_cast<unsigned>(static_cast<unsigned char>(raw.front())));
            raw.remove_prefix(1);
            continue;
        }
        text.append(raw.substr(0, length));
        raw.remove_prefix(length);
    }
    return text;
}

/// @p text cut to @p ceiling bytes on a code-point boundary, marked when it was cut.
/// @param text Well-formed UTF-8.
/// @param ceiling The most bytes it may take; at least `ClampedMarker`'s length.
/// @return The text, or a prefix of it ending in `ClampedMarker`.
[[nodiscard]] inline std::string ClampUtf8(std::string text, std::size_t ceiling)
{
    if (text.size() <= ceiling)
        return text;
    auto cut = ceiling - ClampedMarker.size();
    // Back off a continuation byte at a time, so the cut never splits a sequence -- a split one
    // would be the very non-text this clamp runs after `EscapeNonUtf8` to avoid.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & Utf8ContinuationMask) == Utf8ContinuationMark)
        --cut;
    text.resize(cut);
    text += ClampedMarker;
    return text;
}

/// @p raw as text a line may carry: escaped, then clamped to @p ceiling.
/// @param raw What a peer sent.
/// @param ceiling The most bytes it may take once escaped.
/// @return Well-formed UTF-8 of at most @p ceiling bytes.
[[nodiscard]] inline std::string BoundedPeerText(std::string_view raw, std::size_t ceiling)
{
    return ClampUtf8(EscapeNonUtf8(raw), ceiling);
}

/// @p lead followed by @p items, joined with `, ` for as long as they fit in @p ceiling bytes.
///
/// The rest are COUNTED rather than cut mid-name -- `a, b and 3 more` -- because a list cut at an
/// arbitrary byte reads as naming its last, truncated item. Each item is escaped first, so the
/// list is text whatever its items were.
/// @param lead What comes before the list.
/// @param items The items, in the order to name them.
/// @param ceiling The most bytes the list should take, room for the count included.
/// @return The list.
[[nodiscard]] inline std::string ListWithinCeiling(std::string_view lead,
                                                   std::span<std::string const> items,
                                                   std::size_t ceiling)
{
    auto list = std::string { lead };
    for (auto const index: std::views::iota(std::size_t { 0 }, items.size()))
    {
        auto const separator = index == 0 ? std::string_view { " " } : std::string_view { ", " };
        auto const rest = items.size() - index - 1;
        // Room for this item AND for the count of whatever follows it, so the list is never cut
        // with no room left to say how much was left out.
        auto const tail = rest == 0 ? std::string {} : std::format(" and {} more", rest);
        auto const candidate = std::format("{}{}", separator, EscapeNonUtf8(items[index]));
        if (list.size() + candidate.size() + tail.size() > ceiling)
            return list + std::format(" and {} more", items.size() - index);
        list += candidate;
    }
    return list;
}

} // namespace FastCache
