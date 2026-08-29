// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeMembership.hpp"
#include "WorkerServer.hpp"

#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <CompileJob.hpp>
#include <StubObjectTestSupport.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A runner that writes a canned object.
class StubRunner final: public Cc::IProcessRunner
{
  public:
    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        Cc::Test::WriteStubObject(argv);
        return Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }
};

/// A runner that reports it started and then blocks, so a case can hold several
/// compiles in flight at once and look at the worker while they are.
///
/// The wait is BOUNDED and the arrival count is what a case asserts on. An
/// unbounded one would turn a regression -- a worker that serves one at a time --
/// into a suite that hangs rather than one that fails, which this file has a
/// paragraph about further down.
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
            (void) _changed.wait_for(guard, std::chrono::seconds { 5 }, [this] { return _released; });
        }
        Cc::Test::WriteStubObject(argv);
        return Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

    /// @param many How many compiles to wait for.
    /// @return Whether that many had started before the bound elapsed.
    [[nodiscard]] bool WaitForStarted(std::size_t many)
    {
        auto guard = std::unique_lock { _mutex };
        return _changed.wait_for(guard, std::chrono::seconds { 5 }, [this, many] { return _started >= many; });
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

/// A listener that hands out prepared connections in order and then reports EOF.
class ScriptedListener final: public IListener
{
  public:
    explicit ScriptedListener(std::vector<std::unique_ptr<ISocket>> sockets):
        _sockets { std::move(sockets) }
    {
    }

    AcceptAwaitable Accept() override
    {
        if (_next < _sockets.size())
            return AcceptAwaitable { AcceptResult { std::move(_sockets[_next++]) } };
        return AcceptAwaitable { AcceptResult { std::unexpect,
                                                NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} } } };
    }
    void Close() noexcept override {}

    [[nodiscard]] std::uint16_t BoundPort() const noexcept override
    {
        return 0;
    }

  private:
    std::vector<std::unique_ptr<ISocket>> _sockets;
    std::size_t _next { 0 };
};

/// A listener that hands out one prepared connection and then reports EOF, so a
/// single `Run()` serves exactly one request and returns.
class OneShotListener final: public IListener
{
  public:
    explicit OneShotListener(std::unique_ptr<ISocket> socket):
        _socket { std::move(socket) }
    {
    }

    AcceptAwaitable Accept() override
    {
        if (_socket != nullptr)
            return AcceptAwaitable { AcceptResult { std::move(_socket) } };
        // After the one connection, report EOF so the loop returns and the test
        // completes rather than parking on a listener nothing will ever feed.
        return AcceptAwaitable { AcceptResult { std::unexpect,
                                                NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} } } };
    }
    void Close() noexcept override {}

    /// @return 0: these fakes hand back scripted sockets and never bind.
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override
    {
        return 0;
    }

  private:
    std::unique_ptr<ISocket> _socket;
};

/// A listener that reports the poll timeout a real one reports while idle, and
/// runs a hook once it has done so a given number of times.
///
/// Two deliberate choices. It reports *Timeout* rather than EOF, because a
/// listener that ended the loop by itself would let the shutdown cases pass
/// without the stop mechanism existing at all -- and "idle, then stopped" is
/// precisely the production shape, a supervisor sending SIGTERM to a worker that
/// happens to have no client.
///
/// And the hook is called from inside `Accept()` rather than from a second thread
/// watching the counter. A watcher thread races on that counter, and worse: if a
/// regression ever stopped the loop from polling, the watcher would spin forever
/// waiting for a count that never arrives, so the suite would HANG instead of
/// failing. Measured -- that is exactly what the first version of this test did
/// when the poll-timeout branch was mutated away. A test that wedges CI reports
/// less than no test.
class IdleListener final: public IListener
{
  public:
    IdleListener() = default;

    /// @param pollsBeforeHook Run `hook` on this poll (1-based); 0 never runs it.
    /// @param hook Called from inside Accept(), single-threaded.
    IdleListener(int pollsBeforeHook, std::function<void()> hook):
        _pollsBeforeHook { pollsBeforeHook },
        _hook { std::move(hook) }
    {
    }

    AcceptAwaitable Accept() override
    {
        ++_polls;
        if (_closed)
            return AcceptAwaitable { AcceptResult {
                std::unexpect, NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} } } };
        if (_polls == _pollsBeforeHook && _hook)
            _hook();
        return AcceptAwaitable { AcceptResult {
            std::unexpect, NetError { .code = NetErrorCode::Timeout, .systemCode = 0, .context = {} } } };
    }
    void Close() noexcept override
    {
        _closed = true;
    }

    /// @return 0: these fakes hand back scripted sockets and never bind.
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override
    {
        return 0;
    }

    /// How many times the loop has asked for a connection.
    [[nodiscard]] int Polls() const noexcept
    {
        return _polls;
    }

    /// Whether the loop closed this listener, i.e. observed a shutdown.
    [[nodiscard]] bool Closed() const noexcept
    {
        return _closed;
    }

  private:
    int _pollsBeforeHook { 0 };
    std::function<void()> _hook;
    bool _closed { false };
    int _polls { 0 };
};

/// An executor that resumes on the caller's thread, so a case stays single-threaded.
///
/// `ResumeOn` always suspends and posts, so this is a real hop through the seam
/// rather than a bypass of it -- the coroutine continues inside `Submit`. What it
/// buys is determinism: a case that asserts on a reply after `Run()` returns is
/// asserting on work that has finished, which a pool cannot promise and does not
/// need to.
class InlineExecutor final: public IExecutor
{
  public:
    void Submit(std::coroutine_handle<> handle) override
    {
        handle.resume();
    }
};

struct Fixture
{
    StubRunner runner;
    InlineExecutor executor;
    ScratchDirectory scratch { "fc-ws" };
    Cc::CompileJobRunner jobs;
    AtomicMetricsSink metrics;
    Cc::WorkerProtocol protocol;
    NullLogger logger;

    /// Admits everybody, because these cases are about the accept loop rather than
    /// about who may reach it. The anti-leeching rule has its own case below, which
    /// substitutes a listed oracle for exactly that reason.
    Distributed::OpenMembership membership;

    Fixture():
        jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } },
        protocol { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics }
    {
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;
};

/// A COMPILE frame carrying `source` as its source field, already enveloped.
///
/// The envelope belongs to the caller because that is what the budget cases differ
/// in -- an honest `Identity` one, or one declaring an expansion it does not carry.
/// Everything around it is boilerplate no case varies.
/// @param source The source field, exactly as it should travel.
/// @param fingerprint The toolchain to claim.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> FrameWithSource(std::span<std::byte const> source,
                                                     std::string_view fingerprint = "gcc-13")
{
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = source,
                                                      .acceptedCodecs = { Wire::IdentityCodec },
                                                      .sourceName = "a.cpp" });
}

[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13")
{
    constexpr std::string_view Source = "int main(){return 0;}";
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(Source.size()), Wire::AsBytes(Source));
    return FrameWithSource(enveloped, fingerprint);
}

/// A COMPILE header that declares `declared` payload bytes and carries none.
///
/// The budget is checked on the DECLARED length before a payload byte is read, so a
/// case can hold the whole of it with a header and never allocate 256 MiB.
/// @param declared What the header claims will follow.
/// @return Exactly `Wire::RequestHeaderSize` bytes.
[[nodiscard]] std::vector<std::byte> DeclaringHeader(std::uint32_t declared)
{
    std::vector<std::byte> header(Wire::RequestHeaderSize);
    WireFrame::PutHeader(std::span<std::byte> { header },
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::Compile),
                         declared);
    return header;
}

/// A COMPILE frame whose source envelope DECLARES `rawLength` and carries a
/// handful of bytes.
///
/// The attack #241 left reachable: the frame is about a hundred bytes, so the
/// budget's frame-length reservation is almost free, while the field one layer in
/// tells the decoder to size a buffer of `rawLength` and value-initialize it.
///
/// The codec is deliberately one no build has. What is under test is the price
/// charged for what the envelope DECLARES, and a codec this build could decode
/// would make an unfixed worker genuinely attempt the allocation the case is about
/// -- turning a regression into a test host that swaps rather than one that fails.
/// @param rawLength What the envelope claims it expands to.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> DeclaringEnvelopeFrame(std::uint32_t rawLength)
{
    constexpr std::uint8_t NoSuchCodec = 0xFE;
    constexpr std::string_view Compressed = "a few dozen bytes, and no more";
    return FrameWithSource(Wire::EncodeCodecEnvelope(NoSuchCodec, rawLength, Wire::AsBytes(Compressed)));
}

/// Read everything a server wrote back on one connection.
/// @param client The client side of the pair.
/// @return The bytes written back, which may be empty.
[[nodiscard]] std::vector<std::byte> ReadAll(ISocket& client)
{
    return SyncRun([](ISocket* s) -> Task<std::vector<std::byte>> {
        std::vector<std::byte> out;
        while (true)
        {
            std::vector<std::byte> chunk(4096);
            auto const r = co_await s->Read(std::span<std::byte> { chunk });
            if (!r.has_value() || *r == 0)
                break;
            out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*r));
            if (*r < chunk.size())
                break;
        }
        co_return out;
    }(&client));
}

/// Drive one request through a server whose policy and caller are both given.
///
/// Both are parameters because "who is calling, and what was this node told about
/// them" is the entire subject of the membership cases -- and a second copy of the
/// write / serve / read dance is how two cases meaning the same thing drift apart.
/// @param fix The protocol, metrics and executor to serve with.
/// @param membership The policy this server consults.
/// @param peer The address the connection appears to arrive from.
/// @param request The frame the caller sends.
/// @param slots The worker's concurrency cap; two, which admits every case here
///        that is not about the cap itself.
/// @return Everything the server wrote back, which may be empty.
[[nodiscard]] std::vector<std::byte> ServeOneFrom(Fixture& fix,
                                                  Distributed::IMembershipOracle const& membership,
                                                  std::string peer,
                                                  std::vector<std::byte> const& request,
                                                  std::size_t slots = 2)
{
    auto pair = InMemorySocketPair::Create(0, std::move(peer));
    REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
        auto const r = co_await s->Write(std::span<std::byte const> { bytes });
        co_return r.has_value();
    }(pair.client.get(), request)));
    pair.client->ShutdownWrite();

    OneShotListener listener { std::move(pair.server) };
    WorkerServer server { listener, fix.protocol, slots, membership, fix.metrics, fix.logger, fix.executor };
    SyncRun(server.Run());

    return ReadAll(*pair.client);
}

/// Drive one request through a server that admits everybody.
///
/// What the cases about the accept loop itself want: the caller's identity is not
/// their subject, so they do not spell one out.
[[nodiscard]] std::vector<std::byte> ServeOne(Fixture& fix, std::vector<std::byte> const& request, std::size_t slots)
{
    return ServeOneFrom(fix, fix.membership, {}, request, slots);
}

/// Whether a reply frame says the request was served.
///
/// `ErrorOf`'s sibling, and there for the same reason: every case asking "was this
/// caller admitted" otherwise spells out the same decode, guard and member access.
/// @param frame A reply frame.
/// @return Its status.
[[nodiscard]] Wire::Status StatusOf(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    REQUIRE(header.has_value());
    return Unwrap(header).status;
}

[[nodiscard]] Wire::ErrorCode ErrorOf(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    if (!header.has_value())
        return Wire::ErrorCode::MalformedFrame;
    auto const body = std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize);
    auto const decoded = Wire::DecodeErrorPayload(body);
    return decoded.has_value() ? decoded->first : Wire::ErrorCode::MalformedFrame;
}

} // namespace

TEST_CASE("A worker server answers one compile and closes", "[worker-server]")
{
    Fixture fix;
    auto const reply = ServeOne(fix, CompileFrame(), /*slots=*/2);

    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Ok);
}

TEST_CASE("A worker at capacity refuses rather than queues", "[worker-server]")
{
    // Refused, never queued: queueing hides the overload from the scheduler that is
    // trying to route around it, and the client has a local compile waiting either
    // way. Zero slots is the degenerate case of a full worker.
    Fixture fix;
    auto const reply = ServeOne(fix, CompileFrame(), /*slots=*/0);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::NoCapacity);
}

TEST_CASE("A capacity refusal is answered before the payload is read", "[worker-server]")
{
    // The cap is checked before the request is buffered, so an over-capacity client
    // does not make this worker hold its multi-megabyte payload first. Observable
    // as an answer to a frame whose body was never sent.
    Fixture fix;
    auto truncated = CompileFrame();
    truncated.resize(Wire::RequestHeaderSize); // header only; the body never arrives
    CHECK(ErrorOf(ServeOne(fix, truncated, /*slots=*/0)) == Wire::ErrorCode::NoCapacity);
}

TEST_CASE("A foreign magic gets no reply at all", "[worker-server]")
{
    // There is no framing in which an answer would be meaningful to a peer that is
    // not speaking this protocol, so the connection is simply closed.
    Fixture fix;
    std::vector<std::byte> junk(Wire::RequestHeaderSize, std::byte { 'G' });
    CHECK(ServeOne(fix, junk, /*slots=*/2).empty());
}

TEST_CASE("A wrong-fingerprint job is refused by the server path too", "[worker-server]")
{
    Fixture fix;
    CHECK(ErrorOf(ServeOne(fix, CompileFrame("clang-19"), /*slots=*/2)) == Wire::ErrorCode::FingerprintMismatch);
}

TEST_CASE("In-flight returns to zero after a job", "[worker-server]")
{
    // The heartbeat reports this number, and a count that drifted upward would make
    // the scheduler believe a worker is busier than it is -- taking it out of
    // rotation permanently and silently.
    Fixture fix;
    auto pair = InMemorySocketPair::Create();
    auto const request = CompileFrame();
    REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
        auto const r = co_await s->Write(std::span<std::byte const> { bytes });
        co_return r.has_value();
    }(pair.client.get(), request)));
    pair.client->ShutdownWrite();

    OneShotListener listener { std::move(pair.server) };
    WorkerServer server { listener, fix.protocol, 2, fix.membership, fix.metrics, fix.logger, fix.executor };
    SyncRun(server.Run());
    CHECK(server.InFlight() == 0);
}

TEST_CASE("A worker serves its slots at once, not one at a time", "[worker-server]")
{
    // The whole of #213. A node registers `--slots` and the scheduler dispatches
    // against it, so a worker that served inline advertised thirty and ran one --
    // `_inFlight` could never exceed 1, the cap was unreachable, and the busiest
    // reading a saturated fleet could report was `1 / 30 compiling`.
    ScratchDirectory const scratch { "worker-concurrent" };
    HoldingRunner runner;
    Cc::CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };
    AtomicMetricsSink metrics;
    Cc::WorkerProtocol protocol {
        jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics
    };
    Distributed::OpenMembership membership;
    NullLogger logger;
    ThreadPoolExecutor pool { 2 };

    // Three clients at a worker with two slots.
    std::vector<std::unique_ptr<ISocket>> accepted;
    std::vector<InMemorySocketPair> pairs;
    for ([[maybe_unused]] auto const index: { 0, 1, 2 })
    {
        auto pair = InMemorySocketPair::Create();
        auto const request = CompileFrame();
        REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
            auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
            co_return written.has_value();
        }(pair.client.get(), request)));
        pair.client->ShutdownWrite();
        accepted.push_back(std::move(pair.server));
        pairs.push_back(std::move(pair));
    }

    ScriptedListener listener { std::move(accepted) };
    {
        WorkerServer server { listener, protocol, 2, membership, metrics, logger, pool };
        SyncRun(server.Run());

        // TWO compiles inside the compiler at the same moment, which one thread
        // could not produce however long it was given.
        CHECK(runner.WaitForStarted(2));
        CHECK(server.InFlight() == 2);

        // And the third is refused rather than queued, which is the cap doing the
        // job it was written for -- unreachable until now.
        CHECK(metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 1);

        runner.Release();
    } // ~WorkerServer drains the two held compiles; the pool is joined below

    CHECK(metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 1);
}

TEST_CASE("A poll timeout keeps the accept loop running", "[worker-server]")
{
    // The half of the shutdown contract that fails silently: if a poll timeout
    // ended the loop, an idle worker would exit the first time nothing connected
    // -- still registered with the scheduler, gone from the network, and every job
    // dispatched to it hanging until the client's deadline.
    //
    // The listener stops the server on its third poll, so reaching three is proof
    // the loop survived the first two.
    Fixture fix;
    WorkerServer* running = nullptr;
    IdleListener listener { 3, [&] { running->Shutdown(); } };
    WorkerServer server { listener, fix.protocol, /*slots=*/2, fix.membership, fix.metrics, fix.logger, fix.executor };
    running = &server;

    SyncRun(server.Run());

    CHECK(listener.Polls() >= 3);
    CHECK(listener.Closed());
}

TEST_CASE("Shutdown ends an idle accept loop", "[worker-server]")
{
    // What a SIGTERM ultimately does. The signal handler only sets a flag and the
    // accept loop is parked inside Accept(), so closing the listener is the whole
    // mechanism by which a stop reaches the loop -- if this did not return, a
    // supervisor's stop would time out and escalate to SIGKILL.
    Fixture fix;
    IdleListener listener;
    WorkerServer server { listener, fix.protocol, /*slots=*/2, fix.membership, fix.metrics, fix.logger, fix.executor };

    server.Shutdown();
    SyncRun(server.Run());

    CHECK(listener.Closed());
    CHECK(server.InFlight() == 0);
}

TEST_CASE("A capacity refusal is counted apart from every other refusal", "[worker-server][metrics]")
{
    // The worker's half of `dispatch_leases_no_capacity`, and it must not be summed
    // with the four reasons the protocol counts: a busy worker and one whose
    // toolchain nobody matches are different operator problems, and the second is
    // the one that hides behind the first. Counted here rather than in the protocol
    // because the cap is checked before the request is read, so the protocol never
    // sees this job at all.
    Fixture fix;
    using Sink = IMetricsSink::Counter;

    auto const reply = ServeOne(fix, CompileFrame(), /*slots=*/0);
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::NoCapacity);

    CHECK(fix.metrics.Read(Sink::WorkerJobsRefusedNoSlot) == 1);

    // And nothing else moved: the job was never started, so it cannot appear as one
    // the worker took, nor as a refusal of any other kind.
    CHECK(fix.metrics.Read(Sink::WorkerJobsStarted) == 0);
    CHECK(fix.metrics.Read(Sink::WorkerJobsRefusedUnknownFingerprint) == 0);
    CHECK(fix.metrics.Read(Sink::WorkerBytesReceived) == 0);
}

TEST_CASE("A served request counts the bytes in both directions", "[worker-server][metrics]")
{
    // What says whether a codec negotiation is doing anything: preprocessed text
    // in against object bytes out. Counted at the socket, so "received" means the
    // payload as it arrived rather than what it decompressed to.
    Fixture fix;
    using Sink = IMetricsSink::Counter;

    auto const request = CompileFrame();
    auto const reply = ServeOne(fix, request, /*slots=*/2);
    REQUIRE_FALSE(reply.empty());

    CHECK(fix.metrics.Read(Sink::WorkerBytesReceived) == request.size());
    CHECK(fix.metrics.Read(Sink::WorkerBytesReturned) == reply.size());
}

TEST_CASE("A stranger is refused this machine's CPU before it can send a payload", "[node][worker][membership]")
{
    // The most serious of the three anti-leeching gates, and the one that was missing:
    // `--bind` defaults to 0.0.0.0 and the lease validator accepts every token, so
    // before this check anybody who could route to the port could have this machine
    // run their compiler for them.
    //
    // Checked BEFORE the request is read, which is why it lives in the accept loop
    // rather than in the protocol. Afterwards would let a caller with no claim on
    // this machine make it buffer a multi-megabyte preprocessed translation unit --
    // a memory-exhaustion hole opened by the check meant to close a hole, exactly
    // as `CompileCacheHandler`'s auth gate documents.
    Fixture fix;
    Distributed::ClusterMembership const listed { { "10.0.0.1:6676" } };

    auto const request = CompileFrame();

    auto refuse = [&](std::string peer) {
        return ServeOneFrom(fix, listed, std::move(peer), request, 2);
    };

    SECTION("a machine nobody admitted")
    {
        // Answered rather than dropped: a misconfigured peer learns it is not a
        // member instead of seeing a connection it cannot tell from a dead host.
        auto const reply = refuse("10.9.9.9");
        auto const header = Wire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).status == Wire::Status::Error);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNotAMember) == 1);

        // And no job was started, which is the property that matters: the refusal
        // happened before the payload, so nothing was buffered and no compiler ran.
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
    }

    SECTION("this machine, whatever the member list says")
    {
        // The rule that keeps an unconfigured node useful: a developer's own
        // `fastcache-cc` must reach its own worker even though the operator listed
        // only their peers.
        auto const reply = refuse("127.0.0.1");
        auto const header = Wire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).status == Wire::Status::Ok);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNotAMember) == 0);
    }

    SECTION("a listed peer")
    {
        auto const reply = refuse("10.0.0.1");
        auto const header = Wire::DecodeReplyHeader(reply);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).status == Wire::Status::Ok);
    }
}

TEST_CASE("A worker admits whoever its operator's policy names, scheduler or not", "[node][worker][membership]")
{
    // #235, and a WIRING case rather than an oracle one: `ClusterMembership` was
    // always correct about a listed peer -- the case above proves that -- and a pure
    // worker could nonetheless never be handed one. `StartupPolicyRejection` refused
    // `--fleet-member` on any node without `--listen-scheduler`, so the only oracle
    // such a node could construct was an empty list, which admits loopback and
    // nothing else. Every dispatched compile was refused `NotAMember` one hop after
    // the lease was granted, so no counter on either side moved.
    //
    // So every row drives the whole chain a `main()` walks below its parser -- the
    // configuration, the startup rules, `NodeMembership`, the oracle it hands out
    // and `WorkerServer` -- rather than the oracle alone. Substituting a hand-built
    // oracle is exactly what let the defect live behind a passing suite.
    struct Row
    {
        char const* what;                 ///< The shape, for the failure message.
        std::vector<std::string> members; ///< `--fleet-member`, if any.
        bool open;                        ///< Whether `--fleet-open` was given.
        char const* peer;                 ///< Where the connection appears to arrive from.
        Wire::Status expected;            ///< `Ok`, or `Error` meaning `NotAMember`.
    };

    auto const rows = std::to_array<Row>({
        { .what = "a listed peer, on a worker that schedules nothing",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .peer = "10.0.0.1",
          .expected = Wire::Status::Ok },
        // Admitting a listed peer is not opening the port: a worker serving whoever
        // could route to it would run a stranger's compiler on source they chose,
        // and `--bind` defaults to the wildcard.
        { .what = "a stranger, against that same list",
          .members = { "10.0.0.1:6676" },
          .open = false,
          .peer = "10.9.9.9",
          .expected = Wire::Status::Error },
        // The other half of the remedy, and the one a build LAN reaches for.
        // `OpenMembership` is only ever arrived at by an operator saying so.
        { .what = "a stranger, under --fleet-open",
          .members = {},
          .open = true,
          .peer = "10.9.9.9",
          .expected = Wire::Status::Ok },
        // The default #235 must not have moved: a node given neither flag is closed
        // to the network and useful to the machine it runs on, which is what makes
        // "off by default" safe and what an operator who types nothing gets.
        { .what = "this machine, with no policy at all",
          .members = {},
          .open = false,
          .peer = "127.0.0.1",
          .expected = Wire::Status::Ok },
        { .what = "the network, with no policy at all",
          .members = {},
          .open = false,
          .peer = "10.0.0.1",
          .expected = Wire::Status::Error },
    });

    for (auto const& row: rows)
    {
        INFO(row.what);

        // The command line the getting-started page documents, plus whatever policy
        // this row names. `--scheduler` and `--advertise` are read by neither the
        // rules nor the oracle; they are here because they are what makes this a
        // worker rather than a bare struct, and a worker is the shape under test.
        NodeConfig cfg;
        cfg.scheduler = "scheduler.internal:6675";
        cfg.advertise = "worker-01.internal:6676";
        cfg.fleetMembers = row.members;
        cfg.fleetOpen = row.open;

        // The link that was broken: a worker naming who may spend its CPU, and
        // naming no scheduler of its own, is a configuration that has to START.
        CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

        NodeMembership const membership { cfg };
        Fixture fix;

        CHECK(StatusOf(ServeOneFrom(fix, membership.Oracle(), row.peer, CompileFrame())) == row.expected);

        // A refusal costs nothing, which is the property that matters: it happens
        // before the payload is read, so nothing was buffered and no compiler ran.
        auto const refused = row.expected == Wire::Status::Error ? 1U : 0U;
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNotAMember) == refused);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 1U - refused);
    }
}

TEST_CASE("A worker bounds the payload bytes it reads at once, not just the jobs", "[worker-server]")
{
    // The other half of #213, and the rulebook says it had to land in the same
    // commit as the detach rather than after it: serving one compile at a time
    // bounded peak memory to a single request by accident, and serving `slots` of
    // them makes it `slots` times that -- 8 GiB on a 32-slot node, asked for by any
    // cluster member. A fix that opens a memory-exhaustion hole is not a fix.
    //
    // `MaxInFlightBytes` is deliberately one request's worth, so ordinary
    // translation units run side by side and a single monster cannot be joined.
    constexpr std::uint32_t WholeBudget = 256U * 1024U * 1024U;

    Fixture fix;

    // The first client declares the entire budget and sends none of it, so its job
    // is parked inside the payload read holding the reservation. No shutdown here:
    // the point is a job that is still reading, not one that failed.
    auto holder = InMemorySocketPair::Create();
    REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
        auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
        co_return written.has_value();
    }(holder.client.get(), DeclaringHeader(WholeBudget))));

    // The second is an ordinary compile, and there is a slot free for it.
    auto second = InMemorySocketPair::Create();
    REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
        auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
        co_return written.has_value();
    }(second.client.get(), CompileFrame())));
    second.client->ShutdownWrite();

    std::vector<std::unique_ptr<ISocket>> accepted;
    accepted.push_back(std::move(holder.server));
    accepted.push_back(std::move(second.server));

    ScriptedListener listener { std::move(accepted) };
    {
        WorkerServer server { listener, fix.protocol, /*slots=*/4, fix.membership, fix.metrics, fix.logger, fix.executor };
        SyncRun(server.Run());

        // Refused for MEMORY, with a slot free -- which is why it is its own code and
        // its own counter. Reported as NoCapacity it would send an operator to buy
        // machines over something more machines would not fix.
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy) == 1);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 0);
        CHECK(ErrorOf(ReadAll(*second.client)) == Wire::ErrorCode::EndpointBusy);

        // Let the holder go, or the destructor waits for a job nothing will finish.
        holder.client->Close();
    }
}

TEST_CASE("A stop that has nothing left to wait for is clean, however long it took", "[node][worker][drain]")
{
    // `Finished` outranks an expired bound, and that ordering is the one an operator
    // notices: reporting an abandonment for a stop that had already finished would
    // put a false alarm in the log at exactly the moment the thing worked.
    using namespace std::chrono_literals;

    CHECK(NextDrainAction(0, 0s, 30s) == DrainAction::Finished);
    CHECK(NextDrainAction(0, 60s, 30s) == DrainAction::Finished);
    CHECK(NextDrainAction(0, 60s, 0s) == DrainAction::Finished);
}

TEST_CASE("A stop reports while it waits and abandons only once the bound is spent", "[node][worker][drain]")
{
    // The decision `~WorkerServer` carries out, tested here because the branch that
    // matters ENDS THE PROCESS (#239) -- a side effect no in-process case can survive
    // is one no case would otherwise check. So the arithmetic lives in a pure
    // function and the destructor is left with nothing but obeying it.
    using namespace std::chrono_literals;

    CHECK(NextDrainAction(1, 0s, 30s) == DrainAction::Report);
    CHECK(NextDrainAction(4, 29s, 30s) == DrainAction::Report);

    // At the bound, not past it: a stop that waited exactly its timeout has spent it.
    CHECK(NextDrainAction(1, 30s, 30s) == DrainAction::Abandon);
    CHECK(NextDrainAction(1, 31s, 30s) == DrainAction::Abandon);
}

TEST_CASE("A zero bound waits forever, which is what this did before the bound", "[node][worker][drain]")
{
    // Kept sayable rather than removed. An operator who would rather have the
    // supervisor's timeout than this one has to be able to ask for that -- and a
    // behaviour change nobody can turn off is how a stop-policy change becomes an
    // incident on somebody else's fleet.
    using namespace std::chrono_literals;

    CHECK(NextDrainAction(1, 0s, 0s) == DrainAction::Report);
    CHECK(NextDrainAction(1, 24h, 0s) == DrainAction::Report);
}

TEST_CASE("A worker charges what an envelope declares it expands to, not the frame", "[worker-server]")
{
    // #241, one layer past the fix that named it. `Unenvelope` refuses a declared
    // expansion above THIS request's ceiling, but that ceiling is per request and the
    // in-flight budget never saw the number: `_bytesInFlight` reserved the COMPRESSED
    // frame length. So a member could open `slots` connections, send `slots` frames of
    // a hundred bytes each declaring a 256 MiB expansion, and drive `slots` x 256 MiB
    // of value-initialized memory concurrently -- the "slots times it" shape the budget
    // was introduced to close, reopened where it could not see it.
    constexpr std::uint32_t WholeBudget = 256U * 1024U * 1024U;

    Fixture fix;

    // ONE BYTE of somebody else's traffic, which is the whole point of the case. A
    // holder declaring a single payload byte and sending none parks inside the payload
    // read holding a one-byte reservation; if the frames below are charged their frame
    // length, all three fit beside it with 255 MiB to spare and every one is admitted.
    // They are refused only if the number the ENVELOPE declares is what is charged.
    auto holder = InMemorySocketPair::Create();
    REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
        auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
        co_return written.has_value();
    }(holder.client.get(), DeclaringHeader(1))));

    // Three complete requests, each declaring the per-request maximum expansion, each
    // arriving while the others are outstanding.
    std::vector<InMemorySocketPair> callers;
    std::vector<std::unique_ptr<ISocket>> accepted;
    accepted.push_back(std::move(holder.server));
    for ([[maybe_unused]] auto const index: { 0, 1, 2 })
    {
        auto pair = InMemorySocketPair::Create();
        REQUIRE(SyncRun([](ISocket* sock, std::vector<std::byte> bytes) -> Task<bool> {
            auto const written = co_await sock->Write(std::span<std::byte const> { bytes });
            co_return written.has_value();
        }(pair.client.get(), DeclaringEnvelopeFrame(WholeBudget))));
        pair.client->ShutdownWrite();
        accepted.push_back(std::move(pair.server));
        callers.push_back(std::move(pair));
    }

    ScriptedListener listener { std::move(accepted) };
    {
        // Four slots, so CPU is not what refuses anything here.
        WorkerServer server { listener, fix.protocol, /*slots=*/4, fix.membership, fix.metrics, fix.logger, fix.executor };
        SyncRun(server.Run());

        for (auto& caller: callers)
            CHECK(ErrorOf(ReadAll(*caller.client)) == Wire::ErrorCode::EndpointBusy);

        // Refused for MEMORY with three slots free, which is why `EndpointBusy` is its
        // own code and its own counter -- and refused BEFORE the decoder was asked, so
        // nothing allocated and no compiler ran.
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy) == 3);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedNoSlot) == 0);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);

        // Let the holder go, or the destructor waits for a job nothing will finish.
        holder.client->Close();
    }
}

TEST_CASE("An expansion no worker could ever hold is the decoder's refusal, not a busy signal", "[worker-server]")
{
    // The other half of the rule, and the half that decides what an operator does
    // next. `EndpointBusy` means "ask again shortly"; a frame declaring more than the
    // whole budget can never fit, so answering it that way on a completely idle worker
    // sends a client into a retry loop over something no amount of waiting fixes. An
    // unpayable price is therefore not charged at all -- it is left to `Unenvelope`,
    // which refuses it by name and without allocating.
    constexpr std::uint32_t WholeBudget = 256U * 1024U * 1024U;

    struct Row
    {
        std::string_view what;
        std::uint32_t declared;
        Wire::ErrorCode expected;
    };

    auto const rows = std::to_array<Row>({
        // Exactly the ceiling is payable, so an idle worker admits it and the decoder
        // gets its say -- here, that it has no such codec. Not a busy signal.
        { .what = "exactly the per-request maximum",
          .declared = WholeBudget,
          .expected = Wire::ErrorCode::UnsupportedCodec },
        // One byte past it is refused by the decoder that owns the per-request
        // ceiling, under the name that says which field was too large.
        { .what = "one byte past the per-request maximum",
          .declared = WholeBudget + 1U,
          .expected = Wire::ErrorCode::PayloadTooLarge },
    });

    for (auto const& row: rows)
    {
        INFO(row.what);
        Fixture fix;
        CHECK(ErrorOf(ServeOne(fix, DeclaringEnvelopeFrame(row.declared), /*slots=*/2)) == row.expected);
        CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy) == 0);
    }
}
