// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"
#include "NodeConditions.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheSession.hpp"
#include "SharedCacheStatus.hpp"

#include <FastCache/Cluster/SharedCacheTarget.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CacheProtocol.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

namespace FastCache::Node
{

/// The private tier's upstream when the fleet's `shared-cache` setting names another machine.
///
/// Per operation it asks the directory where the shared cache is, takes a PROVEN session to it,
/// and only then sends the fleet verbs (`FleetSharedCacheVerbs`) -- never the private tier's own,
/// which another machine refuses.
///
/// **It holds no credential source, and that is structural.** The proven session identifies both
/// ends, so nothing on this leg presents `--requirepass`: there is no `ICredentialSource` among the
/// constructor's parameters or the members, and a credential could reach this leg only by adding
/// one -- which `SharedCacheUpstream_test.cpp` refuses by name.
///
/// Reactor thread only for `Fetch` and `Store`; `Report` and `Configured` are safe from any thread.
class SharedCacheUpstream final: public ICacheUpstream, public ISharedCacheStatusSource
{
  public:
    /// @param targets Where the shared cache is right now; must outlive this.
    /// @param sessions Where proven sessions come from; must outlive this.
    /// @param reactor The loop each exchange's deadline runs on; null for a blocking connector.
    /// @param metrics Where an unresolved target is counted.
    /// @param conditions Where `shared-cache-unproven` is answered; null when nobody reads it.
    /// @param logger Where a change in how the shared cache is reached is said, once.
    /// @param policy The exchange bound, `ioTimeout`.
    SharedCacheUpstream(ISharedCacheTargetSource const& targets,
                        SharedSessionPool& sessions,
                        core::net::EventLoop* reactor,
                        IMetricsSink& metrics,
                        NodeConditions* conditions,
                        ILogger& logger,
                        SharedCacheDialPolicy policy);

    /// @copydoc ICacheUpstream::Fetch
    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override;

    /// @copydoc ICacheUpstream::Store
    [[nodiscard]] core::async::Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override;

    /// @copydoc ICacheUpstream::Configured
    ///
    /// Asked of the CURRENT target rather than fixed at construction, so the scrape follows the
    /// setting as it moves: configured exactly while the setting names another machine, whatever
    /// became of reaching it.
    [[nodiscard]] bool Configured() const noexcept override;

    /// @copydoc ISharedCacheStatusSource::Report
    [[nodiscard]] CompileCacheWire::SharedCacheStatusFields Report() const override;

    /// @copydoc ISharedCacheStatusSource::ReportFor
    [[nodiscard]] CompileCacheWire::SharedCacheStatusFields ReportFor(SharedCacheTarget const& target) const override;

    /// @copydoc ICacheUpstream::StateApplied
    ///
    /// `shared-cache-unproven` is a LIVE row, so it describes the state now, not the last
    /// operation: once the setting stops naming another machine it clears; a setting naming a
    /// machine this node cannot use raises it with the row's reason; and a newly resolved target --
    /// another machine, or the same one now reachable, or at another endpoint -- clears it,
    /// reported as not tried, until an operation tries it. Only a verdict about the very target
    /// the directory names now is kept. Counts nothing: an apply is not an operation, and
    /// `NodeSharedCacheUnresolved` counts operations.
    void StateApplied() override;

    /// @copydoc ICacheUpstream::SharedCacheStatus
    [[nodiscard]] ISharedCacheStatusSource const* SharedCacheStatus() const noexcept override
    {
        return this;
    }

  private:
    /// A session, and the target it was proven for -- which is what it is given back under, even
    /// when an apply has moved the directory on while the exchange ran.
    struct Held
    {
        std::unique_ptr<SealedFrameSocket> socket; ///< Null when no session could be proven.
        Cluster::ResolvedSharedCache target;       ///< What it was proven for.
        bool reused { false };                     ///< Whether it was kept from an earlier operation.
    };

    /// One request and its reply over a proven session: a FETCH or a STORE, already framed by the
    /// caller's choice of verb.
    using Request = std::function<core::async::Task<Cc::CacheOutcome>(SealedFrameSocket*)>;

    /// A session for @p target, once the target has been judged dialable.
    /// @param target The target; by value, as every coroutine parameter here is.
    /// @return The session, null when there is none, and its target.
    [[nodiscard]] core::async::Task<Held> SessionFor(Cluster::ResolvedSharedCache target);

    /// @p request over a session for @p target, retried ONCE on a freshly proven session when a KEPT
    /// one turns out to have died between operations. A fresh session's failure is the answer, and so
    /// is an exchange the deadline ended: that is a slow machine, not a dead session.
    ///
    /// The session goes back to the pool only after a reply was read in full; a transport failure
    /// leaves the stream at a position nobody knows. And never after the server refused to ADMIT
    /// this node: that ends the session and is noted, so the next operation meets it at a proof.
    /// @param target The target; by value.
    /// @param request The exchange; by value.
    /// @return The exchange's outcome, or nothing when no session could be proven.
    [[nodiscard]] core::async::Task<std::optional<Cc::CacheOutcome>> Exchange(Cluster::ResolvedSharedCache target,
                                                                              Request request);

    /// @p request over @p session, bounded by `ioTimeout`: the deadline is armed around the exchange
    /// alone and released when it returns, so it never fires on a session kept idle afterwards.
    /// @param session The session; not owned.
    /// @param request The exchange; not owned, and must outlive the coroutine.
    /// @return Its outcome; a transport failure the deadline caused says `Expired`, whatever the
    ///         read that noticed the close made of it.
    [[nodiscard]] core::async::Task<Cc::CacheOutcome> Bounded(SealedFrameSocket* session, Request const* request);

    /// Whether @p target is one an operation dials, doing the bookkeeping when it is not.
    [[nodiscard]] bool Dials(SharedCacheTarget const& target);

    /// Record how reaching the shared cache went: the report, the condition and one log line per
    /// change.
    ///
    /// **Dropped when @p target is no longer the one the directory names.** An operation takes its
    /// target at its start and notes at its end; an apply between the two has already judged the
    /// new target, and a verdict about the old one would re-raise the row naming a machine nobody
    /// asks about any more. Judged under the same lock `StateApplied` decides under, so no apply can
    /// land between the check and the write.
    /// @param target What the operation dialled.
    /// @param state How it went.
    /// @param detail Why, for a failure.
    void Note(Cluster::ResolvedSharedCache const& target,
              CompileCacheWire::WireSharedCacheState state,
              std::string const& detail);

    /// The verdict, the report and the condition in one step. `_mutex` must be held.
    /// @param target The target the verdict is about.
    /// @param state The verdict.
    /// @param detail Why, for a failure.
    /// @return Whether the target or the verdict changed, which is when a line is logged.
    bool RecordLocked(Cluster::ResolvedSharedCache const& target,
                      CompileCacheWire::WireSharedCacheState state,
                      std::string const& detail);

    /// Log a change `RecordLocked` reported; called without the lock.
    /// @param target The target.
    /// @param state The verdict.
    /// @param detail Why, for a failure.
    void LogChange(Cluster::ResolvedSharedCache const& target,
                   CompileCacheWire::WireSharedCacheState state,
                   std::string const& detail);

    ISharedCacheTargetSource const& _targets;
    SharedSessionPool& _sessions;
    core::net::EventLoop* _reactor;
    IMetricsSink& _metrics;
    NodeConditions* _conditions;
    ILogger& _logger;
    SharedCacheDialPolicy _policy;
    Cc::CredentialNotice _notice;

    /// Guards the verdict AND every write of the condition, so an operation's verdict and an
    /// apply's re-judgement are ordered rather than interleaved.
    mutable std::mutex _mutex;
    /// The whole target the verdict is about, resolution included: an `Unresolved` verdict can then
    /// never be taken for one about a target that dials. Guarded by `_mutex`.
    Cluster::ResolvedSharedCache _reported;
    CompileCacheWire::WireSharedCacheState _state { CompileCacheWire::WireSharedCacheState::NotTried }; ///< Guarded.
    std::string _detail; ///< Guarded by `_mutex`.
};

} // namespace FastCache::Node
