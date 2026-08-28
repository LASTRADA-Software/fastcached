// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Server/AdminCredential.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// Under TLS the surface below OWNS a context, so the complete type is needed for
// its deleter; without it the class is only ever a pointer that is always null,
// and the forward declaration keeps this header free of OpenSSL either way.
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#else
namespace FastCache
{
class TlsContext;
}
#endif

namespace FastCache::Node
{

class CacheTier;

/// What a node's `/metrics` scrape reads, gathered in one place.
///
/// Pointers rather than references because one of them is legitimately absent —
/// a node whose every cache half was turned off has no tier — and because the
/// bundle then has an obvious "nothing configured" spelling for a test.
///
/// Every one of these must outlive the endpoint built from it. In `WorkerBody`
/// they all do: each is declared before the endpoint and therefore destroyed
/// after it.
struct NodeScrapeSources
{
    /// Where the machine's own numbers come from. Never null.
    ///
    /// The same source the advertised capacity came from, so a scrape and a
    /// registration cannot disagree about the machine they describe.
    IHostFactsSource const* host {};
    /// How many compiles are running right now.
    ///
    /// A callable rather than a `WorkerServer const*`, and the reason is testing
    /// rather than generality: a real `WorkerServer` needs a bound listener, a
    /// protocol, a membership oracle, a sink and a logger, so a snapshot that
    /// demanded one could only be exercised by standing a server up. It is also a
    /// gauge sampled per scrape, which is exactly what a callable is.
    std::function<std::size_t()> busySlots;
    /// The node's cache, or **null** when it runs none.
    CacheTier const* cache {};
    /// Concurrent compiles this node advertises.
    std::size_t slots {};
    /// The filesystem whose free space is reported.
    std::filesystem::path scratchRoot;
};

/// Build the provider that answers each `/metrics` scrape.
///
/// A function rather than a lambda in `main()`, for the reason `AdminEndpoint`
/// itself is a class rather than three locals: `main.cpp` is in no test target, so
/// a snapshot assembled there has no coverage at all — and this one has a branch
/// worth covering, since a node without a cache must report **no** cache rather
/// than an empty one. It also keeps `WorkerBody` under clang-tidy's
/// cognitive-complexity limit, which is the symptom that said the decision had
/// spread too far.
///
/// Everything it reports is sampled per scrape rather than captured once: the disk
/// fills, the busy count moves, and the cache grows, so a value frozen at startup
/// is worse than no value because it looks current.
/// @param sources What to read; must outlive the returned provider.
/// @param startedAt When the process began, for the uptime gauge.
/// @return A provider suitable for `AdminEndpoint::Start`.
[[nodiscard]] AdminHttpServer::SnapshotProvider MakeNodeSnapshotProvider(NodeScrapeSources sources,
                                                                         std::chrono::steady_clock::time_point startedAt);

/// Read the dashboard credential out of the file an operator named.
///
/// Fallible and reported rather than warned about, for the reason the endpoint's
/// own failure is: a credential file that could not be read must not silently
/// become "no credential", which is the one failure mode that turns a guarded
/// fleet map into an open one.
///
/// The trailing newline every editor adds is trimmed, so a secret typed into a
/// file works without the operator having to know that.
/// @param path Where the secret is.
/// @return The credential, or why it could not be used.
[[nodiscard]] std::expected<AdminCredential, std::string> ReadDashboardToken(std::filesystem::path const& path);

/// The routes that serve the fleet dashboard.
///
/// A free function rather than a lambda in `main()`, for the reason
/// `MakeNodeSnapshotProvider` is one: `main.cpp` is in no test target, so wiring
/// assembled there has no coverage at all -- and this has branches worth covering,
/// since a follower must render a page rather than a redirect and an unauthorised
/// caller must get a challenge a browser can act on.
/// @param sources What the fleet is read from; must outlive the returned routes.
/// @param credential What a caller must present, or a default-constructed one for none.
/// @param refreshSeconds How often the page reloads itself.
/// @return `/fleet` and `/fleet.json`.
/// The rolling record the fleet routes draw over time, and what it promises.
///
/// A bundle rather than two parameters because the two are one fact from a route's
/// point of view: what to read, and what the page should tell an operator about how
/// long it will last. `history` is null on a build that keeps none, and the page
/// then simply has no charts -- which is a legitimate way to run and not a failure.
struct FleetHistorySource
{
    /// Where samples are kept. Null when nothing samples.
    Distributed::FleetHistory const* history {};
    /// Whether that record is written to disk and so survives a restart.
    bool durable { false };
};

[[nodiscard]] std::vector<AdminRoute> MakeFleetRoutes(Distributed::FleetSources sources,
                                                      AdminCredential const& credential,
                                                      unsigned refreshSeconds,
                                                      FleetHistorySource history = {});

/// Samples the fleet on a timer, for as long as this node LEADS it.
///
/// Only while leading, and that is the whole reason this is a class rather than a
/// lambda in `main()`: a follower's registry holds whatever registered against it
/// rather than the fleet, so sampling there would record a fraction as though it
/// were the whole -- and the resulting chart would show a fleet shrinking every
/// time leadership moved.
///
/// The loop is interruptible. A stop that waits out a full interval makes teardown
/// look hung, which this repository has already paid for once as a `systemctl stop`
/// escalating to SIGKILL.
class FleetSampler
{
  public:
    /// How often the ring is written back, while sampling.
    ///
    /// Not every sample: the file is the whole ring, so writing it each minute
    /// would rewrite 24 hours of buckets to record one. Also written on shutdown.
    static constexpr auto SaveInterval = std::chrono::minutes { 5 };

    /// Start sampling.
    ///
    /// TWO series, because they answer different questions and only one of them has
    /// a leader. The fleet series is the scheduler's own -- dispatch outcomes nobody
    /// but a leader produces -- and the node series is this machine's cache and
    /// slots, which are facts about it whoever leads. Sampling only while leading is
    /// why the record used to change machine under an election.
    ///
    /// @param sources What the fleet series reads; the same bundle the routes collect
    ///                from.
    /// @param node What the node series reads: the same provider `/metrics` scrapes,
    ///             so the two cannot disagree about the machine they describe.
    /// @param wall Where a bucket's timestamp comes from.
    /// @param fleetPath Where to keep the fleet series, or empty for memory-only.
    /// @param nodePath Where to keep the node series, or empty for memory-only.
    /// @param logger Shared logger.
    FleetSampler(Distributed::FleetSources sources,
                 AdminHttpServer::SnapshotProvider node,
                 IWallClock const& wall,
                 std::filesystem::path fleetPath,
                 std::filesystem::path nodePath,
                 ILogger& logger);

    /// Stops the thread and writes the ring out one last time.
    ~FleetSampler();

    FleetSampler(FleetSampler const&) = delete;
    FleetSampler& operator=(FleetSampler const&) = delete;
    FleetSampler(FleetSampler&&) = delete;
    FleetSampler& operator=(FleetSampler&&) = delete;

    /// What the routes read: the fleet-wide series.
    ///
    /// The dashboard is the leader's page and shows the fleet, so this is the one the
    /// charts draw. A follower's copy stays empty, which is the truth about what it
    /// can see.
    [[nodiscard]] Distributed::FleetHistory const& History() const noexcept
    {
        return _fleet;
    }

    /// This machine's own series, recorded whether or not it leads.
    ///
    /// What a node contributes rather than what the fleet did, and the half that
    /// survives an election -- the leader's view of the fleet is rebuilt from these.
    [[nodiscard]] Distributed::FleetHistory const& NodeHistory() const noexcept
    {
        return _node;
    }

    /// Whether both rings are written to disk.
    ///
    /// Every half, because the page says "written to disk, so it survives a restart"
    /// and a history holding a file a NEWER build wrote persists nothing at all. A
    /// path alone would have the page contradicting the startup warning beside it,
    /// and one series persisting while the other does not is not "durable" either.
    [[nodiscard]] bool Durable() const noexcept
    {
        return !_fleetPath.empty() && !_nodePath.empty() && !_fleet.ReadOnly() && !_node.ReadOnly();
    }

    /// Take one sample now, whatever the timer is doing.
    ///
    /// The seam a test drives instead of waiting a minute for the thread: every
    /// rule about *what* a sample holds is then a case over a scripted registry
    /// rather than a sleep.
    /// The NODE series is recorded either way; the return value is about the fleet
    /// one, which only a leader can answer for.
    /// @return True when a fleet sample was taken; false when this node does not lead.
    bool SampleOnce();

  private:
    /// One of the two series, paired with where it is written and what to call it.
    struct Series
    {
        Distributed::FleetHistory* history; ///< The ring.
        std::filesystem::path const* path;  ///< Where it is kept, or empty for memory-only.
        std::string_view what;              ///< What a log line calls it.
    };

    /// Both series, in one place.
    ///
    /// Stated ONCE: restoring at construction and saving on the timer walk exactly
    /// this list, so a third series cannot be something one of them silently misses
    /// -- which for the save half would be a year of readings that is never written
    /// and is discovered only at the next restart.
    /// @return The two, fleet first.
    [[nodiscard]] std::array<Series, 2> EachSeries();

    void Persist();

    Distributed::FleetSources _sources;
    /// This machine's own facts, read the same way `/metrics` reads them.
    AdminHttpServer::SnapshotProvider _nodeFacts;
    Distributed::FleetHistory _fleet;
    Distributed::FleetHistory _node;
    std::filesystem::path _fleetPath;
    std::filesystem::path _nodePath;
    ILogger& _logger;
    std::mutex _wakeMutex;
    std::condition_variable_any _wake;
    std::jthread _thread;
};

/// Turn one fleet snapshot into the FLEET-scoped readings a sample holds.
///
/// Separate from the sampler so it is a pure function of a snapshot: which slot
/// each number lands in is the part worth a unit test, and it needs no thread, no
/// clock and no registry to check.
/// @param snapshot What `CollectFleet` returned.
/// @return The readings, counters cumulative.
[[nodiscard]] EnumTable<Distributed::FleetMetric, std::uint64_t> SampleFrom(Distributed::FleetSnapshot const& snapshot);

/// Turn one metrics scrape into the NODE-scoped readings a sample holds.
///
/// From the same provider `/metrics` reads, so a machine cannot describe itself two
/// ways -- the argument `NodeScrapeSources::host` already makes about a scrape and a
/// registration. Node-scoped slots only; the rest stay zero, because a node that is
/// not scheduling has not refused anything and must not say it refused nothing.
///
/// @param metrics Where this node's own cache counters are read -- the same ones
///                `CacheTier::Snapshot` reports to the fleet, so one machine cannot
///                be described two ways on one page.
/// @param snapshot What this node reports about itself.
/// @return The readings, node-scoped slots filled.
[[nodiscard]] EnumTable<Distributed::FleetMetric, std::uint64_t> NodeSampleFrom(IMetricsSink const& metrics,
                                                                                MetricsSnapshot const& snapshot);

/// Where one of a node's two history files lives.
///
/// The same enum names the slots and the file that holds them, so a scope added there
/// is a file here rather than a third path spelled somewhere else.
/// @param cfg The parsed configuration.
/// @param scope Which series.
/// @return The path, or empty when this node keeps no state directory.
[[nodiscard]] std::filesystem::path HistoryPathFor(NodeConfig const& cfg, Distributed::FleetMetricScope scope);

/// Where a node keeps its fleet history.
///
/// The cluster directory first, because the history is a leader's record and a
/// leader is a cluster member; the cache directory next, because a node given one
/// has somewhere durable already; and otherwise nothing, which means memory-only.
/// No new flag: a third place to say "put state here" is a third place for an
/// operator to point at the wrong disk.
/// @param cfg The parsed configuration.
/// @return The file path, or empty for memory-only.
[[nodiscard]] std::filesystem::path FleetHistoryPath(NodeConfig const& cfg);

/// How often the dashboard reloads itself, in seconds.
///
/// A named constant rather than a flag: the endpoint serves one connection at a
/// time on its owning thread, so the interval is a property of what the surface can
/// sustain rather than a preference. Ten seconds is slower than a heartbeat, which
/// is the rate at which anything on the page can actually change.
inline constexpr unsigned DashboardRefreshSeconds = 10;

/// The worker's `/metrics` and `/healthz` endpoint: listener, server and the
/// thread that serves them, owned as one thing.
///
/// A class rather than three locals in `main()` for two reasons, and the second is
/// the one that matters. The three have a *destruction order* -- the server must be
/// told to close its listener before the thread serving it can be joined -- and a
/// `main()` holding them separately expresses that as a `Shutdown()` call somebody
/// has to remember at every return path. Here it is the destructor, which is the
/// RAII rule this codebase applies to every other resource handle. And `main()` in
/// this binary is in no test target, so wiring that lives there has no unit
/// coverage at all -- the mistake `CacheProtocol.cpp` and `RootReconciler.cpp` are
/// each recorded as having been extracted to avoid.
class AdminEndpoint
{
  public:
    /// Bind the endpoint and start serving it.
    ///
    /// Fallible, and reported rather than warned about: an operator who asked a
    /// *worker* for an endpoint is almost always wiring a probe to it, so a worker
    /// that silently started without one looks healthy to the very thing that
    /// would otherwise have noticed it was not.
    ///
    /// The error is a diagnostic string rather than one of the project's four error
    /// enums, and that is a deliberate departure. This fails in two ways that belong
    /// to two different taxonomies -- a malformed `--admin-listen` is a
    /// `ConfigError`, an address that will not bind is a `NetError` -- and
    /// `ConfigErrorCode` has no enumerator that describes the second at all. The
    /// caller's response is identical either way (log it, refuse to start), so
    /// picking one enum would buy nothing and mislabel half the failures. What the
    /// caller *does* need is the text, because the flag may have been a bare port
    /// and the message has to say what that resolved to.
    /// @param listenSpec `port`, `host:port` or `[v6]:port`.
    /// @param defaultHost What a bare port binds to; loopback for a scrape surface.
    /// @param metrics The sink to render.
    /// @param snapshot What to report per scrape.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    /// @param routes Routes beyond `/metrics` and `/healthz`; may be empty.
    /// @param tls Server TLS context, or nullptr to serve plaintext.
    [[nodiscard]] static std::expected<std::unique_ptr<AdminEndpoint>, std::string> Start(
        std::string_view listenSpec,
        std::string_view defaultHost,
        IMetricsSink& metrics,
        AdminHttpServer::SnapshotProvider snapshot,
        ILogger& logger,
        std::vector<AdminRoute> routes = {},
        TlsContext* tls = nullptr);

    /// Stop serving and join the thread.
    ~AdminEndpoint();

    AdminEndpoint(AdminEndpoint const&) = delete;
    AdminEndpoint& operator=(AdminEndpoint const&) = delete;

    // Neither movable: the serving thread holds a pointer to `_server`, and
    // `_server` a reference to `_listener`. Both survive a move of the owning
    // object only because they are behind `unique_ptr` -- but saying so and
    // relying on it are different things, and the type is always held by pointer.
    AdminEndpoint(AdminEndpoint&&) = delete;
    AdminEndpoint& operator=(AdminEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    /// @return Host and port, as text.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    /// Take ownership of an already-bound listener and start serving it.
    /// @param listener The bound listener.
    /// @param metrics The sink to render.
    /// @param snapshot What to report per scrape.
    /// @param boundEndpoint What `BoundEndpoint()` reports.
    /// @param logger Where the server reports.
    AdminEndpoint(std::unique_ptr<BlockingListener> listener,
                  IMetricsSink& metrics,
                  AdminHttpServer::SnapshotProvider snapshot,
                  std::string boundEndpoint,
                  ILogger& logger,
                  std::vector<AdminRoute> routes,
                  TlsContext* tls);

    std::unique_ptr<BlockingListener> _listener;
    std::unique_ptr<AdminHttpServer> _server;
    std::string _boundEndpoint;
    std::jthread _thread;
};

/// The admin surface and everything it borrows, owned as one thing.
///
/// The TLS context is declared **before** the endpoint so it is destroyed
/// **after** it: every accepted socket holds that context for as long as it lives,
/// and the endpoint's destructor is what drains them. Two locals in `WorkerBody`
/// would express that ordering as a comment somebody has to keep believing; here
/// it is the member order, which is the same reason `CacheTier` and
/// `SchedulerTier` are each one object rather than five locals.
struct AdminSurface
{
#if defined(FC_TLS_ENABLED)
    /// Server TLS context when a certificate was named, else null.
    std::unique_ptr<TlsContext> tls;
#endif
    /// The sampler, when this node serves a dashboard over a fleet.
    ///
    /// **Declared before `endpoint`, and that ordering is load-bearing**: the routes
    /// hold a pointer into this history, and members are destroyed in reverse
    /// declaration order -- so the endpoint (and the thread serving requests
    /// through those routes) is torn down first.
    std::unique_ptr<FleetSampler> sampler;
    /// The running endpoint. Null when the operator asked for no admin surface.
    std::unique_ptr<AdminEndpoint> endpoint;
};

/// Build the whole admin surface from the configuration, or explain the refusal.
///
/// The sibling of `StartCacheTierOrExplain`, and a function for the same two
/// reasons: it reads three flags that can each refuse for a different reason, and
/// `main.cpp` is in no test target -- so assembled there, none of the TLS, token
/// and route-selection branches would have any coverage at all. It also keeps
/// `WorkerBody` under clang-tidy's cognitive-complexity limit, which is the
/// symptom that says a decision has spread too far.
///
/// Fleet routes are contributed only when `fleet` is present. A node with no
/// scheduler passes nullopt and `/fleet` is then a plain 404: a process with no
/// fleet view offers no fleet route, rather than one answering with an empty fleet.
/// @param cfg The parsed configuration.
/// @param host Where the machine's own facts come from, for a generated
///        certificate's subject names.
/// @param metrics The sink `/metrics` renders.
/// @param snapshot What to report per scrape.
/// @param fleet What the dashboard reads, or nullopt to serve none.
/// @param logger Where to announce the bound address.
/// @return The surface (whose endpoint is null when none was asked for), or why
///         it could not be served.
[[nodiscard]] std::expected<AdminSurface, std::string> StartAdminSurfaceOrExplain(
    NodeConfig const& cfg,
    IHostFactsSource const& host,
    IMetricsSink& metrics,
    AdminHttpServer::SnapshotProvider snapshot,
    std::optional<Distributed::FleetSources> fleet,
    ILogger& logger);

} // namespace FastCache::Node
