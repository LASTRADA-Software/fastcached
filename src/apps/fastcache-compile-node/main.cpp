// SPDX-License-Identifier: Apache-2.0
///
/// `fastcache-compile-node` — a compile worker for fastcached's distributed
/// execution.
///
/// Registers with a scheduler, then answers `Compile` requests from clients the
/// scheduler sent. It is not a cache and not a scheduler: it holds no keys, stores
/// nothing, and is given no cache credentials. The object it produces goes back to
/// the client, which stores it — see `Cc::Dispatch` for why that is the trust model
/// rather than an accident of layering.
///
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "ClusterAdminCli.hpp"
#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeMembership.hpp"
#include "NodeToolchains.hpp"
#include "SchedulerTier.hpp"
#include "WorkerServer.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/InheritedListener.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <Dispatch.hpp>
#include <EndpointDial.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <WorkerProtocol.hpp>

namespace
{
namespace Wire = FastCache::CompileCacheWire;
using namespace FastCache;
using namespace FastCache::Node;

/// How often a worker tells the scheduler it is alive.
///
/// Comfortably inside `WorkerRegistry::DefaultHeartbeatTimeout` (90 s), because the
/// two errors are not symmetric: a heartbeat that arrives late costs this worker
/// its place in the fleet until it re-registers, while one that arrives early costs
/// a few bytes.
constexpr std::chrono::seconds HeartbeatInterval { 20 };

/// Slices the heartbeat sleep is broken into, so a stop request is observed
/// promptly rather than after a full interval.
constexpr int HeartbeatSlices = 20;

/// How often a parked `accept()` returns so the loop can observe a shutdown.
///
/// POSIX honours `SO_RCVTIMEO` for `accept()`, and it is the ONLY portable way to
/// stop this loop: closing the listening socket does not unblock a parked accept
/// on Linux. Short enough that a stop is prompt, long enough that an idle worker
/// is not spinning.
constexpr std::chrono::milliseconds AcceptPollInterval { 200 };

/// How long a single request may take to arrive once accepted.
///
/// Generous, because a request carries a whole preprocessed translation unit and
/// the client may be on the other side of a slow link -- but not unbounded, so one
/// stalled client cannot hold a slot forever against a worker that serves its jobs
/// inline.
constexpr std::chrono::milliseconds RequestIoTimeout { 120'000 };

/// Per-call send/recv ceiling on the heartbeat's own connection to the scheduler.
///
/// Was ten seconds passed as BOTH the dial bound and the I/O bound, which is the
/// collapse `Cc::DialEndpoint` used to make: ten seconds is a reasonable ceiling
/// on an exchange and a very long time to wait for a TCP handshake.
constexpr std::chrono::milliseconds HeartbeatIoTimeout { 10'000 };

/// Ceiling on OPENING that connection, name resolution included.
constexpr std::chrono::milliseconds HeartbeatConnectTimeout { 1'000 };

/// How often the stop watcher looks at the stop flag.
///
/// A signal handler may portably do almost nothing -- it sets a flag -- so
/// something else has to notice and close the listener, and it cannot be the
/// accept loop, which is parked inside `Accept()`. Short enough that `systemctl
/// stop` and Ctrl-C feel immediate, long enough to cost nothing while idle.
constexpr std::chrono::milliseconds StopPollInterval { 100 };

/// Record a stop request.
///
/// `extern "C"` and doing nothing but setting the process-wide flag, because a
/// signal handler is not allowed to do more: `DaemonControls`' flag is atomic and
/// lock-free, which is what makes this legal where logging or allocating here
/// would not be.
extern "C" void HandleNodeStopSignal(int /*signum*/)
{
    DaemonControls::Instance().RequestStop();
}

/// Ask for a graceful stop on the signals a supervisor actually sends.
///
/// SIGHUP is deliberately NOT handled: the daemon reloads its configuration on it,
/// and this worker has nothing it could reload -- its toolchain table is what its
/// registration advertised, so re-reading it would leave the scheduler dispatching
/// against a set this worker no longer serves. Leaving SIGHUP at its default is
/// therefore the honest behaviour rather than an omission.
void InstallNodeStopHandlers()
{
    std::signal(SIGINT, &HandleNodeStopSignal);
    std::signal(SIGTERM, &HandleNodeStopSignal);
}

/// Adopt a socket-activated listener, when a supervisor handed one over.
///
/// Separated from `main` so the whole handoff -- how many descriptors arrived,
/// and whether the configuration can still describe this worker afterwards -- is
/// one decision with one answer, rather than three checks interleaved with
/// everything else a startup does.
/// @param cfg What the operator asked for.
/// @param logger Where the adoption is announced.
/// @return The adopted listener, null when nothing was handed over, or why the
///         handoff cannot be served.
[[nodiscard]] std::expected<std::unique_ptr<IListener>, std::string> AdoptActivatedListener(NodeConfig const& cfg,
                                                                                            ILogger& logger)
{
    // When a supervisor already bound the port and handed the descriptor over,
    // binding it again would fail with "address already in use" -- against
    // ourselves. Falling through to Bind() when nothing was handed over is what
    // lets one binary serve both a `.socket` unit and a plain `--port`, with no
    // flag distinguishing them: the environment says which, and it says so
    // unambiguously.
    auto inherited = AdoptInheritedListeners(AcceptPollInterval, RequestIoTimeout);
    if (inherited.empty())
        return std::unique_ptr<IListener> {};

    // Only the first would be used. This worker answers one protocol on one port,
    // so a unit listing several sockets is a misconfiguration -- reported rather
    // than half-honoured, since silently ignoring the rest would leave an operator
    // with a port that accepts nothing and no clue why.
    if (inherited.size() > 1)
        return std::unexpected { std::format("socket activation handed over {} listeners; this worker serves exactly one",
                                             inherited.size()) };

    // Socket activation makes --advertise mandatory, because the fallback becomes
    // a guess the process cannot make. `--bind` and `--port` were not used -- the
    // socket unit chose the port and this process is never told which -- so the
    // default would register `0.0.0.0:6676` from config values that describe
    // nothing, and 0.0.0.0 is not an address a remote client can dial anyway.
    //
    // The consequence of guessing is the worst-shaped failure this system has: the
    // registration SUCCEEDS, the worker heartbeats happily, the scheduler leases
    // that endpoint to clients, and every one of them fails to connect and
    // compiles locally. Nothing reports an error, and the fleet looks healthy from
    // both ends. Refusing at startup, where it can be explained, is the whole
    // difference.
    if (cfg.advertise.empty())
        return std::unexpected { std::string {
            "--advertise is required under socket activation: the socket unit owns the port, so this worker "
            "cannot know what address clients should use" } };

    logger.Logf(LogLevel::Info, "adopted a socket-activated listener; --bind and --port are not used");
    return std::move(inherited.front());
}

/// What `main` returns when the operator's configuration is wrong.
///
/// Every refusal in this program is that same fact -- a flag missing, a flag that
/// cannot be honoured, an endpoint that will not bind -- so it is one name rather
/// than a `2` spelled seven times, and distinct from the `1` a supervisor reads as
/// "it ran and then died".
constexpr int ExitUsage = 2;

/// What `main` returns when the worker served until it was asked to stop.
constexpr int ExitOk = 0;

/// Serve until asked to stop.
///
/// Everything `main` does once it has decided this process is going to BE a
/// worker. Separated so it can be handed to an `IDaemonHost`, which double-forks
/// on POSIX or hands control to the Windows SCM -- neither of which can wrap a
/// `main` that has already parsed, validated and registered.
///
/// The cheap, fallible checks stay in `main` deliberately: run in here they would
/// print their diagnosis to a stdout the POSIX host has already redirected to
/// /dev/null, so a misconfigured worker would exit in silence.
/// @param cfg What this worker was told to be.
/// @param logger Where it reports.
/// @return Process exit code.
[[nodiscard]] int WorkerBody(NodeConfig const& cfg, ILogger& logger)
{
    // Socket activation is resolved BEFORE the toolchains, and the order is
    // deliberate. Computing a fingerprint walks the whole include tree and takes
    // seconds; a bad handoff is decided in microseconds. Doing the cheap, fallible
    // thing first means a misconfigured unit fails immediately instead of after a
    // multi-second pause -- and it means the startup log reads in the order things
    // actually happened, so an operator watching a worker come up sees what it did
    // with the socket before the long quiet part.
    auto activatedOrError = AdoptActivatedListener(cfg, logger);
    if (!activatedOrError.has_value())
    {
        logger.Logf(LogLevel::Error, "{}", activatedOrError.error());
        return ExitUsage;
    }
    auto activated = std::move(*activatedOrError);

    auto const runner = Cc::MakeProcessRunner();

    auto const toolchainHost = Cc::MakeToolchainHost();
    auto const discovery = cfg.toolchainDiscovery ? Cc::MakeToolchainDiscovery(*toolchainHost, *runner) : nullptr;
    auto toolchainsOrNone = ResolveToolchains(cfg, discovery.get(), *runner, *toolchainHost, logger);
    if (!toolchainsOrNone.has_value())
        return ExitUsage;
    auto const toolchains = *std::move(toolchainsOrNone);

    // Computed HERE and advertised, rather than left for the scheduler to derive.
    // Both would use the same `OfferableSlots`, so the numbers would agree -- but
    // this worker has to enforce a limit before it has a scheduler to ask, and a
    // worker running to one number while the scheduler leases against another is
    // exactly the "fuller and slower than the scheduler believes" failure `--slots`
    // documents. One call, one answer, used for both.
    // The machine arrives through a seam rather than through `hardware_concurrency()`
    // and `QueryHostTotalMemoryBytes()` directly: `NodeCapacityOf` is what decides
    // which facts come from the operator and which from the hardware, and that rule
    // is only checkable if a test can present a two-core laptop and a 128-thread
    // server in one run. It also lives in `NodeConfig.cpp` rather than here, because
    // this file is in no test target.
    auto const host = MakeSystemHostFacts();
    auto const capacity = Node::NodeCapacityOf(cfg, *host);
    auto const slots = Distributed::OfferableSlots(capacity, cfg.slots);
    auto const advertise = cfg.advertise.empty() ? std::format("{}:{}", cfg.bindAddress, cfg.port) : cfg.advertise;

    // `IsBound()`, not a null check: Bind() NEVER returns null -- it hands back a
    // listener carrying the diagnostic, for Accept() to surface later. Testing for
    // null therefore tested nothing, and the failure it let through was silent and
    // actively harmful: on a port conflict this worker logged "ready", registered
    // with the scheduler advertising a port it was not listening on, and then
    // exited 0 the first time the accept loop touched the dead socket. The
    // scheduler would go on leasing it to clients until the heartbeat lapsed.
    // BindError() is what says WHICH of address-in-use, permission or bad address
    // it was.
    std::unique_ptr<BlockingListener> bound;
    if (activated == nullptr)
    {
        bound = BlockingListener::Bind(cfg.bindAddress, cfg.port, /*backlog=*/128);
        if (bound == nullptr || !bound->IsBound())
        {
            logger.Logf(LogLevel::Error,
                        "could not bind {}:{} ({})",
                        cfg.bindAddress,
                        cfg.port,
                        bound ? bound->BindError() : std::string_view { "null listener" });
            return 1;
        }
    }

    // Without this the accept loop cannot be stopped on Linux at all, and the way
    // that presents is worse than a crash: POSIX does not unblock a parked
    // `accept()` when another thread closes the socket, so `Shutdown()` would set a
    // flag nothing ever comes back to read and `systemctl stop` would hang until
    // the supervisor escalated to SIGKILL. macOS hides it -- there `close()` does
    // wake the accept -- which is exactly why this was worth catching in CI rather
    // than on one developer's machine. `WorkerServer::Run` already documents the
    // poll timeout as the mechanism it relies on; this is what supplies it.
    //
    // The I/O timeout is separate and larger: it bounds reading a request, which
    // carries a whole preprocessed translation unit over a possibly slow link,
    // while the accept poll only decides how promptly a stop is noticed.
    // An adopted listener already has these -- AdoptInheritedListeners requires
    // them, so there is no path to a listener that cannot be shut down.
    if (bound != nullptr)
        bound->SetTimeouts(AcceptPollInterval, RequestIoTimeout);

    IListener& listenerRef = activated != nullptr ? *activated : static_cast<IListener&>(*bound);

    auto const scratch = std::filesystem::temp_directory_path() / "fastcache-compile-node";
    Cc::CompileJobRunner jobs { *runner, scratch, toolchains };

    // Every lease is accepted, and that is stated rather than hidden. The boundary
    // today is reachability of this port plus the shared credential -- the same
    // boundary the cache itself has. Validating a token against the scheduler is the
    // seam's other implementation and belongs with mTLS rather than bolted on here;
    // LeaseValidator exists so that is a substitution, not a rewrite.
    AtomicMetricsSink metrics;
    // One policy for all THREE surfaces -- the compile port here, the scheduler and
    // the cache below -- and it outlives every one of them. A node that answered "is
    // this peer one of ours" differently at two of its surfaces would admit a peer to
    // the fleet and refuse it the objects that fleet produced, or worse, the reverse.
    // Not `const`: consensus republishes the member set into it while the node runs,
    // which is the whole point of membership being a replicated log entry rather than
    // a command-line list.
    Node::NodeMembership membership { cfg };

    // No lease validation, and it is recorded here rather than left looking like a
    // stub. `WorkerServer` gates the port on membership, so what reaches this
    // protocol is already this machine or a peer the operator admitted -- and a
    // worker cannot check a token it did not issue without a round trip to the
    // scheduler per job, or a signing key the two would have to share.
    //
    // The residual, deliberately: an admitted member can compile here without taking
    // a lease first, bypassing the scheduler's slot accounting. Inside a trusted
    // fleet that is a fairness question rather than a security one -- the machine
    // would be busier than the scheduler believes, which the heartbeat corrects
    // within one interval. Closing it properly means signing lease tokens, which is
    // its own change.
    Cc::WorkerProtocol protocol {
        jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics
    };

    Node::WorkerServer server { listenerRef, protocol, slots, membership.Oracle(), metrics, logger };

    // The admin endpoint used to be built here, and moving it down is the fix
    // rather than tidying. Its snapshot lambda has to report the node's cache, and
    // the cache tier is started further down -- so a lambda captured at this point
    // could only ever say `.storage = std::nullopt`, which is exactly what it said,
    // comment and all, for as long as this program has had a cache.

    // The fleet scheduler, when this node is the one running it. Off unless asked
    // for: handing out other machines' CPU time is an operator's decision, not
    // something they get by starting a worker.
    //
    // The three objects have to outlive the endpoint, which holds references into
    // them, so they are declared here rather than inside the `if` -- and in
    // construction order, since each takes the one before it.
    // The reactor both framed surfaces share, and the connector over it.
    //
    // Declared BEFORE the tiers and therefore destroyed AFTER them, which is exactly
    // right and is why it is a local here rather than something each tier owns: a
    // tier's destructor closes its listener and its open connections by posting onto
    // this reactor, so the reactor has to still be running when that happens.
    //
    // It is NOT started yet. Every endpoint binds and adopts first, so that when the
    // thread does begin there is a listener behind every port a client might dial.
    Node::NodeIoLoop nodeIo;

    // The two framed surfaces this node may serve besides its worker port, each
    // owned as one object. Both are off unless asked for: handing out other machines'
    // CPU time, and caching to this machine's disk, are decisions an operator makes
    // rather than things they get by starting a worker.
    SteadyClock schedulerClock;
    SteadyClock cacheClock;

    std::unique_ptr<Node::SchedulerTier> schedulerTier;
    if (!cfg.schedulerListen.empty())
    {
        auto started = Node::SchedulerTier::Start(nodeIo, cfg, membership.Oracle(), schedulerClock, metrics, logger);
        if (!started.has_value())
        {
            // Fatal for the same reason the admin endpoint's is: an operator who asked
            // for this is relying on it, and a node that started without it looks
            // healthy to everything that would otherwise have noticed.
            logger.Logf(LogLevel::Error, "--listen-scheduler {}; refusing to start", started.error());
            return ExitUsage;
        }
        schedulerTier = std::move(*started);
    }

    // The node's own cache tier, in front of the shared one. It exists so a local
    // rebuild on a slow or bad network never reaches the wire: the shared cache holds
    // every object, but what is saved here is the round trip rather than the compile.
    //
    // Five collaborators in a reference chain, owned as one object rather than as
    // locals whose declaration ORDER is load-bearing and silently so.
    // Consensus, when the operator configured a cluster. It is what turns the
    // scheduler tier's standalone leadership into a real one: without it, every node
    // in a fleet believes it schedules, and two nodes handing out the same machine's
    // slots is the one thing the architecture says only one may do.
    //
    // Started AFTER the scheduler tier, because its observers push into it, and
    // declared after too, so it is destroyed first and cannot call into a tier that
    // has gone.
    auto consensusOrRefusal = Node::StartConsensusOrExplain(cfg, schedulerTier, membership, logger);
    if (!consensusOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "--node-id {}; refusing to start", consensusOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: no `--node-id` means this node leads alone.
    auto const consensusTier = std::move(*consensusOrRefusal);

    // Discovery, when the operator configured it. Declared AFTER consensus and so
    // destroyed before it, because its observer pushes into the tier above: a
    // discovery loop outliving the thing it hands peers to is a dangling reference
    // that only fires while a node is shutting down.
    auto discoveryOrRefusal = Node::StartDiscoveryOrExplain(cfg, consensusTier, logger);
    if (!discoveryOrRefusal.has_value())
    {
        // Fatal, like the other two surfaces an operator has to ask for: a node that
        // started without the discovery it was told to run looks healthy to a fleet
        // that will never hear from it.
        logger.Logf(LogLevel::Error, "--discovery {}; refusing to start", discoveryOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: no `--discovery` means the cluster is the
    // `--raft-peer` list an operator typed, which is the ordinary deployment.
    auto const discoveryTier = std::move(*discoveryOrRefusal);

    auto cacheTierOrRefusal = Node::StartCacheTierOrExplain(nodeIo, cfg, membership.Oracle(), cacheClock, metrics, logger);
    if (!cacheTierOrRefusal.has_value())
    {
        // No flag prefix here, unlike its neighbours: this tier can fail over two
        // different flags -- the directory and the port -- and only
        // `StartCacheTierOrExplain` knows which, so it names it.
        logger.Logf(LogLevel::Error, "{}; refusing to start", cacheTierOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: `StartCacheTierOrExplain` treats an emptied
    // `--listen-cache`, and a DEFAULT address that was already taken, as reasons to
    // carry on without a tier rather than as failures. Both have been logged.
    auto const cacheTier = std::move(*cacheTierOrRefusal);

    // The admin endpoint, when the operator asked for one. Off by default and on
    // loopback for a bare port: a scrape surface reachable from the network is a
    // decision, not a default.
    //
    // The same `AdminHttpServer` the daemon runs, over the same renderer. A second
    // implementation for a process with no cache is what `MetricsSnapshot::storage`
    // being optional exists to avoid -- and it brings `/healthz`, which this worker
    // has never had: a supervisor could tell that the process was alive and not
    // that it was answering.
    //
    // Declared AFTER the cache tier, which is what makes the pointer its lambda
    // captures safe: locals are destroyed in reverse, so this endpoint stops
    // serving -- and joins its thread -- before the tier it reads is gone.
    // The admin surface: `/metrics`, `/healthz`, and the fleet dashboard when the
    // operator asked for one. Assembled by a function rather than here, and not
    // for tidiness -- `main.cpp` is in no test target, so the TLS, credential and
    // route-selection branches would otherwise have no coverage at all, and this
    // is the third time that reasoning has moved wiring out of `WorkerBody`.
    //
    // Declared AFTER the tiers it reads, which is what makes the pointers safe:
    // locals are destroyed in reverse, so the surface stops serving -- and joins
    // its thread -- before the scheduler, the cluster and the cache tier are gone.
    auto surfaceOrRefusal = Node::StartAdminSurfaceOrExplain(
        cfg,
        *host,
        metrics,
        // Built by a function rather than spelled as a lambda here, for the reason
        // above: a node with no cache must report NO cache rather than an empty
        // one, and that branch has to be reachable from a test.
        Node::MakeNodeSnapshotProvider(Node::NodeScrapeSources { .host = host.get(),
                                                                 .busySlots = [&server] { return server.InFlight(); },
                                                                 .cache = cacheTier.get(),
                                                                 .slots = slots,
                                                                 .scratchRoot = jobs.ScratchRoot() },
                                       std::chrono::steady_clock::now()),
        // Absent when this node runs no scheduler: there is then no registry to
        // report, so no fleet route is registered and `/fleet` is a plain 404.
        schedulerTier ? std::optional { Distributed::FleetSources { .scheduler = &schedulerTier->Service(),
                                                                    // Legitimately null: a node with no
                                                                    // `--node-id` leads itself and has no
                                                                    // replicated state for anybody to read.
                                                                    .cluster = consensusTier.get(),
                                                                    .metrics = &metrics } }
                      : std::nullopt,
        logger);

    // Fatal rather than a warning, unlike the daemon's: an operator who asked a
    // *worker* for an endpoint is almost always wiring a probe to it, and a worker
    // that starts without one looks healthy to everything that would have noticed.
    if (!surfaceOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", surfaceOrRefusal.error());
        return ExitUsage;
    }
    auto const adminSurface = std::move(*surfaceOrRefusal);

    // Both surfaces have bound and adopted, so the loop can start accepting. Doing
    // it here rather than at construction is the ordering `ConsensusTier::Launch`
    // already uses, and for the same reason: a client that dials the instant a port
    // is bound must not find a listener nobody is accepting on.
    //
    // A node with neither surface enabled adopts nothing and starts no thread.
    nodeIo.Start();

    Cc::Credential const credential { .username = {}, .secret = cfg.token };

    // One registrar per toolchain, because REGISTER carries ONE fingerprint and
    // `--toolchain` is repeatable. Registering only the first -- which is what
    // this did -- meant a worker configured with g++ and clang++ served exactly
    // one of them, and which one depended on where two hex digests happened to
    // sort. The scheduler never heard about the other, so every job for it went
    // to a local compile with nothing anywhere reporting a reason.
    //
    // The scheduler keys a worker on (fingerprint, endpoint), so these are
    // separate entries with separate slot budgets -- which looks like advertising
    // N times this machine's capacity, and is not. Every entry heartbeats the
    // SAME machine-wide in-flight count, so once this worker is busy all of its
    // entries report themselves busy together and the scheduler stops picking any
    // of them. The pool behaves as one because the number it reports describes
    // the machine rather than the entry.
    //
    // The capacity they announce is the machine facts PLUS what the cache tier
    // actually became, and the second half is read off the tier rather than off
    // the configuration that asked for it. `--listen-cache` may have been taken and
    // `--cache-memory 0` may have turned the memory half off, so a node announcing
    // the budget it was configured with would have the leader reporting a cache
    // that is not there. It cannot be folded into `capacity` above either: that one
    // is what `slots` was derived from, and the tier does not exist at that point.
    auto advertised = capacity;
    advertised.cache = Node::CacheCapacityOf(cacheTier.get());

    auto advertisedWire = Distributed::CapacityToWire(advertised);
    // Compiled in, never configurable. The point of the column this feeds is to tell
    // an operator which binary is actually running on each machine -- most often
    // part-way through a rolling upgrade -- and a version a node could be *told* to
    // report is one that can be wrong exactly when somebody is relying on it. It
    // travels inside the capacity record because that is REGISTER's one extensible
    // field; the message's own arity is exact and stays that way forever.
    advertisedWire.version = VersionString;

    std::vector<Cc::WorkerRegistrar> registrars;
    registrars.reserve(toolchains.size());
    for (auto const& [fingerprint, compiler]: toolchains)
        registrars.emplace_back(fingerprint, advertise, slots, Wire::CodecList { Wire::IdentityCodec }, advertisedWire);

    // One sampler for the whole loop, not one per heartbeat. CPU utilization is a
    // difference between two readings, so a sampler constructed per iteration would
    // have no earlier reading to difference against and would report nothing,
    // forever -- a scheduler that never learned this machine was busy, with nothing
    // anywhere saying so.
    //
    // Configured with the scratch path at construction rather than handed one per
    // call: which filesystem this worker writes to is a property of the worker, and
    // the "zero free bytes means the query failed, not that the disk is full" rule
    // belongs beside the query rather than at whoever remembers to apply it.
    auto const loadSampler = MakeHostLoadSampler(MakeSystemCounterSource(jobs.ScratchRoot()));

    // Registration and heartbeating are one loop because they are one concern: a
    // worker is registered exactly as long as it keeps saying so, and a scheduler
    // that has forgotten it answers the heartbeat by telling it to register again.
    // Splitting them would need the two halves to agree about which owns recovery.
    // A thread whose entire job is to block, which is the shape `IConnector`
    // names as correct for a dialler that has no reactor. The connector is a
    // `BlockingConnector` and is passed to `DialEndpointBlocking` by that type,
    // so the `SyncRun` inside it is sound by construction rather than by comment.
    BlockingConnector heartbeatConnector { DefaultAddressResolver(),
                                           BlockingConnectorOptions { .ioTimeout = HeartbeatIoTimeout } };
    std::jthread const heartbeat { [&](std::stop_token const& stop) {
        while (!stop.stop_requested())
        {
            auto client = Cc::DialEndpointBlocking(heartbeatConnector, cfg.scheduler, HeartbeatConnectTimeout);
            if (client == nullptr)
                logger.Logf(LogLevel::Warn, "scheduler {} unreachable", cfg.scheduler);
            else
            {
                // Counted rather than short-circuited: one toolchain the scheduler
                // refuses must not stop the others from being announced, or a
                // single bad entry silently un-registers the whole worker.
                std::size_t accepted = 0;
                auto const inFlight = static_cast<std::uint32_t>(server.InFlight());

                // Sampled once per round rather than once per registrar: every
                // entry describes the SAME machine, so sampling per toolchain would
                // report several different views of one host and, worse, would cut
                // the CPU interval into pieces too short to mean anything.
                auto const sampled = loadSampler->Sample();
                // The cache is sampled here too, and per ROUND rather than per
                // registrar for the same reason: a node with two `--toolchain`
                // flags is two registry entries against one machine and one cache,
                // so both entries carry the same figures. Summing them across
                // entries counts that cache twice, which is what
                // `WorkerRegistry::NodeCaches()` exists to prevent on the other end.
                auto const load =
                    Distributed::LoadToWire(Distributed::NodeLoad { .inFlight = inFlight,
                                                                    .cpuBusyPermille = sampled.cpuBusyPermille,
                                                                    .availableMemoryBytes = sampled.availableMemoryBytes,
                                                                    .freeScratchBytes = sampled.freeScratchBytes,
                                                                    .cache = Node::CacheLoadOf(cacheTier.get(), metrics) });

                for (auto& registrar: registrars)
                {
                    if (!registrar.WorkerId().empty() && registrar.Heartbeat(*client, inFlight, load, credential))
                    {
                        ++accepted;
                        continue;
                    }

                    // The scheduler's own reason, logged per toolchain. The
                    // summary below can only say how many did not register, and
                    // "0 of 1" is exactly as much as an operator knew about a
                    // node that had silently dropped out of the fleet -- a
                    // fingerprint the scheduler will not accept, a cluster this
                    // node is not a member of, a leader that has moved.
                    if (auto const registered = registrar.Register(*client, credential); registered.has_value())
                        ++accepted;
                    else
                        logger.Logf(LogLevel::Warn,
                                    "scheduler {} did not register {}: {}",
                                    cfg.scheduler,
                                    registrar.Fingerprint(),
                                    registered.error());
                }
                bool const ok = accepted == registrars.size();
                logger.Logf(ok ? LogLevel::Debug : LogLevel::Warn,
                            "scheduler {}: {} of {} toolchain(s) registered",
                            cfg.scheduler,
                            accepted,
                            registrars.size());
            }

            // Slept in slices so a stop request is observed promptly: a worker that
            // took a full heartbeat interval to exit would hold its port that long
            // against a restart.
            for (int slice = 0; slice < HeartbeatSlices && !stop.stop_requested(); ++slice)
                std::this_thread::sleep_for(HeartbeatInterval / HeartbeatSlices);
        }
    } };

    // Installed only once the listener is up and the heartbeat is running, so a
    // stop arriving during startup cannot close a listener that does not exist yet.
    InstallNodeStopHandlers();

    // The watcher exists because the two halves of a stop cannot be the same
    // thread: the signal handler may only set a flag, and the accept loop is parked
    // inside Accept() and cannot look at one. Closing the listener is what unparks
    // it -- on POSIX via the poll timeout the loop already treats as "not a
    // failure", which is the mechanism WorkerServer::Run documents.
    //
    // In-flight work needs no separate drain: compiles are served INLINE in the
    // accept loop, so by the time Run() returns there is nothing still running.
    // A worker that detached its jobs would need one here, and would need to
    // decide how long to wait; serving inline is what makes that question not
    // arise.
    std::jthread const stopWatch { [&](std::stop_token const& stop) {
        while (!stop.stop_requested() && !DaemonControls::Instance().StopRequested())
            std::this_thread::sleep_for(StopPollInterval);
        if (DaemonControls::Instance().StopRequested())
        {
            logger.Logf(LogLevel::Info, "stop requested; no longer accepting compiles");
            server.Shutdown();
        }
    } };

    // The listening endpoint is described by where it CAME FROM, not by the
    // config. When a socket was adopted, `--bind` and `--port` were never used,
    // and printing them names an address this process is not listening on -- which
    // in the one line an operator reads to confirm a worker came up is worse than
    // printing nothing. What matters to a client is the advertised endpoint, and
    // that is reported either way.
    auto const listeningOn = activated != nullptr ? std::string { "a socket-activated listener" }
                                                  : std::format("{}:{}", cfg.bindAddress, cfg.port);
    logger.Logf(LogLevel::Info,
                "compile node ready on {}, advertising {}, {} slot(s) as a {} node, {} toolchain(s)",
                listeningOn,
                advertise,
                slots,
                Distributed::TraitsFor(cfg.nodeClass).name,
                toolchains.size());

    SyncRun(server.Run());

    // Unblocks the admin accept loop so its jthread can join -- without this the
    // destructor's implicit join waits on a thread parked in accept(), and a worker
    // that stops cleanly would hang instead. The same reason the daemon does it.
    // `adminEndpoint`'s destructor stops the server and joins its thread; there is
    // deliberately nothing to remember at this return path or any other.

    // No deregistration is sent, and that is a decision rather than a gap. There is
    // no such verb: the scheduler learns a worker is gone by its heartbeat lapsing,
    // which is the ONE mechanism that also covers the cases a polite goodbye cannot
    // -- a killed process, a severed network, a crashed host. Adding a second path
    // would mean the fleet had two ways to believe a worker is alive and only one of
    // them exercised in the failure that matters. A client that leases this worker
    // in the gap finds it unreachable and compiles locally, which is the same
    // fallback every other refusal takes.
    logger.Logf(LogLevel::Info, "compile node stopped");
    return ExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    std::span<char const* const> const argvSpan { const_cast<char const* const*>(argv), static_cast<std::size_t>(argc) };

    NodeConfig cfg;
    auto const flow = ParseOptionsInto(NodeOptions(), argvSpan.subspan(1), cfg);
    if (!flow.has_value())
    {
        std::cerr << "fastcache-compile-node: " << flow.error().context << '\n';
        return ExitUsage;
    }

    if (cfg.help)
    {
        // The color decision is made here rather than inside the help renderer so
        // that module stays free of ambient probes -- and on Windows the call also
        // enables virtual-terminal processing, so it must precede any output.
        std::cout << HelpText(StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain);
        return 0;
    }
    if (cfg.version)
    {
        std::cout << "fastcache-compile-node " << FASTCACHE_NODE_VERSION << '\n';
        return 0;
    }

    ConsoleLogger logger { std::cerr, cfg.logLevel };

    // Service registration, before anything that costs time. A misconfiguration is
    // decided in microseconds while a toolchain fingerprint takes seconds, which is
    // the same cheap-and-fallible-first ordering the socket-activation check follows.
    //
    // The command line is registered as typed. Every other flag alongside
    // --install-service is baked in and reused at every start, which is why the
    // rejections below are install-time rules rather than startup ones: a
    // registration that cannot work must fail here, where an operator is watching,
    // rather than at every boot where nobody is.
    if (cfg.installService || cfg.uninstallService)
    {
        // Only an install has to be viable; an uninstall merely names a
        // registration to remove, and refusing to remove one because it was
        // misconfigured is how a bad registration becomes permanent.
        if (cfg.installService)
            if (auto const rejection = NodeServiceRejection(cfg))
            {
                logger.Logf(LogLevel::Error, "{}", *rejection);
                return ExitUsage;
            }

        auto const spec = MakeNodeServiceSpec(CurrentExecutablePath(), cfg);
        auto const result =
            cfg.installService ? InstallService(spec, cfg.serviceScope) : UninstallService(spec, cfg.serviceScope);
        if (result.exitCode == 0)
            std::cout << "fastcache-compile-node: " << result.message << '\n';
        else
            std::cerr << "fastcache-compile-node: " << result.message << '\n';
        return result.exitCode;
    }

    // A question asked OF a running cluster, rather than a worker starting up.
    // After the service block, because an installation is about this machine and
    // this is about somebody else's; before the `--scheduler` and `--toolchain`
    // checks, because a cluster command needs the first and not the second.
    if (cfg.cluster.action != ClusterAction::None)
    {
        auto const answer = RunClusterAdmin(cfg, cfg.cluster);
        if (!answer.has_value())
        {
            std::cerr << "fastcache-compile-node: " << answer.error() << '\n';
            return ExitUsage;
        }
        std::cout << *answer;
        return ExitOk;
    }

    // Both are refused at startup rather than at the first job. A worker missing
    // either would register (or fail to) and then refuse everything, which presents
    // to an operator as "distribution does not work" rather than as a misconfigured
    // node.
    if (cfg.scheduler.empty())
    {
        logger.Logf(LogLevel::Error, "--scheduler is required; a worker nothing knows about serves nobody");
        return ExitUsage;
    }
    // Only when the machine will not be asked. With discovery on, "no toolchain" is
    // not yet a fact -- it is a question the node answers a few lines later, and
    // refusing here would refuse every worker installed by a package.
    if (cfg.toolchains.empty() && !cfg.toolchainDiscovery)
    {
        logger.Logf(LogLevel::Error,
                    "--no-toolchain-discovery was given and no --toolchain: a worker with none would register "
                    "and then refuse every job the scheduler sent it");
        return ExitUsage;
    }

    // Checked here rather than inside WorkerBody, for the reason the two above are:
    // the POSIX host has already redirected stdout to /dev/null by the time the body
    // runs, so a diagnosis printed there goes nowhere in the one deployment where a
    // scheduler is most likely to be misconfigured.
    if (auto const rejection = StartupPolicyRejection(cfg))
    {
        logger.Logf(LogLevel::Error, "{}", *rejection);
        return ExitUsage;
    }

    // The host is chosen last, so everything that can be reported to a terminal
    // already has been. `--daemon` is what a SUPERVISOR THAT WANTS BACKGROUNDING
    // passes: the Windows SCM needs it, and systemd and launchd must not pass it,
    // because they supervise the process they started and reap a job that forks
    // as "exited".
    std::unique_ptr<IDaemonHost> host;
    if (cfg.daemon)
    {
#if defined(_WIN32)
        host = MakeWindowsServiceHost(cfg.serviceName);
#else
        host = MakePosixDaemonHost(cfg.pidfile);
#endif
    }
    if (!host)
        host = std::make_unique<ForegroundHost>();

    return host->Run([&cfg, &logger] { return WorkerBody(cfg, logger); });
}
