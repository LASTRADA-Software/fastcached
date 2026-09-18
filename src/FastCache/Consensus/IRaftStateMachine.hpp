// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Consensus
{

/// Which of its two callers handed `IRaftStateMachine::RestoreSnapshot` a snapshot.
///
/// **Private: never transmitted or persisted**, so no enumerator states a value.
///
/// The two are not interchangeable once the bytes cannot be read, which is why the
/// call says which it is (#1542). A snapshot this node RECOVERED is its own durable
/// state, and a node that cannot read it refuses to start -- running on the entries
/// above it alone would be a state no member of the cluster ever held. One a leader
/// INSTALLED arrived while the node was running, and the machine keeps what it holds.
enum class SnapshotOrigin : std::uint8_t
{
    Recovered, ///< This node's own, read back from its storage when the driver was built.
    Installed, ///< A leader's, sent because the entries this node lacked are compacted away.
    Last,      ///< Not an origin, and has no row: `SnapshotOriginTable`'s length.
};

/// What each origin is called, and what a snapshot of it that cannot be read costs.
struct SnapshotOriginTraits
{
    SnapshotOrigin origin; ///< The origin.

    /// The snapshot, named for a log line: "the snapshot this node recovered".
    std::string_view described;

    /// What happens when the machine cannot read it, for the same line.
    std::string_view consequence;
};

/// One row per `SnapshotOrigin`, in enumerator order.
///
/// A table rather than two literals at the one implementation that logs, because
/// the consequence is not that implementation's to decide: a recovered snapshot is
/// refused by `RaftDriver::Create`, and an installed one is kept out by every
/// machine's contract below. The wording belongs beside the contract it states.
inline constexpr EnumTable<SnapshotOrigin, SnapshotOriginTraits> SnapshotOriginTable { {
    { .origin = SnapshotOrigin::Recovered,
      .described = "the snapshot this node recovered from its own storage",
      .consequence = "this node will not start on it" },
    { .origin = SnapshotOrigin::Installed,
      .described = "a snapshot the leader installed",
      .consequence = "keeping current state" },
} };

static_assert(RowsInEnumeratorOrder(SnapshotOriginTable, &SnapshotOriginTraits::origin),
              "SnapshotOriginTable must hold one row per SnapshotOrigin, in enumerator order");

/// The row describing @p origin; an index, as `TraitsOf(Role)` is.
/// @param origin Which caller.
/// @return Its traits.
[[nodiscard]] constexpr SnapshotOriginTraits const& TraitsOf(SnapshotOrigin origin) noexcept
{
    return SnapshotOriginTable[static_cast<std::size_t>(origin)];
}

/// What consensus is deciding the order of.
///
/// Entries arrive here only once committed — that is, once they are guaranteed
/// to be present in every future leader's log — and in ascending index order,
/// each exactly once per node. So an implementation may act on them directly and
/// needs no idempotence within a single run.
///
/// **Across a restart it does need idempotence**, and that is the one thing worth
/// stating plainly: a recovered node re-applies from the beginning of whatever
/// its log holds, because the commit index is not durable and is re-learned from
/// the first leader that reaches it. Making the commit index durable would trade
/// that for an extra flush on the hot path, and re-applying a deterministic state
/// machine from a log is cheap by comparison — but a state machine with side
/// effects outside itself has to know.
///
/// **And a recovered node that holds a snapshot starts from the snapshot**: its
/// state is handed to `RestoreSnapshot` when the driver is built, before any entry
/// above it is applied, and only the entries above it are re-applied. The log below
/// the snapshot is gone, so this restore is the only way the state it produced comes
/// back (#1542).
///
/// **Or it does not start at all.** A node whose own snapshot, or any command its
/// own log still holds, is something this machine cannot read refuses to start --
/// `CanRead` and `RestoreSnapshot` answer for it, and `RaftDriver::Create` asks both
/// before anything is applied. The alternative is a node running on part of its own
/// state: a snapshot left unread takes the members, the settings and the forget
/// tombstones with it, and a removal that fails OPEN reports nothing.
class IRaftStateMachine
{
  public:
    IRaftStateMachine() = default;
    IRaftStateMachine(IRaftStateMachine const&) = delete;
    IRaftStateMachine(IRaftStateMachine&&) = delete;
    IRaftStateMachine& operator=(IRaftStateMachine const&) = delete;
    IRaftStateMachine& operator=(IRaftStateMachine&&) = delete;
    virtual ~IRaftStateMachine() = default;

    /// Act on one committed entry.
    /// @param entry The entry, with the index it was committed at.
    virtual void Apply(AppliedEntry const& entry) = 0;

    /// Whether `Apply` could act on @p command, without acting on it.
    ///
    /// Asked on RECOVERY, of every command this node's own log holds above its
    /// snapshot, before the snapshot is restored (#1542) -- so a node whose log another
    /// build wrote refuses to start rather than skipping the entries it cannot read
    /// and running on the rest. Those entries are this node's own state and will be
    /// applied as soon as a leader commits them; there is nobody to fetch them from
    /// in a form this build reads.
    ///
    /// Const and free of effects: a refusal must leave nothing half-applied, so the
    /// question is asked of every entry before any answer is acted on.
    /// @param command An entry's payload.
    /// @return Nothing when `Apply` would act on it; otherwise why not, in the two
    ///         codes `RestoreSnapshot` refuses with.
    [[nodiscard]] virtual std::expected<void, ConsensusError> CanRead(std::span<std::byte const> command) const = 0;

    /// Serialize everything applied so far, so the log below it can be discarded.
    ///
    /// The bytes are opaque to consensus, exactly as an entry's payload is. What
    /// consensus guarantees in return is *when* it asks: only ever at an index it
    /// has applied, so the state described is a state this machine actually
    /// reached.
    /// @return The serialized state.
    [[nodiscard]] virtual std::vector<std::byte> TakeSnapshot() = 0;

    /// Replace this machine's state with `state`, wholesale.
    ///
    /// Called in TWO situations, and @p origin says which:
    ///   - on **recovery**, once, when the driver is built over a node that recovered
    ///     a snapshot from its own storage -- before any entry is applied;
    ///   - on **install**, when a follower is handed state it cannot replay its way
    ///     to, because the entries that would have taken it there have been
    ///     compacted away everywhere.
    ///
    /// Both have the same reason: the entries below the snapshot will never be
    /// applied here again, so the state they produced arrives only through this call.
    /// **Replace, do not merge**: the snapshot is the complete state as of its index,
    /// and folding it into what this machine already holds would keep entries the
    /// cluster has superseded.
    ///
    /// **Replace or refuse, and a refusal changes NOTHING.** Bytes this machine
    /// cannot read are refused whole, never half-restored: at recovery the refusal
    /// stops the node starting (`RaftDriver::Create`), and at install the machine
    /// keeps what it holds, since clearing it would turn "I cannot read your state"
    /// into "the cluster has no members". What a node should do next with a leader's
    /// snapshot it cannot read is the install path's own question, and this call does
    /// not answer it.
    /// @param state Bytes previously produced by `TakeSnapshot` on some node.
    /// @param origin Which of the two callers this is, so a refusal says so.
    /// @return Nothing once the state is replaced. Otherwise why not:
    ///         `UnsupportedFormatVersion` for bytes that are intact and another build
    ///         laid out -- the storage rule's code, never the damage one -- and
    ///         `StorageFailure` for bytes that are no state this machine could have
    ///         written.
    [[nodiscard]] virtual std::expected<void, ConsensusError> RestoreSnapshot(std::span<std::byte const> state,
                                                                              SnapshotOrigin origin) = 0;
};

} // namespace FastCache::Consensus
