// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// One scripted peer: replays canned replies and records what it was sent.
class ScriptedPeer final: public ISocket
{
  public:
    explicit ScriptedPeer(std::vector<std::byte> replies, std::vector<std::byte>* sentSink):
        _replies(std::move(replies)),
        _sent(sentSink)
    {
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
    {
        if (_sent != nullptr)
            _sent->insert(_sent->end(), bytes.begin(), bytes.end());
        return IoAwaitable { IoResult { bytes.size() } };
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        // A read of zero is EOF, which is how a peer that ran out of script tells
        // RecvExactly the frame was short.
        auto const take = std::min(_replies.size() - _cursor, buffer.size());
        std::copy_n(_replies.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
        _cursor += take;
        return IoAwaitable { IoResult { take } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        return IoAwaitable { IoResult { 0 } };
    }

    void Close() noexcept override {}
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return "scripted";
    }

  private:
    std::vector<std::byte> _replies;
    std::vector<std::byte>* _sent;
    std::size_t _cursor { 0 };
};

/// A dialer that hands out a scripted peer per endpoint, and records which
/// endpoints were dialled and in what order.
class ScriptedDialer final: public IEndpointDialer
{
  public:
    /// Register a peer for `endpoint`. An endpoint with no entry is unreachable,
    /// which is how the "worker is down" cases are written.
    void Serve(std::string endpoint, std::vector<std::byte> replies)
    {
        _scripts.emplace(std::move(endpoint), std::move(replies));
    }

    std::unique_ptr<ISocket> Dial(std::string_view hostPort) override
    {
        _dialled.emplace_back(hostPort);
        auto const it = _scripts.find(std::string { hostPort });
        if (it == _scripts.end())
            return nullptr;
        return std::make_unique<ScriptedPeer>(it->second, &_sent[std::string { hostPort }]);
    }

    /// Endpoints dialled, in order. The ORDER is the assertion in several cases:
    /// the client must ask the scheduler and then go where it was sent, never
    /// guess at a worker.
    [[nodiscard]] std::vector<std::string> const& Dialled() const noexcept
    {
        return _dialled;
    }

    /// Everything written to `endpoint`.
    [[nodiscard]] std::vector<std::byte> const& SentTo(std::string const& endpoint)
    {
        return _sent[endpoint];
    }

  private:
    std::vector<std::string> _dialled;
    std::map<std::string, std::vector<std::byte>> _sent;
    std::map<std::string, std::vector<std::byte>> _scripts;
};

constexpr std::string_view Scheduler = "sched:6675";
constexpr std::string_view Worker = "worker:6676";

/// A lease reply granting `Worker`.
[[nodiscard]] std::vector<std::byte> GrantReply(Wire::CodecList codecs = {})
{
    return Wire::EncodeReply(Wire::Status::Ok,
                             Wire::EncodeLeaseGrant(Wire::LeaseGrant {
                                 .endpoint = Worker, .leaseToken = "l1", .workerCodecs = std::move(codecs) }));
}

/// A compile reply carrying `object` and an exit code.
[[nodiscard]] std::vector<std::byte> CompileReply(std::string_view object,
                                                  std::uint32_t exitCode = 0,
                                                  std::string_view err = {})
{
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(object.size()), Wire::AsBytes(object));
    return Wire::EncodeReply(
        Wire::Status::Ok,
        Wire::EncodeCompileResult(Wire::CompileResult {
            .exitCode = exitCode, .object = enveloped, .stdoutText = {}, .stderrText = Wire::AsBytes(err) }));
}

[[nodiscard]] DispatchRequest Request(std::span<std::string const> args)
{
    return DispatchRequest { .schedulerEndpoint = Scheduler,
                             .fingerprint = "gcc-13-abc",
                             .objectKey = "objkey",
                             .args = args,
                             .preprocessed = "int main() { return 0; }",
                             .sourceName = "a.cpp" };
}

} // namespace

TEST_CASE("A dispatched compile reaches the worker the scheduler named", "[dispatch]")
{
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJECTBYTES"));

    std::vector<std::string> const args { "-O2", "-std=c++23" };
    auto const result = Dispatch(dialer, Request(args));

    REQUIRE(result.Ran());
    CHECK(result.exitCode == 0);
    CHECK(std::string(reinterpret_cast<char const*>(result.object.data()), result.object.size()) == "OBJECTBYTES");
    CHECK(result.workerEndpoint == Worker);

    // The scheduler is asked first, then the worker it named -- the client never
    // guesses at an endpoint.
    REQUIRE(dialer.Dialled().size() == 2);
    CHECK(dialer.Dialled()[0] == Scheduler);
    CHECK(dialer.Dialled()[1] == Worker);
}

TEST_CASE("A failing remote compile is a successful dispatch", "[dispatch]")
{
    // The distinction the whole result type turns on: the compiler RAN and rejected
    // the code. That is not a dispatch failure, and reporting it as one would send
    // the caller down the "the cache let us down" path instead of showing the user
    // their own compile error.
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("", 1, "error: no"));

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(dialer, Request(args));

    REQUIRE(result.Ran());
    CHECK(result.exitCode == 1);
    CHECK(result.stderrText == "error: no");
}

TEST_CASE("Every scheduler refusal is a decline, not a failure", "[dispatch]")
{
    // NoWorker, NoCapacity, AlreadyInFlight and DispatchNotPermitted are all
    // ordinary, and all answered the same way by the caller: compile locally. The
    // scheduler's own words travel so the caller can say which it was.
    for (auto const code: { Wire::ErrorCode::NoWorker,
                            Wire::ErrorCode::NoCapacity,
                            Wire::ErrorCode::AlreadyInFlight,
                            Wire::ErrorCode::DispatchNotPermitted })
    {
        ScriptedDialer dialer;
        dialer.Serve(std::string { Scheduler }, Wire::EncodeErrorReply(code, {}));

        std::vector<std::string> const args { "-O2" };
        auto const result = Dispatch(dialer, Request(args));

        INFO("error code " << static_cast<unsigned>(code));
        CHECK(result.status == DispatchStatus::Declined);
        CHECK_FALSE(result.Ran());
        CHECK_FALSE(result.detail.empty());
        // The worker is never dialled when there is no grant.
        CHECK(dialer.Dialled().size() == 1);
    }
}

TEST_CASE("An unreachable scheduler is unavailable, and no worker is dialled", "[dispatch]")
{
    ScriptedDialer dialer; // nothing registered
    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(dialer, Request(args));

    CHECK(result.status == DispatchStatus::Unavailable);
    CHECK(result.detail.contains("scheduler"));
    CHECK(dialer.Dialled().size() == 1);
}

TEST_CASE("An unreachable worker is unavailable and names the endpoint", "[dispatch]")
{
    // The scheduler granted a lease pointing at a machine that is not there. Naming
    // it is the difference between an operator finding a dead node and an operator
    // seeing "distribution stopped working".
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(dialer, Request(args));

    CHECK(result.status == DispatchStatus::Unavailable);
    CHECK(result.detail.contains(Worker));
}

TEST_CASE("A worker refusing the job is a decline, not a compile", "[dispatch]")
{
    // An unknown lease, a fingerprint it does not have, an argument it will not
    // accept. Distinct from the compiler running and rejecting the code.
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, Wire::EncodeErrorReply(Wire::ErrorCode::UnknownLease, {}));

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(dialer, Request(args));

    CHECK(result.status == DispatchStatus::Declined);
    CHECK_FALSE(result.Ran());
    CHECK(result.detail.contains("unknown-lease"));
}

TEST_CASE("A malformed grant or result is unavailable, never a silent success", "[dispatch]")
{
    SECTION("grant")
    {
        ScriptedDialer dialer;
        dialer.Serve(std::string { Scheduler }, Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes("not-a-grant")));
        std::vector<std::string> const args { "-O2" };
        CHECK(Dispatch(dialer, Request(args)).status == DispatchStatus::Unavailable);
    }
    SECTION("result")
    {
        ScriptedDialer dialer;
        dialer.Serve(std::string { Scheduler }, GrantReply());
        dialer.Serve(std::string { Worker }, Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes("junk")));
        std::vector<std::string> const args { "-O2" };
        CHECK(Dispatch(dialer, Request(args)).status == DispatchStatus::Unavailable);
    }
}

TEST_CASE("The arguments the worker receives are the ones it was given", "[dispatch]")
{
    // Round-tripped through the wire encoding, because an argument containing a
    // space is the case a joined-string encoding would silently split.
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2", "-DMSG=hello world", "-std=c++23" };
    REQUIRE(Dispatch(dialer, Request(args)).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const header = Wire::DecodeRequestHeader(toWorker);
    REQUIRE(header.has_value());
    auto const payload = std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize);
    auto const compile = Wire::DecodeCompilePayload(payload);
    REQUIRE(compile.has_value());
    CHECK(DecodeArgs(Unwrap(compile).args) == args);
}

TEST_CASE("The worker is told what to call its scratch file, and not where it came from", "[dispatch]")
{
    // A compiler records the name of the file it was handed, so an object built
    // from a worker-invented name differs from a locally built one in that name
    // and nothing else -- seven bytes on clang-cl, and none once they agree.
    //
    // The DIRECTORY is deliberately not sent. The worker has no use for it and no
    // business learning where a client's checkout lives, and it could not honour it
    // if it wanted to: the file it creates is inside its own scratch directory.
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.sourceName = "/home/dev/checkout/src/Widget.cpp";
    REQUIRE(Dispatch(dialer, request).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(Wire::AsStringView(Unwrap(compile).sourceName) == "Widget.cpp");
}

TEST_CASE("A Windows-spelled source path is reduced to its base name too", "[dispatch]")
{
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.sourceName = R"(D:\checkout\src\Widget.cpp)";
    REQUIRE(Dispatch(dialer, request).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(Wire::AsStringView(Unwrap(compile).sourceName) == "Widget.cpp");
}

TEST_CASE("The preprocessed source reaches the worker intact", "[dispatch]")
{
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.preprocessed = "int answer() { return 42; }";
    REQUIRE(Dispatch(dialer, request).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    auto const envelope = Wire::DecodeCodecEnvelope(Unwrap(compile).source);
    REQUIRE(envelope.has_value());
    CHECK(Unwrap(envelope).rawLength == request.preprocessed.size());
}

TEST_CASE("A worker that accepts no codec is sent Identity", "[dispatch]")
{
    // The grant relays what the worker can decode. A worker offering nothing must
    // still receive something it can read, or the payload crosses the network only
    // to be refused.
    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply(/*codecs=*/ {}));
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    REQUIRE(Dispatch(dialer, Request(args)).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    auto const envelope = Wire::DecodeCodecEnvelope(Unwrap(compile).source);
    REQUIRE(envelope.has_value());
    CHECK(Unwrap(envelope).codec == Wire::IdentityCodec);
}

TEST_CASE("DecodeArgs refuses a truncated list rather than returning a prefix", "[dispatch]")
{
    // A partial argument list is a DIFFERENT compile from the one that was
    // authorized -- running it would produce an object nobody asked for.
    std::vector<std::byte> field(4, std::byte { 0 });
    field[3] = std::byte { 8 }; // declares an 8-byte argument that is not there
    CHECK(DecodeArgs(field).empty());
}

TEST_CASE("DecodeArgs round-trips an empty list and an empty argument", "[dispatch]")
{
    CHECK(DecodeArgs({}).empty());

    ScriptedDialer dialer;
    dialer.Serve(std::string { Scheduler }, GrantReply());
    dialer.Serve(std::string { Worker }, CompileReply("OBJ"));
    std::vector<std::string> const args { "", "-O2" };
    REQUIRE(Dispatch(dialer, Request(args)).Ran());

    auto const& toWorker = dialer.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(DecodeArgs(Unwrap(compile).args) == args);
}
