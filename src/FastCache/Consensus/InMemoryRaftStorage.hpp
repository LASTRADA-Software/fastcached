// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftStorage.hpp>

#include <cstddef>
#include <vector>

namespace FastCache::Consensus
{

/// `IRaftStorage` that keeps everything in memory.
///
/// Two uses, and the second is the one that matters. It is the store a
/// multi-node simulation gives each of its nodes, so a whole cluster runs in one
/// process with no filesystem. And it can be told to **fail on demand**, which is
/// how the paths that only run when a disk refuses get exercised at all —
/// `InMemoryPageStore` next door exists for the same reason and injects faults
/// the same way.
///
/// A restart is modelled by constructing a new `RaftNode` from this store's
/// `Load()` while the store itself lives on: that is exactly what a process
/// restart looks like from the node's point of view.
class InMemoryRaftStorage final: public IRaftStorage
{
  public:
    /// Which call to fail, counted from one; zero never fails.
    ///
    /// Counted rather than a flag because the interesting cases are ordinal: a
    /// crash *between* the state write and the log write leaves a node that voted
    /// but did not record the entry, and only a counter can express which of the
    /// two goes wrong.
    struct FailurePlan
    {
        std::size_t failNthSaveState { 0 }; ///< Fail this SaveState call.
        std::size_t failNthSaveLog { 0 };   ///< Fail this SaveLog call.
        std::size_t failNthLoad { 0 };      ///< Fail this Load call.
    };

    InMemoryRaftStorage() = default;

    /// Construct with faults armed.
    /// @param plan Which calls should fail.
    explicit InMemoryRaftStorage(FailurePlan plan) noexcept:
        _plan { plan }
    {
    }

    [[nodiscard]] std::expected<void, ConsensusError> SaveState(PersistentState const& state) override;
    [[nodiscard]] std::expected<void, ConsensusError> SaveLog(LogAppend const& append) override;
    [[nodiscard]] std::expected<RecoveredState, ConsensusError> Load() override;

    /// How many entries are currently stored. For assertions about truncation.
    /// @return The stored log length.
    [[nodiscard]] std::size_t StoredEntryCount() const noexcept
    {
        return _entries.size();
    }

  private:
    FailurePlan _plan;
    std::size_t _saveStateCalls { 0 };
    std::size_t _saveLogCalls { 0 };
    std::size_t _loadCalls { 0 };

    PersistentState _state;
    std::vector<LogEntry> _entries;
};

} // namespace FastCache::Consensus
