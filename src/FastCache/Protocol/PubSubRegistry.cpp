// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/PubSubRegistry.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    /// Match one literal pattern character (handling a leading '\' escape)
    /// against text char `c`, advancing `pos` past the consumed pattern char(s)
    /// only on a successful match. On mismatch `pos` is left unchanged so the
    /// caller can backtrack.
    /// @param pattern Whole pattern.
    /// @param pos     In/out cursor at the literal (or escape) character.
    /// @param c       The text character to test.
    /// @return True if the literal matches `c`.
    [[nodiscard]] bool MatchesLiteral(std::string_view pattern, std::size_t& pos, char c) noexcept
    {
        if (pattern[pos] == '\\' && pos + 1 < pattern.size())
        {
            if (pattern[pos + 1] != c)
                return false;
            pos += 2;
            return true;
        }
        if (pattern[pos] != c)
            return false;
        ++pos;
        return true;
    }

    /// Match one character `c` against a `[...]` class beginning at `pattern[pos]`
    /// (which must be the '['), advancing `pos` past the closing ']'. Supports a
    /// leading '^' negation, 'a-z' ranges, and '\' escapes — the redis subset.
    /// @param pattern Whole pattern.
    /// @param pos     In/out cursor; on entry at '[', on exit just past ']'.
    /// @param c       The text character to test.
    /// @return True if `c` is in the (possibly negated) class.
    [[nodiscard]] bool MatchCharClass(std::string_view pattern, std::size_t& pos, char c) noexcept
    {
        auto cur = pos + 1;
        bool negate = false;
        if (cur < pattern.size() && pattern[cur] == '^')
        {
            negate = true;
            ++cur;
        }
        bool matched = false;
        while (cur < pattern.size() && pattern[cur] != ']')
        {
            if (pattern[cur] == '\\' && cur + 1 < pattern.size())
            {
                matched = matched || pattern[cur + 1] == c;
                cur += 2;
            }
            else if (cur + 2 < pattern.size() && pattern[cur + 1] == '-' && pattern[cur + 2] != ']')
            {
                auto lo = pattern[cur];
                auto hi = pattern[cur + 2];
                if (lo > hi)
                    std::swap(lo, hi);
                matched = matched || (c >= lo && c <= hi);
                cur += 3;
            }
            else
            {
                matched = matched || pattern[cur] == c;
                ++cur;
            }
        }
        pos = cur < pattern.size() ? cur + 1 : cur; // step past ']'
        return negate ? !matched : matched;
    }

} // namespace

bool GlobMatch(std::string_view pattern, std::string_view text) noexcept
{
    // Iterative glob matcher with backtracking for '*', mirroring redis's
    // stringmatchlen semantics: '*' any run, '?' one char, '[...]' a class,
    // '\' escapes the next char. On any mismatch we backtrack to the last '*'.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t starMatch = 0;

    auto const backtrack = [&] {
        if (star == std::string_view::npos)
            return false;
        p = star + 1;
        t = ++starMatch;
        return true;
    };

    while (t < text.size())
    {
        if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            starMatch = t;
        }
        else if (p < pattern.size() && pattern[p] == '?')
        {
            ++p;
            ++t;
        }
        else if (p < pattern.size() && pattern[p] == '[')
        {
            if (MatchCharClass(pattern, p, text[t]))
                ++t;
            else if (!backtrack())
                return false;
        }
        else if (p < pattern.size() && MatchesLiteral(pattern, p, text[t]))
        {
            ++t;
        }
        else if (!backtrack())
        {
            return false;
        }
    }

    // Consume any trailing '*' in the pattern.
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::size_t PubSubRegistry::CountFor(ISubscriber* sub) const
{
    std::size_t count = 0;
    for (auto const& [channel, subs]: _channels)
        if (subs.contains(sub))
            ++count;
    for (auto const& [pattern, subs]: _patterns)
        if (subs.contains(sub))
            ++count;
    return count;
}

std::size_t PubSubRegistry::Subscribe(std::shared_ptr<ISubscriber> sub, std::string_view channel)
{
    std::scoped_lock const lock { _mu };
    auto* const raw = sub.get();
    _channels[std::string { channel }].emplace(raw, std::weak_ptr<ISubscriber> { sub });
    return CountFor(raw);
}

std::size_t PubSubRegistry::Unsubscribe(ISubscriber* sub, std::string_view channel)
{
    std::scoped_lock const lock { _mu };
    if (auto it = _channels.find(std::string { channel }); it != _channels.end())
    {
        it->second.erase(sub);
        if (it->second.empty())
            _channels.erase(it);
    }
    return CountFor(sub);
}

std::size_t PubSubRegistry::PSubscribe(std::shared_ptr<ISubscriber> sub, std::string_view pattern)
{
    std::scoped_lock const lock { _mu };
    auto* const raw = sub.get();
    _patterns[std::string { pattern }].emplace(raw, std::weak_ptr<ISubscriber> { sub });
    return CountFor(raw);
}

std::size_t PubSubRegistry::PUnsubscribe(ISubscriber* sub, std::string_view pattern)
{
    std::scoped_lock const lock { _mu };
    if (auto it = _patterns.find(std::string { pattern }); it != _patterns.end())
    {
        it->second.erase(sub);
        if (it->second.empty())
            _patterns.erase(it);
    }
    return CountFor(sub);
}

std::vector<std::string> PubSubRegistry::SnapshotChannels(ISubscriber* sub) const
{
    std::scoped_lock const lock { _mu };
    std::vector<std::string> out;
    for (auto const& [channel, subs]: _channels)
        if (subs.contains(sub))
            out.push_back(channel);
    return out;
}

std::vector<std::string> PubSubRegistry::SnapshotPatterns(ISubscriber* sub) const
{
    std::scoped_lock const lock { _mu };
    std::vector<std::string> out;
    for (auto const& [pattern, subs]: _patterns)
        if (subs.contains(sub))
            out.push_back(pattern);
    return out;
}

void PubSubRegistry::UnsubscribeAll(ISubscriber* sub)
{
    std::scoped_lock const lock { _mu };
    // Erase the subscriber from every channel/pattern; drop now-empty buckets.
    for (auto it = _channels.begin(); it != _channels.end();)
    {
        it->second.erase(sub);
        it = it->second.empty() ? _channels.erase(it) : std::next(it);
    }
    for (auto it = _patterns.begin(); it != _patterns.end();)
    {
        it->second.erase(sub);
        it = it->second.empty() ? _patterns.erase(it) : std::next(it);
    }
}

std::size_t PubSubRegistry::Publish(std::string_view channel, std::string_view message)
{
    // Snapshot the matching deliveries under the lock, upgrading every weak_ptr
    // to a shared_ptr while still holding _mu. This pins each subscriber's
    // lifetime through the subsequent Deliver call that runs OUTSIDE the lock:
    // a concurrent disconnect cannot destroy the subscriber while we hold an
    // owning reference. Holding the lock across Deliver is unnecessary (Deliver
    // enqueues + wakes another reactor) and would be a lock-ordering hazard.
    struct Delivery
    {
        std::shared_ptr<ISubscriber> sub;
        PushMessage message;
    };
    std::vector<Delivery> deliveries;
    {
        std::scoped_lock const lock { _mu };
        if (auto const it = _channels.find(std::string { channel }); it != _channels.end())
            for (auto const& [raw, weak]: it->second)
            {
                auto strong = weak.lock();
                if (!strong)
                    continue; // subscriber already gone; skip silently.
                deliveries.push_back(Delivery { .sub = std::move(strong),
                                                .message = PushMessage { .kind = PushMessage::Kind::Message,
                                                                         .pattern = {},
                                                                         .channel = std::string { channel },
                                                                         .payload = std::string { message } } });
            }
        for (auto const& [pattern, subs]: _patterns)
            if (GlobMatch(pattern, channel))
                for (auto const& [raw, weak]: subs)
                {
                    auto strong = weak.lock();
                    if (!strong)
                        continue;
                    deliveries.push_back(Delivery { .sub = std::move(strong),
                                                    .message = PushMessage { .kind = PushMessage::Kind::PMessage,
                                                                             .pattern = pattern,
                                                                             .channel = std::string { channel },
                                                                             .payload = std::string { message } } });
                }
    }
    for (auto& delivery: deliveries)
        delivery.sub->Deliver(std::move(delivery.message));
    return deliveries.size();
}

} // namespace FastCache
