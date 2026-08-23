// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>

#include <cstddef>

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
    if (append.fromIndex.value > _entries.size() + 1)
        return std::unexpected { FastCache::StorageFailure("a log append would leave a gap") };

    // `fromIndex` is a truncation point as well as a start, so anything at or
    // after it is discarded before the new run is written. A store that only
    // appended would recover entries the cluster had overwritten.
    auto const keep = append.fromIndex.value > 0 ? append.fromIndex.value - 1 : 0;
    if (keep < _entries.size())
        _entries.resize(static_cast<std::size_t>(keep));

    _entries.insert(_entries.end(), append.entries.begin(), append.entries.end());
    return {};
}

std::expected<RecoveredState, ConsensusError> InMemoryRaftStorage::Load()
{
    ++_loadCalls;
    if (_plan.failNthLoad != 0 && _loadCalls == _plan.failNthLoad)
        return std::unexpected { FastCache::StorageFailure("injected Load failure") };

    return RecoveredState { .state = _state, .entries = _entries };
}

} // namespace FastCache::Consensus
