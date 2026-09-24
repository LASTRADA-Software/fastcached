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
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "ConsensusTier.hpp"
#include "CordonCli.hpp"
#include "DiscoveryTier.hpp"
#include "EnrollClient.hpp"
#include "EnrollmentResponder.hpp"
#include "EnrollmentWindow.hpp"
#include "FleetTextResponder.hpp"
#include "LiveStatsResponder.hpp"
#include "LiveStatsSources.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIdentity.hpp"
#include "NodeIoLoop.hpp"
#include "NodeKey.hpp"
#include "NodeLogging.hpp"
#include "NodeMembership.hpp"
#include "NodePresenceTier.hpp"
#include "NodeProofResponder.hpp"
#include "NodeReload.hpp"
#include "NodeRoster.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeSurfaces.hpp"
#include "NodeToolchains.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerTier.hpp"
#include "ScratchClaim.hpp"
#include "WorkerLease.hpp"
#include "WorkerTier.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Config/SecretExposureWatcher.hpp>
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/ReadinessMarker.hpp>
#include <FastCache/Core/StopAwareWait.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/InheritedListener.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/NarrowText.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Platform/WindowsEventLogger.hpp>
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
#include <functional>
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
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/net/BlockingSocket.hpp>

namespace
{
using namespace FastCache;
using namespace FastCache::Node;

/// How often the stop watcher looks at the stop flag.
///
/// A signal handler may portably do almost nothing -- it sets a flag -- so
/// something else has to notice and close the listener, and it cannot be the
/// accept loop, which is parked inside `Accept()`. Short enough that `systemctl
/// stop` and Ctrl-C feel immediate, long enough to cost nothing while idle.
///
/// **A poll, deliberately, where the heartbeat and the enrollment ticker WAIT
/// (`CompileCapacity::WaitForHeartbeat` and `WaitForStopOr`, #1339).** Those two wait
/// on a `std::stop_token` that another
/// thread requests; this loop waits on `DaemonControls`' flags, which a signal or
/// console handler sets, and a handler may only store an atomic -- notifying a
/// condition variable from one is not async-signal-safe. Nothing can wake a wait
/// here without a new mechanism (a self-pipe, an event handle), and the same loop
/// also takes reload requests off the other flag.
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

/// Record a reload request.
///
/// Same constraint as the stop handler: a signal handler sets a lock-free flag and
/// does nothing else. The re-read, the immutability check and the logging all happen
/// on the thread that notices the flag.
extern "C" void HandleNodeReloadSignal(int /*signum*/)
{
    DaemonControls::Instance().RequestReload();
}

/// Ask for a graceful stop, and for a reload, on the signals a supervisor sends.
///
/// **SIGHUP used to be deliberately unhandled, and the argument for that is now the
/// argument for the `reloadable` column.** It read: *this worker has nothing it could
/// reload -- its toolchain table is what its registration advertised, so re-reading it
/// would leave the scheduler dispatching against a set this worker no longer serves.*
///
/// The premise stopped being true when [#291](https://github.com/LASTRADA-Software/fastcached/issues/291)
/// gave the node a configuration file. **The reasoning did not**, and it is exactly why
/// reloadability is a column of `NodeOptions()` rather than a property of the reload
/// path: those fields must refuse to change, for precisely the reason that paragraph
/// gives. Restated rather than deleted, because a correct argument attached to a false
/// premise is worth more than either half
/// ([#292](https://github.com/LASTRADA-Software/fastcached/issues/292)).
///
/// So SIGHUP is handled, and **what it can change is decided by the table** --
/// `NodeOptions()`'s `reloadable` column, which is the one place that answer lives.
///
/// This sentence used to finish *"today only `--log-level`"*, and that was a second
/// source of truth for a fact the table already held. It had gone false: the column
/// marks several rows now, among them `--requirepass` and the flags that decide who
/// this node admits. So a reader who believed the comment thought a SIGHUP could not
/// rotate this node's credential or change its admission policy, when it can do both
/// -- which is the wrong direction to be wrong in, and exactly the reading somebody
/// reaches for while deciding whether a reload is safe to send.
///
/// **No corrected count replaces it.** A restated figure drifts again, and the reason
/// to name one here was never good: whoever needs the set can read the column, and
/// whoever needs it to be RIGHT needs it in one place rather than two that agree
/// today. `--log-level` is still the field an operator reaches for mid-incident and
/// the one `ILogger` can be told about while running -- an example, not the set.
void InstallNodeStopHandlers()
{
    std::signal(SIGINT, &HandleNodeStopSignal);
    std::signal(SIGTERM, &HandleNodeStopSignal);
#if !defined(_WIN32)
    // POSIX only: Windows has no SIGHUP, and a service there is reconfigured through
    // the SCM control handler rather than by a signal.
    std::signal(SIGHUP, &HandleNodeReloadSignal);
#endif
}

/// The descriptor a supervisor handed this worker, when there was one.
///
/// Separated from `main` so the whole handoff -- how many descriptors arrived, and
/// whether the configuration can still describe this worker afterwards -- is one
/// decision with one answer, rather than three checks interleaved with everything
/// else a startup does.
///
/// **A descriptor rather than a listener**, since #290 stage 3. The merged `0xFC`
/// surface runs on the reactor, and the listener that serves it is built by
/// `FrameEndpoint::StartAdopted` from this descriptor; building one here would own
/// it, and handing an owned descriptor on is a double close rather than a handover.
///
/// Nothing here closes what it returns. On the two refusal paths the process exits
/// immediately, and on the success path ownership passes to the listener -- which
/// takes it even when the adoption itself fails.
/// @param cfg What the operator asked for.
/// @param logger Where the handoff is announced.
/// @return The descriptor, `std::nullopt` when nothing was handed over, or why the
///         handoff cannot be served.
[[nodiscard]] std::expected<std::optional<int>, std::string> ActivatedDescriptor(NodeConfig const& cfg, ILogger& logger)
{
    // When a supervisor already bound the port and handed the descriptor over,
    // binding it again would fail with "address already in use" -- against
    // ourselves. Falling through to the ordinary bind when nothing was handed over
    // is what lets one binary serve both a `.socket` unit and a plain
    // `--listen-node`, with no flag distinguishing them: the environment says which,
    // and it says so unambiguously.
    auto const inherited = AdoptInheritedDescriptors();
    if (inherited.empty())
        return std::optional<int> {};

    // Only the first would be used. This worker answers one protocol on one port, so
    // a unit listing several sockets is a misconfiguration -- reported rather than
    // half-honoured, since silently ignoring the rest would leave an operator with a
    // port that accepts nothing and no clue why.
    if (inherited.size() > 1)
        return std::unexpected { std::format("socket activation handed over {} listeners; this worker serves exactly one",
                                             inherited.size()) };

    // Socket activation makes --advertise mandatory, because the fallback becomes a
    // guess the process cannot make. `--listen-node` was not used -- the socket unit
    // chose the address and this process is never told which -- so the fallback would
    // register a value from configuration that describes nothing, and the wildcard is
    // not an address a remote client can dial anyway.
    //
    // The consequence of guessing is the worst-shaped failure this system has: the
    // registration SUCCEEDS, the worker heartbeats happily, the scheduler leases that
    // endpoint to clients, and every one of them fails to connect and compiles
    // locally. Nothing reports an error, and the fleet looks healthy from both ends.
    // Refusing at startup, where it can be explained, is the whole difference.
    if (cfg.advertise.empty())
        return std::unexpected { std::string {
            "--advertise is required under socket activation: the socket unit owns the port, so this worker "
            "cannot know what address clients should use" } };

    logger.Logf(LogLevel::Info, "a supervisor handed over a listening socket; --listen-node is not used");
    return std::optional { inherited.front() };
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

/// Report a one-shot operator verb's answer and say what to exit with.
///
/// **Three verbs, one shape.** A cluster command, an enrollment decision and a
/// machine asking to be let in all do the same two things with their runner's
/// answer: print a refusal to stderr and exit `ExitUsage`, or print the answer to
/// stdout and exit `ExitOk`. Written out at each site that was three copies of one
/// branch pair -- blocks diverging by a name, which this codebase treats as a defect
/// on its own -- and it is also what took `main` past the cognitive-complexity
/// threshold the build enforces when the enrollment verbs added the second and
/// third. `AnnounceOnce` above was split out of `WorkerBody` for the same reason, so
/// this is the file's own idiom rather than a new one.
///
/// The runner CALL stays at each call site, because that is the part that genuinely
/// differs -- and so does the credential, which is constructed per site so that the
/// day one of these is asked from a running worker it reads the live secret rather
/// than one captured here.
/// @param answer What the runner said.
/// @param prefix Prepended to a successful answer, for the verbs that name the binary.
/// @return The process exit code.
[[nodiscard]] int ReportOneShotVerb(std::expected<std::string, std::string> const& answer, std::string_view prefix = {})
{
    if (!answer.has_value())
    {
        std::cerr << "fastcache-compile-node: " << answer.error() << '\n';
        return ExitUsage;
    }

    std::cout << prefix << *answer;
    return ExitOk;
}

/// Whether this node will serve an enrollment surface.
///
/// Two clauses, and they answer different KINDS of question. `RunsConsensus` is the
/// RULE, asked of the configuration, and `tier != nullptr` is a runtime fact about
/// what was actually built, which only this translation unit knows and which no test
/// links.
///
/// **There was a third clause, a named `--cluster-key-file`, and it is gone for good**: an
/// approval no longer hands over any key (#178), so a window has nothing a key file could be
/// missing for.
///
/// Named rather than spelled inline because `WorkerBody` is at the
/// cognitive-complexity ceiling the build enforces, and because the two sites that
/// need this answer must not be able to disagree: reporting a window no verb can act
/// on is the reading the repeating open-window warning exists to make impossible.
/// @param cfg The parsed configuration.
/// @param tier The scheduler tier, or nullptr when none was started.
/// @return True when both halves hold.
[[nodiscard]] bool ServesEnrollment(NodeConfig const& cfg, Node::SchedulerTier const* tier) noexcept
{
    return Node::RunsConsensus(cfg) && tier != nullptr;
}

/// The address held by @p slot, or nullptr when it holds nothing.
///
/// **A component this node does not run is a NULL source, and an `optional` spells
/// that with a ternary at every site.** Named instead, because `WorkerBody` carried
/// several copies of that one branch -- blocks diverging by a name, and each one a
/// charge against the cognitive-complexity threshold the enrollment surface pushed
/// that function over. It reads as what it means at the call site, which a ternary
/// buried in a designated initialiser does not.
/// @tparam T What the slot may hold.
/// @param slot The optional to read.
/// @return Its address, or nullptr.
template <typename T>
[[nodiscard]] T* AddressOrNull(std::optional<T>& slot) noexcept
{
    return slot.has_value() ? &*slot : nullptr;
}

/// The scheduler service this node runs, or nullptr when it runs none.
///
/// The third member of `AddressOrNull`'s family, and the one that cannot be either of
/// the others: the source only EXISTS behind the tier, so the test and the dereference
/// cannot be separated and an eager helper taking the value would dereference a null
/// tier to build its argument. Named here for the same two reasons as its siblings --
/// it reads as what it means at the call site, and `WorkerBody` sits one point under
/// the cognitive-complexity ceiling the build enforces.
/// @param tier The scheduler tier, or nullptr when none was started.
/// @return The service, or nullptr.
[[nodiscard]] Distributed::SchedulerService const* ServiceOrNull(Node::SchedulerTier const* tier) noexcept
{
    return tier != nullptr ? &tier->Service() : nullptr;
}

/// @p value's address when @p present, and nullptr otherwise.
///
/// The sibling of `AddressOrNull` for a source this node OWNS unconditionally but
/// reports on only when it serves the component -- the enrollment window is held
/// whatever this node runs, because it is two words and a mutex, so whether to report
/// it is a separate question from whether it exists.
/// @tparam T The source's type.
/// @param present Whether this node serves the component.
/// @param value The source.
/// @return Its address, or nullptr.
template <typename T>
[[nodiscard]] T* AddressWhen(bool present, T& value) noexcept
{
    return present ? &value : nullptr;
}

/// How often the open window is asked whether a warning is due.
///
/// A cadence, not a teardown cost: the wait below carries the stop token, so a stop is
/// observed immediately whatever this is. It is stated beside its one reader rather
/// than in the constants block above, because reading it apart from that sentence is
/// what invites shortening it to buy responsiveness it does not buy.
constexpr std::chrono::seconds EnrollmentWarningTick { 1 };

/// Log the enrollment window's due warning until asked to stop.
///
/// Split out of `WorkerBody` for `AnnounceOnce`'s two reasons below, the second being
/// the load-bearing one: a `while` holding an `if` costs that function more
/// cognitive-complexity budget than either construct suggests, because the inner test
/// is charged for its nesting as well as itself -- and a loop with a decision in it is
/// more behaviour than belongs in the one translation unit no test reaches.
///
/// It owns no rule. The DECISION is `EnrollmentWindow::TakeDueWarning`, which is pure
/// over an injected clock and is tested against a `core::platform::ManualClock`; this is the driver.
///
/// The stop token is IN the wait rather than checked between sleeps (`WaitForStopOr`),
/// so a stop ends this loop at once. `EnrollmentWarningTick` therefore bounds only how
/// late a DUE warning is logged; it is not a teardown cost. The heartbeat loop waits the
/// same way, through the same function.
/// @param window The window to ask.
/// @param logger Where a due warning is written.
/// @param stop Participates in the wait, so a requested stop ends it immediately.
void WarnWhileWindowIsOpen(Node::EnrollmentWindow& window, ILogger& logger, std::stop_token const& stop)
{
    while (!stop.stop_requested())
    {
        if (auto const due = window.TakeDueWarning(); due.has_value())
            logger.Logf(LogLevel::Warn, "{}", *due);
        if (WaitForStopOr(stop, EnrollmentWarningTick) == WaitEnd::Stopped)
            break;
    }
}

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
/// The node's reloader, named once in `NodeReload.hpp` so that the policies reading
/// the live snapshot -- `Node::ReloadedCredential` among them -- do not each respell
/// `ConfigReloaderOf<NodeConfig>`.
using Node::NodeReloader;

/// Resolve this node's identity and put it into both configurations.
///
/// A function rather than a block in `main` for the reason `StartCacheTierOrExplain`
/// is one: it is a coherent decision with one answer, and inline it pushed `main` past
/// clang-tidy's cognitive-complexity limit. What it decides is `NodeIdentity`'s and not
/// this file's -- `main.cpp` is in no test target, so a rule spelled here is one
/// nothing can assert.
///
/// BOTH configurations, because they are used for two different things and each is
/// wrong without it: @p cfg is what this process runs with and what the startup table
/// judges, and @p cliOnly is what a service registration bakes in.
/// @param cfg The merged configuration, completed in place.
/// @param cliOnly The command-line-only parse a registration is built from.
/// @param random Where a minted identity's bits come from.
/// @param logger Where the identity, or the refusal, is reported.
/// @param publicKey The key this node holds, already resolved (#178); nothing on the install
///        path, which mints no key, and on a node that holds none.
/// @return The identity, DISENGAGED when this invocation needs none, or why it could
///         not be resolved. `std::expected` over a nested optional, because "no
///         identity is wanted here" and "one was wanted and could not be had" are the
///         two states a caller acts on differently and two optionals render alike. A node
///         that needs no id but holds a key gets an identity carrying only the key, so the
///         one `ApplyNodeIdentity` a reload runs applies it too.
[[nodiscard]] std::expected<std::optional<Node::NodeIdentity>, std::string> AdoptNodeIdentity(
    NodeConfig& cfg,
    NodeConfig& cliOnly,
    ISecureRandom& random,
    ILogger& logger,
    std::optional<Ed25519PublicKey> const& publicKey)
{
    if (Node::NodeIdentityNeed(cfg) != Node::IdentityNeed::Mint)
    {
        if (!publicKey.has_value())
            return std::optional<Node::NodeIdentity> {};
        auto keyOnly = Node::NodeIdentity { .id = {}, .origin = Node::NodeIdentityOrigin::Recorded, .publicKey = publicKey };
        Node::ApplyNodeIdentity(cfg, keyOnly);
        return std::optional { std::move(keyOnly) };
    }

    auto resolved = Node::ResolveNodeIdentity(Node::NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!resolved.has_value())
        return std::unexpected { std::move(resolved).error() };
    resolved->publicKey = publicKey;

    Node::ApplyNodeIdentity(cfg, *resolved);
    Node::ApplyNodeIdentity(cliOnly, *resolved);

    // Which of the three it was, said out loud once. "This node kept the identity it
    // had" and "this node invented one" are the pair that matters and the pair a silent
    // start cannot tell apart -- the second is a machine the cluster has never
    // admitted, and an operator who sees it after a restart has lost a state directory.
    logger.Logf(LogLevel::Info, "node identity {} ({})", resolved->id, Node::DescribeNodeIdentityOrigin(resolved->origin));
    return std::optional<Node::NodeIdentity> { *std::move(resolved) };
}

/// Resolve the identity key this node holds, and say so once.
///
/// A function rather than a block in `main` for `AdoptNodeIdentity`'s reason, and what it
/// decides is `NodeKey`'s: this only reports it. The PUBLIC half is logged whole, in the one
/// spelling `--node-status` and `@<key>` share; the secret half is never logged. It is held
/// for the process's life, because the Raft peer wire signs every handshake with it (#178),
/// and read ONCE: the key a node announces and the key it proves itself with are then one
/// reading of one file rather than two that a replaced file could make disagree.
/// @param cfg The resolved configuration.
/// @param random Where a minted key's seed comes from.
/// @param logger Where the key, or its absence, is reported.
/// @return The key pair, DISENGAGED on a node that holds none, or why there is none.
[[nodiscard]] std::expected<std::optional<Ed25519KeyPair>, std::string> AdoptNodeKey(NodeConfig const& cfg,
                                                                                     ISecureRandom& random,
                                                                                     ILogger& logger)
{
    auto nodeKey = Node::ResolveNodeKeyFor(cfg, random);
    if (!nodeKey.has_value())
        return std::unexpected { std::move(nodeKey).error().message };
    if (!nodeKey->has_value())
    {
        logger.Logf(LogLevel::Info, "{}", Node::NoNodeKeySentence);
        return std::optional<Ed25519KeyPair> {};
    }

    auto& held = **nodeKey;
    logger.Logf(LogLevel::Info,
                "identity key {} ({})",
                FormatEd25519PublicKey(held.pair.PublicKey()),
                Node::DescribeNodeKeyOrigin(held.origin));
    return std::optional { std::move(held.pair) };
}

/// The public half of a key this node may hold.
/// @param key The key pair, or nothing.
/// @return Its public key, or nothing.
[[nodiscard]] std::optional<Ed25519PublicKey> PublicHalf(std::optional<Ed25519KeyPair> const& key)
{
    return key.transform([](Ed25519KeyPair const& pair) { return pair.PublicKey(); });
}

[[nodiscard]] int WorkerBody(NodeConfig const& cfg,
                             std::optional<Ed25519KeyPair> const& identityKey,
                             ILogger& logger,
                             NodeReloader* reloader)
{
    // ONE origin for every uptime this process reports. `/healthz` and the `0xFC`
    // `NodeStatus` verb both answer *how long has this been serving*, and two
    // independently sampled origins are two numbers that can disagree about one fact --
    // an operator comparing a scrape against the CLI would be reading a difference that
    // describes nothing. Taken as the first statement, so it is this body's own start
    // rather than whichever startup step happened to be declared above the reader.
    auto const startedAt = std::chrono::steady_clock::now();

    // Socket activation is resolved BEFORE the toolchains, and the order is
    // deliberate. Computing a fingerprint walks the whole include tree and takes
    // seconds; a bad handoff is decided in microseconds. Doing the cheap, fallible
    // thing first means a misconfigured unit fails immediately instead of after a
    // multi-second pause -- and it means the startup log reads in the order things
    // actually happened, so an operator watching a worker come up sees what it did
    // with the socket before the long quiet part.
    auto const activatedOrError = ActivatedDescriptor(cfg, logger);
    if (!activatedOrError.has_value())
    {
        logger.Logf(LogLevel::Error, "{}", activatedOrError.error());
        return ExitUsage;
    }
    auto const activated = *activatedOrError;

    // The ONE derivation, shared with the startup refusal that judges it. This value
    // goes to the worker tier's lease validator and to its REGISTER, and a lease's MAC is
    // taken over exactly this string -- so the endpoint the scheduler
    // signs, the endpoint this worker verifies and the endpoint clients dial are one
    // fact with one author. See `AdvertisedEndpoint`.
    auto const advertise = Node::AdvertisedEndpoint(cfg);

    // **One per process, and that is the whole reason it is here rather than inside the worker
    // tier** (#1440). Both the worker's registrations and the presence loop's announcements
    // name this machine's address, and two sources would be two values changing at two
    // moments -- the defect #1279 closed inside the worker, reopened between two components.
    // Seeded with what the process started with; the worker tier republishes it when a
    // re-survey finds the configuration's answer has moved, and is its only writer.
    //
    // Declared up here, above every component that reads it, so it outlives all of them.
    Node::AnnouncedEndpoint announced { advertise };

    // The descriptor travels to `StartNodeSurfaceOrExplain` below and is served
    // there. It used to be refused here, for the two months between the surfaces
    // merging and the reactor listeners learning to adopt: `AdoptInheritedListeners`
    // handed back a BLOCKING listener, the merged surface runs on the reactor, and
    // there was nothing that could join the two. #464 added `AdoptInheritedListener`
    // and this is the last stitch of #290 stage 3 closing over it.
    //
    // The interim was a refusal rather than an adoption left unserved, and that shape
    // is worth keeping in mind for the next such gap: a packaged Linux install enables
    // the worker THROUGH the socket unit -- the `.service` deliberately has no
    // `[Install]` section -- so a node that took the descriptor and then answered
    // nothing on it would have been the silent failure this whole ticket exists to
    // remove, on the deployment path most people use.

    // **A node that works for other machines and opens no admin surface is told once,
    // at INFO** (#1304). `--admin-listen` is off unless asked for, so such a node has
    // no `/healthz`, no `/metrics` and no dashboard: everything works and nothing off
    // this machine can see it. The default is deliberately NOT flipped -- binding a
    // port nobody asked for is the decision this binary refuses everywhere.
    //
    // WHO it applies to and WHAT it says are `Node::ObservabilityAnnouncement`'s, for
    // the reason the line above delegates: this file is in no test target (#909), so a
    // rule written here could only ever be checked by reading it. INFO rather than the
    // Warn beside it because nothing was widened and nothing is wrong.
    if (auto const said = Node::ObservabilityAnnouncement(cfg))
        logger.Log(LogLevel::Info, *said);

    AtomicMetricsSink metrics;

    // **What this node has detected that an operator must act on, as one table** (#1364). Every
    // row was log-only before, and a log line scrolls away. Declared above every component that
    // raises or clears a row, so it outlives all of them; each component that runs answers its own
    // rows as it starts, and `Settle` below answers the rest before any surface serves.
    //
    // The process scope first, because it needs nothing but the sink: whether this build's
    // catalogue and sink agree is fixed before the process serves anything.
    Node::NodeConditions conditions;
    Node::EvaluateProcessConditions(conditions, metrics);

    // One policy for all THREE surfaces -- the compile port here, the scheduler and
    // the cache below -- and it outlives every one of them. A node that answered "is
    // this peer one of ours" differently at two of its surfaces would admit a peer to
    // the fleet and refuse it the objects that fleet produced, or worse, the reverse.
    // Not `const`: consensus republishes the member set into it while the node runs,
    // which is the whole point of membership being a replicated log entry rather than
    // a command-line list.
    //
    // It reports a `--fleet-member` entry the cluster has forgotten only where there IS a cluster:
    // `RunsConsensus`, the one predicate every consensus-dependent site asks.
    Node::NodeMembership membership { cfg, logger, AddressWhen(Node::RunsConsensus(cfg), conditions) };

    // **The roster every lease grant is verified against** (#178), built before anything that
    // reads it: the worker's validator borrows it, consensus feeds it every applied state and
    // every endorsement this node signs, and the presence loop carries the endorsement out and
    // the certified roster back. A WALL clock, because a certificate's lapse was stamped on
    // another machine.
    //
    // Refusing here is where `RosterlessWorkerRefusal` is answered, since only the state
    // directory knows whether it holds a roster, and a kept roster this build cannot use -- never
    // a quiet fall back to the `--voter-key` anchors, which may name a voter revoked since.
    core::platform::SystemWallClock const rosterWallClock;
    auto rosterOrRefusal = Node::NodeRoster::Build(cfg, rosterWallClock, metrics, logger);
    if (!rosterOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", rosterOrRefusal.error());
        return ExitUsage;
    }
    auto const nodeRoster = std::move(*rosterOrRefusal);

    // The worker server and the admin endpoint are both built BELOW the cache tier,
    // and in both cases moving them down was the fix rather than tidying: one takes
    // a slot count that is not knowable until the tier has been built or not been
    // (#167), and the other's snapshot lambda has to report the node's cache -- so
    // captured up here it could only ever say `.storage = std::nullopt`, which is
    // exactly what it said, comment and all, for as long as this program has had a
    // cache.

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

    // The two framed COMPONENTS this node may serve besides its worker port, each
    // owned as one object. Both are off unless asked for: handing out other machines'
    // CPU time, and caching to this machine's disk, are decisions an operator makes
    // rather than things they get by starting a worker.
    //
    // Components rather than surfaces since #290: they share one `0xFC` listener,
    // opened below once both exist, and neither binds anything of its own. The order
    // here is therefore load-bearing in a new way -- the listener holds a reference to
    // each, so it is declared after both and destroyed before them.
    core::platform::SteadyClock schedulerClock;
    core::platform::SteadyClock cacheClock;

    // Wall-clock, and separate from the steady clocks beside it, because a lease
    // grant's expiry is checked on ANOTHER machine: a steady instant is meaningless
    // off the host that read it, while a system-clock instant is comparable anywhere
    // -- which is also why the check that reads it carries skew slack.
    core::platform::SystemWallClock const schedulerWallClock;

    std::unique_ptr<Node::SchedulerTier> schedulerTier;
    if (cfg.serveScheduler)
    {
        auto started = Node::SchedulerTier::Start(
            cfg, membership.Oracle(), schedulerClock, schedulerWallClock, metrics, logger, identityKey);
        if (!started.has_value())
        {
            // Fatal for the same reason the admin endpoint's is: an operator who asked
            // for this is relying on it, and a node that started without it looks
            // healthy to everything that would otherwise have noticed.
            logger.Logf(LogLevel::Error, "--serve-scheduler {}; refusing to start", started.error());
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
    // Who may read this machine's build output is "this machine", full stop (#287)
    // -- not the member list beside it, which names peers that may spend this node's
    // CPU. The two questions were one list until a fleet peer could FETCH every
    // object this machine had ever compiled.
    //
    // The answer is ambient, so it arrives through a seam with a clock, and the set
    // is refreshed on an interval rather than per request: `GetAdaptersAddresses`
    // costs milliseconds on Windows, and a probe a stranger could provoke by asking
    // is a probe a stranger can bill this machine for. `CachedLocalityOracle`
    // carries both failure directions.
    //
    // And who may cordon this worker is "this machine" for the same reason (#1303):
    // whether a machine's CPU serves the fleet is decided on that machine. One oracle,
    // asked by both, so the two surfaces cannot disagree about which machine this is.
    auto const hostAddresses = MakeSystemHostAddresses();
    CachedLocalityOracle const locality { *hostAddresses, cacheClock };

    // ONE source, and every site that presents this worker's credential borrows it.
    // Declared here because the first of those sites is the cache tier immediately
    // below and the last is the heartbeat round far further down, and a local declared
    // between them would be an object two consumers reach and one of them outlives.
    //
    // It reads the RELOADER rather than `cfg`, which is the whole of #404: `cfg` is
    // the configuration this worker STARTED with, and a rotated `--requirepass` lives
    // in the snapshot the reloader publishes. A worker with no configuration file
    // passes null and gets `cfg`'s value forever, which is correct rather than a
    // fallback -- it has no second moment for anything to arrive at.
    Node::ConfiguredCredential const credential { cfg, reloader };

    auto cacheTierOrRefusal = Node::StartCacheTierOrExplain(nodeIo, cfg, credential, locality, cacheClock, metrics, logger);
    if (!cacheTierOrRefusal.has_value())
    {
        // No flag prefix here, unlike its neighbours: this tier can fail over two
        // different flags -- the directory and the port -- and only
        // `StartCacheTierOrExplain` knows which, so it names it.
        logger.Logf(LogLevel::Error, "{}; refusing to start", cacheTierOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: `StartCacheTierOrExplain` treats an emptied
    // `--listen-node`, and nowhere to keep objects, as reasons to
    // carry on without a tier rather than as failures. Both have been logged.
    auto const cacheTier = std::move(*cacheTierOrRefusal);

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
    //
    // And it is computed BELOW the cache tier, which is the whole of #167: what a
    // compile cannot have is what the tier actually HOLDS, which is what the
    // configuration asked for only sometimes. `NodeCapacityOf`'s contract carries the
    // three ways those differ; this is the ordering that lets it be honoured.
    auto const host = MakeSystemHostFacts();
    auto const capacity =
        Node::NodeCapacityOf(cfg, *host, Node::CacheCapacityOf(cacheTier.get()), Node::IndexReserveBytesOf(cacheTier.get()));

    // Where a node handshake's nonce and ephemeral key come from, on both ends: the operating
    // system's generator (#1527). Its own instance rather than a share of `identityRandom`, which
    // is a different lifetime -- an identity is minted once at startup and handshakes are drawn
    // for as long as the process serves.
    SystemSecureRandom proofRandom;

    // **How this machine proves WHICH machine it is to a scheduler** (#178): its id, the identity
    // key it holds, whom it may prove itself to -- the roster its grants are checked against --
    // and the generator above. Built wherever this node names a scheduler; `SchedulerNeedsIdentity`
    // refuses a node that does so without an identity, so a serving node that announces always
    // has one. Declared ABOVE the worker and presence tiers, which borrow it.
    std::optional<Node::NodeProofClient> prover;
    if (identityKey.has_value() && !cfg.schedulers.empty())
        prover.emplace(cfg.nodeId, *identityKey, *nodeRoster, proofRandom);

    // The worker: survey, scratch root, lease check, slot cap, compile responder and
    // heartbeat, as one object (#1387). Built BELOW the cache tier, because the slots it
    // offers are what the tier built leaves (#167), and ABOVE the node surface, which
    // routes the compile family to it and is therefore destroyed first. Null on a node
    // started with `--slots=0` (#206): no pool thread, no validator, no heartbeat.
    auto workerOrRefusal =
        Node::WorkerTier::Start(Node::WorkerTierParts { .cfg = cfg,
                                                        .reloader = reloader,
                                                        .capacity = capacity,
                                                        .announced = announced,
                                                        .activation = activated.has_value() ? Node::SocketActivation::Yes
                                                                                            : Node::SocketActivation::No,
                                                        .membership = membership.Oracle(),
                                                        .locality = locality,
                                                        .io = nodeIo,
                                                        .host = *host,
                                                        .cacheTier = cacheTier.get(),
                                                        .credential = credential,
                                                        .prover = AddressOrNull(prover),
                                                        .leaseRoster = nodeRoster->Lease(),
                                                        .metrics = metrics,
                                                        .logger = logger,
                                                        .conditions = conditions },
                                &Node::MakeSystemWorkerMachine);
    if (!workerOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", workerOrRefusal.error());
        return ExitUsage;
    }
    auto const workerTier = std::move(*workerOrRefusal);

    // This node's one `0xFC` listener, opened once every component exists and holding a
    // reference to each (#290). Before the merge each tier bound its own port, which
    // made the listener the routing decision; now `MergedResponder` decides per frame
    // and the port is a property of the node rather than of any one component.
    //
    // Declared AFTER every component it routes to -- both tiers, the worker and its
    // responder -- and therefore destroyed BEFORE them, which is what keeps the router
    // from outliving what it routes to. And before consensus, because what a leader
    // advertises for the scheduler is what this BOUND.
    // What this node IS, for the operator verbs below. Its own clock, because uptime is
    // the one field that moves between requests and `Describe()` is asked per request.
    core::platform::SteadyClock const statusClock;

    // **Observed, never inferred from a flag** -- with one deliberate exception, below.
    // A `--cache-dir` that would not open has already stopped startup, so a tier read off
    // the flag that asked for it would report a component that is not there; the pointer
    // is what actually started.
    //
    // `worker` answers *does this node have a worker component*, and since #206 that has
    // two answers: `--slots=0` runs none. It was a literal `true` until then, because
    // nothing could make it false -- #1295 asked for it to be conditional and #1315
    // declined, rightly, because what #1295 was really reading it for is *is that worker
    // serving anything yet*: a different question with three answers, which is
    // the worker tier's runtime state rather than a fourth reading of this bool. The bit
    // is the component, never the state, whichever way it answers.
    //
    // `consensus` is the exception, and it is the STRONGER answer rather than a weaker
    // one: the tier is constructed BELOW this point, so there is no pointer to read, and
    // `RunsConsensus` is the one predicate `StartConsensusOrExplain` itself asks (#1022,
    // #613). Reporting it cannot disagree with whether a tier gets built, which a second
    // spelling of the same question could.

    // **Whether this node serves enrollment at all, asked ONCE.**
    //
    // Two sites need it -- what `NodeStatus` REPORTS about a window, and whether the
    // responder that serves one is built -- and they must agree or an operator is shown
    // a window no verb can act on: `--enroll-list` against a node advertising `open`
    // would meet a refusal from a surface that does not exist, which is the one reading
    // the repeating open-warning is designed to make impossible to miss.
    //
    // They were two conditions, and the comment below this one already argued they must
    // not be able to disagree -- *"`RunsConsensus` already implies a scheduler tier,
    // which is why this is one condition rather than two that could disagree"* -- while
    // the code under it spelled `RunsConsensus(cfg) && schedulerTier != nullptr` at one
    // site and `RunsConsensus(cfg)` alone at the other. A stated invariant does not hold
    // itself; naming it once is what does. If the implication ever stops being true, the
    // two sites move together because there is only one of them.
    //
    // No test reaches this: `main.cpp` is the translation unit none of them links, so
    // the guard here is CONSTRUCTION rather than a case -- a test built around a second
    // copy of this expression would assert something other than what ships.
    auto const servesEnrollment = ServesEnrollment(cfg, schedulerTier.get());

    // Where this node sits in its consensus configuration (#1449), for `NodeStatus`. A SLOT,
    // because the tier that answers is built below and this surface is built now -- see
    // `ConsensusStandingSlot`. Declared before the status that reads it, so destroyed after.
    Node::ConsensusStandingSlot consensusStanding;

    // The runtime enrollment window, and the key source it hands over from.
    //
    // Held whatever this node runs, because it is two words of state and a mutex, and
    // declared here so it outlives both the surface that mutates it and the status
    // source that reads it. **What decides whether it is REACHABLE is
    // `servesEnrollment` above** -- `RunsConsensus`, which is the one predicate the
    // consensus tier itself asks so the surface and the tier cannot disagree about
    // whether this node has a cluster, AND a scheduler tier, without which the responder
    // has no references to hold. A node missing either leaves the component null and the
    // whole family is refused at the door: a window that could never admit anybody
    // should not be openable, and one nothing serves should not be reported.
    //
    // It reports itself as a condition only where it can be opened at all: `servesEnrollment`
    // above, so a node with no window reports that row NOT EVALUATED rather than a reassuring
    // `clear` for a window nothing can open (#1364).
    Node::EnrollmentWindow enrollmentWindow { statusClock, AddressWhen(servesEnrollment, conditions) };

    Node::ConfiguredNodeStatus const nodeStatus {
        cfg,
        statusClock,
        startedAt,
        std::string { VersionString },
        cfg.nodeId,
        Node::NodeComponents { .cacheTier = cacheTier != nullptr,
                               .worker = workerTier != nullptr,
                               .scheduler = schedulerTier != nullptr,
                               .consensus = Node::RunsConsensus(cfg) },
        // `capacity`, `scheduler` and `enrollment` are read LIVE; only `runtime` is
        // published into. All three are already in scope and already thread-safe, and a
        // node running no scheduler passes a null -- which is what makes an absent role
        // mean *runs no scheduler* rather than *is in an election*.
        //
        // The enrollment pointer follows the same rule one step further: a node with no
        // cluster reports NOTHING about a window rather than reporting one that is shut,
        // because a reassuring `closed` for a thing that does not exist is exactly the
        // reading that stops an operator looking.
        //
        // The worker's two readings are the worker tier's, read through its runtime state
        // -- the ONLY thing `Describe()` reaches the survey through, since the served map
        // has one writer and no lock. A node running none reports no toolchains and no
        // slots at the CELL, which is *no worker* rather than a worker surveying nothing
        // forever (#206).
        Node::NodeRuntimeSources { .runtime = workerTier != nullptr ? &workerTier->Runtime() : nullptr,
                                   .capacity = workerTier != nullptr ? &workerTier->Capacity() : nullptr,
                                   .scheduler = ServiceOrNull(schedulerTier.get()),
                                   .enrollment = AddressWhen(servesEnrollment, enrollmentWindow),
                                   // `RunsConsensus`, not `servesEnrollment`: the committed
                                   // tombstone set exists wherever this node participates in
                                   // the cluster's state, which is a broader condition than
                                   // serving an enrollment window (that also wants a scheduler
                                   // tier). Asked of the ONE predicate rather than spelled as a
                                   // conjunction here, which is the rule this file already
                                   // carries for `servesEnrollment` two lines up.
                                   //
                                   // `NodeMembership` exists on every node; the committed SET
                                   // only exists where consensus runs, so this pointer is what
                                   // draws the distinction and a keyless node reports the field
                                   // ABSENT rather than `0` (#1471).
                                   .membership = AddressWhen(Node::RunsConsensus(cfg), membership),
                                   // The same predicate, for the same reason: a standing is a
                                   // claim about a configuration only a consensus node holds, so
                                   // a node running none reports the field ABSENT (#1449).
                                   .consensus = AddressWhen(Node::RunsConsensus(cfg), consensusStanding),
                                   // Every node has conditions, so this is never null here: a node
                                   // with nothing raised reports every row `clear` or
                                   // `not-evaluated`, never an empty list (#1364).
                                   .conditions = &conditions,
                                   // Every node has one; one that holds no roster reports the
                                   // field ABSENT through it (#178).
                                   .roster = nodeRoster.get() },
    };

    // The operator verbs. Declared BEFORE the surface that routes to it and therefore
    // destroyed after, like every other responder here.
    //
    // `membership.Oracle()` bound once, by reference, the way every surface binds it: an
    // implementation re-asking for an oracle per request could never see `--fleet-open`
    // change, and a test that re-acquired it would pass under exactly that defect.
    //
    // What live stats and `NodeMetrics` read, built against a SLOT, because what they read -- the
    // scrape provider, the fleet and the sampler -- is built after consensus, and consensus after
    // this surface: see `LiveStatsSourceSlot`. Declared before every responder reading it, so it is
    // destroyed after them.
    LiveStatsSourceSlot liveSources;
    Node::NodeStatusResponder nodeStatusResponder { nodeStatus, liveSources, membership.Oracle(), metrics };

    // The dashboard credential, read ONCE for the surfaces that guard the fleet with it: `/fleet`
    // over HTTP, and over `0xFC` the fleet subject of a live-stats subscription and `fleet-text`.
    auto dashboardOrRefusal = Node::LoadDashboardCredentialOrExplain(cfg);
    if (!dashboardOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", dashboardOrRefusal.error());
        return ExitUsage;
    }
    auto const dashboardCredential = std::move(*dashboardOrRefusal);

    // Live stats as a subscription rather than a poll (#1399), reading the slot declared above.
    // Declared before the surface, like every responder here, so it is destroyed after it.
    Node::LiveStatsResponder liveStatsResponder {
        liveSources, membership.Oracle(), dashboardCredential, nodeIo.Reactor(), metrics
    };

    // The fleet document read once (#1391): the same slot, and admitted by the same decision as
    // the fleet subject of a subscription.
    Node::FleetTextResponder fleetTextResponder { liveSources, membership.Oracle(), dashboardCredential, metrics };

    // The enrollment surface, built only where there is a cluster to be admitted to.
    //
    // An `optional` rather than a null pointer with a branch at the call site, because
    // the responder holds references and a node with no scheduler has none to give it.
    // `servesEnrollment` above is that condition, asked once and shared with what
    // `NodeStatus` reports -- so the two cannot disagree, which is what this comment
    // used to assert on the strength of `RunsConsensus` implying a scheduler tier while
    // the two sites spelled different expressions.
    //
    // `membership.Oracle()` bound once, by reference, exactly as every other surface
    // binds it. The credential is the SCHEDULER's -- `AUTH` routes there -- so this
    // surface holds the same policy object rather than a second one, or a node with a
    // token file would gate nine verbs and leave the tenth open.
    std::optional<Node::EnrollmentResponder> enrollmentResponder;
    if (servesEnrollment)
        enrollmentResponder.emplace(enrollmentWindow,
                                    schedulerTier->ServiceForSurfaces(),
                                    membership.Oracle(),
                                    metrics,
                                    logger,
                                    schedulerTier->Policy());

    // The identity prover (#178), built wherever this node runs CONSENSUS: a proof is judged
    // against the cluster's applied roster -- members, enrolled principals and revoked keys --
    // which only a consensus member holds, and it is asked of `membership`, whose `ExplainKey` is
    // the one door to that answer for the proof and for every later verb alike. A consensus node
    // always holds an identity key (`HoldsNodeKey`).
    //
    // The credential is the SCHEDULER's, for the reason the enrollment surface holds the same
    // object: `AUTH` is a `Session` verb and routes there, so a node with a token file must gate
    // these two verbs with it as well -- and `schedulerTier` may legitimately be null on a
    // consensus member that schedules nothing, which is what the conditional below reads.
    std::optional<Node::NodeProofResponder> nodeProofResponder;
    if (Node::RunsConsensus(cfg) && identityKey.has_value())
        nodeProofResponder.emplace(cfg.nodeId,
                                   *identityKey,
                                   membership,
                                   proofRandom,
                                   metrics,
                                   logger,
                                   schedulerTier != nullptr ? schedulerTier->Policy() : nullptr);

    auto nodeSurfaceOrRefusal = Node::StartNodeSurfaceOrExplain(
        nodeIo,
        cfg,
        // Designated, so the NAME travels with each pointer. Four bare
        // `IFrameResponder*` arguments are four a call site can silently transpose,
        // and a transposed pair routes every cache verb to the scheduler -- which
        // answers *served nowhere* for traffic this node is holding a tier for.
        Node::SurfaceComponents { .cache = cacheTier != nullptr ? &cacheTier->Responder() : nullptr,
                                  .scheduler = schedulerTier != nullptr ? &schedulerTier->Responder() : nullptr,
                                  // Absent on a node running no worker (#206): the router then
                                  // answers the compile family's refusal for a node without one.
                                  .compile = workerTier != nullptr ? &workerTier->Responder() : nullptr,
                                  .node = &nodeStatusResponder,
                                  .enrollment = AddressOrNull(enrollmentResponder),
                                  .live = &liveStatsResponder,
                                  .fleet = &fleetTextResponder,
                                  // Absent on a node running no consensus: the router then
                                  // answers the node-proof family `NoCluster`.
                                  .nodeProof = AddressOrNull(nodeProofResponder) },
        activated,
        metrics,
        logger,
        // Asked of the RUNNER rather than of a captured copy of the map, so a
        // re-survey that replaces the set is reflected in the very next line
        // (#238). The tier outlives the surface -- declared above it, destroyed
        // after -- which is what makes the pointer safe to hold.
        [worker = workerTier.get()](std::string_view fingerprint) {
            return worker != nullptr ? worker->CompilerFor(fingerprint) : std::string {};
        });
    if (!nodeSurfaceOrRefusal.has_value())
    {
        // No flag prefix: this can fail over --listen-node or over --serve-scheduler,
        // and only `StartNodeSurfaceOrExplain` knows which, so it names the flag.
        logger.Logf(LogLevel::Error, "{}; refusing to start", nodeSurfaceOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null, and for exactly one reason: a node with none of the
    // three components serves no 0xFC port, which has been logged. A bind FAILURE is
    // not that case -- this surface's row states `Refuse`, so it arrives as the error
    // above rather than as a null. See `BindFailurePolicy` in NodeSurfaces.hpp.
    auto const nodeSurface = std::move(*nodeSurfaceOrRefusal);

    // Says, once a minute and for as long as it is open, that this node is holding an
    // enrollment window.
    //
    // **Repeating rather than one line at open, and that is the whole of #1298's
    // observability half.** A single line scrolls away, and this is the one state in
    // which this machine will hand its cluster's key to a stranger that asked and was
    // approved. An operator who opened a window and was called away has the log and
    // `NodeStatus`, and only one of those reaches somebody who is not already looking.
    //
    // A thread of its own rather than a ride on the heartbeat, because the heartbeat
    // belongs to the WORKER and this belongs to the leader -- a scheduler holding no
    // worker is an ordinary deployment and would otherwise be the one node that never
    // said anything. It exists only where the surface does, so a node with no cluster
    // starts no thread.
    //
    // The DECISION is `TakeDueWarning`, which is pure over an injected clock and is
    // tested against a `core::platform::ManualClock`. The driver is `WarnWhileWindowIsOpen` above,
    // split out of this function exactly as `AnnounceOnce` was and for the same two
    // reasons -- it took `WorkerBody` past the cognitive-complexity ceiling the build
    // enforces, and a loop with a decision in it is more behaviour than belongs in the
    // one translation unit no test reaches.
    //
    // An `optional` rather than an unconditional `jthread` whose body returns at once,
    // which is what this was: that spelling STARTED a thread on every node in the fleet
    // and made the sentence above ("a node with no cluster starts no thread") false by
    // one word. The condition is `servesEnrollment`, so the thread and the surface
    // cannot disagree about whether there is a window to watch.
    std::optional<std::jthread> enrollmentWatch;
    if (servesEnrollment)
        enrollmentWatch.emplace([&](std::stop_token const& stop) { WarnWhileWindowIsOpen(enrollmentWindow, logger, stop); });

    // Consensus, when the operator configured a cluster -- which every scheduler has, even a
    // lone one (#178). It is what gives the scheduler tier a role at all: without it every
    // node in a fleet would believe it schedules, and two nodes handing out the same
    // machine's slots is the one thing the architecture says only one may do.
    //
    // Started AFTER the scheduler tier, because its observers push into it, and
    // declared after too, so it is destroyed first and cannot call into a tier that
    // has gone.
    auto consensusOrRefusal =
        Node::StartConsensusOrExplain(cfg,
                                      schedulerTier,
                                      nodeSurface != nullptr ? nodeSurface->BoundEndpoint() : std::string {},
                                      identityKey,
                                      membership,
                                      *nodeRoster,
                                      rosterWallClock,
                                      metrics,
                                      logger,
                                      &conditions);
    if (!consensusOrRefusal.has_value())
    {
        // No flag prefix here, for the reason the cache tier's line below has none:
        // consensus fails over --node-id, --raft-peer, --listen-raft or
        // --cluster-dir, and only the tier knows which, so its message names the
        // flag. It used to prefix `--node-id `, which rendered the peer refusal as
        // "--node-id --node-id=n1 names no --raft-peer" -- and that message is now
        // `StartupPolicyRejection`'s own row, which names the flag by construction.
        logger.Logf(LogLevel::Error, "{}; refusing to start", consensusOrRefusal.error());
        return ExitUsage;
    }
    // Null on a node with no `--listen-raft`: a pure worker, since #178 makes every scheduler a
    // consensus member.
    auto const consensusTier = std::move(*consensusOrRefusal);

    // Attached as soon as the tier exists and before anything serves, and detached before the
    // tier is destroyed: the attachment is declared after it. A null tier attaches nothing.
    auto const consensusStandingAttached = consensusStanding.Attach(consensusTier.get());

    // Discovery, when the operator configured it. Declared AFTER consensus and so
    // destroyed before it, because its observer pushes into the tier above: a
    // discovery loop outliving the thing it hands peers to is a dangling reference
    // that only fires while a node is shutting down.
    auto discoveryOrRefusal = Node::StartDiscoveryOrExplain(cfg, consensusTier, metrics, logger);
    if (!discoveryOrRefusal.has_value())
    {
        // Fatal; why is `RowFor(NodeSurface::Discovery).bindFailureReason` (#352).
        // Not restated here -- this line and that row would be the two places the
        // ticket is about.
        logger.Logf(LogLevel::Error, "--discovery {}; refusing to start", discoveryOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: no `--discovery` means the cluster is the
    // `--raft-peer` list an operator typed, which is the ordinary deployment.
    auto const discoveryTier = std::move(*discoveryOrRefusal);

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
    // Built by a function rather than spelled as a lambda here: a node with no cache
    // must report NO cache rather than an empty one, and that branch has to be
    // reachable from a test. Held in a local because the sampler and the scrape
    // surface both read it, and they must not disagree about the machine.
    //
    // The load source is the snapshot's own and holds no baseline, so the scrape, the
    // sampler and every live subscription read the same raw counters without taking an
    // interval from the heartbeat's sampler, which owns a separate one. No scratch root:
    // the free space a snapshot reports is `host`'s, on the same filesystem.
    auto const scrapeLoad = MakeSystemCounterSource();
    auto snapshotProvider = Node::MakeNodeSnapshotProvider(
        Node::NodeScrapeSources {
            .host = host.get(),
            .load = scrapeLoad.get(),
            .busySlots =
                [worker =
                     workerTier.get()] { return worker != nullptr ? worker->Capacity().InFlight() : std::size_t { 0 }; },
            .cordoned = [worker = workerTier.get()] { return worker != nullptr && worker->Capacity().IsCordoned(); },
            .cache = cacheTier.get(),
            // Zero and the scratch base on a node running no worker, until #1440
            // makes both absent on every surface.
            .slots = workerTier != nullptr ? workerTier->Slots() : 0,
            .scratchRoot = workerTier != nullptr ? workerTier->ScratchRoot() : Node::ScratchBaseDirectory(),
            // Empty when this node runs no consensus, which is what makes the
            // scrape say NO cluster rather than a cluster of nobody. The tier is
            // declared above, so it outlives the provider: locals are destroyed in
            // reverse.
            .consensus = Node::ConsensusScrapeSource(consensusTier.get()),
            // The roster's lapse (#178), declared above the provider for the tier's reason.
            .roster = nodeRoster.get() },
        startedAt);

    // Absent when this node runs no scheduler: there is then no registry to report,
    // so no fleet route is registered and `/fleet` is a plain 404.
    auto const fleetSources = schedulerTier
                                  ? std::optional { Distributed::FleetSources { .scheduler = &schedulerTier->Service(),
                                                                                // Null only while consensus is being
                                                                                // built: every scheduler runs it since
                                                                                // #178, so a node with a scheduler tier
                                                                                // has replicated state to read.
                                                                                .cluster = consensusTier.get(),
                                                                                .metrics = &metrics } }
                                  : std::nullopt;

    // EVERY node samples itself, whatever surfaces it serves. A pure worker runs no
    // dashboard and often no admin endpoint at all, and it is exactly the machine
    // doing the compiles -- so a sampler that existed only alongside a page would
    // leave the fleet's year with a hole where its busiest members should be.
    //
    // Declared AFTER the tiers it reads and BEFORE the surface that reads it, which
    // is what makes both sets of pointers safe: locals are destroyed in reverse.
    static core::platform::SystemWallClock const wall;
    Node::FleetSampler sampler { fleetSources, metrics, snapshotProvider, wall, Node::HistoryPaths::For(cfg), logger };

    // Wired here because this is the one scope holding both: the scheduler tier is
    // built before the sampler that receives for it, so the sink cannot be a
    // constructor argument on either.
    if (schedulerTier != nullptr)
        schedulerTier->SetHistorySink(&sampler.Received());

    // What a live-stats stream reads: the same provider, fleet and sampler the admin surface is
    // handed below, so a panel and `/metrics` cannot disagree about this machine. The attachment
    // is declared AFTER all of them and so detaches before the first of them is destroyed, on
    // every way out of this function -- including the surface outliving them, which it does.
    //
    // `endpoint` is the STARTUP value and deliberately not the seam (#1279). It is the
    // dashboard's source line -- where this process ANSWERS -- and the bind behind that cannot
    // move, `--listen-node` being unreloadable. So a node that re-advertises shows the address
    // it booted with in one cosmetic line, and the alternative is a third spelling of
    // `AdvertisedEndpoint` in the one translation unit no test can reach. The two sites that
    // DECIDE anything, the registration and the lease check, both follow the seam.
    Node::NodeLiveStatsSources const nodeLiveSources { Node::NodeLiveStatsParts { .metrics = &metrics,
                                                                                  .snapshot = snapshotProvider,
                                                                                  .identity = &nodeStatus,
                                                                                  .fleet = fleetSources,
                                                                                  .history = &sampler,
                                                                                  .endpoint = advertise,
                                                                                  // The same set the admin endpoint
                                                                                  // renders from, derived from one
                                                                                  // function so a subscriber and a
                                                                                  // scrape cannot disagree about
                                                                                  // which rows this node can write.
                                                                                  .surfaces =
                                                                                      Node::NodeServedSurfacesFor(cfg) } };
    auto const liveSourcesAttached = liveSources.Attach(nodeLiveSources);

    auto surfaceOrRefusal = Node::StartAdminSurfaceOrExplain(
        cfg, *host, metrics, std::move(snapshotProvider), fleetSources, &sampler, dashboardCredential, logger, conditions);

    // Fatal here and not in the daemon, which is a real difference between the two
    // binaries rather than a defect in either; the row says why
    // (`RowFor(NodeSurface::Admin).bindFailureReason`, #352).
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
    // Every component that will ever exist in this process has now been built, so every condition
    // row is answered -- by the component that runs it, or here, from its scope, for a component
    // this node does not run. BEFORE any surface serves, so no reader ever sees the `undecided` a
    // correct node never shows. A row still undecided is a component that runs and never said
    // anything: named at Error, and reported as `undecided` rather than dressed as a neighbour.
    for (auto const undecided: conditions.Settle(Node::PresentComponents { .worker = workerTier != nullptr,
                                                                           .scheduler = schedulerTier != nullptr,
                                                                           .adminSurface = adminSurface.endpoint != nullptr,
                                                                           .enrollment = servesEnrollment,
                                                                           .consensus = Node::RunsConsensus(cfg) }))
        logger.Logf(LogLevel::Error,
                    "condition {} was never evaluated although this node runs what evaluates it; every surface "
                    "reports it undecided",
                    Node::RowFor(undecided).id);

    // A node with neither surface enabled adopts nothing and starts no thread.
    nodeIo.Start();

    // Started only where there is a worker, and joined before the sampler it hands history
    // through is destroyed: the handle is declared after it.
    std::optional<Node::WorkerHeartbeat> heartbeat;
    if (workerTier != nullptr)
        heartbeat.emplace(workerTier->Launch(statusClock));

    // **Started unconditionally, and that one word is the whole of #1440.** A node with
    // `--slots=0` has no `workerTier`, so before this existed such a machine reached the fleet
    // through nothing at all -- absent from the Machines table it was itself serving, and
    // handing over no history across an election.
    //
    // Declared AFTER the sampler it hands history through and therefore destroyed before it.
    // That is the ordering rationale `WorkerHeartbeat` used to carry, and it moved here with
    // the history: this loop is now the only thing in the process that reads the sampler's
    // handover cursor.
    auto const presence = Node::NodePresence::Start(Node::NodePresenceParts { .cfg = cfg,
                                                                              .capacity = capacity,
                                                                              .announced = announced,
                                                                              .cacheTier = cacheTier.get(),
                                                                              .metrics = metrics,
                                                                              .sampler = sampler,
                                                                              .credential = credential,
                                                                              .logger = logger,
                                                                              .conditions = conditions,
                                                                              .roster = nodeRoster.get(),
                                                                              .prover = AddressOrNull(prover) });

    // Installed only once the listener is up and the heartbeat is running, so a
    // stop arriving during startup cannot close a listener that does not exist yet.
    InstallNodeStopHandlers();

    // The listening endpoint is described by where it CAME FROM, not by the
    // config. When a socket was adopted, `--bind` and `--port` were never used,
    // and printing them names an address this process is not listening on -- which
    // in the one line an operator reads to confirm a worker came up is worse than
    // printing nothing. What matters to a client is the advertised endpoint, and
    // that is reported either way.
    // The endpoint this node actually LISTENS on, which since #290 stage 3 is the one
    // 0xFC surface -- there is no second port to disambiguate and no config value that
    // describes a socket this process did not open.
    auto const listeningOn = Node::AdvertisedEndpoint(cfg);
    // The admission policy is part of this line, and that is #235's second half: a
    // worker given no policy starts, binds the wildcard, registers, is leased out
    // and refuses every dispatched compile -- and until this said so, the one line
    // an operator reads to confirm the worker came up reported nothing but health.
    // The scheduler tier prints the same phrase from the same function, so a node
    // running both says one thing rather than two.
    logger.Logf(LogLevel::Info,
                // The marker text is a row of `ReadinessMarkerTable`, never a literal here.
                // Four fixtures in two languages wait on these bytes and this build recompiles
                // none of them, so a reword breaks them by TIMEOUT rather than by a failed
                // build (#654). Referencing the row is what makes a rename a compile error.
                "{} on {}, advertising {}, {}, {}",
                ReadinessMarkerText(ReadinessMarker::CompileNode),
                listeningOn,
                advertise,
                // The toolchain count this node is BRINGING UP, not the count it is
                // serving: the survey runs on the heartbeat thread and has almost
                // certainly not finished when this prints (#365) -- and a ready line is a
                // statement about starting anyway.
                Node::WorkerReadinessPhrase(cfg,
                                            workerTier != nullptr ? std::optional { workerTier->Slots() } : std::nullopt,
                                            workerTier != nullptr ? workerTier->StartupToolchainCount() : 0),
                Node::AdmissionSummary(cfg));

    // **Main waits here now, and there is no accept loop to interrupt.** This was
    // `core::async::syncRun(server.Run())` -- the dedicated listener's accept loop, which blocked
    // this thread until a stop closed it, and which needed a watcher thread to notice
    // the stop at all. The merged surface's loops run on `nodeIo`'s own thread, so what
    // is left for main to do is wait for the stop and then drain.
    //
    // That deletes the watcher rather than rehoming it: it existed only because a
    // parked `accept()` cannot be woken by a flag.
    while (!DaemonControls::Instance().StopRequested())
    {
        // Taken here rather than in the handler, for the reason the handler's own note
        // gives. `TakeReloadRequest` clears the flag atomically, so a burst of SIGHUPs
        // costs one re-read rather than one per signal.
        //
        // Nothing is handed to the heartbeat thread: it compares snapshots itself, so
        // this call publishes a new configuration and says so, and the next beat picks
        // it up on its own.
        if (DaemonControls::Instance().TakeReloadRequest())
            Node::ApplyReloadRequest(reloader, membership, logger);
        std::this_thread::sleep_for(StopPollInterval);
    }
    logger.Logf(LogLevel::Info, "stop requested; no longer accepting compiles");

    // **Every compile drained HERE, while the node's reactor is still turning** -- see
    // `WorkerTier::StopAndDrain` for why destruction order cannot express it.
    if (workerTier != nullptr)
        workerTier->StopAndDrain();

    // Unwired BEFORE the sampler goes, and that ordering is the whole reason this
    // line exists: locals are destroyed in reverse declaration order, so the
    // scheduler tier -- declared before the sampler because the sampler reads its
    // registry -- outlives the store it routes to. A heartbeat arriving in that
    // window would file a machine's buckets through a pointer to a destroyed one.
    if (schedulerTier != nullptr)
        schedulerTier->SetHistorySink(nullptr);

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
    // A node that came up, served, and then found it had nothing to compile with
    // exits as the refusal it would have been before #365 -- late, but with the same
    // code and the same diagnostic. A supervisor that restarts on failure must not
    // read this as a clean stop.
    return workerTier != nullptr && workerTier->EndedInRefusal() ? ExitUsage : ExitOk;
}

/// What every early verb is handed.
///
/// One context rather than a parameter list per verb, because a shared signature is what
/// makes the table below a table: a verb needing a different one would be a verb that
/// could not be a row. `cliOnly` rides along for the service registration alone, which
/// bakes in the command line AS TYPED and must not see what the file supplied -- the
/// distinction the two parses at the top of `main` exist to draw.
struct EarlyVerbContext
{
    /// The merged configuration: the file, then the command line applied over it.
    ///
    /// NOT `const`, and that is a statement about what these verbs do rather than an
    /// oversight: the service registration MINTS this node's identity into it, because a
    /// registration replays its command line forever and one that omitted the identity
    /// would let a re-image answer to an identity the cluster never admitted. A `const`
    /// context would read as "the verbs only inspect the configuration", which is false
    /// for exactly the row where it matters most.
    NodeConfig& cfg;

    /// The command line alone, with no file applied.
    ///
    /// Not `const` either, and for a sharper reason than `cfg`: `AdoptNodeIdentity`
    /// applies the resolved identity to BOTH, and this is the copy `MakeNodeServiceSpec`
    /// bakes into the registration. A registration built from an un-stamped `cliOnly`
    /// would replay forever with no `--node-id` at all, which is the defect the minting
    /// exists to prevent rather than a lost optimisation.
    NodeConfig& cliOnly;

    /// Where a refusal goes. Still the CONSOLE logger, because every verb here answers
    /// an operator at a terminal; the switch to an event log happens below them all.
    ILogger& logger;
};

/// One verb that answers an operator and exits, ahead of the startup table.
struct EarlyVerbRow
{
    /// Whether this verb is what was asked for. Reads the parsed configuration and
    /// nothing else, so choosing a verb costs no I/O and cannot fail.
    bool (*applies)(NodeConfig const& cfg);

    /// The verb. What it returns is what `main` returns.
    int (*run)(EarlyVerbContext const& context);
};

/// Print the resolved surface map, and judge it (`--print-surfaces`).
/// @param context The configuration and the console logger.
/// @return `ExitOk` when the configuration would start, `ExitUsage` when it would not.
[[nodiscard]] int RunPrintSurfaces(EarlyVerbContext const& context)
{
    // **The ordering is right; the exit code was the defect** (#582). This printed the
    // map and returned 0 for a configuration the node then refuses to run -- measured:
    // the documented scheduler line without the pre-shared key it then needed exited 2 on
    // its own and 0 through this flag. Since the flag prints the RESOLVED configuration, an
    // operator reaches for it before writing a unit file, and it answered "fine" to a
    // command line with no chance of starting.
    //
    // Printing the map for a broken configuration is the feature; reporting SUCCESS for
    // it is not. `ReportSurfaces` renders and judges in one call so this site decides
    // nothing: the verdict is `StartupPolicyRejection`'s own sentence, and the exit code
    // follows from whether there is one. See that function for why it is a function
    // rather than four lines here -- the verbs in `EarlyVerbs` answer and exit ahead of
    // the startup table, and they do not all want the same answer.
    auto const report = ReportSurfaces(context.cfg);
    std::cout << report.text;
    if (!report.refusal.has_value())
        return ExitOk;
    context.logger.Logf(LogLevel::Error, "{}", *report.refusal);
    return ExitUsage;
}

/// Print this node's identity, minting what the state directory does not hold yet
/// (`--print-identity`, #178).
///
/// **Minting is the point, not a side effect**, which is what separates this verb from every
/// other one in `EarlyVerbs`: a cluster's members each need every other member's key on their
/// `--raft-peer` before any of them starts, and the key does not exist until something mints
/// it. The start that follows reads the same files back as `Recorded`. Through the resolvers
/// the start uses, so an id or a key this prints is the one that start will run as -- and a
/// key file that is there and cannot be used is refused here exactly as it is there.
/// @param context The configuration and the console logger.
/// @return `ExitOk` once printed; `ExitUsage` for a node with nowhere to keep an identity, or
///         one whose identity could not be read or minted.
[[nodiscard]] int RunPrintIdentity(EarlyVerbContext const& context)
{
    auto& cfg = context.cfg;
    if (!Node::HoldsNodeKey(cfg))
    {
        context.logger.Logf(LogLevel::Error, "{}", Node::PrintIdentityNeedsStateDirectory);
        return ExitUsage;
    }

    SystemSecureRandom random;
    auto key = Node::ResolveNodeKeyFor(cfg, random);
    if (!key.has_value() || !key->has_value())
    {
        context.logger.Logf(LogLevel::Error,
                            "{}",
                            key.has_value() ? std::string { Node::PrintIdentityNeedsStateDirectory } : key.error().message);
        return ExitUsage;
    }
    auto const publicKey = (*key)->pair.PublicKey();

    // The id travels with the key (#178): a consensus member is admitted under it, and a worker
    // with a `--cluster-dir` proves it on every connection to a scheduler -- and is admitted under
    // it by `--cluster-admit-worker=<id>@<key>`, typed from these very lines.
    auto identity = Node::ResolveNodeIdentity(Node::NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!identity.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", identity.error());
        return ExitUsage;
    }
    identity->publicKey = publicKey;
    Node::ApplyNodeIdentity(cfg, *identity);

    auto dialAddress = std::optional<std::string> {};
    if (RunsConsensus(cfg))
        if (auto const dial = ConsensusDialAddressOf(cfg); dial.has_value())
            dialAddress = *dial;

    std::cout << Node::DescribeIdentity(
        cfg.nodeId, publicKey, dialAddress, RunsConsensus(cfg) ? Node::IdentityRole::Member : Node::IdentityRole::Worker);
    return ExitOk;
}

/// Write the packaged configuration template to this binary's own system path
/// (`--seed-config`).
///
/// The WORKER seeds its own file. `fastcached --seed-config` derives its destination
/// from `DaemonApplicationName`, so it can only ever write the daemon's -- which is why
/// the MSI shipped no worker configuration and the .pkg shipped none either (#397).
/// Deriving it here from this binary's own application name is what makes the seeded
/// path and the path the startup lookup walks one answer rather than two that agree
/// until somebody edits one.
/// @param context The configuration and the console logger.
/// @return `ExitOk` once the file is in place, `ExitUsage` when it could not be written.
[[nodiscard]] int RunSeedConfig(EarlyVerbContext const& context)
{
    auto const seeded =
        SystemConfigPath(SystemConfigPathProbe {}, NodeApplicationName).and_then([&context](auto const& destination) {
            return SeedConfigFile(context.cfg.seedConfigTemplate, destination, DirectoryPolicy::AdministratorsOnly);
        });
    if (!seeded.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", seeded.error().ToString());
        return ExitUsage;
    }
    context.logger.Logf(LogLevel::Info, "{}", SeedOutcomeSentence(*seeded, context.cfg.seedConfigTemplate));
    return ExitOk;
}

/// Register or remove this worker's service entry (`--install-service`,
/// `--uninstall-service`).
/// @param context The merged configuration, the command line alone, and the logger.
/// @return The registration's own exit code, or `ExitUsage` when it was refused.
[[nodiscard]] int RunServiceRegistration(EarlyVerbContext const& context)
{
    // Only an install has to be viable; an uninstall merely names a registration to
    // remove, and refusing to remove one because it was misconfigured is how a bad
    // registration becomes permanent.
    //
    // Judged on the MERGED configuration, and registered from the command line alone --
    // which is not a contradiction. What the service will run with is the file plus
    // these arguments, so judging the command line alone would refuse the documented
    // setup, where `--scheduler` and the toolchains come out of the packaged file. What
    // is baked in is still only what was typed, plus the `--config` path that supplies
    // the rest.
    if (context.cfg.installService)
        if (auto const rejection = NodeInstallRejection(context.cfg))
        {
            context.logger.Logf(LogLevel::Error, "{}", *rejection);
            return ExitUsage;
        }

    // The identity the registration will bake in, resolved here because a registration
    // replays its command line forever: one that omitted it would let a re-image answer
    // to an identity the cluster never admitted, with both machines up throughout. AFTER
    // `NodeInstallRejection`, so a command line this install is about to refuse leaves
    // no state directory behind, exactly as the start path resolves after its own table.
    //
    // An uninstall reaches neither -- `NodeIdentityNeed` declines it -- because removing
    // a registration is the recovery an operator reaches for when the configuration is
    // already wrong.
    //
    // No KEY is minted here (#178), and deliberately: a registration carries no key, the
    // service mints its own at its first start -- as the account it RUNS as, which is not
    // necessarily the one installing it, and a key file this installer owned would be one
    // that account might never be able to read.
    SystemSecureRandom identityRandom;
    if (auto const adopted = AdoptNodeIdentity(context.cfg, context.cliOnly, identityRandom, context.logger, std::nullopt);
        !adopted.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}; refusing to install", adopted.error());
        return ExitUsage;
    }

    // `cliOnly`, never `cfg`: a registration replays its arguments at every start, so
    // baking in what the FILE said would freeze one reading of that file into launch
    // arguments that then outrank the file itself -- the operator edits it, restarts the
    // service, and nothing changes. What the registration does carry is the `--config`
    // path, so the service reads the current file at every start rather than a snapshot
    // of it.
    auto const spec = MakeNodeServiceSpec(CurrentExecutablePath(), context.cliOnly);
    auto const result = context.cfg.installService ? InstallService(spec, context.cfg.serviceScope)
                                                   : UninstallService(spec, context.cfg.serviceScope);
    if (result.exitCode == 0)
        std::cout << "fastcache-compile-node: " << result.message << '\n';
    else
        std::cerr << "fastcache-compile-node: " << result.message << '\n';
    return result.exitCode;
}

/// Convert this node's disk tier to the format this build reads (`--migrate-cache`).
/// @param context The configuration and the console logger.
/// @return 0 once converted, `ExitUsage` when the store could not be.
[[nodiscard]] int RunMigrateCache(EarlyVerbContext const& context)
{
    auto const outcome = MigrateDiskTier(context.cfg);
    if (!outcome.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", outcome.error());
        return ExitUsage;
    }
    std::cout << "fastcache-compile-node: " << *outcome << '\n';
    return 0;
}

/// Put a cluster question to a running cluster and report the answer (`--cluster-*`).
///
/// No reloader exists at this point and none ever will on this path: a cluster verb
/// answers and the process returns. The seam is threaded through anyway, so the day one
/// of these is asked from a running worker it reads the live secret rather than the one
/// this process was started with.
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunClusterVerb(EarlyVerbContext const& context)
{
    Node::ConfiguredCredential const credential { context.cfg, nullptr };
    return ReportOneShotVerb(RunClusterAdmin(context.cfg, context.cfg.cluster, credential));
}

/// Cordon this machine's own worker, or lift its cordon (`--cordon`, `--uncordon`).
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunCordonVerb(EarlyVerbContext const& context)
{
    Node::ConfiguredCredential const credential { context.cfg, nullptr };
    return ReportOneShotVerb(Node::RunCordonAdmin(context.cfg, context.cfg.cordon, credential));
}

/// Decide who may join, on behalf of an operator (`--enroll-*`).
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunEnrollVerb(EarlyVerbContext const& context)
{
    Node::ConfiguredCredential const credential { context.cfg, nullptr };
    return ReportOneShotVerb(Node::RunEnrollAdmin(context.cfg, context.cfg.enroll, credential));
}

/// Ask another cluster to let this machine in (`--enroll-from`).
/// @param context The configuration.
/// @return What the exchange answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunEnrollFrom(EarlyVerbContext const& context)
{
    SystemSecureRandom enrollRandom;
    Node::ConfiguredCredential const credential { context.cfg, nullptr };
    return ReportOneShotVerb(Node::RunEnrollClient(context.cfg, credential, enrollRandom), "fastcache-compile-node: ");
}

/// The verbs that answer an operator and exit, in the order they are asked.
///
/// A plain array rather than an `EnumTable`, and the reason is that nothing indexes
/// this: the ORDER is the content. There is no enumerator to be in the order of, so
/// `RowsInEnumeratorOrder` would have nothing to check, and a trailing `Last` would be a
/// sentinel for a sequence that is keyed by nothing.
///
/// Each row carries the reason it sits where it does. That ordering used to be expressed
/// by the LAYOUT of seven `if` blocks in `main` -- true, readable only by scrolling, and
/// with nothing that made reordering them show up as a change to the thing being ordered.
constexpr std::array<EarlyVerbRow, 9> EarlyVerbs { {
    // **Below the logger and still above the startup table** (#582). Its refusal has to
    // reach the same terminal a start prints to, rendered the same way -- an operator
    // who meets that sentence here and again at boot should be reading one message, not
    // matching two. Ahead of `StartupPolicyRejection`, so the worksheet is printed for a
    // broken configuration: an operator reaches for this BECAUSE a port is wrong, and
    // withholding the map until the configuration is valid would withhold it exactly
    // when it is wanted. It opens nothing, so there is no state to protect.
    { .applies = [](NodeConfig const& cfg) { return cfg.printSurfaces; }, .run = &RunPrintSurfaces },

    // Beside the worksheet and for its reason: it is reached for while a cluster is being
    // PUT TOGETHER, before any member's command line is complete -- each needs the others'
    // keys -- so gating it on the startup rules would withhold it exactly when it is wanted.
    // Unlike the worksheet it writes, and deliberately: see `RunPrintIdentity`.
    { .applies = [](NodeConfig const& cfg) { return cfg.printIdentity; }, .run = &RunPrintIdentity },

    // Seeding, ahead of the startup table for `--print-surfaces`' reason: it is an
    // INSTALLER step, run before this machine has a working configuration at all, so
    // refusing it until the configuration is already valid would make it unusable at the
    // only moment it is wanted.
    { .applies = [](NodeConfig const& cfg) { return !cfg.seedConfigTemplate.empty(); }, .run = &RunSeedConfig },

    // Service registration, before anything that costs time. A misconfiguration is
    // decided in microseconds while a toolchain fingerprint takes seconds, which is the
    // same cheap-and-fallible-first ordering the socket-activation check follows.
    //
    // The command line is registered as typed. Every other flag alongside
    // `--install-service` is baked in and reused at every start, so a registration that
    // cannot work must fail here, where an operator is watching, rather than at every
    // boot where nobody is. That is why the gate is `NodeInstallRejection` and not
    // `NodeServiceRejection`: an install has to satisfy the STARTUP rules as well, since
    // this returns before they are ever reached and every one of them is decided by the
    // command line being baked in.
    { .applies = [](NodeConfig const& cfg) { return cfg.installService || cfg.uninstallService; },
      .run = &RunServiceRegistration },

    // Converting the store acts on the files and exits. After the service row for the
    // same reason that one is early -- it costs microseconds to decide -- and before
    // everything below it, because a node whose store is of the wrong vintage cannot
    // start at all: making the operator satisfy `--scheduler` or a toolchain probe first
    // would be demanding they fix a running configuration before being allowed to fix
    // the store that stops it running.
    { .applies = [](NodeConfig const& cfg) { return cfg.migrateCache; }, .run = &RunMigrateCache },

    // A question asked OF a running cluster, rather than a worker starting up. After the
    // service row, because an installation is about this machine and this is about
    // somebody else's; before the `--scheduler` and `--toolchain` checks, because a
    // cluster command needs the first and not the second.
    { .applies = [](NodeConfig const& cfg) { return cfg.cluster.action != ClusterAction::None; }, .run = &RunClusterVerb },

    // An operator taking THIS machine out of the fleet or putting it back (#1303). Beside
    // the cluster row because it is the same kind of thing -- a question put to a running
    // node by a person at a terminal -- and before the startup table for that row's reason:
    // it serves nothing, so the rules for a configuration this node would SERVE with do not
    // apply. What it needs, a `--listen-node` to dial, it refuses by name itself.
    { .applies = [](NodeConfig const& cfg) { return cfg.cordon != CordonCommand::None; }, .run = &RunCordonVerb },

    // An operator deciding who may join, rather than a worker starting up. Beside the
    // cluster row because it is the same kind of thing -- a question put to a running
    // cluster by a person at a terminal, answered, and the process exits.
    { .applies = [](NodeConfig const& cfg) { return cfg.enroll.action != EnrollAction::None; }, .run = &RunEnrollVerb },

    // A machine asking to be LET IN to somebody else's cluster. Beside the cluster row
    // and after it, because the two are the same kind of thing from opposite ends --
    // that one is an operator administering a cluster they are already in, this one is a
    // machine that is not in one yet.
    //
    // Before the startup table below, and that is the point rather than an accident:
    // those rules judge a configuration this node will SERVE with, and this one serves
    // nothing at all. A node enrolling has no --scheduler and no toolchain, and being
    // refused for either would refuse exactly the fresh install the mode exists for.
    // What this mode itself requires -- somewhere to write the key, and a consensus
    // identity to be admitted AS -- it refuses by name itself, which is the shape
    // `RunClusterAdmin` already uses for its own `--scheduler`.
    { .applies = [](NodeConfig const& cfg) { return !cfg.enrollFrom.empty(); }, .run = &RunEnrollFrom },
} };

} // namespace

// **`main` scores 37 against a threshold of 60, and NOTHING ENFORCES THAT MARGIN.**
//
// Recorded rather than guarded, which is the honest half of #1351 and is stated here so
// nobody reads it as a guarantee. `readability-function-cognitive-complexity.Threshold`
// in `.clang-tidy` is GLOBAL: there is no per-function budget to express this in, and
// lowering the global number to enforce it would redden every function sitting between
// the new number and 60. So this figure is checked by nobody and will decay exactly the
// way the 60 it replaced did -- incrementally, with every contributor seeing a passing
// build. A comment claiming otherwise would be worse than no comment, because it would
// retire the suspicion.
//
// Measured 2026-09-13 on `b5ff67f1`, with `clang-tidy-22` at apt.llvm.org snapshot
// `1:22.1.8~++20260714014902+ca7933e47d3a` -- the build, not just the major, because two
// snapshots a month apart print the same `--version`. Re-derive it in one command
// against any build directory holding a compile database. The indented lines are ONE
// command, wrapped for width:
//
//     clang-tidy-22 -p <build-dir> --quiet
//       --config="{Checks: '-*,readability-function-cognitive-complexity',
//                  CheckOptions: [{key: readability-function-cognitive-complexity.Threshold,
//                                  value: '1'}]}"
//       src/apps/fastcache-compile-node/main.cpp
//
// Wrapped WITHOUT trailing backslashes, deliberately. A `//` line ending in one is a
// LINE SPLICE: the next line is swallowed into the comment, and GCC refuses the file
// under `-Wcomment` with `-Werror` while clang's leg says nothing at all. Measured --
// this exact block failed `gate-gcc-release` that way, during dependency scanning, so
// the error named a phase rather than a line anyone was looking at.
//
// A threshold of 1 makes every function report its own score; at the real threshold of
// 60 this file reports nothing at all.
//
// The check fires on EXCEEDING the threshold, which is measured here rather than
// inferred from its documentation: `WorkerBody` scores 59, and it reports at a threshold
// of 58 and stays silent at 59. So 60 passes and 61 fails, and the margin below is 23
// ordinary edits' worth at the 1-to-3 points an added conditional typically costs.
//
// **`WorkerBody` is at 59 and is the next instance.** This change did not buy `main`'s
// headroom out of it: it measured 59 before and 59 after.
int main(int argc, char** argv)
{
    std::span<char const* const> const argvSpan { const_cast<char const* const*>(argv), static_cast<std::size_t>(argc) };

    // Parsed TWICE, into two results, and which one a decision reads is the whole
    // of this arrangement.
    //
    // `cliOnly` holds nothing but the command line. It answers the two questions a
    // configuration file must not be able to answer: WHICH file to read, and what
    // gets baked into a service registration -- `--install-service` registers the
    // command line as typed, so a setting the file supplied must not be copied into
    // launch arguments that then outrank the file forever.
    //
    // `cfg` is the file applied first and the command line applied over it, through
    // the SAME appliers in that order. "The command line wins" is therefore the
    // order two loops run in rather than a per-field merge with a per-field
    // explicit bit -- which is the shape the daemon has, and which has shipped a
    // flag that parsed but never merged four times.
    NodeConfig cliOnly;
    auto const flow = ParseOptionsInto(NodeOptions(), argvSpan.subspan(1), cliOnly);
    if (!flow.has_value())
    {
        // The FIELD as well as the reason. Without it an unrecognised argument reads
        // as `unrecognised argument` and names nothing at all -- the daemon and the
        // test client have always printed both, and this is the binary whose flags
        // an operator is most likely to be typing by hand.
        std::cerr << "fastcache-compile-node: " << flow.error().field << ": " << flow.error().context << '\n';

        // Said only where it explains something. A non-ASCII argument does not reach
        // a Windows process as UTF-8 unless its active code page is UTF-8, which
        // every executable here asks for by manifest -- so this line appears only on
        // a host too old to honour that, where it is the whole answer and no other
        // surface would ever give it (issue #155).
        //
        // The optional is tested rather than `NarrowTextIsUtf8()`, which implies but
        // does not state that there is a code page to print: an inference a reader
        // has to make is one clang-tidy makes differently.
        if (auto const codePage = ActiveCodePage(); codePage.has_value() && *codePage != Utf8CodePage)
            std::cerr << "fastcache-compile-node: this host's active code page is " << *codePage
                      << ", not UTF-8, so a non-ASCII argument does not reach this process as the bytes you typed\n";
        return ExitUsage;
    }

    if (cliOnly.help)
    {
        // The color decision is made here rather than inside the help renderer so
        // that module stays free of ambient probes -- and on Windows the call also
        // enables virtual-terminal processing, so it must precede any output.
        std::cout << HelpText(StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain);
        return 0;
    }
    if (cliOnly.version)
    {
        std::cout << "fastcache-compile-node " << FASTCACHE_NODE_VERSION << '\n';
        return 0;
    }

    // Both of the above answer without reading anything, deliberately: a file this
    // build cannot parse must not be able to stop `--help` from explaining the flag
    // that would name a different one.

    // A leftover from when this worker was configured by a bag of arguments in an
    // EnvironmentFile. The unit no longer reads it, so a value left behind would
    // silently stop taking effect -- an operator's settings disappearing at an
    // upgrade with nothing anywhere saying why. Said once, at every start, because
    // the remedy is to delete the file and there is nothing else to report.
    if (ReadEnvironmentVariable("FASTCACHE_NODE_ARGS").has_value())
        std::cerr << "fastcache-compile-node: FASTCACHE_NODE_ARGS is set and is no longer read; this worker is "
                     "configured by --config=<file> (see /etc/fastcached/fastcache-compile-node.yaml). Delete "
                     "the leftover /etc/fastcached/compile-node.env and put those settings in the YAML file.\n";

    // The file the operator named, or else whichever platform default is actually
    // there. `EffectiveConfigPath` owns that rule -- a named path is strict and a
    // discovered one is skipped when it is absent, unreadable or untrusted -- and
    // it is the same rule and the same code the daemon's lookup uses.
    auto const lookup = EffectiveConfigPath(cliOnly.configPath, SystemConfigPathProbe {}, NodeApplicationName);

    // A file that is there and readable and was passed over anyway has to say so.
    // Silence would leave an operator editing a file this worker has quietly
    // decided not to obey, over a permission problem only they can fix.
    for (auto const& [rejectedPath, reason]: lookup.rejected)
        std::cerr << "fastcache-compile-node: " << rejectedPath.string() << ": " << reason << '\n';

    // A file this run cannot use is fatal -- with ONE exception, and it is the same
    // one the daemon carves out and for the same reason. `--uninstall-service`
    // names a registration to remove and reads nothing out of the file; refusing to
    // run it because the file at the default location has a typo blocks the very
    // recovery an operator reached for, and the registration being removed is
    // frequently the thing that put the bad file there.
    //
    // Narrower than the daemon's carve-out, deliberately. `--install-service` is
    // judged against the merged configuration, `--print-surfaces` prints it, and a
    // cluster verb dials an endpoint that may come from it -- so for those three, a
    // file that did not load would produce a confident answer about a configuration
    // this process never assembled.
    //
    // A path the operator NAMED is fatal even then: they are owed the news that the
    // file they typed did not arrive.
    auto const fileIsAdvisory = cliOnly.uninstallService && cliOnly.configPath.empty();

    NodeConfig cfg;
    bool fileApplied = false;
    if (!lookup.path.empty())
    {
        auto const loaded =
            ReadYamlSettings(lookup.path).and_then([&cfg, &lookup, argvSpan](std::vector<YamlSetting> const& settings) {
                return ApplyNodeConfiguration(settings, lookup.path, argvSpan.subspan(1), cfg);
            });
        if (loaded.has_value())
            fileApplied = true;
        else if (!fileIsAdvisory)
        {
            std::cerr << "fastcache-compile-node: " << loaded.error().ToString() << '\n';
            return ExitUsage;
        }
        else
            std::cerr << "fastcache-compile-node: ignoring " << lookup.path.string() << ": " << loaded.error().ToString()
                      << '\n';
    }

    if (!fileApplied)
        // Assigned rather than left as it is: a file that failed halfway leaves
        // `cfg` holding part of a document this run has decided not to obey, and
        // "some of the settings, up to the bad line" is a configuration nobody
        // wrote. Declining a file means the command line stands alone -- which is
        // also the ordinary no-file case, where `cliOnly` is already the answer.
        cfg = cliOnly;

    // The admin verbs below answer an operator at a terminal, so they report to one
    // -- even under `--daemon`, which the registered command line carries and which
    // is therefore exactly what somebody copies out of `sc qc` to try by hand. Their
    // refusals going to the event log while the terminal showed only an exit code
    // would be the same defect this file is fixing, pointed the other way.
    // Built by the factory, never here: the third argument used to be absent at this
    // very line, so the node could not emit a time and no flag existed to ask for one
    // (#485). Omitting it no longer compiles -- `ConsoleLogger` takes `LogTimestamps`
    // with no default -- and the factory is what carries the CONFIGURATION to it,
    // which is the half a signature cannot check.
    auto const consoleLogger = MakeNodeConsoleLogger(std::cerr, cfg);

    // The verbs that answer an operator and exit, asked in one place.
    //
    // Each of these was an `if` block returning from `main`, and together they were 25
    // of this function's 60 cognitive-complexity points against a threshold of 60 --
    // because a test nested one level inside another costs TWICE what the same test
    // costs at the top level, so the error handling inside each verb was charged at the
    // nested rate (#1351). The same seven verbs cost three points as a walk.
    //
    // The order is `EarlyVerbs`' order, and each row carries the reason it sits where it
    // does.
    EarlyVerbContext const verbContext { .cfg = cfg, .cliOnly = cliOnly, .logger = *consoleLogger };
    for (auto const& verb: EarlyVerbs)
        if (verb.applies(cfg))
            return verb.run(verbContext);

    // NOW the sink is chosen. Everything from here is what a RUNNING service reports
    // -- every startup refusal below, the toolchain survey, and the loop itself --
    // and a service has no console for any of it to land on (#179). Everything above
    // was an operator at a terminal, and stays there.
    //
    // The factory answers nullptr wherever there is no event log, which is what keeps
    // this one expression rather than a platform branch, and `cfg.daemon` rather than
    // "am I on Windows" is what keeps a foreground run pointed at its terminal on a
    // machine that has one.
    auto const eventLogger = cfg.daemon ? MakeWindowsEventLogger(cfg.serviceName, cfg.logLevel) : nullptr;
    ILogger& logger = eventLogger ? static_cast<ILogger&>(*eventLogger) : static_cast<ILogger&>(*consoleLogger);

    // Its OWN version, first thing, the way `fastcached` already does
    // ("fastcached {} starting"). A manually bundled install is built once,
    // deployed once, and then runs indefinitely with no further contact with what
    // is current -- and the persisted disk tier makes that worse rather than
    // better, because a stale object does not expire on its own. Without this line
    // "how old is this install" is answerable only by cross-referencing file
    // timestamps against git history by hand (#181).
    //
    // A LINE OF ITS OWN, deliberately, and not a field on the readiness line. That
    // line is a wire contract: four fixtures in two languages match its bytes, and
    // one of them `sed`s a field out of it by position. Adding a field there would
    // break them by TIMEOUT rather than by a failed build, which is the failure
    // mode #654 records about that exact string.
    logger.Logf(LogLevel::Info, "fastcache-compile-node {} starting", VersionString);

    // An empty `--scheduler` and "no --toolchain and no discovery" were both refused
    // HERE, as inline `if`s, and both are now `StartupPolicyRejection` rows -- the
    // toolchain pair by #403, the scheduler by #386. Neither reads anything but the
    // parsed configuration, which is what that table is for, and a rule in this tier
    // is one that a RELOAD cannot consult and that no test can reach: this file is in
    // no test target, so the copy that ran at startup was the untested one while the
    // tests asserted `NodeServiceRejection`'s install-time twin. They had already
    // drifted on the sentence they say.
    //
    // Nothing replaces them here. The table is consulted a few lines below, on the
    // same logger and with the same exit code, and there is no `return` in between --
    // so this is a deletion rather than a move of the refusal's timing.

    // Checked here rather than inside WorkerBody, for the reason the two above are:
    // the POSIX host has already redirected stdout to /dev/null by the time the body
    // runs, so a diagnosis printed there goes nowhere in the one deployment where a
    // scheduler is most likely to be misconfigured.
    if (auto const rejection = StartupPolicyRejection(cfg))
    {
        logger.Logf(LogLevel::Error, "{}", *rejection);
        return ExitUsage;
    }

    // **This node's identity, resolved once and applied to every configuration this
    // process builds** (#1024). It is minted into `--cluster-dir` on the first start
    // and read back on every one after, so a fleet is built without inventing a name
    // per machine and typing it twice.
    //
    // **AFTER the table that judges this command line, and that is not a preference.**
    // Resolving MINTS: it creates a directory and writes a file. A configuration the
    // node is about to refuse must not leave one behind -- and worse, an unwritable
    // `--cluster-dir` would answer with "cannot create ..." in place of the
    // configuration error that is the operator's actual problem. Which is also what
    // keeps the startup rules pure functions of the command line, since they never see
    // an identity: the self-peer row asks `--raft-self` rather than looking for the
    // member `ApplyNodeIdentity` synthesises.
    //
    // The install path resolves at its OWN site, after its own table, for the same
    // reason and it cannot share this one: `--install-service` returns long before
    // here, and its refusals are `NodeInstallRejection`'s rather than these.
    //
    // **The identity KEY first** (#178), so the one `ApplyNodeIdentity` puts the id, this
    // node's own member entry and its key into the configuration together -- and every
    // reload candidate after it. Read, or minted into the state directory when there is
    // none there; a key file that is there and cannot be used is a refusal, never a re-mint.
    // A node with no state directory holds none, and says so.
    SystemSecureRandom identityRandom;
    auto const identityKey = AdoptNodeKey(cfg, identityRandom, logger);
    if (!identityKey.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", identityKey.error());
        return ExitUsage;
    }
    auto const publicKey = PublicHalf(*identityKey);

    auto const identity = AdoptNodeIdentity(cfg, cliOnly, identityRandom, logger, publicKey);
    if (!identity.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", identity.error());
        return ExitUsage;
    }

    // An `@<key>` this node's own `--raft-peer` states is a claim about the machine it is
    // typed on, so one that is not the key it holds is refused rather than announced.
    if (auto const contradiction = Node::SelfKeyContradiction(cfg, publicKey); contradiction.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", *contradiction);
        return ExitUsage;
    }

    // Built only when there IS a file, and holding the SAME argv the startup parse
    // used -- so a reload reproduces the startup order exactly: a fresh configuration,
    // the file applied through the appliers argv reaches, then the command line second.
    // "The command line wins" therefore stays a question of which loop ran last, never
    // a per-field merge with per-field presence bits.
    //
    // **Declared before the secret-exposure watch below and before the daemon host**,
    // which is what makes both safe. The watch subscribes to this object and its
    // report closure refers to `logger`, declared further up, so the reloader is
    // destroyed first -- the ordering `WatchSecretExposure` requires. And the host is
    // chosen afterwards because the POSIX one redirects stdout to /dev/null inside
    // `Run()`: every startup warning has to be emitted before that.
    std::optional<NodeReloader> reloader;
    if (!lookup.path.empty())
        reloader.emplace(
            cfg,
            lookup.path,
            [argvSpan, identity = identity->value_or(Node::NodeIdentity {})](
                std::filesystem::path const& path) -> std::expected<NodeConfig, ConfigError> {
                // A FRESH configuration, never the live one. A file that fails halfway
                // is then discarded whole rather than leaving the running worker
                // holding part of a document nobody wrote.
                NodeConfig candidate;
                auto const loaded =
                    ReadYamlSettings(path).and_then([&candidate, &path, argvSpan](std::vector<YamlSetting> const& settings) {
                        return ApplyNodeConfiguration(settings, path, argvSpan.subspan(1), candidate);
                    });
                if (!loaded.has_value())
                    return std::unexpected(loaded.error());
                // The identity again, through the same function the start used. A
                // candidate rebuilt without it holds an empty `--node-id`, which is an
                // unreloadable field that has CHANGED -- so every reload would be
                // refused by name, on a worker whose configuration was perfectly
                // valid. Resolved once at startup and applied here, never re-resolved:
                // this runs on a signal, and a reload must not be able to mint.
                Node::ApplyNodeIdentity(candidate, identity);
                return candidate;
            },
            &ValidateNodeReloadable);

    // A secret is only as private as the file holding it, and this worker has one per
    // row of `NodeSecretFileTable()` beside its configuration file, where the daemon has
    // one. #384 landed that rule and wired only the daemon; the exposure here is
    // identical for `--requirepass` out of a configuration file, and WIDER for the
    // secrets reached by path -- a world-readable identity key is this machine's signature on
    // every proof it makes, handed to every account on the machine
    // ([#752](https://github.com/LASTRADA-Software/fastcached/issues/752)).
    //
    // **And re-asked at every accepted reload**, which is the half a snapshot cannot
    // answer: a file's MODE is in no configuration, so an operator who loosens a key
    // file an hour after this worker started was met with silence for
    // the rest of the process's life
    // ([#868](https://github.com/LASTRADA-Software/fastcached/issues/868)).
    //
    // `lookup.path` UNGUARDED by `fileApplied`, and never `cfg.configPath`. The
    // resolved path is what was opened, and a discovered machine-wide file is named by
    // no flag at all -- so the flag's value would answer "no file" for the deployment
    // the packaging ships. The `fileApplied` guard it used to carry is DROPPED because
    // it can never decide anything, and that is the whole claim -- MEASURED against the
    // control flow above rather than reasoned from what a reload might do. A non-empty
    // `lookup.path` that would not load already exited `ExitUsage` far above, except
    // under `fileIsAdvisory`, which is `--uninstall-service` and returns at the service
    // block. So every run that reaches this line with a path either applied the file or
    // never had one. Even in the unreachable case the gate answers the same: `cfg` IS
    // `cliOnly` there, so either argv named the token (`namedOnCommandLine`) or no
    // secret is in force (`secretInForce`).
    //
    // What it is NOT is a fix for a reload hole, and the reason CHANGED with #404
    // while the conclusion did not. `--requirepass` used to be `Reloadable::No`, so a
    // SIGHUP that introduced a token was refused and a node file could not GAIN a
    // secret across a reload the way the daemon's could; it is `Reloadable::Yes` now,
    // so it can. That makes the FILESYSTEM re-ask below load-bearing rather than
    // merely tidy -- and the re-ask already covers it, because `secretFiles` is a
    // function of the LIVE snapshot rather than of the configuration this process
    // started with. What this expression could never decide is still what it could
    // never decide: `argvNamedSecret` is about ARGV, which no reload rewrites.
    //
    // **A warning and not a refusal**, for #384's reason: refusing a MISSING
    // credential fails closed and breaks nothing that worked, while refusing an
    // EXPOSED one breaks a deployment that is running today. It goes to `logger`,
    // which is journald, the Windows event log or launchd's file -- a durable record
    // rather than a console line that scrolls past.
    //
    // After `StartupPolicyRejection`, so a worker that will not start is told the
    // one thing it can act on rather than that plus a list about files it never
    // reaches.
    //
    // **`!cliOnly.requirePass.empty()` is provenance, not a value comparison.** `cliOnly` is
    // the command line ALONE -- parsed above, before any file was opened -- so a
    // non-empty token in it is argv having named one. The rulebook's clause is that a
    // flag whose default is EMPTY needs no explicit bit, there being nothing to arrive
    // at without asking; `--requirepass=` typed on purpose empties the merged value
    // too, and `secretInForce` is what answers it.
    //
    // A `requirePassExplicit` column would buy nothing here and would be a live hazard.
    // `ApplyFileSettings` sets a row's `explicitBit` for a key it read out of the FILE
    // (`Config/FileOptions.hpp` says so, and names this call site), and the worker
    // applies the file and argv into ONE `NodeConfig` -- so the bit read off `cfg`
    // means "named anywhere" and answers true for exactly the secret this warning
    // exists to report. Only a bit read off `cliOnly` would be right, which is the
    // same fact this expression already reads, one column and one bool later.
    SecretSubjectFiles<NodeConfig> const secretFiles =
        [configFile = lookup.path, argvNamedSecret = !cliOnly.requirePass.empty()](NodeConfig const& live) {
            return NodeSecretFiles(live, configFile, argvNamedSecret);
        };
    SecretExposureReport report = [&logger](std::string_view warning) {
        logger.Logf(LogLevel::Warn, "{}", warning);
    };

    // Two arms, and the difference is whether there is a SECOND moment at all. A
    // worker configured entirely from argv still names its key files and still has to
    // be told about them; what it has no use for is the memory that keeps a standing
    // exposure from being repeated, because nothing will re-ask.
    if (reloader.has_value())
        WatchSecretExposure<NodeConfig>(*reloader, secretFiles, std::move(report));
    else
        ReportSecretExposure<NodeConfig>(cfg, secretFiles, report);

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
        // **A worker states its own working directory, and it is not `/`** (#784).
        //
        // Daemonizing has to leave the invocation directory, and `/` was what this
        // host chdir'd to unconditionally. For a process that spawns a compiler that
        // is a wrong object under a correct key: the worker builds
        // `-fdebug-prefix-map=<its own directory>=<what the client asked for>`, a
        // prefix-map rule appends the unmatched tail, and `/` matches every absolute
        // path in the object -- `/usr/include/stdio.h` becomes `.usr/include/stdio.h`.
        // #674 gave the shipped unit a `WorkingDirectory=`, which closes the packaged
        // route and not this one: a unit's directory does not survive the double
        // fork's chdir, because the chdir happens after it.
        //
        // `WorkerPrefixMapRules` still DROPS the worker's own rule when its directory
        // contains the client's, so a `--daemon` worker produced the pre-#506 clang
        // state rather than the corrupted object. That drop stays -- it is what covers
        // a hand-written unit -- and this makes the case it covers rarer rather than
        // relying on it.
        //
        // Created here rather than left to the host, because this is the last point
        // at which anything can be REPORTED: `PosixDaemonHost` has redirected stdout
        // to /dev/null by the time the body runs, so a failure diagnosed inside it
        // reaches nobody. A directory that cannot be made falls back to `/`, which is
        // what this call did before, and says so.
        std::error_code scratchBaseError;
        auto const daemonDirectory = Node::ScratchBaseDirectory();
        std::filesystem::create_directories(daemonDirectory, scratchBaseError);
        if (scratchBaseError)
            logger.Logf(LogLevel::Warn,
                        "cannot create {}: {}; daemonizing into / instead, where this worker's own "
                        "-fdebug-prefix-map rule is dropped and a dispatched clang object records this machine's "
                        "directory",
                        daemonDirectory.string(),
                        scratchBaseError.message());
        auto const daemonWorkingDirectory = scratchBaseError ? std::filesystem::path { "/" } : daemonDirectory;
        host = MakePosixDaemonHost(cfg.pidfile, daemonWorkingDirectory.string());
#endif
    }
    if (!host)
        host = std::make_unique<ForegroundHost>();

    auto* const reloaderPtr = reloader.has_value() ? &*reloader : nullptr;
    return host->Run(
        [&cfg, &identityKey, &logger, reloaderPtr] { return WorkerBody(cfg, *identityKey, logger, reloaderPtr); });
}
