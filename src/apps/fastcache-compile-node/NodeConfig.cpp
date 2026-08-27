// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
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
        // Host total passed, so `N%` parses -- which is the vocabulary this flag's
        // own default is stated in. A default an operator cannot spell is one they
        // cannot adjust by a little: without this, "a quarter of RAM, but half"
        // means working out the bytes for every machine by hand.
        auto const parsed = ParseByteSize(sv, "cache-memory", QueryHostTotalMemoryBytes());
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        return static_cast<std::uint64_t>(*parsed);
    }

    /// The on-disk tier's byte budget.
    ///
    /// `k`/`m`/`g` as `--cache-memory` takes them, but **not** its `N%`: that share
    /// is of host RAM, and a disk budget expressed as a fraction of memory would be
    /// a number with no meaning on any machine whose disk is not its RAM.
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

    /// Memory this node's cache tiers hold, and so cannot lend to a compile.
    ///
    /// A fold over `StorageTierTable`'s `budgetIsResidentMemory` column rather than a
    /// look at the memory tier by name: the taxonomy is open and enumerators are
    /// appended, so a resident tier added later reaches this arithmetic by being a
    /// row. Naming `StorageTier::Memory` here would ignore it, and under-counting is
    /// the direction that over-commits the machine.
    ///
    /// Only budgets that are *denominated in* RAM are summed, which is what that
    /// column says. A disk tier's own in-memory key index is real and is not covered
    /// here (#175) -- but adding its DISK budget to a memory total would be wrong by
    /// the ratio between the two, not right by accident.
    ///
    /// Total over the vocabulary rather than correct by distant assumption, which is
    /// the difference between the two spellings of "nothing to add":
    ///
    ///   - **Absent** is a tier this node does not run, and contributes nothing.
    ///   - **Present and zero** is a tier with no ceiling -- the same UNBOUNDED that
    ///     `--cache-memory 0` once meant to `InMemoryLruStorage` -- so a resident one
    ///     may take the whole machine and is reserved as such. `BuildStorage` does
    ///     not build the memory half that way today, but reading it as "reserve
    ///     nothing" would be the exact inverse of what it says, and that number's two
    ///     meanings have bitten this codebase before.
    /// @param cache What this node's tiers actually hold.
    /// @param totalMemoryBytes The machine's RAM, which an unbounded tier may take all of.
    /// @return Bytes held back from compiles.
    [[nodiscard]] std::uint64_t ResidentCacheBytes(Distributed::NodeCacheCapacity const& cache,
                                                   std::uint64_t totalMemoryBytes) noexcept
    {
        std::uint64_t reserved = 0;
        for (auto const& row: StorageTierTable)
        {
            if (!row.budgetIsResidentMemory)
                continue;

            auto const& budget = cache.tierBytesLimit[static_cast<std::size_t>(row.tier)];
            if (!budget.has_value())
                continue;
            if (*budget == 0)
                return totalMemoryBytes;

            // Saturating rather than wrapping, for the reason `OfferableSlots`'s core
            // reserve is: budgets that summed past 64 bits would otherwise come back
            // as a SMALL reservation, which is the over-commit direction again.
            if (*budget > std::numeric_limits<std::uint64_t>::max() - reserved)
                return std::numeric_limits<std::uint64_t>::max();
            reserved += *budget;
        }
        return reserved;
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
          .description = "a toolchain this worker serves; repeatable. An OVERRIDE:\n"
                         "naming any pins this worker to exactly that set, and\n"
                         "naming none means serve whatever this machine has.\n"
                         "There is still no default COMPILER -- a default is how a\n"
                         "job ends up running against something nobody chose." },
        { .primary = "--no-toolchain-discovery",
          .arity = Arity::None,
          .apply = SetFalse<&NodeConfig::toolchainDiscovery>(),
          .description = "do not survey this machine for compilers. Without\n"
                         "--toolchain this leaves the worker with nothing to\n"
                         "serve, so it refuses to start -- and refuses to be\n"
                         "INSTALLED as a service, which is the registration that\n"
                         "would otherwise fail at every boot with nobody watching." },
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
        { .primary = "--discovery-reply-port",
          .arity = Arity::Value,
          .operand = "=<n>",
          .apply = AssignFrom<&NodeConfig::discoveryReplyPort, ParseNodePort>(),
          .description = "port peers unicast their discovery challenges and\n"
                         "proofs to; kernel-chosen unless given. NOT the\n"
                         "--discovery port: that one is shared by every node on\n"
                         "the segment, and only one socket sharing a port is\n"
                         "handed a unicast. Pin it where a host firewall opens\n"
                         "named ports only -- one per node on the machine." },
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
        { .primary = "--dashboard",
          .apply = SetTrue<&NodeConfig::dashboard>(),
          .description = "also serve the fleet dashboard on --admin-listen, at\n"
                         "/fleet and /fleet.json. Off unless given: the page is a\n"
                         "map of every member's hostname, endpoint and capacity.\n"
                         "Answered in full only while this node LEADS; anyone else\n"
                         "names the leader rather than showing half a fleet." },
        { .primary = "--dashboard-token-file",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::dashboardTokenFile, ParsePathValue>(),
          .description = "credential the dashboard requires, as Basic or Bearer.\n"
                         "A FILE and not a flag: a command line is readable\n"
                         "through ps. Its own secret rather than --requirepass,\n"
                         "which every member of the fleet already holds. Required\n"
                         "when --admin-listen is not on loopback." },
        { .primary = "--tls-self-signed",
          .apply = SetTrue<&NodeConfig::tlsSelfSigned>(),
          .description = "generate a self-signed certificate at startup and serve\n"
                         "the admin surface over HTTPS with it, so an internal\n"
                         "deployment needs no certificate to obtain. Encrypts the\n"
                         "traffic; it does NOT prove which node answered, so the\n"
                         "fingerprint is logged for you to compare. Regenerated\n"
                         "every restart -- name --tls-cert for a stable identity." },
        { .primary = "--tls-cert",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::tlsCertFile, ParsePathValue>(),
          .description = "serve the admin surface over HTTPS with this\n"
                         "certificate. TLS is on by naming a certificate and a\n"
                         "key rather than by a flag, so there is no way to ask\n"
                         "for it without the material to do it." },
        { .primary = "--tls-key",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::tlsKeyFile, ParsePathValue>(),
          .description = "private key for --tls-cert. Both or neither." },
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
          .operand = "=<size>",
          .apply = AssignFrom<&NodeConfig::cacheMemoryBytes, ParseCacheBytes>(),
          .explicitBit = &NodeConfig::cacheMemoryExplicit,
          .description = "size of this node's own in-memory cache tier;\n"
                         "k/m/g = KiB/MiB/GiB or N% of host RAM. Defaults\n"
                         "to 25% of RAM within [512m, 8g]; 0 turns it off.\n"
                         "It exists so a local rebuild on a slow or bad\n"
                         "network never reaches the wire at all." },
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
                         "operator should have to name. ONE node per path,\n"
                         "enforced: the store is claimed exclusively, so a\n"
                         "second node sharing it refuses to start." },
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
        { .primary = "--migrate-cache",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::migrateCache>(),
          .description = "convert the --cache-dir store to this build's on-disk\n"
                         "record layout and exit, instead of serving. Run it\n"
                         "with the worker STOPPED. Safe to re-run: a store\n"
                         "already in this layout is left untouched, and a run\n"
                         "that is interrupted resumes where it stopped" },
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

Distributed::NodeCapacity NodeCapacityOf(NodeConfig const& cfg,
                                         IHostFactsSource const& host,
                                         Distributed::NodeCacheCapacity const& cache)
{
    return Distributed::NodeCapacity { .logicalCores = host.LogicalCores(),
                                       .totalMemoryBytes = host.TotalMemoryBytes(),
                                       .reservedMemoryBytes = ResidentCacheBytes(cache, host.TotalMemoryBytes()),
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
                                       .reserveIsExplicit = cfg.reservedCores.has_value(),
                                       // The same record the reservation was derived
                                       // from, carried on to the leader -- so what
                                       // this node holds back and what the fleet is
                                       // shown cannot drift apart at this end.
                                       .cache = cache };
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
    if (cfg.dashboard)
        argv.emplace_back("--dashboard");
    // The PATH, never the secret it holds -- the same rule `--requirepass` is
    // refused outright by, one step less strict because a path is not a credential.
    emitPathIfSet("dashboard-token-file", cfg.dashboardTokenFile.string());
    if (cfg.tlsSelfSigned)
        argv.emplace_back("--tls-self-signed");
    emitPathIfSet("tls-cert", cfg.tlsCertFile.string());
    emitPathIfSet("tls-key", cfg.tlsKeyFile.string());
    emitIfSet("listen-scheduler", cfg.schedulerListen, defaults.schedulerListen);
    // On whether they SAID it, not on whether it differs. This default is a share of
    // host RAM, so the value an operator reads off the startup line and types back to
    // pin it is the one value `emitIfSet` would drop -- leaving the service to
    // re-derive from RAM at every start, and the budget to move under a VM resize.
    if (cfg.cacheMemoryExplicit)
        argv.emplace_back(std::format("--cache-memory={}", cfg.cacheMemoryBytes));
    emitIfSet("cache-disk", cfg.cacheDiskBytes, defaults.cacheDiskBytes);
    emitIfSet("listen-cache", cfg.cacheListen, defaults.cacheListen);
    emitIfSet("node-id", cfg.nodeId, defaults.nodeId);
    emitIfSet("listen-raft", cfg.raftListen, defaults.raftListen);
    emitPathIfSet("cluster-dir", cfg.clusterDir.string());
    emitIfSet("cluster-id", cfg.clusterId, defaults.clusterId);
    emitIfSet("discovery", cfg.discoveryAddress, defaults.discoveryAddress);
    emitIfSet("discovery-reply-port", cfg.discoveryReplyPort, defaults.discoveryReplyPort);
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

    // Carried into the registration, because it changes what the service DOES at
    // every boot. A node installed with discovery off and no toolchain is refused
    // below; one installed with it off and a toolchain named must come back with it
    // still off, or the service quietly starts serving compilers the operator
    // deliberately excluded.
    if (!cfg.toolchainDiscovery)
        argv.emplace_back("--no-toolchain-discovery");

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
        // Conditional, where it used to be absolute. Registering a service before
        // anybody knows what the machine holds is the entire point of #139 -- the
        // node answers that at boot. What still cannot work is discovery turned OFF
        // with nothing named, and that is refused here, where an operator is
        // watching, rather than at every boot where nobody is.
        { .refuses = [](NodeConfig const& c) { return c.toolchains.empty() && !c.toolchainDiscovery; },
          .message = "--toolchain is required alongside --no-toolchain-discovery: with both, a worker has nothing to "
                     "serve, so it would register and then refuse every job the scheduler sends it. Drop "
                     "--no-toolchain-discovery to let the machine answer at boot instead." },
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
        { .refuses = [](NodeConfig const& c) { return c.discoveryReplyPort != 0 && c.discoveryAddress.empty(); },
          .message = "--discovery-reply-port is where discovery is ANSWERED, and --discovery is not set. A port "
                     "pinned for a service that is off is a port nothing will ever bind, so this is a typo or a "
                     "half-finished configuration rather than an instruction." },
        { .refuses =
              [](NodeConfig const& c) {
                  if (c.discoveryReplyPort == 0)
                      return false;
                  auto const beacon = SplitHostPort(c.discoveryAddress);
                  return beacon.has_value() && ParseTcpPort(beacon->second) == c.discoveryReplyPort;
              },
          .message = "--discovery-reply-port names the --discovery port, and they are the two halves this node "
                     "keeps APART. It listens on the beacon port, which every node on the segment shares, and "
                     "answers somewhere only it holds -- because just one of the sockets sharing a port is handed "
                     "a unicast. Pointing both at one port is the configuration that made two nodes on a host see "
                     "each other and never finish proving the key. Pick another, or leave it unset." },
        { .refuses = [](NodeConfig const& c) { return !c.clusterKeyFile.empty() && c.discoveryAddress.empty(); },
          .message = "--cluster-key-file is read by discovery and nothing else, and --discovery is not set. A "
                     "secret an operator went to the trouble of provisioning, being read by nobody, is exactly "
                     "the silent no-op this list exists to refuse." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && c.adminListen.empty(); },
          .message = "--dashboard needs --admin-listen: the dashboard is served on the admin surface, and "
                     "without one there is no port for it to answer on. It would start, log nothing wrong, "
                     "and serve the page to nobody." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && c.schedulerListen.empty(); },
          .message = "--dashboard needs --listen-scheduler: a node that runs no scheduler never leads a fleet, "
                     "so the page could only ever say it is not the leader. The fleet-wide facts live where "
                     "leadership does." },
        { .refuses = [](NodeConfig const& c) { return !c.dashboardTokenFile.empty() && !c.dashboard; },
          .message = "--dashboard-token-file guards the dashboard and nothing else, and --dashboard is not set. "
                     "A secret an operator provisioned, being read by nobody, is the silent no-op this list "
                     "exists to refuse." },
        { .refuses = [](NodeConfig const& c) { return c.tlsSelfSigned && !c.tlsCertFile.empty(); },
          .message = "--tls-self-signed and --tls-cert contradict each other: one generates a certificate and "
                     "the other names one. Silently preferring either would serve an identity the operator did "
                     "not choose, which is the whole thing a certificate is for." },
        { .refuses =
              [](NodeConfig const& c) { return (c.tlsSelfSigned || !c.tlsCertFile.empty()) && c.adminListen.empty(); },
          .message = "--tls-self-signed and --tls-cert serve the admin surface, and --admin-listen is not set. "
                     "TLS material nothing terminates is the silent no-op this list exists to refuse." },
        { .refuses = [](NodeConfig const& c) { return c.tlsCertFile.empty() != c.tlsKeyFile.empty(); },
          .message = "--tls-cert and --tls-key are both or neither: a certificate with no key cannot terminate "
                     "TLS, and this node would otherwise start and serve the admin surface in the clear while "
                     "an operator believed it was encrypted." },
        // The rule that keeps a fleet map off an open port. Loopback needs no
        // credential -- reaching it already means being on the machine -- but a
        // bind an operator deliberately exposed does, and HTTPS alone does not
        // supply it: TLS authenticates the SERVER to the browser and says nothing
        // about who the browser is.
        { .refuses =
              [](NodeConfig const& c) {
                  if (!c.dashboard || c.adminListen.empty() || !c.dashboardTokenFile.empty())
                      return false;
                  // The same default host `AdminEndpoint::Start` binds with, so
                  // this rule judges the address the endpoint will actually take
                  // rather than the text an operator typed.
                  auto const endpoint = ParseEndpoint(c.adminListen, "127.0.0.1");
                  return endpoint.has_value() && !IsLoopbackHost(endpoint->first);
              },
          .message = "--dashboard on a non-loopback --admin-listen needs --dashboard-token-file: the page is a "
                     "map of every member's hostname, endpoint and capacity, and an operator who bound it to "
                     "the network is publishing that to whoever asks. A bare port binds loopback and needs no "
                     "credential." },
    });

    for (auto const& rule: Rules)
        if (rule.refuses(cfg))
            return std::string { rule.message };

    return std::nullopt;
}

std::optional<std::string> NodeInstallRejection(NodeConfig const& cfg)
{
    // Composed rather than merged, so each table keeps the contract its own callers
    // rely on: this is the only place that says an install must satisfy both, and a
    // new row in either reaches the install path without anybody remembering to add
    // it here.
    //
    // The install-time table is asked first because its messages name the action --
    // "is required to install a service" reads as an answer to `--install-service`
    // in a way a startup rule does not, and only one refusal is ever printed.
    if (auto rejection = NodeServiceRejection(cfg))
        return rejection;

    // Every startup rule is a pure invariant of the parsed configuration -- no
    // clock, no filesystem, no port -- so each is decided the moment the command
    // line is typed. `--install-service` bakes that command line in and replays it
    // at every boot, which makes a startup rule strictly MORE worth refusing here
    // than at a start: a start refuses once, in front of the operator who typed it,
    // while a registration refuses forever into a log nobody reads.
    return StartupPolicyRejection(cfg);
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
