// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/AtomicMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <StubObjectTestSupport.hpp>
#include <WorkerProtocol.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A runner that writes a canned object and remembers which thread it ran on.
///
/// The thread id is the whole subject of this file. A compile that ran on the
/// reactor produces exactly the same object as one that ran on the pool, so nothing
/// about the reply can tell the two apart -- which is why the runner, the one thing
/// that is inside the blocking call, is what records it.
class ThreadRecordingRunner final: public Cc::IProcessRunner
{
  public:
    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        _ranOn = std::this_thread::get_id();
        _runs.fetch_add(1, std::memory_order_acq_rel);
        Cc::Test::WriteStubObject(argv);
        return Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

    /// @return The thread the last compile ran on, or a default id if none did.
    [[nodiscard]] std::thread::id RanOn() const noexcept
    {
        return _ranOn;
    }

    /// @return How many compiles have been spawned.
    [[nodiscard]] std::size_t Runs() const noexcept
    {
        return _runs.load(std::memory_order_acquire);
    }

  private:
    // Written on the pool thread and read on the test thread after a future has been
    // waited on, which is the synchronisation. `std::thread::id` is not atomic and
    // does not need to be here; the handshake is the `std::promise`.
    std::thread::id _ranOn;
    std::atomic<std::size_t> _runs { 0 };
};

/// Everything a compile responder needs, and nothing that decides a thread.
///
/// The two executors are deliberately NOT members: which one is the reactor and which
/// is the pool is what every case here is about, so each case names them itself.
struct Fixture
{
    ThreadRecordingRunner runner;
    ScratchDirectory scratch { "fc-compile-responder" };
    Cc::CompileJobRunner jobs;
    AtomicMetricsSink metrics;
    Cc::WorkerProtocol protocol;
    NullLogger logger;

    /// Admits everybody. The anti-leeching rule has its own case, which substitutes a
    /// listed oracle for exactly that reason.
    Distributed::OpenMembership membership;

    Fixture():
        jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, Cc::ToolchainSurvey::Completed() },
        protocol { jobs, Cc::UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics }
    {
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;
};

/// A framed COMPILE naming @p fingerprint.
/// @param fingerprint The toolchain to claim.
/// @return The request frame.
[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13")
{
    constexpr std::string_view Source = "int main(){return 0;}";
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Source.size()), Wire::AsBytes(Source));
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = enveloped,
                                                      .acceptedCodecs = { Wire::IdentityCodec },
                                                      .sourceName = "a.cpp" });
}

/// The status a reply frame carries.
[[nodiscard]] Wire::Status StatusOf(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    REQUIRE(header.has_value());
    return Unwrap(header).status;
}

/// The error code a refusal frame carries.
[[nodiscard]] Wire::ErrorCode ErrorOf(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    if (!header.has_value())
        return Wire::ErrorCode::MalformedFrame;
    auto const body = std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize);
    auto const decoded = Wire::DecodeErrorPayload(body);
    return decoded.has_value() ? decoded->first : Wire::ErrorCode::MalformedFrame;
}

/// The id of the one thread @p pool runs work on.
///
/// Discovered by hopping through the seam rather than asked of the pool, because that
/// is what a coroutine reaching it will actually observe -- and because `IExecutor`
/// deliberately says nothing about threads, which is why a compile could ever have
/// ended up on the wrong one.
/// @param pool A single-threaded executor.
/// @return The thread it resumes handles on.
[[nodiscard]] std::thread::id ThreadOf(IExecutor& pool)
{
    std::promise<std::thread::id> where;
    auto future = where.get_future();
    // The promise travels BY VALUE into the frame. A coroutine's reference parameters
    // are not kept alive by its frame, and a caller that returns the moment the value
    // is set is exactly the shape where that bites.
    [](IExecutor& target, std::promise<std::thread::id> out) -> DetachedTask {
        co_await ResumeOn { target };
        out.set_value(std::this_thread::get_id());
        co_return;
    }(pool, std::move(where));
    return future.get();
}

/// What one `Answer` did, and where.
struct Answered
{
    std::thread::id startedOn;  ///< The thread the request was handed over on.
    std::thread::id returnedOn; ///< The thread the reply came back on.
    std::vector<std::byte> reply;
};

/// Ask @p responder for one answer, starting on @p reactor, and record the threads.
///
/// This is the shape `FrameEndpoint::ServeConnection` has: a connection task running
/// on the reactor awaits `Answer` and then writes what it returns to a socket that
/// belongs to that reactor. So the driver starts on the reactor, and both thread
/// readings are taken exactly where the endpoint would take them.
/// @param responder Who to ask.
/// @param reactor The loop standing in for the node's own.
/// @param frame The request.
/// @param peer The caller's host.
/// @return What was answered, and on which threads.
[[nodiscard]] Answered AnswerFrom(CompileResponder& responder,
                                  IExecutor& reactor,
                                  std::vector<std::byte> frame,
                                  std::string peer = "127.0.0.1")
{
    std::promise<Answered> done;
    auto future = done.get_future();
    [](CompileResponder& target,
       IExecutor& loop,
       std::vector<std::byte> request,
       std::string caller,
       std::promise<Answered> out) -> DetachedTask {
        co_await ResumeOn { loop };
        auto const startedOn = std::this_thread::get_id();

        // `request` is a local of THIS frame, which stays alive across the suspension
        // inside `Answer` -- the contract `IFrameResponder::Answer` states for the span
        // it borrows, and the same way the endpoint's own connection task holds it.
        auto reply = co_await target.Answer(request, std::move(caller));

        out.set_value(Answered { .startedOn = startedOn,
                                 .returnedOn = std::this_thread::get_id(),
                                 .reply = std::move(reply) });
        co_return;
    }(responder, reactor, std::move(frame), std::move(peer), std::move(done));
    return future.get();
}

} // namespace

TEST_CASE("A compile leaves the reactor and the reply comes back to it", "[node][compile-responder]")
{
    // **The case this whole class exists for, and neither half is visible in a reply.**
    //
    // The worker's own port is a BLOCKING listener, so one hop suffices there: nothing
    // after `ResumeOn { _jobs }` suspends. The merged 0xFC surface is a reactor, so a
    // compile arrives on the reactor thread and there are two ways to get it wrong:
    //
    //   1. Never leaving -- a compiler spawned on the reactor stalls every other
    //      connection that reactor owns, and the worker advertises `slots` while
    //      running one at a time. That is #213, one layer over.
    //   2. Never coming back -- `FrameEndpoint` writes what this returns to a reactor
    //      socket, so a reply returned from the pool thread has the connection task's
    //      writes, its deadline rearm and its close running off the loop that owns
    //      them.
    //
    // Both produce a correct object. A test that checked only the reply would pass
    // under either, which is why these are assertions about thread identity.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/2, /*byteBudget=*/64ULL * 1024ULL * 1024ULL, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    auto const reactorThread = ThreadOf(reactor);
    auto const jobsThread = ThreadOf(jobs);
    REQUIRE(reactorThread != jobsThread);

    auto const answered = AnswerFrom(responder, reactor, CompileFrame());

    // The request really did arrive on the reactor, so the two readings below are
    // about a hop and not about a driver that started somewhere else.
    REQUIRE(answered.startedOn == reactorThread);

    // The compiler ran, and NOT on the reactor.
    REQUIRE(fix.runner.Runs() == 1);
    CHECK(fix.runner.RanOn() != reactorThread);
    CHECK(fix.runner.RanOn() == jobsThread);

    // And the answer came home. This is the invisible one: nothing in the type system
    // reports a reactor socket written from a pool thread.
    CHECK(answered.returnedOn == reactorThread);

    CHECK(StatusOf(answered.reply) == Wire::Status::Ok);

    // Nothing is left held. A slot leaked once is a worker that reports itself
    // permanently busier than it is, and the scheduler takes it out of rotation
    // silently.
    CHECK(capacity.InFlight() == 0);
}

TEST_CASE("A refusal is answered on the reactor without reaching the pool", "[node][compile-responder]")
{
    // The refusals are decided before the hop, deliberately: a caller this surface will
    // not serve must not be able to occupy a compile thread on its way to being told so.
    // Which means they are also the one path where the reply is produced on the reactor
    // by never having left it -- so this case pins the thread as well, or a future
    // rearrangement could move a refusal onto the pool and nothing would notice.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/0, /*byteBudget=*/1024ULL, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    auto const reactorThread = ThreadOf(reactor);
    auto const answered = AnswerFrom(responder, reactor, CompileFrame());

    // Zero slots is the degenerate case of a full worker. Refused, never queued:
    // queueing hides the overload from the scheduler that is trying to route around it.
    CHECK(ErrorOf(answered.reply) == Wire::ErrorCode::NoCapacity);
    CHECK(answered.returnedOn == reactorThread);
    CHECK(fix.runner.Runs() == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 1);
}

TEST_CASE("The merged surface applies the worker's own membership rule", "[node][compile-responder]")
{
    // Not a new policy and not a relaxed one: the same `RefuseUnlessMember` the
    // dedicated accept loop has always used, so the additional door cannot admit a
    // caller the original refuses. Without this the merged port would run a stranger's
    // compiler for them -- and it binds the wildcard on any node that schedules.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/2, /*byteBudget=*/1024ULL * 1024ULL, std::chrono::seconds { 5 }, fix.logger };
    Distributed::ClusterMembership const listed { { "10.0.0.1:6676" } };
    CompileResponder responder { fix.protocol, capacity, listed, jobs, reactor, fix.metrics, fix.logger };

    auto const stranger = AnswerFrom(responder, reactor, CompileFrame(), "10.9.9.9");
    CHECK(ErrorOf(stranger.reply) == Wire::ErrorCode::NotAMember);
    CHECK(fix.runner.Runs() == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNotAMember) == 1);

    // Refused at the DOOR as well as in `Answer`, on the peer alone, so the endpoint
    // can ask it before it reads a payload byte (#285, #377).
    auto const early = responder.RefusePeer("10.9.9.9", static_cast<std::uint8_t>(Wire::Op::Compile));
    CHECK(early.has_value());

    // And a member is served.
    auto const member = AnswerFrom(responder, reactor, CompileFrame(), "10.0.0.1");
    CHECK(StatusOf(member.reply) == Wire::Status::Ok);
    CHECK(fix.runner.Runs() == 1);
}

TEST_CASE("A stopping worker admits no more compiles", "[node][compile-responder]")
{
    // `CompileCapacity::BeginShutdown` closes the accept loop's door by making its
    // condition false; this surface has no loop, so it has to ask. Without it a compile
    // admitted after `~WorkerServer` began waiting would be a job the drain had already
    // stopped counting on -- started against members it is about to free.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/4, /*byteBudget=*/1024ULL * 1024ULL, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    capacity.BeginShutdown();
    auto const answered = AnswerFrom(responder, reactor, CompileFrame());

    CHECK(ErrorOf(answered.reply) == Wire::ErrorCode::NoCapacity);
    CHECK(fix.runner.Runs() == 0);
    CHECK(capacity.InFlight() == 0);
}

TEST_CASE("A compile declaring more than the budget is refused, not charged", "[node][compile-responder]")
{
    // #241 on this surface too. The frame is about a hundred bytes while the codec
    // envelope one layer in tells the decoder to size a buffer of `rawLength`, so the
    // frame length is not what the request costs -- and the responder charges the
    // FOOTPRINT. A budget below it and above the frame is what separates the two.
    //
    // The codec is one no build has, deliberately: what is under test is the price
    // charged for what the envelope DECLARES, and a codec this build could decode would
    // make an unfixed responder genuinely attempt the allocation.
    constexpr std::uint8_t NoSuchCodec = 0xFE;
    constexpr std::string_view Compressed = "a few dozen bytes, and no more";
    constexpr std::uint32_t Declared = 8ULL * 1024ULL * 1024ULL;

    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/2, /*byteBudget=*/Declared, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    auto const enveloped = Wire::EncodeCodecEnvelope(NoSuchCodec, Declared, Wire::AsBytes(Compressed));
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = enveloped,
                                                                  .acceptedCodecs = { Wire::IdentityCodec },
                                                                  .sourceName = "a.cpp" });

    // One of these fits the budget exactly and the second cannot, so the second is told
    // to come back rather than being told the fleet is full: an operator sent to buy
    // machines over a transient byte budget is being sent to fix something that was
    // never wrong. The first is held by taking its reservation directly, which is what
    // a compile in flight looks like to the budget.
    auto const held = capacity.TryTakeBytes(Declared);
    REQUIRE(held.has_value());

    auto const answered = AnswerFrom(responder, reactor, frame);
    CHECK(ErrorOf(answered.reply) == Wire::ErrorCode::EndpointBusy);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy) == 1);
    CHECK(fix.runner.Runs() == 0);

    // The slot it took to get that far is given back.
    CHECK(capacity.InFlight() == 0);
}

TEST_CASE("The compile surface requires no connection credential", "[node][compile-responder]")
{
    // `Op::Compile` is a `RequiresAuth` row, so this answer is what decides whether the
    // merged listener demands a credential before it. It must not: a compile already
    // carries one, per job rather than per connection -- the lease token the scheduler
    // signed for this worker's endpoint, checked inside `WorkerProtocol`. Answering
    // `true` here would refuse every client the dedicated port serves today.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder const responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    CHECK_FALSE(responder.AuthRequired(static_cast<std::uint8_t>(Wire::Op::Compile)));

    // Which is the whole reason the question takes the verb: the scheduler verbs on
    // this same listener still require what #289 added, and a surface-wide answer has
    // no correct value.
    CHECK(Wire::DecidePrePayload({ .opRaw = static_cast<std::uint8_t>(Wire::Op::Compile),
                                   .declaredLength = 1024,
                                   .sessionCap = responder.MaxRequestBytes(),
                                   .authRequired = responder.AuthRequired(static_cast<std::uint8_t>(Wire::Op::Compile)),
                                   .credentialAccepted = false })
          == Wire::PrePayloadDecision::Serve);

    // The ceilings it advertises are the worker's own, not a second set: the request cap
    // is the constant `WorkerProtocol` was built with, and the in-flight budget is the
    // one `Answer` actually charges against.
    CHECK(responder.MaxRequestBytes() == WorkerMaxRequestBytes);
    CHECK(responder.MaxInFlightBytes() == capacity.ByteBudget());
}

TEST_CASE("The merged router sends a compile to the compile responder", "[node][compile-responder]")
{
    // The routing half, which is the small half. `MergedResponder` used to answer
    // `UnimplementedVerb` for the whole `Compile` family because there was no component
    // to route to; now there is, and the cache and scheduler answers are unchanged.
    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/2, /*byteBudget=*/1024ULL * 1024ULL, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder compile { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };
    MergedResponder merged { nullptr, nullptr, &compile };

    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::Compile)) == &compile);

    // A node whose only component is its worker still refuses the verbs it has nobody
    // for, which is the property that did not change.
    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::Fetch)) == nullptr);
    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::Lease)) == nullptr);

    // And the ceilings fold over the compile responder as over any other owner.
    CHECK(merged.MaxRequestBytes() == WorkerMaxRequestBytes);
    CHECK(merged.MaxInFlightBytes() == capacity.ByteBudget());
}
