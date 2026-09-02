// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeMembership.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
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

    /// Parse `--drain-timeout`, in seconds.
    ///
    /// Zero is accepted and means "wait forever", which is a different answer from
    /// not passing the flag -- and is what this program did before the flag existed,
    /// so it has to stay sayable.
    /// @param sv Text to parse.
    /// @return The seconds, or why it is not a count of them.
    [[nodiscard]] std::expected<std::uint32_t, ConfigError> ParseDrainTimeout(std::string_view sv)
    {
        auto value = 0U;
        auto const* const begin = sv.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::unexpected(
                ArgvError(ConfigErrorCode::OutOfRange, "drain-timeout", std::format("not a number of seconds: {}", sv)));
        return value;
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

            if constexpr (Action == ClusterAction::Admit || Action == ClusterAction::Set)
            {
                // These two COMMIT their operand through consensus: an admitted
                // member's id and endpoint land in every peer's `ClusterState` and
                // are rendered into `/fleet.json`, and a setting is agreed by the
                // whole cluster and survives every restart. A consensus entry is
                // applied after it is committed, with nobody left to refuse it, so
                // this is the last place a person can be told.
                //
                // `Forget` is deliberately NOT here, and that omission is the trap
                // issue #159 records: its operand IS the offending id, so a check
                // covering it would make a bad member -- one admitted by an older
                // peer -- impossible to remove, and it would count towards quorum
                // forever.
                if (auto const text = ParseUtf8Text(value); !text.has_value())
                    return std::unexpected(text.error());
            }

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

    /// A `--toolchain` value, with the half that TRAVELS checked for being text.
    ///
    /// Split on the first `=`, as `SplitToolchain` does and for the same reason: a
    /// fingerprint is hex and contains none, so a compiler path holding one is only
    /// reachable through the override form. Restated here rather than shared,
    /// because sharing it would mean this file depending on the toolchain module it
    /// configures -- and what is restated is one `find`, while the rule that
    /// matters (what counts as text, and what the operator is told) stays in
    /// `ParseUtf8Text`.
    ///
    /// Only the fingerprint. The compiler beside it is a path on this machine, and
    /// on a host that transcodes nothing a legacy filename is a perfectly good
    /// filename -- refusing it would break a working node over a rule about a field
    /// it is not.
    ///
    /// Asked HERE rather than where the two halves are used, so it is decided by the
    /// parse: `--install-service` returns before a toolchain is ever resolved, and a
    /// registration that bakes in a fingerprint no scheduler will accept is one that
    /// fails at every boot with nobody watching.
    ///
    /// @param sv The flag's value.
    /// @return `sv` verbatim, or why its pinned half is not text.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseToolchain(std::string_view sv)
    {
        if (auto const eq = sv.find('='); eq != std::string_view::npos)
            if (auto const pinned = ParseUtf8Text(sv.substr(0, eq)); !pinned.has_value())
                return std::unexpected(pinned.error());
        return std::string { sv };
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

    /// One cluster member, from the `<id>=<host>:<port>` an operator typed.
    ///
    /// Through `Cluster::ParseMemberSpec`, which is the grammar `--cluster-admit`
    /// already takes: the documentation tells an operator to copy the same token
    /// between the two flags, so a second implementation would be two flags
    /// accepting different token sets for one concept -- with only one of them being
    /// what the transport actually dials.
    ///
    /// Here rather than in the tier that consumes it, because the parser is the one
    /// place on every path and the only one that can name the offending token --
    /// see `NodeConfig::raftPeers` for what that cost while it was not (#168).
    /// @param sv Text to parse.
    /// @return The member, or why the token is not one.
    [[nodiscard]] std::expected<Cluster::ClusterMember, ConfigError> ParseRaftPeer(std::string_view sv)
    {
        // Text FIRST, and over the whole token, because both halves of it are
        // published: the id is a member's identity in `ClusterState` and the address
        // is what every peer is told to dial. A value that is not valid UTF-8 is
        // refused by `SchedulerService::Register` on every heartbeat forever, with
        // the operator's only recovery being to rename the thing -- so it is refused
        // here, where they typed it and the flag can be named (issue #155). See
        // `ParseUtf8Text`, and `NodeOptions()` for the other rows in this column.
        if (auto const text = ParseUtf8Text(sv); !text.has_value())
            return std::unexpected(text.error());

        auto member = Cluster::ParseMemberSpec(sv);
        if (!member.has_value())
            return std::unexpected(
                ArgvError(ConfigErrorCode::ParseError, "raft-peer", std::format("not <id>=<host>:<port>: {}", sv)));
        return *std::move(member);
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

std::expected<void, ConfigError> ApplyNodeConfiguration(std::vector<YamlSetting> const& settings,
                                                        std::filesystem::path const& path,
                                                        std::span<char const* const> args,
                                                        NodeConfig& result)
{
    if (auto applied = ApplyFileSettings(NodeOptions(), settings, path, result); !applied.has_value())
        return applied;

    // A repeatable row's applier APPENDS, so a `--toolchain` on the command line
    // would otherwise EXTEND the file's list rather than replace it. Replacement is
    // the rule -- mixing partial file values with partial command-line values makes
    // precedence depend on declaration order, which is not something an operator can
    // reason about -- and it is driven off the table's own `clear` column, so a
    // fourth repeatable flag needs no edit here.
    ClearListsNamedOn(NodeOptions(), args, result);

    // The command line over the file-seeded result, through the same appliers. Its
    // outcome is discarded rather than ignored: the caller has already parsed this
    // exact argv once to find the config path, so anything that could be refused
    // here was refused there, with the message an operator wants and before a file
    // was read at all.
    (void) ParseOptionsInto(NodeOptions(), args, result);
    return {};
}

std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept
{
    static constexpr auto options = std::to_array<OptionSpec<NodeConfig>>({
        { .primary = "--config",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::configPath, ParseText>(),
          .description = "read settings from this YAML file. Every SETTING flag here\n"
                         "is a key in it, spelled with underscores; the one-shot\n"
                         "verbs (--install-service, --cluster-*, --help) are not, and\n"
                         "neither is this flag. The command line wins where both name\n"
                         "one. A named path is strict -- absent, unreadable or\n"
                         "malformed refuses to start -- while the machine-wide file\n"
                         "this looks for when unset is skipped when it is not there.\n"
                         "Its MODE is not checked: anyone who can write it decides\n"
                         "what this worker runs (#384)." },
        {
            .primary = "--scheduler",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            .apply = AssignFrom<&NodeConfig::scheduler, ParseText>(),
            .description = "the scheduler's --listen-node endpoint. Required: a\n"
                           "worker nothing knows about serves nobody.",
            .yamlKey = "scheduler",
            .same = FieldEq<&NodeConfig::scheduler>(),
        },
        {
            .primary = "--advertise",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            .apply = AssignFrom<&NodeConfig::advertise, ParseUtf8Text>(),
            .description = "host:port CLIENTS should use to reach this worker.\n"
                           "Defaults to --listen-node, which binds loopback unless\n"
                           "widened: the scheduler hands this string to clients\n"
                           "verbatim, so a worker that advertises an address only it\n"
                           "can reach is leased and then never answers.",
            .yamlKey = "advertise",
            .same = FieldEq<&NodeConfig::advertise>(),
        },
        {
            .primary = "--toolchain",
            .arity = Arity::Value,
            .operand = "=<compiler>|<fingerprint>=<compiler>",
            .apply = AppendFrom<&NodeConfig::toolchains, ParseToolchain>(),
            .description = "a toolchain this worker serves; repeatable. An OVERRIDE:\n"
                           "naming any pins this worker to exactly that set, and\n"
                           "naming none means serve whatever this machine has.\n"
                           "There is still no default COMPILER -- a default is how a\n"
                           "job ends up running against something nobody chose.",
            .yamlKey = "toolchain",
            .same = FieldEq<&NodeConfig::toolchains>(),
            .clear = ClearList<&NodeConfig::toolchains>(),
        },
        {
            .primary = "--no-toolchain-discovery",
            .arity = Arity::None,
            .apply = SetFalse<&NodeConfig::toolchainDiscovery>(),
            .description = "do not survey this machine for compilers. Without\n"
                           "--toolchain this leaves the worker with nothing to\n"
                           "serve, so it refuses to start -- and refuses to be\n"
                           "INSTALLED as a service, which is the registration that\n"
                           "would otherwise fail at every boot with nobody watching.",
            .yamlKey = "no_toolchain_discovery",
            .same = FieldEq<&NodeConfig::toolchainDiscovery>(),
        },
        {
            .primary = "--slots",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::slots, ParseSlots>(),
            .description = "concurrent compiles. Default: derived from this\n"
                           "machine's cores and memory, less what --node-class\n"
                           "reserves. A number given here is the answer and is\n"
                           "not clamped or reduced further. Advertised to the\n"
                           "scheduler AND enforced here: a worker that accepted\n"
                           "more would be fuller and slower than the scheduler\n"
                           "believes, at the same moment.",
            .yamlKey = "slots",
            .same = FieldEq<&NodeConfig::slots>(),
        },
        {
            .primary = "--node-class",
            .arity = Arity::Value,
            .operand = "=workstation|dedicated",
            .apply = AssignFrom<&NodeConfig::nodeClass, ParseNodeClass>(),
            .description = "how hard this machine may be driven (default:\n"
                           "workstation). A workstation keeps cores free for the\n"
                           "person using it; a dedicated node may be driven to its\n"
                           "slot limit. The default is the safe answer rather than\n"
                           "the common one.",
            .yamlKey = "node_class",
            .same = FieldEq<&NodeConfig::nodeClass>(),
        },
        {
            .primary = "--drain-timeout",
            .arity = Arity::Value,
            .operand = "=<seconds>",
            .apply = AssignFrom<&NodeConfig::drainTimeoutSeconds, ParseDrainTimeout>(),
            .description = "seconds a stop waits for compiles still running\n"
                           "before giving up and saying what it abandoned;\n"
                           "0 waits forever. Unbounded, the supervisor\n"
                           "decides instead and answers with SIGKILL and no\n"
                           "diagnostic.",
            .yamlKey = "drain_timeout_seconds",
            .same = FieldEq<&NodeConfig::drainTimeoutSeconds>(),
        },
        {
            .primary = "--reserve-cores",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::reservedCores, ParseReservedCores>(),
            .description = "cores never offered to the fleet, overriding what the\n"
                           "node class reserves. 0 is a real answer and is not the\n"
                           "same as omitting the flag. Ignored when --slots names\n"
                           "a number, which is the operator's answer already.",
            .yamlKey = "reserve_cores",
            .same = FieldEq<&NodeConfig::reservedCores>(),
        },
        {
            .primary = "--node-id",
            .arity = Arity::Value,
            .operand = "=<id>",
            .apply = AssignFrom<&NodeConfig::nodeId, ParseUtf8Text>(),
            .description = "this node's identity in the cluster. Giving it turns\n"
                           "consensus ON; without it this node leads alone,\n"
                           "which is right for one machine and is the default.",
            .yamlKey = "node_id",
            .same = FieldEq<&NodeConfig::nodeId>(),
        },
        {
            .primary = "--listen-raft",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::raftListen, ParseText>(),
            .description = "where peers reach this node's consensus port. A bare\n"
                           "port binds the WILDCARD: peers are on other machines\n"
                           "by definition, so loopback would silently not work.",
            .yamlKey = "listen_raft",
            .same = FieldEq<&NodeConfig::raftListen>(),
        },
        {
            .primary = "--raft-peer",
            .arity = Arity::Value,
            .operand = "=<id>=<host>:<port>",
            .apply = AppendFrom<&NodeConfig::raftPeers, ParseRaftPeer>(),
            .description = "a cluster member and where it answers; repeatable.\n"
                           "Both halves in one token because a member id with\n"
                           "no address is a node counted towards quorum and\n"
                           "unreachable. This is the BOOTSTRAP set only:\n"
                           "membership is replicated once the cluster runs.",
            .yamlKey = "raft_peer",
            .same = FieldEq<&NodeConfig::raftPeers>(),
            .clear = ClearList<&NodeConfig::raftPeers>(),
        },
        {
            .primary = "--raft-join",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::raftJoin>(),
            .description = "start with NO cluster and wait to be admitted to one.\n"
                           "--raft-peer then lists nodes this one can REACH rather\n"
                           "than a cluster it belongs to -- itself, and whoever\n"
                           "will admit it, because a joiner has to be able to\n"
                           "answer the leader before it can learn where that\n"
                           "leader is. Without this flag a node bootstraps a\n"
                           "cluster of itself, elects itself, and can never\n"
                           "afterwards be admitted to anybody else's.",
            .yamlKey = "raft_join",
            .same = FieldEq<&NodeConfig::raftJoin>(),
        },
        {
            .primary = "--cluster-dir",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::clusterDir, ParsePathValue>(),
            .description = "where consensus keeps its durable state. A node\n"
                           "that answered a vote and forgot it votes twice in\n"
                           "one term after a restart, which is two leaders.",
            .yamlKey = "cluster_dir",
            .same = FieldEq<&NodeConfig::clusterDir>(),
        },
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
        {
            .primary = "--cluster-id",
            .arity = Arity::Value,
            .operand = "=<name>",
            .apply = AssignFrom<&NodeConfig::clusterId, ParseUtf8Text>(),
            .description = "which fleet this node belongs to. Plain text in every\n"
                           "beacon and NOT a credential: what it buys is that two\n"
                           "unrelated fleets on one segment ignore each other.",
            .yamlKey = "cluster_id",
            .same = FieldEq<&NodeConfig::clusterId>(),
        },
        {
            .primary = "--discovery",
            .arity = Arity::Value,
            .operand = "=<address>:<port>",
            .apply = AssignFrom<&NodeConfig::discoveryAddress, ParseText>(),
            .description = "announce this node on the segment and listen for peers\n"
                           "here; off unless given. Needs --node-id and\n"
                           "--cluster-key-file. Without it a cluster is exactly\n"
                           "the --raft-peer list an operator typed.",
            .yamlKey = "discovery",
            .same = FieldEq<&NodeConfig::discoveryAddress>(),
        },
        {
            .primary = "--discovery-reply-port",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::discoveryReplyPort, ParseNodePort>(),
            .description = "port peers unicast their discovery challenges and\n"
                           "proofs to; kernel-chosen unless given. NOT the\n"
                           "--discovery port: that one is shared by every node on\n"
                           "the segment, and only one socket sharing a port is\n"
                           "handed a unicast. Pin it where a host firewall opens\n"
                           "named ports only -- one per node on the machine.",
            .yamlKey = "discovery_reply_port",
            .same = FieldEq<&NodeConfig::discoveryReplyPort>(),
        },
        {
            .primary = "--cluster-key-file",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::clusterKeyFile, ParsePathValue>(),
            .description = "the cluster's pre-shared key. A FILE and not a flag:\n"
                           "a command line is readable through ps, and a key that\n"
                           "leaks admits a node whose objects the whole fleet\n"
                           "then caches. Discovery proves the cluster with it,\n"
                           "and the scheduler SIGNS lease grants with it. Without\n"
                           "one, a grant is unsigned and any client that can reach\n"
                           "a worker's compile port can spend it.",
            .yamlKey = "cluster_key_file",
            .same = FieldEq<&NodeConfig::clusterKeyFile>(),
        },
        {
            .primary = "--scheduler-token-file",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::schedulerTokenFile, ParsePathValue>(),
            .description = "credential this node REQUIRES on its scheduler verbs.\n"
                           "The inbound half of --requirepass, which only says\n"
                           "what this node presents. A FILE and not a flag: a\n"
                           "command line is readable through ps. Without it,\n"
                           "membership is the only gate, and that is a host list,\n"
                           "not a secret.",
            .yamlKey = "scheduler_token_file",
            .same = FieldEq<&NodeConfig::schedulerTokenFile>(),
        },
        {
            .primary = "--admin-listen",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::adminListen, ParseText>(),
            .description = "serve /metrics and /healthz here; off unless given.\n"
                           "A bare port binds loopback: a scrape endpoint on a\n"
                           "public interface is an operator's decision, not a\n"
                           "default. /healthz is also the liveness probe this\n"
                           "worker otherwise has none of.",
            .yamlKey = "admin_listen",
            .same = FieldEq<&NodeConfig::adminListen>(),
        },
        {
            .primary = "--dashboard",
            .apply = SetTrue<&NodeConfig::dashboard>(),
            .description = "also serve the fleet dashboard on --admin-listen, at\n"
                           "/fleet and /fleet.json. Off unless given: the page is a\n"
                           "map of every member's hostname, endpoint and capacity.\n"
                           "Answered in full only while this node LEADS; anyone else\n"
                           "names the leader rather than showing half a fleet.",
            .yamlKey = "dashboard",
            .same = FieldEq<&NodeConfig::dashboard>(),
        },
        {
            .primary = "--dashboard-token-file",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::dashboardTokenFile, ParsePathValue>(),
            .description = "credential the dashboard requires, as Basic or Bearer.\n"
                           "A FILE and not a flag: a command line is readable\n"
                           "through ps. Its own secret rather than --requirepass,\n"
                           "which every member of the fleet already holds. Required\n"
                           "when --admin-listen is not on loopback.",
            .yamlKey = "dashboard_token_file",
            .same = FieldEq<&NodeConfig::dashboardTokenFile>(),
        },
        {
            .primary = "--tls-self-signed",
            .apply = SetTrue<&NodeConfig::tlsSelfSigned>(),
            .description = "generate a self-signed certificate at startup and serve\n"
                           "the admin surface over HTTPS with it, so an internal\n"
                           "deployment needs no certificate to obtain. Encrypts the\n"
                           "traffic; it does NOT prove which node answered, so the\n"
                           "fingerprint is logged for you to compare. Regenerated\n"
                           "every restart -- name --tls-cert for a stable identity.",
            .yamlKey = "tls_self_signed",
            .same = FieldEq<&NodeConfig::tlsSelfSigned>(),
        },
        {
            .primary = "--tls-cert",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::tlsCertFile, ParsePathValue>(),
            .description = "serve the admin surface over HTTPS with this\n"
                           "certificate. TLS is on by naming a certificate and a\n"
                           "key rather than by a flag, so there is no way to ask\n"
                           "for it without the material to do it.",
            .yamlKey = "tls_cert",
            .same = FieldEq<&NodeConfig::tlsCertFile>(),
        },
        {
            .primary = "--tls-key",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::tlsKeyFile, ParsePathValue>(),
            .description = "private key for --tls-cert. Both or neither.",
            .yamlKey = "tls_key",
            .same = FieldEq<&NodeConfig::tlsKeyFile>(),
        },
        {
            .primary = "--serve-scheduler",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::serveScheduler>(),
            .description = "serve the fleet's scheduler verbs; off unless given.\n"
                           "Answered on --listen-node, beside the cache verbs,\n"
                           "and only while this node LEADS the cluster; a\n"
                           "follower redirects to the leader and an election in\n"
                           "progress refuses, both of which a client answers by\n"
                           "compiling locally. It also decides where a bare\n"
                           "--listen-node binds: the wildcard here, because\n"
                           "peers have to reach it, and loopback without it.",
            .yamlKey = "serve_scheduler",
            .same = FieldEq<&NodeConfig::serveScheduler>(),
        },
        {
            .primary = "--fleet-member",
            .arity = Arity::Value,
            .operand = "=<host>[:<port>]",
            .apply = AppendFrom<&NodeConfig::fleetMembers, ParseText>(),
            .description = "a peer this node serves; repeatable. Gates all three\n"
                           "surfaces -- the compile port, the cache tier and the\n"
                           "scheduler -- so a WORKER needs it too, or it compiles\n"
                           "for its own machine alone. Only the host is matched:\n"
                           "a peer dials from an ephemeral port, so an endpoint\n"
                           "is not something a connection can be compared to.",
            .yamlKey = "fleet_member",
            .same = FieldEq<&NodeConfig::fleetMembers>(),
            .clear = ClearList<&NodeConfig::fleetMembers>(),
        },
        {
            .primary = "--fleet-open",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::fleetOpen>(),
            .description = "admit every caller to this node, not only\n"
                           "--fleet-member hosts. For one machine, or a network\n"
                           "that is already the boundary. Explicit because\n"
                           "'no policy' and 'admit everybody' must be the same\n"
                           "decision -- listing nobody refuses everybody.",
            .yamlKey = "fleet_open",
            .same = FieldEq<&NodeConfig::fleetOpen>(),
        },
        {
            .primary = "--cache-memory",
            .arity = Arity::Value,
            .operand = "=<size>",
            .apply = AssignFrom<&NodeConfig::cacheMemoryBytes, ParseCacheBytes>(),
            .explicitBit = &NodeConfig::cacheMemoryExplicit,
            .description = "size of this node's own in-memory cache tier;\n"
                           "k/m/g = KiB/MiB/GiB or N% of host RAM. Defaults\n"
                           "to 25% of RAM within [512m, 8g]; 0 turns it off.\n"
                           "It exists so a local rebuild on a slow or bad\n"
                           "network never reaches the wire at all.",
            .yamlKey = "cache_memory",
            .same = FieldEq<&NodeConfig::cacheMemoryBytes>(),
        },
        {
            .primary = "--cache-disk",
            .arity = Arity::Value,
            .operand = "=<bytes>",
            .apply = AssignFrom<&NodeConfig::cacheDiskBytes, ParseCacheDiskBytes>(),
            .description = "cap this node's on-disk cache tier at this size\n"
                           "(default 0, meaning grow as needed). Only means\n"
                           "anything with --cache-dir: without a path there is\n"
                           "no disk tier for a budget to bound.",
            .yamlKey = "cache_disk",
            .same = FieldEq<&NodeConfig::cacheDiskBytes>(),
        },
        {
            .primary = "--cache-dir",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::cacheDir, ParsePathValue>(),
            .description = "back the local cache tier with disk at this path.\n"
                           "Memory-only otherwise: a disk tier is a resource an\n"
                           "operator should have to name. ONE node per path,\n"
                           "enforced: the store is claimed exclusively, so a\n"
                           "second node sharing it refuses to start.",
            .yamlKey = "cache_dir",
            .same = FieldEq<&NodeConfig::cacheDir>(),
        },
        {
            .primary = "--listen-node",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::nodeListen, ParseText>(),
            .explicitBit = &NodeConfig::nodeListenExplicit,
            .description = "this node's 0xFC port: cache verbs, and the\n"
                           "scheduler verbs with --serve-scheduler (default\n"
                           "port 6674, where fastcache-cc already looks; empty\n"
                           "closes it). A bare port binds LOOPBACK on a worker\n"
                           "-- a cache any host can dial is this machine's\n"
                           "whole build output served to strangers -- and the\n"
                           "wildcard with --serve-scheduler, whose peers are\n"
                           "elsewhere by definition. Widening it admits nobody\n"
                           "new to the cache: those verbs answer this machine\n"
                           "alone whatever this is bound to.",
            .yamlKey = "listen_node",
            .same = FieldEq<&NodeConfig::nodeListen>(),
        },
        {
            .primary = "--upstream",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            .apply = AssignFrom<&NodeConfig::upstream, ParseText>(),
            .description = "the shared fastcached this node reads through to.\n"
                           "Empty is honest rather than broken: one developer's\n"
                           "machine has no shared cache.",
            .yamlKey = "upstream",
            .same = FieldEq<&NodeConfig::upstream>(),
        },
        {
            .primary = "--requirepass",
            .arity = Arity::Value,
            .operand = "=<secret>",
            .apply = AssignFrom<&NodeConfig::token, ParseText>(),
            .description = "credential presented to the scheduler",
            .yamlKey = "requirepass",
            .same = FieldEq<&NodeConfig::token>(),
        },
        {
            .primary = "--log-level",
            .arity = Arity::Value,
            .operand = "=<level>",
            .apply = AssignFrom<&NodeConfig::logLevel, ParseNodeLogLevel>(),
            .description = "trace, debug, info, warn, error, fatal (default info)",
            .yamlKey = "log_level",
            // The ONE reloadable row today, and it earns it: `ILogger::SetMinLevel`
            // exists, so raising the level to diagnose something takes effect on a
            // running worker without restarting it mid-build. `logTimestamps` next
            // door is deliberately NOT marked -- `ConsoleLogger` takes its timestamp
            // setting at construction and offers no setter, so marking it would
            // publish a snapshot the logger does not honour, which is the exact
            // disagreement the default guards against.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::logLevel>(),
        },
        {
            // Beside `--log-level` because they are one concern, and two flags
            // because they are not one question: a level is a FILTER and this is a
            // FORMAT. Folding them into one grammar would invent a spelling neither
            // binary has.
            //
            // Spelled exactly as `fastcached`'s. An operator who learned it there
            // must not find the worker wanting a different word for the same thing --
            // the rule `--service-scope` already follows here.
            //
            // **No `explicitBit`, and that is the node's shape rather than an
            // omission.** The daemon carries `logTimestampsExplicit` because its
            // merge is `MergeField` copying field by field, so a file value and a
            // typed value are told apart per field or the command line stops winning.
            // This table applies a FILE and then argv through the same appliers, in
            // that order, so "the command line wins" is which loop runs second. The
            // node has no explicit-bit layer for booleans at all -- `--daemon`,
            // `--dashboard` and `--no-toolchain-discovery` have none either -- and a
            // default of false that only ever sets true has nothing to arrive at
            // without being asked for, which is the whole question a provenance bit
            // answers.
            .primary = "--log-timestamps",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::logTimestamps>(),
            .description = "prefix every log line with an ISO 8601 UTC timestamp\n"
                           "(default: on under macOS, where nothing else stamps a\n"
                           "service's output; off elsewhere)",
            .yamlKey = "log_timestamps",
            .same = FieldEq<&NodeConfig::logTimestamps>(),
        },
        {
            // The negative spelling, for the reason the daemon's carries: the DEFAULT
            // is platform-dependent now (#496, #507), so under macOS this is the only
            // way to say "off" -- and the only way a REGISTRATION can, since
            // `--install-service` replays its command line forever and a flag that can
            // only say "on" would turn an operator's explicit "off" back on at every
            // boot.
            //
            // **No `yamlKey`, and it is on `notFromFile` below with that reason.** The
            // file keeps one key, `log_timestamps`, a boolean that already wins in both
            // directions. A second key would give the file two ways to say one thing,
            // and `apply` runs on `true` alone -- so `no_log_timestamps: false` would
            // pass nothing while reading like it said something.
            .primary = "--no-log-timestamps",
            .arity = Arity::None,
            .apply = SetFalse<&NodeConfig::logTimestamps>(),
            .description = "do not prefix log lines with a timestamp, overriding the\n"
                           "platform default. The one way to ask for unstamped output\n"
                           "under macOS",
        },
        { .primary = "--daemon",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::daemon>(),
          .description = "run in the background (POSIX), or as the service body\n"
                         "the Windows SCM starts. Supervisors that manage a\n"
                         "foreground process -- systemd and launchd -- must NOT\n"
                         "pass it: they reap a job that forks as 'exited'." },
        {
            .primary = "--pidfile",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::pidfile, ParseText>(),
            .description = "write the pid here when daemonizing (POSIX)",
            .yamlKey = "pidfile",
            .same = FieldEq<&NodeConfig::pidfile>(),
        },
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
        // `ParseFlow::Continue`, unlike `--help` and `--version`, and the difference
        // is the whole point of the flag. Those two ignore the rest of the command
        // line; this one REPORTS on it. With `Stop`, `--print-surfaces
        // --listen-node 6675` parsed nothing after the first flag and printed
        // the defaults -- a worksheet that silently describes a different node from
        // the one the operator asked about, which is the misleading-document failure
        // this flag exists to prevent.
        { .primary = "--print-surfaces",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::printSurfaces>(),
          .description = "list every port this configuration would open,\n"
                         "with its protocol, and exit. Generated from the\n"
                         "node's own port map rather than from prose, so a\n"
                         "firewall list comes from the binary that binds\n"
                         "them. Pass the flags you would run with: it\n"
                         "prints what THIS configuration serves, not the\n"
                         "defaults." },
        { .primary = "--version",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::version>(),
          .flow = ParseFlow::Stop,
          .description = "print the version and exit" },
    });
    static_assert(TableIsWellFormed<NodeConfig>(options));

    // Every row a file may NOT carry, and why for each.
    //
    // The guard below reads this array rather than restating it, so the two cannot
    // disagree: a new flag either names a key or is listed here with a reason, and
    // there is no third state in which it is quietly unreachable from the file an
    // operator is told configures this worker.
    //
    // Two kinds live here and they are not the same objection. A one-shot verb
    // (`--install-service`, `--cluster-forget`, `--migrate-cache`, `--help`) is a
    // decision taken once; a file is read at EVERY start, so a key for one would
    // replay that decision forever -- a worker that re-registers itself, or asks
    // the cluster a question, instead of serving. The rest are settings that
    // describe how this process was STARTED rather than what it does, and reading
    // them from the very file the start already found is circular.
    static constexpr auto notFromFile = std::to_array<std::pair<std::string_view, std::string_view>>({
        { "--config", "names the file being read; a key for it would name a file to read while reading one" },
        { "--no-log-timestamps",
          "the negative spelling of a key the file already carries. `log_timestamps` is a boolean and "
          "wins in both directions, so a second key would be two ways to say one thing -- and `apply` "
          "runs on `true` alone, so `no_log_timestamps: false` would pass nothing while reading like it "
          "said something. argv needs the spelling because argv cannot carry a value; a file can" },
        { "--daemon",
          "how this process was started, decided by whoever started it -- a service is already "
          "supervised, and a file that forked an operator's foreground run would take away the "
          "console they were watching" },
        { "--service-name",
          "the identity a registration is made under, read back from the file that "
          "registration points at -- so the name would come from the file the name found" },
        { "--service-scope", "the same circle as --service-name, for which supervisor the registration goes to" },
        { "--install-service", "registers and exits; a key would re-register at every start" },
        { "--uninstall-service", "removes the registration and exits; a key would remove it at every start" },
        { "--migrate-cache",
          "converts the store and exits; a key would convert at every start, on a store "
          "that after the first run has nothing left to convert" },
        { "--print-surfaces", "prints the ports and exits; a key would print them instead of serving them" },
        { "--cluster-status", "asks a running cluster a question and exits" },
        { "--cluster-set", "changes a running cluster's settings and exits" },
        { "--cluster-admit", "admits a member and exits" },
        { "--cluster-forget", "removes a member and exits" },
        { "--help", "prints usage and exits" },
        { "--version", "prints the version and exits" },
    });

    // A row is reachable from the file or it is named above. Checked at compile
    // time, because a flag that is silently absent from the file is not a condition
    // to report -- it is a setting an operator writes, restarts, and never sees take
    // effect, with nothing anywhere saying why.
    static_assert(std::ranges::all_of(options,
                                      [](OptionSpec<NodeConfig> const& spec) {
                                          return !spec.yamlKey.empty()
                                                 || std::ranges::any_of(notFromFile, [&spec](auto const& excluded) {
                                                        return excluded.first == spec.primary;
                                                    });
                                      }),
                  "every --flag must carry a yamlKey or be listed in notFromFile with a reason");

    // And the converse: a row named above must not also carry a key, which is how
    // an exclusion becomes a comment describing something that stopped being true.
    static_assert(std::ranges::all_of(notFromFile,
                                      [](auto const& excluded) {
                                          return std::ranges::any_of(options, [&excluded](auto const& spec) {
                                              return spec.primary == excluded.first && spec.yamlKey.empty();
                                          });
                                      }),
                  "every notFromFile entry must name a real row that carries no yamlKey");

    // A row a FILE can set must be comparable, and a row it cannot must not be.
    //
    // **Both directions, at compile time, because the correspondence was verified by
    // hand once and nothing kept it in step.** That is exactly
    // [#406](https://github.com/LASTRADA-Software/fastcached/issues/406)'s shape --
    // a hand-checked list beside a table -- and reproducing it inside the change that
    // gives its sibling a column would be the joke telling itself. Row 39 arrives
    // without a comparator, and without this nothing says so: a reload would then
    // read that field as unchanged forever, which is the silent-success failure this
    // whole mechanism exists to prevent.
    //
    // The forward direction is the load-bearing one. The converse matters too: a
    // comparator on a row no file can reach is dead code that reads as coverage, and
    // the next person to audit this would count it.
    static_assert(
        std::ranges::all_of(
            options, [](OptionSpec<NodeConfig> const& spec) { return spec.yamlKey.empty() == (spec.same == nullptr); }),
        "a row with a yamlKey needs a FieldEq comparator, and a row without one must not have it");

    return options;
}

std::vector<std::string_view> UnreloadableChanges(NodeConfig const& previous, NodeConfig const& candidate)
{
    std::vector<std::string_view> changed;
    for (auto const& spec: NodeOptions())
    {
        // A row with no comparator is not configuration state -- a one-shot verb, or
        // an install-time flag. No file can set it, so no reload can change it. The
        // static_assert beside the table is what keeps that true rather than assumed.
        if (spec.same == nullptr || spec.reloadable == Reloadable::Yes)
            continue;
        if (!spec.same(previous, candidate))
            changed.push_back(spec.primary);
    }
    return changed;
}

std::expected<void, ConfigError> ValidateNodeReloadable(NodeConfig const& previous, NodeConfig const& candidate)
{
    auto const changed = UnreloadableChanges(previous, candidate);
    if (changed.empty())
        return {};

    std::string names;
    for (auto const& flag: changed)
    {
        if (!names.empty())
            names += ", ";
        names += flag;
    }

    return std::unexpected(ConfigError {
        .code = ConfigErrorCode::ImmutableChanged,
        .source = {},
        .line = 0,
        // The FIRST name, because the field is one string and something has to go in
        // it; the whole list is in the context, which is what an operator reads.
        .field = std::string { changed.front() },
        .context = std::format("not reloadable, so nothing was applied: {}", names),
    });
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

    /// Emit `--flag=value` when the operator TYPED it, whatever its value.
    ///
    /// A named sibling of `emitIfSet` rather than an `if` per flag, because reaching
    /// for `emitIfSet` IS the mistake: it compares the value against the default, and
    /// the two answers part company on the one input that matters -- an operator
    /// typing the default, which is what somebody does after reading the value off
    /// the startup line to pin it. That value is then dropped from the registration
    /// and the service re-derives it at every start.
    ///
    /// Which flags need this is not a matter of taste. `--cache-memory`'s default is
    /// a share of host RAM, so a pinned budget moves under a VM resize; and
    /// `--listen-node` decides on provenance whether a bind failure is FATAL, so a
    /// registration that lost the bit warns past a taken port forever and the node
    /// comes up healthy serving no cache (#286). `NodeConfig_test` walks
    /// `NodeOptions()` and requires every row carrying an `explicitBit` to arrive
    /// here, so a new one cannot go back to `emitIfSet` by omission.
    auto const emitIfExplicit = [&argv](std::string_view flag, auto const& value, bool wasTyped) {
        if (wasTyped)
            argv.emplace_back(std::format("--{}={}", flag, value));
    };

    /// Emit a path flag, made absolute.
    ///
    /// A service does not inherit the installing shell's working directory, so a
    /// relative path captured at install time resolves somewhere else at start --
    /// which for a pidfile means a supervisor that cannot find its own process.
    /// Resolve a path the same way `emitPathIfSet` does, for a spec FIELD.
    ///
    /// Shared with it rather than restated: a `configPath` that disagreed with the
    /// `--config=` in the very same registration is two answers to one question,
    /// and the refusal that reads the field would then name a path the service was
    /// never given.
    auto const absoluteOrAsWritten = [](std::string const& value) {
        if (value.empty())
            return std::string {};
        std::error_code ec;
        auto const absolute = std::filesystem::absolute(value, ec);
        return ec ? value : absolute.string();
    };

    auto const emitPathIfSet = [&argv, &absoluteOrAsWritten](std::string_view flag, std::string const& value) {
        if (value.empty())
            return;
        argv.emplace_back(std::format("--{}={}", flag, absoluteOrAsWritten(value)));
    };

    // Unconditional: it is what the running service identifies itself by, and on
    // launchd it is what the job label derives from.
    argv.push_back(std::format("--service-name={}", cfg.serviceName));

    // First, and made absolute like every other path: it is what supplies every
    // setting the operator did NOT type here, and a service does not inherit the
    // installing shell's working directory. Emitted rather than resolved -- an
    // empty value means the operator named no file, and the service repeats the
    // machine-wide lookup at each start, which is what lets a package replace that
    // file without touching the registration.
    emitPathIfSet("config", cfg.configPath);

    emitIfSet("scheduler", cfg.scheduler, defaults.scheduler);
    emitIfSet("advertise", cfg.advertise, defaults.advertise);
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
    emitPathIfSet("scheduler-token-file", cfg.schedulerTokenFile.string());
    if (cfg.tlsSelfSigned)
        argv.emplace_back("--tls-self-signed");
    emitPathIfSet("tls-cert", cfg.tlsCertFile.string());
    emitPathIfSet("tls-key", cfg.tlsKeyFile.string());
    if (cfg.serveScheduler)
        argv.emplace_back("--serve-scheduler");
    emitIfExplicit("cache-memory", cfg.cacheMemoryBytes, cfg.cacheMemoryExplicit);
    emitIfSet("cache-disk", cfg.cacheDiskBytes, defaults.cacheDiskBytes);
    emitIfExplicit("listen-node", cfg.nodeListen, cfg.nodeListenExplicit);
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
    // not as a packaging bug. Re-rendered rather than echoed, because the list is
    // stored parsed; it is the same token either way, since `ParseMemberSpec` splits
    // at the FIRST `=` and keeps the endpoint verbatim.
    for (auto const& peer: cfg.raftPeers)
        argv.push_back(std::format("--raft-peer={}={}", peer.id, peer.raftEndpoint));
    emitIfSet("upstream", cfg.upstream, defaults.upstream);
    emitPathIfSet("cache-dir", cfg.cacheDir.string());
    if (cfg.raftJoin)
        argv.emplace_back("--raft-join");
    if (cfg.fleetOpen)
        argv.emplace_back("--fleet-open");
    for (auto const& member: cfg.fleetMembers)
        argv.push_back(std::format("--fleet-member={}", member));
    emitIfSet("drain-timeout", cfg.drainTimeoutSeconds, defaults.drainTimeoutSeconds);
    emitIfSet("log-level", LogLevelName(cfg.logLevel), LogLevelName(defaults.logLevel));

    // **Both spellings, because the DEFAULT is platform-dependent** (#496, #507).
    // Emitting only the positive one is sound while the default is false everywhere:
    // "it is on" is then the only thing a registration ever has to say. Under macOS
    // the default is true, so an operator who asked for `--no-log-timestamps` would
    // have that dropped from the registration and get timestamps back at every boot,
    // silently and forever -- a registration replays its command line.
    //
    // Compared against the DEFAULT rather than tested for truth, so the registration
    // carries a flag exactly when it has something to say. `--no-toolchain-discovery`
    // below keeps the one-sided shape: its default is true on every platform.
    if (cfg.logTimestamps != NodeConfig {}.logTimestamps)
        argv.emplace_back(cfg.logTimestamps ? "--log-timestamps" : "--no-log-timestamps");
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
                         // What the operator named, so InlineCredentialRejection can
                         // say where the secret belongs instead of merely that it
                         // may not go here. Absolute for the same reason the flag
                         // above is: an install run from a shell resolves a relative
                         // path somewhere the service never will.
                         .configPath = absoluteOrAsWritten(cfg.configPath),
                         // Still empty, and the reason has CHANGED with #291 -- so
                         // it is restated rather than left to read as before.
                         //
                         // This worker now has a config file, so "configured
                         // entirely from argv" is no longer why. What is still true
                         // is that it has no `--storage`, and `applicationName` is
                         // the one bit `WithScopeDefaults` reads before appending
                         // BOTH defaults: naming an application here would bake a
                         // `--storage=` into every user-scope registration, and the
                         // job would answer its own command line with "unrecognised
                         // argument" at every start -- reported installed, dead at
                         // every boot.
                         //
                         // What that costs is the system-scope `--config=` default,
                         // which this spec therefore emits itself above from what the
                         // operator typed, and the install-time readability check
                         // `ServiceAccountReadDenial` performs on a configPath the
                         // installer chose. Both are gaps rather than decisions:
                         // #396, which is `WithScopeDefaults` learning to decide
                         // the two defaults separately.
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

Cluster::ClusterMember const* ClusterSelfMember(NodeConfig const& cfg) noexcept
{
    auto const self = std::ranges::find(cfg.raftPeers, cfg.nodeId, &Cluster::ClusterMember::id);
    return self != cfg.raftPeers.end() ? std::to_address(self) : nullptr;
}

std::string AdvertisedEndpoint(NodeConfig const& cfg)
{
    // The flag wins whenever it was given. Written ONCE -- `main` hands the result to
    // `MakeWorkerLeaseValidator` and to the heartbeat's REGISTER, and the refusals
    // below judge it -- so the three consumers of this endpoint cannot disagree about
    // what it is. This expression once stood character-for-character in `main.cpp` too.
    if (!cfg.advertise.empty())
        return cfg.advertise;

    // The fallback is the `Node` surface, which is where a dispatched compile now
    // ARRIVES. It was `{--bind}:{--port}`, a dedicated compile port that no longer
    // exists -- telling clients to dial one would be silent at both ends, because the
    // registration still succeeds.
    //
    // Resolved through the surface row rather than read off `nodeListen`, because a
    // bare port's host depends on the configuration (`NodeListenDefaultHost`) and the
    // row is where that is decided. Reading the flag directly would be the second
    // author this function was written to delete.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    if (node.empty())
        return {};
    return FormatHostPort(node.front().host, node.front().port);
}

/// Whether the endpoint this node would advertise names no host a client can dial.
///
/// A worker that names the wildcard registers `0.0.0.0:<port>` -- which the
/// scheduler hands to clients verbatim, and a client on another machine dialling the
/// wildcard reaches **itself**. `NodeServiceRejection` has always said so for an
/// install; this is the same fact for a hand-started worker.
///
/// Judged on the endpoint the node would actually advertise rather than on whether
/// the flag was typed, so an operator who spells the default out is answered too.
/// Only a wildcard is refused, never an address that merely might not resolve: a
/// host that is down today can be the right one at the next boot (#208), while the
/// wildcard is wrong by construction on every machine and forever.
/// @param cfg The parsed configuration.
/// @return Whether remote clients would be told to dial a wildcard.
[[nodiscard]] bool AdvertisesWildcard(NodeConfig const& cfg)
{
    // Through the one derivation, never spelled again here. This function is a startup
    // REFUSAL, so a copy of the rule would let it pass a configuration whose advertised
    // endpoint differs from the one it judged -- and that endpoint is what every lease's
    // MAC is taken over. See `AdvertisedEndpoint`.
    auto const advertised = AdvertisedEndpoint(cfg);

    // `HostOfEndpoint` rather than `SplitHostPort` plus a fallback: an endpoint that
    // will not split is a bare host rather than a parse failure, and that rule is
    // spelled once in `Core/HostPort` for every caller that needs it.
    auto const host = HostOfEndpoint(advertised);

    // The two spellings of "every interface", plus the empty host -- which reaches
    // `getaddrinfo` as nullptr under AI_PASSIVE and is therefore the wildcard as
    // well, the same third case `--listen-node=:6674` is refused for. The bracketed
    // spelling needs no row of its own: `HostOfEndpoint` strips the brackets whether
    // or not a port follows, so `[::]` and `[::]:6676` both arrive here as `::`.
    return host.empty() || host == "0.0.0.0" || host == "::";
}

/// Whether a machine that is not this one could reach this node's COMPILE verbs.
///
/// **ONE port answers them now**, and this function is what is left of a rule that
/// once had to ask two. `--bind` was the whole answer until the compile verbs gained a
/// second door on the merged `0xFC` listener, and for one release the question was the
/// disjunction: asking either half alone let an open surface pass this table --
/// `--bind 127.0.0.1 --serve-scheduler --fleet-open` with no `--cluster-key-file`
/// looked local, passed, and served unauthenticated compiles on a wildcard-bound port
/// with every `worker_jobs_refused_lease_*` counter reading zero. #290 stage 3 retires
/// the dedicated port, so the surface row is the whole answer again -- and it is the
/// row rather than `--listen-node`, because a bare port's host is decided by
/// `NodeListenDefaultHost`.
///
/// The node surface is asked of its own ROW rather than re-deriving the host here,
/// which is the rule the dashboard credential rule below already follows: that row's
/// default host depends on the configuration, it may resolve to nothing at all, and a
/// second author of the resolution is one that judges an address the surface no longer
/// binds. The row also answers "not served" for a node with neither a cache tier nor a
/// scheduler -- correctly, because such a node opens no `0xFC` port and its compiles
/// arrive only on `--bind`.
///
/// Whether this node accepts from the network and then tells peers to dial loopback.
///
/// The wildcard's sibling, and the shape #290 stage 3 newly made the default. The
/// advertised endpoint used to fall back to `{--bind}:{--port}`, whose bind defaults to
/// the wildcard; it now falls back to the `Node` surface, whose host defaults to
/// LOOPBACK on a node that does not schedule. Both are unreachable from another
/// machine and both fail the same silent way -- the worker registers, heartbeats, is
/// leased out, and every remote client dials something that is not this worker.
///
/// **The bind must DISAGREE with the advertise, and that clause is the whole rule.**
/// An earlier draft refused a loopback advertise outright, on the reasoning that
/// loopback is unreachable from another machine. True, and not a defect when the
/// SURFACE is on loopback too: nobody else is meant to reach that node, which is the
/// single-machine fleet this project supports and tests. The three reachability rows
/// are one idea -- refuse when what a peer is TOLD and what this node ACCEPTS ON do
/// not agree -- and this one is its second cell, `AdvertisesPastALoopbackBind` the
/// third.
///
/// Kept a separate predicate behind a separate row rather than folded into
/// `AdvertisesWildcard`, because the two are different operator mistakes with
/// different remedies and one message could only serve them by describing neither. A
/// row is the refusal here, not the predicate.
/// @param cfg The parsed configuration.
/// @return Whether remote clients would be told to dial their own machine.
[[nodiscard]] bool AdvertisesLoopbackFromAReachableBind(NodeConfig const& cfg)
{
    if (!IsLoopbackHost(HostOfEndpoint(AdvertisedEndpoint(cfg))))
        return false;

    // **And the bind has to disagree with it.** A node whose surface is ALSO on
    // loopback is a coherent single-machine fleet, not a mistake: no remote client is
    // supposed to reach it, so "no remote client can reach the advertised endpoint" is
    // the configuration working rather than failing. This repository runs whole fleets
    // that way -- `dist-compile-e2e.sh` starts a `--fleet-open` scheduler on
    // `127.0.0.1` and advertises `127.0.0.1` -- and an earlier draft of this rule
    // refused it, which would have broken the fixture that exercises the fleet.
    auto const bound = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(bound, [](SurfaceEndpoint const& endpoint) { return !IsLoopbackHost(endpoint.host); });
}

/// Whether this node advertises an address peers can dial and then binds where it
/// cannot accept them.
///
/// The rule's THIRD spelling, and the one reached by obeying the second's advice. A
/// worker told to "name --advertise with an address peers can dial" sets
/// `worker-01.internal:6674` and stops -- and `--listen-node` still defaults to
/// loopback, so the advertised host is neither loopback nor the wildcard, both rows
/// above pass, and the surface is bound to `127.0.0.1`. The worker registers, is
/// leased out, and every dispatch fails to connect. Same silent shape, same
/// consequence, arrived at from the other side.
///
/// **Routable is decided SYNTACTICALLY and the host is never resolved.**
/// `StartupPolicyRejection` is a table of pure functions of the parsed configuration,
/// and a lookup here would make this daemon's ability to start depend on the network:
/// a resolver outage would become a refusal to boot, and an unbounded call would sit
/// in a table whose whole contract is that it needs nothing but argv. "Neither
/// loopback nor a wildcard" is enough -- a host that does not resolve today can be the
/// right one at the next boot (#208), exactly as `AdvertisesWildcard` documents.
///
/// **Its own conditions, never "not the other two".** An earlier draft defined this as
/// the negation of its siblings, on the reasoning that it made the three cases
/// mutually exclusive by construction. It did -- and then row 2 was narrowed, the
/// loopback/loopback cell stopped matching it, and fell straight through into THIS
/// row: a coherent single-machine fleet refused with a message about an address peers
/// cannot dial. A predicate defined by what its neighbours are not changes meaning
/// silently whenever a neighbour does, which is the same defect as two authors of one
/// fact, wearing the costume of a tidy invariant. Each row states what it is about.
/// @param cfg The parsed configuration.
/// @return Whether peers are told an address this node will not accept on.
[[nodiscard]] bool AdvertisesPastALoopbackBind(NodeConfig const& cfg)
{
    // A ROUTABLE advertise, decided syntactically: neither the wildcard nor loopback.
    // Both of those are other rows' business, and a loopback advertise over a loopback
    // bind is not any row's business at all -- it is the single-machine fleet.
    if (AdvertisesWildcard(cfg))
        return false;
    if (IsLoopbackHost(HostOfEndpoint(AdvertisedEndpoint(cfg))))
        return false;

    // The row is asked rather than `nodeListen` read, for the reason
    // `AdvertisedEndpoint` gives: a bare port's host is decided by
    // `NodeListenDefaultHost`, and the row is where that lives.
    auto const bound = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(bound, [](SurfaceEndpoint const& endpoint) { return IsLoopbackHost(endpoint.host); });
}

/// Whether this node registers with a scheduler that is on another machine.
///
/// Syntactic, never resolved, for the reason `AdvertisesPastALoopbackBind` gives: this
/// table is a set of pure functions of argv, and a lookup here would make the node's
/// ability to start depend on a resolver being up.
///
/// "Not loopback" rather than "routable", which is the same asymmetry
/// `AdvertisesWildcard` documents from the other side: a host that is down today can
/// be the right one at the next boot, so only the shapes that are wrong on every
/// machine and forever are judged. Loopback is one of exactly two things -- this
/// machine, or a typo -- and the caller below only cares which of those it is.
/// @param cfg The parsed configuration.
/// @return Whether `--scheduler` names a host that is not this machine's loopback.
[[nodiscard]] bool SchedulerIsRemote(NodeConfig const& cfg)
{
    return !cfg.scheduler.empty() && !IsLoopbackHost(HostOfEndpoint(cfg.scheduler));
}

/// Whether a worker registers an address only its own machine can reach, with a
/// scheduler that is not on that machine.
///
/// **The fourth cell of the reachability rule, and the one an operator reaches by
/// typing NOTHING.** Its three siblings all need a flag to have been written: the
/// wildcard and the loopback-past-a-network-bind rows judge an `--advertise` somebody
/// typed, and `AdvertisesPastALoopbackBind` judges a `--listen-node` they did not
/// widen after being told to name `--advertise`. A fleet worker started with neither
/// flag reaches none of them -- `--advertise` falls back to the `Node` surface, which
/// is loopback on a worker, so the advertise and the bind AGREE and the rule about
/// their disagreement has nothing to say. That configuration registers `127.0.0.1`
/// with a scheduler on another machine, heartbeats, is leased out, and every client
/// dials its own machine: the silent shape all four rows exist to refuse, arrived at
/// by leaving both flags off, which is precisely the line an operator omits (#463).
///
/// **What separates it from the single-machine fleet is WHERE THE SCHEDULER IS**, and
/// nothing else can. Loopback-bind-plus-loopback-advertise is a correct configuration
/// -- `dist-compile-e2e.sh` runs whole fleets that way -- so the bind cannot decide
/// this and neither can the advertise. A scheduler on another machine is the one part
/// of such a configuration that cannot also be true of a fleet living on this host.
///
/// Ordered after its siblings, which are more specific about the configurations they
/// share with it: a loopback advertise over a WIDE bind is answered by
/// `AdvertisesLoopbackFromAReachableBind`, whose message names the two flags that
/// disagree. This one answers what is left, where the two flags agree and both are
/// wrong for the fleet they were pointed at.
/// @param cfg The parsed configuration.
/// @return Whether a remote scheduler is handed an endpoint only this machine can dial.
[[nodiscard]] bool AdvertisesLoopbackToARemoteScheduler(NodeConfig const& cfg)
{
    if (!SchedulerIsRemote(cfg))
        return false;
    return IsLoopbackHost(HostOfEndpoint(AdvertisedEndpoint(cfg)));
}

/// An EMPTY bind address is the wildcard rather than a missing answer: it reaches
/// `getaddrinfo` as nullptr under AI_PASSIVE, the same third case `AdvertisesWildcard`
/// exists for. So it is reachable, and `IsLoopbackHost("")` answering false is the
/// behaviour this relies on rather than an accident. Why it is asked at all is on the
/// rule that asks it.
/// @param cfg The parsed configuration.
/// @return Whether either door onto the compile verbs answers anywhere but this
///         machine.
[[nodiscard]] bool CompilePortFacesTheNetwork(NodeConfig const& cfg)
{
    auto const nodePort = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(nodePort, [](SurfaceEndpoint const& endpoint) { return !IsLoopbackHost(endpoint.host); });
}

std::string AdmissionSummary(NodeConfig const& cfg)
{
    if (cfg.fleetOpen)
        return "every caller admitted";
    if (cfg.fleetMembers.empty())
        // Split on whether consensus runs, because a clustered node is about to
        // admit hosts nobody typed and a line that ignored that would be read as a
        // final answer. Both remedies are still named: the agreed member set ADDS to
        // what an operator listed rather than replacing it (#251), so
        // `--fleet-member` is worth giving on a clustered node too -- it is the only
        // route by which a machine that is not a cluster peer is admitted at all.
        return cfg.nodeId.empty() ? std::string { "this machine only -- give --fleet-member or --fleet-open to "
                                                  "admit peers" }
                                  : std::string { "this machine and the cluster's members -- give --fleet-member or "
                                                  "--fleet-open to admit callers that are not cluster peers" };

    // One sentence with a conditional tail rather than two whole ones: written twice
    // they drift, and a phrase an operator reads is exactly the thing nobody notices
    // has drifted.
    return std::format("this machine plus {} member host(s){}",
                       cfg.fleetMembers.size(),
                       cfg.nodeId.empty() ? "" : " and the cluster's members");
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
                     "whatever --listen-node resolves to, which defaults to loopback on a worker and is not an "
                     "address another machine can dial. Such a worker "
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
    // The value each listen flag takes, judged here rather than inside the tier that
    // binds it. Every one of these grammars used to be checked in its tier -- which
    // `--install-service` returns long before reaching -- so a typo registered
    // cleanly and then exited at every boot into a log nobody reads (#186).
    //
    // Walked off `NodeSurfaceTable()`, which is the port map. This used to be an
    // `EndpointFlags` table of its own, four rows carrying a flag spelling, a member
    // pointer, a grammar and a shape -- the same four columns for the same flags the
    // surface rows carry, which made it the fifth place the port map lived and the
    // one closest in shape to the table replacing it. Two tables with two member
    // pointers for one concept do not merely drift, they drift *silently*: the half
    // an operator is refused by and the half `--print-surfaces` prints them from
    // would have been different sets, so a worksheet could name a surface whose
    // spelling was never validated (#288).
    //
    // Each row asks its OWN surface's question, which is why `--discovery` carries a
    // different grammar: a beacon is sent TO an address, so no bare port may default
    // to a host, and a shared "is this an endpoint" test would accept `6681` here and
    // leave the tier to refuse it at every boot.
    //
    // **`--listen-raft` is now in this loop, and its absence used to be correct.**
    // The old comment said its own rules already refuse it absent and unusable, and
    // that a second row would answer in their place -- true while this table held
    // four hand-picked flags, and falsified by one that holds every surface. Leaving
    // it out would now need a column meaning "somebody else checks this one", which
    // is a column encoding an exception, in the table written to delete exceptions.
    //
    // What makes the coexistence safe is that both narrower rules still fire for the
    // inputs they were written about: `--node-id` with an EMPTY `--listen-raft` still
    // reaches the first, and a WELL-FORMED `--listen-raft` with no `--node-id` still
    // reaches the second. Only a malformed address moves here -- and it is better
    // answered here, because this message echoes what the operator typed and the
    // consensus prose can name a flag but not a value.
    //
    // "Parses when GIVEN", never "must parse". Empty is how five of the six say the
    // surface is off, and `--listen-node` carries a non-empty default every ordinary
    // node runs with -- so a rule spelled the other way would refuse the default
    // deployment outright.
    //
    // Asked before the rules below, because a value that is not an address is a typo
    // and answering it with a policy rule about the flag it was typed on describes
    // the wrong problem. Unlike those rules, whose messages are static prose, this
    // one echoes what the operator wrote -- which is the half a table row cannot do
    // and the half that matters when five ports were typed and one is wrong.
    for (auto const& surface: NodeSurfaceTable())
    {
        // The compile port has no spec text: its halves are `--bind` and `--port`,
        // each validated by its own value parser, so there is nothing here to judge.
        if (surface.spec == nullptr)
            continue;

        auto const& text = cfg.*surface.spec;
        if (text.empty() || surface.grammar.parses(text))
            continue;

        // "cannot use" rather than "cannot bind": most of these are bound and
        // discovery is sent to, and a message that named the wrong verb would send an
        // operator looking for a listening socket discovery never opens.
        return std::format("{}={} is not {}. The surface it configures cannot use an address that was never one, so "
                           "this node refuses to start.",
                           PrimaryFlag(surface),
                           text,
                           surface.grammar.shape);
    }

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
        { .refuses = [](NodeConfig const& c) { return c.serveScheduler && !c.fleetOpen && c.fleetMembers.empty(); },
          .message = "--serve-scheduler needs --fleet-member or --fleet-open: a scheduler with an empty member set "
                     "refuses every caller, which is the right default but not a working configuration. It would "
                     "start, bind, log nothing wrong, and decline the whole fleet." },
        { .refuses = [](NodeConfig const& c) { return c.fleetOpen && !c.fleetMembers.empty(); },
          .message = "--fleet-open and --fleet-member contradict each other: one admits everybody and the other "
                     "admits a list. Silently preferring either would make the narrower of the two a no-op an "
                     "operator believes is in force." },
        // There is deliberately NO mirror of the row above -- membership WITHOUT a
        // scheduler is the ordinary worker, not a mistake. This table used to refuse
        // it, on the reasoning that "a policy nothing consults is a policy an
        // operator believes is in force", and that premise was simply wrong: one
        // `NodeMembership` serves all three surfaces, and `WorkerServer` is
        // constructed unconditionally, so the policy is consulted on every node.
        //
        // What the row actually did was pin every non-scheduler node's oracle to an
        // empty list, which admits loopback and nothing else -- so the worker the
        // getting-started page documents refused every dispatched compile with
        // `NotAMember` (#235). The refusal a rule exists to prevent was the rule's
        // own doing, and it was invisible from the side anybody watches: the lease
        // WAS granted, so no scheduler counter moves. The only signal is the
        // worker's own `WorkerJobsRefusedNotAMember`, on a machine whose operator
        // has no reason to scrape it and which exports nothing without
        // `--admin-listen`. Hence `AdmissionSummary` in the ready line.
        //
        // What DOES need a row is the shape the row above newly makes reachable.
        // Admitting peers is only ever so they can dial this worker, and a worker
        // that registers a wildcard has told them to dial themselves -- so the two
        // halves have to be typed together or neither is worth anything. Scoped to a
        // node that registers with a scheduler, because that is what makes the
        // advertised endpoint travel: a node admitting peers to its CACHE tier is
        // reached at `--listen-node` and needs no advertise at all. And a node with
        // no membership flags is untouched, which is what keeps the one-machine
        // deployment -- no advertise, loopback clients, and correct -- working.
        { .refuses =
              [](NodeConfig const& c) {
                  return !c.scheduler.empty() && (c.fleetOpen || !c.fleetMembers.empty()) && AdvertisesWildcard(c);
              },
          .message = "--fleet-member and --fleet-open admit peers so that they can dial this worker, and --advertise "
                     "names no address they can dial: the wildcard resolves to "
                     "the CALLER's own machine. This worker would register, heartbeat, be leased out and never be "
                     "reached, with no error at either end. Name --advertise, or drop the membership flags and serve "
                     "this machine alone." },
        // The wildcard row's sibling, and it must stay a SEPARATE row. Since the bind
        // merged, `--advertise` falls back to the `Node` surface, which defaults to
        // loopback on a node that does not schedule -- so the shape an operator now
        // reaches by typing nothing is `127.0.0.1`, not `0.0.0.0`. It fails exactly as
        // silently and it is a different mistake: the wildcard is usually a bind
        // address pasted into the wrong flag, this one is having named no address at
        // all. One message for both would tell each operator about the other's error.
        //
        // Ordered after the wildcard so a configuration that is somehow both is
        // reported as the wildcard, which is the more specific diagnosis: first match
        // wins.
        //
        // The admission gate is IDENTICAL to the row above and deliberately so -- it
        // is what keeps the one-machine install working, where advertising loopback is
        // not a mistake but the correct answer. Widening the endpoint test while
        // relaxing this one would refuse the deployment an operator gets by installing
        // the package.
        { .refuses =
              [](NodeConfig const& c) {
                  return !c.scheduler.empty() && (c.fleetOpen || !c.fleetMembers.empty())
                         && AdvertisesLoopbackFromAReachableBind(c);
              },
          .message = "--fleet-member and --fleet-open admit peers so that they can dial this worker, --listen-node "
                     "accepts from the network, and --advertise names loopback -- so every peer is told to dial "
                     "ITSELF. This worker would register, heartbeat, be leased out and never be reached, with no "
                     "error at either end. Give --advertise=<this host>:6674, an address peers can dial. (A node "
                     "that binds loopback AND advertises loopback is a single-machine fleet and is fine; it is the "
                     "disagreement between the two that cannot work.)" },
        // The rule's third spelling, and the one an operator reaches by DOING WHAT THE
        // ROW ABOVE TELLS THEM: name --advertise with a routable address and stop.
        // Both rows above then pass while the surface is still on loopback, so the
        // worker is registered, leased out and unreachable -- the same silent shape,
        // entered through the remedy for it.
        //
        // A refusal that steers an operator into an unrefused failure is worse than no
        // refusal, which is why this is part of the same change rather than a
        // follow-up: the configuration could not arise before #290 stage 3, because
        // --bind defaulted to the wildcard.
        //
        // Disjoint from its siblings by construction -- `AdvertisesPastALoopbackBind`
        // is false whenever either of them is true -- so the order of the three is not
        // load-bearing and no configuration can be answered by the wrong one.
        { .refuses =
              [](NodeConfig const& c) {
                  return !c.scheduler.empty() && (c.fleetOpen || !c.fleetMembers.empty()) && AdvertisesPastALoopbackBind(c);
              },
          .message = "--advertise names an address peers can dial, but --listen-node binds loopback, so this worker "
                     "would never accept the connections it told them to make: it registers, heartbeats, is leased "
                     "out, and every dispatched compile fails to connect with no error at either end. Give "
                     "--listen-node=0.0.0.0:6674 so it accepts from the network, or drop the membership flags and "
                     "serve this machine alone." },
        // The rule's FOURTH spelling, and the only one an operator reaches by typing
        // neither flag. #463 asked whether `--listen-node` should widen itself for a
        // fleet participant so that this case stops arising; the answer is no -- a
        // default that widens a listening socket on a three-flag predicate is a
        // security decision nobody typed, and the defaulted bind is the one whose
        // failure to bind is a WARNING rather than fatal, so widening it would hand a
        // fleet worker the "no 0xFC port at all, still registers, still leased"
        // shape. What the ticket found is real, though: the three rows above all
        // judge a flag somebody wrote, so a worker started with neither `--advertise`
        // nor `--listen-node` sailed past all of them and registered loopback with a
        // remote scheduler. `NodeServiceRejection` refuses that at INSTALL time and
        // nothing refused it at a hand start, which is the reverse of the asymmetry
        // #166 composed the tables to delete.
        //
        // The admission gate is identical to its three siblings for the reason they
        // give, and the message names the two flags together because naming only
        // `--advertise` is what steers an operator into the row above.
        { .refuses =
              [](NodeConfig const& c) {
                  return !c.scheduler.empty() && (c.fleetOpen || !c.fleetMembers.empty())
                         && AdvertisesLoopbackToARemoteScheduler(c);
              },
          .message = "--scheduler names a machine that is not this one, and this worker would register loopback "
                     "with it -- neither --advertise nor --listen-node was given, so both fall back to an address "
                     "only this machine can dial. Every client the scheduler leases it to would dial ITSELF: the "
                     "worker registers, heartbeats, is leased out and is never reached, with no error at either "
                     "end. Give --listen-node=0.0.0.0:6674 and --advertise=<this host>:6674. (A fleet that really "
                     "is one machine names --scheduler on loopback too, and is fine.)" },
        { .refuses = [](NodeConfig const& c) { return c.raftJoin && c.nodeId.empty(); },
          .message = "--raft-join needs --node-id: a node waiting to be admitted to a cluster still has to have an "
                     "identity, because that is what the cluster admits and what every vote is counted against. "
                     "Without one it would listen forever and could never be named." },
        { .refuses = [](NodeConfig const& c) { return c.raftJoin && c.raftPeers.empty(); },
          .message = "--raft-join needs --raft-peer: at least this node's own address, which is the half only it "
                     "knows, and normally the cluster's as well. A joiner cannot answer the leader that admits it "
                     "without one, and it cannot learn any address until it has answered." },
        // After the two `--raft-join` rows, which say the same thing about a joiner
        // more precisely: first match wins, so putting this one ahead of them would
        // answer in their place and leave both stating a rule nothing reaches.
        // `ConsensusTier::Start` decided this from inside the tier, which the install
        // path returns long before reaching -- so a registration naming no reachable
        // self was written happily and then died at every boot (#168).
        { .refuses = [](NodeConfig const& c) { return !c.nodeId.empty() && ClusterSelfMember(c) == nullptr; },
          .message = NodeIdNamesNoPeerRefusal },
        // The third refusal `ConsensusTier::Start` made and no table did.
        // `ParseEndpoint` is asked here rather than `raftListen.empty()` because it
        // answers nullopt for an unusable port as readily as for a missing one, and
        // the tier decides on that same answer -- the way the `--dashboard` row
        // below judges the address `AdminEndpoint` will actually take rather than
        // the text an operator typed.
        { .refuses =
              [](NodeConfig const& c) {
                  // Asked of the raft row, which is what `ConsensusTier::Start`
                  // resolves through -- so this rule and the tier decide on one
                  // answer. The row's own `--node-id` gate is why the id is tested
                  // first: without it the row resolves to nothing for a reason that
                  // is not this rule's.
                  return !c.nodeId.empty() && RowFor(NodeSurface::Raft).Resolve(c).empty();
              },
          .message = "--node-id needs a usable --listen-raft: consensus is what --node-id turns on, and that port "
                     "is where every peer dials this node. Without one nothing binds, no vote could arrive, and "
                     "the node refuses to start rather than join a cluster that cannot see it." },
        // The other half of the same flag group: consensus configured with the
        // switch that turns it on left off. `--cluster-dir` is deliberately NOT
        // here -- `FleetHistoryPath` reads it for the dashboard's history file, so
        // a node running no consensus still has a use for it.
        { .refuses = [](NodeConfig const& c) { return c.nodeId.empty() && (!c.raftListen.empty() || !c.raftPeers.empty()); },
          .message = "--listen-raft and --raft-peer configure consensus, and --node-id is what turns consensus ON: "
                     "without one this node runs none, so the port is never bound and the peers are never dialled. "
                     "Nothing would say so, which is the silent no-op this list exists to refuse. Give --node-id, "
                     "or drop them." },
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
        // NOT here: a rule refusing `--cluster-key-file` when nothing reads it.
        //
        // It has been narrowed twice and is now gone, and each step is the same
        // mistake caught later. It began as "unless --discovery"; the scheduler
        // became a second reader when it started SIGNING grants (#281) and the rule
        // turned a correct configuration into a node that would not start. It was
        // widened to "unless --discovery or the scheduler tier"; the worker became a
        // third reader here (#282), and the rule immediately refused the configuration
        // the rule below REQUIRES -- a plain worker admitting remote peers, which
        // needs the key precisely and runs neither of the other two surfaces.
        //
        // There is no fourth narrowing to make. Whether a worker tier exists is not a
        // fact about the configuration at all: it depends on what `--toolchain` and
        // discovery resolve to on this machine, which happens long after this table
        // runs. A refusal whose premise has become false is worse than no refusal, and
        // one that cannot state its premise without guessing has no business firing.
        // Newly reachable since the surfaces merged (#290), and silent without a row:
        // the scheduler verbs are answered on `--listen-node`, so emptying that flag
        // closes the port the scheduler would have been reached on. Before the merge
        // these were two flags naming two ports and neither could cancel the other; now
        // one can, and a node would start, log a scheduler, and answer nobody.
        { .refuses = [](NodeConfig const& c) { return c.serveScheduler && c.nodeListen.empty(); },
          .message = "--serve-scheduler needs --listen-node: the scheduler verbs are answered on the node's 0xFC "
                     "port, beside the cache verbs, and an empty --listen-node closes it. There would be nothing "
                     "left for a peer to dial." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && c.adminListen.empty(); },
          .message = "--dashboard needs --admin-listen: the dashboard is served on the admin surface, and "
                     "without one there is no port for it to answer on. It would start, log nothing wrong, "
                     "and serve the page to nobody." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && !c.serveScheduler; },
          .message = "--dashboard needs --serve-scheduler: a node that runs no scheduler never leads a fleet, "
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
        // A worker that admits OTHER machines must be able to check the grants they
        // present. The scheduler signs a lease; without the cluster key this node
        // cannot verify that signature, so its compile port would serve whoever can
        // reach it -- the fleet's CPU spent on an unauthenticated request (#282).
        //
        // A STARTUP refusal rather than a per-request fallback, and that distinction
        // is the rule rather than an implementation detail. "No key, so skip the
        // check" decided per request is silent degradation of exactly the kind this
        // list exists to refuse: the surface is open, every refusal counter reads
        // zero, and the fleet looks healthy from both ends. Decided once, before
        // anything is served, it is a node that states what it cannot do.
        //
        // Scoped to the reachable-and-admitted node rather than to "is a key
        // configured", because that is the question -- can a machine that is not this
        // one reach the compile surface. It has two halves and EITHER closes it: a
        // node bound to loopback answers nobody else whatever its policy says, and a
        // node admitting only its own machine escalates nobody however it is bound. A
        // process on this host already has this host's compiler, so refusing either
        // shape would break every single-machine install to prevent nothing.
        //
        // See #303, which asks the same question of the scheduler and should take
        // this shape rather than "is a key configured".
        { .refuses =
              [](NodeConfig const& c) {
                  return c.clusterKeyFile.empty() && CompilePortFacesTheNetwork(c) && AdmitsRemotePeers(c);
              },
          .message = "a node that admits peers on other machines needs --cluster-key-file: the scheduler signs "
                     "the lease a client presents to a worker, and without the key this node cannot check that "
                     "signature -- so it would compile for anybody who can reach its port, and report nothing "
                     "wrong while doing it. A node admitting only its own machine needs no key, because a "
                     "process on this host already has this host's compiler." },
        // The rule that keeps a fleet map off an open port. Loopback needs no
        // credential -- reaching it already means being on the machine -- but a
        // bind an operator deliberately exposed does, and HTTPS alone does not
        // supply it: TLS authenticates the SERVER to the browser and says nothing
        // about who the browser is.
        { .refuses =
              [](NodeConfig const& c) {
                  if (!c.dashboard || c.adminListen.empty() || !c.dashboardTokenFile.empty())
                      return false;
                  // The address `AdminEndpoint::Start` will actually take, asked of
                  // the surface's own row rather than derived here from the same
                  // constant. This is the ONE security decision that hangs on the
                  // loopback default, so a second author of the resolution is the
                  // last place to keep one -- and the admin row could grow a
                  // condition the way the raft row already has (`--node-id`), at
                  // which point a copy would judge an address the surface no longer
                  // binds and silently stop requiring a credential.
                  auto const endpoints = RowFor(NodeSurface::Admin).Resolve(c);
                  return !endpoints.empty() && !IsLoopbackHost(endpoints.front().host);
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
