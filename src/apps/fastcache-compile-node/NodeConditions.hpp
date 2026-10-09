// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/NodeConditionWire.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <core/net/AcceptLoopHealth.hpp>

/// @file NodeConditions.hpp
/// What this node has detected that an operator must act on, as ONE table (#1364).
///
/// Every condition here was log-only before this existed, and a log line scrolls away: across
/// forty machines nobody reads forty logs, and the operator who needs to know is the one who
/// arrives an hour later on a machine they did not start. The table is the one source of truth;
/// `NodeStatus` carries it to `fastcache-cli node` and `node-conditions`, `NodeAnnounce` carries
/// it to the leader's fleet page, and the `node` panel of `live-stats` reads it off the status
/// its stream already pushes. **No renderer restates the list** -- each walks what arrived, and
/// the row text travels with it.
///
/// The log lines stay. The table is an additional carrier, not a replacement: a line is what a
/// person watching startup reads, and the table is what somebody arriving later can ask for.

namespace FastCache::Node
{

/// Which condition a row describes.
///
/// **PRIVATE: persisted and transmitted nowhere.** What travels is the row's `id` text, so the
/// enumerator values bind nothing and a row may be inserted anywhere; the explicit `= 0` is
/// `EnumTable`'s anchor rather than a contract. Declaration order is the order every renderer
/// lists the rows in.
enum class NodeCondition : std::uint8_t
{
    CounterTableSkew = 0,           ///< The metrics catalogue names counters this build's sink has no slot for (#1362).
    ScratchRootUnmappable,          ///< The worker's scratch root cannot be written into a debug-prefix-map rule (#810).
    GeneratedTlsCertificate,        ///< The admin surface serves a certificate generated at startup.
    EnrollmentWindowOpen,           ///< An armed auto-approve window admits whoever asks, unexamined (#1298).
    UnreadableLeaderSnapshot,       ///< This build cannot read the snapshot its leader sends, so it stays behind (#1552).
    UnqualifiedHostName,            ///< Peers are told to dial a host name with no domain.
    HostNameReachesOnlyThisMachine, ///< This machine's name reaches only itself, so nothing offers it.
    EnrollmentRequestsWaiting,      ///< Machines have asked to join and nobody has decided about them.
    ForeignFleetVisible,     ///< Another fleet proves itself on this segment that this node will not merge with or join.
    SharedCacheUnavailable,  ///< The fleet names this machine its shared cache, and its shared tier will not open.
    SharedCacheUnproven,     ///< The fleet names another machine its shared cache, and this node is not reaching it.
    SchedulerUnreachable,    ///< A scheduler this node registers with does not answer a dial.
    UnservedToolchain,       ///< Clients asked the leader for a toolchain no live worker serves.
    MixedNodeVersions,       ///< The leader sees one wire served by more than one build.
    OwnRecordAwaited,        ///< Its own cluster has not recorded this node's key, so it announces to nobody.
    RefusedCompileArguments, ///< The worker refused compiles over arguments it will not pass to its compiler.
    SurfaceNotAccepting,     ///< A serving surface's accept loop has ended: its port listens and refuses.
    SurfaceAcceptDegraded,   ///< A serving surface's accept loop is backing off on failures nothing classifies.
    FleetSplitHealing, ///< This fleet and another are one fleet split in two: healing by itself, or waiting on an operator.
    FormationMoveRefused,   ///< A move of this node's formation was refused: the startup rules refuse the shape it moves to.
    ConsensusLeaderSilent,  ///< No leader this node's applied configuration counts has spoken for too long to trust its
                            ///< grants.
    StateDirectoryUnsynced, ///< The state directory's filesystem cannot sync a directory, so its replaces may not survive
                            ///< a power loss.
    Last,                   ///< Not a condition.
};

/// Which of this node's components evaluates a row.
///
/// A node may run none of a scope's component, and then the row is answered `NotEvaluated` with
/// the scope's reason -- by `NodeConditions::Settle`, walking this, rather than by each place that
/// decided not to build the component remembering to say so. Private for `NodeCondition`'s reason.
enum class ConditionScope : std::uint8_t
{
    Process = 0,  ///< Every process: evaluated at startup, before any component is built.
    Worker,       ///< The worker tier; absent with `--slots=0`.
    Scheduler,    ///< The scheduler tier; absent unless the node's mode serves one (`ServesScheduler`).
    AdminSurface, ///< The admin HTTP surface; absent without `--admin-listen`.
    Enrollment,   ///< The enrollment window; served only by a consensus node that also schedules.
    Consensus,    ///< Consensus and the cluster state it replicates; absent without `--listen-raft`.
    Announce,     ///< The announce loops; absent when this node registers with no scheduler (`AnnouncesToAScheduler`).
    Last,         ///< Not a scope.
};

/// Which of the scoped components this node actually built -- observed, never read off the flags
/// that asked for them, for the reason `NodeComponents` is.
struct PresentComponents
{
    bool worker;       ///< A worker tier exists.
    bool scheduler;    ///< A scheduler tier exists.
    bool adminSurface; ///< An admin endpoint is serving.
    bool enrollment;   ///< An enrollment window is reachable.
    bool consensus;    ///< Consensus runs.
    bool announces;    ///< The announce loops run: this node registers somewhere (`AnnouncesToAScheduler`).
};

/// One scope: which `PresentComponents` member says it runs, and what a row of it answers when it
/// does not.
struct ConditionScopeRow
{
    ConditionScope scope; ///< The enumerator this row describes.
    /// Whether this node runs the scope's component; null for `Process`, which every node runs.
    bool PresentComponents::* present;
    /// The `NotEvaluated` detail a row of this scope carries on a node that runs none of it.
    std::string_view notEvaluated;
};

/// Every scope, in enumerator order.
inline constexpr EnumTable<ConditionScope, ConditionScopeRow> ConditionScopeTable { {
    { .scope = ConditionScope::Process, .present = nullptr, .notEvaluated = {} },
    { .scope = ConditionScope::Worker,
      .present = &PresentComponents::worker,
      .notEvaluated = "this node runs no worker (--slots=0)" },
    { .scope = ConditionScope::Scheduler,
      .present = &PresentComponents::scheduler,
      .notEvaluated = "this node runs no scheduler (its mode serves none, or its consensus is closed)" },
    { .scope = ConditionScope::AdminSurface,
      .present = &PresentComponents::adminSurface,
      .notEvaluated = "this node serves no admin surface (no --admin-listen)" },
    { .scope = ConditionScope::Enrollment,
      .present = &PresentComponents::enrollment,
      .notEvaluated = "this node serves no enrollment window: that takes consensus (--listen-raft) and a scheduler "
                      "(its mode)" },
    { .scope = ConditionScope::Consensus,
      .present = &PresentComponents::consensus,
      .notEvaluated = "this node runs no consensus (no --listen-raft), so no cluster state reaches it" },
    { .scope = ConditionScope::Announce,
      .present = &PresentComponents::announces,
      .notEvaluated = "this node registers with no scheduler, so it dials none that could be unreachable" },
} };
static_assert(RowsInEnumeratorOrder(ConditionScopeTable, &ConditionScopeRow::scope),
              "ConditionScopeTable must hold one row per ConditionScope, in enumerator order");

/// One condition: what travels about it, and who evaluates it.
struct NodeConditionRow
{
    NodeCondition condition; ///< The enumerator this row describes.
    /// Stable and kebab-case: what `node-conditions` prints, what the fleet page lists and what a
    /// script keys on. Renaming one is a change every reader sees.
    std::string_view id;
    CompileCacheWire::ConditionPersistence persistence; ///< Whether it can clear while this process runs.
    CompileCacheWire::ConditionSeverity severity;       ///< How loudly it asks, when raised.
    ConditionScope scope;                               ///< Which component evaluates it.
    /// What to do about it, for somebody who has never seen it.
    ///
    /// **The only part of a row most people read, and nothing tests what it SAYS**, so each is
    /// written against one question: if I did exactly this, where do I end up? A remedy that sends
    /// its reader somewhere the condition persists is worse than none.
    std::string_view remedy;
};

/// Every condition this node can report, in the order every surface lists them.
inline constexpr EnumTable<NodeCondition, NodeConditionRow> NodeConditionTable { {
    { .condition = NodeCondition::CounterTableSkew,
      .id = "counter-table-skew",
      .persistence = CompileCacheWire::ConditionPersistence::Latched,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Process,
      .remedy = "Rebuild this binary from one clean build tree and redeploy it: its metrics catalogue and its counter "
                "sink were compiled against different versions of the counter list, so the counters named here are "
                "missing from every scrape and stream. Nothing else is affected, and restarting the same binary will "
                "not clear it." },
    { .condition = NodeCondition::ScratchRootUnmappable,
      .id = "scratch-root-unmappable",
      .persistence = CompileCacheWire::ConditionPersistence::Latched,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Worker,
      .remedy = "Point TMPDIR (TEMP on Windows) at a directory whose path has no whitespace, no '=' and no control "
                "character, then restart the node: the scratch root is chosen once, at startup. Compiles are "
                "unaffected meanwhile; only the debug names inside objects this worker builds for other machines "
                "record its own scratch path instead of the client's." },
    { .condition = NodeCondition::GeneratedTlsCertificate,
      .id = "generated-tls-certificate",
      .persistence = CompileCacheWire::ConditionPersistence::Latched,
      .severity = CompileCacheWire::ConditionSeverity::Notice,
      .scope = ConditionScope::AdminSurface,
      .remedy = "Compare this fingerprint with the one your browser shows before trusting the page: nothing signs the "
                "certificate, and it changes on every restart. To stop comparing, replace --tls-self-signed with "
                "--tls-cert and --tls-key naming a certificate your browsers already trust." },
    { .condition = NodeCondition::EnrollmentWindowOpen,
      .id = "enrollment-window-open",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Enrollment,
      .remedy = "Check who got in with --enroll-list, which marks each auto-approved row with when the window was "
                "armed, and compare each key with the one its machine printed. --enroll-auto-approve=off ends the "
                "window; so does a restart or a change of leader, since it is held in the leader's memory alone." },
    { .condition = NodeCondition::UnreadableLeaderSnapshot,
      .id = "unreadable-leader-snapshot",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Consensus,
      .remedy = "Run the build this node's leader runs: it cannot read the cluster state the leader sends, so it stays "
                "where it was rather than take on a state it cannot hold, and follows no change the cluster makes -- "
                "members, settings, forgets -- until it can. It catches up by itself once it reads the leader's "
                "snapshot; nothing needs moving aside. A fleet upgrades as one: see "
                "docs/operations/upgrading-a-fleet.md." },
    { .condition = NodeCondition::UnqualifiedHostName,
      .id = "unqualified-host-name",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Process,
      .remedy = "Set --advertise (advertise in the configuration file) and --raft-self (raft_self) -- a reload "
                "applies both -- to a name every peer resolves, or to an address; or give this machine a DNS "
                "domain and restart the node, since the name is read once at startup. Until then a peer whose DNS "
                "search list does not complete the name cannot reach this node, which registers and answers nobody "
                "there." },
    { .condition = NodeCondition::HostNameReachesOnlyThisMachine,
      .id = "host-name-reaches-only-this-machine",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Process,
      .remedy = "Fix this machine's DNS so its name resolves for other machines, and restart (the name is read once "
                "at startup); or set --raft-self (raft_self) and --advertise (advertise) to an address or name they "
                "resolve: a reload applies both to what this node advertises, but the consensus port this confined to "
                "loopback is bound again only by a restart. Until then this node is a fleet of its own on loopback -- "
                "its own scheduler and worker, no discovery -- that can neither form nor join a fleet." },
    { .condition = NodeCondition::EnrollmentRequestsWaiting,
      .id = "enrollment-requests-waiting",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Notice,
      .scope = ConditionScope::Enrollment,
      .remedy = "Run --enroll-list and compare each machine's key with the one that machine printed; admit one with "
                "--enroll-approve=<id>@<key>, which admits exactly the key named, or refuse it with --enroll-reject=<id>. "
                "A machine that stops asking for ten minutes is forgotten." },
    { .condition = NodeCondition::ForeignFleetVisible,
      .id = "foreign-fleet-visible",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "Decide which fleet each machine named here belongs to, and --cluster-forget it from the other: "
                "established fleets merge only when a key this fleet holds proves them one fleet split in two, and "
                "one that merely CLAIMS a machine of this one never merges. For a fleet --fleet-id keeps this node "
                "out of, change the pin if it names the wrong fleet. It clears a few minutes after the other fleet "
                "stops being heard." },
    { .condition = NodeCondition::SharedCacheUnavailable,
      .id = "shared-cache-unavailable",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Consensus,
      .remedy = "The fleet's shared-cache setting names this machine and its shared tier will not open; every other "
                "node's builds miss meanwhile and compile locally. Fix what the detail names -- usually another process "
                "holding <state-dir>/shared-cache, or a full disk -- and it opens at the next change the cluster "
                "applies or within 30 seconds; or name another machine with --cluster-set shared-cache=<id>." },
    { .condition = NodeCondition::SharedCacheUnproven,
      .id = "shared-cache-unproven",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "This node's builds are not reaching the fleet's shared cache and compile locally instead. The detail "
                "names why: a machine at the announced address that proved another key (an address reassigned, or an "
                "impostor -- nothing was sent to it), the named machine refusing this node's key (its roster does not "
                "hold it, or holds it revoked), one that did not answer, or a setting naming a machine this cluster "
                "cannot reach by key. Check --cluster-status, and run fastcache-cli node on the named machine." },
    { .condition = NodeCondition::SchedulerUnreachable,
      .id = "scheduler-unreachable",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Announce,
      .remedy = "Check the VPN or network between this machine and the schedulers named here; the node keeps "
                "serving this machine meanwhile. It rejoins the fleet by itself on the first round that gets "
                "through, so nothing needs restarting once the network is back." },
    { .condition = NodeCondition::UnservedToolchain,
      .id = "unserved-toolchain",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Scheduler,
      .remedy = "Put the named compiler, at the named version, on at least one worker, or move the clients to a "
                "compiler the fleet already serves (the fleet page's compiler column lists them): every lease counted "
                "here was refused and compiled on the client's own machine instead. It clears once a worker serving "
                "it registers, or once no client has asked for it for fifteen minutes." },
    { .condition = NodeCondition::MixedNodeVersions,
      .id = "mixed-node-versions",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Scheduler,
      .remedy = "Upgrade every node to one build: these still speak one wire, so nothing refuses them and nothing "
                "else reports it, but a fleet upgrades as one and two builds can disagree about anything the wire "
                "does not carry. The detail and the fleet page's version column name each build and its machines. It "
                "clears once the last odd machine runs the fleet's build or has been gone for ninety seconds; see "
                "docs/operations/upgrading-a-fleet.md." },
    { .condition = NodeCondition::OwnRecordAwaited,
      .id = "own-record-awaited",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "This node announces itself to no scheduler until its own cluster records its key. If that cluster "
                "cannot elect, bring back a majority of its voters. If this machine joined a running cluster, admit "
                "it: it asks to enroll and --enroll-approve admits it, or --cluster-admit=<id>=<host>:<port>@<key> "
                "on a member, with what --print-identity prints. If the detail says its id is recorded under another key, "
                "restore that "
                "node-key, or --cluster-forget=<id> and admit this key." },
    // Live: an operator's reload of the allowlist is the fix landing, and it re-judges the row --
    // each argument the new list admits leaves, and the row clears once none is left.
    // A warning and not an alert, because nothing is WRONG with what a build produces -- each
    // refused compile runs on its own client -- only distribution is lost, which is what the
    // counter alone could say and never say which flag.
    { .condition = NodeCondition::RefusedCompileArguments,
      .id = "refused-compile-arguments",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Worker,
      .remedy = "Builds stay correct: each refused compile ran on its own client, losing only distribution. Admit a "
                "named argument that runs no program and names no path with --allow-compile-arg=<argument> on every "
                "worker, then reload. The reload re-judges the row: each argument the list now admits leaves, and the "
                "row clears once none is named. One still named was not admitted: check its spelling; the list never "
                "overrides the built-in rules. Report a flag every build carries: it belongs in the built-in table." },
    { .condition = NodeCondition::SurfaceNotAccepting,
      .id = "surface-not-accepting",
      .persistence = CompileCacheWire::ConditionPersistence::Latched,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Process,
      .remedy = "Restart this node: the surfaces named here have stopped accepting connections and do not start "
                "again by themselves, so their ports still listen and refuse every client -- builds compile cold, "
                "and a scheduler named here hands out nothing. /healthz answers 503 while this is raised. The "
                "reason is what the last accept answered; report it with this node's version, because only a "
                "closed or vanished listener is meant to end a loop." },
    // Live: the loop goes on accepting through its backoff, and the first accept that succeeds
    // clears the row -- as does the surface being shut down, which leaves nothing degraded.
    // An alert, as /healthz's 503 is: a surface that accepts little or nothing compiles cold.
    { .condition = NodeCondition::SurfaceAcceptDegraded,
      .id = "surface-accept-degraded",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Process,
      .remedy = "The surfaces named here have failed to accept connections for longer than any transient explains, "
                "on a failure this build does not classify; each backs off and accepts little or nothing, so builds "
                "compile cold. /healthz answers 503 while this is raised, and the row clears by itself once an "
                "accept succeeds. Check this host for exhausted file descriptors or memory, and report the reason "
                "with this node's version so the failure can be classified." },
    { .condition = NodeCondition::FleetSplitHealing,
      .id = "fleet-split-healing",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "Where the detail says which side yields, nothing: that side rejoins the other by itself, and a "
                "machine the survivor did not record waits on its --enroll-list. Where it says an operator decides, "
                "nothing moves until you do: ask the machine it names which fleet it is in, and check the other with "
                "--cluster-status. Not yours: leave it. Yours: --cluster-forget=<id> each machine of the fleet that "
                "does not stay, then --enroll-approve=<id>@<key> it on the survivor. Clears once the other is no "
                "longer heard." },
    { .condition = NodeCondition::FormationMoveRefused,
      .id = "formation-move-refused",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Alert,
      .scope = ConditionScope::Consensus,
      .remedy = "Fix the rule the detail names and restart this node; until then it keeps the mode it is in rather "
                "than serve a shape its next restart would refuse." },
    { .condition = NodeCondition::ConsensusLeaderSilent,
      .id = "consensus-leader-silent",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "Restore this node's Raft sessions to its fleet -- check the network path to its voters and "
                "--cluster-status on one of them. Until a leader the fleet counts speaks to it again, every grant it is "
                "handed is refused and the compiles fall back to their own machines; a voter its fleet forgot meanwhile is "
                "what this protects against. It clears at the first leader contact." },
    { .condition = NodeCondition::StateDirectoryUnsynced,
      .id = "state-directory-unsynced",
      .persistence = CompileCacheWire::ConditionPersistence::Latched,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Process,
      .remedy = "Stop the node, move its state directory whole to a local volume -- it holds the node's identity, so a "
                "new empty directory would be a new machine -- point --cluster-dir at the new place and start it. "
                "The filesystem the detail names cannot sync a directory, so a state file replaced there (the "
                "Raft term and vote, the formation record) is not known to survive a power loss: one a power cut "
                "takes back is read as the file before it. The node serves meanwhile; a restart on the same "
                "volume will not clear this." },
} };
static_assert(RowsInEnumeratorOrder(NodeConditionTable, &NodeConditionRow::condition),
              "NodeConditionTable must hold one row per NodeCondition, in enumerator order");

/// Whether every row of the table can travel: FEWER than `MaxNodeConditions` of them -- one row to
/// spare, so the next condition costs a row of the table rather than a re-derived share -- each with
/// an id and a remedy inside their ceilings, no id empty or spelled twice, every remedy non-empty.
/// @return True when the table fits the wire.
[[nodiscard]] consteval bool NodeConditionTableFitsTheWire() noexcept
{
    if (NodeConditionTable.empty() || NodeConditionTable.size() >= CompileCacheWire::MaxNodeConditions)
        return false;
    for (auto const& row: NodeConditionTable)
    {
        if (row.id.empty() || row.id.size() > CompileCacheWire::MaxConditionIdBytes || row.remedy.empty()
            || row.remedy.size() > CompileCacheWire::MaxConditionRemedyBytes)
            return false;
        if (std::ranges::count(NodeConditionTable, row.id, &NodeConditionRow::id) != 1)
            return false;
    }
    return true;
}
static_assert(NodeConditionTableFitsTheWire(),
              "every condition must fit the wire with a row to spare: fewer than MaxNodeConditions rows, each with a "
              "unique id and a remedy inside their ceilings");

/// The row describing @p condition.
/// @param condition The condition.
/// @return Its row.
[[nodiscard]] constexpr NodeConditionRow const& RowFor(NodeCondition condition) noexcept
{
    return NodeConditionTable[static_cast<std::size_t>(condition)];
}

/// This process's answer for every condition, raised and cleared by the components that detect
/// them.
///
/// **Owned by the node and injected by reference** into whatever raises or clears a row, and
/// thread-safe, because those are on different threads: the membership publisher is the consensus
/// thread, the enrollment window is a reactor's, and every reader -- `NodeStatus`, the presence
/// loop -- is yet another.
///
/// Every row starts `Undecided`. A component that runs evaluates its rows as it starts;
/// `Settle` answers the rows of every component this node does not run, and names whatever is left
/// -- a row whose component runs and never said anything, which is a wiring defect rather than a
/// state an operator should ever see.
class NodeConditions
{
  public:
    NodeConditions() = default;
    NodeConditions(NodeConditions const&) = delete;
    NodeConditions& operator=(NodeConditions const&) = delete;
    NodeConditions(NodeConditions&&) = delete;
    NodeConditions& operator=(NodeConditions&&) = delete;
    ~NodeConditions() = default;

    /// Raise @p condition, with what was observed.
    ///
    /// The detail is made TEXT here -- a byte that belongs to no UTF-8 sequence is written `\xNN`
    /// -- and clamped to `MaxConditionDetailBytes` on a code-point boundary, so a scratch path a
    /// host spelled in some other encoding cannot make the leader refuse this machine's whole
    /// announcement. That is the node composing its own sentence, not a renderer repairing one.
    /// @param condition The condition.
    /// @param detail What was observed.
    void Raise(NodeCondition condition, std::string_view detail);

    /// Clear @p condition: checked, and found benign.
    ///
    /// **A programmer error on a `Latched` row that has been raised** -- asserted, and ignored in a
    /// release build, where the row stays raised: *latched* is the claim that nothing this process
    /// does can clear it, and a clear arriving anyway would turn *still broken* into *fixed*.
    /// @param condition The condition.
    void Clear(NodeCondition condition);

    /// Answer @p condition as not evaluated on this node, and say why.
    ///
    /// For a component that runs but cannot ask the question -- a worker that found nothing to
    /// compile with has claimed no scratch root. A component that does not run at all is answered
    /// by `Settle`, from its scope's row.
    /// @param condition The condition.
    /// @param reason Why nothing could be observed.
    void NotEvaluated(NodeCondition condition, std::string_view reason);

    /// Answer every row of a scope this node does not run, and name the rows nothing answered.
    ///
    /// Called ONCE, after every component has been built and before any surface serves, so no
    /// reader ever sees the `Undecided` a correct node never shows. A row whose scope runs and that
    /// is still `Undecided` is left so -- it is reported that way rather than dressed as a
    /// neighbour -- and returned, for the caller to name at startup.
    /// @param present What this node built.
    /// @return The rows left undecided, in table order; empty on a correctly wired node.
    [[nodiscard]] std::vector<NodeCondition> Settle(PresentComponents const& present);

    /// What this process found for @p condition.
    /// @param condition The condition.
    /// @return Its state.
    [[nodiscard]] CompileCacheWire::ConditionState StateOf(NodeCondition condition) const;

    /// Every row, in table order, as the wire carries it.
    /// @return One entry per `NodeConditionTable` row, never fewer.
    [[nodiscard]] std::vector<CompileCacheWire::NodeConditionFields> Snapshot() const;

  private:
    /// One row's answer.
    struct Reading
    {
        CompileCacheWire::ConditionState state { CompileCacheWire::ConditionState::Undecided };
        std::string detail {};
    };

    /// Set a row, holding the latched rule. Caller holds `_mutex`.
    void SetLocked(NodeCondition condition, CompileCacheWire::ConditionState state, std::string detail);

    mutable std::mutex _mutex;
    EnumTable<NodeCondition, Reading> _readings {}; ///< Guarded by `_mutex`.
};

/// Evaluate the `Process` scope: whether this build's metrics catalogue and sink agree (#1362).
///
/// Asked through `CatalogueRowsWithoutASlot`, which is the scrape's own question, so the startup
/// report and the `# SKEW` lines cannot disagree about what a skew is.
/// @param conditions Where the answer goes.
/// @param metrics This process's sink.
void EvaluateProcessConditions(NodeConditions& conditions, IMetricsSink const& metrics);

/// A detail naming a list, clamped to what one row may carry.
///
/// Items are joined with `, ` for as long as they fit, and the rest are COUNTED rather than cut
/// mid-name: `a, b and 3 more`. A list cut at an arbitrary byte reads as naming its last,
/// truncated item.
/// @param lead What comes before the list.
/// @param items The items, in the order to name them.
/// @return The detail.
[[nodiscard]] std::string ListDetail(std::string_view lead, std::vector<std::string> const& items);

/// Keep `surface-not-accepting` and `surface-accept-degraded` true to @p health for as long as this
/// process runs.
///
/// Clears both rows now -- checked and benign, since nothing has stopped or degraded yet. Each
/// change the registry records re-reads it: `surface-not-accepting` is raised, naming every surface
/// that gave up and why, and stays raised, since a loop that gave up does not start again;
/// `surface-accept-degraded` names every degraded surface, and clears once none is left. The ONE
/// place that reads the node's accept-loop registry into a condition, so the rows and `/healthz`
/// answer from the same registry. Safe from any number of loop threads at once: the read and the
/// write it answers with are one step, so the last answer is from the newest record.
/// @param health The node's registry; must outlive @p conditions' use of it.
/// @param conditions Where the row is answered; must outlive @p health.
void WatchAcceptLoops(core::net::AcceptLoopHealth& health, NodeConditions& conditions);

} // namespace FastCache::Node
