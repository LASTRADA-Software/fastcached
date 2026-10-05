// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <format>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace
{

    /// One place a node may keep its state: which origin it stands for, whether it applies to
    /// this process, and where it is. `ConfigCandidate`'s shape, for the same reason -- the rows are
    /// a lookup order, and the next location is a row rather than a branch.
    struct StateDirectoryRow
    {
        StateDirectoryOrigin origin; ///< What choosing this row means, for the report.
        /// Whether the row applies to a process with the privilege given.
        bool (*applies)(bool privileged);
        /// Environment variable holding the base; empty when `parent` is already absolute.
        std::string_view baseVar;
        /// Under the base, or absolute; empty to take the base as it is.
        std::string_view parent;
        /// Whether `NodeStateDirectoryName` is appended. False only where the base already IS
        /// this node's directory, which is what a service manager hands over.
        bool appendsName;
    };

    constexpr auto WhenPrivileged = [](bool privileged) {
        return privileged;
    };
    constexpr auto WhenUnprivileged = [](bool privileged) {
        return !privileged;
    };

    /// The lookup, in priority order; the first row that applies and resolves wins.
    ///
    /// **The service manager's row comes first on POSIX**, because the shipped systemd unit runs
    /// as an unprivileged account whose home is `/`: without it the machine-wide service would
    /// take the PER-USER row and keep its identity under `/.local/state`, which
    /// `ProtectSystem=strict` refuses. `StateDirectory=fastcache-node` is what makes systemd hand
    /// over `STATE_DIRECTORY`, and a launchd registration states the same variable. Windows has
    /// no such convention and needs none: a service's token carries the service SID, which
    /// `IsPrivilegedProcess` counts.
#if defined(_WIN32)
    constexpr auto StateDirectoryRows = std::to_array<StateDirectoryRow>({
        { .origin = StateDirectoryOrigin::MachineWide,
          .applies = WhenPrivileged,
          .baseVar = "ProgramData",
          .parent = "",
          .appendsName = true },
        { .origin = StateDirectoryOrigin::PerUser,
          .applies = WhenUnprivileged,
          .baseVar = "LOCALAPPDATA",
          .parent = "",
          .appendsName = true },
    });
#else
    #if defined(__APPLE__)
    constexpr std::string_view MachineStateParent = "/Library/Application Support";
    #else
    constexpr std::string_view MachineStateParent = "/var/lib";
    #endif
    constexpr auto Always = [](bool /*privileged*/) {
        return true;
    };
    constexpr auto StateDirectoryRows = std::to_array<StateDirectoryRow>({
        { .origin = StateDirectoryOrigin::ServiceManager,
          .applies = Always,
          .baseVar = ServiceStateDirectoryVariable,
          .parent = "",
          .appendsName = false },
        { .origin = StateDirectoryOrigin::MachineWide,
          .applies = WhenPrivileged,
          .baseVar = "",
          .parent = MachineStateParent,
          .appendsName = true },
        { .origin = StateDirectoryOrigin::PerUser,
          .applies = WhenUnprivileged,
          .baseVar = "XDG_STATE_HOME",
          .parent = "",
          .appendsName = true },
        { .origin = StateDirectoryOrigin::PerUser,
          .applies = WhenUnprivileged,
          .baseVar = "HOME",
          .parent = ".local/state",
          .appendsName = true },
    });
#endif

    static_assert(std::ranges::any_of(StateDirectoryRows,
                                      [](StateDirectoryRow const& row) {
                                          return row.origin == StateDirectoryOrigin::MachineWide;
                                      }),
                  "every platform needs a machine-wide row: it is where a service keeps its identity");

    /// Why a node keeps its state where it does, in the words `--print-surfaces` and
    /// `--node-status` print.
    constexpr auto OriginReasons = EnumTable<StateDirectoryOrigin, std::string_view> {
        "named by --cluster-dir",
        "handed over by the service manager (STATE_DIRECTORY)",
        "machine-wide: this process runs privileged, so it is the machine's own node",
        "per-user: this process is not privileged, so it keeps its identity apart from the machine's service",
    };
    static_assert(std::ranges::none_of(OriginReasons, [](std::string_view text) { return text.empty(); }),
                  "every origin needs words; an empty one reads as no answer at all");

    /// The warning's second half for an outcome that falls back to the bare host name.
    /// @param why Why the start fell back.
    /// @param resolved What it resolved.
    /// @return The whole sentence.
    [[nodiscard]] std::string BareNameWarning(std::string const& why, ResolvedHostNames const& resolved)
    {
        return std::format("{}; peers are told to dial the bare host name '{}' instead, which resolves only where "
                           "their DNS search list completes it. Set --advertise and --raft-self to a name every peer "
                           "resolves, or give this machine a DNS domain",
                           why,
                           resolved.names.fqdn);
    }

    /// What a start that did not get a qualified name says, one row per outcome; null for the one
    /// that needs no warning. Whole sentences, because the outcomes do not share a consequence: a
    /// bare name resolves where a search list completes it, a withheld one is offered to nobody.
    constexpr auto HostNamingWarnings = EnumTable<HostNamingOutcome, std::string (*)(ResolvedHostNames const&)> {
        nullptr,
        [](ResolvedHostNames const& resolved) {
            return BareNameWarning(
                std::format("this machine names itself '{}', a placeholder domain only this machine resolves",
                            resolved.declined),
                resolved);
        },
        [](ResolvedHostNames const& resolved) {
            return BareNameWarning(
                std::format("this machine's fully qualified name did not arrive within {} ms", resolved.bound.count()),
                resolved);
        },
        [](ResolvedHostNames const& resolved) {
            return std::format(
                "this machine names itself '{}', and '{}' reaches only this machine: every peer told to dial it would "
                "reach ITSELF. So it is offered to no peer -- nothing is advertised to a scheduler elsewhere, and "
                "unless --raft-self names this node its consensus is confined to loopback, a fleet of its own that "
                "can neither form nor join one -- and the node serves this machine alone. Set --advertise and --raft-self "
                "to an address or a name other machines resolve, or give "
                "this machine a real host name",
                resolved.declined.empty() ? resolved.names.withheld : resolved.declined,
                resolved.names.withheld);
        },
    };

    /// The state `ThreadedHostNamingLookup` shares with the thread doing the read, which may
    /// outlive the caller that stopped waiting for it.
    struct HostNamingHandover
    {
        std::mutex mutex;
        std::condition_variable arrived;
        std::unique_ptr<IHostNaming> answer; ///< Guarded by `mutex`.
        bool done { false };                 ///< Guarded by `mutex`.
    };

    /// The directory a service manager handed over, when it is this node's.
    ///
    /// systemd lists every `StateDirectory=` a unit declares, `:`-separated, and a CHILD inherits
    /// the variable -- so a node started from some other service's shell would otherwise keep its
    /// identity in that service's directory. Only an absolute entry named
    /// `NodeStateDirectoryName` is this node's.
    /// @param value The variable's value.
    /// @return The directory, or nothing when no entry is this node's.
    [[nodiscard]] std::optional<std::filesystem::path> ServiceManagerDirectory(std::string_view value)
    {
        for (auto const entry: value | std::views::split(':'))
        {
            auto path = std::filesystem::path { std::string_view { entry.begin(), entry.end() } };
            if (path.is_absolute() && path.filename() == NodeStateDirectoryName)
                return path;
        }
        return std::nullopt;
    }

    /// Where one row puts the directory, or nothing when its base is not set.
    /// @param row The row.
    /// @param probe Where the environment is read.
    /// @return The directory, or nothing.
    [[nodiscard]] std::optional<std::filesystem::path> DirectoryOf(StateDirectoryRow const& row,
                                                                   IConfigPathProbe const& probe)
    {
        auto under = std::filesystem::path { row.parent };
        if (row.appendsName)
            under /= NodeStateDirectoryName;
        if (row.baseVar.empty())
            return under;

        // An unset or empty base skips the row: an empty one would make the directory relative to
        // the working directory, which for a service is `System32` or `/`. So does a RELATIVE one,
        // for the same reason and on the XDG Base Directory specification's own terms: "If a
        // relative path is set in any of these variables, it is considered invalid and should be
        // ignored."
        auto const base = probe.GetEnv(row.baseVar);
        if (!base.has_value() || base->empty())
            return std::nullopt;
        if (!row.appendsName)
            return ServiceManagerDirectory(*base);
        auto const basePath = std::filesystem::path { *base };
        if (!basePath.is_absolute())
            return std::nullopt;
        return basePath / under;
    }

} // namespace

std::string_view DescribeStateDirectoryOrigin(StateDirectoryOrigin origin) noexcept
{
    return OriginReasons[static_cast<std::size_t>(origin)];
}

std::optional<NodeStateDirectoryChoice> DefaultNodeClusterDirectory(IConfigPathProbe const& probe)
{
    auto const privileged = probe.IsPrivilegedProcess();
    for (auto const& row: StateDirectoryRows)
    {
        if (!row.applies(privileged))
            continue;
        if (auto directory = DirectoryOf(row, probe); directory.has_value())
            return NodeStateDirectoryChoice { .path = *std::move(directory), .origin = row.origin };
    }
    return std::nullopt;
}

void ApplyNodeStateDirectory(NodeConfig& cfg, IConfigPathProbe const& probe)
{
    cfg.stateDirectory = DefaultNodeClusterDirectory(probe);
}

void ApplyHostNames(NodeConfig& cfg, NodeHostNames names)
{
    cfg.hostNames = std::move(names);
}

void ApplyHostNaming(NodeConfig& cfg, IHostNaming const& naming)
{
    ApplyHostNames(
        cfg, NodeHostNames { .fqdn = naming.FullyQualifiedName(), .dnsSuffix = naming.PrimaryDnsSuffix(), .withheld = {} });
}

ThreadedHostNamingLookup::ThreadedHostNamingLookup(std::function<std::unique_ptr<IHostNaming>()> read):
    _read { std::move(read) }
{
}

std::unique_ptr<IHostNaming> ThreadedHostNamingLookup::ReadWithin(std::chrono::milliseconds bound)
{
    // Detached, and that is the point rather than an omission: a resolver that has not answered
    // is one this process cannot interrupt, and joining it would make the bound a fiction. What
    // it writes is the shared handover, which it co-owns, so a read landing after the caller gave
    // up writes into memory still alive and is then freed by the last owner.
    auto handover = std::make_shared<HostNamingHandover>();
    std::thread { [handover, read = _read] {
        auto answer = read();
        {
            auto const lock = std::scoped_lock { handover->mutex };
            handover->answer = std::move(answer);
            handover->done = true;
        }
        handover->arrived.notify_all();
    } }.detach();

    // A condition variable rather than an atomic wait, whose return is not tied to the notify.
    auto lock = std::unique_lock { handover->mutex };
    if (!handover->arrived.wait_for(lock, bound, [&handover] { return handover->done; }))
        return nullptr;
    return std::move(handover->answer);
}

namespace
{
    /// The names as read, before the withholding rule.
    [[nodiscard]] ResolvedHostNames ReadNames(IHostNamingLookup& lookup,
                                              std::string_view bareHostName,
                                              std::chrono::milliseconds bound)
    {
        auto const naming = lookup.ReadWithin(bound);
        if (naming == nullptr)
            return ResolvedHostNames {
                .names = NodeHostNames { .fqdn = std::string { bareHostName }, .dnsSuffix = {}, .withheld = {} },
                .outcome = HostNamingOutcome::TimedOut,
                .declined = {},
                .bound = bound
            };

        auto declined = naming->DeclinedName();
        auto const outcome = declined.empty() ? HostNamingOutcome::Resolved : HostNamingOutcome::Declined;
        return ResolvedHostNames { .names = NodeHostNames { .fqdn = naming->FullyQualifiedName(),
                                                            .dnsSuffix = naming->PrimaryDnsSuffix(),
                                                            .withheld = {} },
                                   .outcome = outcome,
                                   .declined = std::move(declined),
                                   .bound = bound };
    }
} // namespace

ResolvedHostNames ResolveHostNames(IHostNamingLookup& lookup, std::string_view bareHostName, std::chrono::milliseconds bound)
{
    auto resolved = ReadNames(lookup, bareHostName, bound);

    // **Whatever produced it**: a platform answer, the bare host name a timed-out lookup falls back
    // to, or the first label a placeholder domain was refused for -- RHEL's unset default is
    // `localhost.localdomain`, which the placeholder rule reduces to exactly `localhost`. A name
    // every machine resolves to itself is withheld, never offered: a peer told to dial it reaches
    // ITSELF, which is a confident wrong signal where no name at all is a vague right one.
    if (NamesOnlyThisMachine(resolved.names.fqdn))
    {
        resolved.names = NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = std::move(resolved.names.fqdn) };
        resolved.outcome = HostNamingOutcome::ReachesOnlyThisMachine;
    }
    return resolved;
}

std::optional<std::string> HostNamingWarning(ResolvedHostNames const& resolved)
{
    auto const warning = HostNamingWarnings[static_cast<std::size_t>(resolved.outcome)];
    if (warning == nullptr)
        return std::nullopt;
    return warning(resolved);
}

std::optional<std::string> UnqualifiedHostNameInUse(NodeConfig const& cfg)
{
    if (!cfg.hostNames.has_value())
        return std::nullopt;
    auto const& name = cfg.hostNames->fqdn;
    if (name.empty() || name.contains('.'))
        return std::nullopt;

    // Asked of the two endpoints a peer is told to dial, through the derivations that tell
    // them, rather than re-deciding here when each falls back to the name.
    std::vector<std::string_view> through;
    if (cfg.advertise.empty() && HostOfEndpoint(AdvertisedEndpoint(cfg)) == name)
        through.emplace_back("--advertise");
    if (cfg.raftSelf.empty())
        if (auto const dial = ConsensusDialAddressOf(cfg); dial.has_value() && HostOfEndpoint(*dial) == name)
            through.emplace_back("--raft-self");
    if (through.empty())
        return std::nullopt;

    return std::format("peers are told to dial '{}' in place of {}: a name with no domain, which resolves only where "
                       "a DNS search list completes it",
                       name,
                       through.size() == 1 ? std::string { through.front() }
                                           : std::format("{} and {}", through.front(), through.back()));
}

std::optional<std::string> NameReachesOnlyThisMachineInUse(NodeConfig const& cfg)
{
    if (!cfg.hostNames.has_value() || cfg.hostNames->withheld.empty())
        return std::nullopt;

    // What stood down, asked of the predicates that stood it down rather than re-decided here.
    std::vector<std::string_view> consequences;
    if (AdvertisedNameWithheld(cfg))
        consequences.emplace_back(SchedulerIsRemote(cfg)
                                      ? "nothing is advertised to a scheduler in place of --advertise"
                                      : "its worker is advertised at loopback, to this machine's own scheduler");
    if (ConsensusConfinedToThisMachine(cfg))
        consequences.emplace_back("it runs as a fleet of its own on loopback and can neither form nor join a fleet "
                                  "-- fix this machine's DNS, or give --raft-self and --advertise");
    if (consequences.empty())
        return std::nullopt;

    auto said = std::format("this machine names itself '{}', which every machine resolves to itself, so it is offered "
                            "to no peer: {}",
                            cfg.hostNames->withheld,
                            consequences.front());
    for (auto const& consequence: consequences | std::views::drop(1))
        said += std::format(", and {}", consequence);
    return said;
}

void EvaluateHostNameCondition(NodeConditions& conditions, NodeConfig const& cfg)
{
    // One row per question, walked: each evaluator answers its own detail or nothing.
    struct HostNameRow
    {
        NodeCondition condition;
        std::optional<std::string> (*inUse)(NodeConfig const&);
    };
    constexpr auto Rows = std::to_array<HostNameRow>({
        { .condition = NodeCondition::UnqualifiedHostName, .inUse = &UnqualifiedHostNameInUse },
        { .condition = NodeCondition::HostNameReachesOnlyThisMachine, .inUse = &NameReachesOnlyThisMachineInUse },
    });
    for (auto const& row: Rows)
    {
        if (auto const detail = row.inUse(cfg); detail.has_value())
            conditions.Raise(row.condition, *detail);
        else
            conditions.Clear(row.condition);
    }
}

std::optional<std::filesystem::path> MachineWideNodeClusterDirectory(IConfigPathProbe const& probe)
{
    // The machine-wide rows, asked whatever the privilege: a registration names where the SERVICE
    // keeps its identity, not where the installing process would keep its own.
    for (auto const& row: StateDirectoryRows)
    {
        if (row.origin != StateDirectoryOrigin::MachineWide)
            continue;
        if (auto directory = DirectoryOf(row, probe); directory.has_value())
            return directory;
    }
    return std::nullopt;
}

bool ServiceManagerHandsOverStateDirectory() noexcept
{
    return std::ranges::any_of(
        StateDirectoryRows, [](StateDirectoryRow const& row) { return row.origin == StateDirectoryOrigin::ServiceManager; });
}

} // namespace FastCache::Node
