// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// An exchange that answers from a scripted peer per endpoint, and records which
/// endpoints were reached, in what order, and under which budget.
///
/// The peer is driven with `SyncRun` because a `ScriptedPeer`'s awaitables resolve
/// inline and so never leave the task suspended -- the precondition `SyncRun`
/// states. That keeps the framing under test: these cases assert what went ON the
/// wire, which an exchange double returning a hand-built `CacheOutcome` could not.
class ScriptedFleet final: public IEndpointExchange
{
  public:
    /// Register a peer for `endpoint`. An endpoint with no entry is unreachable,
    /// which is how the "worker is down" cases are written.
    void Serve(std::string endpoint, std::vector<std::byte> replies)
    {
        _scripts.emplace(std::move(endpoint), std::move(replies));
    }

    /// Stop answering `endpoint` after `dials` connections.
    ///
    /// The peer that was there and is not any more -- a scheduler that restarted or
    /// lost leadership while a compile ran. Reachability has to change *during* a
    /// dispatch to be that case at all, which is why it is a count rather than a
    /// registration the test could simply omit.
    void ReachableFor(std::string endpoint, std::size_t dials)
    {
        _limits.insert_or_assign(std::move(endpoint), dials);
    }

    /// Make `endpoint` take `duration` to answer.
    ///
    /// A worker writes nothing until the compiler has finished, so how long a
    /// remote compile takes is the whole of what the budget has to cover. An
    /// exchange given less than this gets what a real one would: the deadline
    /// closes the socket, the reply never arrives, and the outcome is a transport
    /// failure. No clock is advanced -- the duration is a property of the scripted
    /// peer, and comparing it against the budget is the whole model.
    void AnswersAfter(std::string endpoint, std::chrono::milliseconds duration)
    {
        _durations.insert_or_assign(std::move(endpoint), duration);
    }

    CacheOutcome Exchange(std::string_view hostPort,
                          std::vector<std::byte> frame,
                          Credential const& credential,
                          ExchangeBudget budget) override
    {
        auto const key = std::string { hostPort };
        _dialled.push_back(key);
        _budgets.push_back(budget);
        auto const it = _scripts.find(key);
        if (it == _scripts.end())
            return CacheOutcome {};
        if (auto const limit = _limits.find(key); limit != _limits.end())
        {
            if (limit->second == 0)
                return CacheOutcome {};
            --limit->second;
        }
        if (auto const slow = _durations.find(key); slow != _durations.end())
            if (budget.BoundsTotal() && budget.total < slow->second)
                return CacheOutcome {};
        ScriptedPeer peer { it->second, &_sent[key] };
        return SyncRun(ExchangeFramed(&peer, std::move(frame), credential));
    }

    /// Endpoints dialled, in order. The ORDER is the assertion in several cases:
    /// the client must ask the scheduler and then go where it was sent, never
    /// guess at a worker.
    [[nodiscard]] std::vector<std::string> const& Dialled() const noexcept
    {
        return _dialled;
    }

    /// The budget each exchange ran under, in the same order as `Dialled()`.
    [[nodiscard]] std::vector<ExchangeBudget> const& Budgets() const noexcept
    {
        return _budgets;
    }

    /// Everything written to `endpoint`.
    [[nodiscard]] std::vector<std::byte> const& SentTo(std::string const& endpoint)
    {
        return _sent[endpoint];
    }

  private:
    std::vector<std::string> _dialled;
    std::vector<ExchangeBudget> _budgets;
    std::map<std::string, std::vector<std::byte>> _sent;
    std::map<std::string, std::vector<std::byte>> _scripts;
    std::map<std::string, std::size_t> _limits;
    std::map<std::string, std::chrono::milliseconds> _durations;
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

/// The request frames written to `endpoint`, split apart.
///
/// A connection here carries more than one request now -- a LEASE and, later, the
/// RELEASE that resolves it -- so a test that decoded the buffer from offset zero
/// would only ever see the first.
/// @param fleet The scripted fleet that recorded them.
/// @param endpoint Whose connection to read.
/// @return One span per whole frame, in the order they were written.
[[nodiscard]] std::vector<std::span<std::byte const>> FramesTo(ScriptedFleet& fleet, std::string_view endpoint)
{
    auto const& bytes = fleet.SentTo(std::string { endpoint });
    std::vector<std::span<std::byte const>> frames;
    for (std::size_t offset = 0; offset + Wire::RequestHeaderSize <= bytes.size();)
    {
        auto const rest = std::span<std::byte const> { bytes }.subspan(offset);
        auto const header = Wire::DecodeRequestHeader(rest);
        if (!header.has_value())
            break;
        auto const whole = Wire::RequestHeaderSize + header->payloadLength;
        if (whole > rest.size())
            break;
        frames.push_back(rest.subspan(0, whole));
        offset += whole;
    }
    return frames;
}

/// The opcode a request frame declares.
/// @param frame A whole request frame.
/// @return Its op, or nullopt when the header does not decode.
[[nodiscard]] std::optional<Wire::Op> OpOf(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        return std::nullopt;
    auto const* const descriptor = Wire::FindOp(header->opRaw);
    return descriptor != nullptr ? std::optional { descriptor->code } : std::nullopt;
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
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJECTBYTES"));

    std::vector<std::string> const args { "-O2", "-std=c++23" };
    auto const result = Dispatch(fleet, Request(args));

    REQUIRE(result.Ran());
    CHECK(result.exitCode == 0);
    CHECK(std::string(reinterpret_cast<char const*>(result.object.data()), result.object.size()) == "OBJECTBYTES");
    CHECK(result.workerEndpoint == Worker);

    // The scheduler is asked first, then the worker it named -- the client never
    // guesses at an endpoint -- and then the scheduler again, to hand the lease
    // back. A FRESH connection for that, not the one the grant arrived on: the
    // scheduler sweeps a connection idle for five seconds and a compile takes
    // longer, so reusing it would fail exactly when there was something to release.
    REQUIRE(fleet.Dialled().size() == 3);
    CHECK(fleet.Dialled()[0] == Scheduler);
    CHECK(fleet.Dialled()[1] == Worker);
    CHECK(fleet.Dialled()[2] == Scheduler);
}

TEST_CASE("A compile longer than the cache deadline is still dispatched", "[dispatch]")
{
    // Issue #223. The launcher armed ONE deadline for every exchange it made, and
    // the dispatch legs got the cache's. A cache exchange is answered out of memory,
    // so ten seconds is right there; a worker writes nothing until the compiler has
    // finished, so the client sits in one read for the whole compile. Every
    // translation unit taking longer than ten seconds was therefore abandoned and
    // rebuilt locally -- precisely the ones distribution exists for -- while the
    // worker ran the job to completion and wrote back an object nobody read.
    using namespace std::chrono_literals;

    // Well over a minute, which is what the translation units this feature exists
    // for actually cost here -- and what a ceiling merely bigger than the cache's
    // would still have cut off. #223 measured 23.5 s on one; the slow ones are
    // slower than that, and the default has to clear them by a wide margin rather
    // than by an argument about averages.
    constexpr auto RemoteCompileTime = 90s;

    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJECTBYTES"));
    fleet.AnswersAfter(std::string { Worker }, RemoteCompileTime);
    std::vector<std::string> const args { "-O2" };

    SECTION("under the default budgets")
    {
        auto const result = Dispatch(fleet, Request(args));
        REQUIRE(result.Ran());
        CHECK(std::string(reinterpret_cast<char const*>(result.object.data()), result.object.size()) == "OBJECTBYTES");
    }

    SECTION("but not when the compile leg is handed the cache's budget")
    {
        // The defect itself, written as a configuration: one budget for all three
        // legs. Without this the case above would still pass with the compile leg
        // simply never bounded, and the assertion would be about nothing.
        DispatchBudgets const shared { .control = ExchangeBudget {}, .compile = ExchangeBudget {} };
        auto const result = Dispatch(fleet, Request(args), shared);

        CHECK(result.status == DispatchStatus::Unavailable);
        CHECK(result.detail.contains(Worker));
        // And the lease was still handed back, so the key is not pinned for the
        // scheduler's whole lease timeout on the way out (#212).
        REQUIRE(fleet.Dialled().size() == 3);
        CHECK(fleet.Dialled()[2] == Scheduler);
    }
}

TEST_CASE("A slow cache does not get the compile's minutes", "[dispatch]")
{
    // The other half of the same rule. The scheduler's LEASE and RELEASE are short
    // request/reply verbs answered from its own tables, so they keep the launcher's
    // ordinary budget: raising the compile ceiling to two minutes must not make a
    // wedged scheduler hold a build for two minutes as well.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    REQUIRE(Dispatch(fleet, Request(args)).Ran());

    // Lease, compile, release -- in that order, so the budgets line up with them.
    REQUIRE(fleet.Budgets().size() == 3);
    DispatchBudgets const defaults;
    CHECK(fleet.Budgets()[0].total == defaults.control.total);
    CHECK(fleet.Budgets()[1].total == defaults.compile.total);
    CHECK(fleet.Budgets()[2].total == defaults.control.total);
    // Not merely different: the compile's is the longer one, and both are bounded.
    CHECK(defaults.control.total < defaults.compile.total);
    CHECK(defaults.compile.total == DefaultDispatchTotal);
    // And it clears a real slow translation unit by a wide margin rather than by a
    // hair. A default sized just above the worst one anybody has measured is the
    // same defect one release later, so the assertion is about the MARGIN.
    CHECK(DefaultDispatchTotal >= std::chrono::minutes { 5 });
}

TEST_CASE("A failing remote compile is a successful dispatch", "[dispatch]")
{
    // The distinction the whole result type turns on: the compiler RAN and rejected
    // the code. That is not a dispatch failure, and reporting it as one would send
    // the caller down the "the cache let us down" path instead of showing the user
    // their own compile error.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("", 1, "error: no"));

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

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
        ScriptedFleet fleet;
        fleet.Serve(std::string { Scheduler }, Wire::EncodeErrorReply(code, {}));

        std::vector<std::string> const args { "-O2" };
        auto const result = Dispatch(fleet, Request(args));

        INFO("error code " << static_cast<unsigned>(code));
        CHECK(result.status == DispatchStatus::Declined);
        CHECK_FALSE(result.Ran());
        CHECK_FALSE(result.detail.empty());
        // The worker is never dialled when there is no grant.
        CHECK(fleet.Dialled().size() == 1);
    }
}

TEST_CASE("An unreachable scheduler is unavailable, and no worker is dialled", "[dispatch]")
{
    ScriptedFleet fleet; // nothing registered
    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

    CHECK(result.status == DispatchStatus::Unavailable);
    CHECK(result.detail.contains("scheduler"));
    CHECK(fleet.Dialled().size() == 1);
}

TEST_CASE("An unreachable worker is unavailable and names the endpoint", "[dispatch]")
{
    // The scheduler granted a lease pointing at a machine that is not there. Naming
    // it is the difference between an operator finding a dead node and an operator
    // seeing "distribution stopped working".
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

    CHECK(result.status == DispatchStatus::Unavailable);
    CHECK(result.detail.contains(Worker));
}

TEST_CASE("A worker refusing the job is a decline, not a compile", "[dispatch]")
{
    // An unknown lease, a fingerprint it does not have, an argument it will not
    // accept. Distinct from the compiler running and rejecting the code.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, Wire::EncodeErrorReply(Wire::ErrorCode::UnknownLease, {}));

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

    CHECK(result.status == DispatchStatus::Declined);
    CHECK_FALSE(result.Ran());
    CHECK(result.detail.contains("unknown-lease"));
}

TEST_CASE("The lease is handed back however the job ended", "[dispatch]")
{
    // The client is the party the lease was issued to, and every branch below is a
    // way its job can end. Before there was a verb for this the key stayed marked
    // in-flight for the scheduler's whole lease timeout -- ten minutes -- so
    // recompiling the same translation unit inside that window was refused
    // `already-in-flight` and fell back to a local compile (#212).
    //
    // Asserted per outcome rather than once, because "somebody added a branch and
    // forgot the release" is exactly the regression the shape of `Dispatch` was
    // changed to prevent.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    std::vector<std::string> const args { "-O2" };

    SECTION("after a compile that ran")
    {
        fleet.Serve(std::string { Worker }, CompileReply("OBJ"));
        REQUIRE(Dispatch(fleet, Request(args)).Ran());
    }
    SECTION("after the worker refused the job")
    {
        fleet.Serve(std::string { Worker }, Wire::EncodeErrorReply(Wire::ErrorCode::UnknownLease, {}));
        REQUIRE(Dispatch(fleet, Request(args)).status == DispatchStatus::Declined);
    }
    SECTION("after a worker that was not there at all")
    {
        // The case the old comment argued was not worth a verb -- "a client that
        // cannot reach the worker is exactly the client least able to report
        // anything about it". It is holding the token, nothing was compiled, and
        // the key is pinned until it says so.
        REQUIRE(Dispatch(fleet, Request(args)).status == DispatchStatus::Unavailable);
    }
    SECTION("after a result it could not decode")
    {
        fleet.Serve(std::string { Worker }, Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes("junk")));
        REQUIRE(Dispatch(fleet, Request(args)).status == DispatchStatus::Unavailable);
    }

    auto const frames = FramesTo(fleet, Scheduler);
    REQUIRE(frames.size() == 2);
    CHECK(OpOf(frames[0]) == Wire::Op::Lease);
    REQUIRE(OpOf(frames[1]) == Wire::Op::Release);

    // Naming the token the grant carried, not some other one: a release that names
    // nothing the scheduler issued is refused and resolves no key at all.
    auto const resolved = Wire::DecodeReleasePayload(frames[1].subspan(Wire::RequestHeaderSize));
    REQUIRE(resolved.has_value());
    CHECK(Wire::AsStringView(Unwrap(resolved).leaseToken) == "l1");
    // And the key it was granted on, which is what makes the release resolve THIS
    // client's lease rather than whichever a restarted scheduler reissued the
    // number to.
    CHECK(Wire::AsStringView(Unwrap(resolved).key) == "objkey");
}

TEST_CASE("A scheduler that has gone away by then changes nothing", "[dispatch]")
{
    // The release is best effort and must stay that way: the scheduler restarting
    // or losing leadership during a compile is ordinary, the lease expires on its
    // own, and a dispatch that had already succeeded must not be reported as a
    // failure because of what happened after it.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJECTBYTES"));
    fleet.ReachableFor(std::string { Scheduler }, 1);

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

    REQUIRE(result.Ran());
    CHECK(std::string(reinterpret_cast<char const*>(result.object.data()), result.object.size()) == "OBJECTBYTES");
    CHECK(result.detail.empty());
    // It did try, which is the other half: a silent skip would look the same here.
    REQUIRE(fleet.Dialled().size() == 3);
    CHECK(fleet.Dialled()[2] == Scheduler);
}

TEST_CASE("A malformed grant or result is unavailable, never a silent success", "[dispatch]")
{
    SECTION("grant")
    {
        ScriptedFleet fleet;
        fleet.Serve(std::string { Scheduler }, Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes("not-a-grant")));
        std::vector<std::string> const args { "-O2" };
        CHECK(Dispatch(fleet, Request(args)).status == DispatchStatus::Unavailable);
    }
    SECTION("result")
    {
        ScriptedFleet fleet;
        fleet.Serve(std::string { Scheduler }, GrantReply());
        fleet.Serve(std::string { Worker }, Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes("junk")));
        std::vector<std::string> const args { "-O2" };
        CHECK(Dispatch(fleet, Request(args)).status == DispatchStatus::Unavailable);
    }
}

TEST_CASE("A worker's reply may not declare an expansion above the launcher's ceiling", "[dispatch]")
{
    // The other half of issue #241, and the reason the guard is ONE function: this
    // path and the worker's were copies of each other, so a guard added to one would
    // have been half a fix and the two would have had to agree forever after.
    //
    // The launcher dialled a worker the SCHEDULER named, which is not the same as a
    // worker this process trusts with its address space: a rogue or compromised fleet
    // member answers this exchange too, and `Decompress` value-initializes whatever
    // the reply declares.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());

    constexpr std::uint32_t FourGiB = 0xFFFFFFFFU;
    std::array<std::byte, 16> const payload { std::byte { 0x41 } };
    auto const bomb = Wire::EncodeCodecEnvelope(/*codec=*/1, FourGiB, payload);
    fleet.Serve(std::string { Worker },
                Wire::EncodeReply(Wire::Status::Ok,
                                  Wire::EncodeCompileResult(Wire::CompileResult {
                                      .exitCode = 0, .object = bomb, .stdoutText = {}, .stderrText = {} })));

    std::vector<std::string> const args { "-O2" };
    auto const result = Dispatch(fleet, Request(args));

    // Unavailable, never Compiled: a reply this process refused to open is not an
    // object, and the launcher answers it the way it answers every dispatch failure --
    // by compiling locally.
    CHECK(result.status == DispatchStatus::Unavailable);
    CHECK(result.object.empty());
    // The reason travels and names the endpoint, because "distribution stopped
    // helping" is otherwise a whole investigation.
    CHECK(result.detail.contains(Worker));
    CHECK(result.detail.contains("declared decompressed size"));
}

TEST_CASE("The launcher's envelope ceiling is a budget the caller can set", "[dispatch]")
{
    // A byte budget beside the two time budgets, bounding the same thing they do:
    // what one exchange may cost this process.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJECT"));

    std::vector<std::string> const args { "-O2" };

    // Default ceiling: an ordinary object comes back.
    CHECK(Dispatch(fleet, Request(args)).status == DispatchStatus::Compiled);

    // A ceiling below this object's size refuses it, so the figure is genuinely the
    // caller's rather than a constant baked into the decoder.
    ScriptedFleet tight;
    tight.Serve(std::string { Scheduler }, GrantReply());
    tight.Serve(std::string { Worker }, CompileReply("OBJECT"));
    DispatchBudgets budgets;
    budgets.maxDecompressedBytes = 2;
    CHECK(Dispatch(tight, Request(args), budgets).status == DispatchStatus::Unavailable);
}

TEST_CASE("The arguments the worker receives are the ones it was given", "[dispatch]")
{
    // Round-tripped through the wire encoding, because an argument containing a
    // space is the case a joined-string encoding would silently split.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2", "-DMSG=hello world", "-std=c++23" };
    REQUIRE(Dispatch(fleet, Request(args)).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
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
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.sourceName = "/home/dev/checkout/src/Widget.cpp";
    REQUIRE(Dispatch(fleet, request).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(Wire::AsStringView(Unwrap(compile).sourceName) == "Widget.cpp");
}

TEST_CASE("A Windows-spelled source path is reduced to its base name too", "[dispatch]")
{
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.sourceName = R"(D:\checkout\src\Widget.cpp)";
    REQUIRE(Dispatch(fleet, request).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(Wire::AsStringView(Unwrap(compile).sourceName) == "Widget.cpp");
}

TEST_CASE("The preprocessed source reaches the worker intact", "[dispatch]")
{
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    auto request = Request(args);
    request.preprocessed = "int answer() { return 42; }";
    REQUIRE(Dispatch(fleet, request).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
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
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply(/*codecs=*/ {}));
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));

    std::vector<std::string> const args { "-O2" };
    REQUIRE(Dispatch(fleet, Request(args)).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
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

    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJ"));
    std::vector<std::string> const args { "", "-O2" };
    REQUIRE(Dispatch(fleet, Request(args)).Ran());

    auto const& toWorker = fleet.SentTo(std::string { Worker });
    auto const compile =
        Wire::DecodeCompilePayload(std::span<std::byte const> { toWorker }.subspan(Wire::RequestHeaderSize));
    REQUIRE(compile.has_value());
    CHECK(DecodeArgs(Unwrap(compile).args) == args);
}

TEST_CASE("A lease refused with NotLeader is retried against the leader it names", "[dispatch][redirect]")
{
    // Issue #237. `SchedulerService` already answers a non-leader with the leader's
    // endpoint, and this client threw it away: `NotLeader` fell into the same branch
    // as `NoWorker` and `NoCapacity`, so one election took every launcher out of
    // distribution until somebody re-pointed them by hand.
    //
    // TWO schedulers, because a fixture with one cannot fail. If the first endpoint
    // contacted is already the leader, a build that follows no redirect at all still
    // passes -- so what this pins is that the SECOND scheduler is reached, and that
    // the client goes where it was sent rather than guessing at an endpoint.
    constexpr std::string_view Leader = "leader:6675";

    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, Leader));
    fleet.Serve(std::string { Leader }, GrantReply());
    fleet.Serve(std::string { Worker }, CompileReply("OBJECTBYTES"));

    std::array<std::string, 1> const args { "-c" };
    auto const result = Dispatch(fleet, Request(args), DispatchBudgets {}, Credential {}, {});

    REQUIRE(result.status == DispatchStatus::Compiled);
    CHECK(Wire::AsStringView(result.object) == "OBJECTBYTES");

    // The order is the assertion: the demoted scheduler first, then the one it
    // named, then the worker that one granted.
    REQUIRE(fleet.Dialled().size() >= 3);
    CHECK(fleet.Dialled()[0] == Scheduler);
    CHECK(fleet.Dialled()[1] == Leader);
    CHECK(fleet.Dialled()[2] == Worker);

    // And the lease is resolved against the scheduler that ISSUED it. Releasing to
    // the configured endpoint would leave the key marked in flight on the machine
    // that actually holds it for the whole lease timeout -- the rulebook's "a resolve
    // answers on liveness" reached from the client's side.
    auto const toLeader = FramesTo(fleet, Leader);
    REQUIRE(toLeader.size() == 2);
    CHECK(OpOf(toLeader[0]) == Wire::Op::Lease);
    CHECK(OpOf(toLeader[1]) == Wire::Op::Release);

    // Nothing beyond the refusal is sent to the demoted one.
    auto const toDemoted = FramesTo(fleet, Scheduler);
    REQUIRE(toDemoted.size() == 1);
    CHECK(OpOf(toDemoted[0]) == Wire::Op::Lease);
}

TEST_CASE("A NotLeader naming no address is a refusal, not somewhere to dial", "[dispatch][redirect]")
{
    // An election in progress. `SchedulerService` answers `NotLeader` with nothing
    // when it knows of no leader -- but "nothing" does not reach the wire as an empty
    // string: the error table substitutes its default sentence, so this arrives
    // looking exactly like a redirect until somebody tries to split it.
    //
    // A client that dialled the message would report a scheduler endpoint no operator
    // ever typed, which is the failure `ClusterAdminCli`'s comment records. Here it
    // must decline and compile locally, having dialled nobody else.
    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler },
                Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "the cluster has no leader right now"));

    std::array<std::string, 1> const args { "-c" };
    auto const result = Dispatch(fleet, Request(args), DispatchBudgets {}, Credential {}, {});

    CHECK(result.status == DispatchStatus::Declined);
    // Exactly one dial: the configured scheduler, and nothing invented from prose.
    CHECK(fleet.Dialled() == std::vector<std::string> { std::string { Scheduler } });
}

TEST_CASE("Two schedulers naming each other stop at the redirect ceiling", "[dispatch][redirect]")
{
    // A partition healing, or a stale `_knownLeader` on both sides: each node is
    // certain the other leads. The chain is not this client's to trust, so it is
    // bounded -- without a ceiling a build would spend one connect per translation
    // unit per hop discovering that nobody leads.
    //
    // The ceiling is what makes this an ordinary local compile, which is what every
    // other lease refusal already means. Asserted as a COUNT rather than "it
    // returned", because a fix that stopped after one hop and a fix that never
    // followed one at all both return `Declined`.
    constexpr std::string_view Other = "other:6675";

    ScriptedFleet fleet;
    fleet.Serve(std::string { Scheduler }, Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, Other));
    fleet.Serve(std::string { Other }, Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, Scheduler));

    std::array<std::string, 1> const args { "-c" };
    auto const result = Dispatch(fleet, Request(args), DispatchBudgets {}, Credential {}, {});

    CHECK(result.status == DispatchStatus::Declined);
    // Three asks: the configured one, then two redirects, then the ceiling. It must
    // both follow more than one hop and stop.
    CHECK(fleet.Dialled().size() == 3);
    CHECK(fleet.Dialled()[0] == Scheduler);
    CHECK(fleet.Dialled()[1] == Other);
    CHECK(fleet.Dialled()[2] == Scheduler);

    // Nothing was leased, so nothing is released: a RELEASE here would resolve a
    // token no scheduler ever issued.
    for (auto const& endpoint: { Scheduler, Other })
        for (auto const& frame: FramesTo(fleet, endpoint))
            CHECK(OpOf(frame) == Wire::Op::Lease);
}
