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
#include "WorkerServer.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/Environment.hpp>
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
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <Dispatch.hpp>
#include <WorkerProtocol.hpp>

namespace
{
namespace Wire = FastCache::CompileCacheWire;
using namespace FastCache;

/// Everything this worker was told to be.
struct NodeConfig
{
    std::string scheduler; ///< host:port of the scheduler's dispatch endpoint.
    std::string advertise; ///< host:port clients should reach this worker on.
    std::string bindAddress { "0.0.0.0" };
    std::uint16_t port { 6676 };
    /// fingerprint=compilerPath, repeatable. A worker with none serves nothing,
    /// which is deliberate: there is no default compiler, because a default is how
    /// a job ends up running against something nobody chose.
    std::vector<std::string> toolchains;
    std::uint32_t slots { 0 }; ///< 0 means "one per hardware thread".
    std::string token;
    std::string user;
    LogLevel logLevel { LogLevel::Info };
    bool help { false };
    bool version { false };
};

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

/// Split `fingerprint=path`.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> SplitToolchain(std::string_view spec)
{
    auto const eq = spec.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= spec.size())
        return std::nullopt;
    return std::pair { std::string { spec.substr(0, eq) }, std::string { spec.substr(eq + 1) } };
}

/// A port, refusing 0 rather than letting a bind fail with a confusing message.
[[nodiscard]] std::expected<std::uint16_t, ConfigError> ParseNodePort(std::string_view sv)
{
    auto value = 0U;
    auto const* const begin = sv.data();
    auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
    auto const [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {} || ptr != end || value == 0 || value > 65535)
        return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, "port", std::format("not a port: {}", sv)));
    return static_cast<std::uint16_t>(value);
}

/// A positive slot count.
[[nodiscard]] std::expected<std::uint32_t, ConfigError> ParseSlots(std::string_view sv)
{
    auto value = 0U;
    auto const* const begin = sv.data();
    auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
    auto const [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {} || ptr != end || value == 0)
        return std::unexpected(
            ArgvError(ConfigErrorCode::OutOfRange, "slots", std::format("must be a positive count: {}", sv)));
    return value;
}

/// A log level by name.
[[nodiscard]] std::expected<LogLevel, ConfigError> ParseNodeLogLevel(std::string_view sv)
{
    // Spelled here rather than shared with the daemon's parser, which lives in an
    // anonymous namespace in CliParser.cpp. The names must match what the daemon
    // accepts -- an operator setting the same level on both should not have to learn
    // two vocabularies.
    static constexpr std::array<std::pair<std::string_view, LogLevel>, 6> levels { {
        { "trace", LogLevel::Trace },
        { "debug", LogLevel::Debug },
        { "info", LogLevel::Info },
        { "warn", LogLevel::Warn },
        { "error", LogLevel::Error },
        { "fatal", LogLevel::Fatal },
    } };
    // Iterated rather than searched with an iterator, and that is a portability
    // fix rather than a style choice. `readability-qualified-auto` asks for
    // `auto const* const` here, which is right on libc++ -- where a std::array
    // iterator IS a raw pointer -- and does not compile on MSVC's STL, where it is
    // a class type. Taking the value directly sidesteps the difference entirely.
    for (auto const& [name, level]: levels)
        if (name == sv)
            return level;
    return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, "log-level", std::format("unknown level: {}", sv)));
}

/// Every accepted option, one row each.
///
/// The same table idiom the daemon and the launcher use, so an accepted spelling
/// is necessarily a documented one and adding a flag is adding a row.
[[nodiscard]] std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept
{
    static constexpr auto options = std::to_array<OptionSpec<NodeConfig>>({
        { .primary = "--scheduler",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignFrom<&NodeConfig::scheduler, ParseText>(),
          .description = "the scheduler's --listen-dispatch endpoint. Required: a\n"
                         "worker nothing knows about serves nobody." },
        { .primary = "--advertise",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignFrom<&NodeConfig::advertise, ParseText>(),
          .description = "host:port CLIENTS should use to reach this worker.\n"
                         "Defaults to --bind and --port, which is wrong behind NAT\n"
                         "or on a multi-homed host: the scheduler hands this string\n"
                         "to clients verbatim, so a worker that advertises an\n"
                         "address only it can reach is leased and then never\n"
                         "answers." },
        { .primary = "--bind",
          .arity = Arity::Value,
          .operand = "=<address>",
          .apply = AssignFrom<&NodeConfig::bindAddress, ParseText>(),
          .description = "address to listen on (default 0.0.0.0)" },
        { .primary = "--port",
          .arity = Arity::Value,
          .operand = "=<n>",
          .apply = AssignFrom<&NodeConfig::port, ParseNodePort>(),
          .description = "port to listen on (default 6676)" },
        { .primary = "--toolchain",
          .arity = Arity::Value,
          .operand = "=<fingerprint>=<compiler>",
          .apply = AppendFrom<&NodeConfig::toolchains, ParseText>(),
          .description = "a toolchain this worker serves; repeatable. Required.\n"
                         "There is deliberately no default compiler: a default is\n"
                         "how a job ends up running against something nobody chose." },
        { .primary = "--slots",
          .arity = Arity::Value,
          .operand = "=<n>",
          .apply = AssignFrom<&NodeConfig::slots, ParseSlots>(),
          .description = "concurrent compiles (default: one per hardware thread).\n"
                         "Advertised to the scheduler AND enforced here: a worker\n"
                         "that accepted more would be fuller and slower than the\n"
                         "scheduler believes, at the same moment." },
        { .primary = "--requirepass",
          .arity = Arity::Value,
          .operand = "=<secret>",
          .apply = AssignFrom<&NodeConfig::token, ParseText>(),
          .description = "credential presented to the scheduler" },
        { .primary = "--log-level",
          .arity = Arity::Value,
          .operand = "=<level>",
          .apply = AssignFrom<&NodeConfig::logLevel, ParseNodeLogLevel>(),
          .description = "trace, debug, info, warn, error, fatal (default info)" },
        { .primary = "--help",
          .alias = "-h",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::help>(),
          .flow = ParseFlow::Stop,
          .description = "show this help and exit" },
        { .primary = "--version",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::version>(),
          .flow = ParseFlow::Stop,
          .description = "print the version and exit" },
    });
    static_assert(TableIsWellFormed<NodeConfig>(options));
    return options;
}

/// Render the usage text from the same rows the parser matches.
[[nodiscard]] std::string HelpText(UsageColor color)
{
    UsageRows optionRows;
    AddOptionRows(optionRows, NodeOptions());

    auto const blocks = std::to_array<UsageBlock>({ { .entries = optionRows.Rows() } });
    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "fastcache-compile-node - a compile worker for fastcached distributed builds." },
        { .title = "OPTIONS", .blocks = allBlocks.subspan(0, 1) },
    });
    return RenderUsage({ .sections = sections }, color);
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
        return 2;
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

    // Both are refused at startup rather than at the first job. A worker missing
    // either would register (or fail to) and then refuse everything, which presents
    // to an operator as "distribution does not work" rather than as a misconfigured
    // node.
    if (cfg.scheduler.empty())
    {
        logger.Logf(LogLevel::Error, "--scheduler is required; a worker nothing knows about serves nobody");
        return 2;
    }
    if (cfg.toolchains.empty())
    {
        logger.Logf(LogLevel::Error,
                    "--toolchain is required; a worker with none would register and then refuse every job "
                    "the scheduler sent it");
        return 2;
    }

    std::map<std::string, std::string> toolchains;
    for (auto const& spec: cfg.toolchains)
    {
        auto const split = SplitToolchain(spec);
        if (!split.has_value())
        {
            logger.Logf(LogLevel::Error, "malformed --toolchain '{}'; expected <fingerprint>=<compiler>", spec);
            return 2;
        }
        toolchains.emplace(split->first, split->second);
    }

    auto const slots = cfg.slots != 0 ? cfg.slots : std::max(1U, std::thread::hardware_concurrency());
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
    auto listener = BlockingListener::Bind(cfg.bindAddress, cfg.port, /*backlog=*/128);
    if (listener == nullptr || !listener->IsBound())
    {
        logger.Logf(LogLevel::Error,
                    "could not bind {}:{} ({})",
                    cfg.bindAddress,
                    cfg.port,
                    listener ? listener->BindError() : std::string_view { "null listener" });
        return 1;
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
    listener->SetTimeouts(AcceptPollInterval, RequestIoTimeout);

    auto const runner = Cc::MakeProcessRunner();
    auto const scratch = std::filesystem::temp_directory_path() / "fastcache-compile-node";
    Cc::CompileJobRunner jobs { *runner, scratch, toolchains };

    // Every lease is accepted, and that is stated rather than hidden. The boundary
    // today is reachability of this port plus the shared credential -- the same
    // boundary the cache itself has. Validating a token against the scheduler is the
    // seam's other implementation and belongs with mTLS rather than bolted on here;
    // LeaseValidator exists so that is a substitution, not a rewrite.
    Cc::WorkerProtocol protocol { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec } };

    Node::WorkerServer server { *listener, protocol, slots, logger };

    Cc::Credential const credential { .username = {}, .secret = cfg.token };
    Cc::WorkerRegistrar registrar { toolchains.begin()->first, advertise, slots, { Wire::IdentityCodec } };

    // Registration and heartbeating are one loop because they are one concern: a
    // worker is registered exactly as long as it keeps saying so, and a scheduler
    // that has forgotten it answers the heartbeat by telling it to register again.
    // Splitting them would need the two halves to agree about which owns recovery.
    std::jthread const heartbeat { [&](std::stop_token const& stop) {
        while (!stop.stop_requested())
        {
            auto client = Cc::ConnectTcp(cfg.scheduler, std::chrono::milliseconds { 10'000 });
            if (client == nullptr)
                logger.Logf(LogLevel::Warn, "scheduler {} unreachable", cfg.scheduler);
            else
            {
                bool ok = !registrar.WorkerId().empty()
                          && registrar.Heartbeat(*client, static_cast<std::uint32_t>(server.InFlight()), credential);
                if (!ok)
                    ok = registrar.Register(*client, credential);
                logger.Logf(
                    ok ? LogLevel::Debug : LogLevel::Warn, "scheduler {}: {}", cfg.scheduler, ok ? "registered" : "refused");
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

    logger.Logf(LogLevel::Info,
                "compile node ready on {}:{}, advertising {}, {} slot(s), {} toolchain(s)",
                cfg.bindAddress,
                cfg.port,
                advertise,
                slots,
                toolchains.size());

    SyncRun(server.Run());

    // No deregistration is sent, and that is a decision rather than a gap. There is
    // no such verb: the scheduler learns a worker is gone by its heartbeat lapsing,
    // which is the ONE mechanism that also covers the cases a polite goodbye cannot
    // -- a killed process, a severed network, a crashed host. Adding a second path
    // would mean the fleet had two ways to believe a worker is alive and only one of
    // them exercised in the failure that matters. A client that leases this worker
    // in the gap finds it unreachable and compiles locally, which is the same
    // fallback every other refusal takes.
    logger.Logf(LogLevel::Info, "compile node stopped");
    return 0;
}
