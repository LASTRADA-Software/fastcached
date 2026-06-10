// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Auth/AuthPolicy.hpp>

namespace FastCache
{

class IReactor;              // Async/IReactor.hpp — the reactor this connection is pinned to.
class IPubSubRegistry;       // Protocol/IPubSubRegistry.hpp — process-wide pub/sub registry.
class IStreamWaiterRegistry; // Protocol/IStreamWaiterRegistry.hpp — blocking stream-read coordinator.
class WatchRegistry;         // Protocol/RedisTransaction.hpp — process-wide WATCH registry for Redis transactions.
class KeyspaceNotifier;      // Protocol/KeyspaceNotifier.hpp — Redis keyspace notification publisher.

/// Per-server, immutable context handed to every protocol handler's command
/// loop. Bundles the optional collaborators a connection needs beyond its
/// socket and the shared cache engine.
///
/// Nullable members mean "feature off": a null `auth` means authentication is
/// disabled and every command is served without a credential check. The
/// referenced objects are owned by the daemon body and outlive every
/// connection, so handlers and connections hold borrowed pointers only — the
/// struct itself is a cheap value, copied by reference-sized members.
struct SessionContext
{
    /// Authentication source. Indirected through `IAuthSource` so the daemon
    /// can atomically swap the active `AuthPolicy` on SIGHUP (live secret
    /// rotation) without restarting connections. A null pointer here means
    /// "auth is permanently disabled for this server"; a non-null source
    /// whose `Current()` returns null means "auth is currently disabled but
    /// could be enabled later by a reload" — both cases skip the credential
    /// check at the handler level.
    IAuthSource* authSource { nullptr };

    /// Process-wide publish/subscribe registry, or nullptr when pub/sub is not
    /// wired in (e.g. unit tests that don't exercise it). Shared, read-mostly,
    /// thread-safe; owned by the daemon body.
    IPubSubRegistry* pubsub { nullptr };

    /// Process-wide registry coordinating blocking XREAD/XREADGROUP reads with
    /// XADD-side appends, or nullptr when blocking reads are not wired in (unit
    /// tests, or a build without the coordinator). A null pointer makes the
    /// Redis handler serve XREAD/XREADGROUP non-blockingly (poll once) instead
    /// of parking the connection. Shared, thread-safe; owned by the daemon body.
    IStreamWaiterRegistry* streamWaiters { nullptr };

    /// The reactor this connection is pinned to, used by a subscriber to wake
    /// its own command loop when a message is delivered from another reactor
    /// thread. Null when there is no reactor (blocking/in-memory transports);
    /// pub/sub delivery then resumes inline on the same thread.
    IReactor* reactor { nullptr };

    /// Process-wide WATCH registry for the Redis `WATCH`/`MULTI`/`EXEC`
    /// transaction family. Null when transactions are not wired in (unit
    /// tests that don't exercise them); the Redis handler then rejects WATCH
    /// with the standard error and the MULTI/EXEC flow runs without
    /// invalidation. Shared, thread-safe; owned by the daemon body.
    WatchRegistry* watches { nullptr };

    /// Redis keyspace-notification publisher. Null when notifications are
    /// disabled (the default) or unwired (tests). A non-null pointer with
    /// `IsEnabled() == false` is also a valid "off" state. Owned by the
    /// daemon body; outlives every connection.
    KeyspaceNotifier* keyspaceNotifier { nullptr };

    /// Resolve the currently-active policy through `authSource`. Callers hold
    /// the returned shared_ptr for the lifetime of a single verify so a
    /// concurrent reload cannot pull the rug out from under them.
    /// @return The active policy, or null when auth is disabled.
    [[nodiscard]] std::shared_ptr<AuthPolicy const> CurrentAuth() const noexcept
    {
        return authSource != nullptr ? authSource->Current() : std::shared_ptr<AuthPolicy const> {};
    }
};

} // namespace FastCache
