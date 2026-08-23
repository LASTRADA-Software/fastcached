// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftOutput.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace FastCache::Consensus
{

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
    /// Called when a follower is handed state it cannot replay its way to,
    /// because the entries that would have taken it there have been compacted
    /// away everywhere. **Replace, do not merge**: the snapshot is the complete
    /// state as of its index, and folding it into what this machine already holds
    /// would keep entries the cluster has superseded.
    /// @param state Bytes previously produced by `TakeSnapshot` on some node.
    virtual void RestoreSnapshot(std::span<std::byte const> state) = 0;
};

} // namespace FastCache::Consensus
