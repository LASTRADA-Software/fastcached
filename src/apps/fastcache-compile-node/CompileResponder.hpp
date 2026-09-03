// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CompileCapacity.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// Serves the compile verbs on the node's merged `0xFC` listener.
///
/// The second half of #290, and it is **a change to how compiles RUN, not to how frames
/// are routed**. Routing a `Compile` to a component is one row of `MergedResponder`;
/// what makes it work is the two lines in `Answer` below, and they are the whole
/// subject of this class.
///
/// ## The reactor may not compile
///
/// `WorkerServer` serves its own port over a **blocking** listener, so one hop suffices
/// there: `Serve` steps onto the executor before it reads anything, and nothing after
/// that line suspends -- `BlockingSocket::Read` does its `recv` eagerly and hands back
/// an already-ready awaitable, so the whole request stays on the pool thread.
///
/// This surface is a **reactor**. A frame arrives on the reactor thread, and:
///
///   1. `_protocol.Answer` spawns a compiler and blocks for seconds. Run there it would
///      stall every other connection that reactor owns -- #213 one layer over, and the
///      worker would advertise `slots` while running one at a time.
///   2. The reply is written by `FrameEndpoint`'s connection task to a **reactor
///      socket**, so the answer has to come back to the reactor thread before it is
///      returned.
///
/// **Neither failure is visible at a call site.** A compile served on the reactor
/// produces a correct object; a reply returned from the pool thread writes a correct
/// socket, most of the time, on the platform you tested. So the property is asserted by
/// `CompileResponder_test.cpp` on the thread identities themselves -- a test that only
/// checks the object passes under both bugs.
///
/// ## It spends the worker's own accounting, not a second one
///
/// The `CompileCapacity` here is `WorkerServer`'s: the same slot cap, the same in-flight
/// byte budget, the same drain. Two counters would make one machine answer to two caps
/// depending on which door a client used, and the slot figure it advertises to the fleet
/// would describe neither.
///
/// ## What it does NOT change
///
/// Every policy is the one the dedicated port already applies. Membership is
/// `RefuseUnlessMember`, shared with the accept loop rather than restated. The lease is
/// checked where it always was, inside `Cc::WorkerProtocol`, by the validator this node
/// chose at startup. Nothing here widens who may compile on this machine -- the merged
/// listener is an additional door onto the same policy.
class CompileResponder final: public IFrameResponder
{
  public:
    /// @param protocol Answers each compile; must outlive this. The same object the
    ///        dedicated worker port answers with, so the lease validator, the codec
    ///        list and the toolchain map cannot differ between the two doors.
    /// @param capacity `WorkerServer::Capacity()`, never a second one; must outlive
    ///        this.
    /// @param membership Decides who may spend this machine's CPU; must outlive this.
    /// @param jobs Where a compile runs. Sized to the slot cap by the assembler, which
    ///        is what makes an admitted job always find a thread. Must outlive this.
    /// @param home Where the answer is returned from -- the reactor this surface's
    ///        sockets belong to. **Not derivable here**: `Answer` is handed a frame and
    ///        a peer, and nothing in either says which loop is driving the connection,
    ///        so the one thread the reply may be produced on is injected rather than
    ///        guessed. Must outlive this.
    /// @param metrics Where the refusals and the byte counters land; must outlive this.
    /// @param logger Where a compile that threw is reported; must outlive this.
    CompileResponder(Cc::WorkerProtocol& protocol,
                     CompileCapacity& capacity,
                     Distributed::IMembershipOracle const& membership,
                     IExecutor& jobs,
                     IExecutor& home,
                     IMetricsSink& metrics,
                     ILogger& logger) noexcept:
        _protocol { protocol },
        _capacity { capacity },
        _membership { membership },
        _jobs { jobs },
        _home { home },
        _metrics { metrics },
        _logger { logger }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Admits, hops to the pool, compiles, hops back, answers. The two hops are the
    /// point; see the class comment for why each one is invisible when it is missing.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// Membership, decided from the peer's host alone -- so the endpoint asks it before
    /// a payload byte is read, and a caller with no claim on this machine cannot make it
    /// buffer a multi-megabyte preprocessed translation unit on the way to being refused
    /// (#285, #377). The same ordering the accept loop has used since it existed.
    ///
    /// **This is not the whole gate**, and deliberately so: a compile also needs a lease
    /// the scheduler issued for this worker's endpoint, and that is a property of the
    /// payload rather than of the peer. It is checked where it always was, inside
    /// `Cc::WorkerProtocol`.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No, and stated rather than inherited.** `Op::Compile` is a `RequiresAuth` row of
    /// the wire table, so this answer is what decides whether the merged listener demands
    /// a connection credential before it -- and demanding one would refuse every client
    /// the dedicated port serves today, for a door that is supposed to be additional.
    ///
    /// The reason it is safe is that a compile already carries its own credential, one
    /// layer in and per job rather than per connection: the lease token the scheduler
    /// signed for **this worker's endpoint**, verified by the validator this node chose
    /// at startup. A connection-scoped secret would be strictly weaker than that -- it
    /// says who is attached, where the lease says which job was granted, to whom, and
    /// for how long -- and holding both would mean an operator configuring a second
    /// secret to reach a surface the first one already governs.
    ///
    /// So the answer follows the VERB, which is exactly what #290 made expressible: the
    /// scheduler verbs on this same listener still require the credential #289 added.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// There is no connection-scoped policy here, so this answers `NoPolicy` for every
    /// payload. It is unreachable in practice -- `AUTH` is a `Session` verb and
    /// `MergedResponder` routes the credential to the scheduler -- and is written down
    /// rather than left to a default for the reason the interface is pure virtual: a
    /// surface that inherits an answer inherits an open door by saying nothing.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(nullptr, payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    ///
    /// Counted against this worker's own frame-level series, so a client hammering the
    /// merged port with oversized COMPILE declarations moves the same counter it moves
    /// on the dedicated one. An operator reads the two side by side and must not have to
    /// know which door a refusal came through to find it.
    ///
    /// Every arm goes through `Cc::Refuse`, including the two this surface cannot
    /// produce: `ctest -R worker-refusals-counted` covers this file now, and it covers
    /// it by there being no `EncodeErrorReply` here to leave a refusal uncounted.
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    ///
    /// The byte-budget arm is the one that matters and the one that regressed: the
    /// dedicated compile port refused an unaffordable frame with
    /// `CompileRefusal::EndpointBusy` and the merged listener answered a bare code, so
    /// `worker_jobs_refused_endpoint_busy_total` -- which the operator documentation
    /// still tells a reader to watch -- stopped moving when that port was retired
    /// (#447).
    ///
    /// The credential arms cannot fire here: `AUTH` belongs to the `Session` family,
    /// which `MergedResponder` routes to the scheduler, and this surface answers
    /// `AuthRequired` false because a compile carries its own per-job lease. They are
    /// counted anyway, for the reason `RefusalFor` gives about its own two dead arms:
    /// if either ever fires, what changed is who may compile here, and a counter is how
    /// an operator would find out.
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// **However long a compiler runs, which is the whole of #223 arriving on the
    /// server side.** The endpoint's own window is five seconds -- right for a cache
    /// round trip, and a guarantee that every translation unit worth distributing is
    /// compiled and then thrown away, because `ServeConnection` is not parked on the
    /// socket while this answers: the sweep closes it silently, the compile finishes,
    /// hops home, and its write fails. Short TUs would keep succeeding, so a smoke
    /// test passes.
    ///
    /// `Wire::DefaultCompileLeaseTimeout` rather than a number of this surface's own,
    /// and it is the same argument #249 made for the two ends of the client's side:
    /// past it the scheduler has reclaimed the lease and may have re-granted the key,
    /// so an object produced after it is one nobody can use. Serving longer than the
    /// grant lives is doing work the fleet has already given away.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return CompileCacheWire::DefaultCompileLeaseTimeout;
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    ///
    /// The worker's own ceiling, and the same constant `Cc::WorkerProtocol` was built
    /// with -- which is why it is exported from `WorkerServer.hpp` rather than spelled
    /// again here. A surface that buffered more than the decoder's ceiling would accept,
    /// and allocate, precisely the frames the budget skips.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return WorkerMaxRequestBytes;
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// Hundreds, for the reason the cache surface's is: this bounds descriptors, not
    /// work. What bounds the expensive thing is the slot cap, which is checked in
    /// `Answer` and refuses with `NoCapacity` rather than queueing -- so sizing this to
    /// the slot count would make a client that arrives at a busy worker wait for a
    /// descriptor instead of being told the fleet is full.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 256;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    ///
    /// The worker's own budget, read off the shared accounting rather than restated:
    /// this is the figure `Answer` charges the declared footprint against, and a
    /// ceiling advertised to the endpoint that differed from the one actually spent
    /// would refuse at one number and account at another.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return _capacity.ByteBudget();
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **Yes, and this surface is why the column exists.** `Answer` charges
    /// `DeclaredRequestFootprint` against `CompileCapacity` -- the same accounting
    /// the dedicated port uses and the same figure the worker advertises -- and it
    /// holds that for the compile. The endpoint holding its own reservation over the
    /// same frame counted one buffer twice for minutes, in a pool shared with the
    /// scheduler verbs on this listener (#448).
    ///
    /// Verb-blind on purpose. `MergedResponder` routes by verb FAMILY, so every verb
    /// reaching here is a compile verb, and answering per-verb would be a second
    /// place for the family table to be restated and disagree with itself.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return true;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Yes, and this is the one surface for which the question is worth asking.** A
    /// compile runs for seconds to minutes while nothing here reads the socket, so a
    /// client that is killed mid-build -- `Ctrl-C`, a cancelled CI job, a reclaimed
    /// runner -- goes unnoticed until the object is written into a socket nobody is
    /// reading. #223 measured that at 84 MB for a single translation unit.
    ///
    /// The compile itself is unaffected and still counts in `WorkerJobsCompleted`: it
    /// ran and this machine paid for it, and only the handover found nobody there.
    /// Stopping the compile as well needs a cancellable process seam, which
    /// `IProcessRunner` does not have and
    /// [#661](https://github.com/LASTRADA-Software/fastcached/issues/661) is about.
    ///
    /// Verb-blind, exactly as `HoldsOwnByteBudget` above: `MergedResponder` routes by
    /// verb FAMILY, so every verb arriving here is a compile verb, and answering
    /// per-verb would restate the family table somewhere it can disagree with itself.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return IMetricsSink::Counter::WorkerJobsAbandonedClientGone;
    }

  private:
    Cc::WorkerProtocol& _protocol;
    CompileCapacity& _capacity;
    Distributed::IMembershipOracle const& _membership;
    IExecutor& _jobs;
    IExecutor& _home;
    IMetricsSink& _metrics;
    ILogger& _logger;
};

} // namespace FastCache::Node
