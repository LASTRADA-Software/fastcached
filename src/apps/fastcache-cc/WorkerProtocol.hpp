// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"
#include "CompileJob.hpp"

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
/// Injected rather than called directly, because "ask the scheduler" is I/O and
/// this file has none. It is also the seam where a worker's trust model can change
/// without touching the framing: today the plausible implementations are "accept
/// any token" (a fleet on a trusted network, where the scheduler's reachability is
/// the boundary) and "ask the scheduler", and a signed token would be a third.
///
/// @param leaseToken The token the client presented.
/// @param fingerprint The toolchain the client says it compiled against.
/// @return True when the job may run.
using LeaseValidator = std::function<bool(std::string_view leaseToken, std::string_view fingerprint)>;

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
    /// @param validator Decides whether a lease token authorizes a job.
    /// @param acceptedCodecs What this worker can decode, most-preferred first.
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
    /// @param acceptedCodecs What this worker can decode.
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
