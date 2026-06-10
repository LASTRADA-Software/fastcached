// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Protocol/IPubSubRegistry.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <string_view>

namespace FastCache
{

/// Bitmask of enabled keyspace-notification classes. Mirrors redis's
/// `notify-keyspace-events` bitmask, but restricted to the event types this
/// daemon actually emits. The flags compose with bitwise OR.
namespace KeyspaceEvents
{
    constexpr std::uint32_t None = 0;
    constexpr std::uint32_t Keyspace = 1U << 0; ///< `K` — publish to __keyspace@N__:key
    constexpr std::uint32_t Keyevent = 1U << 1; ///< `E` — publish to __keyevent@N__:event
    constexpr std::uint32_t Generic = 1U << 2;  ///< `g` — del / expire / persist
    constexpr std::uint32_t String = 1U << 3;   ///< `$` — set
    /// `x` — expired (lazy-expiry). The flag bit is reserved but unwired: the
    /// daemon currently has no expiry callback at the storage layer, so no
    /// code path can publish an `expired` event. Once the
    /// `NotifyingStorage` decorator lands (see TODO.md), restore `x` to
    /// `FlagTable` in KeyspaceNotifier.cpp and include `Expired` in `All`.
    constexpr std::uint32_t Expired = 1U << 4;

    /// `A` is shorthand for every class type the daemon currently emits.
    /// `Expired` is intentionally absent — see the note on `Expired` above.
    constexpr std::uint32_t All = Generic | String;
} // namespace KeyspaceEvents

/// Parse a redis-style keyspace-event flag string into a bitmask. Each
/// character maps to a `KeyspaceEvents::*` bit; unknown letters return a
/// ConfigError so main.cpp can fail fast.
/// @param flags The raw flag string (e.g. "AKE", "Kg$").
/// @return The bitmask on success, or a ConfigError on an unknown letter.
[[nodiscard]] std::expected<std::uint32_t, ConfigError> ParseKeyspaceEvents(std::string_view flags);

/// Fan-out helper: every Redis write verb calls `OnEvent(key, "set"|"del"|...)`
/// after its engine mutation succeeds; the notifier consults the bitmask and
/// publishes to whichever of `__keyspace@0__:<key>` / `__keyevent@0__:<event>`
/// the operator enabled. Cheap when off (one bit test and an empty bitmask
/// suppresses the channel build entirely); the publish itself is a hashed
/// channel lookup with no subscribers, so the steady-state cost on a daemon
/// nobody is subscribing to is one branch per write.
///
/// Database index is fixed at 0 (no `SELECT` in this daemon).
class KeyspaceNotifier
{
  public:
    /// Construct a notifier wired to the daemon's pub/sub registry.
    /// @param pubsub  Process-wide registry (must outlive the notifier),
    ///                or nullptr to disable publication entirely (tests).
    /// @param classes Enabled-event bitmask, parsed from `Config::notifyKeyspaceEvents`.
    KeyspaceNotifier(IPubSubRegistry* pubsub, std::uint32_t classes) noexcept;

    /// Fire one keyspace event. No-op when the relevant class is disabled.
    /// @param classFlag  One of `KeyspaceEvents::Generic` / `String` / `Expired`
    ///                   — which class this event belongs to.
    /// @param event      The event name (e.g. "set", "del", "expire", "expired").
    /// @param key        The key the event is associated with.
    void OnEvent(std::uint32_t classFlag, std::string_view event, std::string_view key) const;

    /// @return True iff at least one class is enabled (callers can avoid
    /// building string-formatted event names when this is false).
    [[nodiscard]] bool IsEnabled() const noexcept;

    /// @return The currently-active class bitmask. Atomic load — the
    /// daemon's ConfigReloader may swap this on SIGHUP via SetClasses.
    [[nodiscard]] std::uint32_t Classes() const noexcept;

    /// Atomically replace the active class bitmask. Called by
    /// ConfigReloader when the operator changes `notify-keyspace-events`
    /// at runtime. New connections AND existing connections (which read
    /// `Classes()` on each OnEvent / IsEnabled call) observe the new mask
    /// on their next probe — no restart required.
    ///
    /// Per-connection `state->keyspaceEnabled` is a separate snapshot
    /// cached at connection start; that's documented as fixed-for-life
    /// because flipping the off-fast-path under an existing connection
    /// would surprise mid-flight handlers. New connections see the
    /// updated mask immediately.
    /// @param classes New bitmask (KeyspaceEvents::None disables).
    void SetClasses(std::uint32_t classes) noexcept;

    /// Lock-free fast-path probe: would a `OnEvent` of `classFlag` plausibly
    /// publish something? True iff this notifier is enabled, the class is
    /// in the active bitmask, and the pub/sub registry has at least one
    /// subscriber. Callers in per-key loops (MSET / MSETNX / DEL) probe
    /// this ONCE per command and skip the per-key OnEvent entirely when
    /// false — replacing N atomic loads with 1 on the hot write path.
    /// @param classFlag One of `KeyspaceEvents::Generic` / `String` / `Expired`.
    [[nodiscard]] bool WouldPublish(std::uint32_t classFlag) const noexcept;

  private:
    IPubSubRegistry* _pubsub;
    std::atomic<std::uint32_t> _classes;
};

} // namespace FastCache
