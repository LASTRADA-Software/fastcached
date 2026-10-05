// SPDX-License-Identifier: Apache-2.0
#include "LocalCache.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Where a resolution says this node reads the shared cache from, before `--upstream` is asked.
    struct SourceRow
    {
        Cluster::SharedCacheResolution resolution; ///< The resolution this row describes.
        Wire::WireSharedCacheSource source;        ///< What `--node-status` calls it.
    };

    /// Every named machine this node cannot reach is still the SETTING's answer -- the setting
    /// named it -- so only an unset setting and this machine itself are anything else.
    constexpr EnumTable<Cluster::SharedCacheResolution, SourceRow> SourceTable { {
        { .resolution = Cluster::SharedCacheResolution::Unset, .source = Wire::WireSharedCacheSource::None },
        { .resolution = Cluster::SharedCacheResolution::Resolved, .source = Wire::WireSharedCacheSource::Setting },
        { .resolution = Cluster::SharedCacheResolution::ThisMachine, .source = Wire::WireSharedCacheSource::ThisMachine },
        { .resolution = Cluster::SharedCacheResolution::UnknownMachine, .source = Wire::WireSharedCacheSource::Setting },
        { .resolution = Cluster::SharedCacheResolution::KeyRevoked, .source = Wire::WireSharedCacheSource::Setting },
        { .resolution = Cluster::SharedCacheResolution::NoEndpoint, .source = Wire::WireSharedCacheSource::Setting },
    } };
    static_assert(RowsInEnumeratorOrder(SourceTable, &SourceRow::resolution),
                  "SourceTable must hold one row per SharedCacheResolution, in enumerator order");

    /// The target @p resolved gives, with @p overrideEndpoint winning whenever it is set.
    /// @param resolved What the setting resolved to.
    /// @param overrideEndpoint `--upstream`, or empty.
    /// @return The target.
    [[nodiscard]] SharedCacheTarget TargetOf(Cluster::ResolvedSharedCache resolved, std::string const& overrideEndpoint)
    {
        auto const source = overrideEndpoint.empty() ? SourceTable[static_cast<std::size_t>(resolved.resolution)].source
                                                     : Wire::WireSharedCacheSource::Override;
        return SharedCacheTarget { .source = source, .resolved = std::move(resolved), .overrideEndpoint = overrideEndpoint };
    }
} // namespace

SharedCacheDirectory::SharedCacheDirectory(std::string selfId, std::string overrideEndpoint):
    _selfId { std::move(selfId) },
    _override { std::move(overrideEndpoint) },
    _current { TargetOf(Cluster::ResolvedSharedCache {}, _override) }
{
}

void SharedCacheDirectory::Applied(Cluster::ClusterState const& state)
{
    // Resolved outside the lock: a reader waits only for the copy, never for the walk.
    auto target = TargetOf(Cluster::ResolveSharedCache(state, _selfId), _override);
    std::scoped_lock const lock { _mutex };
    _current = std::move(target);
}

SharedCacheTarget SharedCacheDirectory::Current() const
{
    std::scoped_lock const lock { _mutex };
    return _current;
}

void ApplySharedCacheState(Cluster::ClusterState const& state,
                           SharedCacheDirectory& directory,
                           SharedCacheHost* host,
                           ICacheUpstream* upstream)
{
    directory.Applied(state);
    if (host != nullptr)
        host->Applied(state);
    if (upstream != nullptr)
        upstream->StateApplied();
}

} // namespace FastCache::Node
