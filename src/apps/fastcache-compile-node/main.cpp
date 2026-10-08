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
#include "EndpointDialer.hpp"
#include "EndpointResolver.hpp"
#include "EnrollClient.hpp"
#include "EnrollmentResponder.hpp"
#include "EnrollmentWindow.hpp"
#include "FleetSummaryResponder.hpp"
#include "FleetTextResponder.hpp"
#include "FormationLoop.hpp"
#include "FormationRuntime.hpp"
#include "LiveNodeConfig.hpp"
#include "LiveStatsResponder.hpp"
#include "LiveStatsSources.hpp"
#include "NodeActivation.hpp"
#include "NodeAnnounce.hpp"
#include "NodeAudience.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeFirewall.hpp"
#include "NodeFormation.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIdentity.hpp"
#include "NodeIoLoop.hpp"
#include "NodeKey.hpp"
#include "NodeLogging.hpp"
#include "NodeMachineStanding.hpp"
#include "NodeMembership.hpp"
#include "NodePresenceTier.hpp"
#include "NodeProofResponder.hpp"
#include "NodeRefusal.hpp"
#include "NodeReload.hpp"
#include "NodeRoster.hpp"
#include "NodeStateFiles.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeSurfaces.hpp"
#include "NodeToolchains.hpp"
#include "OperatorCredentials.hpp"
#include "RaftStoreArchiver.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"
#include "SchedulerTier.hpp"
#include "SchedulingRedirect.hpp"
#include "ScratchClaim.hpp"
#include "SessionResponder.hpp"
#include "SharedCacheResponder.hpp"
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
#include <FastCache/Core/BoundedDrain.hpp>
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
#include <FastCache/Platform/Firewall.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/HostNaming.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/InheritedListener.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/NarrowText.hpp>
#include <FastCache/Platform/NetworkChangeWatcher.hpp>
#include <FastCache/Platform/ProcessExit.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Platform/WindowsEventLogger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
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

/// The ids of the accounts THIS process counts as the node's own when it judges a state directory
/// (`OwnStateAccountRows`): the node's service account for an administrator running a verb that
/// only INSPECTS the directory (`StateUseOf`), nothing beyond its own and the administrators' for
/// anything that runs the node -- a foreground start and the service under its supervisor alike.
/// @param cfg The configuration, for the verb and the service it names.
/// @return The ids.
[[nodiscard]] std::vector<std::string> StateOwnIds(NodeConfig const& cfg)
{
    return OwnStateAccountIds(
        IsAdministratorProcess() ? JudgingCaller::Administrator : JudgingCaller::Other, StateUseOf(cfg), cfg, &AccountIdOf);
}

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
/// Separated from `main` so the whole handoff -- how many descriptors arrived -- is one decision
/// with one answer, rather than checks interleaved with everything else a startup does. Where the
/// socket is bound is `Node::AdoptActivatedBind`'s, which `main` asks right after this.
///
/// **A descriptor rather than a listener**, since #290 stage 3. The merged `0xFC`
/// surface runs on the reactor, and the listener that serves it is built by
/// `FrameEndpoint::StartAdopted` from this descriptor; building one here would own
/// it, and handing an owned descriptor on is a double close rather than a handover.
///
/// Nothing here closes what it returns. On the refusal path the process exits
/// immediately, and on the success path ownership passes to `Node::ActivationHold`,
/// which keeps it for the process and hands every body a copy its listener owns --
/// and takes even when the adoption itself fails.
/// @return The descriptor, `std::nullopt` when nothing was handed over, or why the
///         handoff cannot be served.
[[nodiscard]] std::expected<std::optional<int>, NodeRefusal> ActivatedDescriptor()
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
        return std::unexpected { Refusal(
            NodeRefusalCause::HandedOverListeners,
            std::format("socket activation handed over {} listeners; this worker serves exactly one", inherited.size())) };

    // `--advertise` is NOT required here any more. It was, while the socket's address was a fact
    // this process never read: `--listen-node` described nothing, so the fallback registered a
    // value that described nothing either. The socket says where it listens (`getsockname`), and
    // `main` adopts that as the node bind, so the advertised endpoint is derived from the socket
    // clients actually reach -- a wildcard one advertises the address this machine routes from on
    // the socket's port, one bound to an address advertises that address.
    return std::optional { inherited.front() };
}

/// The socket a supervisor handed over, and where it is bound.
struct ActivatedSocket
{
    std::optional<int> descriptor;          ///< The descriptor, or nothing when this node binds its own.
    std::optional<BoundEndpoint> bind = {}; ///< Where it is bound; engaged exactly when `descriptor` is.
};

/// Take the socket a supervisor handed over, when there is one, and make @p cfg describe it.
///
/// The bind is adopted into the configuration (`Node::AdoptActivatedBind`) rather than carried
/// beside it, because every endpoint this node derives -- the advertised one, which the startup
/// table judges, the endpoint resolver publishes and a reload candidate is shaped by -- is derived
/// from the configuration in force. A bind kept anywhere else would be a second author of the
/// port clients are sent to, and `--listen-node`'s default is a port the unit does not serve.
/// @param cfg The configuration the start shaped; the Node surface's bind is the socket's after.
/// @param logger Where the handoff is announced.
/// @return The descriptor and its bind, both empty with no handoff, or why the start must stop.
[[nodiscard]] std::expected<ActivatedSocket, NodeRefusal> AdoptActivation(NodeConfig& cfg, ILogger& logger)
{
    auto const descriptor = ActivatedDescriptor();
    if (!descriptor.has_value())
        return std::unexpected { descriptor.error() };
    if (!descriptor->has_value())
        return ActivatedSocket { .descriptor = std::nullopt };

    auto const bind = BoundEndpointOfDescriptor(**descriptor);
    if (auto adopted = Node::AdoptActivatedBind(cfg, bind); !adopted.has_value())
        return std::unexpected { std::move(adopted).error() };
    logger.Logf(
        LogLevel::Info, "a supervisor handed over a listening socket on {}; --listen-node is not used", cfg.nodeListen);
    return ActivatedSocket { .descriptor = *descriptor, .bind = bind };
}

// A one-shot verb stopped before it runs -- a configuration file that did not load, a command line
// that did not parse -- ends as a command stopped at that step (`Platform/ProcessExit.hpp`,
// `ExitReaderRows`' `Operator` row, which the daemon's one-shots read as well): 1 for an I/O arm,
// 2 for a verdict. A START answers its `StartStage`'s code, because a supervisor reads that one.
// One code for both is what made them indistinguishable to systemd. `RefuseUnderService` asks
// `SelectsOneShotVerb`, and the daemon asks its own table the same question.
//
// **No site in this file spells an exit code.** A one-shot verb states its ENDING
// (`CommandEnding`) and `CommandExitCode` is the one mapping to a code; a start answers through
// `ExitCodeFor` or `ExitCodeOf`. `ProcessExit_test` holds both mains to that.

/// Whether @p cfg selects a one-shot verb -- a row of `EarlyVerbs` -- rather than a start.
/// @param cfg The configuration to ask, as far as it was assembled.
/// @return True when a verb answers and exits.
[[nodiscard]] bool SelectsOneShotVerb(NodeConfig const& cfg);

/// Refuse a start decided before the configuration exists -- a command line that does not
/// parse, a configuration file that does not load -- under the service argv names.
///
/// **Unless argv selects a one-shot verb**, which is no start: it answers the ending a command
/// stopped at @p stage has, reported to the terminal and never to a service host, as every verb's
/// own refusal is. Read off
/// the command line, since the file is what did not load; after a parse failure, off what parsed
/// before it -- where an operator types the verb.
///
/// **A registration replays its command line forever**, so an upgrade that retires a flag
/// leaves a service whose every start stops here, and a file edited into a typo does the same.
/// Under the SCM that exited before connecting: error 1053, *did not respond in a timely
/// fashion*, with the reason on a stderr nobody holds and in no log at all. So the reason also
/// goes to the event log, and the stop is reported with the step's exit code as the
/// service-specific code (`IDaemonHost::Refuse`). Read off the command line alone because
/// nothing else exists yet -- after a parse failure, off what parsed before it, which is where
/// the installer puts `--daemon` and `--service-name`.
/// @param commandLine What argv said, as far as it parsed.
/// @param stage The step that refused, whose row says which code a supervisor reads.
/// @param reason Why; already printed to stderr by the caller.
/// @return The code of @p stage's ending for a one-shot verb, otherwise @p stage's exit code.
[[nodiscard]] int RefuseUnderService(NodeConfig const& commandLine, StartStage stage, std::string_view reason)
{
    if (SelectsOneShotVerb(commandLine))
        return RefusalExitCode(ExitReader::Operator, stage);
    auto const code = ExitCodeFor(stage);
    auto const host = commandLine.daemon
                          ? MakeWindowsServiceHost(commandLine.serviceName,
                                                   ServiceHostOptions { .stop = StopPendingPlanFor(std::nullopt) })
                          : nullptr;
    if (host == nullptr)
        return code;
    auto const eventLogger = MakeWindowsEventLogger(commandLine.serviceName, commandLine.logLevel);
    if (eventLogger == nullptr)
        return host->Refuse(code);
    return RefuseStart(*host, *eventLogger, reason, code);
}

/// Report a one-shot operator verb's answer and say what to exit with.
///
/// **Three verbs, one shape.** A cluster command, an enrollment decision and a
/// machine asking to be let in all do the same two things with their runner's
/// answer: print a refusal to stderr and exit with its ending's code -- 2 for a
/// decision, made here or replied by the peer, 1 for an answer that never arrived or
/// decided nothing (`AnswerSource`) -- or print the answer to stdout and exit as
/// completed. Written out at each site that was three copies of
/// one branch pair -- blocks diverging by a name, which this codebase treats as a defect on its own -- and it is also what
/// took `main` past the cognitive-complexity threshold the build enforces when the enrollment verbs added the second and
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
[[nodiscard]] int ReportOneShotVerb(std::expected<std::string, UnfinishedCommand> const& answer,
                                    std::string_view prefix = {})
{
    if (!answer.has_value())
    {
        std::cerr << "fastcache-compile-node: " << answer.error().reason << '\n';
        return CommandExitCode(answer.error().ending);
    }

    std::cout << prefix << *answer;
    return CommandExitCode(CommandEnding::Completed);
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

/// Log the enrollment list's due warning -- machines waiting for a person -- until asked to stop.
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
void WarnWhileJoinersWait(Node::EnrollmentWindow& window, ILogger& logger, std::stop_token const& stop)
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
[[nodiscard]] std::expected<std::optional<Node::NodeIdentity>, Node::NodeIdentityRefusal> AdoptNodeIdentity(
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
/// @param keyGuard Who may read the key file, asked and established.
/// @param logger Where the key, or its absence, is reported.
/// @return The key pair, DISENGAGED on a node that holds none, or why there is none.
[[nodiscard]] std::expected<std::optional<Ed25519KeyPair>, NodeKeyRefusal> AdoptNodeKey(NodeConfig const& cfg,
                                                                                        ISecureRandom& random,
                                                                                        Node::INodeKeyFileGuard& keyGuard,
                                                                                        ILogger& logger)
{
    auto nodeKey = Node::ResolveNodeKeyFor(cfg, random, keyGuard);
    if (!nodeKey.has_value())
        return std::unexpected { std::move(nodeKey).error() };

    // Every node holds one now: every node has a state directory (`NodeStateDirectory`). The
    // result stays optional because the tiers that take it still model a node holding none,
    // which no configuration reaches any more.
    auto& held = *nodeKey;
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

/// Where a supervisor handed this body's node surface over, read off the socket.
/// @param activated The descriptor, or nothing when this node binds its own.
/// @return The address and port, `Node::AsConfigured` with no descriptor, or why the socket would not
///         say -- a refusal about the socket the supervisor handed over (`HandedOverListeners`).
[[nodiscard]] std::expected<Node::ActivatedNodeEndpoint, Node::NodeRefusal> ActivatedNodeEndpointOf(
    std::optional<int> activated)
{
    if (!activated.has_value())
        return Node::AsConfigured;
    auto const bound = BoundEndpointOfDescriptor(*activated);
    if (!bound.has_value())
        return std::unexpected { Node::Refusal(
            Node::NodeRefusalCause::HandedOverListeners,
            "the socket a supervisor handed over will not say which address and port it listens on, so this "
            "node cannot tell its own worker where its scheduler answers") };
    return Node::ActivatedEndpoint { .host = bound->host, .port = bound->port };
}

/// Where a body asks which address this machine routes from, and where the last answer is kept for
/// the process -- so a reformed body starts from the route the last one found, and a reload candidate
/// reads the one the running node advertises.
struct RouteParts
{
    IRouteProbe const& probe;  ///< The kernel's answer.
    Node::RouteHostCell& cell; ///< The last usable route host.
};

/// Everything the node runs, under whichever host runs it.
/// @param cfg The configuration the node started with.
/// @param identityKey The machine's identity key, or nothing.
/// @param logger Where the node reports.
/// @param reloader The live configuration; null with no file.
/// @param hostEvents Where the host's suspend, resume and network events arrive.
/// @param routeAt Where this machine's route is asked, and where the last one is kept for the process.
/// @return The process's exit status.
[[nodiscard]] int WorkerBody(NodeConfig const& cfg,
                             std::optional<Ed25519KeyPair> const& identityKey,
                             ILogger& logger,
                             NodeReloader* reloader,
                             IHostEvents& hostEvents,
                             Node::FormationBody const& formation,
                             std::optional<int> activated,
                             Node::LeaseCheckInForce& leaseCheck,
                             RouteParts const& routeAt)
{
    // ONE origin for every uptime this process reports. `/healthz` and the `0xFC`
    // `NodeStatus` verb both answer *how long has this been serving*, and two
    // independently sampled origins are two numbers that can disagree about one fact --
    // an operator comparing a scrape against the CLI would be reading a difference that
    // describes nothing. Taken as the first statement, so it is this body's own start
    // rather than whichever startup step happened to be declared above the reader.
    auto const startedAt = std::chrono::steady_clock::now();

    // Socket activation was resolved before the first body, and before the toolchains
    // (`RunNodeBodies`): `activated` is this body's own copy of what the supervisor handed over.
    //
    // And the ADDRESS and PORT it listens on, read off that socket here, once, before any tier
    // that registers: under activation `--listen-node` describes nothing, so a node serving its
    // own scheduler would otherwise register its worker and its presence where it does not serve
    // (`SchedulersOf`). Read here rather than in the tiers, which take it as a value.
    auto const activatedNodeEndpointOrError = ActivatedNodeEndpointOf(activated);
    if (!activatedNodeEndpointOrError.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", activatedNodeEndpointOrError.error().reason);
        return ExitCodeFor(activatedNodeEndpointOrError.error().cause);
    }
    auto const& activatedNodeEndpoint = *activatedNodeEndpointOrError;

    // Where the worker's heartbeat and the presence loop register, ONE for the process and re-read at
    // every round: this node's own scheduler and then the other voters its applied state records, or
    // those voters alone on a node serving none -- told by the
    // consensus tier below at every apply -- else the formation record's answer (`AppliedSchedulers`).
    // Declared before every tier that reads it or tells it, so it outlives them all.
    Node::AppliedSchedulers appliedSchedulers { cfg, activatedNodeEndpoint };

    // Where the leader the consensus tier follows answers the scheduling verbs, for the launchers a
    // node running no scheduler redirects there (#1639). Declared beside `appliedSchedulers` and for
    // its reason: the surface that reads it binds before consensus starts, and the tier that tells
    // it must not outlive it.
    Node::KnownSchedulingLeader knownSchedulingLeader;
    Node::SchedulingLeaderPublisher schedulingLeader { knownSchedulingLeader };

    // The ONE derivation, shared with the startup refusal that judges it. This value
    // goes to the worker tier's lease validator and to its REGISTER, and a lease's MAC is
    // taken over exactly this string -- so the endpoint the scheduler
    // signs, the endpoint this worker verifies and the endpoint clients dial are one
    // fact with one author. See `AdvertisedEndpoint`.
    //
    // Derived over the process's LIVE route host rather than the one `cfg` was shaped with at the
    // start: a body reformed after a roam starts at the address the fleet already has.
    auto routed = cfg;
    Node::ApplyRouteHost(routed, routeAt.cell.Current());
    auto const advertise = Node::AdvertisedEndpoint(routed);

    // **One per process, and that is the whole reason it is here rather than inside the worker
    // tier** (#1440). Both the worker's registrations and the presence loop's announcements
    // name this machine's address, and two sources would be two values changing at two
    // moments -- the defect #1279 closed inside the worker, reopened between two components.
    // Seeded with what the process started with; `endpointResolver` below republishes it when
    // the address this machine routes from moves or a reload re-pins it, and is its only writer.
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

    // **The sole publisher of both endpoints this node advertises** -- the `0xFC` one above and the
    // Raft one beside it -- re-derived from the configuration in force and the address this machine
    // routes from. Subscribed to the host's events BEFORE every other listener, so a network change
    // marks the route stale before the worker's heartbeat or the presence loop is woken for it --
    // and both refresh it at the top of each round anyway, so that order is a head start rather
    // than a dependency. Declared above every component that reads either endpoint.
    Node::AnnouncedEndpoint raftAnnounced { Node::RaftSelfEndpoint(routed) };
    Node::LiveNodeConfig const endpointConfig { cfg, reloader };
    Node::SchedulerProbeTargets const probeTargets { appliedSchedulers };
    core::platform::SteadyClock const endpointClock;
    Node::EndpointResolver endpointResolver { endpointConfig, routeAt.probe, probeTargets, endpointClock, routeAt.cell,
                                              announced,      raftAnnounced, metrics,      logger };
    HostEventSubscription const endpointEvents { hostEvents, endpointResolver };
    auto const endpointRefresh = endpointResolver.Launch();

    // **What this node has detected that an operator must act on, as one table** (#1364). Every
    // row was log-only before, and a log line scrolls away. Declared above every component that
    // raises or clears a row, so it outlives all of them; each component that runs answers its own
    // rows as it starts, and `Settle` below answers the rest before any surface serves.
    //
    // The process scope first, because it needs nothing but the sink: whether this build's
    // catalogue and sink agree is fixed before the process serves anything.
    Node::NodeConditions conditions;
    Node::EvaluateProcessConditions(conditions, metrics);
    Node::EvaluateHostNameCondition(conditions, cfg);

    // How a state file here is replaced, said -- and counted -- once per body when it falls back to the
    // classic rename, which on Windows a reader holding the file open refuses, and when its filesystem
    // cannot sync a directory, which degrades every replace there and raises `state-directory-unsynced`
    // (`ReportReplaceRoute`). A node keeping no state directory replaces no state file, and says so.
    if (auto const stateDirectory = Node::ChosenStateDirectory(cfg); stateDirectory.has_value())
    {
        Consensus::SystemDurableFiles durableFiles;
        Platform::SystemReplacingRename const replacingRename;
        static_cast<void>(
            Node::ReportReplaceRoute(stateDirectory->path, durableFiles, replacingRename, logger, metrics, conditions));
    }
    else
    {
        conditions.NotEvaluated(Node::NodeCondition::StateDirectoryUnsynced,
                                "this node keeps no state directory, so it replaces no state file");
    }

    // One policy for all THREE surfaces -- the compile port here, the scheduler and
    // the cache below -- and it outlives every one of them. A node that answered "is
    // this peer one of ours" differently at two of its surfaces would admit a peer to
    // the fleet and refuse it the objects that fleet produced, or worse, the reverse.
    // Not `const`: consensus republishes the member set into it while the node runs,
    // which is the whole point of membership being a replicated log entry rather than
    // a command-line list.
    Node::NodeMembership membership { cfg, logger };

    // **The roster every lease grant is verified against** (#178), built before anything that
    // reads it: the worker's validator borrows it, and consensus feeds it every applied state.
    //
    // Refusing here is where `RosterlessWorkerRefusal` is answered: a node that runs no consensus
    // holds no roster, so a worker on it that other machines can reach could verify nothing. The
    // startup table refused that shape already, so this is its belt.
    //
    // Its clock is the ONE silence is measured on: consensus reports how long ago a leader spoke,
    // an age, and past `LeaderSilenceBound` without a leader its applied state counts every grant is
    // refused (`consensus-leader-silent`).
    core::platform::SteadyClock const rosterClock;
    auto rosterOrRefusal = Node::NodeRoster::Build(cfg, rosterClock, &conditions);
    if (!rosterOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", rosterOrRefusal.error().reason);
        return ExitCodeFor(rosterOrRefusal.error().cause);
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
    // Before anything serves: a surface whose accept loop ends raises `surface-not-accepting`, one
    // that degrades `surface-accept-degraded`, and either turns `/healthz` into a 503, all read
    // from this one registry.
    Node::WatchAcceptLoops(nodeIo.AcceptLoops(), conditions);

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
    if (Node::ServesScheduler(cfg))
    {
        auto started = Node::SchedulerTier::Start(cfg,
                                                  membership.Oracle(),
                                                  schedulerClock,
                                                  schedulerWallClock,
                                                  metrics,
                                                  logger,
                                                  identityKey,
                                                  conditions,
                                                  Node::SchedulerConditionInterval);
        if (!started.has_value())
        {
            // Fatal for the same reason the admin endpoint's is: an operator who asked
            // for this is relying on it, and a node that started without it looks
            // healthy to everything that would otherwise have noticed.
            logger.Logf(LogLevel::Error, "the scheduler {}; refusing to start", started.error().reason);
            return ExitCodeFor(started.error().cause);
        }
        schedulerTier = std::move(*started);
    }

    // **The formation, before anything that describes this node** (`MakeFormationRuntime`): the
    // controller IS the summary every beacon, proof and enrollment answer reads, the observer every
    // proven fleet goes to, and what consensus tells the applied state. Declared after the scheduler,
    // whose service holds the join memos it reads, and before every tier that reads it, so it
    // outlives them all. Its beat begins once consensus runs, below.
    auto formationOrRefusal = Node::MakeFormationRuntime(
        cfg, identityKey, formation, announced, raftAnnounced, schedulerTier.get(), metrics, logger, &conditions);
    if (!formationOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", formationOrRefusal.error());
        return ExitCodeFor(StartStage::Formation);
    }
    auto const formationRuntime = *std::move(formationOrRefusal);

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
    //
    // And it is what the worker's heartbeat REPORTS (`HeartbeatRound::locality`): the
    // scheduler hints a dial at a reported address, the client mints its ticket for that
    // address, and the ticket audience below answers from this same set -- so a hint can
    // never name an address this node would refuse the ticket for. No second probe.
    //
    // And re-probed at the first question after a NETWORK CHANGE or a wake, not only on its
    // interval: a VPN re-address would otherwise leave the ticket audience accepting the address
    // this machine just lost, for up to one interval (W-7). A host event, never a miss, so no
    // peer can provoke the probe.
    auto const hostAddresses = MakeSystemHostAddresses();
    CachedLocalityOracle locality { *hostAddresses, cacheClock };
    HostEventSubscription const localityRefresh { hostEvents, locality };

    // ONE source, and the one site that presents this worker's credential borrows it: the
    // cache tier's `--upstream` client, immediately below. Nothing that talks to a scheduler
    // does -- a scheduler checks no password, and this machine's proof is its credential there.
    //
    // It reads the RELOADER rather than `cfg`, which is the whole of #404: `cfg` is
    // the configuration this worker STARTED with, and a rotated `--requirepass` lives
    // in the snapshot the reloader publishes. A worker with no configuration file
    // passes null and gets `cfg`'s value forever, which is correct rather than a
    // fallback -- it has no second moment for anything to arrive at.
    Node::ConfiguredCredential const credential { cfg, reloader };

    // Where a node handshake's nonce and ephemeral key come from, on both ends: the operating
    // system's generator (#1527). Its own instance rather than a share of `identityRandom`, which
    // is a different lifetime -- an identity is minted once at startup and handshakes are drawn
    // for as long as the process serves.
    SystemSecureRandom proofRandom;

    // **How this machine proves WHICH machine it is** (#178): its id, the identity key it holds,
    // whom it may prove itself to as a scheduler's client -- the roster its grants are checked
    // against -- and the generator above. Built wherever this node HOLDS an identity, not only
    // where it names a scheduler: the fleet's shared cache is proved to with it too, from every
    // node that has one. The presence tier still decides for itself whether it announces. Declared
    // ABOVE the cache, worker and presence tiers, which borrow it.
    //
    // On a node that runs consensus it is also handed `membership` -- the oracle this node's own
    // node-proof surface judges proofs by, below -- so neither announcing loop dials a scheduler of
    // this node's cluster before that cluster has recorded this node (`HoldUntilRecorded`). Without
    // it a node scheduling for itself proved itself to itself before its own first election and was
    // refused `node-key-unknown` with a remedy to admit it, at every such start.
    //
    // The prover answers `own-record-awaited` on a consensus node that announces -- one whose
    // formation record names somewhere it registers (`AwaitsItsOwnRecord`). One that registers
    // nowhere announces to nobody, so the row has nothing to observe there and says so, rather than
    // reading `clear` from a prover that is only ever asked to reach the shared cache.
    auto const consensusAnnounces = Node::AwaitsItsOwnRecord(cfg, activatedNodeEndpoint);
    std::optional<Node::NodeProofClient> prover;
    if (identityKey.has_value())
        prover.emplace(cfg.nodeId,
                       *identityKey,
                       *nodeRoster,
                       AddressWhen<Distributed::IMembershipOracle const>(Node::RunsConsensus(cfg), membership),
                       AddressWhen(consensusAnnounces, conditions),
                       proofRandom);
    if (Node::RunsConsensus(cfg) && !(consensusAnnounces && prover.has_value()))
        conditions.NotEvaluated(Node::NodeCondition::OwnRecordAwaited,
                                "this node registers with no scheduler, so it announces itself to none");

    // The fleet's shared cache, built on EVERY node: dormant until the applied cluster state names
    // this machine, when its host opens the tier on a thread of its own. Declared BEFORE the cache
    // tier -- whose upstream reads its directory and borrows its host -- and before the surface
    // that routes to it and the consensus tier that feeds it, so it is destroyed after all three.
    // The first of those is enforced as well as ordered: an in-process upstream that outlived the
    // host would end the process by name (`SharedCacheHost::Borrow`). Its condition is this node's
    // to answer only where cluster state reaches it.
    Node::SharedCacheService sharedCache { cfg,
                                           membership.Oracle(),
                                           cacheClock,
                                           metrics,
                                           logger,
                                           AddressWhen(Node::RunsConsensus(cfg), conditions),
                                           Node::ReconcileOn::OwnThread };

    // What the private tier reads through to is chosen from these, by a table keyed on the kind:
    // `--upstream` if typed, else the fleet's setting for a node with an identity, else nothing.
    // Every one of them is declared above the tier and outlives it.
    Node::UpstreamParts const upstreamParts { .upstream = cfg.upstream,
                                              .credential = credential,
                                              .directory = sharedCache.Directory(),
                                              .prover = AddressOrNull(prover),
                                              .io = nodeIo,
                                              .clock = cacheClock,
                                              .metrics = metrics,
                                              .conditions = AddressWhen(Node::RunsConsensus(cfg), conditions),
                                              .logger = logger,
                                              .host = &sharedCache.Host() };

    auto cacheTierOrRefusal = Node::StartCacheTierOrExplain(cfg, upstreamParts, locality, cacheClock, metrics, logger);
    if (!cacheTierOrRefusal.has_value())
    {
        // No flag prefix here, unlike its neighbours: this tier can fail over two
        // different flags -- the directory and the port -- and only
        // `StartCacheTierOrExplain` knows which, so it names it.
        logger.Logf(LogLevel::Error, "{}; refusing to start", cacheTierOrRefusal.error().reason);
        return ExitCodeFor(cacheTierOrRefusal.error().cause);
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

    // The worker: survey, scratch root, lease check, slot cap, compile responder and
    // heartbeat, as one object (#1387). Built BELOW the cache tier, because the slots it
    // offers are what the tier built leaves (#167), and ABOVE the node surface, which
    // routes the compile family to it and is therefore destroyed first. Null on a node
    // started with `--slots=0` (#206): no pool thread, no validator, no heartbeat.
    //
    // Its heartbeat dials through `workerDialer`, declared above it so it outlives the tier.
    Node::BlockingEndpointDialer workerDialer { Node::HeartbeatIoTimeout };
    auto workerOrRefusal = Node::WorkerTier::Start(Node::WorkerTierParts { .cfg = cfg,
                                                                           .reloader = reloader,
                                                                           .capacity = capacity,
                                                                           .announced = announced,
                                                                           .endpoints = endpointResolver,
                                                                           .activatedNodeEndpoint = activatedNodeEndpoint,
                                                                           .schedulers = appliedSchedulers,
                                                                           .membership = membership.Oracle(),
                                                                           .locality = locality,
                                                                           .io = nodeIo,
                                                                           .host = *host,
                                                                           .cacheTier = cacheTier.get(),
                                                                           .prover = AddressOrNull(prover),
                                                                           .leaseRoster = nodeRoster->Lease(),
                                                                           .leaseCheck = leaseCheck,
                                                                           .metrics = metrics,
                                                                           .logger = logger,
                                                                           .conditions = conditions,
                                                                           .hostEvents = hostEvents,
                                                                           .suspendWait = DefaultDrainWait(),
                                                                           .schedulerDialer = workerDialer },
                                                   &Node::MakeSystemWorkerMachine);
    if (!workerOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", workerOrRefusal.error().reason);
        return ExitCodeFor(workerOrRefusal.error().cause);
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
    //
    // And an identity key, which signs every admission the surface answers: one without would sign
    // nothing, and an unsigned admission is one every joiner refuses. Every configuration holds one
    // (`AdoptNodeKey`), so the clause narrows nothing that runs -- it is here so the surface and what
    // `NodeStatus` reports stay one condition if that ever changes.
    //
    // And WHY when it does not: the reason is the joiner's answer (`EnrollmentAbsenceTable`), derived
    // by the same function `ServesEnrollment` answers from, so the surface and its refusal cannot
    // disagree about whether -- or why.
    auto enrollmentAbsence = Node::EnrollmentAbsenceOf(cfg, schedulerTier != nullptr);
    if (!enrollmentAbsence.has_value() && !identityKey.has_value())
        enrollmentAbsence = Node::EnrollmentAbsence::NoIdentityKey;
    auto const servesEnrollment = !enrollmentAbsence.has_value();

    // Where this node sits in its consensus configuration (#1449), for `NodeStatus`. A SLOT,
    // because the tier that answers is built below and this surface is built now -- see
    // `ConsensusStandingSlot`. Declared before the status that reads it, so destroyed after.
    Node::ConsensusStandingSlot consensusStanding;

    // The enrollment list: who asked to join, and what an operator decided.
    //
    // Held whatever this node runs, because it is two words of state and a mutex, and
    // declared here so it outlives both the surface that mutates it and the status
    // source that reads it. **What decides whether it is REACHABLE is
    // `servesEnrollment` above** -- `RunsConsensus`, which is the one predicate the
    // consensus tier itself asks so the surface and the tier cannot disagree about
    // whether this node has a cluster, AND a scheduler tier, without which the responder
    // has no references to hold. A node missing either leaves the component null and the
    // whole family is refused at the door: a list that could never admit anybody
    // should not record requests, and one nothing serves should not be reported.
    //
    // It reports itself as a condition only where it is reachable at all: `servesEnrollment`
    // above, so a node with no list reports those rows NOT EVALUATED rather than a reassuring
    // `clear` for a list nothing can reach (#1364). A row forgotten undecided is counted in the
    // node's own sink.
    // A WALL clock: the instants an armed window's condition names are read against it.
    core::platform::SystemWallClock const enrollmentWallClock;
    Node::EnrollmentWindow enrollmentWindow {
        statusClock, AddressWhen(servesEnrollment, conditions), &metrics, enrollmentWallClock
    };

    // What `--node-status` says about the fleet's shared cache: the directory for where it is, the
    // cache tier's fleet half for how reaching another machine went, the host for whether this one
    // serves it, and the announced endpoint for where it does. Every one of them is declared above
    // and outlives it; a node with no tier, or none reading through to the fleet, passes a null
    // fleet half and still reports what it can.
    Node::NodeSharedCacheStatus const sharedCacheStatus { sharedCache.Directory(),
                                                          cacheTier != nullptr ? cacheTier->SharedCacheStatus() : nullptr,
                                                          &sharedCache.Host(),
                                                          announced };

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
                                   // `RunsConsensus`, not `servesEnrollment`: a standing is a
                                   // claim about a configuration only a consensus node holds, which
                                   // is a broader condition than serving an enrollment window (that
                                   // also wants a scheduler tier), so a node running none reports
                                   // the field ABSENT (#1449). Asked of the ONE predicate rather
                                   // than spelled as a conjunction here.
                                   .consensus = AddressWhen(Node::RunsConsensus(cfg), consensusStanding),
                                   // Every node has conditions, so this is never null here: a node
                                   // with nothing raised reports every row `clear` or
                                   // `not-evaluated`, never an empty list (#1364).
                                   .conditions = &conditions,
                                   // Every node has one; one that holds no roster reports the
                                   // field ABSENT through it (#178).
                                   .roster = nodeRoster.get(),
                                   // Every node has one, so a node with no shared cache says
                                   // `none` rather than nothing.
                                   .sharedCache = &sharedCacheStatus,
                                   // The Raft endpoint the resolver publishes, so a roamed node
                                   // reports where peers dial it NOW rather than at the start.
                                   .raftEndpoint = &raftAnnounced },
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
    // What `explain-admission <machine>` answers from: the roster grants are verified against, and
    // the enrollment window where this node serves one.
    Node::NodeMachineStanding const machineStanding { *nodeRoster, AddressWhen(servesEnrollment, enrollmentWindow) };
    Node::NodeStatusResponder nodeStatusResponder { nodeStatus, liveSources, membership.Oracle(), machineStanding, metrics };

    // The dashboard credential, read ONCE for the surfaces that guard the fleet with it: `/fleet`
    // over HTTP, and over `0xFC` the fleet subject of a live-stats subscription and `fleet-text`.
    auto dashboardOrRefusal = Node::LoadDashboardCredentialOrExplain(cfg);
    if (!dashboardOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", dashboardOrRefusal.error().reason);
        return ExitCodeFor(dashboardOrRefusal.error().cause);
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

    // The `Session` family's owner, on every built node: the node checks no password, and what an
    // `AUTH` can establish -- which machine a ticket speaks for -- does not depend on which
    // components this node runs.
    //
    // A ticket is spendable here when it names one of this node's own endpoints: the advertised
    // one (read per ticket, since `--advertise` reloads), this machine's host name, or any address
    // of this machine on the `Node` surface's ports. Who signed it is asked of the SAME roster a
    // lease is checked against, so a revoked or forgotten machine's tickets stop at once.
    Node::NodeAudience const ticketAudience {
        announced, QueryHostFacts().hostName, Node::NodeAudience::PortsOf(cfg), locality
    };
    Distributed::SpentTickets spentTickets;
    Distributed::TicketVerifier const ticketVerifier { nodeRoster->Lease(), ticketAudience, spentTickets };
    core::platform::SystemWallClock const ticketWallClock;
    Node::SessionResponder sessionResponder {
        ticketVerifier,
        Node::SessionKeys { .identityKey = identityKey.has_value() ? &*identityKey : nullptr, .machineId = cfg.nodeId },
        proofRandom,
        ticketWallClock,
        metrics
    };

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
    // binds it.
    //
    // Every admission it answers is SIGNED by this node's identity key, which `servesEnrollment`
    // requires, for the cluster id the summary here states -- the summary a joiner's probe proves
    // that key with -- so the summary is taken before it.
    // (Restated beside the dereference, which a reader -- and the optional-access check -- sees
    // here and not three screens up; it narrows nothing `servesEnrollment` does not.)
    //
    // The formation's summary when it runs one, which every mode does; the summary the start
    // computed for a node whose consensus is closed, which moves nowhere.
    Node::FixedFleetSummary const answeredSummary { Node::AnsweredFleetSummary(cfg) };
    auto const& summary = Node::SummarySourceOf(formationRuntime.get(), &answeredSummary);
    // Where each pending row's challenge is drawn: the operating system's generator, its own
    // instance for `proofRandom`'s reason -- a lifetime of its own.
    SystemSecureRandom enrollmentChallengeRandom;
    std::optional<Node::EnrollmentResponder> enrollmentResponder;
    if (servesEnrollment && identityKey.has_value())
        enrollmentResponder.emplace(enrollmentWindow,
                                    schedulerTier->ServiceForSurfaces(),
                                    membership.Oracle(),
                                    summary,
                                    *identityKey,
                                    enrollmentChallengeRandom,
                                    metrics,
                                    logger);

    // The identity prover (#178), built wherever this node runs CONSENSUS: a proof is judged
    // against the cluster's applied roster -- members and revoked keys --
    // which only a consensus member holds, and it is asked of `membership`, whose `ExplainKey` is
    // the one door to that answer for the proof and for every later verb alike. A consensus node
    // holds an identity key, as every node does.
    std::optional<Node::NodeProofResponder> nodeProofResponder;
    if (Node::RunsConsensus(cfg) && identityKey.has_value())
        nodeProofResponder.emplace(cfg.nodeId, *identityKey, membership, consensusStanding, proofRandom, metrics, logger);

    // Which fleet this node is in, answered to anybody who asks: every node holds an identity key,
    // and one that runs no consensus answers `NoCluster` itself rather than leaving the family
    // missing at the door. The summary is fixed for the process, and it is the ONE this node
    // announces: the discovery tier below is handed the same object, never a second derivation --
    // nor does the enrollment surface above, which signs admissions for the cluster it states.
    std::optional<Node::FleetSummaryResponder> fleetSummaryResponder;
    if (identityKey.has_value())
        fleetSummaryResponder.emplace(summary, *identityKey);

    // What a node running consensus and no scheduler -- a learner -- answers the scheduling verbs
    // with: `NotLeader` naming the leader it follows (#1639), so a launcher pointed at this machine
    // reaches the fleet. Asked of `RedirectsScheduling`, which the per-mode table pins, never of a mode.
    std::optional<Node::SchedulingRedirectResponder> schedulingRedirect;
    if (Node::RedirectsScheduling(cfg))
        schedulingRedirect.emplace(membership.Oracle(), knownSchedulingLeader);

    auto nodeSurfaceOrRefusal = Node::StartNodeSurfaceOrExplain(
        nodeIo,
        cfg,
        // Composed where the wiring test composes it: every component a required parameter
        // of its own type, so one left out, or two transposed, fails the build here.
        Node::ComposeSurfaceComponents(cacheTier.get(),
                                       schedulerTier.get(),
                                       AddressOrNull(schedulingRedirect),
                                       workerTier.get(),
                                       nodeStatusResponder,
                                       enrollmentResponder.has_value() ? Node::EnrollmentOwner { &*enrollmentResponder }
                                                                       : std::unexpected { enrollmentAbsence.value_or(
                                                                             Node::EnrollmentAbsence::NoIdentityKey) },
                                       liveStatsResponder,
                                       fleetTextResponder,
                                       AddressOrNull(nodeProofResponder),
                                       AddressOrNull(fleetSummaryResponder),
                                       sessionResponder,
                                       sharedCache),
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
        // No flag prefix: this can fail over --listen-node or over the scheduler,
        // and only `StartNodeSurfaceOrExplain` knows which, so it names the flag.
        logger.Logf(LogLevel::Error, "{}; refusing to start", nodeSurfaceOrRefusal.error().reason);
        return ExitCodeFor(nodeSurfaceOrRefusal.error().cause);
    }
    // May legitimately be null, and for exactly one reason: a node with none of the
    // three components serves no 0xFC port, which has been logged. A bind FAILURE is
    // not that case -- this surface's row states `Refuse`, so it arrives as the error
    // above rather than as a null. See `BindFailurePolicy` in NodeSurfaces.hpp.
    auto const nodeSurface = std::move(*nodeSurfaceOrRefusal);

    // Says, once a minute and for as long as any is waiting, that machines have asked to join
    // and nobody has decided about them.
    //
    // **Repeating rather than one line when the first asks**, because a single line scrolls away
    // and a machine waiting for a person is waiting for somebody who may not be looking. The
    // condition `enrollment-requests-waiting` carries the same fact to somebody who is not
    // reading this log.
    //
    // A thread of its own rather than a ride on the heartbeat, because the heartbeat
    // belongs to the WORKER and this belongs to the leader -- a scheduler holding no
    // worker is an ordinary deployment and would otherwise be the one node that never
    // said anything. It exists only where the surface does, so a node with no cluster
    // starts no thread.
    //
    // The DECISION is `TakeDueWarning`, which is pure over an injected clock and is
    // tested against a `core::platform::ManualClock`. The driver is `WarnWhileJoinersWait` above,
    // split out of this function exactly as `AnnounceOnce` was and for the same two
    // reasons -- it took `WorkerBody` past the cognitive-complexity ceiling the build
    // enforces, and a loop with a decision in it is more behaviour than belongs in the
    // one translation unit no test reaches.
    //
    // An `optional` rather than an unconditional `jthread` whose body returns at once,
    // which is what this was: that spelling STARTED a thread on every node in the fleet
    // and made the sentence above ("a node with no cluster starts no thread") false by
    // one word. The condition is `servesEnrollment`, so the thread and the surface
    // cannot disagree about whether there is a list to watch.
    std::optional<std::jthread> enrollmentWatch;
    if (servesEnrollment)
        enrollmentWatch.emplace([&](std::stop_token const& stop) { WarnWhileJoinersWait(enrollmentWindow, logger, stop); });

    // Consensus, when the operator configured a cluster -- which every scheduler has, even a
    // lone one (#178). It is what gives the scheduler tier a role at all: without it every
    // node in a fleet would believe it schedules, and two nodes handing out the same
    // machine's slots is the one thing the architecture says only one may do.
    //
    // Started AFTER the scheduler tier, because its observers push into it, and
    // declared after too, so it is destroyed first and cannot call into a tier that
    // has gone.
    auto consensusOrRefusal = Node::StartConsensusOrExplain(
        cfg,
        schedulerTier,
        announced,
        raftAnnounced,
        identityKey,
        membership,
        *nodeRoster,
        appliedSchedulers,
        schedulingLeader,
        Node::SharedCacheListeners { .directory = sharedCache.Directory(),
                                     .host = sharedCache.Host(),
                                     .upstream = cacheTier != nullptr ? &cacheTier->Upstream() : nullptr },
        metrics,
        logger,
        &conditions,
        Node::FormationHooksOf(formationRuntime.get()));
    if (!consensusOrRefusal.has_value())
    {
        // No flag prefix here, for the reason the cache tier's line below has none:
        // consensus fails over --node-id, --raft-self, --listen-raft or
        // --cluster-dir, and only the tier knows which, so its message names the
        // flag. It used to prefix `--node-id `, which rendered the peer refusal as
        // "--node-id --node-id=n1 names no --raft-peer" -- and that message is now
        // `StartupPolicyRejection`'s own row, which names the flag by construction.
        logger.Logf(LogLevel::Error, "{}; refusing to start", consensusOrRefusal.error().reason);
        return ExitCodeFor(consensusOrRefusal.error().cause);
    }
    // Null on a node with no `--listen-raft`: a pure worker, since #178 makes every scheduler a
    // consensus member.
    auto const consensusTier = std::move(*consensusOrRefusal);

    // The formation's beat, over the tier it proposes through. Declared after the tier, so the beat
    // has stopped and let go of it before the tier is destroyed.
    auto const formationBeat = Node::BeginFormation(formationRuntime.get(), consensusTier.get());

    // Attached as soon as the tier exists and before anything serves, and detached before the
    // tier is destroyed: the attachment is declared after it. A null tier attaches nothing.
    auto const consensusStandingAttached = consensusStanding.Attach(consensusTier.get());

    // The consensus tier runs its own reactor and owns its own registry; the node reads one.
    if (consensusTier != nullptr)
        consensusTier->AcceptLoops().forward(nodeIo.AcceptLoops());

    // Discovery, when the operator configured it. Declared AFTER consensus and so
    // destroyed before it, because its observer pushes into the tier above: a
    // discovery loop outliving the thing it hands peers to is a dangling reference
    // that only fires while a node is shutting down.
    // `summary` is the one summary the FLEET-SUMMARY responder signs, so a beacon and an answer from
    // this node are one derivation; every fleet it proves goes on to the formation.
    auto discoveryOrRefusal = Node::StartDiscoveryOrExplain(
        cfg, consensusTier, summary, conditions, metrics, logger, Node::DiscoveryFormationOf(formationRuntime.get()));
    if (!discoveryOrRefusal.has_value())
    {
        // Fatal; why is `RowFor(NodeSurface::Discovery).bindFailureReason` (#352).
        // Not restated here -- this line and that row would be the two places the
        // ticket is about.
        logger.Logf(LogLevel::Error, "--discovery {}; refusing to start", discoveryOrRefusal.error().reason);
        return ExitCodeFor(discoveryOrRefusal.error().cause);
    }
    // May legitimately be null: no `--discovery` means the cluster is the members
    // an operator admitted, which is an ordinary deployment.
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
            .consensus = Node::ConsensusScrapeSource(consensusTier.get()) },
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
    Node::FileTrustNodeKeyGuard platformHistoryGuard;
    Node::OwnStateAccountsGuard historyGuard { platformHistoryGuard, StateOwnIds(cfg) };
    Node::FleetSampler sampler { fleetSources,
                                 metrics,
                                 snapshotProvider,
                                 wall,
                                 Node::SetAsideForeignHistory(Node::HistoryPaths::For(cfg), historyGuard, logger),
                                 logger };

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

    auto surfaceOrRefusal = Node::StartAdminSurfaceOrExplain(cfg,
                                                             *host,
                                                             metrics,
                                                             std::move(snapshotProvider),
                                                             fleetSources,
                                                             &sampler,
                                                             dashboardCredential,
                                                             logger,
                                                             conditions,
                                                             nodeIo.AcceptLoops());

    // Fatal here and not in the daemon, which is a real difference between the two
    // binaries rather than a defect in either; the row says why
    // (`RowFor(NodeSurface::Admin).bindFailureReason`, #352).
    if (!surfaceOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", surfaceOrRefusal.error().reason);
        return ExitCodeFor(surfaceOrRefusal.error().cause);
    }
    auto const adminSurface = std::move(*surfaceOrRefusal);

    // How loudly a scheduler that does not answer, or refuses, is said: ONE for the process, lent to
    // the worker's heartbeat and to the presence loop below, so a machine says each loss and each
    // recovery once rather than once per loop. Declared before both handles, so it outlives the
    // threads that report into it.
    core::platform::SteadyClock const reachabilityClock;
    // Over the list `NodePresence::Start` builds its link from, so the scope and the loop cannot
    // disagree about whether this node announces. Only an announcing node answers the row; `Settle`
    // answers the rest.
    auto const announces = Node::AnnouncesToAScheduler(cfg, activatedNodeEndpoint);
    Node::SchedulerReachability schedulerReachability { reachabilityClock, announces ? &conditions : nullptr };

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
                                                                           .consensus = Node::RunsConsensus(cfg),
                                                                           .announces = announces }))
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
        heartbeat.emplace(workerTier->Launch(statusClock, schedulerReachability));

    // **Started unconditionally, and that one word is the whole of #1440.** A node with
    // `--slots=0` has no `workerTier`, so before this existed such a machine reached the fleet
    // through nothing at all -- absent from the Machines table it was itself serving, and
    // handing over no history across an election.
    //
    // Declared AFTER the sampler it hands history through and therefore destroyed before it.
    // That is the ordering rationale `WorkerHeartbeat` used to carry, and it moved here with
    // the history: this loop is now the only thing in the process that reads the sampler's
    // handover cursor.
    //
    // Its rounds dial through `presenceDialer`, declared just above it so it outlives the loop.
    Node::BlockingEndpointDialer presenceDialer { Node::PresenceIoTimeout };
    auto const presence =
        Node::NodePresence::Start(Node::NodePresenceParts { .cfg = cfg,
                                                            .schedulers = appliedSchedulers,
                                                            .capacity = capacity,
                                                            .announced = announced,
                                                            .cacheTier = cacheTier.get(),
                                                            .metrics = metrics,
                                                            .sampler = sampler,
                                                            .logger = logger,
                                                            .conditions = conditions,
                                                            .prover = AddressOrNull(prover),
                                                            .reachability = schedulerReachability,
                                                            .dialer = presenceDialer,
                                                            .hostEvents = hostEvents,
                                                            .askedJoins = Node::AskedJoinsOf(formationRuntime.get()),
                                                            .endpoints = &endpointResolver });

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
    auto const listeningOn = Node::DescribeListeningEndpoint(cfg, activated.has_value());
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
                // Named even when there is none: a withheld name reads `withheld (localhost)`,
                // never the empty field `advertising ,` that says nothing about why.
                Node::DescribeAdvertisedEndpoint(cfg),
                // The toolchain count this node is BRINGING UP, not the count it is
                // serving: the survey runs on the heartbeat thread and has almost
                // certainly not finished when this prints (#365) -- and a ready line is a
                // statement about starting anyway.
                Node::WorkerReadinessPhrase(cfg,
                                            workerTier != nullptr ? std::optional { workerTier->Slots() } : std::nullopt,
                                            workerTier != nullptr ? workerTier->StartupToolchainCount() : 0),
                Node::AdmissionSummary(cfg));
    // The line's fact, handed to the service host, which reports RUNNING on it. After the line, so
    // whoever is told the node serves finds the line already written.
    DaemonControls::Instance().MarkServing();

    // **Main waits here now, and there is no accept loop to interrupt.** This was
    // `core::async::syncRun(server.Run())` -- the dedicated listener's accept loop, which blocked
    // this thread until a stop closed it, and which needed a watcher thread to notice
    // the stop at all. The merged surface's loops run on `nodeIo`'s own thread, so what
    // is left for main to do is wait for the stop and then drain.
    //
    // That deletes the watcher rather than rehoming it: it existed only because a
    // parked `accept()` cannot be woken by a flag.
    //
    // A reform ends it the same way: a move that changed this node's shape saved the record, and
    // `main` starts the next body from it once this one has drained exactly as for a stop.
    while (!DaemonControls::Instance().StopRequested() && !formation.durables.reform.Pending())
    {
        // Taken here rather than in the handler, for the reason the handler's own note
        // gives. `TakeReloadRequest` clears the flag atomically, so a burst of SIGHUPs
        // costs one re-read rather than one per signal.
        //
        // Nothing is handed to the heartbeat thread: it compares snapshots itself, so
        // this call publishes a new configuration and says so, and the next beat picks
        // it up on its own.
        if (DaemonControls::Instance().TakeReloadRequest())
            Node::ApplyReloadRequest(reloader, membership, conditions, logger);
        std::this_thread::sleep_for(StopPollInterval);
    }
    logger.Log(LogLevel::Info, Node::DrainSentence(formation.durables.reform.Pending()));

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
    // A node that came up, served, and then found it had nothing to compile with exits late but
    // with the same diagnostic, and a supervisor must not read that as a clean stop. Which exit is
    // `WorkerEnding`'s: a failure a compiler installed since would fix.
    if (auto const ending = workerTier != nullptr ? workerTier->Ending() : std::nullopt; ending.has_value())
        return ExitCodeFor(*ending);
    return ExitCodeOf(ProcessExit::Served);
}

/// What `main` serves with, across reforms: what outlives every body.
struct ServingParts
{
    Cluster::IFormationStore& store;        ///< Where the record is kept.
    Cluster::FleetEndpointsFile& endpoints; ///< Where the fleet endpoints are remembered.
    Node::IStoreArchiver& archiver;         ///< What finishes a left cluster's store at every reform.
    Node::ReformRequest& reform;            ///< Raised by a move; ends a body and starts the next.
    Node::FormationRuntimeParts runtime;    ///< What every body's formation acts through.
    Node::LeaseCheckInForce& leaseCheck;    ///< Where every body's worker records the lease check it built.
    RouteParts route;                       ///< Where every body's endpoint resolver asks and keeps the route.
};

/// Wait @p wait, returning early once a stop is asked: the pause between two reforms.
/// @param wait How long.
void PauseUnlessStopped(std::chrono::milliseconds wait)
{
    auto const until = std::chrono::steady_clock::now() + wait;
    while (!DaemonControls::Instance().StopRequested() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(StopPollInterval);
}

/// Serve until a body ends other than for a reform (`Node::RunNodeBodies`).
///
/// What is left here is what only `main` can build -- the hold over the socket activation `main`
/// read once for the process, and the serving body itself; what a reform decides is
/// `Node::RunNodeBodies`', which a test target builds.
/// @param cfg The configuration the start shaped: the first body's, and every reformed body's while
///        this node has no file. With one, a reformed body is adopted into the reloader's snapshot.
/// @param identityKey This node's identity key.
/// @param logger Where every body logs.
/// @param reloader The live configuration, or null when this node has no file.
/// @param running What the start adopted; rewritten to what the running body was adopted from.
/// @param hostEvents Where the host's resume and network events arrive, for every body.
/// @param activated The socket a supervisor handed over (`AdoptActivation`), or nothing.
/// @param parts What outlives every body.
/// @return The last body's exit code.
[[nodiscard]] int ServeNodeBodies(NodeConfig const& cfg,
                                  std::optional<Ed25519KeyPair> const& identityKey,
                                  ILogger& logger,
                                  NodeReloader* reloader,
                                  IHostEvents& hostEvents,
                                  Node::AdoptedFormation& running,
                                  std::optional<int> activated,
                                  ServingParts const& parts)
{
    // The socket a supervisor handed over was taken by `main`, ONCE for the process, before the
    // startup table judged the endpoint derived from its bind (`AdoptActivation`): the handoff
    // clears the environment it read, and a body's listener closes what it adopts. The hold keeps
    // the original and every body serves a copy (`Node::ActivationHold`), so a reformed body never
    // binds a port the supervisor holds.
    SystemInheritedDescriptors const inheritedDescriptors;
    Node::ActivationHold const activation { activated, inheritedDescriptors };

    core::platform::SteadyClock const loopClock;
    auto publisher = std::optional<Node::ReloaderPublisher> {};
    if (reloader != nullptr)
        publisher.emplace(*reloader);
    auto const bodies = Node::NodeBodies {
        .adoption = Node::ReformAdoption { .store = parts.store,
                                           .archiver = parts.archiver,
                                           .endpoints = parts.endpoints,
                                           .publisher = publisher.has_value() ? &*publisher : nullptr },
        .reform = parts.reform,
        .activation = activation,
        .controls = Node::LoopControls { .stopRequested = [] { return DaemonControls::Instance().StopRequested(); },
                                         .pause = PauseUnlessStopped,
                                         .clock = loopClock },
    };
    auto const durables = Node::FormationDurables { parts.store, parts.endpoints, parts.reform, parts.runtime };
    // The configuration IN FORCE, which an accepted reload changes and `cfg` does not.
    Node::LiveNodeConfig const inForce { cfg, reloader };
    return Node::RunNodeBodies(
        inForce,
        running,
        bodies,
        [&](NodeConfig const& shaped, Cluster::FormationRecord const& record, std::optional<int> served) {
            return WorkerBody(shaped,
                              identityKey,
                              logger,
                              reloader,
                              hostEvents,
                              Node::FormationBody { .record = record, .durables = durables, .reloader = reloader },
                              served,
                              parts.leaseCheck,
                              parts.route);
        },
        logger);
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

    /// Where the environment and this process's privilege are read -- the machine-wide state
    /// directory a service registration owns among them.
    IConfigPathProbe const& pathProbe;
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
/// @return Completed's code when the configuration would start, Declined's when it would not.
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
    if (report.refusal.has_value())
        context.logger.Logf(LogLevel::Error, "{}", *report.refusal);
    return CommandExitCode(report.ending);
}

/// Print this node's identity, minting what the state directory does not hold yet
/// (`--print-identity`, #178).
///
/// **Minting is the point, not a side effect**, which is what separates this verb from every
/// other one in `EarlyVerbs`: an operator admitting a member needs its key before it is
/// admitted, and the key does not exist until something mints it. The start that follows reads the same files back as
/// `Recorded`. Through the resolvers the start uses, so an id or a key this prints is the one that start will run as -- and
/// a key file that is there and cannot be used is refused here exactly as it is there.
/// @param context The configuration and the console logger.
/// @return Completed's code once printed; otherwise the ending minting came to (`EndingOf`).
[[nodiscard]] int RunPrintIdentity(EarlyVerbContext const& context)
{
    auto& cfg = context.cfg;
    SystemSecureRandom random;
    Node::FileTrustNodeKeyGuard platformGuard;
    Node::OwnStateAccountsGuard keyGuard { platformGuard, StateOwnIds(cfg) };
    auto key = Node::ResolveNodeKeyFor(cfg, random, keyGuard);
    if (!key.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", key.error().message);
        return CommandExitCode(EndingOf(key.error().fault));
    }
    auto const publicKey = key->pair.PublicKey();

    // The id travels with the key (#178): a member is admitted under it -- `--enroll-approve`
    // names both, as these very lines print them -- and proves it on every connection to a
    // scheduler.
    auto identity = Node::ResolveNodeIdentity(Node::NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!identity.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", identity.error().message);
        return CommandExitCode(EndingOf(identity.error().fault));
    }
    identity->publicKey = publicKey;
    Node::ApplyNodeIdentity(cfg, *identity);

    auto dialAddress = std::optional<std::string> {};
    if (RunsConsensus(cfg))
        if (auto const dial = ConsensusDialAddressOf(cfg); dial.has_value())
            dialAddress = *dial;

    std::cout << Node::DescribeIdentity(cfg.nodeId, publicKey, dialAddress);
    // And where it is kept: an unelevated run prints a PER-USER identity the service never holds.
    std::cout << Node::DescribeIdentityOrigin(cfg);
    return CommandExitCode(CommandEnding::Completed);
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
/// @return Completed's code once the file is in place; otherwise the ending seeding came to (`EndingOf`),
///         declined for a template it could not use and failed for a file it began writing --
///         the one mapping `fastcached --seed-config` answers through as well.
[[nodiscard]] int RunSeedConfig(EarlyVerbContext const& context)
{
    auto const seeded =
        SystemConfigPath(SystemConfigPathProbe {}, NodeApplicationName).and_then([&context](auto const& destination) {
            return SeedConfigFile(context.cfg.seedConfigTemplate, destination, DirectoryPolicy::AdministratorsOnly);
        });
    if (!seeded.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", seeded.error().ToString());
        return CommandExitCode(EndingOf(seeded.error().code));
    }
    context.logger.Logf(LogLevel::Info, "{}", SeedOutcomeSentence(*seeded, context.cfg.seedConfigTemplate));
    return CommandExitCode(CommandEnding::Completed);
}

/// Register or remove this worker's service entry (`--install-service`,
/// `--uninstall-service`).
/// @param context The merged configuration, the command line alone, and the logger.
/// @return The code of the registration's own ending, or Declined's when it was refused.
[[nodiscard]] int RunServiceRegistration(EarlyVerbContext const& context)
{
    // Only an install has to be viable; an uninstall merely names a registration to
    // remove, and refusing to remove one because it was misconfigured is how a bad
    // registration becomes permanent.
    //
    // Judged on the MERGED configuration, and registered from the command line alone --
    // which is not a contradiction. What the service will run with is the file plus
    // these arguments, so judging the command line alone would refuse the documented
    // setup, where the toolchains come out of the packaged file. What
    // is baked in is still only what was typed, plus the `--config` path that supplies
    // the rest.
    //
    // Only a configuration the formation SHAPED is judged here. One whose record was held is
    // unshaped -- it runs no consensus, so the rules about an admitting node stay silent -- and is
    // judged after the registration has secured the directory, on the configuration the service
    // starts with (`InstallWithServiceFirewall`).
    if (context.cfg.installService && context.cfg.formation.has_value())
        if (auto const rejection = NodeInstallRejection(context.cfg))
        {
            context.logger.Logf(LogLevel::Error, "{}", *rejection);
            return CommandExitCode(CommandEnding::Declined);
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
        context.logger.Logf(LogLevel::Error, "{}; refusing to install", adopted.error().message);
        return CommandExitCode(EndingOf(adopted.error().fault));
    }

    // `cliOnly`, never `cfg`: a registration replays its arguments at every start, so
    // baking in what the FILE said would freeze one reading of that file into launch
    // arguments that then outrank the file itself -- the operator edits it, restarts the
    // service, and nothing changes. What the registration does carry is the `--config`
    // path, so the service reads the current file at every start rather than a snapshot
    // of it.
    //
    // The firewall follows the registration: opened for what the SERVICE will open at its next
    // start -- the merged configuration shaped by the formation record read AFTER the registration
    // secured the state directory, and read from exactly the directory it secured
    // (`InstallNodeService`) -- and closed on removal once the delete succeeded or found no
    // registration, never after a refused one (`WithRemovalFirewall`), since a rule outliving its
    // service admits nothing but misleads whoever reads the list.
    auto const firewall = MakeSystemFirewall();
    auto const result = [&] {
        if (context.cfg.installService)
            return Node::InstallNodeService(
                context.cfg,
                context.cliOnly,
                context.pathProbe,
                CurrentExecutablePath(),
                [&](ServiceSpec const& spec) { return InstallService(spec, context.cfg.serviceScope); },
                [&](ServiceSpec const& spec) { return UninstallService(spec, context.cfg.serviceScope); },
                firewall.get());
        auto const spec = MakeNodeServiceSpec(CurrentExecutablePath(), context.cliOnly, context.pathProbe);
        return WithRemovalFirewall(UninstallService(spec, context.cfg.serviceScope), firewall.get(), spec.serviceName);
    }();
    if (result.Ending() == CommandEnding::Completed)
        std::cout << "fastcache-compile-node: " << result.message << '\n';
    else
        std::cerr << "fastcache-compile-node: " << result.message << '\n';
    return result.ExitCode();
}

/// Convert this node's disk tier to the format this build reads (`--migrate-cache`).
/// @param context The configuration and the console logger.
/// @return Completed's code once converted; otherwise the conversion's ending, declined when it changed nothing
///         and failed when it stopped part-way -- as `fastcached --migrate-storage` answers.
[[nodiscard]] int RunMigrateCache(EarlyVerbContext const& context)
{
    auto const outcome = MigrateDiskTier(context.cfg);
    if (!outcome.has_value())
    {
        context.logger.Logf(LogLevel::Error, "{}", outcome.error().reason);
        return CommandExitCode(outcome.error().ending);
    }
    std::cout << "fastcache-compile-node: " << *outcome << '\n';
    return CommandExitCode(CommandEnding::Completed);
}

/// Say one line of an operator verb's credential handling on stderr.
/// @param text The line.
void SayOperatorCredential(std::string_view text)
{
    std::cerr << "fastcache-compile-node: " << text << '\n';
}

/// Put a cluster question to a running cluster and report the answer (`--cluster-*`).
///
/// Each endpoint the verb dials is shown a ticket minted by this machine's own node
/// (`OperatorCredentials`), so the verb works from any machine the cluster holds the key of.
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunClusterVerb(EarlyVerbContext const& context)
{
    Node::OperatorCredentials operatorCredentials { context.cfg, Node::DefaultOneShotDialer(), &SayOperatorCredential };
    return ReportOneShotVerb(RunClusterAdmin(context.cfg, context.cfg.cluster, operatorCredentials.Credentials()));
}

/// Cordon this machine's own worker, or lift its cordon (`--cordon`, `--uncordon`).
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunCordonVerb(EarlyVerbContext const& context)
{
    return ReportOneShotVerb(Node::RunCordonAdmin(context.cfg, context.cfg.cordon));
}

/// Decide who may join, on behalf of an operator (`--enroll-*`).
/// @param context The configuration.
/// @return What the verb answered, rendered by `ReportOneShotVerb`.
[[nodiscard]] int RunEnrollVerb(EarlyVerbContext const& context)
{
    Node::OperatorCredentials operatorCredentials { context.cfg, Node::DefaultOneShotDialer(), &SayOperatorCredential };
    return ReportOneShotVerb(Node::RunEnrollAdmin(context.cfg, context.cfg.enroll, operatorCredentials.Credentials()));
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
constexpr std::array<EarlyVerbRow, 8> EarlyVerbs { {
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
    // start at all: making the operator satisfy a toolchain probe first
    // would be demanding they fix a running configuration before being allowed to fix
    // the store that stops it running.
    { .applies = [](NodeConfig const& cfg) { return cfg.migrateCache; }, .run = &RunMigrateCache },

    // A question asked OF a running cluster, rather than a worker starting up. After the
    // service row, because an installation is about this machine and this is about
    // somebody else's; before the startup table, which refuses `--scheduler` on a node
    // that serves: a cluster command is what that flag aims, and it needs no toolchain.
    { .applies = [](NodeConfig const& cfg) { return Node::IsOneShotVerb(cfg, Node::OneShotVerb::Cluster); },
      .run = &RunClusterVerb },

    // An operator taking THIS machine out of the fleet or putting it back (#1303). Beside
    // the cluster row because it is the same kind of thing -- a question put to a running
    // node by a person at a terminal -- and before the startup table for that row's reason:
    // it serves nothing, so the rules for a configuration this node would SERVE with do not
    // apply. What it needs, a `--listen-node` to dial, it refuses by name itself.
    { .applies = [](NodeConfig const& cfg) { return cfg.cordon != CordonCommand::None; }, .run = &RunCordonVerb },

    // An operator deciding who may join, rather than a worker starting up. Beside the
    // cluster row because it is the same kind of thing -- a question put to a running
    // cluster by a person at a terminal, answered, and the process exits.
    { .applies = [](NodeConfig const& cfg) { return Node::IsOneShotVerb(cfg, Node::OneShotVerb::Enroll); },
      .run = &RunEnrollVerb },
} };

bool SelectsOneShotVerb(NodeConfig const& cfg)
{
    return std::ranges::any_of(EarlyVerbs, [&cfg](EarlyVerbRow const& verb) { return verb.applies(cfg); });
}

/// Where the network watcher starts, relative to the host's `Run`.
enum class NetworkWatcherStart : std::uint8_t
{
    BeforeTheHost, ///< Windows: no host forks, so the watcher starts before `Run`, outliving all of it.
    InsideTheBody, ///< POSIX: a thread does not survive `PosixDaemonHost`'s fork.
};

/// This platform's answer: the one place that decides it.
#if defined(_WIN32)
constexpr auto ThisNetworkWatcherStart = NetworkWatcherStart::BeforeTheHost;
#else
constexpr auto ThisNetworkWatcherStart = NetworkWatcherStart::InsideTheBody;
#endif

/// Start the network watcher when @p at is where this platform starts it; a refusal is reported
/// and not fatal, because no consumer may depend on a host event arriving (`HostEvent` says why).
/// @param at Where the caller is.
/// @param sink Where the events go; must outlive the watcher.
/// @param clock What the debouncer reads; must outlive the watcher.
/// @param logger Where a refusal is reported.
/// @return The watcher; null when it does not start here, the platform offers none, or the OS refused.
[[nodiscard]] std::unique_ptr<NetworkChangeWatcher> StartNetworkWatcherAt(NetworkWatcherStart at,
                                                                          IHostEventSink& sink,
                                                                          core::platform::IClock const& clock,
                                                                          ILogger& logger)
{
    if (at != ThisNetworkWatcherStart)
        return nullptr;
    auto started = StartNetworkChangeWatcher(sink, clock, NetworkDebounce {});
    if (started.has_value())
        return std::move(*started);
    logger.Logf(LogLevel::Warn, "network changes will not be reported: {}", started.error());
    return nullptr;
}

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
    auto const flow = ParseNodeCommandLine(argvSpan.subspan(1), cliOnly);
    if (!flow.has_value())
    {
        // The FIELD as well as the reason. Without it an unrecognised argument reads
        // as `unrecognised argument` and names nothing at all -- the daemon and the
        // test client have always printed both, and this is the binary whose flags
        // an operator is most likely to be typing by hand.
        auto const unparsed = std::format("{}: {}", flow.error().field, flow.error().context);
        std::cerr << "fastcache-compile-node: " << unparsed << '\n';

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

        // What the command line NAMED, from the whole of it rather than from what parsed before
        // the bad token: the verb it asked for and the service it runs as are the operator's
        // wherever they typed them, so `--no-such-flag --print-surfaces` is the worksheet's refusal.
        NodeConfig named;
        ApplyRecognisedOptions(NodeOptions(), argvSpan.subspan(1), named);
        return RefuseUnderService(named, StartStage::CommandLine, unparsed);
    }

    if (cliOnly.help)
    {
        // The color decision is made here rather than inside the help renderer so
        // that module stays free of ambient probes -- and on Windows the call also
        // enables virtual-terminal processing, so it must precede any output.
        std::cout << HelpText(StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain);
        return CommandExitCode(CommandEnding::Completed);
    }
    if (cliOnly.version)
    {
        std::cout << "fastcache-compile-node " << FASTCACHE_NODE_VERSION << '\n';
        return CommandExitCode(CommandEnding::Completed);
    }
    // The parse above IS the check: an argument its row refuses has exited there, naming the flag.
    // Answered before any file is read for `--version`'s reason, and because what the installer
    // asks is whether the values it is about to remember are ones this node reads (B3-1).
    if (cliOnly.checkArguments)
    {
        std::cout << "fastcache-compile-node: every argument parses\n";
        return CommandExitCode(CommandEnding::Completed);
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
    SystemConfigPathProbe const pathProbe;
    auto const lookup = EffectiveConfigPath(cliOnly.configPath, pathProbe, NodeApplicationName);

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
            return RefuseUnderService(cliOnly, ConfigurationFileStage(loaded.error().code), loaded.error().ToString());
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

    // **Where this node keeps its identity, before any verb can ask** -- an install mints the id
    // there and `--print-surfaces` names it. From this process's privilege and environment,
    // through the probe the config lookup used, into both configurations as `ApplyNodeIdentity`
    // writes the id. An invocation that would write there and has none refuses here, by name;
    // one that writes nothing -- an uninstall among them -- goes on (`StateDirectoryRefusal`).
    Node::ApplyNodeStateDirectory(cfg, pathProbe);
    Node::ApplyNodeStateDirectory(cliOnly, pathProbe);
    if (auto const refusal = Node::StateDirectoryRefusal(cfg); refusal.has_value())
    {
        // A directory the environment could not name is an arm that failed, not a verdict on the
        // configuration: the next start may find it (`NodeIdentityIo`).
        std::cerr << "fastcache-compile-node: " << *refusal << '\n';
        return RefuseUnderService(cliOnly, StartStage::NodeIdentityIo, *refusal);
    }

    // **The formation record, READ before any verb or rule is asked** -- a mode is the state held
    // in the cluster dir, and it decides whether this node runs consensus, opens the Raft port and
    // serves a scheduler. Read and never written here: an install, a `--print-surfaces` and the
    // startup rules below are judged by the mode the node WILL run in, which before a first start
    // is the solitary record that start mints (`ProspectiveRecord`). The mint itself waits until
    // this command line has been judged, as the identity's does.
    //
    // A record that cannot be read is held rather than refused here, so the verbs that write
    // nothing -- an uninstall among them -- still run; the start refuses it by name below, and an
    // install reads it again once its handover has secured the directory, deriving the service's
    // firewall rules from that (`InstallWithServiceFirewall`). So is a directory another account
    // may write in or wrote into: its writers, then who owns each file, are asked before any is
    // read (`ReadStateDirectoryFormation`), as the key resolution asks again.
    //
    // Judged through ONE guard for the whole invocation (`OwnStateAccountsGuard`): this process's own
    // account and the administrators', and -- for an administrator running a verb that only inspects
    // the directory -- the node's service account, so an elevated `--print-surfaces` over the
    // service's directory is judged as the service will run it. A start, foreground or under the
    // supervisor, keeps the strict set: it writes there.
    Node::FileTrustNodeKeyGuard platformStateGuard;
    Node::OwnStateAccountsGuard stateGuard { platformStateGuard, StateOwnIds(cfg) };
    auto const formationDirectory = Node::ChosenStateDirectory(cfg);
    std::optional<Cluster::FileFormationStore> formationStore;
    std::optional<Cluster::FleetEndpointsFile> endpointsFile;
    auto keptFormation = std::expected<Node::KeptFormation, Node::FormationUnread> { std::unexpected {
        // The formation's own step, which is `FormationUnread`'s default: no directory resolved yet.
        Node::FormationUnread { .reason = "this node has no state directory to keep its formation record in" } } };
    if (formationDirectory.has_value())
    {
        formationStore.emplace(formationDirectory->path);
        endpointsFile.emplace(formationDirectory->path);
        keptFormation = Node::ReadStateDirectoryFormation(formationDirectory->path, stateGuard);
    }

    // Shaped by what was read; where nothing could shape it, told WHY (`ShapeByKeptFormation`), so the
    // mode line and the `--raft-self` row name the reading's refusal -- that row told an operator who
    // had typed `--listen-raft` that its absence was the cause.
    auto shaped = Node::ShapeByKeptFormation(cfg, keptFormation).and_then([&] {
        return Node::ShapeByKeptFormation(cliOnly, keptFormation);
    });
    if (!shaped.has_value())
        keptFormation = std::unexpected { std::move(shaped).error() };

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
    EarlyVerbContext const verbContext { .cfg = cfg, .cliOnly = cliOnly, .logger = *consoleLogger, .pathProbe = pathProbe };
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

    // **The SCM's host, chosen HERE, before anything below judges the configuration** -- and the
    // same object the body runs under at the end. Every refusal between here and `Run` returned
    // from `main` before connecting to the SCM, which then reported error 1053, *did not respond
    // in a timely fashion*, for a service that had refused by name into the event log. Each now
    // goes through `RefuseStart`, which connects and reports the stop with the refusing step's
    // exit code as the service-specific code (`ExitCodeFor`). Null off Windows and without `--daemon`: there the foreground
    // host answers, whose refusal is the exit code -- a POSIX daemon has not forked yet.
    //
    // The host's events hub is declared with it, since the Windows host is handed it here and
    // both outlive the body the host runs; the network watcher that feeds it starts below.
    HostEventHub hostEvents;
    // RUNNING once the node serves (`MarkServing` beside the readiness line), never as its body
    // begins: a node that then could not bind its port had already been reported started, and an
    // installer's start action answered success over it (B4-6).
    auto serviceHost = cfg.daemon ? MakeWindowsServiceHost(cfg.serviceName,
                                                           ServiceHostOptions { .stop = StopPendingPlanFor(cfg.drainTimeout),
                                                                                .hostEvents = &hostEvents,
                                                                                .readiness = ServiceReadiness::BodySignals })
                                  : nullptr;
    ForegroundHost foreground;
    IDaemonHost& startHost = serviceHost ? *serviceHost : static_cast<IDaemonHost&>(foreground);

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
    // HERE, as inline `if`s, and both became `StartupPolicyRejection` rows -- the
    // toolchain pair by #403, the scheduler by #386, which the formation record then
    // retired: where a worker registers is no flag any more. Neither reads anything but the
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
    //
    // **A state directory no reading could shape this line from ends the start FIRST, by its own
    // arm** (`UnreadStateStage`): a transient read -- a listing, an access list the platform would
    // not answer, a record that would not open -- is `Failed` and retried by the supervisor, as it
    // was before the startup table learnt to name it; a verdict is `Refused`. Ahead of the table,
    // whose every row ends a start as the permanent `StartupRules`, and whose flag rows would judge
    // a mode this configuration does not have.
    if (auto const stage = UnreadStateStage(cfg); stage.has_value())
        return RefuseStart(
            startHost, logger, std::format("{}; refusing to start", StateDirectoryUnreadRefusal(cfg)), ExitCodeFor(*stage));

    // **The socket a supervisor handed over, adopted before anything derives an endpoint from the
    // bind** -- the table below judges the advertised endpoint, and under activation that is the
    // socket's address and port rather than `--listen-node`'s default (`AdoptActivation`). Resolved
    // ONCE for the process: the handoff clears the environment it read, and `ServeNodeBodies` keeps
    // the descriptor for every body (`Node::ActivationHold`). Before the toolchains too, which take
    // seconds, while a bad handoff is decided in microseconds.
    auto const activation = AdoptActivation(cfg, logger);
    if (!activation.has_value())
        return RefuseStart(startHost,
                           logger,
                           std::format("{}; refusing to start", activation.error().reason),
                           ExitCodeFor(activation.error().cause));

    // **The address this machine routes from, probed once before the table judges the endpoints it
    // derives** -- `auto` advertises it on a wildcard bind, and a rule over an address judged without
    // it would judge one this node never advertises. Applied to both configurations as the names are
    // below; asks the kernel's routing table only, sends nothing and waits on nothing. The cell keeps
    // it for the process: every body's resolver re-probes from it, and every reload candidate is
    // shaped by its live value (`ReloadBasis::routeHost`).
    auto const routeProbe = MakeSystemRouteProbe();
    Node::RouteHostCell routeHost { Node::ProbeRouteHost(*routeProbe, {}).value_or(std::string {}) };
    Node::ApplyRouteHost(cfg, routeHost.Current());
    Node::ApplyRouteHost(cliOnly, routeHost.Current());
    if (auto const rejection = StartupPolicyRejection(cfg))
        return RefuseStart(startHost, logger, *rejection, ExitCodeFor(StartStage::StartupRules));

    // **This machine's names, BOUNDED** (`HostNamingBound`), after the refusal above so a typo
    // is not kept waiting on DNS, and before the identity, because the member entry this node
    // synthesises for itself dials the name. The bare host name stands in when the lookup does
    // not answer in time, and the warning says so -- peers may not resolve it. Nothing serves
    // yet, which is why this is a bound rather than a wait behind serving.
    Node::ThreadedHostNamingLookup namingLookup { [] { return MakeSystemHostNaming(); } };
    auto const names = Node::ResolveHostNames(namingLookup, QueryHostFacts().hostName, Node::HostNamingBound);
    if (auto const warning = Node::HostNamingWarning(names); warning.has_value())
        logger.Logf(LogLevel::Warn, "{}", *warning);
    Node::ApplyHostNames(cfg, names.names);
    Node::ApplyHostNames(cliOnly, names.names);

    // Asked again of the names, because a rule over an address derived from them answered the
    // awaited name above as "supplied at startup", and it is supplied now.
    if (auto const rejection = StartupPolicyRejection(cfg))
        return RefuseStart(startHost, logger, *rejection, ExitCodeFor(StartStage::StartupRules));

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
    SystemSecureRandom identityRandom;

    auto const identityKey = AdoptNodeKey(cfg, identityRandom, stateGuard, logger);
    if (!identityKey.has_value())
        return RefuseStart(startHost,
                           logger,
                           std::format("{}; refusing to start", identityKey.error().message),
                           ExitCodeFor(StageOf(identityKey.error().fault)));
    auto const publicKey = PublicHalf(*identityKey);

    auto const identity = AdoptNodeIdentity(cfg, cliOnly, identityRandom, logger, publicKey);
    if (!identity.has_value())
        return RefuseStart(startHost,
                           logger,
                           std::format("{}; refusing to start", identity.error().message),
                           ExitCodeFor(StageOf(identity.error().fault)));

    // **The formation record this node runs by**, kept -- minted and SAVED when the state
    // directory holds none -- before any tier starts. After the table above for the identity's
    // reason: a configuration the node refuses must not leave a record behind. And after the
    // KEY, whose resolution is what judges the state directory -- and creates it, its owner's
    // alone -- so nothing is written into a directory other accounts could have planted in.
    // The identity already saw the mode it runs in: the record, or the one this mints.
    if (!keptFormation.has_value())
        return RefuseStart(startHost,
                           logger,
                           std::format("{}; refusing to start", keptFormation.error().reason),
                           ExitCodeFor(StartStage::Formation));
    // Engaged whenever a record was read -- the store is what read it -- so this is the same
    // fact stated where the dereference can see it.
    if (!formationStore.has_value() || !endpointsFile.has_value())
        return RefuseStart(startHost,
                           logger,
                           "no state directory keeps this node's formation record; refusing to start",
                           ExitCodeFor(StartStage::Formation));

    // **Adopted** (`AdoptFormation`), HERE where a refusal can still be reported: minted and SAVED
    // when the state directory holds none, and a move a crash interrupted finished -- the store a
    // dissolve left in the root archived -- before any consensus tier opens the directory. Every
    // reform adopts it again the same way (`RunNodeBodies`).
    Node::RaftStoreArchiver archiver { Node::NodeStateDirectory(cfg) };
    Node::ReformRequest reform;
    auto adopted = Node::AdoptFormation(
        cfg, *formationStore, archiver, *endpointsFile, identityRandom, core::platform::defaultSystemWallClock());
    if (!adopted.has_value())
        return RefuseStart(
            startHost, logger, std::format("{}; refusing to start", adopted.error()), ExitCodeFor(StartStage::Formation));
    if (auto applied = Node::ApplyFormation(cliOnly, adopted->record, adopted->remembered); !applied.has_value())
        return RefuseStart(
            startHost, logger, std::format("{}; refusing to start", applied.error()), ExitCodeFor(StartStage::Formation));
    logger.Logf(LogLevel::Info, "{}, cluster {}", Node::DescribeFormationMode(cfg), cfg.clusterId);

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
    //
    // **What the running worker's lease check IS**, declared before the reloader that reads it at
    // every reload and destroyed after it: a reload may not widen admission on a worker that built
    // the unchecked one (`Node::ReloadCheckWith`). Recorded by the factory that builds the check,
    // in whichever body runs, so the guard asks what was BUILT rather than a flag shape.
    Node::LeaseCheckInForce leaseCheck;
    std::optional<NodeReloader> reloader;
    if (!lookup.path.empty())
        reloader.emplace(cfg,
                         lookup.path,
                         Node::ReloadCandidateReader(argvSpan.subspan(1),
                                                     Node::ReloadBasis {
                                                         .stateDirectory = cfg.stateDirectory,
                                                         .hostNames = names.names,
                                                         // The record the RUNNING body was adopted from,
                                                         // which a reform rewrites -- never the file, which a
                                                         // move saves on the beat thread before the body it
                                                         // ends has drained. A reload runs on the body's stop
                                                         // loop, the thread a reform adopts on between bodies.
                                                         .formation = Node::RunningFormationOf(*adopted),
                                                         .identity = identity->value_or(Node::NodeIdentity {}),
                                                         // The route the running node derives its
                                                         // endpoints from NOW, so the table judges a
                                                         // candidate by the address it would advertise.
                                                         .routeHost = [&routeHost] { return routeHost.Current(); },
                                                         // The socket a supervisor handed over, which every
                                                         // candidate binds as the start did.
                                                         .activatedBind = activation->bind,
                                                     }),
                         Node::ReloadCheckWith(leaseCheck));

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
    // `lookup.path` that would not load already exited far above, except
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
            return NodeSecretFileSubjects(live, configFile, argvNamedSecret);
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

    // The host's events: power through the SCM, network changes through the watcher. The hub is
    // declared with the start host above, and the clock below the hub, because both outlive the
    // body the host runs. WHERE the watcher starts is `ThisNetworkWatcherStart`'s: on Windows here,
    // before `host->Run`; on POSIX inside the body `host->Run` is handed, because the POSIX watcher
    // reads its socket on a thread and a thread does not survive `PosixDaemonHost`'s fork -- started
    // here, it would be left behind in the parent that exits. Either way the watcher outlives the
    // body it serves: this one the whole `Run`, the body's own the `ServeNodeBodies` call. A watcher
    // the OS refused is reported and not fatal; inside a POSIX daemon's body that report reaches
    // the logger the body has, whose console the host has redirected.
    core::platform::SteadyClock networkClock;
    auto const networkWatcher = StartNetworkWatcherAt(NetworkWatcherStart::BeforeTheHost, hostEvents, networkClock, logger);

    // The host is chosen last, so everything that can be reported to a terminal
    // already has been. `--daemon` is what a SUPERVISOR THAT WANTS BACKGROUNDING
    // passes: the Windows SCM needs it, and systemd and launchd must not pass it,
    // because they supervise the process they started and reap a job that forks
    // as "exited".
    //
    // Windows' host was chosen above, before the refusals it reports, and is the one taken here.
    std::unique_ptr<IDaemonHost> host = std::move(serviceHost);
#if !defined(_WIN32)
    if (cfg.daemon)
    {
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
    }
#endif
    if (!host)
        host = std::make_unique<ForegroundHost>();

    auto* const reloaderPtr = reloader.has_value() ? &*reloader : nullptr;

    // What every body's formation acts through, built once here -- the one place production seams are
    // constructed -- and handed to each body's runtime (`Node::FormationRuntimeParts`).
    SystemStopAwareWait const formationWait;
    Node::BlockingEndpointDialer formationDialer { Node::FormationRuntime::IoTimeout };
    auto const formationSrv = MakeSystemSrvResolver();
    auto const parts = ServingParts { .store = *formationStore,
                                      .endpoints = *endpointsFile,
                                      .archiver = archiver,
                                      .reform = reform,
                                      .runtime =
                                          Node::FormationRuntimeParts {
                                              .wall = core::platform::defaultSystemWallClock(),
                                              .random = identityRandom,
                                              .dialer = formationDialer,
                                              .srv = *formationSrv,
                                              .wait = formationWait,
                                          },
                                      .leaseCheck = leaseCheck,
                                      .route = RouteParts { .probe = *routeProbe, .cell = routeHost } };
    return host->Run([&cfg,
                      &identityKey,
                      &logger,
                      reloaderPtr,
                      &hostEvents,
                      &networkClock,
                      &adopted,
                      activated = activation->descriptor,
                      &parts] {
        auto const bodyNetworkWatcher =
            StartNetworkWatcherAt(NetworkWatcherStart::InsideTheBody, hostEvents, networkClock, logger);
        return ServeNodeBodies(cfg, *identityKey, logger, reloaderPtr, hostEvents, *adopted, activated, parts);
    });
}
