// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"
#include "StubObjectTestSupport.hpp"
#include "WorkerProtocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A runner that writes a canned object and reports success.
class StubRunner final: public IProcessRunner
{
  public:
    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        Test::WriteStubObject(argv);
        return CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }
};

struct Fixture
{
    StubRunner runner;
    FastCache::Testing::ScratchDirectory scratch { "fc-wp" };
    CompileJobRunner jobs;
    AtomicMetricsSink metrics;
    WorkerProtocol worker;

    Fixture():
        jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } },
        worker { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics }
    {
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;
};

/// A well-formed COMPILE frame.
/// A COMPILE frame carrying `source` as its source field, already enveloped.
///
/// The envelope belongs to the caller because that is exactly what the footprint
/// cases differ in -- an honest `Identity` one, one declaring an expansion it does
/// not carry, or a field too short to be an envelope at all. Everything around it
/// is boilerplate no case varies, so it lives here once.
/// @param source The source field, enveloped or not, exactly as it should travel.
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

[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13",
                                                  std::string_view source = "int main(){return 0;}")
{
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(source.size()), Wire::AsBytes(source));
    return FrameWithSource(enveloped, fingerprint);
}

/// Decode a reply frame into its status and payload.
struct Reply
{
    bool present { false };
    Wire::Status status { Wire::Status::Miss };
    std::vector<std::byte> payload;
};

[[nodiscard]] Reply Decode(std::vector<std::byte> const& frame)
{
    auto const header = Wire::DecodeReplyHeader(frame);
    if (!header.has_value())
        return {};
    auto const body = std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize);
    return Reply { .present = true, .status = header->status, .payload = { body.begin(), body.end() } };
}

[[nodiscard]] Wire::ErrorCode ErrorOf(std::vector<std::byte> const& frame)
{
    auto const reply = Decode(frame);
    auto const decoded = Wire::DecodeErrorPayload(reply.payload);
    return decoded.has_value() ? decoded->first : Wire::ErrorCode::MalformedFrame;
}

} // namespace

TEST_CASE("A worker compiles a well-formed job", "[worker-protocol]")
{
    Fixture fix;
    auto const answer = fix.worker.Answer(CompileFrame());
    REQUIRE(answer.has_value());

    auto const reply = Decode(Unwrap(answer));
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const result = Wire::DecodeCompileResult(reply.payload);
    REQUIRE(result.has_value());
    CHECK(Unwrap(result).exitCode == 0);

    auto const object = Wire::DecodeCodecEnvelope(Unwrap(result).object);
    REQUIRE(object.has_value());
    CHECK(Wire::AsStringView(Unwrap(object).bytes) == "OBJECT");
}

TEST_CASE("A worker refuses every verb but COMPILE", "[worker-protocol]")
{
    // A worker is not a scheduler and not a cache. The refusal is a REPLY, so a
    // client that sent the wrong verb to the wrong port learns which -- a dropped
    // connection is indistinguishable from a dead host.
    Fixture fix;
    auto const frames = std::vector<std::vector<std::byte>> {
        Wire::EncodeFetch("k"),
        Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-13", .key = "k", .acceptedCodecs = {} }),
        Wire::EncodeRegister(
            Wire::RegisterRequest { .fingerprint = "gcc-13", .endpoint = "h:1", .slots = 1, .acceptedCodecs = {} }),
        Wire::EncodeHeartbeat("w1", 0),
    };
    for (auto const& frame: frames)
    {
        auto const answer = fix.worker.Answer(frame);
        REQUIRE(answer.has_value());
        CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::DispatchNotPermitted);
    }
}

TEST_CASE("A codec envelope declaring more than the cap is refused before it is decompressed", "[worker-protocol]")
{
    // Issue #241. `Compression::Decompress` VALUE-INITIALIZES a buffer of the declared
    // size, so the pages are touched rather than lazily reserved -- and that size is a
    // `u32` read straight off the wire, with no enforced relation to the compressed
    // bytes beside it. A frame of a few dozen bytes therefore drove a multi-gigabyte
    // allocation, and the listener's in-flight byte budget charged only the FRAME
    // length, so it passed admission unnoticed.
    //
    // `CompileCacheWire.hpp` already said this is what `rawLen` is for: it "lets a
    // decoder reject a payload whose declared expansion exceeds its cap before
    // decompressing a byte". No decoder in the tree did.
    Fixture fix;

    // A non-Identity codec, so the payload takes the decompressing path, declaring the
    // largest expansion the field can hold.
    constexpr std::uint32_t FourGiB = 0xFFFFFFFFU;
    std::array<std::byte, 16> const payload { std::byte { 0x41 } };
    auto const bomb = Wire::EncodeCodecEnvelope(/*codec=*/1, FourGiB, payload);
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = bomb,
                                                                  .acceptedCodecs = { Wire::IdentityCodec },
                                                                  .sourceName = "a.cpp" });
    // The entire hostile frame is smaller than the header of what it asked this worker
    // to allocate. That is the amplification, stated as an assertion.
    CHECK(frame.size() < 128);

    auto const answer = fix.worker.Answer(frame);
    // A REPLY, never a close: the frame declared its own length, so the connection is
    // still synchronised and the peer learns which of its fields was refused.
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::PayloadTooLarge);

    // Nothing downstream ever saw the frame.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("An Identity envelope may not lie about the size of the bytes beside it", "[worker-protocol]")
{
    // The Identity path never reaches `Decompress`, so it never reached that
    // function's own length check either -- it could declare any size at all next to
    // any payload. That is not an allocation, but it is a field describing bytes it
    // does not describe, and the next receiver to believe it is the next defect.
    Fixture fix;

    std::array<std::byte, 16> const payload { std::byte { 0x41 } };
    auto const liar = Wire::EncodeCodecEnvelope(Wire::IdentityCodec, 0xFFFFFFFFU, payload);
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = liar,
                                                                  .acceptedCodecs = { Wire::IdentityCodec },
                                                                  .sourceName = "a.cpp" });
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::PayloadTooLarge);

    // A modest overstatement is refused too, and as a different fact: it is within the
    // cap, so what is wrong with it is the disagreement rather than the size -- and it
    // is answered `malformed-frame`, not `unsupported-codec`. The codec was never in
    // question, and a refusal whose code and message disagree sends an operator
    // hunting a codec mismatch that never happened.
    auto const modest = Wire::EncodeCodecEnvelope(Wire::IdentityCodec, 64, payload);
    auto const modestFrame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                        .fingerprint = "gcc-13",
                                                                        .args = {},
                                                                        .source = modest,
                                                                        .acceptedCodecs = { Wire::IdentityCodec },
                                                                        .sourceName = "a.cpp" });
    auto const modestAnswer = fix.worker.Answer(modestFrame);
    REQUIRE(modestAnswer.has_value());
    CHECK(ErrorOf(Unwrap(modestAnswer)) == Wire::ErrorCode::MalformedFrame);
}

TEST_CASE("An envelope refusal's wire code and its message are one fact", "[worker-protocol]")
{
    // They come from one table row, never a ternary beside a lookup. This was a
    // ternary, and it answered `unsupported-codec` for a malformed envelope while the
    // message said "malformed" -- a refusal that sends an operator hunting a codec
    // mismatch that never happened. The rule this file already states for
    // `RefusalTable` applies one layer in.
    CHECK(WireCodeFor(EnvelopeError::Malformed) == Wire::ErrorCode::MalformedFrame);
    CHECK(WireCodeFor(EnvelopeError::UnsupportedCodec) == Wire::ErrorCode::UnsupportedCodec);
    CHECK(WireCodeFor(EnvelopeError::DeclaredTooLarge) == Wire::ErrorCode::PayloadTooLarge);
    CHECK(WireCodeFor(EnvelopeError::Corrupt) == Wire::ErrorCode::MalformedFrame);

    // Every reason says something, and no two say the same thing -- a description
    // shared between reasons is one an operator cannot act on.
    std::vector<std::string_view> seen;
    for (auto const reason: { EnvelopeError::Malformed,
                              EnvelopeError::UnsupportedCodec,
                              EnvelopeError::DeclaredTooLarge,
                              EnvelopeError::Corrupt })
    {
        auto const text = DescribeEnvelopeError(reason);
        CHECK_FALSE(text.empty());
        CHECK(std::ranges::find(seen, text) == seen.end());
        seen.push_back(text);
    }
}

TEST_CASE("The envelope ceiling is the surface's own, not a figure this class assumed", "[worker-protocol]")
{
    // Passed in rather than baked in: this class never sees the listener that enforced
    // the frame length, so a listener with a smaller request cap has to say so or the
    // two disagree about how much memory one request may cost.
    StubRunner runner;
    FastCache::Testing::ScratchDirectory const scratch { "fc-wp" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };
    AtomicMetricsSink metrics;
    constexpr std::size_t TinyCap = 8;
    WorkerProtocol worker {
        jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec }, metrics, TinyCap
    };

    // Well under the default ceiling, and over this worker's.
    auto const answer = worker.Answer(CompileFrame("gcc-13", "int main(){return 0;}"));
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::PayloadTooLarge);
}

TEST_CASE("A foreign magic is the one case that closes rather than replies", "[worker-protocol]")
{
    // There is no framing in which a reply would be meaningful to a peer that is
    // not speaking this protocol.
    Fixture fix;
    std::vector<std::byte> const junk { std::byte { 'G' }, std::byte { 'E' }, std::byte { 'T' } };
    CHECK_FALSE(fix.worker.Answer(junk).has_value());
}

TEST_CASE("An unauthorized lease is refused before the payload is even decoded", "[worker-protocol]")
{
    // Checked before decompression, let alone compilation: an unauthorized peer
    // must not be able to make this worker do the expensive part.
    StubRunner runner;
    FastCache::Testing::ScratchDirectory const scratch { "fc-wp-deny" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };
    AtomicMetricsSink metrics;
    WorkerProtocol worker {
        jobs, [](std::string_view, std::string_view) { return false; }, { Wire::IdentityCodec }, metrics
    };

    auto const answer = worker.Answer(CompileFrame());
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::UnknownLease);
}

TEST_CASE("A fingerprint this worker does not serve is refused as a mismatch", "[worker-protocol]")
{
    // A distinct code, because it is a distinct operator problem: the scheduler
    // sent a job to the wrong fleet, or somebody reached this port directly.
    Fixture fix;
    auto const answer = fix.worker.Answer(CompileFrame("clang-19"));
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::FingerprintMismatch);
}

TEST_CASE("A source in an undecodable codec is refused, not compiled as garbage", "[worker-protocol]")
{
    Fixture fix;
    auto const bogus = Wire::EncodeCodecEnvelope(/*codec=*/200, 10, Wire::AsBytes("xxxxxxxxxx"));
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = bogus,
                                                                  .acceptedCodecs = {},
                                                                  .sourceName = "a.cpp" });
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::UnsupportedCodec);
}

TEST_CASE("A frame shorter than its declared payload is refused", "[worker-protocol]")
{
    // The declared length is what makes the framing work; a frame that lies about
    // it must not be read past its end.
    Fixture fix;
    auto frame = CompileFrame();
    frame.resize(Wire::RequestHeaderSize + 2);
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::MalformedFrame);
}

TEST_CASE("An unsupported wire version is refused with a version error", "[worker-protocol]")
{
    Fixture fix;
    auto frame = CompileFrame();
    frame[1] = std::byte { 99 };
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::UnsupportedVersion);
}

TEST_CASE("An unknown opcode is refused and does not close the connection", "[worker-protocol]")
{
    Fixture fix;
    auto frame = CompileFrame();
    frame[2] = std::byte { 0x7F };
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::UnknownOpcode);
}

TEST_CASE("A compile is counted from start to finish", "[worker-protocol][metrics]")
{
    // Started and completed are two counters rather than a gauge, because this
    // sink is counter-only by design: their difference is how many compiles are
    // running, and the completed count is also the divisor for the wall-time sum.
    Fixture fix;
    using Sink = IMetricsSink::Counter;

    REQUIRE(fix.worker.Answer(CompileFrame()).has_value());

    CHECK(fix.metrics.Read(Sink::WorkerJobsStarted) == 1);
    CHECK(fix.metrics.Read(Sink::WorkerJobsCompleted) == 1);

    // No refusal was counted anywhere. A job that ran must not also appear as one
    // that did not, which is what a counter incremented on every exit path does.
    for (auto const counter: { Sink::WorkerJobsRefusedUnknownFingerprint,
                               Sink::WorkerJobsRefusedRejectedArgument,
                               Sink::WorkerJobsRefusedScratchUnavailable,
                               Sink::WorkerJobsRefusedSpawnFailed })
        CHECK(fix.metrics.Read(counter) == 0);
}

TEST_CASE("A refusal is counted under its own reason", "[worker-protocol][metrics]")
{
    // The split is the whole point, exactly as the scheduler's no-worker /
    // no-capacity split is: a fingerprint nobody serves says the fleet is
    // misconfigured, and a scratch disk that will not take a file says a machine
    // is broken. Summing them hides the first behind the second.
    Fixture fix;
    using Sink = IMetricsSink::Counter;

    REQUIRE(fix.worker.Answer(CompileFrame("clang-19")).has_value());

    CHECK(fix.metrics.Read(Sink::WorkerJobsRefusedUnknownFingerprint) == 1);
    CHECK(fix.metrics.Read(Sink::WorkerJobsRefusedScratchUnavailable) == 0);
    CHECK(fix.metrics.Read(Sink::WorkerJobsRefusedSpawnFailed) == 0);

    // The job started -- the worker took it -- but did not complete, so the
    // in-flight difference still returns to zero and the wall-time sum is
    // untouched by a compile that never ran.
    CHECK(fix.metrics.Read(Sink::WorkerJobsStarted) == 1);
    CHECK(fix.metrics.Read(Sink::WorkerJobsCompleted) == 0);
    CHECK(fix.metrics.Read(Sink::WorkerCompileMillisTotal) == 0);
}

TEST_CASE("A refusal's counter and its wire code come from one row", "[worker-protocol][metrics]")
{
    // Both halves of the same refusal, asserted together: the client is told
    // `fingerprint-mismatch` and the operator sees the fingerprint counter rise.
    // Two switches could answer these differently and still compile, which is why
    // they are one table.
    Fixture fix;

    auto const answer = fix.worker.Answer(CompileFrame("clang-19"));
    REQUIRE(answer.has_value());

    auto const reply = Decode(Unwrap(answer));
    CHECK(reply.status == Wire::Status::Error);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint) == 1);
}
TEST_CASE("A request's declared footprint is what its envelope asks for", "[worker-protocol]")
{
    // What an admitting surface charges a request, and the number that was missing:
    // a frame's own `payloadLength` is the COMPRESSED length, while the envelope one
    // layer in declares the buffer `Unenvelope` will size and value-initialize. A
    // hundred bytes may ask for 256 MiB, so a budget reading the frame length admits
    // as many of them as there are slots (#241).
    //
    // Everything that is not a decodable COMPILE answers the frame length instead:
    // `Answer` refuses each of those on its own terms and none of them ever reaches a
    // decoder, so charging them anything more would turn a malformed frame into a
    // busy signal.
    constexpr std::uint32_t Declared = 64U * 1024U * 1024U;

    SECTION("an envelope declaring a large expansion is charged that expansion")
    {
        auto const enveloped = Wire::EncodeCodecEnvelope(0xFE, Declared, Wire::AsBytes(std::string_view { "xx" }));
        auto const frame = FrameWithSource(enveloped);
        CHECK(frame.size() < 256U);
        CHECK(DeclaredRequestFootprint(frame) == Declared);
    }

    SECTION("an ordinary compile is charged its frame length")
    {
        // The envelope declares its own true size, which is smaller than the payload
        // carrying it, so the frame length is the larger of the two.
        auto const frame = CompileFrame();
        CHECK(DeclaredRequestFootprint(frame) == frame.size() - Wire::RequestHeaderSize);
    }

    SECTION("a foreign verb is charged its frame length")
    {
        auto frame = CompileFrame();
        frame[2] = std::byte { 0x7F };
        CHECK(DeclaredRequestFootprint(frame) == frame.size() - Wire::RequestHeaderSize);
    }

    SECTION("a frame shorter than it claims is charged what it claimed")
    {
        auto const whole = CompileFrame();
        auto truncated = whole;
        truncated.resize(Wire::RequestHeaderSize);
        CHECK(DeclaredRequestFootprint(truncated) == whole.size() - Wire::RequestHeaderSize);
    }

    SECTION("a source field too short to be an envelope is charged the frame length")
    {
        auto const frame = FrameWithSource({});
        // Zero bytes cannot hold a codec byte and a `u32`, so the envelope decode is
        // the one below `SplitFields` that still fails here.
        CHECK(DeclaredRequestFootprint(frame) == frame.size() - Wire::RequestHeaderSize);
    }

    SECTION("a frame this protocol does not speak at all is charged nothing")
    {
        std::vector<std::byte> const junk(Wire::RequestHeaderSize, std::byte { 'G' });
        CHECK(DeclaredRequestFootprint(junk) == 0);
    }
}
