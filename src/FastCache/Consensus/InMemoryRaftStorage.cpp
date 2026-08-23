// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace FastCache::Consensus
{

std::expected<void, ConsensusError> InMemoryRaftStorage::SaveState(PersistentState const& state)
{
    ++_saveStateCalls;
    if (_plan.failNthSaveState != 0 && _saveStateCalls == _plan.failNthSaveState)
        return std::unexpected { FastCache::StorageFailure("injected SaveState failure") };

    _state = state;
    return {};
}

std::expected<void, ConsensusError> InMemoryRaftStorage::SaveLog(LogAppend const& append)
{
    ++_saveLogCalls;
    if (_plan.failNthSaveLog != 0 && _saveLogCalls == _plan.failNthSaveLog)
        return std::unexpected { FastCache::StorageFailure("injected SaveLog failure") };

    // A gap is refused rather than quietly closed up. `fromIndex` states where
    // these entries *begin*, so writing them anywhere else produces a store whose
    // indices no longer match the node's -- and silently, in the one component
    // whose entire job is not being silently wrong. No correct driver produces
    // one, which is exactly why it should be an error rather than a coincidence.
    // Both ends are checked, because a compacted store no longer starts at 1 and
    // an append below its first index names entries it has deliberately discarded.
    if (append.fromIndex < _firstIndex || append.fromIndex.value > _firstIndex.value + _entries.size())
        return std::unexpected { FastCache::StorageFailure("a log append would leave a gap") };

    // `fromIndex` is a truncation point as well as a start, so anything at or
    // after it is discarded before the new run is written. A store that only
    // appended would recover entries the cluster had overwritten.
    _entries.resize(static_cast<std::size_t>(append.fromIndex.value - _firstIndex.value));
    _entries.insert(_entries.end(), append.entries.begin(), append.entries.end());
    return {};
}

std::expected<void, ConsensusError> InMemoryRaftStorage::SaveSnapshot(RaftSnapshot const& snapshot)
{
    ++_saveSnapshotCalls;
    if (_plan.failNthSaveSnapshot != 0 && _saveSnapshotCalls == _plan.failNthSaveSnapshot)
        return std::unexpected { FastCache::StorageFailure("injected SaveSnapshot failure") };

    _snapshot = snapshot;

    // The covered prefix goes only after the snapshot is held, which here is the
    // same statement -- the point being the order, which the file-backed store has
    // to arrange deliberately and which a restart between the two must survive.
    auto const covered = snapshot.lastIncludedIndex;
    if (covered >= _firstIndex)
    {
        auto const drop = std::min<std::size_t>(_entries.size(), covered.value - _firstIndex.value + 1);
        _entries.erase(_entries.begin(), _entries.begin() + static_cast<std::ptrdiff_t>(drop));
        _firstIndex = covered.Advanced(1);
    }

    return {};
}

std::expected<RecoveredState, ConsensusError> InMemoryRaftStorage::Load()
{
    ++_loadCalls;
    if (_plan.failNthLoad != 0 && _loadCalls == _plan.failNthLoad)
        return std::unexpected { FastCache::StorageFailure("injected Load failure") };

    return RecoveredState { .state = _state, .entries = _entries, .firstIndex = _firstIndex, .snapshot = _snapshot };
}

} // namespace FastCache::Consensus
