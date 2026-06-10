// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/IPubSubRegistry.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace FastCache
{

/// Glob match for pub/sub patterns (PSUBSCRIBE), supporting `*`, `?`, and
/// `[...]` character classes — the subset redis uses (`stringmatchlen`).
/// Exposed for unit testing; the registry uses it internally.
/// @param pattern The glob pattern.
/// @param text    The candidate channel name.
/// @return True if `pattern` matches `text`.
[[nodiscard]] bool GlobMatch(std::string_view pattern, std::string_view text) noexcept;

/// In-memory IPubSubRegistry: a mutex-guarded channel/pattern map. One instance
/// is shared process-wide (owned by the daemon body) and injected into every
/// connection's SessionContext. Subscribers are borrowed pointers; each must
/// UnsubscribeAll itself before destruction.
class PubSubRegistry final: public IPubSubRegistry
{
  public:
    [[nodiscard]] std::size_t Subscribe(std::shared_ptr<ISubscriber> sub, std::string_view channel) override;
    [[nodiscard]] std::size_t Unsubscribe(ISubscriber* sub, std::string_view channel) override;
    [[nodiscard]] std::size_t PSubscribe(std::shared_ptr<ISubscriber> sub, std::string_view pattern) override;
    [[nodiscard]] std::size_t PUnsubscribe(ISubscriber* sub, std::string_view pattern) override;
    void UnsubscribeAll(ISubscriber* sub) override;
    [[nodiscard]] std::size_t Publish(std::string_view channel, std::string_view message) override;
    [[nodiscard]] std::vector<std::string> SnapshotChannels(ISubscriber* sub) const override;
    [[nodiscard]] std::vector<std::string> SnapshotPatterns(ISubscriber* sub) const override;
    [[nodiscard]] bool HasAnySubscribers() const noexcept override;

  private:
    /// Count a subscriber's live subscriptions (channels + patterns) while the
    /// lock is held. Used for the confirmation replies.
    [[nodiscard]] std::size_t CountFor(ISubscriber* sub) const;

    /// Per-channel/pattern subscriber set. The raw pointer is the lookup key
    /// (it identifies the subscriber across its lifetime). The weak_ptr
    /// lets `Publish` upgrade to a shared_ptr under the lock, pinning the
    /// subscriber's lifetime through the Deliver call that happens outside
    /// the lock — closing the use-after-free hole where a concurrent
    /// disconnect could destroy the subscriber between snapshot and Deliver.
    using SubscriberSet = std::unordered_map<ISubscriber*, std::weak_ptr<ISubscriber>>;

    mutable std::mutex _mu;
    /// channel -> subscribers exactly subscribed to it.
    std::unordered_map<std::string, SubscriberSet> _channels;
    /// pattern -> subscribers subscribed to that glob pattern.
    std::unordered_map<std::string, SubscriberSet> _patterns;
    /// Total live (subscriber, channel-or-pattern) entries across both
    /// maps. Maintained under `_mu` (same critical section that mutates
    /// the maps) but read lock-free in `HasAnySubscribers` so the
    /// keyspace notifier's hot-write probe collapses to a single atomic
    /// load. Without this, every successful cache mutation on a daemon
    /// with `notify-keyspace-events=AKE` set but no subscriber yet (the
    /// "enabled for later" case in the doc) paid a global mutex
    /// acquisition just to read map emptiness.
    std::atomic<std::size_t> _entryCount { 0 };
};

} // namespace FastCache
