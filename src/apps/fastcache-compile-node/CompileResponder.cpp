// SPDX-License-Identifier: Apache-2.0
#include "CompileResponder.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// A held slot, released when it goes out of scope.
    ///
    /// RAII for the reason `CompileCapacity::Bytes` is: the paths out of a compile are
    /// many -- a refusal, a decode failure, a protocol that threw, the ordinary reply --
    /// and a release written at each of them is the one that will be missed at the next
    /// one added. A slot leaked once is a worker that reports itself permanently busier
    /// than it is, and the scheduler takes it out of rotation silently.
    ///
    /// The destructor runs at the coroutine's `co_return`, which is **after** the hop
    /// home, so the release happens on the same thread everything else about the reply
    /// does.
    class SlotHeld
    {
      public:
        /// @param capacity What to give the slot back to.
        explicit SlotHeld(CompileCapacity& capacity) noexcept:
            _capacity { &capacity }
        {
        }

        SlotHeld(SlotHeld const&) = delete;
        SlotHeld& operator=(SlotHeld const&) = delete;
        SlotHeld(SlotHeld&&) = delete;
        SlotHeld& operator=(SlotHeld&&) = delete;

        ~SlotHeld()
        {
            _capacity->ReleaseSlot();
        }

      private:
        CompileCapacity* _capacity;
    };
} // namespace

std::optional<std::vector<std::byte>> CompileResponder::RefusePeer(std::string_view peer, std::uint8_t /*opRaw*/) const
{
    // The verb is ignored: `MergedResponder` only ever routes the `Compile` family
    // here, so every question this surface is asked is about a compile. It is taken
    // anyway because the seam carries it -- and because a second compile-family verb
    // would arrive here without a signature change, which is the point of the column.
    return RefuseUnlessMember(_membership, _metrics, peer);
}

std::vector<std::byte> CompileResponder::RefusalReply(Wire::PrePayloadDecision decision, std::uint8_t /*opRaw*/) const
{
    // Only the oversize arm carries a counter on this surface. The other two cannot be
    // reached: `UnknownOpcode` is answered by the router before a verb gets here, and
    // `Unauthenticated` needs `AuthRequired` to have said yes, which this surface never
    // does. They are still answered, because `DecidePrePayload` is total over every
    // byte value and a surface that could not encode one of its outcomes would be a
    // hole rather than a simplification.
    if (decision == Wire::PrePayloadDecision::PayloadTooLarge)
        return Cc::Refuse(_metrics, CompileRefusal::PayloadTooLarge);

    // No detail. What a message could add is which verb the caller failed to reach, and
    // a caller that got this far learns nothing useful from being told.
    return Wire::EncodeErrorReply(Wire::ErrorCodeFor(decision), {});
}

Task<std::vector<std::byte>> CompileResponder::Answer(std::span<std::byte const> frame, std::string peer)
{
    // The gate is re-asked here rather than taken on the endpoint's word. `Answer` is
    // reachable directly -- which is why `CacheResponder` re-asks its own -- and a
    // predicate enforced only at the door is one a later caller walks around.
    //
    // The verb is read back out of the frame this call was handed: a frame too short to
    // carry a header cannot name one, and `0xFF` is unassigned, so the question is about
    // a verb no policy admits rather than about a verb this guessed.
    auto const header = Wire::DecodeRequestHeader(frame);
    auto const opRaw = header.has_value() ? header->opRaw : std::uint8_t { 0xFF };
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);

    // A worker that has begun stopping admits nothing more, and says the fleet is full
    // rather than saying nothing: `CompileCapacity::TryTakeSlot` does not itself refuse
    // during a drain -- it counts, and the door is whoever owns the door. The accept
    // loop's door is its listener; this one's is here. Without it a compile admitted
    // after `~WorkerServer` began waiting would be a job the drain has already stopped
    // counting on, started against members it is about to free.
    if (_capacity.IsShuttingDown())
        co_return Cc::Refuse(_metrics, CompileRefusal::NoCapacity, "this worker is stopping");

    // The cap is enforced here as well as advertised, and a job over it is REFUSED
    // rather than queued: refusing costs the client one local compile, while queueing
    // hides the overload from the scheduler that is trying to route around it.
    if (!_capacity.TryTakeSlot())
        co_return Cc::Refuse(_metrics, CompileRefusal::NoCapacity);
    SlotHeld const slot { _capacity };

    // Counted at the socket, which is what "bytes received" means to an operator sizing
    // a link: the payload as it arrived, not what it decompressed to.
    _metrics.Increment(IMetricsSink::Counter::WorkerBytesReceived, static_cast<std::uint64_t>(frame.size()));

    // What this request COSTS, not what its frame is long. A codec envelope one layer in
    // declares what it expands to, and `Unenvelope` allocates that -- so a few dozen
    // bytes declaring a 256 MiB expansion would pass the endpoint's own frame-length
    // budget having reserved almost nothing (#241).
    //
    // A price above the whole budget is deliberately NOT charged: it is the per-request
    // ceiling, which the decoder answers by name and without allocating a byte, and
    // charged here it would come back `EndpointBusy` on a completely idle worker and
    // send the client off to retry a frame that can never fit. That hand-off is sound
    // only while the decoder's ceiling is no larger than this budget, which is why
    // `WorkerMaxRequestBytes` is one exported constant rather than a literal per side.
    //
    // Initialized in ONE declaration rather than assigned into: a reservation is
    // move-constructible and deliberately not move-assignable, because a release is
    // owed exactly once and an assignment would be a second object's claim landing on
    // the first one's storage.
    auto const footprint = Cc::DeclaredRequestFootprint(frame);
    auto const chargeable = footprint <= _capacity.ByteBudget();
    auto const reserved =
        chargeable ? _capacity.TryTakeBytes(footprint) : std::optional<CompileCapacity::Bytes> { std::nullopt };
    if (chargeable && !reserved.has_value())
        // Its own code, not `NoCapacity`: this says "come back shortly", while
        // `NoCapacity` says "the fleet is full". An operator sent to buy machines over
        // a transient byte budget is being sent to fix something that was never wrong.
        co_return Cc::Refuse(_metrics, CompileRefusal::EndpointBusy);

    // --- Off the reactor. ---
    //
    // A compile spawns a process and blocks for seconds. Left on the reactor it would
    // stall every other connection that reactor owns, and this worker would advertise
    // its slot cap while running one job at a time -- #213, which was measured on the
    // accept loop and is exactly as reachable here.
    co_await ResumeOn { _jobs };

    std::optional<std::vector<std::byte>> reply;
    bool threw = false;
    try
    {
        reply = _protocol.Answer(frame);
    }
    catch (...)
    {
        // Caught HERE rather than left to the endpoint's own firewall, and that is not
        // belt-and-braces: an exception thrown on the pool thread propagates through
        // this task's promise and is rethrown where the awaiter resumes, so the
        // endpoint's `catch` -- and the socket close, and the untrack -- would run on
        // the POOL thread. The hop below is what keeps that on the reactor, and an
        // exception that skipped it would skip the hop too.
        threw = true;
    }

    // --- Back onto the reactor, before anything is returned. ---
    //
    // `FrameEndpoint` writes what this returns, to a socket that belongs to the reactor.
    // Returning from the pool thread would have the connection task -- its writes, its
    // deadline rearm, its close -- running off the loop that owns them, and nothing in
    // the type system or in a functional test reports it: the object is correct and the
    // write usually succeeds. `CompileResponder_test.cpp` asserts the thread identity
    // instead, because that is the only thing that can see this.
    co_await ResumeOn { _home };

    if (threw)
    {
        // Costs this client rather than the worker. Empty is CLOSE, which is the honest
        // answer: nothing was produced, and there is no reply that would describe what
        // happened in a way the client could act on differently from a dropped link.
        _logger.Logf(LogLevel::Error, "worker: dropping a compile that threw while being served on the node port");
        co_return std::vector<std::byte> {};
    }

    if (!reply.has_value())
        // No reply means a foreign magic: the peer is not speaking this protocol, and
        // there is no framing in which an answer would be meaningful. `IFrameResponder`
        // spells that as an empty vector, which is the same close the accept loop does.
        co_return std::vector<std::byte> {};

    _metrics.Increment(IMetricsSink::Counter::WorkerBytesReturned, static_cast<std::uint64_t>(reply->size()));
    co_return *std::move(reply);
}

} // namespace FastCache::Node
