// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/HostMemory.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace FastCache
{

/// fastcached's own TCP port: 6674, the leading digits of the gravitational
/// constant G = 6.674e-11.
///
/// Deliberately *not* memcached's 11211 nor redis's 6379. The daemon detects
/// the wire format per connection, so the port number selects no protocol —
/// memcached text, memcached binary, RESP and the 0xFC compile-cache protocol
/// are all served here. Borrowing another project's registered port only
/// suggested otherwise, and collided with a real memcached on the same host.
///
/// Chosen so it stays bindable everywhere: unassigned in the IANA service-name
/// registry, above the privileged floor (no CAP_NET_BIND_SERVICE), and below
/// Linux's ephemeral range (`ip_local_port_range` starts at 32768) so it cannot
/// lose a race against an outbound connection's source port.
inline constexpr std::uint16_t DefaultPort { 6674 };

/// TCP port for the admin HTTP endpoint (`/metrics`, `/healthz`). Separate from
/// `DefaultPort` so the admin surface never shares a listener with the cache
/// protocols.
inline constexpr std::uint16_t DefaultMetricsPort { 9259 };

/// One listening endpoint. Multiple `BindConfig` entries on a single daemon
/// let an operator serve plaintext on one interface (e.g. a private LAN)
/// while terminating TLS on another (e.g. the public WAN) from one process.
/// When `tls` is true the daemon must also be built with `FC_TLS_ENABLED`
/// and the legacy `tlsCertPath` / `tlsKeyPath` fields must be populated;
/// per-bind certs (SNI) are out of scope.
/// What a listener is allowed to serve.
///
/// A bitmask rather than an enumeration of endpoint kinds, because an operator
/// may legitimately want one endpoint to do both — and because the alternative,
/// hard-coding "the cache port" and "the dispatch port", is the shape that makes
/// exposure a build-time constant instead of their decision.
///
/// The default is `Cache` alone, and that default is the security posture. The
/// compile-cache surface may reasonably be reachable across a build LAN; the
/// surface that causes a compiler to *run* on someone else's machine must be
/// something an operator switched on deliberately, on an endpoint they chose, so
/// they can firewall it, require TLS on it, or simply never enable it. Serving
/// both from one listener is expressible, but it has to be spelled.
enum class ListenerRole : std::uint8_t
{
    Cache = 1U << 0,   ///< memcached / RESP / the 0xFC compile cache.
    Dispatch = 1U << 1 ///< Distributed-execution scheduling verbs.
};

/// Bitwise OR of two roles, so a mask reads as `Cache | Dispatch`.
[[nodiscard]] constexpr std::uint8_t operator|(ListenerRole a, ListenerRole b) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// Whether `mask` carries `role`.
/// @param mask The listener's role mask.
/// @param role The role being asked about.
/// @return True when the listener serves that role.
[[nodiscard]] constexpr bool HasRole(std::uint8_t mask, ListenerRole role) noexcept
{
    return (mask & static_cast<std::uint8_t>(role)) != 0;
}

/// A role's name in YAML, and the bit it sets.
///
/// A table rather than an if-ladder in the reader, for the reason every other
/// table here exists: a third role is a row, and the name a config file spells is
/// necessarily one the parser accepts. It is separate from `ListenerFlags` above
/// because the two answer different questions -- that one maps a whole *flag* to
/// a complete mask (`--listen-dispatch` means exactly Dispatch), while this maps
/// one *role name* to one bit, so `roles: [cache, dispatch]` can be composed.
struct ListenerRoleName
{
    std::string_view name; ///< Spelling accepted in `roles:`.
    ListenerRole role;     ///< The bit it contributes.
};

/// Every role name a config file may use.
inline constexpr std::array<ListenerRoleName, 2> ListenerRoleNames {
    ListenerRoleName { .name = "cache", .role = ListenerRole::Cache },
    ListenerRoleName { .name = "dispatch", .role = ListenerRole::Dispatch },
};

/// Look up a role by the name a config file spelled.
/// @param name The spelling from `roles:`.
/// @return The role, or nullopt when the name is not one of them.
[[nodiscard]] constexpr std::optional<ListenerRole> ListenerRoleFor(std::string_view name) noexcept
{
    for (auto const& row: ListenerRoleNames)
        if (row.name == name)
            return row.role;
    return std::nullopt;
}

/// One listener flag, and what an endpoint declared with it serves.
///
/// The single source of truth for the mapping between a `--listen*` spelling and
/// a `BindConfig`, in **both** directions: `CliParser` binds a parser to each row,
/// and `BuildServiceArgv` spells a bind back out by finding its row. Those two
/// must agree or a daemon registers with a listener set it will not accept back —
/// which is the class of defect `FormatListenHost` and `MaybeQuote` already exist
/// to prevent, reached through a different door.
///
/// It was a `bind.tls ? "listen-tls" : "listen"` ternary in each place while there
/// were two kinds. A third made that a three-way conditional in three files, which
/// is the shape a table replaces.
struct ListenerFlagSpec
{
    std::string_view flag; ///< Flag spelling, without the leading `--`.
    bool tls;              ///< Whether accepted sockets are TLS-wrapped.
    std::uint8_t roles;    ///< What the endpoint is permitted to serve.
};

/// Every listener flag. Order is documentation order; lookup is by content.
inline constexpr std::array<ListenerFlagSpec, 3> ListenerFlags {
    ListenerFlagSpec { .flag = "listen", .tls = false, .roles = static_cast<std::uint8_t>(ListenerRole::Cache) },
    ListenerFlagSpec { .flag = "listen-tls", .tls = true, .roles = static_cast<std::uint8_t>(ListenerRole::Cache) },
    // Deliberately NOT `Cache | Dispatch`: the point of a separate endpoint is that
    // the surface which causes a compiler to run elsewhere can be reached,
    // firewalled and TLS-required independently of the one serving a cache.
    ListenerFlagSpec { .flag = "listen-dispatch", .tls = false, .roles = static_cast<std::uint8_t>(ListenerRole::Dispatch) },
};

struct BindConfig
{
    /// Bind address: IPv4/IPv6 literal or hostname. `::` selects dual-stack.
    std::string address {};
    /// TCP port (1..65535).
    std::uint16_t port { 0 };
    /// What this endpoint serves. Defaults to the cache alone; see `ListenerRole`
    /// for why distributed execution is opt-in per endpoint rather than global.
    std::uint8_t roles { static_cast<std::uint8_t>(ListenerRole::Cache) };
    /// When true, accepted sockets on this endpoint are wrapped through
    /// `TlsWrap` before the protocol handler ever sees a byte.
    bool tls { false };

    /// Structural equality. Used by ConfigReloader::ValidateImmutable
    /// (the listener set is live-wired at startup; a SIGHUP that mutates
    /// it must reject), plus by tests asserting parsed binds verbatim.
    friend bool operator==(BindConfig const&, BindConfig const&) = default;
};

/// Which listener flag spells `bind`.
///
/// Falls back to plain `listen` when no row matches, which is the safe direction:
/// a bind whose role mask this build does not recognise is re-registered as an
/// ordinary cache endpoint rather than silently gaining a dispatch surface it was
/// never given.
/// @param bind The endpoint to spell.
/// @return The flag name, without the leading `--`.
[[nodiscard]] constexpr std::string_view ListenFlagFor(BindConfig const& bind) noexcept
{
    for (auto const& row: ListenerFlags)
        if (row.tls == bind.tls && row.roles == bind.roles)
            return row.flag;
    return ListenerFlags.front().flag;
}

/// Durability policy for the persistent storage backend. Decoupled from
/// the storage subsystem so the Config layer does not depend on Cache
/// internals; main.cpp translates this into the backend's enum.
enum class StorageDurability : std::uint8_t
{
    Fsync = 0,   ///< fsync after every write. Slowest, safest.
    Batched = 1, ///< Buffer writes, fsync at commit boundaries (default).
    None = 2,    ///< OS page cache only; no fsync.
};

/// Which supervisor domain `--install-service` registers the daemon in.
///
/// Deliberately *not* a Config field, only a vocabulary type living beside the
/// others: it configures the installation, not the running daemon. As a Config
/// field it would also make BuildServiceArgv's "emit every field that differs
/// from the default" rule bake a meaningless `--service-scope` into the
/// registered job's own arguments. See CliResult::serviceScope.
///
/// Meaningful on macOS (launchd has two domains); ignored on Windows, whose SCM
/// has only one.
enum class ServiceScope : std::uint8_t
{
    /// A LaunchAgent in the invoking user's `~/Library/LaunchAgents`, running as
    /// that user from their next login onwards. Needs no privileges.
    User = 0,
    /// A LaunchDaemon in `/Library/LaunchDaemons`, running as a dedicated
    /// service account from boot, before anyone logs in. Requires root.
    System = 1,
};

/// CPU-affinity policy for the reactor worker threads.
enum class CpuAffinity : std::uint8_t
{
    /// Let the OS scheduler place reactor threads (default for a lone reactor).
    None = 0,
    /// Pin each reactor thread to its own core (default with >1 reactor): keeps
    /// per-worker state cache-resident and avoids cross-core migration churn.
    PerCore = 1,
};

/// In-memory LRU recency policy. Decoupled from the Cache layer (like
/// StorageDurability); main.cpp translates this into the backend's `LruMode`.
enum class LruRecency : std::uint8_t
{
    /// Sampled/deferred promotion: reads run concurrently under a shared lock
    /// and eviction stays approximately-LRU. Favours read throughput. Default.
    Approximate = 0,
    /// Promote on every read for exact LRU order; reads on one shard serialise.
    Strict = 1,
};

/// All runtime configuration. POD-like value type; built once from CLI
/// arguments (and later, from a YAML config file). For SIGHUP reload, the
/// daemon keeps a shared_ptr<const Config> and atomically swaps.
struct Config
{
    /// In-memory storage byte budget. 0 = unbounded (testing/dev only).
    /// Scales with the machine rather than being a fixed constant; see
    /// DefaultMaxMemoryBytes() for the fraction and the bounds.
    std::size_t maxMemoryBytes { DefaultMaxMemoryBytes() };

    /// Maximum size of a single cache value in bytes, enforced by every
    /// storage backend (in-memory LRU and on-disk COW tree).
    /// Set/Add/Replace/CompareAndSwap/Append/Prepend that would exceed
    /// this return StorageErrorCode::ValueTooLarge.
    ///
    /// Sized for the compile-cache workload this daemon exists to serve:
    /// object files routinely pass 16 MiB, and the entries a cap rejects are
    /// precisely the expensive-to-recompute ones it was worth caching. The
    /// cap costs nothing until it is used -- a value does not need a page of
    /// its own, it spills into an overflow-page chain over the fixed 16 KiB
    /// store page size.
    ///
    /// It also raises the wire payload cap, but only for the protocols that
    /// take it from the session: the compile-cache protocol and RESP. Both
    /// memcached framings keep their own hardcoded 16 MiB ceiling.
    std::size_t storageMaxValueBytes { 256 * 1024 * 1024 };

    /// Maximum bytes the on-disk (L2) tier may hold, across all shards
    /// (the `--storage-max-disk` flag). Only meaningful with `--storage`.
    /// 0 (the default) means unbounded — the disk file grows to whatever the
    /// cached content needs. When non-zero, the budget is split evenly across
    /// the physical shards and each shard's CoW tree evicts its LRU tail to
    /// stay within its share, so the total on-disk footprint is capped. Set
    /// this to keep a build cache within a fixed disk allotment.
    std::size_t storageMaxDiskBytes { 0 };

    /// Number of independent pinned reactors to run (the `--threads`
    /// flag). Each reactor is a single-threaded event loop; connections
    /// are pinned to one for their lifetime, so this is the server's
    /// across-core parallelism. 0 means "use
    /// std::thread::hardware_concurrency()".
    std::size_t workerThreads { 0 };

    /// Number of storage shards. 1 means "do not shard" (preserves
    /// PR #10 single-file storage behaviour). When >1 and `storagePath`
    /// is set, the path is treated as a directory containing
    /// `shard-NN.cow` files. 0 means "auto" — defaults to a sensible
    /// value at runtime (min(16, hardware_concurrency)).
    std::size_t storageShards { 0 };

    /// Bind address: IPv4/IPv6 literal or hostname. Default 127.0.0.1
    /// (IPv4 loopback). An IPv6 wildcard (`::`) binds dual-stack and
    /// serves both IPv4 and IPv6 on every platform.
    ///
    /// Legacy single-bind shorthand. When `binds` is non-empty this field
    /// is ignored: every endpoint is described as a `BindConfig` instead.
    /// When `binds` is empty (the common case), main.cpp synthesises a
    /// single `BindConfig { bindAddress, port, tlsEnabled }` so the rest
    /// of the daemon body sees only the data-driven view.
    std::string bindAddress { "127.0.0.1" };

    /// Path of the YAML config file (if any) that produced this Config.
    /// Used by ConfigReloader on SIGHUP. Empty means no file-backed config.
    std::string configPath {};

    /// Optional pidfile path (POSIX daemon mode only).
    std::string pidfile {};

    /// Windows service name; defaults to FastCached.
    std::string serviceName { "FastCached" };

    /// Shared authentication secret (redis `requirepass` style). Empty (the
    /// default) means authentication is disabled and every client is served
    /// without a credential check. When set, clients must authenticate before
    /// any data command: redis via `AUTH`, the memcached binary protocol via
    /// SASL PLAIN. The memcached text protocol has no auth handshake, so it
    /// then rejects data commands.
    std::string requirePass {};

    /// Expected username for the two-argument auth form (redis
    /// `AUTH <user> <pass>` and SASL PLAIN authcid). Defaults to "default",
    /// mirroring redis's default ACL user. Only consulted when `requirePass`
    /// is non-empty.
    ///
    /// Note: the one-argument redis form `AUTH <pass>` authenticates the default
    /// user against `requirePass` alone and does NOT enforce this username
    /// (standard redis semantics). The username only constrains the two-argument
    /// `AUTH <user> <pass>` form and SASL PLAIN, where the supplied authcid must
    /// match it.
    std::string authUsername { "default" };

    /// Optional path to a persistent storage file. When set, the
    /// daemon uses a CoW-tree storage backed by this file; when empty,
    /// the cache is in-memory only.
    std::string storagePath {};

    /// TCP port. Defaults to fastcached's own `DefaultPort`; see the constant
    /// for why the number is ours rather than memcached's or redis's.
    std::uint16_t port { DefaultPort };

    /// Bind address for the admin HTTP endpoint (`/metrics`, `/healthz`).
    /// Defaults to loopback so metrics are not exposed to the world unless the
    /// operator opts in. Only used when `metricsEnabled` is true.
    std::string metricsBindAddress { "127.0.0.1" };

    /// TCP port for the admin HTTP endpoint. Served on a dedicated port so it
    /// never collides with the cache protocols. Only used when `metricsEnabled`.
    std::uint16_t metricsPort { DefaultMetricsPort };

    /// Whether to start the admin HTTP endpoint (Prometheus `/metrics` plus a
    /// `/healthz` liveness probe). Off by default.
    bool metricsEnabled { false };

    /// Whether to terminate TLS on the cache port. Off by default. Requires a
    /// build with `FASTCACHED_ENABLE_TLS=ON` and both `tlsCertPath`/`tlsKeyPath`
    /// set; the daemon fails fast at startup otherwise.
    bool tlsEnabled { false };

    /// PEM certificate (chain) file for TLS. Only used when `tlsEnabled`.
    std::string tlsCertPath {};

    /// PEM private key file for TLS. Only used when `tlsEnabled`.
    std::string tlsKeyPath {};

    /// Redis-style keyspace-event flag string. Empty (the default) disables
    /// every keyspace notification. Each letter enables one event class
    /// (matches redis-server's `notify-keyspace-events`):
    ///   K  publish on `__keyspace@<db>__:<key>` channels
    ///   E  publish on `__keyevent@<db>__:<event>` channels
    ///   g  generic events: del / expire / persist
    ///   $  string events: set
    ///   A  alias for `g$` — everything we currently emit
    /// `x` (expiration events) is documented by Redis but not yet
    /// implemented here — the storage layer has no expiry callback, so
    /// the parser rejects `x` until the NotifyingStorage decorator lands
    /// (see TODO.md). Operators who set `x` get a startup error rather
    /// than the silent miss the original branch shipped with.
    /// Each enabled write verb fires `__keyspace@0__:<key> <event>` (with K)
    /// and `__keyevent@0__:<event> <key>` (with E). At least one of K or E
    /// must be set for any event to actually be published.
    /// The database index is fixed at 0 (this daemon does not implement
    /// `SELECT`).
    /// Unknown letters cause main.cpp to fail fast at startup.
    std::string notifyKeyspaceEvents {};

    /// Listener endpoints. When empty (the default), main.cpp collapses the
    /// legacy single-bind fields (`bindAddress`, `port`, `tlsEnabled`) into
    /// a single synthesised `BindConfig` and proceeds. When non-empty,
    /// every entry defines one endpoint and the legacy single-bind fields
    /// are ignored. The CLI populates this vector from repeatable
    /// `--listen address:port` / `--listen-tls address:port` flags; the
    /// YAML reader populates it from the top-level `listeners:` list.
    std::vector<BindConfig> binds {};

    /// ::listen() backlog — the depth of the kernel's queue of accepted-
    /// but-not-yet-handed-off connections. Bursts of parallel clients (a
    /// `make -jN` driving sccache opens many sockets at once) overflow a
    /// small backlog and get ECONNREFUSED / timeouts at the OS layer before
    /// the daemon ever sees them. Defaults to 511 (the value redis uses);
    /// the kernel silently clamps to its own SOMAXCONN ceiling.
    int listenBacklog { 511 };

    /// Log threshold.
    LogLevel logLevel { LogLevel::Info };

    /// When true, each ConsoleLogger line is prefixed with an ISO 8601 UTC timestamp.
    bool logTimestamps { false };

    /// When true, each connection-scoped log line is prefixed with the client
    /// IP that caused it (e.g. `[INFO] [203.0.113.7] ...`). Global/server log
    /// lines, which have no client, are unaffected.
    bool logSource { false };

    /// When true, the Trace-level command (access) log includes every command,
    /// not just keyspace data operations — i.e. also connection/keepalive/admin
    /// chatter such as PING, COMMAND, HELLO, CLIENT, AUTH. Off by default so the
    /// command log stays focused on GET/SET-style traffic.
    bool logEverything { false };

    /// If true, daemonize (POSIX) or self-register as a Windows service.
    bool daemon { false };

    /// Durability mode for the persistent backend (ignored when
    /// storagePath is empty).
    StorageDurability storageDurability { StorageDurability::Batched };

    /// In-memory LRU recency policy. Approximate (default) favours read
    /// throughput by letting same-shard reads run concurrently; Strict gives
    /// exact LRU order at the cost of serialising reads per shard.
    LruRecency lruRecency { LruRecency::Approximate };

    /// CPU-affinity policy for reactor threads. PerCore (default) pins each
    /// reactor to its own core when running more than one; with a single
    /// reactor it is a no-op regardless. None lets the scheduler place threads.
    CpuAffinity cpuAffinity { CpuAffinity::PerCore };

    /// Codec applied to values before they are written to the persistent
    /// backend (ignored when storagePath is empty). Zstd by default; set to
    /// Identity ("none") to store values verbatim. Reads always return
    /// plaintext, so the codec can be changed between runs with no migration —
    /// each record is decoded by its own stored codec tag.
    CompressionCodec compression { CompressionCodec::Zstd };

    /// Effort level for `compression` (higher = smaller/slower). Ignored by
    /// codecs without a level. Defaults to zstd level 3.
    int compressionLevel { 3 };

    /// Values smaller than this are never compressed (their CPU cost is not
    /// worth it and tiny values rarely shrink). Defaults to 256 bytes.
    std::size_t compressionMinBytes { 256 };

    /// Codec for the IN-MEMORY (L1) tier, independent of the on-disk `compression`
    /// above. `Identity` (the default) keeps reads allocation-free; anything else
    /// trades a decompress on every read for a much larger effective cache.
    ///
    /// Worth enabling when the working set exceeds RAM: compile-cache objects with
    /// embedded debug info compress ~4.5x, so a 27 GB working set becomes ~6 GB, at
    /// ~3.5 ms per 3.3 MB value read. Leave it off for small values or
    /// latency-critical reads, where the decompress dominates.
    CompressionCodec memoryCompression { CompressionCodec::Identity };

    /// Effort level for `memoryCompression`. Level 3 is the measured speed/ratio
    /// knee; higher levels cost much more to write for a few percent of size.
    int memoryCompressionLevel { 3 };

    /// Values smaller than this stay uncompressed in memory.
    std::size_t memoryCompressionMinBytes { 4096 };

    /// Member-wise equality. Lets a test assert that two ways of spelling the
    /// same thing — `--flag=value` and `--flag value`, or a CLI flag and its
    /// YAML key — produce an identical configuration, rather than spot-checking
    /// whichever fields the test author happened to think of.
    friend bool operator==(Config const&, Config const&) = default;
};

} // namespace FastCache
