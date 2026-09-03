// SPDX-License-Identifier: Apache-2.0
#include "CompileResponder.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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

    /// Which counter each pre-payload refusal moves on this surface.
    ///
    /// A `switch` rather than an `EnumTable`, and the reason is the enum rather than a
    /// preference: `PrePayloadDecision` states no `Last`. It is a wire enum that both
    /// binaries compile in, `ErrorCodeFor` answers it with a switch for the same
    /// reason, and adding a count to a shared header to satisfy a local idiom would be
    /// a wire change bought for nothing. The compiler still enumerates it -- no
    /// `default`, so a fifth outcome is a build failure here exactly as it is there.
    ///
    /// Two of the four cannot happen. `UnknownOpcode` is refused by the router before
    /// a verb reaches this responder, and `Unauthenticated` needs `AuthRequired` to
    /// have said yes, which this surface never does. They get counters anyway, because
    /// a refusal answered while nothing rises is indistinguishable on `/metrics` from
    /// a port nobody is talking to -- and if either ever fires, what changed is who may
    /// compile here.
    ///
    /// `Serve` is not a refusal and the interface says so, but a total function has to
    /// answer it. It follows `ErrorCodeFor`'s own choice rather than inventing a
    /// second one.
    /// @param decision What `DecidePrePayload` returned.
    /// @return The row pairing the wire code with its counter.
    [[nodiscard]] constexpr Cc::SurfaceRefusal RefusalFor(Wire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case Wire::PrePayloadDecision::PayloadTooLarge:
                return CompileRefusal::PayloadTooLarge;
            case Wire::PrePayloadDecision::UnknownOpcode:
                return CompileRefusal::UnknownOpcode;
            case Wire::PrePayloadDecision::Unauthenticated:
            case Wire::PrePayloadDecision::Serve:
                break;
        }
        return CompileRefusal::Unauthenticated;
    }

    /// One row per `EndpointRefusal`, in enumerator order.
    struct EndpointRefusalRow
    {
        EndpointRefusal refusal; ///< Which endpoint decision this describes.

        /// What the client is told and what the operator sees rise, or nothing where
        /// this surface deliberately counts none.
        std::optional<Cc::SurfaceRefusal> answer;

        /// Why nothing is counted, for a row whose `answer` is `nullopt`; empty
        /// otherwise. On the row rather than at the call site, for the reason
        /// `Detail::SchedulerEndpointRefusal` states: a reason written once beside the
        /// lookup is correct only while exactly one row needs one.
        std::string_view rationale;
    };

    /// Which refusal each endpoint-decided outcome answers with on this surface.
    ///
    /// A table, unlike `RefusalFor` above, and the difference is the enum rather than
    /// a preference: `EndpointRefusal` is this tree's own and states its own `Last`,
    /// so `RowsInEnumeratorOrder` can check the extent AND every row's position.
    /// `PrePayloadDecision` is a wire enum shared with the launcher and states no
    /// count, which is why its lookup has to stay a switch.
    constexpr EnumTable<EndpointRefusal, EndpointRefusalRow> EndpointRefusalTable { {
        // `.rationale` is spelled out as empty on every counted row rather than left to
        // default. Clang and GCC reject the omission under this project's pedantic
        // flags (`-Wmissing-designated-field-initializers`) and MSVC does not say a
        // word, so the three rows below built clean on Windows and failed four CI legs.
        { .refusal = EndpointRefusal::InFlightBudget, .answer = CompileRefusal::EndpointBusy, .rationale = {} },
        { .refusal = EndpointRefusal::CredentialMalformed, .answer = CompileRefusal::MalformedCredential, .rationale = {} },
        { .refusal = EndpointRefusal::CredentialRejected, .answer = CompileRefusal::RejectedCredential, .rationale = {} },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .answer = std::nullopt,
          .rationale = AnswerDeadlineIsTheEndpointsRationale },
    } };

    static_assert(RowsInEnumeratorOrder(EndpointRefusalTable, &EndpointRefusalRow::refusal),
                  "EndpointRefusalTable must hold one row per EndpointRefusal, in enumerator order");

    // Every row states exactly one of the two things a refusal can assert, for the
    // reason the cache and scheduler tables do: a row asserting neither passes a guard
    // that short-circuits on the absent counter, and ships the new refusal uncounted
    // and unexplained.
    static_assert(std::ranges::all_of(EndpointRefusalTable,
                                      [](EndpointRefusalRow const& row) {
                                          return StatesOneRefusalClaim(row.answer.has_value(), row.rationale);
                                      }),
                  "every endpoint refusal row must state either a counted answer or a rationale, not both");

    // The rows above are CONVERTED from `CompileRefusal`, which already pairs a code
    // with a counter -- the rulebook's instruction rather than restating the pair. What
    // that conversion cannot check is that the row picked answers what this refusal is
    // supposed to answer, so it is checked here against the one place that property
    // lives.
    static_assert(std::ranges::all_of(EndpointRefusalTable,
                                      [](EndpointRefusalRow const& row) {
                                          return !row.answer.has_value() || row.answer->code == ErrorCodeFor(row.refusal);
                                      }),
                  "a converted row must answer the code `ErrorCodeFor` names for its refusal");
} // namespace

std::optional<std::vector<std::byte>> CompileResponder::RefusePeer(std::string_view peer, std::uint8_t /*opRaw*/) const
{
    // The verb is ignored: `MergedResponder` only ever routes the `Compile` family
    // here, so every question this surface is asked is about a compile. It is taken
    // anyway because the seam carries it -- and because a second compile-family verb
    // would arrive here without a signature change, which is the point of the column.
    return RefuseUnlessMember(_membership, _metrics, peer);
}

std::vector<std::byte> CompileResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view detail) const
{
    // The caller's wording, and it is empty for every decision but the frame ceiling.
    // That one names both numbers -- a 256 MiB cap is not guessable from "too large" --
    // and it arrives from the endpoint because the endpoint is what enforced it.
    return Cc::Refuse(_metrics, RefusalFor(decision), detail);
}

std::vector<std::byte> CompileResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view detail) const
{
    auto const& row = EndpointRefusalTable[static_cast<std::size_t>(refusal)];
    return AnswerEndpointRefusal(_metrics, ErrorCodeFor(refusal), row.answer, row.rationale, detail);
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
        co_return Cc::Refuse(_metrics, CompileRefusal::Stopping, "this worker is stopping");

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
    // A price above the whole budget is deliberately not charged AS A FOOTPRINT: it is
    // the per-request ceiling, which the decoder answers by name and without allocating
    // a byte, and charged here it would come back `EndpointBusy` on a completely idle
    // worker and send the client off to retry a frame that can never fit. That hand-off
    // is sound only while the decoder's ceiling is no larger than this budget, which is
    // why `WorkerMaxRequestBytes` is one exported constant rather than a literal per
    // side.
    //
    // It is not charged NOTHING either, which is the distinction `ChargeFor` carries:
    // the frame has arrived and is held, so it costs its own length whatever the
    // decoder is about to decide. Charging zero here while the accept loop kept the
    // frame length it had already reserved made one worker account two ways depending
    // on which door was used (#448).
    //
    // Initialized in ONE declaration rather than assigned into: a reservation is
    // move-constructible and deliberately not move-assignable, because a release is
    // owed exactly once and an assignment would be a second object's claim landing on
    // the first one's storage.
    auto const footprint = Cc::DeclaredRequestFootprint(frame);
    auto const reserved = _capacity.TryTakeBytes(
        _capacity.ChargeFor(footprint, header.has_value() ? std::size_t { header->payloadLength } : frame.size()));
    if (!reserved.has_value())
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
