// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <array>
#include <charconv>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// CLI spelling of a LogLevel, matching what ParseNodeLogLevel accepts.
    ///
    /// The inverse of that parser, and the pair is what makes a log level survive
    /// a round trip into a supervisor's argument list.
    /// @param level Level to name.
    /// @return Its CLI spelling.
    [[nodiscard]] constexpr std::string_view LogLevelName(LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Trace:
                return "trace";
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Error:
                return "error";
            case LogLevel::Fatal:
                return "fatal";
        }
        return "info";
    }

    /// A port, refusing 0 rather than letting a bind fail with a confusing message.
    /// @param sv Text to parse.
    /// @return The port, or why it is not one.
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
    /// @param sv Text to parse.
    /// @return The count, or why it is not one.
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
    /// @param sv Text to parse.
    /// @return The level, or why it is not one.
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

    /// The supervisor domain to register into, by name.
    /// @param sv Text to parse.
    /// @return The scope, or why it is not one.
    [[nodiscard]] std::expected<ServiceScope, ConfigError> ParseNodeServiceScope(std::string_view sv)
    {
        // The library's parser, not a second spelling of it: an operator who learned
        // `--service-scope=user` from the daemon must not find the worker accepting a
        // different vocabulary.
        return ParseServiceScope(sv);
    }
} // namespace

std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept
{
    static constexpr auto options = std::to_array<OptionSpec<NodeConfig>>({
        { .primary = "--scheduler",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignFrom<&NodeConfig::scheduler, ParseText>(),
          .description = "the scheduler's --listen-scheduler endpoint. Required: a\n"
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
          .operand = "=<compiler>|<fingerprint>=<compiler>",
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
        { .primary = "--admin-listen",
          .arity = Arity::Value,
          .operand = "=[<address>:]<port>",
          .apply = AssignFrom<&NodeConfig::adminListen, ParseText>(),
          .description = "serve /metrics and /healthz here; off unless given.\n"
                         "A bare port binds loopback: a scrape endpoint on a\n"
                         "public interface is an operator's decision, not a\n"
                         "default. /healthz is also the liveness probe this\n"
                         "worker otherwise has none of." },
        { .primary = "--listen-scheduler",
          .arity = Arity::Value,
          .operand = "=[<address>:]<port>",
          .apply = AssignFrom<&NodeConfig::schedulerListen, ParseText>(),
          .description = "serve the fleet's scheduler verbs here; off unless\n"
                         "given. Answered only while this node LEADS the\n"
                         "cluster; a follower redirects to the leader and an\n"
                         "election in progress refuses, both of which a client\n"
                         "answers by compiling locally. A bare port binds the\n"
                         "wildcard: peers have to reach it." },
        { .primary = "--fleet-member",
          .arity = Arity::Value,
          .operand = "=<host>[:<port>]",
          .apply = AppendFrom<&NodeConfig::fleetMembers, ParseText>(),
          .description = "a peer this scheduler may hand work to; repeatable.\n"
                         "Only the host is matched: a peer dials from an\n"
                         "ephemeral port, so an endpoint is not something a\n"
                         "connection can be compared against." },
        { .primary = "--fleet-open",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::fleetOpen>(),
          .description = "admit every caller to the fleet, not only\n"
                         "--fleet-member hosts. For one machine, or a network\n"
                         "that is already the boundary. Explicit because\n"
                         "'no policy' and 'admit everybody' must be the same\n"
                         "decision -- listing nobody refuses everybody." },
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
        { .primary = "--daemon",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::daemon>(),
          .description = "run in the background (POSIX), or as the service body\n"
                         "the Windows SCM starts. Supervisors that manage a\n"
                         "foreground process -- systemd and launchd -- must NOT\n"
                         "pass it: they reap a job that forks as 'exited'." },
        { .primary = "--pidfile",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::pidfile, ParseText>(),
          .description = "write the pid here when daemonizing (POSIX)" },
        { .primary = "--install-service",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::installService>(),
          .description = "register this worker with the platform's supervisor\n"
                         "(Windows SCM, macOS launchd) and exit. Every other flag\n"
                         "on this command line is baked into the registration." },
        { .primary = "--uninstall-service",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::uninstallService>(),
          .description = "remove that registration and exit" },
        { .primary = "--service-name",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&NodeConfig::serviceName, ParseText>(),
          .description = "name the supervisor keys the registration on\n"
                         "(default FastCacheCompileNode). Distinct from the\n"
                         "daemon's by default: a machine may run both, and one\n"
                         "name would make installing either displace the other." },
        { .primary = "--service-scope",
          .arity = Arity::Value,
          .operand = "=<user|system>",
          .apply = AssignFrom<&NodeConfig::serviceScope, ParseNodeServiceScope>(),
          .description = "which supervisor domain to register in (default system).\n"
                         "Ignored on Windows, which has only one." },
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

ServiceSpec MakeNodeServiceSpec(std::filesystem::path const& exePath, NodeConfig const& cfg)
{
    NodeConfig const defaults {};
    std::vector<std::string> argv;

    /// Emit `--flag=value` when the field differs from its default.
    auto const emitIfSet = [&argv](std::string_view flag, auto const& value, auto const& fallback) {
        if (value != fallback)
            argv.emplace_back(std::format("--{}={}", flag, value));
    };

    /// Emit a path flag, made absolute.
    ///
    /// A service does not inherit the installing shell's working directory, so a
    /// relative path captured at install time resolves somewhere else at start --
    /// which for a pidfile means a supervisor that cannot find its own process.
    auto const emitPathIfSet = [&argv](std::string_view flag, std::string const& value) {
        if (value.empty())
            return;
        std::error_code ec;
        auto const absolute = std::filesystem::absolute(value, ec);
        argv.emplace_back(std::format("--{}={}", flag, ec ? value : absolute.string()));
    };

    // Unconditional: it is what the running service identifies itself by, and on
    // launchd it is what the job label derives from.
    argv.push_back(std::format("--service-name={}", cfg.serviceName));

    emitIfSet("scheduler", cfg.scheduler, defaults.scheduler);
    emitIfSet("advertise", cfg.advertise, defaults.advertise);
    emitIfSet("bind", cfg.bindAddress, defaults.bindAddress);
    emitIfSet("port", cfg.port, defaults.port);
    emitIfSet("slots", cfg.slots, defaults.slots);
    emitIfSet("admin-listen", cfg.adminListen, defaults.adminListen);
    emitIfSet("listen-scheduler", cfg.schedulerListen, defaults.schedulerListen);
    if (cfg.fleetOpen)
        argv.emplace_back("--fleet-open");
    for (auto const& member: cfg.fleetMembers)
        argv.push_back(std::format("--fleet-member={}", member));
    emitIfSet("log-level", LogLevelName(cfg.logLevel), LogLevelName(defaults.logLevel));
    emitPathIfSet("pidfile", cfg.pidfile);

    // Repeatable, so one token per toolchain rather than one joined value: a
    // worker that came back serving fewer compilers than it was installed with
    // would present as a fleet that stopped matching, not as a packaging bug.
    //
    // A bare compiler path is left as the operator wrote it. It is NOT resolved
    // to a fingerprint here, deliberately: the worker derives that at startup
    // through the identical code its clients use, and baking a digest computed
    // at install time would pin the registration to a toolchain that an update
    // then changes underneath it.
    for (auto const& toolchain: cfg.toolchains)
        argv.push_back(std::format("--toolchain={}", toolchain));

    std::vector<std::filesystem::path> owned;

    return ServiceSpec { .serviceName = cfg.serviceName,
                         .exePath = exePath,
                         .arguments = std::move(argv),
                         .daemonFlag = "--daemon",
                         .displayName = "fastcache-compile-node",
                         .description = "fastcache-compile-node \u2014 a compile worker for fastcached",
                         // Named rather than left empty, and the difference is not
                         // cosmetic: a system-scope launchd job with no UserName runs
                         // as ROOT, and this process compiles input that arrived over
                         // the network. Naming the account the Linux unit already uses
                         // (packaging/linux/fastcache-compile-node.sysusers) puts the
                         // existing "that account does not exist" guard in the way, so
                         // a macOS system-scope install REFUSES until the package
                         // creates it -- rather than silently succeeding as root.
                         // `--service-scope=user` works today and is the per-developer
                         // case anyway: a user agent runs as the invoking account.
                         .serviceAccount = "fastcache-node",
                         .ownedDirectories = std::move(owned),
                         .inlineCredential = cfg.token.empty() ? InlineCredential::Absent : InlineCredential::Present,
                         .configPath = {} };
}

std::optional<std::string> NodeServiceRejection(NodeConfig const& cfg)
{
    // A table, so a new rule is a new row rather than another `if` in main().
    struct Rule
    {
        bool (*refuses)(NodeConfig const&); ///< Whether this rule objects.
        std::string_view message;           ///< What the operator is told, with the remedy.
    };

    constexpr auto Rules = std::to_array<Rule>({
        { .refuses = [](NodeConfig const& c) { return c.scheduler.empty(); },
          .message = "--scheduler is required to install a service: a worker nothing knows about serves nobody, "
                     "and the registration would start and immediately exit at every boot." },
        { .refuses = [](NodeConfig const& c) { return c.toolchains.empty(); },
          .message = "--toolchain is required to install a service: a worker with none registers and then refuses "
                     "every job the scheduler sends it." },
        { .refuses = [](NodeConfig const& c) { return c.advertise.empty(); },
          .message = "--advertise is required to install a service: without it the registration bakes in "
                     "{--bind}:{--port}, and the default 0.0.0.0 is not an address a client can dial. Such a worker "
                     "registers, heartbeats, is leased out, and is never reached -- with no error at either end." },
    });

    for (auto const& rule: Rules)
        if (rule.refuses(cfg))
            return std::string { rule.message };

    return std::nullopt;
}

std::optional<std::string> SchedulerPolicyRejection(NodeConfig const& cfg)
{
    // Separate from NodeServiceRejection because it is a *startup* rule rather than
    // an install-time one: this misconfiguration is fatal every time the process
    // runs, not only when a registration is written, and gating it on
    // --install-service would let a hand-started scheduler make the same mistake.
    struct Rule
    {
        bool (*refuses)(NodeConfig const&); ///< Whether this rule objects.
        std::string_view message;           ///< What the operator is told, with the remedy.
    };

    constexpr auto Rules = std::to_array<Rule>({
        { .refuses =
              [](NodeConfig const& c) { return !c.schedulerListen.empty() && !c.fleetOpen && c.fleetMembers.empty(); },
          .message = "--listen-scheduler needs --fleet-member or --fleet-open: a scheduler with an empty member set "
                     "refuses every caller, which is the right default but not a working configuration. It would "
                     "start, bind, log nothing wrong, and decline the whole fleet." },
        { .refuses = [](NodeConfig const& c) { return c.fleetOpen && !c.fleetMembers.empty(); },
          .message = "--fleet-open and --fleet-member contradict each other: one admits everybody and the other "
                     "admits a list. Silently preferring either would make the narrower of the two a no-op an "
                     "operator believes is in force." },
        { .refuses =
              [](NodeConfig const& c) { return c.schedulerListen.empty() && (c.fleetOpen || !c.fleetMembers.empty()); },
          .message = "--fleet-member and --fleet-open describe who this node's scheduler admits, and it is not "
                     "running one: add --listen-scheduler, or drop them. A policy nothing consults is a policy an "
                     "operator believes is in force." },
    });

    for (auto const& rule: Rules)
        if (rule.refuses(cfg))
            return std::string { rule.message };

    return std::nullopt;
}

std::string HelpText(UsageColor color)
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

} // namespace FastCache::Node
