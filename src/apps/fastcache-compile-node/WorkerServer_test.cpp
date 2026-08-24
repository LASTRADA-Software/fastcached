// SPDX-License-Identifier: Apache-2.0
#include "WorkerServer.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <CompileJob.hpp>
#include <ScratchPathTestSupport.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
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

struct Fixture
{
    StubRunner runner;
    std::filesystem::path scratch;
    Cc::CompileJobRunner jobs;
    AtomicMetricsSink metrics;
    Cc::WorkerProtocol protocol;
    NullLogger logger;

    /// Admits everybody, because these cases are about the accept loop rather than
    /// about who may reach it. The anti-leeching rule has its own case below, which
    /// substitutes a listed oracle for exactly that reason.
    Distributed::OpenMembership membership;

    Fixture():
        scratch { Cc::Test::UniqueScratchPath("fc-ws") },
        jobs { runner, (std::filesystem::create_directories(scratch), scratch), { { "gcc-13", "g++" } } },
        protocol { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics }
    {
    }
    ~Fixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    static int& Counter()
    {
        static int counter = 0;
        return counter;
    }
};

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

/// Drive one request through a server and return everything written back.
[[nodiscard]] std::vector<std::byte> ServeOne(Fixture& fix, std::vector<std::byte> const& request, std::size_t slots)
{
    auto pair = InMemorySocketPair::Create();
    REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
        auto const r = co_await s->Write(std::span<std::byte const> { bytes });
        co_return r.has_value();
    }(pair.client.get(), request)));
    pair.client->ShutdownWrite();

    OneShotListener listener { std::move(pair.server) };
    WorkerServer server { listener, fix.protocol, slots, fix.membership, fix.metrics, fix.logger };
    SyncRun(server.Run());

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
    }(pair.client.get()));
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
    WorkerServer server { listener, fix.protocol, 2, fix.membership, fix.metrics, fix.logger };
    SyncRun(server.Run());
    CHECK(server.InFlight() == 0);
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
    WorkerServer server { listener, fix.protocol, /*slots=*/2, fix.membership, fix.metrics, fix.logger };
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
    WorkerServer server { listener, fix.protocol, /*slots=*/2, fix.membership, fix.metrics, fix.logger };

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
        auto pair = InMemorySocketPair::Create(0, std::move(peer));
        REQUIRE(SyncRun([](ISocket* s, std::vector<std::byte> bytes) -> Task<bool> {
            auto const r = co_await s->Write(std::span<std::byte const> { bytes });
            co_return r.has_value();
        }(pair.client.get(), request)));
        pair.client->ShutdownWrite();

        OneShotListener listener { std::move(pair.server) };
        WorkerServer server { listener, fix.protocol, 2, listed, fix.metrics, fix.logger };
        SyncRun(server.Run());

        return SyncRun([](ISocket* s) -> Task<std::vector<std::byte>> {
            std::vector<std::byte> out;
            while (true)
            {
                std::vector<std::byte> chunk(4096);
                auto const r = co_await s->Read(std::span<std::byte> { chunk });
                if (!r.has_value() || *r == 0)
                    break;
                out.insert(out.end(), chunk.begin(), std::next(chunk.begin(), static_cast<std::ptrdiff_t>(*r)));
            }
            co_return out;
        }(pair.client.get()));
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
