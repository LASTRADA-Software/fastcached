// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"
#include "CompileJob.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Decide whether a lease token authorizes a job.
///
/// Injected rather than called directly, because reading a cluster key and a wall
/// clock is I/O and this file has none. It is also the seam where a worker's trust
/// model lives: today the two implementations are "verify the scheduler's
/// signature" and "accept anything", and which one a node builds is a startup
/// decision it announces rather than a per-request fallback.
///
/// **Returns a REFUSAL, not a `bool`.** Three refusals are distinguishable on the
/// wire and counted apart (`LeaseUnauthorized`, `LeaseEndpointMismatch`,
/// `LeaseExpired`), so a boolean would collapse exactly the distinction an operator
/// needs: somebody probing the port, a worker whose advertised endpoint is not the
/// one clients dial, and a machine whose clock has drifted are three different
/// things to go and do.
///
/// The whole `LeaseRefusal` rather than its `reason` alone, because the other member
/// is a diagnostic built FOR this caller: `VerifyLeaseToken` formats "this lease was
/// issued for X; this worker answers on Y" and "this lease expired N seconds ago",
/// and returning the enum on its own allocated those strings and dropped them. They
/// are safe to send precisely because `detail` is only ever populated after the MAC
/// verified -- everything it names is already in the token the caller is holding, in
/// the clear -- so a refusal an operator can act on costs nothing extra.
///
/// **It does not take the endpoint.** The endpoint a grant is checked against is
/// the WORKER's own advertised address, which is fixed for the life of the process
/// and is not the client's to state -- so a production implementation captures it,
/// along with the signing key and the clock, and this signature carries only what
/// arrives in the request. A parameter here would invite a caller to pass something
/// the request supplied, which is the whole failure the endpoint is inside the MAC
/// to prevent.
///
/// @param leaseToken The token the client presented.
/// @param fingerprint The toolchain the client says it compiled against.
/// @return Nothing when the job may run, or why it may not.
using LeaseValidator =
    std::function<std::optional<Distributed::LeaseRefusal>(std::string_view leaseToken, std::string_view fingerprint)>;

/// The validator a worker holding the cluster's key builds.
///
/// The whole check, and it costs no round trip: a grant carries an HMAC over the
/// worker's endpoint, the toolchain, the object key and an expiry, signed with the
/// key both ends already have for discovery. So this is a local
/// `Distributed::VerifyLeaseToken` and nothing else.
///
/// It lives HERE rather than beside the node's `main`, next to the only class that
/// consults it, because that is the one place both binaries compile: `main.cpp` is
/// not a unit-testable translation unit, and a trust decision that cannot be
/// exercised directly gets exercised by nothing.
///
/// The endpoint is captured, never taken per request. A grant is signed FOR one
/// worker, and that is what stops a token captured on the way to one machine from
/// being replayed against every other machine trusting the same key -- so the
/// address checked against has to be this worker's own, established once at startup.
///
/// @param signingKey The cluster's pre-shared key; copied, since it outlives no
///        particular call.
/// @param advertisedEndpoint This worker's address as clients are told to dial it --
///        exactly the string it registered with the scheduler under, because that is
///        the string the scheduler signed.
/// @param clock Where "now" comes from. A **wall** clock, not a steady one: the
///        expiry was stamped on another machine, and a steady instant means nothing
///        off the host that read it. Borrowed, so it must outlive the validator.
/// @return The validator.
[[nodiscard]] LeaseValidator SignedLeaseValidator(std::vector<std::byte> signingKey,
                                                  std::string advertisedEndpoint,
                                                  IWallClock const& clock);

/// The validator a worker with no cluster key builds: it refuses nothing.
///
/// A named function rather than a lambda written out at each call site, because it
/// is a **policy** and deserves to be greppable -- and because a bare
/// `[](auto, auto) { return std::nullopt; }` in production code reads as a stub
/// somebody forgot to replace rather than as a decision.
///
/// Legitimate for exactly one shape of node: one that admits nothing but its own
/// machine. A process on this host already has this host's compiler, so a lease
/// check there escalates nobody. Every other shape is refused at startup by
/// `NodeConfig`'s policy table rather than degraded to this quietly, which is the
/// distinction that matters: a per-request fallback leaves the port open, every
/// refusal counter at zero, and the node looking healthy from both ends.
/// @return A validator that authorizes every job.
[[nodiscard]] LeaseValidator UncheckedLeaseValidator();

/// Turn one `0xFC` request into one reply, for a compile worker.
///
/// **Pure**: bytes in, bytes out. It opens no socket and spawns nothing itself —
/// spawning is `CompileJobRunner`'s, which is injected. That is what lets the whole
/// worker protocol be tested by handing it a frame and reading the answer, with no
/// listener and no compiler installed.
///
/// A worker answers exactly one verb, `Compile`. Everything else — including the
/// scheduler's own verbs — is refused with `DispatchNotPermitted`, because a worker
/// is not a scheduler and not a cache. That refusal is a *reply*: a client that
/// sent the wrong verb to the wrong port learns which, rather than seeing a dropped
/// connection it cannot tell from a dead host.
class WorkerProtocol
{
  public:
    /// @param jobs Runs the compiles; must outlive this.
    /// @param validator Decides whether a lease token authorizes a job. **Required**,
    ///        and never left empty: an empty `std::function` would be a third lease
    ///        policy -- "refuse nothing" -- reachable by omission, unnamed, untested
    ///        and absent from the startup log. That is the exact shape of the defect
    ///        this seam was built to close, so a node with no key passes
    ///        `UncheckedLeaseValidator()` and says so out loud instead.
    /// @param acceptedCodecs What this worker can produce and decode, most-preferred
    ///        first — normally `AvailableCodecs()`. It is one list because it is one
    ///        negotiation: the same value is registered with the scheduler, relayed to
    ///        clients in their grant, and used here to pick the envelope the object
    ///        goes home in. A worker built with a narrower list than it can actually
    ///        speak simply answers in a weaker codec; one built with a *wider* list
    ///        falls back to `Identity` rather than answering in a codec it cannot
    ///        produce.
    /// @param metrics Where job outcomes are counted; must outlive this.
    ///
    /// The metrics sink is injected like every other collaborator rather than
    /// reached for: a worker under test counts into one the test can read, and the
    /// interface is header-only and depends on nothing but the standard library,
    /// so including it costs `fastcache-cc` — which compiles this file in without
    /// linking `FastCache` — nothing at link time.
    /// @param maxDecompressedBytes Ceiling on what a request's codec envelope may
    ///        declare it expands to. **The surface's own request cap**, passed in
    ///        rather than assumed: this class never sees the listener that enforced
    ///        the frame length, so a listener with a smaller cap has to say so or the
    ///        two disagree about how much memory one request may cost. The default is
    ///        the figure every framed surface here uses.
    WorkerProtocol(CompileJobRunner& jobs,
                   LeaseValidator validator,
                   CompileCacheWire::CodecList acceptedCodecs,
                   IMetricsSink& metrics,
                   std::size_t maxDecompressedBytes = DefaultMaxDecompressedBytes);

    /// Answer one complete request frame.
    /// @param frame A whole request, header included.
    /// @return The reply frame, or nullopt when the request is not this protocol at
    ///         all (a wrong magic), which is the one case a caller must answer by
    ///         closing rather than replying.
    [[nodiscard]] std::optional<std::vector<std::byte>> Answer(std::span<std::byte const> frame);

  private:
    /// Handle a decoded COMPILE payload.
    [[nodiscard]] std::vector<std::byte> Compile(std::span<std::byte const> payload);

    CompileJobRunner& _jobs;
    LeaseValidator _validator;
    CompileCacheWire::CodecList _acceptedCodecs;
    IMetricsSink& _metrics;
    /// What a request's envelope may declare it expands to; see the constructor.
    std::size_t _maxDecompressedBytes;
};

/// What one request declares it will make a worker hold, in bytes.
///
/// The frame's own `payloadLength` is not the answer, and believing it was reopened
/// the hole the in-flight byte budget exists to close, one layer in: a COMPILE
/// carries its preprocessed source in a **codec envelope**, whose `rawLen` is what
/// `Unenvelope` sizes its output buffer from. A few dozen compressed bytes may
/// declare a 256 MiB expansion, so a caller charging only the frame length admits
/// `slots` of them having reserved almost nothing -- `slots` times the per-request
/// ceiling, asked for by any cluster member
/// ([#241](https://github.com/LASTRADA-Software/fastcached/issues/241)).
///
/// The **larger** of the two rather than their sum, and the budget therefore bounds
/// the surface at a constant multiple of one request rather than byte-exactly.
/// Byte-exactness was never on offer and is not what was lost: by the time this is
/// asked, the reader's own buffer, the payload copy and the assembled frame are
/// already three copies of the same bytes. What the sum would buy is a number that
/// still is not the peak, at the price of refusing one honest maximal translation
/// unit on an idle worker -- which is the same mistake as dividing the per-request
/// cap by the slot count, arrived at from the other side. What matters, and what
/// this restores, is that the bound does not grow with `slots`.
///
/// Lives HERE, beside the code that spends it, rather than in the accept loop that
/// charges it: which field of which verb carries an envelope is `Compile`'s own
/// knowledge, and a second reading of it in another file is two answers that would
/// have to agree forever.
///
/// @param frame A whole request, header included.
/// @return The largest buffer this request declares, or the frame's payload length
///         when it declares nothing more than that -- including every request that
///         is not a decodable COMPILE, which the protocol refuses on its own terms.
[[nodiscard]] std::size_t DeclaredRequestFootprint(std::span<std::byte const> frame) noexcept;

/// Register this worker with a scheduler, and keep it registered.
///
/// Separate from `WorkerProtocol` because it is the one part of a worker that
/// *initiates*: everything else answers. Split out so the answering half stays pure
/// and this half can be driven against a scripted `ISocket`.
class WorkerRegistrar
{
  public:
    /// @param fingerprint The toolchain this worker serves.
    /// @param endpoint host:port clients should reach this worker on.
    /// @param slots Concurrent job limit to advertise, or 0 to let the scheduler
    ///        derive one from `capacity`. Zero is what a node should normally send:
    ///        deriving it here would put a workstation's core reserve in every
    ///        worker rather than in the one place that can be checked.
    /// @param acceptedCodecs What this worker can produce and decode — the SAME list
    ///        its `WorkerProtocol` is constructed with, normally `AvailableCodecs()`.
    ///        One list, one negotiation: the scheduler files this against the worker
    ///        and the grant relays it to clients, so it governs what a client
    ///        compresses its source with just as the protocol's copy governs what the
    ///        object comes back in. Two spellings here is how a node comes to advertise
    ///        something it does not answer in.
    /// @param capacity What this machine is, for the scheduler to size it by.
    WorkerRegistrar(std::string fingerprint,
                    std::string endpoint,
                    std::uint32_t slots,
                    CompileCacheWire::CodecList acceptedCodecs,
                    CompileCacheWire::CapacityFields capacity = {});

    /// Announce this worker over an already-connected scheduler client.
    ///
    /// Reports the scheduler's own words rather than a bare false, because the
    /// reasons differ in what an operator must do about them and only the
    /// scheduler knows which one applies: a fleet this node is not a member of, a
    /// leader that has moved, a toolchain fingerprint that is not text. A worker
    /// that discarded them would disappear from the fleet with nothing anywhere
    /// saying why -- the node's own log can only report that it did not register.
    /// @param scheduler Connected transport; not owned.
    /// @param credential Credential to present.
    /// @return Nothing when the scheduler accepted, and the assigned id is kept
    ///         internally; otherwise a phrase naming the refusal or the transport
    ///         failure, ready to log.
    [[nodiscard]] std::expected<void, std::string> Register(ISocket& scheduler, Credential const& credential = {});

    /// Report liveness and current load.
    ///
    /// A scheduler that does not know this worker answers `unknown-lease`, which is
    /// its way of saying "register again" — a worker that ignored that would
    /// heartbeat into a void forever while the fleet ran without it. So this
    /// reports the need to re-register rather than merely failing.
    /// @param scheduler Connected transport; not owned.
    /// @param inFlight Jobs running right now.
    /// @param load What else this machine has to say about itself right now.
    /// @param credential Credential to present.
    /// @return True when accepted; false means "register again".
    [[nodiscard]] bool Heartbeat(ISocket& scheduler,
                                 std::uint32_t inFlight,
                                 CompileCacheWire::LoadFields const& load = {},
                                 Credential const& credential = {});

    /// The id the scheduler assigned, empty until a successful `Register`.
    [[nodiscard]] std::string const& WorkerId() const noexcept
    {
        return _workerId;
    }

    /// The toolchain this registrar announces, so a diagnostic can name which of
    /// a node's several registrars it is about.
    [[nodiscard]] std::string const& Fingerprint() const noexcept
    {
        return _fingerprint;
    }

  private:
    std::string _fingerprint;
    std::string _endpoint;
    std::uint32_t _slots;
    CompileCacheWire::CodecList _acceptedCodecs;
    CompileCacheWire::CapacityFields _capacity;
    std::string _workerId;
};

} // namespace FastCache::Cc
