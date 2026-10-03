// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/SharedCacheTarget.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <mutex>
#include <string>

namespace FastCache::Node
{

/// Where this node reads the fleet's shared cache from, as of the last state it applied.
struct SharedCacheTarget
{
    /// Where the answer came from: nothing, the replicated setting, this node's `--upstream`, or
    /// this node being the named machine itself.
    CompileCacheWire::WireSharedCacheSource source { CompileCacheWire::WireSharedCacheSource::None };

    /// What the setting resolves to, even when `--upstream` overrides it -- so `--node-status` can
    /// say what the override is overriding.
    Cluster::ResolvedSharedCache resolved;

    /// `--upstream`, when `source` is `Override`; empty otherwise.
    std::string overrideEndpoint;

    [[nodiscard]] friend bool operator==(SharedCacheTarget const&, SharedCacheTarget const&) = default;
};

/// What an operation asks before it reads through: where the shared cache is right now.
class ISharedCacheTargetSource
{
  public:
    ISharedCacheTargetSource() = default;
    ISharedCacheTargetSource(ISharedCacheTargetSource const&) = delete;
    ISharedCacheTargetSource(ISharedCacheTargetSource&&) = delete;
    ISharedCacheTargetSource& operator=(ISharedCacheTargetSource const&) = delete;
    ISharedCacheTargetSource& operator=(ISharedCacheTargetSource&&) = delete;
    virtual ~ISharedCacheTargetSource() = default;

    /// Thread-safe: asked from a reactor while the apply thread replaces it.
    /// @return The target as of the last applied state; a copy.
    [[nodiscard]] virtual SharedCacheTarget Current() const = 0;
};

/// This node's view of the fleet's shared cache: `--upstream` when it is set, otherwise the
/// `shared-cache` setting resolved against the last state the cluster applied.
///
/// **Staleness.** Updated at every apply and never from a probe, so it lags the cluster by exactly
/// one apply -- the same lag every other replicated setting has on this node -- and it caches
/// nothing expensive: a resolution is a walk over state this node already holds
/// (`Cluster::ResolveSharedCache`). A stale answer in either direction fails CLOSED: a machine the
/// setting has since moved away from is dialled once more and refuses (`not-shared-cache`), or the
/// machine it has moved to is not dialled until the apply that names it arrives.
///
/// The override is fixed for the process, because `--upstream` is not reloadable.
class SharedCacheDirectory final: public ISharedCacheTargetSource
{
  public:
    /// @param selfId This node's own id, so a setting naming it reads as `ThisMachine`.
    /// @param overrideEndpoint `--upstream`, or empty.
    SharedCacheDirectory(std::string selfId, std::string overrideEndpoint);

    /// Take in a state the cluster applied. Called on the apply thread.
    /// @param state The state as applied.
    void Applied(Cluster::ClusterState const& state);

    /// @copydoc ISharedCacheTargetSource::Current
    [[nodiscard]] SharedCacheTarget Current() const override;

  private:
    std::string _selfId;
    std::string _override;
    mutable std::mutex _mutex;
    SharedCacheTarget _current;
};

class ICacheUpstream;
class SharedCacheHost;

/// Tell one node's shared-cache parts the state the cluster just applied, in the one order that is
/// right: where the shared cache is, then whether this machine serves it, then the private tier's
/// upstream, which re-judges what it reports from the directory it was just told.
///
/// The order's one home: `SharedCacheListeners::Applied` and the fleet test harness both call this,
/// the harness with no host because its client nodes serve nothing.
/// @param state The state the cluster just applied.
/// @param directory Where the shared cache is.
/// @param host Whether this machine serves it; null on a node composed without one.
/// @param upstream The private tier's upstream; null on a node that keeps no objects.
void ApplySharedCacheState(Cluster::ClusterState const& state,
                           SharedCacheDirectory& directory,
                           SharedCacheHost* host,
                           ICacheUpstream* upstream);

} // namespace FastCache::Node
