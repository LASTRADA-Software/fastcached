// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "NodeProofClient.hpp"
#include "ReactorHome.hpp"

#include <FastCache/Cluster/SharedCacheTarget.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/SealedFrameSocket.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <CacheProtocol.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IConnector.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// How long the handshake at the dial hint may take: as short as the hint's dial, and for the same
/// reason -- the announced name is the fallback in the same operation.
///
/// Its own budget rather than `ioTimeout`, because a hint is stale in TWO ways and only one of them
/// fails at the dial: an address that now reaches something which accepts and never answers would
/// otherwise hold the operation for a whole exchange budget before the name is tried.
inline constexpr std::chrono::milliseconds HintHandshakeTimeout { 300 };

/// How many proven sessions a node keeps idle to the named machine: ONE. A second operation that
/// overlaps the first opens a session of its own and closes it when it is done, so misses never
/// queue behind one another; a burst costs extra handshakes only while it overlaps.
inline constexpr std::size_t SharedSessionIdleCapacity = 1;

/// How much sooner than the server this node hangs up an idle kept session.
///
/// The server starts its window once it has WRITTEN a reply and this node starts its own once it has
/// READ it, so the server's runs ahead by one one-way trip; and the server looks only every
/// `FrameServer::SweepInterval`, which delays its sweep and never hastens it. So the margin covers a
/// trip and the reactor's timer granularity, with room to spare on an office VPN.
inline constexpr std::chrono::milliseconds SharedSessionHangUpMargin { 1'000 };

/// How long an idle proven session is kept, and then hung up by THIS end.
///
/// **One silence, measured from two ends, and one number.** The server sweeps a connection whose
/// peer has not named its next verb within `FrameServer::HeaderTimeout` -- and COUNTS it, in
/// `fastcache_frame_request_deadline_sweeps_total`, a series whose meaning is that no honest client
/// appears in it. So this end closes first: the bound is DERIVED from the server's constant rather
/// than written beside it, and a timer on the reactor closes each idle session when it runs out.
///
/// Both staleness directions of a kept session, which is a cached answer to "this socket reaches
/// the machine the roster names":
/// - *Kept too long*: bounded here, by the idle timer on the pool's reactor and by the pool's clock
///   at the next `Take`. A session the server closed anyway -- a restart, a network
///   blip -- fails its next exchange at the transport; it is not given back, and the SAME operation
///   proves a fresh one and retries ONCE. A failure on a FRESH session is the answer, and so is one
///   the exchange deadline caused: an expiry is a live machine that is slow, never a dead session.
/// - *Kept across a change* -- the setting moved, the machine was forgotten, its key revoked: every
///   operation compares its target (id, key, endpoint) with the one the session was proven for and
///   drops it on any difference. And the server re-judges admission on every verb, so a kept session
///   of a machine it has since stopped admitting is refused at its next verb -- and that refusal
///   ENDS the session, so the next operation meets it at a handshake.
/// - *Refresh*: on failure, on a refusal and on a target change -- never on a timer while in use,
///   never on a miss. The idle timer runs only while nobody holds the session. *Seam and clock*: the
///   pool's injected `IClock` for the age, its `IReactorHome` for the timer.
inline constexpr std::chrono::milliseconds SharedSessionIdleLimit { FrameServer::HeaderTimeout - SharedSessionHangUpMargin };
static_assert(SharedSessionIdleLimit > std::chrono::milliseconds::zero()
                  && SharedSessionIdleLimit < FrameServer::HeaderTimeout,
              "a kept shared-cache session must be kept for a while, and hung up before the server sweeps it");

/// How long each leg of reaching the shared cache may take.
struct SharedCacheDialPolicy
{
    /// A dial to the dial hint: short, because the announced name is the fallback in the same
    /// operation, and a hint that does not answer quickly is one that is gone.
    std::chrono::milliseconds hintConnectTimeout { 300 };
    /// The handshake at the dial hint; `HintHandshakeTimeout`.
    std::chrono::milliseconds hintHandshakeTimeout { HintHandshakeTimeout };
    /// A dial to the announced endpoint -- `CacheTier.cpp`'s `UpstreamConnectTimeout`.
    std::chrono::milliseconds connectTimeout { 1'000 };
    /// The handshake at the announced endpoint, and then each exchange over a proven session --
    /// `CacheTier.cpp`'s `UpstreamIoTimeout`.
    std::chrono::milliseconds ioTimeout { 5'000 };
};

/// What opening a session to the shared cache came to. **PRIVATE: in-process only**, never
/// transmitted or persisted, so it carries no values.
enum class ProvenOutcome : std::uint8_t
{
    Proved,   ///< The peer proved the key the roster holds for the named machine; the session is sealed.
    WrongKey, ///< The peer at the announced endpoint proved ANOTHER identity or key: nothing was sent.
    /// The named machine proved its key and refused THIS node's proof -- its roster does not hold
    /// this node's key, or holds it revoked. Neither a stale address nor a network: the machine
    /// answered, and it is the right one.
    Refused,
    Failed, ///< Nothing answered, or the handshake did not complete.
    Last,   ///< Not an outcome: the length of a table keyed by one.
};

/// A session to the shared cache, or why there is none.
struct ProvenSession
{
    std::unique_ptr<SealedFrameSocket> socket;       ///< Sealed both ways; null unless `Proved`.
    ProvenOutcome outcome { ProvenOutcome::Failed }; ///< What opening it came to.
    std::string reason;                              ///< Why not, in words an operator acts on; empty for `Proved`.
};

/// Opens proven sessions to the machine the fleet named its shared cache.
///
/// A session is handed over only once the peer has proved exactly the key the roster holds for
/// exactly the id the setting names (`NamedMachineTrust`): until then this node has written a
/// challenge and nothing else, so an address that now reaches another machine learns nothing.
///
/// **The dial hint, and both of its staleness directions.** The hint is the address the last
/// successfully PROVEN session to that machine connected to -- in memory only, keyed by the
/// machine's id AND its key, so it is dialled first and the name's resolution is skipped. It is
/// safe to keep because a stale hint fails CLOSED: the proof decides trust, never the address.
/// - *Too old, the address is gone*: the hint's dial fails within `hintConnectTimeout` and the
///   announced name is dialled in the same operation.
/// - *Too old, the address answers but not as the named machine* -- another machine, or something
///   that accepts and never answers: the handshake at the hint does not prove the named key within
///   `hintHandshakeTimeout`.
/// - Either way it costs one SHORT timeout, once -- the hint is dropped on that failure -- and it is
///   counted a STALE hint, never a wrong key -- only a wrong key at the
///   ANNOUNCED endpoint is an impostor -- the hint is dropped, and the name is dialled in the same
///   operation.
/// - *Refresh*: never on an interval and never on a miss. It is replaced only when a dial proves
///   the key, and it is dropped when the target's id or key changes. A remote peer cannot make
///   this node re-resolve per request: after one failure the hint is gone and every later
///   operation dials the name until it proves.
/// - *Seam and clock*: none. The hint ages by events rather than time, and the name's resolution
///   is the connector's, exactly as every other dial here.
///
/// Reactor thread only: nothing here is locked.
class SharedCacheDialer
{
  public:
    /// @param prover This node's identity, as it proves it; must outlive this.
    /// @param connector Where sockets come from; must outlive this.
    /// @param reactor The loop the handshake's deadline runs on; null for a blocking connector,
    ///        whose socket bounds itself.
    /// @param metrics Where stale hints, wrong keys, failed proofs and opened sessions are counted.
    /// @param policy The dial and handshake bounds.
    SharedCacheDialer(NodeProofClient const& prover,
                      core::net::IConnector& connector,
                      core::net::EventLoop* reactor,
                      IMetricsSink& metrics,
                      SharedCacheDialPolicy policy) noexcept;

    /// Open a proven session to @p target: the hint first when there is one, then the announced
    /// endpoint.
    /// @param target A resolved target -- one whose row `dials`; by value, as every coroutine
    ///        parameter here is.
    /// @return The session, or why there is none.
    [[nodiscard]] core::async::Task<ProvenSession> Open(Cluster::ResolvedSharedCache target);

    /// Tests only: plant the address a proven session to @p target last connected to, as if one
    /// had. The alternative -- a second server re-bound to the first's port -- cannot be arranged
    /// portably.
    /// @param target The target the hint is for.
    /// @param address `host:port`.
    void SetHintForTesting(Cluster::ResolvedSharedCache const& target, std::string address);

  private:
    /// The address a proven session connected to, and whose proof it was.
    struct Hint
    {
        std::string machineId; ///< The id that proved.
        Ed25519PublicKey key;  ///< The key it proved.
        std::string address;   ///< `host:port` it answered at.
    };

    /// One dial and one handshake, and the address the connection reached.
    struct Attempt
    {
        ProvenSession session;       ///< What it came to.
        std::string observedAddress; ///< `host:port` the socket connected to; empty when nothing did.
    };

    /// @param endpoint Where to dial.
    /// @param connectTimeout The dial's budget.
    /// @param handshakeTimeout The handshake's budget.
    /// @param machineId The id the peer must prove.
    /// @param key The key it must prove it under.
    [[nodiscard]] core::async::Task<Attempt> DialAndProve(std::string endpoint,
                                                          std::chrono::milliseconds connectTimeout,
                                                          std::chrono::milliseconds handshakeTimeout,
                                                          std::string machineId,
                                                          Ed25519PublicKey key);

    /// The held hint, when it is for @p target's id and key; a hint for anything else is dropped.
    [[nodiscard]] std::optional<std::string> TakeHintFor(Cluster::ResolvedSharedCache const& target);

    /// Count what a dial that reached the named machine's answer came to, through `NamedDialCounters`:
    /// the one place an outcome becomes a counter.
    /// @param outcome What it came to.
    void CountDialOutcome(ProvenOutcome outcome);

    /// Remember @p address as where @p target's key was last proven; a target with no key keeps nothing.
    void KeepHint(Cluster::ResolvedSharedCache const& target, std::string address);

    NodeProofClient const& _prover;
    core::net::IConnector& _connector;
    core::net::EventLoop* _reactor;
    IMetricsSink& _metrics;
    SharedCacheDialPolicy _policy;
    Cc::CredentialNotice _notice;
    std::optional<Hint> _hint;
};

/// Where the upstream takes a proven session per operation, and gives it back after a clean one.
///
/// Keeps up to `SharedSessionIdleCapacity` idle sessions, each for `SharedSessionIdleLimit`, for the
/// target it was proven for. Reactor thread only, so nothing here is locked.
///
/// **A kept session is a reactor socket with a reactor timer on it**, so the pool never frees one
/// from its destructor: it hands each to its `IReactorHome`, which frees it with that reactor
/// stopped. Whichever thread destroys the pool, nothing is torn down under a running reactor, and
/// the timer still hangs the session up on the reactor in the meantime.
///
/// **The home and the dialer's connector must be ONE loop, and nothing here can check it**: the
/// pool sees a home, the dialer a connector, and neither sees the other. A session dialled on
/// another loop -- or on a blocking connector -- would be closed by this home's timer from a thread
/// that does not drive its socket, or never hung up at all, leaving the server to sweep it as
/// silent. So they are derived from one `NodeIoLoop` in one place, `BuildSharedCacheUpstream`
/// (`CacheTier.cpp`), which takes nothing that could name a second.
class SharedSessionPool
{
  public:
    /// A session for one operation.
    struct Lease
    {
        std::unique_ptr<SealedFrameSocket> socket;       ///< Null unless `outcome` is `Proved`.
        bool reused { false };                           ///< Whether it was kept from an earlier operation.
        ProvenOutcome outcome { ProvenOutcome::Failed }; ///< What opening it came to.
        std::string reason;                              ///< Why not; empty for `Proved`.
    };

    /// @param dialer Where fresh sessions come from; must outlive this.
    /// @param clock What an idle session's age is read from; must outlive this.
    /// @param home The reactor the dialer's sockets belong to, where each idle session's timer runs
    ///        and where the kept ones go when the pool does; must outlive this. REQUIRED, and a
    ///        reference: a pool with no home would free reactor sockets wherever it happened to be
    ///        destroyed, so that state is not representable.
    SharedSessionPool(SharedCacheDialer& dialer, core::platform::IClock& clock, IReactorHome& home) noexcept;

    SharedSessionPool(SharedSessionPool const&) = delete;
    SharedSessionPool(SharedSessionPool&&) = delete;
    SharedSessionPool& operator=(SharedSessionPool const&) = delete;
    SharedSessionPool& operator=(SharedSessionPool&&) = delete;

    /// Hands every kept session to the home rather than freeing it here; see the class.
    ~SharedSessionPool();

    /// A session to @p target: a kept one, when one is held for exactly this target, still open and
    /// younger than `SharedSessionIdleLimit`, and otherwise a freshly proven one.
    /// @param target A resolved target; by value, as every coroutine parameter here is.
    /// @return The lease; `reused` says which.
    [[nodiscard]] core::async::Task<Lease> Take(Cluster::ResolvedSharedCache target);

    /// Give back a session whose exchange ended with a reply read in full and an answer that keeps
    /// it -- never one a transport failure ended, whose stream position nobody knows, nor one the
    /// server refused to admit. Its idle timer starts here.
    /// @param target The target it was proven for.
    /// @param session The session.
    void Give(Cluster::ResolvedSharedCache const& target, std::unique_ptr<SealedFrameSocket> session);

  private:
    /// A proven session nobody is using, what it was proven for, and the timer that hangs it up.
    /// Defined in the source: it holds the reactor's timer, which nothing outside needs to name.
    struct Idle;

    SharedCacheDialer& _dialer;
    core::platform::IClock& _clock;
    IReactorHome& _home;
    /// By pointer: the timer is immovable and points into its entry.
    std::vector<std::unique_ptr<Idle>> _idle;
};

} // namespace FastCache::Node
