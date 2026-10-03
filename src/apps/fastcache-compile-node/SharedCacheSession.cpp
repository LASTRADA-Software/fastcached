// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheSession.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <cstddef>
#include <format>
#include <utility>

#include <EndpointDial.hpp>
#include <core/net/SocketDeadline.hpp>

namespace FastCache::Node
{

namespace
{
    /// The largest reply a sealed shared-cache exchange may carry: one object at the launcher's store
    /// ceiling, plus the reply's own framing. A sealing reader holds a whole frame before it can check
    /// the tag, so this bounds what one session holds -- the private tier's in-flight budget, once.
    constexpr std::size_t MaxSharedReplyPayload = Cc::DefaultMaxStoreBytes + (64U * 1024U);

    /// What a dial to the ANNOUNCED endpoint counts, per outcome. A dial to the hint counts none of
    /// these: whatever went wrong there is a stale hint.
    struct NamedDialRow
    {
        ProvenOutcome outcome {};                     ///< What the dial came to.
        std::optional<IMetricsSink::Counter> counter; ///< What it moves; absent for a proof.
    };

    constexpr EnumTable<ProvenOutcome, NamedDialRow> NamedDialCounters { {
        { .outcome = ProvenOutcome::Proved, .counter = std::nullopt },
        { .outcome = ProvenOutcome::WrongKey, .counter = IMetricsSink::Counter::NodeSharedCacheProofsRefusedWrongKey },
        // The handshake did not complete in a session: the catalogue's "did not complete the
        // handshake". The report and the condition say which way.
        { .outcome = ProvenOutcome::Refused, .counter = IMetricsSink::Counter::NodeSharedCacheProofsFailed },
        { .outcome = ProvenOutcome::Failed, .counter = IMetricsSink::Counter::NodeSharedCacheProofsFailed },
    } };
    static_assert(RowsInEnumeratorOrder(NamedDialCounters, &NamedDialRow::outcome),
                  "NamedDialCounters must hold one row per ProvenOutcome, in enumerator order");

    /// What a handshake that did not prove came to.
    ///
    /// Three answers, because they are three different places to look: a peer that proved ANOTHER
    /// identity or key (this node refused it), the named machine itself refusing this node (the
    /// server's trust said `Named`, and then it refused the proof), and everything that never
    /// completed.
    /// @param attempt What the handshake learned; not `Proved`.
    /// @return `WrongKey`, `Refused` or `Failed`.
    [[nodiscard]] ProvenOutcome UnprovenOutcomeOf(NodeProofAttempt const& attempt) noexcept
    {
        if (!attempt.standing.has_value())
            return ProvenOutcome::Failed;
        if (attempt.result == NodeProofResult::Untrusted
            && (*attempt.standing == ServerStanding::NotNamed || *attempt.standing == ServerStanding::NamedUnderOtherKey))
            return ProvenOutcome::WrongKey;
        if (attempt.result == NodeProofResult::Refused && *attempt.standing == ServerStanding::Named)
            return ProvenOutcome::Refused;
        return ProvenOutcome::Failed;
    }
} // namespace

SharedCacheDialer::SharedCacheDialer(NodeProofClient const& prover,
                                     core::net::IConnector& connector,
                                     core::net::EventLoop* reactor,
                                     IMetricsSink& metrics,
                                     SharedCacheDialPolicy policy) noexcept:
    _prover { prover },
    _connector { connector },
    _reactor { reactor },
    _metrics { metrics },
    _policy { policy },
    // Silent, because nothing on this leg ever presents a credential for it to report ignored.
    _notice { Cc::CredentialNotice::Silent() }
{
}

std::optional<std::string> SharedCacheDialer::TakeHintFor(Cluster::ResolvedSharedCache const& target)
{
    if (!_hint.has_value())
        return std::nullopt;
    if (_hint->machineId != target.machineId || !target.key.has_value() || _hint->key != *target.key)
    {
        // Another machine, or the same id under another key: the hint was proven for something
        // this node no longer dials, so it goes.
        _hint.reset();
        return std::nullopt;
    }
    return _hint->address;
}

void SharedCacheDialer::KeepHint(Cluster::ResolvedSharedCache const& target, std::string address)
{
    if (target.key.has_value())
        _hint = Hint { .machineId = target.machineId, .key = *target.key, .address = std::move(address) };
}

void SharedCacheDialer::SetHintForTesting(Cluster::ResolvedSharedCache const& target, std::string address)
{
    KeepHint(target, std::move(address));
}

core::async::Task<SharedCacheDialer::Attempt> SharedCacheDialer::DialAndProve(std::string endpoint,
                                                                              std::chrono::milliseconds connectTimeout,
                                                                              std::chrono::milliseconds handshakeTimeout,
                                                                              std::string machineId,
                                                                              Ed25519PublicKey key)
{
    auto raw = co_await Cc::DialEndpoint(&_connector, endpoint, core::net::DialOptions { .connectTimeout = connectTimeout });
    if (raw == nullptr)
        co_return Attempt { .session = ProvenSession { .socket = nullptr,
                                                       .outcome = ProvenOutcome::Failed,
                                                       .reason = std::format("nothing answered at {}", endpoint) },
                            .observedAddress = {} };

    // No budget: this end reads replies, and what it holds is bounded by the payload ceiling alone.
    auto sealed =
        std::make_unique<SealedFrameSocket>(std::move(raw), SealedFrameEnd::Caller, MaxSharedReplyPayload, nullptr);

    auto attempt = NodeProofAttempt {};
    {
        // Built after the dial, from values, and handed to the one await that reads it (#1545).
        NamedMachineTrust const trust { std::move(machineId), key };
        // Bounds the handshake alone; each exchange over the session arms its own.
        core::net::SocketDeadlineTarget deadline { .socket = sealed.get() };
        auto const bound = core::net::armSocketDeadline(_reactor, handshakeTimeout, &deadline);
        // No credential: the proven session identifies both ends, and a proof presents none
        // (`NodeProofClient::ProveAsync` takes no parameter to present one with).
        attempt = co_await _prover.ProveAsync(sealed.get(), &trust);
    }

    // The address the connection reached, with the port that was dialled: a name that resolves to
    // several addresses is remembered as the one that answered. Read AFTER the handshake rather than
    // held across it (the rulebook's ARM64 coroutine rule), from the one socket both reads share.
    auto observed = std::string { endpoint };
    if (auto const peer = sealed->peerAddress(); !peer.empty())
        if (auto const dialled = ParseDialEndpoint(endpoint); dialled.has_value())
            observed = FormatHostPort(peer, dialled->second);

    if (attempt.result == NodeProofResult::Proved)
    {
        _metrics.Increment(IMetricsSink::Counter::NodeSharedCacheSessionsOpened);
        co_return Attempt {
            .session = ProvenSession { .socket = std::move(sealed), .outcome = ProvenOutcome::Proved, .reason = {} },
            .observedAddress = std::move(observed)
        };
    }
    co_return Attempt { .session = ProvenSession { .socket = nullptr,
                                                   .outcome = UnprovenOutcomeOf(attempt),
                                                   .reason = std::move(attempt.reason) },
                        .observedAddress = std::move(observed) };
}

core::async::Task<ProvenSession> SharedCacheDialer::Open(Cluster::ResolvedSharedCache target)
{
    if (!target.key.has_value() || target.endpoint.empty())
        // A caller that asked for an unresolved target; nothing is dialled for it.
        co_return ProvenSession { .socket = nullptr,
                                  .outcome = ProvenOutcome::Failed,
                                  .reason = std::format("{} has no key and endpoint to prove", target.machineId) };
    // The hint first: the address the last PROVEN session to this id under this key connected to.
    if (auto hint = TakeHintFor(target); hint.has_value())
    {
        auto atHint = co_await DialAndProve(
            *std::move(hint), _policy.hintConnectTimeout, _policy.hintHandshakeTimeout, target.machineId, *target.key);
        if (atHint.session.outcome == ProvenOutcome::Proved)
        {
            KeepHint(target, std::move(atHint.observedAddress));
            co_return std::move(atHint.session);
        }
        if (atHint.session.outcome == ProvenOutcome::Refused)
        {
            // The hint reached the RIGHT machine -- it proved the named key -- and that machine
            // refused this node. The hint is not stale, and the name would reach the same machine
            // and the same refusal, so the answer is this one. Counted as the name's would be.
            CountDialOutcome(atHint.session.outcome);
            co_return std::move(atHint.session);
        }
        // Anything else at the hint -- nothing answered, somebody else did, or nothing finished --
        // is a stale hint, never an impostor: the name is what the cluster announced, the hint only
        // what it was.
        _hint.reset();
        _metrics.Increment(IMetricsSink::Counter::NodeSharedCacheStaleHints);
    }

    auto named =
        co_await DialAndProve(target.endpoint, _policy.connectTimeout, _policy.ioTimeout, target.machineId, *target.key);
    if (named.session.outcome == ProvenOutcome::Proved)
        KeepHint(target, std::move(named.observedAddress));
    CountDialOutcome(named.session.outcome);
    co_return std::move(named.session);
}

void SharedCacheDialer::CountDialOutcome(ProvenOutcome outcome)
{
    if (auto const& row = NamedDialCounters[static_cast<std::size_t>(outcome)]; row.counter.has_value())
        _metrics.Increment(*row.counter);
}

struct SharedSessionPool::Idle
{
    /// @param provenFor The target the session was proven for.
    /// @param session The session.
    /// @param givenBack When it was given back, on the pool's clock.
    /// @param reactor Where its hang-up is timed.
    Idle(Cluster::ResolvedSharedCache provenFor,
         std::unique_ptr<SealedFrameSocket> session,
         core::platform::SteadyTimePoint givenBack,
         core::net::EventLoop& reactor):
        target { std::move(provenFor) },
        socket { std::move(session) },
        since { givenBack },
        hangUp { .socket = socket.get() },
        timer { core::net::armSocketDeadline(&reactor, SharedSessionIdleLimit, &hangUp) }
    {
    }

    Idle(Idle const&) = delete;
    Idle(Idle&&) = delete;
    Idle& operator=(Idle const&) = delete;
    Idle& operator=(Idle&&) = delete;
    ~Idle() = default;

    Cluster::ResolvedSharedCache target;       ///< The target it was proven for.
    std::unique_ptr<SealedFrameSocket> socket; ///< The session.
    core::platform::SteadyTimePoint since;     ///< When it was given back.
    core::net::SocketDeadlineTarget hangUp;    ///< What the timer closes; points at `socket`.
    /// Closes the session on the reactor once it has idled `SharedSessionIdleLimit`. Declared LAST,
    /// so it is disarmed before the socket and the target it points at go.
    std::optional<core::net::DeadlineTimer> timer;
};

SharedSessionPool::SharedSessionPool(SharedCacheDialer& dialer, core::platform::IClock& clock, IReactorHome& home) noexcept:
    _dialer { dialer },
    _clock { clock },
    _home { home }
{
}

SharedSessionPool::~SharedSessionPool()
{
    // A reactor's socket and timer are never freed here, whichever thread this is: DEFERRED to the
    // home, which frees them with the reactor stopped. The timer is not touched, so it still hangs the
    // session up on the reactor meanwhile -- the server sees this end leave, never a silence to sweep.
    for (auto& held: _idle)
        _home.Retire(std::move(held));
}

core::async::Task<SharedSessionPool::Lease> SharedSessionPool::Take(Cluster::ResolvedSharedCache target)
{
    // Anything too old, closed by its timer, or proven for another target, is not a candidate: closed,
    // not kept -- on this, the reactor's, thread. Asked BEFORE the await, so nothing computed here is
    // held across it.
    //
    // The `isClosed()` clause has no case of its own, deliberately: without it a session its timer
    // closed would be handed out, fail at the transport and be retried once on a fresh proof -- the
    // same answer for one failed write. It is here to skip that write, not to change an outcome.
    auto const now = _clock.now();
    std::erase_if(_idle, [&target, now](std::unique_ptr<Idle> const& held) {
        return held->target != target || held->socket->isClosed() || now - held->since >= SharedSessionIdleLimit;
    });
    if (!_idle.empty())
    {
        // Its timer goes with its entry: a session in use is never hung up by it.
        auto socket = std::move(_idle.back()->socket);
        _idle.pop_back();
        co_return Lease { .socket = std::move(socket), .reused = true, .outcome = ProvenOutcome::Proved, .reason = {} };
    }

    auto opened = co_await _dialer.Open(std::move(target));
    co_return Lease {
        .socket = std::move(opened.socket), .reused = false, .outcome = opened.outcome, .reason = std::move(opened.reason)
    };
}

void SharedSessionPool::Give(Cluster::ResolvedSharedCache const& target, std::unique_ptr<SealedFrameSocket> session)
{
    // Beyond capacity -- an overlapping operation's session -- it is closed by going out of scope.
    if (session == nullptr || session->isClosed() || _idle.size() >= SharedSessionIdleCapacity)
        return;
    _idle.push_back(std::make_unique<Idle>(target, std::move(session), _clock.now(), _home.Loop()));
}

} // namespace FastCache::Node
