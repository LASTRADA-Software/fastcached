// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"
#include "StubObjectTestSupport.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
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
///
/// The object's bytes are settable because the reply's envelope depends on them:
/// the negotiation falls back to `Identity` for anything compression does not
/// actually shrink, so a case about compression needs an object that compresses and
/// a case about the fallback needs one that does not.
class StubRunner final: public IProcessRunner
{
  public:
    std::string object { "OBJECT" }; ///< What the fake compiler writes.

    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        Test::WriteStubObject(argv, object);
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

    /// @param codecs What this worker can produce and decode; the production node
    ///        passes `AvailableCodecs()`, and a case can narrow it to assert what a
    ///        worker without a codec answers.
    explicit Fixture(Wire::CodecList codecs = AvailableCodecs()):
        jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } },
        worker { jobs, [](std::string_view, std::string_view) { return true; }, std::move(codecs), metrics }
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
/// The envelope belongs to the caller because that is exactly what the footprint
/// cases differ in -- an honest `Identity` one, one declaring an expansion it does
/// not carry, or a field too short to be an envelope at all. Everything around it
/// is boilerplate no case varies, so it lives here once.
/// @param source The source field, enveloped or not, exactly as it should travel.
/// @param fingerprint The toolchain to claim.
/// @param accepted What the client says it can decode; what the reply is negotiated
///        against.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> FrameWithSource(std::span<std::byte const> source,
                                                     std::string_view fingerprint = "gcc-13",
                                                     Wire::CodecList accepted = { Wire::IdentityCodec })
{
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = source,
                                                      .acceptedCodecs = std::move(accepted),
                                                      .sourceName = "a.cpp" });
}

/// A well-formed COMPILE frame.
/// @param fingerprint The toolchain the client claims.
/// @param source The preprocessed translation unit, sent verbatim.
/// @param accepted What the client says it can decode; what the reply is negotiated
///        against.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13",
                                                  std::string_view source = "int main(){return 0;}",
                                                  Wire::CodecList accepted = { Wire::IdentityCodec })
{
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(source.size()), Wire::AsBytes(source));
    return FrameWithSource(enveloped, fingerprint, std::move(accepted));
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

/// A successful reply's object field, envelope header and all.
///
/// Owns its bytes rather than mirroring `Wire::CodecEnvelope`, whose `bytes` is a
/// span into the frame it was decoded from: returning that by value out of a helper
/// that decoded a local would dangle the moment the caller read it.
struct ObjectField
{
    std::uint8_t codec { Wire::IdentityCodec }; ///< The codec the worker answered in.
    std::uint32_t rawLength { 0 };              ///< The size the worker declared, before compression.
    std::vector<std::byte> bytes;               ///< The object as it travelled.
};

/// The still-enveloped object field of a reply that must be a successful compile.
///
/// The shared prefix of every codec assertion below, so the `Ok`-status +
/// `DecodeCompileResult` chain exists once. A case that hand-rolled it dropped the
/// status check and would have passed against an error reply.
/// @param frame The whole reply frame.
/// @return The object field, owned, exactly as it travelled.
[[nodiscard]] std::vector<std::byte> FieldOf(std::vector<std::byte> const& frame)
{
    auto const reply = Decode(frame);
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const result = Wire::DecodeCompileResult(reply.payload);
    REQUIRE(result.has_value());
    auto const field = Unwrap(result).object;
    return { field.begin(), field.end() };
}

/// Decode the object field of a reply that must be a successful compile.
/// @param frame The whole reply frame.
/// @return Its object field.
[[nodiscard]] ObjectField ObjectOf(std::vector<std::byte> const& frame)
{
    auto const field = FieldOf(frame);
    auto const envelope = Wire::DecodeCodecEnvelope(field);
    REQUIRE(envelope.has_value());
    return ObjectField { .codec = Unwrap(envelope).codec,
                         .rawLength = Unwrap(envelope).rawLength,
                         .bytes = { Unwrap(envelope).bytes.begin(), Unwrap(envelope).bytes.end() } };
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

    // And something DID rise. A refusal the worker answers on the wire and counts
    // nowhere is one an operator can only find in a client's log: a port being
    // probed with these looked, on `/metrics`, exactly like a port nobody was
    // talking to. Counted under its own reason, not a shared `bad_envelope`, because
    // "somebody is declaring 4 GiB at my compile port" is not the same page as "two
    // of my machines were packaged with different codecs".
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeDeclaredTooLarge) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeMalformed) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeUnsupportedCodec) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeCorrupt) == 0);
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

    // Two frames, two codes, two counters -- and the counters split where the codes
    // do not. `MalformedFrame` is all the peer can act on either way, while an
    // operator seeing the second rise is looking at a version skew and the first at
    // somebody sizing a request past the cap.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeDeclaredTooLarge) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeMalformed) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
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

    // Everything below walks the enum's own count rather than a list written out
    // here. A hand-written list is the guard shape `RowsInEnumeratorOrder` exists to
    // reject: append a fifth `EnvelopeError` and a four-element list keeps checking
    // four, so the case that proves every reason is distinguishable stops covering
    // the new one and still passes. Derived from `Last`, a fifth reason is checked
    // the moment it exists.
    std::vector<std::string_view> texts;
    std::vector<IMetricsSink::Counter> counters;
    for (auto const index: std::views::iota(std::size_t { 0 }, EnumeratorCount<EnvelopeError>))
    {
        auto const reason = static_cast<EnvelopeError>(index);

        // Every reason says something, and no two say the same thing -- a description
        // shared between reasons is one an operator cannot act on.
        auto const text = DescribeEnvelopeError(reason);
        CHECK_FALSE(text.empty());
        CHECK(std::ranges::find(texts, text) == texts.end());
        texts.push_back(text);

        // The third column, and the one that shipped late: for a while these refusals
        // had a code and a message and incremented nothing at all. No two reasons
        // share a counter -- deliberately, even where two share a wire code -- because
        // summing them would hide the one that is somebody probing the port behind the
        // one that is a packaging mistake.
        auto const counter = CounterFor(reason);
        CHECK(std::ranges::find(counters, counter) == counters.end());
        counters.push_back(counter);

        // And that row names an *envelope* counter rather than a neighbour's. That
        // every counter has a catalog row is a `static_assert`'s job, not a test's;
        // what no compile-time check can see is a row built by copying the one above
        // it and leaving `WorkerJobsRefusedNoSlot` in place -- which would export a
        // plausible series under a reason that never happened. `DescriptorOf` also
        // answers nullptr for `Last`, so this covers a row that named the count.
        auto const* const row = DescriptorOf(counter);
        REQUIRE(row != nullptr);
        INFO("counter " << row->prometheusName);
        CHECK(row->prometheusName.starts_with("fastcache_worker_jobs_refused_envelope_"));
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

    // Its own counter, and the only envelope refusal that is nobody's fault: two
    // honest processes packaged differently. An operator reading a rise here goes to
    // the build of the two binaries, not to the network and not to the firewall.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeUnsupportedCodec) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEnvelopeMalformed) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
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

// --- the reply's codec -------------------------------------------------------
//
// The object crosses the network on the hot path of a parallel build, so what
// envelope it goes home in is not cosmetic. All three of these were silently wrong
// at once (#265): the choice was made from the worker's own list against itself, so
// the client's was never read; the answer was then discarded and Identity
// hard-coded; and the node built its worker with a literal `{ Identity }` anyway.
// Each defect hid the next, and no test distinguished any of it -- which is what
// these five exist to fix.

namespace
{
/// An object that actually compresses, so a case can tell a chosen codec from the
/// shrink-check falling back to Identity.
///
/// A named local rather than `return { 8 * 1024, 'A' }`, which is what
/// `modernize-return-braced-init-list` asks for and is a trap here: `std::string`
/// has an `initializer_list<char>` constructor, and that spelling picks the
/// `(count, char)` one only because `8 * 1024` narrows and so makes the list
/// candidate non-viable. A later edit to a value that fits in a `char` would
/// silently become a two-character string.
[[nodiscard]] std::string CompressibleObject()
{
    std::string object(8 * 1024, 'A');
    return object;
}

/// A well-formed COMPILE frame that varies only in what the client accepts.
///
/// Every case below cares about the third parameter and nothing else, and spelling
/// the first two positionally made `CompileFrame`'s own defaults dead for this whole
/// block -- changing them would reach the older cases and not these.
/// @param accepted What the client says it can decode.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> FrameAccepting(Wire::CodecList accepted)
{
    return CompileFrame("gcc-13", "int main(){return 0;}", std::move(accepted));
}

/// Whether this build has any codec at all beyond Identity.
[[nodiscard]] bool CompressionCompiledIn()
{
    return AvailableCodecs().size() > 1;
}

/// Whether an object field carries `expected` verbatim.
///
/// A named bool rather than the comparison inside the `CHECK`, because Catch2
/// decomposes what it is handed: comparing eight kilobytes of object inline prints
/// all eight on a failure, which buries the one line saying which case broke.
/// @param field The reply's object field.
/// @param expected What the fake compiler wrote.
/// @return True when they are byte-identical.
[[nodiscard]] bool CarriesVerbatim(ObjectField const& field, std::string_view expected)
{
    return Wire::AsStringView(field.bytes) == expected;
}
} // namespace

TEST_CASE("A compiled object comes back in a codec the client accepts", "[worker-protocol][codec]")
{
    if (!CompressionCompiledIn())
        SKIP("this build offers no codec but Identity");

    Fixture fix;
    fix.runner.object = CompressibleObject();

    auto const answer = fix.worker.Answer(FrameAccepting(AvailableCodecs()));
    REQUIRE(answer.has_value());
    auto const object = ObjectOf(Unwrap(answer));

    CHECK(object.codec != Wire::IdentityCodec);
    // The DECLARED size is the uncompressed one -- it is what bounds the client's
    // allocation before it decompresses a byte, so a compressed length here would
    // make every reply undecodable.
    CHECK(object.rawLength == fix.runner.object.size());
    CHECK(object.bytes.size() < fix.runner.object.size());
}

TEST_CASE("A compressed reply is what the launcher already decodes", "[worker-protocol][codec]")
{
    if (!CompressionCompiledIn())
        SKIP("this build offers no codec but Identity");

    // The client side needs no change for any of this, and this is the assertion
    // that says so: the same `Unenvelope` a dispatch runs on the object it gets back
    // must reproduce the compiler's bytes exactly.
    Fixture fix;
    fix.runner.object = CompressibleObject();

    auto const answer = fix.worker.Answer(FrameAccepting(AvailableCodecs()));
    REQUIRE(answer.has_value());

    auto const decoded = Unenvelope(FieldOf(Unwrap(answer)), DefaultMaxDecompressedBytes);
    REQUIRE(decoded.has_value());
    bool const identical = Wire::AsStringView(decoded.value()) == fix.runner.object;
    CHECK(identical);
}

TEST_CASE("A client that accepts only Identity is answered in Identity", "[worker-protocol][codec]")
{
    // The client's list is what decides, and a client is entitled to state a list
    // this worker would rather not use. While the choice was made from the worker's
    // own list on both sides, this case and the one above were indistinguishable.
    Fixture fix;
    fix.runner.object = CompressibleObject();

    auto const answer = fix.worker.Answer(FrameAccepting({ Wire::IdentityCodec }));
    REQUIRE(answer.has_value());
    auto const object = ObjectOf(Unwrap(answer));

    CHECK(object.codec == Wire::IdentityCodec);
    CHECK(object.rawLength == fix.runner.object.size());
    CHECK(CarriesVerbatim(object, fix.runner.object));
}

TEST_CASE("A worker offering no codec answers in Identity however the client asks", "[worker-protocol][codec]")
{
    // The other half of the same negotiation: two peers compiled with different
    // codec sets must still complete the exchange, because a build must never lose
    // distribution over configuration.
    Fixture fix { Wire::CodecList { Wire::IdentityCodec } };
    fix.runner.object = CompressibleObject();

    auto const answer = fix.worker.Answer(FrameAccepting(AvailableCodecs()));
    REQUIRE(answer.has_value());
    auto const object = ObjectOf(Unwrap(answer));

    CHECK(object.codec == Wire::IdentityCodec);
    CHECK(CarriesVerbatim(object, fix.runner.object));
}

TEST_CASE("An object compression does not shrink is sent verbatim", "[worker-protocol][codec]")
{
    // The same shrink-check `Core/Compression` applies to stored values: a six-byte
    // object gets no smaller, and a client should not pay a decompress to learn it.
    Fixture fix;

    auto const answer = fix.worker.Answer(FrameAccepting(AvailableCodecs()));
    REQUIRE(answer.has_value());
    auto const object = ObjectOf(Unwrap(answer));

    CHECK(object.codec == Wire::IdentityCodec);
    CHECK(CarriesVerbatim(object, "OBJECT"));
}

TEST_CASE("A codec both ends name but this build cannot produce falls back to Identity", "[worker-protocol][codec]")
{
    // `WorkerProtocol.hpp`'s stated contract: a worker built with a list WIDER than
    // this build can encode answers Identity rather than a codec it cannot produce.
    // `ChooseCodec` promises only that the choice is in the sender's own list, and
    // that list is injected, so the two can legitimately diverge.
    //
    // What this does NOT do, said plainly because the obvious reading is wrong: it
    // does not hold `Envelope`'s `IsAvailable` guard in place. Delete that guard and
    // this case still passes -- `Compression::Compress` hands back a verbatim copy for
    // a codec it cannot find, the shrink check rejects it, and the answer is Identity
    // by a longer route. The guard is there to skip that copy, and the copy is not
    // observable from here. Checked by deleting it rather than assumed.
    constexpr std::uint8_t noSuchCodec = 200;
    Fixture fix { Wire::CodecList { noSuchCodec, Wire::IdentityCodec } };
    fix.runner.object = CompressibleObject();

    auto const answer = fix.worker.Answer(FrameAccepting({ noSuchCodec, Wire::IdentityCodec }));
    REQUIRE(answer.has_value());
    auto const object = ObjectOf(Unwrap(answer));

    CHECK(object.codec == Wire::IdentityCodec);
    CHECK(CarriesVerbatim(object, fix.runner.object));
}

TEST_CASE("AvailableCodecsCoverEveryCodec", "[worker-protocol][codec]")
{
    // `AvailableCodecs` keeps a hand-written list of the non-Identity codecs, because
    // `Compression` exposes no way to enumerate its descriptor table. A codec added
    // there and forgotten here is never advertised and never negotiated -- in either
    // direction, by either end -- while the build stays green and every other case
    // here passes, since they assert `codec != Identity` rather than which codec.
    // That is #265's failure shape exactly, so it gets a test rather than a comment.
    //
    // `NameList()` is the one public door onto that table: it iterates the same rows
    // and joins their names. Reading the inventory back out through it is what makes
    // the omission loud.
    auto const advertised = AvailableCodecs();
    auto const names = Compression::NameList();

    for (auto const name: std::views::split(names, std::string_view { ", " }))
    {
        auto const codec = Compression::CodecFromName(std::string_view { name.begin(), name.end() });
        REQUIRE(codec.has_value());
        if (!Compression::IsAvailable(Unwrap(codec)))
            continue; // compiled out of this build; correctly absent from the wire list

        auto const id = static_cast<std::uint8_t>(Unwrap(codec));
        INFO("codec " << Compression::NameOf(Unwrap(codec)) << " is compiled in but not advertised");
        CHECK(std::ranges::find(advertised, id) != advertised.end());
    }
}
