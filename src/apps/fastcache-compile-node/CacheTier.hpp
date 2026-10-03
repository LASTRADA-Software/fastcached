// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProxy.hpp"
#include "FrameEndpoint.hpp"
#include "LocalCache.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "Responders.hpp"
#include "SharedCacheDirectory.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

class NodeIoLoop;
class NodeProofClient;
class SharedCacheHost;

/// Which upstream a node's private tier reads through to. **PRIVATE: in-process only**, never
/// transmitted or persisted, so it carries no values.
enum class UpstreamKind : std::uint8_t
{
    None,             ///< No `--upstream`, and no identity to prove to the fleet's shared cache with.
    Daemon,           ///< `--upstream`: a fastcached, or anything speaking its verbs. Presents `--requirepass`.
    FleetSharedCache, ///< The fleet's `shared-cache` setting, whichever machine it names -- this one included.
    InProcessShared,  ///< This machine's own shared tier, read in process: the other half of the setting.
    Last,             ///< Not a kind: the length of a table keyed by one.
};

/// What `MakeCacheUpstream` chooses between and builds from. Borrowed throughout: every member
/// outlives the upstream built from it.
///
/// **One loop, and nothing that could name a second.** The shared cache's dialer connects on
/// `io`'s connector and its session pool keeps sessions on `io`'s reactor; a record that also
/// carried a connector would let the two be different loops, which no type could then catch
/// (`SharedSessionPool`'s own note says what that breaks).
struct UpstreamParts
{
    std::string_view upstream;                 ///< `--upstream`, or empty.
    ICredentialSource const& credential;       ///< Presented on the `--upstream` leg ONLY.
    ISharedCacheTargetSource const& directory; ///< What the fleet's setting resolves to now.
    NodeProofClient const* prover;             ///< This machine's identity, or null when it holds none.
    NodeIoLoop& io;                            ///< Connector, reactor and resolver: ONE loop.
    core::platform::IClock& clock;             ///< The upstream's refreshes and idle ages.
    IMetricsSink& metrics;                     ///< Where each upstream counts.
    NodeConditions* conditions;                ///< Where `shared-cache-unproven` is answered; may be null.
    ILogger& logger;                           ///< Where each upstream reports.
    SharedCacheHost const* host;               ///< This machine's shared tier, for the in-process half; may be null.
};

/// Builds one kind's upstream. Never returns null.
using UpstreamBuilder = std::unique_ptr<ICacheUpstream> (*)(UpstreamParts const& parts);

/// One row per kind: which verb pair it speaks, how the startup line names it, and how it is built.
struct UpstreamKindRow
{
    UpstreamKind kind;                                 ///< The kind this row describes.
    std::optional<CompileCacheWire::CacheVerbs> verbs; ///< Absent for the two that send nothing.
    std::string_view described;                        ///< What the startup line says after `upstream`.
    bool namesEndpoint;                                ///< Whether that is preceded by `--upstream`'s value.
    UpstreamBuilder build;                             ///< Never null.
};

/// @param parts Unused.
/// @return `NoUpstream`.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildNoUpstream(UpstreamParts const& parts);

/// @param parts `upstream`, `credential`, `io`, `clock` and `logger`.
/// @return A `RemoteUpstream` to `--upstream`, presenting `--requirepass`.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildRemoteUpstream(UpstreamParts const& parts);

/// The fleet's shared cache as the setting names it: a proven session to another machine and,
/// where there is a host, this machine's own tier in process -- chosen per operation by the
/// directory (`SwitchingSharedUpstream`, in `CacheTier.cpp`).
/// @param parts `prover` must not be null.
/// @return The upstream.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildSharedCacheUpstream(UpstreamParts const& parts);

/// @param parts `host`; a null host builds `NoUpstream`, a machine that cannot serve the tier.
/// @return An `InProcessSharedUpstream` over the host.
[[nodiscard]] std::unique_ptr<ICacheUpstream> BuildInProcessSharedUpstream(UpstreamParts const& parts);

/// One row per `UpstreamKind`, in enumerator order.
inline constexpr EnumTable<UpstreamKind, UpstreamKindRow> UpstreamKindTable { {
    { .kind = UpstreamKind::None,
      .verbs = std::nullopt,
      .described = "none",
      .namesEndpoint = false,
      .build = &BuildNoUpstream },
    { .kind = UpstreamKind::Daemon,
      .verbs = CompileCacheWire::DaemonCacheVerbs,
      .described = "(override, fastcached verbs)",
      .namesEndpoint = true,
      .build = &BuildRemoteUpstream },
    { .kind = UpstreamKind::FleetSharedCache,
      .verbs = CompileCacheWire::FleetSharedCacheVerbs,
      .described = "shared-cache (the fleet setting; resolved at every apply)",
      .namesEndpoint = false,
      .build = &BuildSharedCacheUpstream },
    { .kind = UpstreamKind::InProcessShared,
      .verbs = std::nullopt,
      .described = "shared-cache (this machine's own tier, in process)",
      .namesEndpoint = false,
      .build = &BuildInProcessSharedUpstream },
} };
static_assert(RowsInEnumeratorOrder(UpstreamKindTable, &UpstreamKindRow::kind),
              "UpstreamKindTable must hold one row per UpstreamKind, in enumerator order");

/// The kind this node's private tier reads through to: the ONE decision, which `MakeCacheUpstream`
/// only looks up.
///
/// `--upstream` wins -- the operator typed that address on this machine, and it is fixed for the
/// process (`Reloadable::No`) -- then the fleet's setting for a node with an identity to prove,
/// then nothing. Never `InProcessShared`: that half is built beside the fleet's, and which of the
/// two answers is the directory's to say per operation, since the setting can name this machine
/// or another at runtime.
/// @param parts What the node has.
/// @return The kind.
[[nodiscard]] UpstreamKind UpstreamKindOf(UpstreamParts const& parts) noexcept;

/// The private tier's upstream, as the kind's row builds it.
/// @param parts What the node has.
/// @return The upstream; never null.
[[nodiscard]] std::unique_ptr<ICacheUpstream> MakeCacheUpstream(UpstreamParts const& parts);

/// The node's cache surface: storage, upstream, read-through logic, protocol and
/// listener, owned as one thing.
///
/// A class rather than six locals in `WorkerBody`, for two reasons and the second is
/// the one that matters. They form a *reference chain* -- the endpoint holds the
/// responder, which holds the proxy, which holds the cache, which holds the storage
/// and the upstream -- so their declaration order in a function body is load-bearing
/// and silently so; getting it wrong is a dangling reference rather than a compile
/// error. Here the order is the member order, which is checked by the language.
///
/// And `WorkerBody` reached a cognitive complexity of 67 against clang-tidy's limit
/// of 60 when this was inline. That number is a symptom worth listening to rather
/// than a rule to argue with: it went over because a coherent decision with one
/// answer -- "does this node cache, and where" -- had been spread across a
/// function that already had several.
class CacheTier
{
  public:
    /// Build the tier and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's error enums,
    /// the same departure `AdminEndpoint::Start` documents and for the same reason:
    /// this fails in ways belonging to two taxonomies and the caller's response is
    /// identical either way.
    /// The storage arrives already built, rather than being assembled in here, and
    /// that is what keeps two unequal failures apart. A store that will not open is
    /// always fatal; a port that will not bind is fatal only when the operator typed
    /// the address. Both reduce to a diagnostic string, so building the store inside
    /// this function would leave `StartCacheTierOrExplain` unable to tell which it
    /// was holding — and a bad `--cache-dir` on a node using the default cache port
    /// would be logged as a warning and stepped over.
    /// @param cfg The parsed configuration.
    /// @param storage Where this tier keeps objects; already opened.
    /// @param upstream What the tier's upstream is built from, by `MakeCacheUpstream`; the startup
    ///        line names the same kind, so the two cannot describe different choices. Every part
    ///        must outlive the tier.
    /// @param locality Decides whether a caller is on this machine -- the whole of
    ///        who may read this tier, since #287; must outlive it.
    /// @param clock Time source for the tier's expiry; must outlive the tier.
    /// @param metrics Where hits, misses, upstream outcomes and locality refusals
    ///        are counted.
    /// @param logger Where to announce what the tier holds.
    /// @return The tier, or why it could not be built.
    ///
    /// **Binds nothing since #290.** The cache verbs are answered on the node's one
    /// `0xFC` listener, beside the scheduler's, so the tier is a component this
    /// process owns rather than a surface it opens -- and what opens that listener is
    /// `StartNodeSurfaceOrExplain`, which is also where a bind failure is judged.
    [[nodiscard]] static std::expected<std::unique_ptr<CacheTier>, std::string> Start(NodeConfig const& cfg,
                                                                                      std::unique_ptr<IStorage> storage,
                                                                                      UpstreamParts const& upstream,
                                                                                      ILocalityOracle const& locality,
                                                                                      core::platform::IClock& clock,
                                                                                      IMetricsSink& metrics,
                                                                                      ILogger& logger);

    ~CacheTier() = default;

    CacheTier(CacheTier const&) = delete;
    CacheTier& operator=(CacheTier const&) = delete;
    CacheTier(CacheTier&&) = delete;
    CacheTier& operator=(CacheTier&&) = delete;

    /// What answers the cache verbs on this node's `0xFC` listener.
    ///
    /// Handed to `MergedResponder`, which routes each frame to the component owning
    /// its verb family. The tier owns no listener of its own (#290).
    /// @return This tier's responder; outlives no longer than the tier.
    [[nodiscard]] IFrameResponder& Responder() noexcept
    {
        return _responder;
    }

    /// What this node's cache holds, as one figure.
    ///
    /// Safe from any thread, which is the whole reason the storage below is behind
    /// a `ShardedStorage`: the tier is mutated on the reactor thread while the
    /// heartbeat thread and the `/metrics` scrape read it.
    /// @return The cache's own statistics.
    [[nodiscard]] StorageStats Snapshot() const noexcept
    {
        return _storage->Snapshot();
    }

    /// The same, kept apart by the tier holding each number.
    ///
    /// What `Snapshot()` cannot say when both halves are configured: the composite
    /// reports the on-disk store's item count, bytes and budget, so the in-memory
    /// half above it leaves no trace. See `IStorage::SnapshotTiers` for why these
    /// are per-tier answers rather than a total waiting to be summed.
    /// @return One entry per tier this node configured.
    [[nodiscard]] TieredStorageStats SnapshotTiers() const noexcept
    {
        return _storage->SnapshotTiers();
    }

    /// Whether this tier reads through to a shared cache.
    ///
    /// Read off the upstream that was actually BUILT rather than off the
    /// configuration that asked for one, the same way `StorageTiersOf` reads the
    /// storage: those are the two things that can disagree, and the scrape must
    /// describe what is running.
    /// @return True when a shared cache is configured.
    [[nodiscard]] bool HasUpstream() const noexcept
    {
        return _upstream->Configured();
    }

    /// The upstream this tier reads through to, for whoever tells it the cluster applied a state.
    /// @return The upstream; outlives no longer than the tier.
    [[nodiscard]] ICacheUpstream& Upstream() noexcept
    {
        return *_upstream;
    }

    /// @return How reaching the fleet's shared cache went, where this tier reads through to it;
    ///         null otherwise. See `ICacheUpstream::SharedCacheStatus`.
    [[nodiscard]] ISharedCacheStatusSource const* SharedCacheStatus() const noexcept
    {
        return _upstream->SharedCacheStatus();
    }

  private:
    CacheTier(std::unique_ptr<IStorage> storage,
              std::unique_ptr<ICacheUpstream> upstream,
              ILocalityOracle const& locality,
              core::platform::IClock& clock,
              IMetricsSink& metrics);

    // Declaration order IS construction order, and every one of these is referenced
    // by the one below it. Reordering them is a dangling reference, which is why
    // they live here rather than as locals somebody has to keep in the right order.
    std::unique_ptr<IStorage> _storage;
    std::unique_ptr<ICacheUpstream> _upstream;
    LocalCache _cache;
    CacheProxy _proxy;
    CacheResponder _responder;
};

/// What the on-disk half's B+tree is called inside `--cache-dir`.
///
/// A file inside the directory rather than the directory itself, because the
/// operator named a place to keep a cache and this tier is entitled to put more than
/// one thing there later. It also keeps `--cache-dir` safe to point at a path that
/// does not exist yet, which is what an operator will do.
///
/// Named in the header rather than kept private to the tier because a test that
/// measures the store FILE has to open it: a second spelling of the name is a test
/// that survives a rename by failing with a message about a missing path rather than
/// about the rename.
constexpr std::string_view DiskStoreFileName = "objects.cow";

/// Where an on-disk store lives, how large it may grow and how it compresses.
///
/// One record for both tiers a node can open on disk -- its private tier under
/// `--cache-dir` and the fleet's shared tier under `--cluster-dir` -- so the two differ
/// by these values and by nothing a second copy of the opening code could get wrong.
struct DiskStoreSpec
{
    std::filesystem::path path; ///< The store file; its directory is created if missing.
    std::uint64_t maxBytes {};  ///< The store's ceiling; zero grows as needed.
    CompressionCodec codec {};  ///< The EFFECTIVE codec, from `Compression::EffectiveCodec`.
    int level {};               ///< The codec's effort.
    std::size_t minBytes {};    ///< Values below this are stored as they are.
    std::string_view inUse;     ///< Who may hold the store when it is claimed already, and what to do: the whole sentence.
};

/// Open an on-disk store, or explain why it will not open.
///
/// ONE process per store, and the store enforces it rather than asking to be trusted:
/// `CowTreeStorage::Open` claims the file for the life of the process, so a second opener
/// is refused with `StorageErrorCode::InUse`. That refusal is spelled out here, naming the
/// path it was given and the spec's remedy, because the flag that places a store is the
/// one most likely to be copied between two nodes on one machine. A filesystem that
/// cannot lock is reported rather than assumed: a guard that silently does nothing reads
/// exactly like one that works.
/// @param spec Where the store is and how it is kept.
/// @param logger Where an unenforceable claim is reported.
/// @return The store, or why it could not be opened.
[[nodiscard]] std::expected<std::unique_ptr<IStorage>, std::string> OpenDiskStore(DiskStoreSpec const& spec,
                                                                                  ILogger& logger);

/// Wrap a tier's storage for the threads that share it.
///
/// A single-shard `ShardedStorage`, and not for sharding: it is the lock. A tier is mutated
/// on a reactor and its statistics are read by the heartbeat thread and by whatever scrapes
/// `/metrics`, and `Snapshot()` on these backends writes a `mutable` member. Inside it a
/// `WriteErrorReportingStorage`, so a persistence failure -- a full disk, an I/O error, a
/// read-only store -- is counted and logged rather than silently not happening (#574), and
/// covered by the same lock.
/// @param storage The composed storage.
/// @param logger Where a write error is reported.
/// @return The wrapped storage.
[[nodiscard]] std::unique_ptr<IStorage> ShareStorage(std::unique_ptr<IStorage> storage, ILogger& logger);

/// The in-memory codec settings this configuration produces.
///
/// Its own function, and public, for two reasons. It is the ONE place a memory
/// tier's codec is decided, so a second tier cannot be built without it -- the
/// daemon learned that as `MakeL1`, whose comment says every L1 goes through it
/// "so the memory-compression settings cannot be applied to some shards and
/// silently missed on others". And it is a pure mapping from configuration to
/// options, so a test can assert the wiring without a filesystem, a socket or a
/// started tier -- which matters because a flag that reaches no tier is inert and
/// looks, from every other surface, exactly like one that works.
///
/// **`codec` is the EFFECTIVE one**, resolved through `Compression::EffectiveCodec`:
/// what this tier will store under, never what the configuration asked for. Being
/// the one place the decision is made is what lets the startup line read its report
/// from here instead of authoring a second answer that disagrees on a build without
/// `FASTCACHED_ENABLE_COMPRESSION`.
/// @param cfg The node configuration.
/// @return The options to hand `InMemoryLruStorage::SetCompression`.
[[nodiscard]] InMemoryLruStorage::CompressionOptions MemoryCompressionOf(NodeConfig const& cfg);

/// Resident bytes this node's caches would spend on their key indexes with the whole
/// store loaded (#175).
///
/// Passed to `NodeCapacityOf` as its own argument rather than added to
/// `NodeCacheCapacity`, which is ON THE WIRE: a new field there would be a frame-shape
/// change, and `MinSupportedVersion == CurrentVersion` makes that a flag day for the
/// fleet (#998). `reservedMemoryBytes` already travels, so the figure reaches the
/// leader through a field that exists.
/// @param tier This node's cache tier, or nullptr when it runs none.
/// @return Bytes to hold back, or 0 without a tier.
[[nodiscard]] std::uint64_t IndexReserveBytesOf(CacheTier const* tier);

/// What @p tier is configured to hold, in the vocabulary the fleet speaks.
///
/// Read off the tier that was actually built rather than off the configuration
/// that asked for it, and the difference is the point: `--listen-node` may have
/// been taken, `--cache-memory 0` may have turned the memory half off, and a
/// node that announced a budget it does not have would have the leader reporting
/// a cache that is not there.
///
/// A registration fact, because a budget does not move while the process runs.
/// @param tier The node's cache, or null when it has none.
/// @return The per-tier budgets; every tier absent when @p tier is null.
[[nodiscard]] Distributed::NodeCacheCapacity CacheCapacityOf(CacheTier const* tier);

/// What @p tier holds right now, in the vocabulary the fleet speaks.
///
/// A heartbeat fact. The hit/miss split comes from the sink rather than from the
/// storage, and deliberately: `LocalCache` counts a hit when it serves from this
/// node's tier and a miss when it has to reach the shared cache, which is the
/// question an operator is asking. The storage's own `getHits` counts something
/// narrower -- reads of one tier -- and a lower tier is only consulted when the
/// one above it missed, so those do not add up to the node's.
/// @param tier The node's cache, or null when it has none.
/// @param metrics Where `LocalCache` counted its hits and misses.
/// @return What the cache holds; every tier absent when @p tier is null.
[[nodiscard]] Distributed::NodeCacheLoad CacheLoadOf(CacheTier const* tier, IMetricsSink const& metrics);

/// Start the node's cache tier, or explain why the node must not start at all.
///
/// Three outcomes, which is why the return type looks the way it does:
///
///   - **A tier** — it is serving.
///   - **Success carrying nothing** — there is deliberately no tier, either because
///     `--listen-node` was emptied or because there is nowhere to keep objects.
///     The node continues; a line has already been logged.
///   - **An error** — the store could not be opened, so startup must stop.
///
/// The third outcome used to include a bind failure the operator had asked for by
/// naming the address. That judgement moved to `StartNodeSurfaceOrExplain` with the
/// surfaces (#290): one listener now carries the cache verbs and the scheduler's, and
/// a failure to open it cannot be judged by one of the two components on it.
///
/// That middle state is the whole reason this is a function rather than four lines
/// in `WorkerBody`. Which of the three applies is a judgement worth stating once and
/// testing, and `main.cpp` is in no test target — the lesson `CacheProtocol.cpp`,
/// `RootReconciler.cpp` and `AdminEndpoint.cpp` were each extracted for. It also kept
/// `WorkerBody` under clang-tidy's cognitive-complexity limit, which is the symptom
/// that said so.
/// @param cfg The parsed configuration.
/// @param upstream What the tier's upstream is built from; built only when a tier is, so a node
///        that keeps no objects opens no session pool. Every part must outlive the tier.
/// @param locality Decides whether a caller is on this machine; must outlive it.
/// @param clock Time source for the tier's expiry; must outlive it.
/// @param metrics Where hits, misses and upstream outcomes are counted.
/// @param logger Where what the tier holds, or why there is none, is announced.
/// @return The tier, a null tier meaning "carry on without one", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<CacheTier>, std::string> StartCacheTierOrExplain(NodeConfig const& cfg,
                                                                                             UpstreamParts const& upstream,
                                                                                             ILocalityOracle const& locality,
                                                                                             core::platform::IClock& clock,
                                                                                             IMetricsSink& metrics,
                                                                                             ILogger& logger);

/// Convert the on-disk half of the tier to this build's record layout, and say
/// what happened.
///
/// Lives here rather than in `main.cpp` because WHERE the store is, is this
/// file's knowledge: the tier decides that `--cache-dir` names a directory and
/// the store is one file inside it. A conversion that reconstructed that path
/// for itself would be a second place to change it, and would convert nothing
/// on the day the first one moved.
/// @param cfg The parsed configuration; `cacheDir` must not be empty.
/// @return A line to print on success, or the reason it could not be done.
[[nodiscard]] std::expected<std::string, std::string> MigrateDiskTier(NodeConfig const& cfg);

} // namespace FastCache::Node
