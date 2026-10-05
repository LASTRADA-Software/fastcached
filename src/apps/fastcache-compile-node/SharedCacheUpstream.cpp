// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheUpstream.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <tuple>
#include <utility>

#include <core/net/SocketDeadline.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// How one attempt to reach the shared cache is reported, and whether it is a condition.
    struct ReachRow
    {
        ProvenOutcome outcome;            ///< What opening a session came to.
        Wire::WireSharedCacheState state; ///< What `--node-status` calls it.
    };

    /// One row per `ProvenOutcome`, in enumerator order.
    constexpr EnumTable<ProvenOutcome, ReachRow> ReachTable { {
        { .outcome = ProvenOutcome::Proved, .state = Wire::WireSharedCacheState::Proven },
        { .outcome = ProvenOutcome::WrongKey, .state = Wire::WireSharedCacheState::WrongKey },
        { .outcome = ProvenOutcome::Refused, .state = Wire::WireSharedCacheState::ProofRefused },
        { .outcome = ProvenOutcome::Failed, .state = Wire::WireSharedCacheState::Unreachable },
    } };
    static_assert(RowsInEnumeratorOrder(ReachTable, &ReachRow::outcome),
                  "ReachTable must hold one row per ProvenOutcome, in enumerator order");

    /// Whether this node reads the shared cache through this upstream at all: only while the
    /// replicated setting names ANOTHER machine. An override is `RemoteUpstream`'s, and a setting
    /// naming this machine is the host's.
    /// @param target The current target.
    /// @return True when the setting names another machine.
    [[nodiscard]] bool ReadsThroughHere(SharedCacheTarget const& target) noexcept
    {
        return target.source == Wire::WireSharedCacheSource::Setting;
    }

    /// The refusals that END a kept session: the server re-judged admission at this verb and no
    /// longer admits the key the session proved. `NotAMember` alone: it is what the shared tier's
    /// door answers a stranger and a revoked key alike (`SharedStranger`, `KeyForgotten`). Every other
    /// refusal -- `NotSharedCache` included, which is said TO an admitted member -- is an answer
    /// about the request, and the session stays good.
    constexpr std::array SessionEndingRefusals { Wire::ErrorCode::NotAMember };

    /// Whether @p outcome is the server no longer admitting this node over the session it holds.
    /// @param outcome An exchange's outcome.
    /// @return True for a refusal in `SessionEndingRefusals`.
    [[nodiscard]] bool RefusedAdmission(Cc::CacheOutcome const& outcome) noexcept
    {
        return outcome.kind == Cc::CacheOutcomeKind::Rejected && std::ranges::contains(SessionEndingRefusals, outcome.code);
    }

    /// Whether a REUSED session's @p outcome is one that died between operations, and so is worth
    /// one fresh proof: a transport failure the exchange deadline did NOT cause.
    /// @param outcome An exchange's outcome over a kept session.
    /// @return True when the operation retries.
    [[nodiscard]] bool DiedWhileKept(Cc::CacheOutcome const& outcome) noexcept
    {
        return outcome.kind == Cc::CacheOutcomeKind::Transport && outcome.transportFailure != Cc::TransportFailure::Expired;
    }

    /// Whether @p state means this node's builds are not reaching the shared cache.
    /// @param state A noted state.
    /// @return True for every state but `Proven` and `NotTried` -- the second of which is not
    ///         `clear` either, but `not-evaluated` (`RecordLocked`).
    [[nodiscard]] bool Unproven(Wire::WireSharedCacheState state) noexcept
    {
        return state == Wire::WireSharedCacheState::WrongKey || state == Wire::WireSharedCacheState::Unreachable
               || state == Wire::WireSharedCacheState::Unresolved || state == Wire::WireSharedCacheState::ProofRefused;
    }
} // namespace

SharedCacheUpstream::SharedCacheUpstream(ISharedCacheTargetSource const& targets,
                                         SharedSessionPool& sessions,
                                         core::net::EventLoop* reactor,
                                         IMetricsSink& metrics,
                                         NodeConditions* conditions,
                                         ILogger& logger,
                                         SharedCacheDialPolicy policy):
    _targets { targets },
    _sessions { sessions },
    _reactor { reactor },
    _metrics { metrics },
    _conditions { conditions },
    _logger { logger },
    _policy { policy },
    // Silent, because nothing on this leg presents a credential for it to report ignored.
    _notice { Cc::CredentialNotice::Silent() }
{
    // Answered from the start, the way an apply answers it: out of the setting is clear, a machine
    // named and not reachable by key is raised, and one that is reachable is not-evaluated until an
    // operation tries it -- nothing has failed to reach it, and nothing has reached it either.
    StateApplied();
}

bool SharedCacheUpstream::Dials(SharedCacheTarget const& target)
{
    if (!ReadsThroughHere(target))
    {
        // Nothing asks this node to read through here any more -- the setting was unset, names this
        // machine, or an override took over -- so a condition raised about reaching the shared
        // cache no longer describes anything. Cleared at the first operation that finds that,
        // rather than left raised by the last one that failed -- and only while the directory
        // still says so, since an apply may have named a machine after this operation read it.
        std::scoped_lock const lock { _mutex };
        if (_conditions != nullptr && !ReadsThroughHere(_targets.Current()))
            _conditions->Clear(NodeCondition::SharedCacheUnproven);
        return false;
    }
    auto const& row = Cluster::RowOf(target.resolved.resolution);
    if (row.dials)
        return true;
    // Named, and not reachable by key: nothing is dialled, and the operation is counted -- the
    // setting asked for a shared cache this node cannot use.
    _metrics.Increment(IMetricsSink::Counter::NodeSharedCacheUnresolved);
    Note(target.resolved, Wire::WireSharedCacheState::Unresolved, std::string { row.why });
    return false;
}

core::async::Task<SharedCacheUpstream::Held> SharedCacheUpstream::SessionFor(Cluster::ResolvedSharedCache target)
{
    // A session is only ever good for the target it was proven for: the pool hands out a kept one
    // for exactly this target and closes any other, so a move drops what the old target proved.
    auto lease = co_await _sessions.Take(target);
    Note(target, ReachTable[static_cast<std::size_t>(lease.outcome)].state, lease.reason);
    co_return Held { .socket = std::move(lease.socket), .target = std::move(target), .reused = lease.reused };
}

core::async::Task<Cc::CacheOutcome> SharedCacheUpstream::Bounded(SealedFrameSocket* session, Request const* request)
{
    // Bounds the exchange by closing the session, as `RemoteUpstream` bounds its own.
    core::net::SocketDeadlineTarget deadline { .socket = session };
    auto const bound = core::net::armSocketDeadline(_reactor, _policy.ioTimeout, &deadline);
    auto outcome = co_await (*request)(session);
    // The timer records which way the socket died, and only it can: the read that noticed the close
    // cannot tell this end giving up from the peer going away ("expiry and a lost peer are one broken
    // socket").
    if (outcome.kind == Cc::CacheOutcomeKind::Transport && deadline.expired)
        outcome.transportFailure = Cc::TransportFailure::Expired;
    co_return outcome;
}

core::async::Task<std::optional<Cc::CacheOutcome>> SharedCacheUpstream::Exchange(Cluster::ResolvedSharedCache target,
                                                                                 Request request)
{
    auto held = co_await SessionFor(std::move(target));
    if (held.socket == nullptr)
        co_return std::nullopt;
    auto outcome = co_await Bounded(held.socket.get(), &request);

    if (held.reused && DiedWhileKept(outcome))
    {
        // The kept session died between operations -- the server restarted, or a network blip closed
        // it. Once, on a fresh proof; the dead one is not given back, so the pool has nothing to hand
        // out again. An EXPIRY is not this: the machine is there and slow, and a second full budget
        // would only double the wait for the same answer.
        auto again = held.target;
        held = co_await SessionFor(std::move(again));
        if (held.socket == nullptr)
            co_return std::nullopt;
        outcome = co_await Bounded(held.socket.get(), &request);
    }

    if (RefusedAdmission(outcome))
    {
        // The machine that proved itself no longer admits this node's key. The session ends here --
        // closed, never given back -- and the report says so: kept, it would be refused at every
        // later verb while `--node-status` went on saying `Proven`. The next operation proves afresh
        // and meets the same refusal at the handshake, where it is counted.
        Note(held.target, Wire::WireSharedCacheState::ProofRefused, outcome.message);
        co_return outcome;
    }
    if (outcome.kind != Cc::CacheOutcomeKind::Transport)
        _sessions.Give(held.target, std::move(held.socket));
    co_return outcome;
}

core::async::Task<std::optional<std::vector<std::byte>>> SharedCacheUpstream::Fetch(std::string_view key)
{
    auto target = _targets.Current();
    if (!Dials(target))
        co_return std::nullopt;
    // No credential, spelled `{}`: the proven session identifies both ends. And the FLEET verbs.
    auto outcome = co_await Exchange(std::move(target.resolved), [this, key](SealedFrameSocket* session) {
        return Cc::CacheFetch(session, &_notice, key, {}, Wire::FleetSharedCacheVerbs);
    });
    if (!outcome.has_value() || !outcome->IsHit())
        co_return std::nullopt;
    co_return std::move(outcome->value);
}

core::async::Task<UpstreamStore> SharedCacheUpstream::Store(std::string_view key, std::span<std::byte const> value)
{
    auto target = _targets.Current();
    if (!Dials(target))
        // Declined when the setting names a machine this node could not use, NotConfigured when
        // nothing asked this node to use one: the counter of failed stores counts only the first.
        co_return ReadsThroughHere(target) ? UpstreamStore::Declined : UpstreamStore::NotConfigured;
    // Empty roots, for `RemoteUpstream::Store`'s reason: `CacheProxy` canonicalized the value at
    // the private tier's STORE, so it is already tokens. Its views borrow this frame's `key` and
    // `value`, which outlive the exchange. Safe to send twice on the retry: a content-addressed
    // object written twice is the same object.
    Wire::StoreRequest const request { .key = key, .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = value };
    auto const outcome = co_await Exchange(std::move(target.resolved), [this, request](SealedFrameSocket* session) {
        return Cc::CacheStore(session, &_notice, request, {}, Wire::FleetSharedCacheVerbs);
    });
    co_return outcome.has_value() && outcome->kind == Cc::CacheOutcomeKind::Hit ? UpstreamStore::Stored
                                                                                : UpstreamStore::Declined;
}

void SharedCacheUpstream::StateApplied()
{
    auto const target = _targets.Current();
    auto const& row = Cluster::RowOf(target.resolved.resolution);
    {
        std::scoped_lock const lock { _mutex };
        if (!ReadsThroughHere(target))
        {
            // Nothing asks this node to read through here any more: no answer about reaching the
            // shared cache describes anything.
            if (_conditions != nullptr)
                _conditions->Clear(NodeCondition::SharedCacheUnproven);
            return;
        }
        if (row.dials)
        {
            // The very target an operation last reached: what it found still describes it.
            // `Unresolved` never matches here -- its target carries a resolution that does not dial.
            if (_reported == target.resolved)
                return;
            // Another machine, the same one now reachable, or at another endpoint: nothing has tried
            // it yet, and what came before says nothing about it. Not logged: nothing happened to it.
            std::ignore = RecordLocked(target.resolved, Wire::WireSharedCacheState::NotTried, {});
            return;
        }
        // Named, and not reachable by key: said now, not at the next miss.
        if (!RecordLocked(target.resolved, Wire::WireSharedCacheState::Unresolved, std::string { row.why }))
            return;
    }
    LogChange(target.resolved, Wire::WireSharedCacheState::Unresolved, std::string { row.why });
}

bool SharedCacheUpstream::Configured() const noexcept
{
    auto const target = _targets.Current();
    return ReadsThroughHere(target) && Cluster::RowOf(target.resolved.resolution).configured;
}

void SharedCacheUpstream::Note(Cluster::ResolvedSharedCache const& target,
                               Wire::WireSharedCacheState state,
                               std::string const& detail)
{
    {
        std::scoped_lock const lock { _mutex };
        // An apply moved the directory on while this operation ran: the verdict is about a target
        // nobody names any more, and the apply has already judged the one it named.
        if (auto const current = _targets.Current(); !ReadsThroughHere(current) || current.resolved != target)
            return;
        if (!RecordLocked(target, state, detail))
            return;
    }
    LogChange(target, state, detail);
}

bool SharedCacheUpstream::RecordLocked(Cluster::ResolvedSharedCache const& target,
                                       Wire::WireSharedCacheState state,
                                       std::string const& detail)
{
    auto const changed = _reported != target || _state != state;
    _reported = target;
    _state = state;
    _detail = detail;
    if (_conditions != nullptr)
    {
        // Three answers, never two: `NotTried` is neither failing nor reaching the machine, and a
        // `clear` for it would vouch for a shared cache no build has reached (undecided must not
        // read as clear).
        if (Unproven(state))
            _conditions->Raise(NodeCondition::SharedCacheUnproven,
                               std::format("the shared cache {}: {}", target.machineId, detail));
        else if (state == Wire::WireSharedCacheState::NotTried)
            _conditions->NotEvaluated(NodeCondition::SharedCacheUnproven,
                                      std::format("not tried: no build has needed the shared cache {} since the "
                                                  "setting named it, and the first operation that does decides this row",
                                                  target.machineId));
        else
            _conditions->Clear(NodeCondition::SharedCacheUnproven);
    }
    return changed;
}

void SharedCacheUpstream::LogChange(Cluster::ResolvedSharedCache const& target,
                                    Wire::WireSharedCacheState state,
                                    std::string const& detail)
{
    // Once per change of target or of how it went, never per operation: a build of thousands of
    // translation units against an unreachable shared cache says so once.
    if (Unproven(state))
        _logger.Logf(LogLevel::Warn,
                     "the fleet's shared cache {} is not being reached, so builds here compile locally: {}",
                     target.machineId,
                     detail);
    else
        _logger.Logf(
            LogLevel::Info, "reading through to the fleet's shared cache {} at {}", target.machineId, target.endpoint);
}

Wire::SharedCacheStatusFields SharedCacheUpstream::Report() const
{
    return ReportFor(_targets.Current());
}

Wire::SharedCacheStatusFields SharedCacheUpstream::ReportFor(SharedCacheTarget const& target) const
{
    auto fields = Wire::SharedCacheStatusFields {
        .source = target.source, .machineId = {}, .endpoint = {}, .state = Wire::WireSharedCacheState::NotTried, .detail = {}
    };
    switch (target.source)
    {
        case Wire::WireSharedCacheSource::None:
            return fields;
        case Wire::WireSharedCacheSource::Override:
            fields.endpoint = target.overrideEndpoint;
            return fields;
        case Wire::WireSharedCacheSource::ThisMachine:
            fields.machineId = target.resolved.machineId;
            return fields;
        case Wire::WireSharedCacheSource::Setting:
            break;
    }

    fields.machineId = target.resolved.machineId;
    fields.endpoint = target.resolved.endpoint;
    if (auto const& row = Cluster::RowOf(target.resolved.resolution); !row.dials)
    {
        // Unresolved is a fact about the state, not about an attempt: said before anything tries.
        fields.state = Wire::WireSharedCacheState::Unresolved;
        fields.detail = std::string { row.why };
        return fields;
    }
    std::scoped_lock const lock { _mutex };
    // How the last attempt went -- for THIS target; one named before it says nothing about it, and
    // neither does a stored `Unresolved`, whose target does not dial while this one does.
    if (_reported == target.resolved)
    {
        fields.state = _state;
        fields.detail = _detail;
    }
    return fields;
}

} // namespace FastCache::Node
