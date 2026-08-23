// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <expected>
#include <optional>
#include <vector>

namespace FastCache::Consensus
{

/// Everything a node recovers from durable storage when it restarts.
///
/// A default-constructed value is a node that has never run: term zero, no
/// vote, empty log. That is deliberate rather than incidental — it lets one
/// entry point serve both a fresh node and a restarted one, so there is no
/// second construction path that could drift from the first.
struct RecoveredState
{
    PersistentState state;         ///< Term and vote as last written.
    std::vector<LogEntry> entries; ///< The log, starting at `firstIndex`.

    /// Index of `entries[0]`; above 1 when the log was compacted.
    ///
    /// Recovered rather than assumed, because a node that came back reporting
    /// `FirstIndex() == 1` for a log whose prefix is gone would answer questions
    /// about indices it does not hold.
    LogIndex firstIndex { .value = 1 };

    /// The snapshot the discarded prefix was replaced by; absent when none.
    std::optional<RaftSnapshot> snapshot;
};

/// Where a Raft node's durable state lives.
///
/// **Synchronous, returning `std::expected`**, matching `IPageStore` in the
/// CowTree next door rather than the coroutine style of the network layer. The
/// operations here are a small write and an `fsync`; expressing them as
/// coroutines would buy the ability to interleave other work during the flush,
/// which the node cannot use — it must not act on anything until the write
/// completes, because that is the entire point of the write.
///
/// ## Why a failure here cannot be swallowed
///
/// Every other refusal in this system ends in a local compile and no harm done.
/// This one does not. If a node replies to a RequestVote and the vote never
/// reaches stable storage, a restart votes again in the same term for a
/// different candidate and one term gets two leaders — so a driver that logged
/// a storage error and carried on would be trading a stalled node for a corrupt
/// cluster. The contract is that a node whose storage fails stops participating,
/// which is why these return `std::expected` rather than reporting through a
/// counter.
class IRaftStorage
{
  public:
    IRaftStorage() = default;
    IRaftStorage(IRaftStorage const&) = delete;
    IRaftStorage(IRaftStorage&&) = delete;
    IRaftStorage& operator=(IRaftStorage const&) = delete;
    IRaftStorage& operator=(IRaftStorage&&) = delete;
    virtual ~IRaftStorage() = default;

    /// Make the current term and vote durable.
    ///
    /// Must not return until the write survives a crash. Called on every term
    /// change and every granted vote — that is, on the latency path of an
    /// election, and nowhere near as often as the log.
    /// @param state What to write.
    /// @return Nothing, or why it failed.
    [[nodiscard]] virtual std::expected<void, ConsensusError> SaveState(PersistentState const& state) = 0;

    /// Make a log change durable.
    ///
    /// `append.fromIndex` is where the entries begin **and** a truncation point:
    /// anything already stored at or after it is no longer part of the log and
    /// must not survive. A store that only appended would recover entries the
    /// cluster had overwritten, which is a divergent log rather than a lost one.
    /// @param append What to write, and from where.
    /// @return Nothing, or why it failed.
    [[nodiscard]] virtual std::expected<void, ConsensusError> SaveLog(LogAppend const& append) = 0;

    /// Make a snapshot durable, replacing any earlier one.
    ///
    /// The write `RaftLog::Compact` requires before it discards anything, and the
    /// one an acknowledged `InstallSnapshot` rests on. Both are the same rule the
    /// log already obeys: a node that acknowledges state it has not made durable
    /// retracts that acknowledgement when it restarts, and a leader may already
    /// have committed on it.
    /// @param snapshot The state and the point it is as of.
    /// @return Nothing on success, or why it could not be written.
    [[nodiscard]] virtual std::expected<void, ConsensusError> SaveSnapshot(RaftSnapshot const& snapshot) = 0;

    /// Read back everything a node needs to resume.
    ///
    /// A store holding nothing yields a default `RecoveredState` rather than an
    /// error: a node starting for the first time is the ordinary case, not a
    /// failure. An error means the state is present but unreadable, which is
    /// different and must not be mistaken for a fresh start — resuming as a fresh
    /// node would discard a vote this node had already given.
    /// @return The recovered state, or why it could not be read.
    [[nodiscard]] virtual std::expected<RecoveredState, ConsensusError> Load() = 0;
};

} // namespace FastCache::Consensus
