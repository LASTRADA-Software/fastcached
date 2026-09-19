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
    CounterTableSkew = 0,     ///< The metrics catalogue names counters this build's sink has no slot for (#1362).
    ScratchRootUnmappable,    ///< The worker's scratch root cannot be written into a debug-prefix-map rule (#810).
    GeneratedTlsCertificate,  ///< The admin surface serves a certificate generated at startup.
    EnrollmentWindowOpen,     ///< A stranger that asks can be admitted to the cluster (#1298).
    ForgottenFleetMember,     ///< `--fleet-member` names a host the cluster has forgotten (#1309).
    UnreadableLeaderSnapshot, ///< This build cannot read the snapshot its leader sends, so it stays behind (#1552).
    Last,                     ///< Not a condition.
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
    Scheduler,    ///< The scheduler tier; absent without `--serve-scheduler`.
    AdminSurface, ///< The admin HTTP surface; absent without `--admin-listen`.
    Enrollment,   ///< The enrollment window; served only by a consensus node that also schedules.
    Consensus,    ///< Consensus and the cluster state it replicates; absent without `--listen-raft`.
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
      .notEvaluated = "this node runs no scheduler (no --serve-scheduler)" },
    { .scope = ConditionScope::AdminSurface,
      .present = &PresentComponents::adminSurface,
      .notEvaluated = "this node serves no admin surface (no --admin-listen)" },
    { .scope = ConditionScope::Enrollment,
      .present = &PresentComponents::enrollment,
      .notEvaluated = "this node serves no enrollment window: that takes consensus (--listen-raft) and a scheduler "
                      "(--serve-scheduler)" },
    { .scope = ConditionScope::Consensus,
      .present = &PresentComponents::consensus,
      .notEvaluated = "this node runs no consensus (no --listen-raft), so no cluster state reaches it" },
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
      .remedy = "Close it with --enroll-close once the machines you meant to admit have joined; check who is waiting "
                "with --enroll-list before approving anyone, because approving hands that machine this cluster's key. "
                "A restart closes it too." },
    { .condition = NodeCondition::ForgottenFleetMember,
      .id = "forgotten-fleet-member",
      .persistence = CompileCacheWire::ConditionPersistence::Live,
      .severity = CompileCacheWire::ConditionSeverity::Warning,
      .scope = ConditionScope::Consensus,
      .remedy = "Remove these hosts from this node's --fleet-member list (fleet_member in its configuration file) and "
                "reload it. They are refused either way, because the cluster's forget outranks the listing; if the "
                "forget was a mistake, --cluster-admit-client undoes it for every node instead." },
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
} };
static_assert(RowsInEnumeratorOrder(NodeConditionTable, &NodeConditionRow::condition),
              "NodeConditionTable must hold one row per NodeCondition, in enumerator order");

/// Whether every row of the table can travel: at most `MaxNodeConditions` of them, each with an id
/// and a remedy inside their ceilings, no id empty or spelled twice, every remedy non-empty.
/// @return True when the table fits the wire.
[[nodiscard]] consteval bool NodeConditionTableFitsTheWire() noexcept
{
    if (NodeConditionTable.empty() || NodeConditionTable.size() > CompileCacheWire::MaxNodeConditions)
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
              "every condition must fit the wire: at most MaxNodeConditions rows, each with a unique id and a remedy "
              "inside their ceilings");

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

} // namespace FastCache::Node
