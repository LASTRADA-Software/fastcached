// SPDX-License-Identifier: Apache-2.0
#include "InputWitness.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <utility>

namespace FastCache::Cc
{

StabilityRow const& RowOf(InputStability stability) noexcept
{
    // Indexed rather than searched: `RowsInEnumeratorOrder` beside the table makes the index a
    // fact. `Last` is the table's length rather than a verdict and would index past its end, so it
    // (and any byte past it) is answered by the row that says the verdict is unknown.
    auto const index = static_cast<std::size_t>(stability);
    return index < StabilityTable.size() ? StabilityTable[index] : UnknownStabilityRow;
}

std::string DescribeInstability(StabilityFinding const& finding)
{
    if (finding.stability == InputStability::Stable)
        return {};
    return std::format("{}: {}", RowOf(finding.stability).sentence, finding.path);
}

InputWitness InputWitness::Begin(std::string resolvedSource, IInputFiles const& files, ContentHashing hashing)
{
    auto witness = InputWitness {};
    witness._started = files.Now();
    witness._hashing = hashing;
    witness._sourceAtStart = files.Identify(resolvedSource);
    if (hashing == ContentHashing::Yes)
        witness._sourceHashAtStart = files.Hash(resolvedSource);
    witness._source = std::move(resolvedSource);
    return witness;
}

void InputWitness::Snapshot(std::span<std::string const> resolvedProjectPaths, IInputFiles const& files)
{
    auto paths = std::vector<std::string> { _source };
    std::ranges::copy_if(
        resolvedProjectPaths, std::back_inserter(paths), [this](std::string const& path) { return path != _source; });

    auto const note = [this](InputStability stability, std::string const& path) {
        if (_atKey.stability == InputStability::Stable)
            _atKey = StabilityFinding { .stability = stability, .path = path };
    };
    auto const hashing = _hashing == ContentHashing::Yes;

    // Re-entrant: a second snapshot starts from nothing, its finding included.
    _entries.clear();
    _atKey = StabilityFinding { .stability = InputStability::Stable, .path = {} };
    _entries.reserve(paths.size());
    for (auto const& path: paths)
    {
        auto const before = files.Identify(path);
        auto hash = hashing ? files.Hash(path) : std::string {};
        auto const after = files.Identify(path);

        // The first matching rule wins within one entry; the first finding across entries wins
        // through `note`.
        if (!before || !after || (hashing && hash.empty()))
            note(InputStability::Unwitnessed, path);
        else if (*before != *after)
            note(InputStability::MovedBeforeKey, path);
        else if (ChangedAtOrAfter(after->changeTime, _started))
            note(InputStability::MovedBeforeKey, path);
        else if (path == _source)
        {
            if (!_sourceAtStart)
                note(InputStability::Unwitnessed, path);
            else if (*_sourceAtStart != *after)
                note(InputStability::MovedBeforeKey, path);
            else if (hashing && _sourceHashAtStart != hash)
                note(InputStability::MovedBeforeKey, path);
        }

        _entries.push_back(Entry { .path = path, .identity = after, .hash = std::move(hash) });
    }
}

StabilityFinding InputWitness::AtKey() const
{
    return _atKey;
}

StabilityFinding InputWitness::AfterCompile(std::span<std::string const> lateResolvedPaths, IInputFiles const& files) const
{
    if (_atKey.stability != InputStability::Stable)
        return _atKey;

    // No clock comparison for an entry: its T1' stamp lies below T0 floored to that stamp's own
    // granularity (Snapshot's clock rule), and any rewrite after T0 is stamped at or above that
    // floor, so the identity differs -- on a whole-second filesystem as well.
    for (auto const& entry: _entries)
    {
        auto const now = files.Identify(entry.path);
        if (!now || now != entry.identity)
            return StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = entry.path };
    }

    auto const isEntry = [this](std::string const& path) {
        return std::ranges::any_of(_entries, [&path](Entry const& entry) { return entry.path == path; });
    };
    for (auto const& path: lateResolvedPaths)
    {
        if (isEntry(path))
            continue;
        auto const now = files.Identify(path);
        if (!now)
            return StabilityFinding { .stability = InputStability::Unwitnessed, .path = path };
        if (ChangedAtOrAfter(now->changeTime, _started))
            return StabilityFinding { .stability = InputStability::MovedDuringCompile, .path = path };
    }
    return StabilityFinding { .stability = InputStability::Stable, .path = {} };
}

std::optional<std::string> InputWitness::KeyedHash(std::string_view resolvedPath) const
{
    auto const found = std::ranges::find(_entries, resolvedPath, &Entry::path);
    if (found == _entries.end() || found->hash.empty())
        return std::nullopt;
    return found->hash;
}

std::expected<std::string, ManifestFault> WitnessedManifestContent::HashAsKeyed(std::string_view resolvedPath) const
{
    if (auto keyed = _witness->KeyedHash(resolvedPath))
        return std::move(*keyed);

    auto const identity = _files->Identify(resolvedPath);
    if (!identity)
        return std::unexpected(ManifestFault::Unreadable);
    if (ChangedAtOrAfter(identity->changeTime, _witness->Started()))
        return std::unexpected(ManifestFault::ChangedDuringInvocation);

    auto hash = _files->Hash(resolvedPath);
    if (hash.empty())
        return std::unexpected(ManifestFault::Unreadable);
    if (_files->Identify(resolvedPath) != identity)
        return std::unexpected(ManifestFault::ChangedDuringInvocation);
    return hash;
}

} // namespace FastCache::Cc
