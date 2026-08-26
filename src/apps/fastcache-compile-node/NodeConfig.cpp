// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/ByteSize.hpp>
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

    /// A node class, by the name `NodeClassTable` spells it.
    ///
    /// Off the shared table rather than a two-arm `if`, so a class added there is
    /// accepted here without an edit -- and so the spelling an operator types is
    /// necessarily the spelling the table documents.
    /// @param sv Text to parse.
    /// @return The class, or why it is not one.
    [[nodiscard]] std::expected<Distributed::NodeClass, ConfigError> ParseNodeClass(std::string_view sv)
    {
        if (auto const parsed = Distributed::NodeClassByName(sv); parsed.has_value())
            return *parsed;

        // Names the accepted spellings, because a rejection that cannot say what
        // would have worked cannot be acted on -- the same reason the wire's
        // `UnsupportedVersion` names its supported range.
        std::string accepted;
        for (auto const& row: Distributed::NodeClassTable)
        {
            if (!accepted.empty())
                accepted += ", ";
            accepted += row.name;
        }
        return std::unexpected(ArgvError(
            ConfigErrorCode::OutOfRange, "node-class", std::format("not a node class: {} (expected {})", sv, accepted)));
    }

    /// A core reserve, which may legitimately be zero.
    ///
    /// Unlike `ParseSlots`, zero is accepted: "reserve nothing" is a real answer an
    /// operator gives for a machine they are not sitting at, and it is a *different*
    /// answer from not passing the flag at all -- which is why the field holding it
    /// is an optional.
    /// @param sv Text to parse.
    /// @return The count, or why it is not one.
    [[nodiscard]] std::expected<std::optional<std::uint32_t>, ConfigError> ParseReservedCores(std::string_view sv)
    {
        auto value = 0U;
        auto const* const begin = sv.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::unexpected(
                ArgvError(ConfigErrorCode::OutOfRange, "reserve-cores", std::format("not a core count: {}", sv)));
        return std::optional<std::uint32_t> { value };
    }

    /// An applier for a flag that names a cluster action AND carries its operand.
    ///
    /// Neither `SelectOutcome` nor `AssignFrom` covers this on its own: one sets
    /// the action and the other the operand, and a row using either would leave
    /// the other half to a second row nobody would remember to add. `Status` takes
    /// no operand at all, which is why the operand handling is a compile-time
    /// branch rather than a runtime one -- a `--cluster-status` row that quietly
    /// stored an empty key would be a row whose arity and whose applier disagreed.
    /// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
    template <ClusterAction Action>
    [[nodiscard]] constexpr auto SelectClusterAction() noexcept
    {
        return [](auto& result, std::string_view value) -> std::expected<void, ConfigError> {
            auto& request = TargetOf<&NodeConfig::cluster>(result);
            request.action = Action;

            if constexpr (Action == ClusterAction::Set)
            {
                auto assignment = ParseSettingAssignment(value);
                if (!assignment.has_value())
                    return std::unexpected(
                        ArgvError(ConfigErrorCode::ParseError, "cluster-set", std::format("not <name>=<value>: {}", value)));
                request.key = std::move(assignment->first);
                request.value = std::move(assignment->second);
            }
            else if constexpr (Action == ClusterAction::Forget)
            {
                if (value.empty())
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError, "cluster-forget", "names no member"));
                request.key = std::string { value };
            }
            else if constexpr (Action == ClusterAction::Admit)
            {
                // The same grammar `--raft-peer` takes, through the same function:
                // an operator adding a member types the token they would have put in
                // that flag, the documentation tells them so, and a second
                // implementation would be two flags accepting different token sets
                // for one concept -- with only one of them being what the transport
                // actually dials.
                auto member = Cluster::ParseMemberSpec(value);
                if (!member.has_value())
                    return std::unexpected(ArgvError(
                        ConfigErrorCode::ParseError, "cluster-admit", std::format("not <id>=<host>:<port>: {}", value)));

                request.key = std::move(member->id);
                request.value = std::move(member->raftEndpoint);
            }

            return {};
        };
    }

    /// A byte count for the local cache tier, accepting the k/m/g suffixes the
    /// daemon's own size flags do.
    ///
    /// Reuses `ParseByteSize` rather than a second parser: an operator who has
    /// written `--storage-max-value=64m` for the daemon must not discover that this
    /// flag spells sizes differently, and two grammars for one concept is the
    /// table-shaped defect this codebase keeps a list about.
    /// @param sv Text to parse.
    /// @return The size in bytes, or why it is not one.
    [[nodiscard]] std::expected<std::uint64_t, ConfigError> ParseCacheBytes(std::string_view sv)
    {
        auto const parsed = ParseByteSize(sv, "cache-memory");
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        return static_cast<std::uint64_t>(*parsed);
    }

    /// The on-disk tier's byte budget, in the same grammar as `--cache-memory`.
    /// @param sv Text to parse.
    /// @return The size in bytes, or why it is not one.
    [[nodiscard]] std::expected<std::uint64_t, ConfigError> ParseCacheDiskBytes(std::string_view sv)
    {
        auto const parsed = ParseByteSize(sv, "cache-disk");
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        return static_cast<std::uint64_t>(*parsed);
    }

    /// A filesystem path, taken as written.
    /// @param sv Text to parse.
    /// @return The path.
    [[nodiscard]] std::expected<std::filesystem::path, ConfigError> ParsePathValue(std::string_view sv)
    {
        return std::filesystem::path { sv };
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

std::optional<std::pair<std::string, std::string>> ParseSettingAssignment(std::string_view text)
{
    auto const split = text.find('=');
    if (split == std::string_view::npos)
        return std::nullopt;

    auto name = std::string { text.substr(0, split) };
    if (name.empty())
        return std::nullopt;

    return std::pair { std::move(name), std::string { text.substr(split + 1) } };
}

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
          .description = "concurrent compiles. Default: derived from this\n"
                         "machine's cores and memory, less what --node-class\n"
                         "reserves. A number given here is the answer and is\n"
                         "not clamped or reduced further. Advertised to the\n"
                         "scheduler AND enforced here: a worker that accepted\n"
                         "more would be fuller and slower than the scheduler\n"
                         "believes, at the same moment." },
        { .primary = "--node-class",
          .arity = Arity::Value,
          .operand = "=workstation|dedicated",
          .apply = AssignFrom<&NodeConfig::nodeClass, ParseNodeClass>(),
          .description = "how hard this machine may be driven (default:\n"
                         "workstation). A workstation keeps cores free for the\n"
                         "person using it; a dedicated node may be driven to its\n"
                         "slot limit. The default is the safe answer rather than\n"
                         "the common one." },
        { .primary = "--reserve-cores",
          .arity = Arity::Value,
          .operand = "=<n>",
          .apply = AssignFrom<&NodeConfig::reservedCores, ParseReservedCores>(),
          .description = "cores never offered to the fleet, overriding what the\n"
                         "node class reserves. 0 is a real answer and is not the\n"
                         "same as omitting the flag. Ignored when --slots names\n"
                         "a number, which is the operator's answer already." },
        { .primary = "--node-id",
          .arity = Arity::Value,
          .operand = "=<id>",
          .apply = AssignFrom<&NodeConfig::nodeId, ParseText>(),
          .description = "this node's identity in the cluster. Giving it turns\n"
                         "consensus ON; without it this node leads alone,\n"
                         "which is right for one machine and is the default." },
        { .primary = "--listen-raft",
          .arity = Arity::Value,
          .operand = "=[<address>:]<port>",
          .apply = AssignFrom<&NodeConfig::raftListen, ParseText>(),
          .description = "where peers reach this node's consensus port. A bare\n"
                         "port binds the WILDCARD: peers are on other machines\n"
                         "by definition, so loopback would silently not work." },
        { .primary = "--raft-peer",
          .arity = Arity::Value,
          .operand = "=<id>=<host>:<port>",
          .apply = AppendFrom<&NodeConfig::raftPeers, ParseText>(),
          .description = "a cluster member and where it answers; repeatable.\n"
                         "Both halves in one token because a member id with\n"
                         "no address is a node counted towards quorum and\n"
                         "unreachable. This is the BOOTSTRAP set only:\n"
                         "membership is replicated once the cluster runs." },
        { .primary = "--raft-join",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::raftJoin>(),
          .description = "start with NO cluster and wait to be admitted to one.\n"
                         "--raft-peer then lists nodes this one can REACH rather\n"
                         "than a cluster it belongs to -- itself, and whoever\n"
                         "will admit it, because a joiner has to be able to\n"
                         "answer the leader before it can learn where that\n"
                         "leader is. Without this flag a node bootstraps a\n"
                         "cluster of itself, elects itself, and can never\n"
                         "afterwards be admitted to anybody else's." },
        { .primary = "--cluster-dir",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::clusterDir, ParsePathValue>(),
          .description = "where consensus keeps its durable state. A node\n"
                         "that answered a vote and forgot it votes twice in\n"
                         "one term after a restart, which is two leaders." },
        { .primary = "--cluster-status",
          .arity = Arity::None,
          .apply = SelectClusterAction<ClusterAction::Status>(),
          .description = "ask the cluster at --scheduler what it has agreed,\n"
                         "print it and exit. Answered by the LEADER; a\n"
                         "follower says who to ask instead." },
        { .primary = "--cluster-set",
          .arity = Arity::Value,
          .operand = "=<name>=<value>",
          .apply = SelectClusterAction<ClusterAction::Set>(),
          .description = "change one replicated cluster setting and exit.\n"
                         "Every member then agrees on it, and it survives\n"
                         "their restarts. --cluster-status lists the keys." },
        { .primary = "--cluster-admit",
          .arity = Arity::Value,
          .operand = "=<id>=<host>:<port>",
          .apply = SelectClusterAction<ClusterAction::Admit>(),
          .description = "add a member to the cluster and exit, or record that\n"
                         "one has moved. Both halves in one token for the\n"
                         "reason --raft-peer takes both: an id with no address\n"
                         "is counted towards quorum and never reached. The\n"
                         "member itself must have been started with\n"
                         "--raft-join." },
        { .primary = "--cluster-forget",
          .arity = Arity::Value,
          .operand = "=<node-id>",
          .apply = SelectClusterAction<ClusterAction::Forget>(),
          .description = "remove a member from the cluster and exit. The one\n"
                         "membership change nothing automatic makes:\n"
                         "discovery only ever adds, because a peer goes\n"
                         "quiet far more often than it leaves." },
        { .primary = "--cluster-id",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&NodeConfig::clusterId, ParseText>(),
          .description = "which fleet this node belongs to. Plain text in every\n"
                         "beacon and NOT a credential: what it buys is that two\n"
                         "unrelated fleets on one segment ignore each other." },
        { .primary = "--discovery",
          .arity = Arity::Value,
          .operand = "=<address>:<port>",
          .apply = AssignFrom<&NodeConfig::discoveryAddress, ParseText>(),
          .description = "announce this node on the segment and listen for peers\n"
                         "here; off unless given. Needs --node-id and\n"
                         "--cluster-key-file. Without it a cluster is exactly\n"
                         "the --raft-peer list an operator typed." },
        { .primary = "--cluster-key-file",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::clusterKeyFile, ParsePathValue>(),
          .description = "the cluster's pre-shared key. A FILE and not a flag:\n"
                         "a command line is readable through ps, and a key that\n"
                         "leaks admits a node whose objects the whole fleet\n"
                         "then caches." },
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
        { .primary = "--cache-memory",
          .arity = Arity::Value,
          .operand = "=<bytes>",
          .apply = AssignFrom<&NodeConfig::cacheMemoryBytes, ParseCacheBytes>(),
          .description = "size of this node's own in-memory cache tier\n"
                         "(default 256m; 0 turns it off). It exists so a\n"
                         "local rebuild on a slow or bad network never\n"
                         "reaches the wire at all." },
        { .primary = "--cache-disk",
          .arity = Arity::Value,
          .operand = "=<bytes>",
          .apply = AssignFrom<&NodeConfig::cacheDiskBytes, ParseCacheDiskBytes>(),
          .description = "cap this node's on-disk cache tier at this size\n"
                         "(default 0, meaning grow as needed). Only means\n"
                         "anything with --cache-dir: without a path there is\n"
                         "no disk tier for a budget to bound." },
        { .primary = "--cache-dir",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::cacheDir, ParsePathValue>(),
          .description = "back the local cache tier with disk at this path.\n"
                         "Memory-only otherwise: a disk tier is a resource an\n"
                         "operator should have to name. ONE node per path:\n"
                         "the store takes no inter-process lock, so two\n"
                         "nodes sharing it corrupt it." },
        { .primary = "--listen-cache",
          .arity = Arity::Value,
          .operand = "=[<address>:]<port>",
          .apply = AssignFrom<&NodeConfig::cacheListen, ParseText>(),
          .description = "serve cache verbs to local clients here (default\n"
                         "127.0.0.1:6674, where fastcache-cc already looks;\n"
                         "empty turns it off). A bare port binds LOOPBACK,\n"
                         "unlike --listen-scheduler: a cache any host can\n"
                         "dial is this machine's whole build output served\n"
                         "to strangers. Widen it and only this machine and\n"
                         "--fleet-member peers are still admitted." },
        { .primary = "--upstream",
          .arity = Arity::Value,
          .operand = "=<host:port>",
          .apply = AssignFrom<&NodeConfig::upstream, ParseText>(),
          .description = "the shared fastcached this node reads through to.\n"
                         "Empty is honest rather than broken: one developer's\n"
                         "machine has no shared cache." },
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

Distributed::NodeCapacity NodeCapacityOf(NodeConfig const& cfg, IHostFactsSource const& host)
{
    return Distributed::NodeCapacity { .logicalCores = host.LogicalCores(),
                                       .totalMemoryBytes = host.TotalMemoryBytes(),
                                       .nodeClass = cfg.nodeClass,
                                       // Absent is not zero, and this is the one line
                                       // where the two are told apart: a reserve the
                                       // operator typed as 0 means "drive this machine
                                       // to its last core", while one they never
                                       // mentioned means "use whatever the class
                                       // reserves". Collapsing them makes somebody's
                                       // desktop unusable in one direction and wastes
                                       // a build server in the other.
                                       .reservedCores = cfg.reservedCores.value_or(0),
                                       .reserveIsExplicit = cfg.reservedCores.has_value() };
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
    emitIfSet("node-class",
              std::string { Distributed::TraitsFor(cfg.nodeClass).name },
              std::string { Distributed::TraitsFor(defaults.nodeClass).name });
    // Emitted on presence rather than on difference, because the difference this
    // flag carries IS presence: a reserve of zero the operator typed and a reserve
    // nobody mentioned are different instructions, and `emitIfSet` compares values.
    if (cfg.reservedCores.has_value())
        argv.push_back(std::format("--reserve-cores={}", *cfg.reservedCores));
    emitIfSet("admin-listen", cfg.adminListen, defaults.adminListen);
    emitIfSet("listen-scheduler", cfg.schedulerListen, defaults.schedulerListen);
    emitIfSet("cache-memory", cfg.cacheMemoryBytes, defaults.cacheMemoryBytes);
    emitIfSet("cache-disk", cfg.cacheDiskBytes, defaults.cacheDiskBytes);
    emitIfSet("listen-cache", cfg.cacheListen, defaults.cacheListen);
    emitIfSet("node-id", cfg.nodeId, defaults.nodeId);
    emitIfSet("listen-raft", cfg.raftListen, defaults.raftListen);
    emitPathIfSet("cluster-dir", cfg.clusterDir.string());
    emitIfSet("cluster-id", cfg.clusterId, defaults.clusterId);
    emitIfSet("discovery", cfg.discoveryAddress, defaults.discoveryAddress);
    emitPathIfSet("cluster-key-file", cfg.clusterKeyFile.string());
    // Repeatable, so one token per peer rather than one joined value -- for the
    // reason the toolchains are: a service that came back knowing fewer members than
    // it was installed with would present as a cluster that stopped forming quorum,
    // not as a packaging bug.
    for (auto const& peer: cfg.raftPeers)
        argv.push_back(std::format("--raft-peer={}", peer));
    emitIfSet("upstream", cfg.upstream, defaults.upstream);
    emitPathIfSet("cache-dir", cfg.cacheDir.string());
    if (cfg.raftJoin)
        argv.emplace_back("--raft-join");
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

    // Directories root will create for an account that is not root. Without the
    // handover the worker's first write fails with EACCES, which launchd surfaces
    // only as a job that exits over and over -- the same reason the daemon hands
    // over its --storage.
    //
    // Only what the operator actually named, never a parent: `--cache-dir=/var/db/fc`
    // must not reassign /var/db, shared with other system services, to an
    // unprivileged compile account.
    std::vector<std::filesystem::path> owned;
    for (auto const& directory: { cfg.cacheDir, cfg.clusterDir })
        if (!directory.empty())
            owned.emplace_back(directory);

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
                         .ownedPaths = std::move(owned),
                         .inlineCredential = cfg.token.empty() ? InlineCredential::Absent : InlineCredential::Present,
                         .configPath = {},
                         // Empty, and load-bearing: this worker is configured
                         // entirely from argv and NodeOptions() has neither
                         // `--config` nor `--storage`. Naming an application here
                         // would invite WithScopeDefaults to bake one in, and the
                         // registration would then be a job that answers its own
                         // command line with "unrecognised argument" at every
                         // start -- reported installed, dead at every boot.
                         .applicationName = {},
                         // The Windows half of the same decision `serviceAccount`
                         // makes for launchd. Told nothing, the SCM logs a service
                         // on as LocalSystem -- the whole machine -- and this one
                         // compiles input that arrived over the network. A virtual
                         // account gives it a per-service SID, no group membership
                         // and no machine credentials on the network, and the SCM
                         // creates it from the service name with no account for the
                         // installer to make and no password to keep.
                         .windowsLogon = WindowsLogonAccount::VirtualAccount };
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
        { .refuses = [](NodeConfig const& c) { return !c.raftListen.empty() && c.clusterDir.empty(); },
          .message = "--listen-raft needs --cluster-dir to install a service: consensus state otherwise lands in "
                     "`fastcache-cluster/<node-id>` relative to the working directory, and a service does not "
                     "inherit the installing shell's. It resolves under C:\\Windows\\System32 for the SCM and under "
                     "/ for launchd -- writable only by the very privileges a worker is deliberately not given, so "
                     "the job registers and then fails to open its own state at every start." },
    });

    for (auto const& rule: Rules)
        if (rule.refuses(cfg))
            return std::string { rule.message };

    return std::nullopt;
}

std::optional<std::string> StartupPolicyRejection(NodeConfig const& cfg)
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
        { .refuses = [](NodeConfig const& c) { return c.raftJoin && c.nodeId.empty(); },
          .message = "--raft-join needs --node-id: a node waiting to be admitted to a cluster still has to have an "
                     "identity, because that is what the cluster admits and what every vote is counted against. "
                     "Without one it would listen forever and could never be named." },
        { .refuses = [](NodeConfig const& c) { return c.raftJoin && c.raftPeers.empty(); },
          .message = "--raft-join needs --raft-peer: at least this node's own address, which is the half only it "
                     "knows, and normally the cluster's as well. A joiner cannot answer the leader that admits it "
                     "without one, and it cannot learn any address until it has answered." },
        { .refuses = [](NodeConfig const& c) { return !c.discoveryAddress.empty() && c.nodeId.empty(); },
          .message = "--discovery needs --node-id: discovery finds peers for a CLUSTER, and without an id this "
                     "node is not in one. It would broadcast, be answered, prove the key and have nowhere to "
                     "put the answer." },
        { .refuses = [](NodeConfig const& c) { return !c.discoveryAddress.empty() && c.clusterKeyFile.empty(); },
          .message = "--discovery needs --cluster-key-file: a beacon is unauthenticated by construction, so the "
                     "key is the only thing separating a peer from anything else on the segment. With none, no "
                     "peer can ever be admitted and this node would announce itself forever to no effect." },
        { .refuses = [](NodeConfig const& c) { return !c.clusterKeyFile.empty() && c.discoveryAddress.empty(); },
          .message = "--cluster-key-file is read by discovery and nothing else, and --discovery is not set. A "
                     "secret an operator went to the trouble of provisioning, being read by nobody, is exactly "
                     "the silent no-op this list exists to refuse." },
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
