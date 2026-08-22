// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>
#include <FastCache/Core/Logger.hpp>

#include <string_view>

namespace FastCache
{

class IReactor;              // Async/IReactor.hpp — the reactor this connection is pinned to.
class IPubSubRegistry;       // Protocol/IPubSubRegistry.hpp — process-wide pub/sub registry.
class IStreamWaiterRegistry; // Protocol/IStreamWaiterRegistry.hpp — blocking stream-read coordinator.
class IMetricsSink;          // Metrics/IMetricsSink.hpp — counter sink for dispatch outcomes.
class WatchRegistry;         // Protocol/RedisTransaction.hpp — process-wide WATCH registry for Redis transactions.
class KeyspaceNotifier;      // Protocol/KeyspaceNotifier.hpp — Redis keyspace notification publisher.

namespace Distributed
{
    class WorkerRegistry; // Distributed/WorkerRegistry.hpp — the live compile-worker fleet.
    class LeaseTable;     // Distributed/LeaseTable.hpp — outstanding compile authorizations.
} // namespace Distributed

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

    /// Per-connection logger used for connection-level trace logging (the
    /// "connection accepted" line and, under `--log-everything`, non-data
    /// commands). Set by Connection to its source-decorated logger, so lines
    /// carry the client IP when `--log-source` is on. Null in tests that drive
    /// a handler directly — `LogCommand` then no-ops. Data operations are NOT
    /// logged here; they surface on the TracingStorage `storage:` line instead
    /// (see `sourceTag`), which avoids logging each GET/SET twice.
    ILogger* logger { nullptr };

    /// Source tag (bracketed client IP, e.g. "[203.0.113.7]", or empty) that
    /// handlers publish to `Detail::storageSourceTag` before each storage call
    /// so the `storage:` trace line names the client. Set by Connection; a view
    /// into the connection frame, valid for the session's lifetime.
    std::string_view sourceTag {};

    /// What the listener this connection arrived on is allowed to serve, as a
    /// `ListenerRole` mask.
    ///
    /// Set per-bind by the server loop, which already copies this struct once per
    /// `BindConfig`, so a connection carries its endpoint's policy rather than the
    /// daemon's. That is the whole point: without it the compile-cache handler
    /// cannot tell which port a frame arrived on, and "which surfaces are exposed
    /// where" stops being an operator decision.
    ///
    /// Defaults to `Cache` alone — the same fail-closed default `BindConfig` has,
    /// so a handler driven directly in a test refuses dispatch verbs unless the
    /// test says otherwise.
    std::uint8_t listenerRoles { static_cast<std::uint8_t>(ListenerRole::Cache) };

    /// Worker registry for distributed execution, or nullptr when the daemon was
    /// not built or configured to schedule. A null pointer here makes every
    /// dispatch verb refuse, which is what a cache-only daemon should do.
    Distributed::WorkerRegistry* workers { nullptr };

    /// Lease table pairing with `workers`. Null and non-null must agree: a
    /// scheduler with a registry and no leases could dispatch but never account for
    /// what it dispatched.
    Distributed::LeaseTable* leases { nullptr };

    /// Where dispatch outcomes are counted, or null when nothing collects them.
    ///
    /// Optional rather than required, because a scheduler must schedule whether or
    /// not anyone is scraping it. Every use is guarded; there is deliberately no
    /// null-object default, since a silently-discarding sink and a genuinely
    /// absent one would then be indistinguishable at the call site.
    IMetricsSink* metrics { nullptr };

    /// Maximum size, in bytes, of a single length-prefixed protocol payload (a
    /// RESP bulk string). Bounds how many bytes one command may push before the
    /// connection is dropped, capping per-request memory. The daemon raises this
    /// to at least the configured storage max-value (`--storage-max-value`) so any
    /// value the cache will accept can also be received off the wire; it defaults
    /// to the 64 MiB protocol floor for tests and handlers that leave it unset.
    std::size_t maxPayloadBytes { 64 * 1024 * 1024 };

    /// When true (`--log-everything`), non-data commands (PING, HELLO, COMMAND,
    /// AUTH, ...) are also logged at the connection level. These never reach
    /// storage, so without this flag they are invisible. Data operations are
    /// unaffected — they always appear on the `storage:` line at Trace.
    bool logEverything { false };

    /// Whether connection-level command logging is currently active — i.e. a
    /// logger is wired and its threshold admits Trace. Handlers check this
    /// before classifying a command, so the hot path pays nothing when trace
    /// logging is off.
    /// @return true when a non-data command line would actually be emitted.
    [[nodiscard]] bool CommandLogEnabled() const noexcept
    {
        return logger != nullptr && logger->MinLevel() <= LogLevel::Trace;
    }

    /// Trace-log one connection-level command line (verb, plus an optional key)
    /// through the per-connection `logger`. Used only for non-data commands
    /// under `--log-everything`; the one place that line's format lives.
    /// @param verb Command verb as the client issued it (e.g. "PING", "hello").
    /// @param key Primary argument, or empty for argument-less commands.
    void LogCommand(std::string_view verb, std::string_view key = {}) const
    {
        if (logger == nullptr)
            return;
        if (key.empty())
            logger->Logf(LogLevel::Trace, "{}", verb);
        else
            logger->Logf(LogLevel::Trace, "{} {}", verb, key);
    }

    /// Severity at which a frame-reader drop is logged, selected by error code
    /// so the classification lives in exactly one place. A peer that closed
    /// mid-frame (`Truncated`) is routine — an ordinary client disconnect — and
    /// logs at Debug so it stays quiet at the default level. Every other framing
    /// error (an oversized payload, an over-long line, a malformed frame) means
    /// the daemon actively rejected and discarded a client's bytes — e.g. a
    /// cache `SET` value too large for the wire cap — so it logs at Warn and is
    /// visible without enabling trace.
    /// @param code The framing error code that ended the command loop.
    /// @return The log severity for the drop line.
    [[nodiscard]] static constexpr LogLevel FrameDropSeverity(ProtocolErrorCode code) noexcept
    {
        return code == ProtocolErrorCode::Truncated ? LogLevel::Debug : LogLevel::Warn;
    }

    /// Log one connection-level line when the frame reader aborts a command
    /// before it reaches storage. Such a drop is otherwise invisible: storage is
    /// never called, so no TracingStorage `storage:` line is emitted and a
    /// discarded write (e.g. an oversized cache `SET`) silently vanishes from the
    /// trace — the symptom being a stream of reads with no matching store. The
    /// line names the protocol, the error code, and its context string (which for
    /// an oversized payload carries the attempted byte count and the cap), so an
    /// operator can tell a rejected write from a benign disconnect. Severity is
    /// data-driven via `FrameDropSeverity`. No-ops when no connection logger is
    /// wired (a handler driven directly in tests) or when the level is filtered.
    /// @param protocol Short protocol label for the line (e.g. "resp").
    /// @param error The framing error that ended the command loop.
    void LogFrameDrop(std::string_view protocol, ProtocolError const& error) const
    {
        if (logger == nullptr)
            return;
        logger->Logf(
            FrameDropSeverity(error.code), "{}: frame dropped, {} ({})", protocol, ToStringView(error.code), error.context);
    }

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
