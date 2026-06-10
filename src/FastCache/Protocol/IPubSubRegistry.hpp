// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// A single pub/sub delivery destined for one subscribed connection. The
/// registry hands these to a subscriber; the subscriber's own reactor thread
/// later formats and writes them to its socket. Carries an owned copy of the
/// channel/pattern/payload so it outlives the publisher's stack frame.
struct PushMessage
{
    /// What the push represents, so the receiver can format the right frame.
    enum class Kind : std::uint8_t
    {
        Message,  ///< A `message` delivery (channel, payload).
        PMessage, ///< A `pmessage` delivery (pattern, channel, payload).
    };

    Kind kind { Kind::Message };
    std::string pattern; ///< Matching pattern (PMessage only; empty otherwise).
    std::string channel; ///< Channel the message was published to.
    std::string payload; ///< The published message body.
};

/// A subscribed connection, as seen by the registry. The registry only needs to
/// hand deliveries to it; everything else (queueing, waking its reactor,
/// formatting frames) lives in the concrete subscriber. Injected as an
/// interface so the registry has no dependency on the connection/socket types
/// and can be unit-tested with a fake subscriber.
///
/// Lifetime is managed via `std::shared_ptr<ISubscriber>`: the registry holds
/// `std::weak_ptr` internally so a concurrent Publish that snapshotted a
/// subscriber pointer cannot race a disconnection — the upgrade-under-lock in
/// Publish either yields a live owning reference (Deliver is safe) or the
/// weak has already expired (skip). Concrete subscribers therefore derive
/// from `std::enable_shared_from_this<ISubscriber>` and live on the heap.
class ISubscriber: public std::enable_shared_from_this<ISubscriber>
{
  public:
    ISubscriber() = default;
    ISubscriber(ISubscriber const&) = delete;
    ISubscriber(ISubscriber&&) = delete;
    ISubscriber& operator=(ISubscriber const&) = delete;
    ISubscriber& operator=(ISubscriber&&) = delete;
    virtual ~ISubscriber() = default;

    /// Hand one message to this subscriber for eventual delivery. Called from
    /// any thread (the publisher's reactor thread, not necessarily this
    /// subscriber's), so implementations must be thread-safe and must not write
    /// the socket directly — they enqueue and wake their own reactor.
    /// @param message The delivery (taken by value; the subscriber owns it).
    virtual void Deliver(PushMessage message) = 0;
};

/// Process-wide publish/subscribe registry: maps channels and glob patterns to
/// the set of subscribed connections, and fans a PUBLISH out to all matching
/// subscribers. Injected (like IClock / IMetricsSink) so the protocol layer
/// depends only on this interface and tests can substitute a fake.
///
/// Thread-safety: every method is safe to call concurrently from any reactor
/// thread. A subscriber must `UnsubscribeAll` itself before it is destroyed so
/// no stale pointer remains in the maps.
class IPubSubRegistry
{
  public:
    IPubSubRegistry() = default;
    IPubSubRegistry(IPubSubRegistry const&) = delete;
    IPubSubRegistry(IPubSubRegistry&&) = delete;
    IPubSubRegistry& operator=(IPubSubRegistry const&) = delete;
    IPubSubRegistry& operator=(IPubSubRegistry&&) = delete;
    virtual ~IPubSubRegistry() = default;

    /// Subscribe `sub` to an exact channel. Takes a shared_ptr so the registry
    /// can hold a `weak_ptr` for upgrade-under-lock during Publish, ensuring
    /// the subscriber cannot be destroyed mid-fanout.
    /// @return The subscriber's new total subscription count (channels +
    ///         patterns), for the SUBSCRIBE confirmation reply.
    virtual std::size_t Subscribe(std::shared_ptr<ISubscriber> sub, std::string_view channel) = 0;

    /// Unsubscribe `sub` from an exact channel (no-op if not subscribed).
    /// @return The subscriber's remaining subscription count.
    virtual std::size_t Unsubscribe(ISubscriber* sub, std::string_view channel) = 0;

    /// Subscribe `sub` to a glob-style channel pattern.
    /// @return The subscriber's new total subscription count.
    virtual std::size_t PSubscribe(std::shared_ptr<ISubscriber> sub, std::string_view pattern) = 0;

    /// Unsubscribe `sub` from a pattern (no-op if not subscribed).
    /// @return The subscriber's remaining subscription count.
    virtual std::size_t PUnsubscribe(ISubscriber* sub, std::string_view pattern) = 0;

    /// Remove `sub` from every channel and pattern. Called on disconnect,
    /// before the subscriber object is destroyed.
    virtual void UnsubscribeAll(ISubscriber* sub) = 0;

    /// Publish `message` to `channel`, delivering to every exact subscriber and
    /// every subscriber whose pattern matches.
    /// @return The number of subscribers the message was delivered to.
    virtual std::size_t Publish(std::string_view channel, std::string_view message) = 0;

    /// Snapshot the set of channels `sub` is currently subscribed to. Returned
    /// in unspecified order; used by no-argument UNSUBSCRIBE to enumerate the
    /// channels to send confirmations for (matching redis's behaviour where
    /// bare UNSUBSCRIBE means "unsubscribe from every currently-subscribed
    /// channel").
    /// @param sub The subscriber whose channels to enumerate.
    /// @return Owned copies of the channel names currently registered for `sub`.
    [[nodiscard]] virtual std::vector<std::string> SnapshotChannels(ISubscriber* sub) const = 0;

    /// Snapshot the set of patterns `sub` is currently psubscribed to. Same
    /// semantics as `SnapshotChannels` but for PUNSUBSCRIBE.
    /// @param sub The subscriber whose patterns to enumerate.
    /// @return Owned copies of the patterns currently registered for `sub`.
    [[nodiscard]] virtual std::vector<std::string> SnapshotPatterns(ISubscriber* sub) const = 0;

    /// O(1) probe for "is anyone subscribed to anything?". Cheap fast path for
    /// publishers (notably the keyspace-notification helper) that would
    /// otherwise format channel names and call Publish even when no subscriber
    /// exists — wasteful when an operator enables `notify-keyspace-events` on
    /// a hot-write workload purely for the option to subscribe later.
    /// @return True iff at least one channel or pattern has a subscriber.
    [[nodiscard]] virtual bool HasAnySubscribers() const noexcept = 0;
};

} // namespace FastCache
