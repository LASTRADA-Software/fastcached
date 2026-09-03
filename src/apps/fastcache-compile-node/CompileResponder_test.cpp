// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "FrameEndpoint.hpp"
#include "NodeConfig.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"
#include "Responders.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
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
    // The promise travels BY VALUE into the frame, and the executor as a POINTER. A
    // coroutine's parameters are copied into its frame but a REFERENCE parameter is not
    // -- the frame keeps the reference, not the referent -- so a caller that returns the
    // moment the value is set is exactly the shape where that bites. A pointer says the
    // lifetime is the caller's, and `cppcoreguidelines-avoid-reference-coroutine-parameters`
    // refuses the other spelling rather than leaving it to a comment.
    [](IExecutor* target, std::promise<std::thread::id> out) -> DetachedTask {
        co_await ResumeOn { *target };
        out.set_value(std::this_thread::get_id());
        co_return;
    }(&pool, std::move(where));
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
    // Pointers, for the reason `ThreadOf` gives: a coroutine frame does not keep a
    // reference parameter's referent alive, and both of these outlive the call by being
    // the caller's locals rather than by anything this frame does.
    [](CompileResponder* target,
       IExecutor* loop,
       std::vector<std::byte> request,
       std::string caller,
       std::promise<Answered> out) -> DetachedTask {
        co_await ResumeOn { *loop };
        auto const startedOn = std::this_thread::get_id();

        // `request` is a local of THIS frame, which stays alive across the suspension
        // inside `Answer` -- the contract `IFrameResponder::Answer` states for the span
        // it borrows, and the same way the endpoint's own connection task holds it.
        auto reply = co_await target->Answer(request, std::move(caller));

        out.set_value(
            Answered { .startedOn = startedOn, .returnedOn = std::this_thread::get_id(), .reply = std::move(reply) });
        co_return;
    }(&responder, &reactor, std::move(frame), std::move(peer), std::move(done));
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
    CompileCapacity capacity {
        /*slots=*/2, /*byteBudget=*/64ULL * 1024ULL * 1024ULL, std::chrono::seconds { 5 }, fix.logger
    };
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
    // admitted after `~CompileCapacity` began waiting would be a job the drain had already
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

    // A FRACTION of the budget is held, not all of it, and the difference is whether
    // this case tests anything. Holding the whole budget makes every non-zero charge
    // fail, so a responder charging the hundred-byte FRAME LENGTH -- which is the bug
    // -- would be refused too and the case would pass while proving nothing. With
    // 1 MiB of 8 held, the frame length fits comfortably and only the declared
    // 8 MiB expansion does not, so the two charges give opposite answers.
    constexpr std::size_t Budget = 8ULL * 1024ULL * 1024ULL;
    constexpr std::size_t Held = 1ULL * 1024ULL * 1024ULL;

    Fixture fix;
    ThreadPoolExecutor reactor { 1 };
    ThreadPoolExecutor jobs { 1 };
    CompileCapacity capacity { /*slots=*/2, Budget, std::chrono::seconds { 5 }, fix.logger };
    CompileResponder responder { fix.protocol, capacity, fix.membership, jobs, reactor, fix.metrics, fix.logger };

    auto const enveloped = Wire::EncodeCodecEnvelope(NoSuchCodec, Declared, Wire::AsBytes(Compressed));
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = enveloped,
                                                                  .acceptedCodecs = { Wire::IdentityCodec },
                                                                  .sourceName = "a.cpp" });

    // A compile already in flight, spelled as the reservation one would be holding.
    auto const held = capacity.TryTakeBytes(Held);
    REQUIRE(held.has_value());

    // The two charges disagree, which is what makes the assertion below meaningful:
    // the frame is small enough to fit in what is left, and what it DECLARES is not.
    REQUIRE(frame.size() < Budget - Held);
    REQUIRE(Cc::DeclaredRequestFootprint(frame) > Budget - Held);

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

// ---------------------------------------------------------------------------
// Against the REAL endpoint.
//
// Everything above substitutes two `ThreadPoolExecutor`s for a reactor, which is what
// makes the thread assertions expressible -- and that substitution cannot see two
// defects that live in `FrameEndpoint` itself: its request deadline, which closes a
// socket out from under an answer, and its stop rule, which can stop the reactor while
// an answer is still out on the pool. Both were found in review after the in-process
// cases were green, so the cases below exist precisely because those are not enough.
// ---------------------------------------------------------------------------

namespace
{

/// A runner that blocks until released, so a case can look at a compile mid-flight.
///
/// Bounded, because an unbounded wait turns a regression into a suite that HANGS
/// rather than one that fails -- and a hang reports a defect as a timeout naming
/// nothing.
class HoldingRunner final: public Cc::IProcessRunner
{
  public:
    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            ++_started;
        }
        _changed.notify_all();
        {
            auto guard = std::unique_lock { _mutex };
            (void) _changed.wait_for(guard, std::chrono::seconds { 10 }, [this] { return _released; });
        }
        Cc::Test::WriteStubObject(argv);
        return Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

    /// @param many How many compiles to wait for.
    /// @return Whether that many started before the bound elapsed.
    [[nodiscard]] bool WaitForStarted(std::size_t many)
    {
        auto guard = std::unique_lock { _mutex };
        return _changed.wait_for(guard, std::chrono::seconds { 10 }, [this, many] { return _started >= many; });
    }

    void Release()
    {
        {
            auto const guard = std::scoped_lock { _mutex };
            _released = true;
        }
        _changed.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _started { 0 };
    bool _released { false };
};

/// Another responder's answers, behind a deadline this one chooses.
///
/// A decorator rather than a second fake, so what the endpoint drives is the REAL
/// `CompileResponder` -- its admission, its hops, its accounting -- with only the
/// number under test replaced. A hand-written stand-in would prove the endpoint
/// honours some responder's window and say nothing about this one's.
class ShortWindowResponder final: public IFrameResponder
{
  public:
    /// @param inner What actually answers; must outlive this.
    /// @param window The deadline to report instead of the inner one's.
    ShortWindowResponder(IFrameResponder& inner, std::chrono::milliseconds window) noexcept:
        _inner { inner },
        _window { window }
    {
    }

    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override
    {
        co_return co_await _inner.Answer(frame, std::move(peer));
    }
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override
    {
        return _inner.RefusePeer(peer, opRaw);
    }
    [[nodiscard]] bool AuthRequired(std::uint8_t opRaw) const noexcept override
    {
        return _inner.AuthRequired(opRaw);
    }
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return _inner.CheckCredential(payload);
    }
    [[nodiscard]] std::vector<std::byte> RefusalReply(Wire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override
    {
        return _inner.RefusalReply(decision, opRaw, detail);
    }
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override
    {
        return _inner.EndpointRefusalReply(refusal, opRaw, detail);
    }
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _window;
    }
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return _inner.MaxRequestBytes();
    }
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return _inner.MaxOpenConnections();
    }
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return _inner.MaxInFlightBytes();
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// Forwarded, like every other question this decorator does not itself answer.
    /// It overrides the WINDOW and nothing else, and a decorator that answered this
    /// on its own behalf would detach the endpoint's accounting from the surface
    /// actually doing the charging (#448).
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t opRaw) const noexcept override
    {
        return _inner.HoldsOwnByteBudget(opRaw);
    }

  private:
    IFrameResponder& _inner;
    std::chrono::milliseconds _window;
};

/// A listener that never yields a connection.
///
/// These cases drive the MERGED surface. The worker is present because it owns the
/// capacity and the stop, not because its own accept loop is under test, and `Run()`
/// is never called on it.
class IdleListener final: public IListener
{
  public:
    AcceptAwaitable Accept() override
    {
        return AcceptAwaitable { AcceptResult { std::unexpect,
                                                NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} } } };
    }
    void Close() noexcept override {}
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override
    {
        return 0;
    }
};

/// A free loopback port, taken and released.
///
/// Per run rather than fixed: `catch_discover_tests` gives every case its own process
/// and the suite runs in parallel, so a chosen number is a failure that appears only
/// under `ctest -j`.
/// @return The port.
[[nodiscard]] std::uint16_t FreePort()
{
    auto probe = BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(probe);
    // Asked of the SOCKET: `Bind` returns a listener in an errored state rather than
    // nothing, so a null check passes on a bind that failed and the port comes back 0.
    REQUIRE(probe->IsBound());
    auto const port = probe->BoundPort();
    probe.reset();
    return port;
}

/// Send one frame to a loopback port and read exactly one framed reply.
///
/// Reads what the protocol declares rather than to EOF: this endpoint keeps the
/// connection, so reading to EOF would wait for the sweeper -- and on a surface whose
/// deadline is now ten minutes, that is a suite that never finishes.
/// @param port Where the endpoint listens.
/// @param frame The request.
/// @return The reply, or empty when the peer closed without answering.
[[nodiscard]] std::vector<std::byte> Exchange(std::uint16_t port, std::vector<std::byte> frame)
{
    BlockingConnector connector;
    auto socket =
        SyncRun(connector.Connect("127.0.0.1", port, DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
    REQUIRE(socket.has_value());

    auto reply = SyncRun([](ISocket* peer, std::vector<std::byte> request) -> Task<std::vector<std::byte>> {
        auto const written = co_await peer->Write(std::span<std::byte const> { request });
        if (!written.has_value())
            co_return std::vector<std::byte> {};

        std::vector<std::byte> received;
        auto want = Wire::ReplyHeaderSize;
        while (received.size() < want)
        {
            std::array<std::byte, 4096> chunk {};
            auto const read = co_await peer->Read(std::span<std::byte> { chunk });
            if (!read.has_value() || *read == 0)
                co_return std::vector<std::byte> {};
            received.insert(received.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*read));

            if (received.size() >= Wire::ReplyHeaderSize && want == Wire::ReplyHeaderSize)
                if (auto const header = Wire::DecodeReplyHeader(received); header.has_value())
                    want = Wire::ReplyHeaderSize + header->payloadLength;
        }
        co_return received;
    }((*socket).get(), std::move(frame)));

    (*socket)->Close();
    return reply;
}

/// A config naming @p port for the node surface.
/// @param port The loopback port to bind.
/// @return The config.
[[nodiscard]] NodeConfig ConfigForPort(std::uint16_t port)
{
    NodeConfig cfg;
    cfg.nodeListen = std::format("127.0.0.1:{}", port);
    return cfg;
}

/// Wait, bounded, for every compile to have given its slot back.
///
/// **This is the DIAGNOSTIC; the drain bound below it is only the net.** A case that
/// ends with a compile still counted in flight does not fail -- `CompileCapacity::Drain`
/// reaches `Abandon` and calls `std::_Exit`, so the case VANISHES: a nonzero exit, two
/// lines of Catch2 banner, no assertion, and the one line that explains it written to a
/// test logger nobody reads. Shortening the bound only makes that happen sooner; it is
/// the same unreadable failure, which is #297's whole complaint. A `REQUIRE` on this
/// gives a person a red case with a number instead.
///
/// **Bounded rather than sampled once**, and that is not timidity: a compile released a
/// moment earlier is legitimately still hopping back onto the reactor, so an instant
/// check would be flaky in the opposite direction and would report a working fixture as
/// broken. What is being asserted is that the slot comes back *at all*, which is exactly
/// the condition whose absence makes the drain vanish.
/// @param capacity The worker's accounting.
/// @param bound How long to allow.
/// @return Whether the in-flight count reached zero.
[[nodiscard]] bool DrainedWithin(CompileCapacity const& capacity, std::chrono::milliseconds bound)
{
    // Against a real deadline, not by accumulating the NOMINAL poll: a 5 ms
    // `sleep_for` returns in about 15 ms on Windows, so counting the requested
    // interval made this bound roughly three times what it claimed -- a figure that
    // reads like five seconds and is not.
    constexpr auto Poll = std::chrono::milliseconds { 5 };
    auto const deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (capacity.InFlight() == 0)
            return true;
        std::this_thread::sleep_for(Poll);
    }
    return capacity.InFlight() == 0;
}

/// The worker, the responder and the router a merged-surface case drives.
///
/// Held together because their declaration ORDER is load-bearing and silently so: the
/// router points at the responder and the responder holds the worker's capacity, so
/// the three are destroyed router-first. Spelled out in each case, that ordering is a
/// thing three cases would each have to get right.
struct MergedWorker
{
    HoldingRunner runner;
    Cc::CompileJobRunner jobs;
    Cc::WorkerProtocol protocol;
    ThreadPoolExecutor pool { 2 };
    // The accounting on its own. It was reached through a `WorkerServer`, which was
    // this object plus the accept loop #290 stage 3 retired -- and the loop was never
    // what these cases were about: they exercise the merged surface's responder, which
    // has always spent the capacity rather than the listener.
    CompileCapacity capacity;
    CompileResponder responder;

    /// @param fix Supplies the scratch directory, metrics, logger and membership.
    /// @param io The loop the responder returns its answers on.
    MergedWorker(Fixture& fix, NodeIoLoop& io):
        jobs { runner, fix.scratch.Path(), { { "gcc-13", "g++" } }, Cc::ToolchainSurvey::Completed() },
        protocol { jobs, Cc::UncheckedLeaseValidator(), { Wire::IdentityCodec }, fix.metrics },
        // **Five seconds rather than the thirty-second default: the NET, not the
        // diagnostic.** `DrainedWithin` above is what reports a slot that never came
        // back, as an ordinary red assertion with a number. This only bounds how long a
        // violation that got past it costs, and it cannot be made legible by shortening
        // -- `Drain` still reaches `Abandon` and calls `std::_Exit`, so the case still
        // vanishes into two lines of banner, just sooner (#297). Five over two for
        // headroom on a loaded runner.
        //
        // **Whether a violation reports or vanishes is a race, and these are the three
        // numbers.** Measured, with the body-level release removed so the slot really
        // does not come back: at this 5 s bound the drain returned 2.5 s after
        // `StopAndWait` began -- the holder's own 10 s self-release landing inside it --
        // and the case reported `exit 1` with a full summary. At a 1 s bound the same
        // violation abandoned and gave `exit 75` with no summary at all.
        //
        // So reporting is NOT a property of using `CHECK`; it is this bound, checked at
        // `DrainReportInterval` granularity, being longer than however long the
        // outstanding job takes to finish -- counted from a moment no case author can
        // see. None of the three figures means anything quoted alone.
        //
        // It is NOT what makes these cases correct -- see the destructor.
        capacity { 2, WorkerMaxRequestBytes, std::chrono::seconds { 5 }, fix.logger },
        responder { protocol, capacity, fix.membership, pool, io.Reactor(), fix.metrics, fix.logger }
    {
    }

    MergedWorker(MergedWorker const&) = delete;
    MergedWorker& operator=(MergedWorker const&) = delete;
    MergedWorker(MergedWorker&&) = delete;
    MergedWorker& operator=(MergedWorker&&) = delete;

    /// A safety net, and deliberately not the mechanism.
    ///
    /// **Every case must drain EXPLICITLY, before its endpoint goes out of scope.** The
    /// endpoint is a local declared after this fixture, so it is destroyed first --
    /// `~FrameEndpoint` stops accepting, its loops end, and `NodeIoLoop` then stops the
    /// reactor. A compile still out on the pool at that moment has nowhere to hop home
    /// to: its slot is never released, and this destructor then waits out the whole
    /// bound and ends the process.
    ///
    /// That is #290's own 1b hazard, reproduced in a fixture by not following the
    /// ordering `WorkerBody` follows -- which is the point worth keeping. It cost a red
    /// CI leg that reproduced on `cl` and not `clang-cl`, because whether the hop home
    /// wins that race is a timing question and nothing about it is compiler-specific.
    ~MergedWorker()
    {
        // **Released BEFORE the drain, so `Abandon` is unreachable from a test.**
        //
        // A case whose `DrainedWithin` assertion fires unwinds into here with the job
        // still not draining -- that is WHY it fired -- so without this, `Drain` reaches
        // `Abandon` and calls `std::_Exit`.
        //
        // What that costs was measured, and it is narrower than it sounds. **The failed
        // assertion's own text is written either way**; what `_Exit` takes is the Catch2
        // SUMMARY and the exit code -- 75 with no `test cases: 1 | 1 failed` line,
        // against 1 with one. So a reader loses the line naming which case failed, and
        // CI reports something that reads as a crashed binary rather than as a failed
        // test. That is worth this call; it is not the assertion being swallowed.
        //
        // Releasing first means the job always completes, the drain always finishes, and
        // `Abandon` is unreachable from a test -- which is what makes the sentence above
        // about a safety net true rather than aspirational.
        //
        // Safe here on stronger grounds than declaration order: every member is alive
        // for the whole of a destructor BODY, since member destruction happens after it.
        // `Release` is idempotent, so the cases that already released pay nothing.
        runner.Release();
        capacity.Drain();
    }
};

} // namespace

TEST_CASE("The endpoint arms the responder's deadline, not its own", "[node][compile-responder]")
{
    // **The blocker, and this half is about the MECHANISM.** `FrameServer` armed one
    // five-second window across the whole request, answer included -- right for a cache
    // round trip and fatal for a compile, which runs for minutes. `ServeConnection` is
    // not parked on the socket while a responder answers, so the sweep acts without
    // waking anything: the compile runs to completion, hops home, and its answer is not
    // the one the client gets. Every translation unit worth distributing would be
    // compiled, paid for and discarded, while short ones succeeded -- so a smoke test
    // passes.
    //
    // Proved in two halves rather than by holding a ten-minute compile:
    //
    //   * HERE, that the endpoint arms the window the RESPONDER names -- by making that
    //     window short and watching the compile's answer be replaced by the deadline
    //     refusal, which is the bug's own shape reproduced deliberately and in under a
    //     second.
    //   * In the case below, that `CompileResponder` names a window a compile fits in.
    //
    // Neither half means anything alone: the first would pass against a hard-coded
    // number, the second against an endpoint that ignored it.
    //
    // **What the client receives changed in #523 and what this case proves did not.**
    // The swept peer used to get a bare close and read back nothing, and this case
    // asserted that emptiness -- which was asserting the SYMPTOM, since a peer that
    // cannot tell a sweep from a crash is the whole of that ticket. It now receives
    // `RequestDeadlineExceeded`, written by the connection itself when `Answer` returns.
    // That is a strictly stronger reading of the same fact: an empty result is also what
    // a crash, a refused connect or an unrelated close look like, while this code can
    // only be produced by the responder's own window expiring. The short window is still
    // what governs, and now the client is told so.
    Fixture fix;
    NodeIoLoop io;
    MergedWorker worker { fix, io };

    // Far below what this compile will take, standing in for the five seconds every
    // dispatched TU used to be given.
    ShortWindowResponder tooShort { worker.responder, std::chrono::milliseconds { 100 } };
    MergedResponder merged { nullptr, nullptr, &tooShort };

    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(io, NodeSurface::Node, ConfigForPort(port), merged, fix.metrics, fix.logger);
    REQUIRE(endpoint.has_value());
    io.Start();

    auto pending = std::async(std::launch::async, [port] { return Exchange(port, CompileFrame()); });
    REQUIRE(worker.runner.WaitForStarted(1));

    // Held until the sweep has actually HAPPENED, then let go.
    //
    // **Waited for, not slept through, and the fixed sleep was wrong twice.** An
    // expired deadline does nothing until the sweeper looks, and it looks every
    // `SweepInterval` -- so a first attempt held 400 ms, exceeded the 100 ms window
    // four times over, was never swept, and reported the fix as broken when the
    // fixture's arithmetic was. A sleep of `SweepInterval * 2` fixed that and then
    // broke the other way once the sweep learned to DEFER: the deferral is bounded by
    // `ExplanationGraceFor(100ms)`, which floors at one sweep interval, so the entry
    // was closed on the following tick and the release landed after the connection
    // was already gone. Two constants either side of a window neither of them names.
    //
    // Waiting on the counter removes both. It is the STAGE this case is about -- the
    // sweep has observed this connection -- and the release then happens promptly
    // inside the grace rather than at a guessed offset from it. Bounded, and it says
    // what it waited for.
    auto const sweptBy = std::chrono::steady_clock::now() + std::chrono::seconds { 30 };
    while (fix.metrics.Read(IMetricsSink::Counter::FrameAnswerDeadlineSweeps) == 0
           && std::chrono::steady_clock::now() < sweptBy)
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    REQUIRE(fix.metrics.Read(IMetricsSink::Counter::FrameAnswerDeadlineSweeps) == 1);
    worker.runner.Release();

    // The window the RESPONDER named is what expired, and the client is told which
    // fact that was. Asserted on the CODE rather than on emptiness: empty is also what
    // a crash, a refused connect and an unrelated close produce, so it could never say
    // that this particular window was the one that governed.
    auto const swept = pending.get();
    REQUIRE_FALSE(swept.empty());
    auto const header = Wire::DecodeReplyHeader(swept);
    REQUIRE(header.has_value());
    REQUIRE(header->status == Wire::Status::Error);
    REQUIRE(header->payloadLength != 0);
    CHECK(static_cast<Wire::ErrorCode>(swept[Wire::ReplyHeaderSize]) == Wire::ErrorCode::RequestDeadlineExceeded);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::FrameDeadlineRefusalsSent) == 1);

    // And the worker still paid in full: the compiler ran and the object was produced,
    // for an answer nobody receives. That is the half #523 did NOT change and this case
    // still exists to hold -- the client now learns the request was abandoned, and the
    // CPU is spent either way.
    //
    // None of the worker's own refusal series moved, which is the point: nothing about
    // this is a capacity decision, a drain or an oversize frame, so an operator reading
    // those graphs would still see a flat line. What rose is the endpoint's deadline
    // rows, because the deadline is the endpoint's fact.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedStopping) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge) == 0);

    // **Drained HERE, while the endpoint is alive and its reactor still turning.**
    //
    // This case is the one that needs it: the connection is swept at about one sweep
    // interval, so `pending` becomes ready long before `Release()` lets the compile
    // finish. The compile is therefore still on the pool as this case ends, and the
    // destructors race its hop home -- endpoint first, which stops the reactor, then
    // the drain, which waits for a slot nothing will ever release. It cost a red CI
    // leg, at forty-four seconds, reporting nothing but the Catch2 banner.
    //
    // The same ordering `WorkerBody` uses in production, and for the same reason.
    //
    // Asserted BEFORE the drain, never left to it: if the slot never comes back this
    // says so with a number, where `StopAndWait` would take the whole binary down
    // without reporting anything at all.
    INFO("the slot taken by this compile never came back, so the drain below would "
         "abandon it and _Exit(75) -- a vanished binary rather than a red case (#297)");
    // CHECK, not REQUIRE, and that is the difference between a red case and a vanished
    // binary. A `REQUIRE` aborts the case here -- unwinding past the drain below and
    // into `~MergedWorker`, which runs AFTER `~endpoint` has already stopped the
    // reactor, so the released job's hop home has nowhere to land and the drain
    // abandons. Measured: the assertion text survived, the exit code was still 75 and
    // Catch2 never printed a summary. Continuing instead keeps the cleanup in the case
    // BODY, where the endpoint is alive and the reactor still turning.
    CHECK(DrainedWithin(worker.capacity, std::chrono::seconds { 5 }));
    worker.capacity.Drain();
}

TEST_CASE("A compile outlives the five seconds that used to bound it", "[node][compile-responder]")
{
    // **The direct proof, and it is worth the seconds it costs.** Same arrangement with
    // `CompileResponder`'s OWN window instead of the short stand-in, and the compile is
    // held past `HeaderTimeout` -- the single five-second window that used to cover the
    // answer as well as the header, and that would therefore have swept this connection
    // and thrown the finished object away.
    //
    // An earlier version held 400ms and asserted only that the responder's NUMBER was
    // large. That is a two-step argument -- the endpoint reads the number, the number is
    // big -- and it would have passed against a build where the endpoint quietly kept
    // its own window for the answer. Holding past the old ceiling collapses both steps
    // into one observation.
    //
    // **Do not shorten this sleep.** Below `HeaderTimeout` the case stops discriminating
    // and becomes a slower copy of the one above.
    Fixture fix;
    NodeIoLoop io;
    MergedWorker worker { fix, io };
    MergedResponder merged { nullptr, nullptr, &worker.responder };

    // The SIZE, asserted rather than waited out: ten minutes of held compile would be a
    // suite nobody runs. Paired with the mechanism above, the two cover both.
    REQUIRE(worker.responder.RequestTimeout(static_cast<std::uint8_t>(Wire::Op::Compile))
            == Wire::DefaultCompileLeaseTimeout);
    REQUIRE(worker.responder.RequestTimeout(static_cast<std::uint8_t>(Wire::Op::Compile)) > FrameServer::HeaderTimeout);

    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(io, NodeSurface::Node, ConfigForPort(port), merged, fix.metrics, fix.logger);
    REQUIRE(endpoint.has_value());
    io.Start();

    auto pending = std::async(std::launch::async, [port] { return Exchange(port, CompileFrame()); });
    REQUIRE(worker.runner.WaitForStarted(1));

    // Past the old ceiling, and then past a sweep -- an expired deadline does nothing
    // until the sweeper looks. Derived from the two constants rather than written as a
    // number, so it cannot silently stop covering either if one of them moves.
    std::this_thread::sleep_for(FrameServer::HeaderTimeout + FrameServer::SweepInterval * 2);
    worker.runner.Release();

    auto const reply = pending.get();
    REQUIRE_FALSE(reply.empty());
    CHECK(StatusOf(reply) == Wire::Status::Ok);

    // No race here today -- this connection is SERVED, so waiting for the reply above
    // already means the compile finished and hopped home. Asserted and drained anyway,
    // because that is a property of what this case happens to assert rather than of how
    // it is built, and the next edit to it need not preserve it.
    INFO("the slot taken by this compile never came back, so the drain below would "
         "abandon it and _Exit(75) -- a vanished binary rather than a red case (#297)");
    // CHECK, not REQUIRE, and that is the difference between a red case and a vanished
    // binary. A `REQUIRE` aborts the case here -- unwinding past the drain below and
    // into `~MergedWorker`, which runs AFTER `~endpoint` has already stopped the
    // reactor, so the released job's hop home has nowhere to land and the drain
    // abandons. Measured: the assertion text survived, the exit code was still 75 and
    // Catch2 never printed a summary. Continuing instead keeps the cleanup in the case
    // BODY, where the endpoint is alive and the reactor still turning.
    CHECK(DrainedWithin(worker.capacity, std::chrono::seconds { 5 }));
    worker.capacity.Drain();
}

TEST_CASE("A compile in flight is drained before anything can stop the reactor", "[node][compile-responder]")
{
    // **The second defect the in-process cases could not see.** `NodeIoLoop` stops its
    // reactor when the last ADOPTED loop ends -- the accept loop and the sweeper -- and
    // a connection task parked off-reactor is not one of them. So tearing the surface
    // down while a compile is still out on the pool stops the reactor underneath the hop
    // home: the coroutine is never resumed, its slot and byte reservation are never
    // released, and the worker then waits out its whole drain timeout and `_Exit`s
    // reporting compiles still running that had in fact already finished. The bounded
    // stop (#239) would blame the thing it broke.
    //
    // `WorkerBody` closes both doors and drains before any of that, which is what
    // `StopAndWait` exists as a callable thing for -- destruction order cannot express
    // it, because the router, the responder and the worker must be destroyed in exactly
    // the opposite order.
    //
    // Asserted three ways: the reply alone would not distinguish a drain that merely
    // won a race.
    Fixture fix;
    NodeIoLoop io;
    MergedWorker worker { fix, io };
    MergedResponder merged { nullptr, nullptr, &worker.responder };

    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(io, NodeSurface::Node, ConfigForPort(port), merged, fix.metrics, fix.logger);
    REQUIRE(endpoint.has_value());
    io.Start();

    auto pending = std::async(std::launch::async, [port] { return Exchange(port, CompileFrame()); });
    REQUIRE(worker.runner.WaitForStarted(1));

    // The compile is on the pool and its slot is held, so the drain below has something
    // real to wait for rather than passing vacuously.
    REQUIRE(worker.capacity.InFlight() == 1);

    worker.runner.Release();

    // The precondition, before the drain is relied on for it. This case asserts the
    // drain's BEHAVIOUR below; without this line a broken one would vanish here rather
    // than fail, and the assertions that follow would never run.
    INFO("the slot taken by this compile never came back, so the drain below would "
         "abandon it and _Exit(75) -- a vanished binary rather than a red case (#297)");
    // CHECK, not REQUIRE, and that is the difference between a red case and a vanished
    // binary. A `REQUIRE` aborts the case here -- unwinding past the drain below and
    // into `~MergedWorker`, which runs AFTER `~endpoint` has already stopped the
    // reactor, so the released job's hop home has nowhere to land and the drain
    // abandons. Measured: the assertion text survived, the exit code was still 75 and
    // Catch2 never printed a summary. Continuing instead keeps the cleanup in the case
    // BODY, where the endpoint is alive and the reactor still turning.
    CHECK(DrainedWithin(worker.capacity, std::chrono::seconds { 5 }));
    worker.capacity.Drain();

    // 1. The drain waited for the hop HOME, not merely for the compiler to exit.
    CHECK(worker.capacity.InFlight() == 0);

    // 2. And it returned while the reactor was still turning -- the ordering itself
    //    rather than a proxy for it. A drain finishing after the loops had ended would
    //    be one whose compile had nowhere to come home to.
    CHECK(io.LoopsRunning() > 0);

    // 3. The client got its object. Under the defect this is empty: the answer is
    //    posted to a reactor nobody drains and never leaves the process.
    auto const reply = pending.get();
    REQUIRE_FALSE(reply.empty());
    CHECK(StatusOf(reply) == Wire::Status::Ok);
}

TEST_CASE("Both doors charge one number for a frame the budget cannot afford", "[node][compile]")
{
    // Issue #448, second half. The two doors into this worker reached the charge by
    // different routes and disagreed about exactly one case: the accept loop reserves
    // the frame length before reading and RAISES, so an unpayable footprint left it
    // holding the frame length; `CompileResponder` takes its reservation in one go
    // once it has the whole frame, so an unpayable footprint left it holding NOTHING.
    //
    // Both were defensible alone and the pair was not -- the whole thesis of one
    // worker behind two doors is that the accounting does not depend on which door
    // was used. `ChargeFor` is now the one number both reach, so this pins the rule
    // rather than either call site.
    NullLogger logger;
    constexpr std::size_t Budget = 1024;
    CompileCapacity capacity { /*slots*/ 4, Budget, std::chrono::seconds { 1 }, logger };

    // An affordable footprint is charged as the footprint: the codec envelope's
    // declared expansion is what the request costs, and it is the larger number.
    CHECK(capacity.ChargeFor(/*footprint*/ 800, /*framed*/ 100) == 800);

    // The boundary is inclusive on both sides, and both sides are pinned -- a guard
    // checked on one side only fires when nothing is wrong.
    CHECK(capacity.ChargeFor(Budget, /*framed*/ 100) == Budget);
    CHECK(capacity.ChargeFor(Budget + 1, /*framed*/ 100) == 100);

    // The case the doors disagreed about. Not the footprint, because `EndpointBusy`
    // on an idle worker is a retry loop against a frame that can never fit; and not
    // ZERO either, because the frame has arrived and is held.
    CHECK(capacity.ChargeFor(/*footprint*/ 1'000'000, /*framed*/ 500) == 500);

    // And it agrees with `IsChargeable`, which is what decides between them: two
    // predicates over one budget that disagreed would be this defect again.
    CHECK_FALSE(capacity.IsChargeable(Budget + 1));
    CHECK(capacity.IsChargeable(Budget));
}

TEST_CASE("The merged listener counts the frame it refuses without reading", "[node][compile]")
{
    // Issue #447, and it is #326 undone by a migration rather than a fresh omission.
    //
    // The frame-level ceiling is the CHEAPEST probe there is -- a 24-byte header
    // declaring more than this surface will ever buffer -- so it is the likeliest
    // thing to be pointed at a node, and on the dedicated compile port #326 gave it
    // the one counter an operator can see. #290 stage 3 retired that port. The merged
    // `0xFC` listener answers the same refusal correctly on the wire and moves
    // nothing, so the series went flat on the surface that became the primary one,
    // with no test failing.
    Fixture fix;
    NodeIoLoop io;
    MergedWorker worker { fix, io };
    MergedResponder merged { nullptr, nullptr, &worker.responder };

    auto const port = FreePort();
    auto endpoint = FrameEndpoint::Start(io, NodeSurface::Node, ConfigForPort(port), merged, fix.metrics, fix.logger);
    REQUIRE(endpoint.has_value());
    io.Start();

    // A header alone, naming COMPILE and declaring more than the surface's own cap.
    // The bytes are never sent, which is the whole point: the refusal costs the peer
    // a header and costs this node one reply.
    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame,
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Compile),
                         static_cast<std::uint32_t>(merged.MaxRequestBytes() + 1));

    auto const reply = Exchange(port, std::vector<std::byte> { frame.begin(), frame.end() });
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::PayloadTooLarge);

    // The reply still names BOTH numbers: "too large" without the ceiling tells an
    // operator nothing about the limit they are up against.
    auto const body = std::span<std::byte const> { reply }.subspan(Wire::ReplyHeaderSize);
    auto const decoded = Wire::DecodeErrorPayload(body);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).second.contains(std::to_string(merged.MaxRequestBytes())));

    // And the only thing an operator can see rose. Without this the node refuses every
    // probe correctly and looks, on /metrics, exactly like a port nobody is talking to.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge) == 1);

    REQUIRE(DrainedWithin(worker.capacity, std::chrono::seconds { 5 }));
}
