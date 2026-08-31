// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "CompileCorrelation.hpp"
#include "Dispatch.hpp"
#include "StubObjectTestSupport.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A notice these cases do not inspect.
///
/// Shared on purpose: every case here asserts the OUTCOME's `credentialIgnored`
/// flag, not the diagnostic, and a fresh object per call would imply they cared. The
/// cases that do care build their own recording notice, because a shared one reports
/// once and would let whichever case ran first silence the rest -- a coupling to
/// Catch2's ordering that is invisible until it fails.
/// @return A notice with no sink.
[[nodiscard]] FastCache::Cc::CredentialNotice& Unwatched()
{
    static FastCache::Cc::CredentialNotice notice = FastCache::Cc::CredentialNotice::Silent();
    return notice;
}

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
    /// @param validator The lease policy. The default is `UncheckedLeaseValidator()`
    ///        -- the production validator of a node with no cluster key, spelled by
    ///        calling the production factory rather than by writing an accept-all
    ///        lambda here. Most cases in this file are about codecs, framing and
    ///        fingerprints, and a lease they would have to mint first is noise; the
    ///        cases that ARE about the lease pass `SignedLeaseValidator`, which is
    ///        the other production shape.
    explicit Fixture(Wire::CodecList codecs = AvailableCodecs(), LeaseValidator validator = UncheckedLeaseValidator()):
        jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() },
        worker { jobs, std::move(validator), std::move(codecs), metrics }
    {
    }
    Fixture(Fixture const&) = delete;
    Fixture& operator=(Fixture const&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() = default;
};

/// The cluster key both actors in a lease case share.
///
/// A constant rather than a random draw: what these cases turn on is which FIELDS a
/// grant names, and a key that differs per run would make a failure look like a
/// flake. Its bytes are otherwise arbitrary.
/// @return The key.
[[nodiscard]] std::vector<std::byte> TestClusterKey()
{
    return std::vector<std::byte>(32, std::byte { 0x5A });
}

/// The endpoint the worker under test advertises, and the one grants must name.
inline constexpr std::string_view ThisWorker = "worker-under-test:6675";

/// The fleet this worker belongs to, and the one a grant has to name.
///
/// Named rather than empty so the cases exercise a real comparison: an empty cluster
/// on both sides passes whether the check runs or not, which is the shape that would
/// have let #322 ship with a verifier that never looked.
inline constexpr std::string_view ThisCluster = "fleet-under-test";

/// What a lease case compiles. Its content is irrelevant to every one of them; it is
/// named only so the calls that vary the lease do not each restate it.
inline constexpr std::string_view DefaultSource = "int main(){return 0;}";

/// The wall clock every lease case reads, at `2024-01-01T00:00:00Z`.
///
/// `ManualWallClock`, never the system one: an expiry compared against real time is
/// a test whose meaning changes while it runs, and the skew slack is five minutes --
/// long enough that a case using `now()` would pass for the wrong reason.
///
/// `const`, and that is the point of it not being an accessor around a mutable
/// static. Nothing here advances it, and a shared non-const clock invites the next
/// case to -- at which point every other lease case's expiry arithmetic silently
/// depends on the order they ran in.
ManualWallClock const LeaseClock { std::chrono::system_clock::time_point { std::chrono::seconds { 1704067200 } } };

/// A grant, signed the way a scheduler signs one.
///
/// Two actors: this mints as the SCHEDULER, and the worker under test verifies. A
/// fixture where one object did both would prove the MAC round-trips and nothing
/// about whether the worker ever asks.
/// @param endpoint The worker the grant is issued FOR.
/// @param fingerprint The toolchain it authorizes.
/// @param validFor How long past `LeaseClock()` it lasts; negative for an expired one.
/// @param cluster Which fleet mints it; defaults to the one the worker belongs to.
/// @return The token, as a client would present it.
[[nodiscard]] std::string GrantFor(std::string_view endpoint,
                                   std::string_view fingerprint = "gcc-13",
                                   std::chrono::seconds validFor = std::chrono::minutes { 10 },
                                   std::string_view cluster = ThisCluster)
{
    return FastCache::Distributed::MintLeaseToken(
        TestClusterKey(),
        FastCache::Distributed::LeaseClaims { .serial = "17",
                                              .endpoint = std::string { endpoint },
                                              .fingerprint = std::string { fingerprint },
                                              .key = "obj-abc",
                                              .expiresAt = LeaseClock.Now() + validFor,
                                              .clusterId = std::string { cluster },
                                              .epoch = 4 });
}

/// A worker that holds the cluster key and therefore checks what it is handed.
/// @return The production validator, built the way `main.cpp` builds it.
[[nodiscard]] LeaseValidator VerifyingValidator()
{
    return SignedLeaseValidator(TestClusterKey(), std::string { ThisWorker }, std::string { ThisCluster }, LeaseClock);
}

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
/// @param leaseToken The grant to present. Defaulted, because most cases here are
///        about codecs, framing and fingerprints and run against a validator that
///        refuses nothing; the lease cases pass a real one. A parameter rather than a
///        second frame builder -- this doc comment already claims that everything
///        around the source field "is boilerplate no case varies, so it lives here
///        once", and a copy would have made that false.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> FrameWithSource(std::span<std::byte const> source,
                                                     std::string_view fingerprint = "gcc-13",
                                                     Wire::CodecList accepted = { Wire::IdentityCodec },
                                                     std::string_view leaseToken = "l1")
{
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = leaseToken,
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
/// @param leaseToken The grant to present; see `FrameWithSource`.
/// @return The framed request.
[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13",
                                                  std::string_view source = "int main(){return 0;}",
                                                  Wire::CodecList accepted = { Wire::IdentityCodec },
                                                  std::string_view leaseToken = "l1")
{
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(source.size()), Wire::AsBytes(source));
    return FrameWithSource(enveloped, fingerprint, std::move(accepted), leaseToken);
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

/// The still-enveloped object field of a reply that must be a successful compile.
///
/// The shared prefix of every codec assertion below, so the `Ok`-status +
/// `DecodeCompileResult` chain exists once. A case that hand-rolled it dropped the
/// status check and would have passed against an error reply.
/// @param frame The whole reply frame.
/// @return The object field, exactly as it travelled.
[[nodiscard]] std::vector<std::byte> FieldOf(std::vector<std::byte> const& frame)
{
    auto const reply = Decode(frame);
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const result = Wire::DecodeCompileResult(reply.payload);
    REQUIRE(result.has_value());
    // Handed back directly: `CompileResultFields` owns its bytes since #366, so this
    // no longer has to copy the span out before the decoded buffer goes away.
    return Unwrap(result).object;
}

/// Decode the object field of a reply that must be a successful compile.
/// @param frame The whole reply frame.
/// @return Its object field.
/// A successful reply's object field, envelope header and all.
///
/// Owns, because this helper RETURNS it and `field` below dies with the call.
/// `Wire::CodecEnvelopeView` deliberately borrows -- its production consumer reads
/// it in scope, and owning there would put a second full copy of a preprocessed
/// translation unit on the path least able to afford it (#366). So the copy belongs
/// here, at the one caller that needs it, rather than in the wire type.
///
/// This is no longer a workaround for an unlabelled hazard, which is what it was
/// before the type said `View`: it is a local answer to a documented property.
struct ObjectField
{
    std::uint8_t codec { Wire::IdentityCodec }; ///< The codec the worker answered in.
    std::uint32_t rawLength { 0 };              ///< The size the worker declared, before compression.
    std::vector<std::byte> bytes;               ///< The object as it travelled.
};

[[nodiscard]] ObjectField ObjectOf(std::vector<std::byte> const& frame)
{
    auto const field = FieldOf(frame);
    auto const envelope = Wire::DecodeCodecEnvelope(field);
    REQUIRE(envelope.has_value());
    return ObjectField { .codec = Unwrap(envelope).codec,
                         .rawLength = Unwrap(envelope).rawLength,
                         .bytes = { Unwrap(envelope).bytes.begin(), Unwrap(envelope).bytes.end() } };
}

/// Every envelope-refusal counter, read back in `EnvelopeError` order.
///
/// Derived from the enum rather than listed, for the reason the table case below
/// records: three cases here each spelled out "and the others are zero" by hand and
/// each spelled a DIFFERENT subset, so between them they left two of the four
/// unchecked on some paths. A fifth reason is now checked by every one of them the
/// moment it exists.
/// @param metrics The fixture's sink.
/// @return One count per reason, in enumerator order.
[[nodiscard]] std::vector<std::uint64_t> EnvelopeCounts(AtomicMetricsSink const& metrics)
{
    std::vector<std::uint64_t> counts;
    for (auto const index: std::views::iota(std::size_t { 0 }, EnumeratorCount<EnvelopeError>))
        counts.push_back(metrics.Read(CounterFor(static_cast<EnvelopeError>(index))));
    return counts;
}

/// The counts a case expects: one for each reason named, zero for every other.
/// @param raised The reasons this case expects to have fired exactly once.
/// @return The expected vector, in enumerator order.
[[nodiscard]] std::vector<std::uint64_t> OnlyRaised(std::initializer_list<EnvelopeError> raised)
{
    std::vector<std::uint64_t> expected(EnumeratorCount<EnvelopeError>, 0U);
    for (auto const reason: raised)
        expected[static_cast<std::size_t>(reason)] = 1U;
    return expected;
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

TEST_CASE("A worker refuses AUTH with the one code the client steps over", "[worker-protocol]")
{
    // NOT `DispatchNotPermitted`, which is what the neighbouring case above asserts
    // for a verb served on a DIFFERENT port. The two are different sentences: that
    // one says "this endpoint does not do that job", a routing fact a client acts on;
    // this says "I do not implement this verb", which is what an absent capability
    // is.
    //
    // `Cc::CacheProtocol::Exchange` tolerates exactly `Wire::UnimplementedVerb` and
    // proceeds unauthenticated -- right against a worker with no credential to check.
    // Answered anything else, a `FASTCACHE_TOKEN`-configured worker was refused at
    // REGISTER and never joined the fleet at all (#340).
    //
    // Asserted by VALUE: the enumerator both ends name is the only thing holding this
    // contract together, and `fastcache-cc` links none of `FastCache`.
    Fixture fix;

    auto const auth = Wire::EncodeAuth(Wire::AuthRequest { .username = "bob", .secret = "s3cret" });
    auto const answer = fix.worker.Answer(auth);
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::UnimplementedVerb);

    // A bare token with no username, which is how `FASTCACHE_TOKEN` alone reaches the
    // wire, takes the same answer.
    auto const tokenOnly = Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = "s3cret" });
    auto const bare = fix.worker.Answer(tokenOnly);
    REQUIRE(bare.has_value());
    CHECK(ErrorOf(Unwrap(bare)) == Wire::UnimplementedVerb);
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
    CHECK(EnvelopeCounts(fix.metrics) == OnlyRaised({ EnvelopeError::DeclaredTooLarge }));
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
    CHECK(EnvelopeCounts(fix.metrics) == OnlyRaised({ EnvelopeError::DeclaredTooLarge, EnvelopeError::Malformed }));
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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };
    AtomicMetricsSink metrics;
    constexpr std::size_t TinyCap = 8;
    WorkerProtocol worker { jobs, UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics, TinyCap };

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
    CHECK(EnvelopeCounts(fix.metrics) == OnlyRaised({ EnvelopeError::UnsupportedCodec }));
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("A payload that does not expand to its declared size is refused as corrupt", "[worker-protocol]")
{
    // The fourth envelope refusal, and the only one of them that implicates the
    // TRANSPORT: framing this build parsed, a codec it has, and bytes that then
    // failed to expand. It was the one reason with no case of its own -- the table
    // proved its row is distinct and nothing proved the worker ever reaches it, so
    // "answered `malformed-frame`, counted as corrupt" was a claim rather than an
    // assertion.
    //
    // Needs a codec that actually decompresses: a build with compression configured
    // out reaches `Corrupt` on no path at all, because Identity's length
    // disagreement is `Malformed` and is covered above.
    auto const codecs = AvailableCodecs();
    if (codecs.size() < 2)
        SKIP("this build offers no codec but Identity");

    Fixture fix;

    // Well inside the ceiling, so `DeclaredTooLarge` cannot answer first, and not a
    // valid frame for any codec, so the decompressor is what refuses it. Most
    // preferred first, so `front()` is never Identity here.
    std::array<std::byte, 16> const garbage { std::byte { 0x7F } };
    auto const rotten = Wire::EncodeCodecEnvelope(codecs.front(), 64, garbage);
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                                  .fingerprint = "gcc-13",
                                                                  .args = {},
                                                                  .source = rotten,
                                                                  .acceptedCodecs = { Wire::IdentityCodec },
                                                                  .sourceName = "a.cpp" });
    auto const answer = fix.worker.Answer(frame);
    REQUIRE(answer.has_value());

    // The same wire code a malformed envelope gets -- all the peer can act on either
    // way -- and a counter of its own, because an operator seeing this rise is
    // looking at the link rather than at a version skew.
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::MalformedFrame);
    CHECK(EnvelopeCounts(fix.metrics) == OnlyRaised({ EnvelopeError::Corrupt }));
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

TEST_CASE("A lease nobody signed buys no compile", "[worker-protocol][lease]")
{
    // This absorbed a second case, "an unauthorized lease is refused before the
    // payload is even decoded", which after the rework built the same fixture, sent
    // the same junk token and made a subset of these assertions. Two cases pinning
    // one scenario are two places to edit when a code or a counter moves, and no way
    // for a reader to tell what distinguishes them.
    //
    // What that case's name was worth keeping is the LAST assertion here: the refusal
    // lands before any compiler runs.
    // #281 made the scheduler SIGN its grants and nothing checked the signature, so
    // a compile port's boundary was reachability plus membership -- and "admitted to
    // the fleet" is not "granted this compile". Any admitted machine could spend any
    // worker's CPU without ever asking the scheduler for a slot (#282).
    //
    // This case was written as a REPRODUCTION asserting `Status::Ok` and passed. It
    // is the same frame; only the expected answer moved.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const answer =
        fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, "not-a-lease-at-all"));
    REQUIRE(answer.has_value());
    auto const reply = Decode(Unwrap(answer));

    CHECK(reply.status == Wire::Status::Error);
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::LeaseUnauthorized);

    // The counter has to MOVE. It has existed since #313 and read zero ever since,
    // by design, because nothing refused a lease -- so a green run in which it still
    // reads zero would mean the refusal came from somewhere else entirely.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized) == 1);

    // And the expensive part never ran. This is checked ahead of decompression, let
    // alone spawning a compiler.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("A lease signed for ANOTHER worker is refused here", "[worker-protocol][lease]")
{
    // Two actors, because a credential is granted by one party and presented to
    // another. Actor one is the SCHEDULER, minting a genuine grant with the cluster
    // key: every field authentic, the MAC valid, and the endpoint naming a different
    // machine. Actor two is THIS worker, which is not that machine.
    //
    // The endpoint is the load-bearing field. It is inside the MAC precisely so a
    // token captured on the way to one worker cannot be replayed against every other
    // worker trusting the same key.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const answer = fix.worker.Answer(
        CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, GrantFor("some-other-worker:6675")));
    REQUIRE(answer.has_value());
    auto const reply = Decode(Unwrap(answer));

    CHECK(reply.status == Wire::Status::Error);

    // A code of its own, and that is what the reason exists for: a forged token and
    // a worker advertising an address the scheduler did not grant are two different
    // things for an operator to go and do. Collapsing the validator to a `bool` would
    // have made them one line in a log.
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::LeaseEndpointMismatch);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("A lease that has expired past the skew slack is refused", "[worker-protocol][lease]")
{
    // Past the slack, not merely past the expiry: the verifier allows five minutes
    // for a fleet whose machines are not all NTP-managed, so a grant that lapsed one
    // second ago is still honoured and a case asserting otherwise would be asserting
    // the wrong contract.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const stale =
        GrantFor(ThisWorker, "gcc-13", -(FastCache::Distributed::LeaseTokenClockSkewSlack + std::chrono::seconds { 1 }));
    auto const answer = fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, stale));
    REQUIRE(answer.has_value());

    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::LeaseExpired);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("A lease that lapsed within the skew slack still compiles", "[worker-protocol][lease]")
{
    // The other half of the previous case, and the one that keeps the slack honest:
    // without it, a workstation whose clock is minutes out would refuse every job it
    // was legitimately granted, on exactly the machines nobody is watching.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const barely = GrantFor(ThisWorker, "gcc-13", -std::chrono::seconds { 1 });
    auto const answer = fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, barely));
    REQUIRE(answer.has_value());

    CHECK(Decode(Unwrap(answer)).status == Wire::Status::Ok);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired) == 0);
}

TEST_CASE("A grant for one toolchain does not pay for another's compile", "[worker-protocol][lease]")
{
    // The fingerprint is inside the MAC as well, so an authentic grant for `clang-18`
    // cannot be spent on a `gcc-13` job. The client here claims the toolchain the
    // worker serves -- so the fingerprint check that already existed passes -- and it
    // is the LEASE that disagrees.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const answer =
        fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, GrantFor(ThisWorker, "clang-18")));
    REQUIRE(answer.has_value());

    // `FingerprintMismatch` rather than a code of its own: a client already answers
    // it correctly, and a second spelling of one fact is how two peers come to
    // disagree about what happened.
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::FingerprintMismatch);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 0);
}

TEST_CASE("An authentic grant naming this worker compiles", "[worker-protocol][lease]")
{
    // The case that keeps the four above from being satisfiable by refusing
    // everything. A validator that never authorizes anything would pass every
    // refusal assertion in this file and break the entire fleet.
    Fixture fix { { Wire::IdentityCodec }, VerifyingValidator() };

    auto const answer =
        fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, GrantFor(ThisWorker)));
    REQUIRE(answer.has_value());

    CHECK(Decode(Unwrap(answer)).status == Wire::Status::Ok);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 1);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch) == 0);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired) == 0);
}

TEST_CASE("A worker with no cluster key refuses no lease, and that is a whole policy", "[worker-protocol][lease]")
{
    // `UncheckedLeaseValidator` is what a single-machine install runs, and it is a
    // named production function rather than an accept-all lambda so that this
    // property is asserted somewhere rather than being an artefact of how a fixture
    // happened to be written.
    //
    // It is only ever reachable for a node admitting nothing but its own machine:
    // `NodeConfig`'s startup table refuses a keyless node that admits peers
    // elsewhere, which is what makes this safe rather than a hole with a comment.
    Fixture fix { { Wire::IdentityCodec }, UncheckedLeaseValidator() };

    auto const answer =
        fix.worker.Answer(CompileFrame("gcc-13", DefaultSource, { Wire::IdentityCodec }, "not-a-lease-at-all"));
    REQUIRE(answer.has_value());

    CHECK(Decode(Unwrap(answer)).status == Wire::Status::Ok);
    CHECK(fix.metrics.Read(IMetricsSink::Counter::WorkerJobsStarted) == 1);
}

namespace
{

// The scripted socket and `Replies` live in `src/tests/ScriptedSocket.hpp`.
//
// The comment that used to sit here said sharing would mean moving ninety lines to
// reuse thirty, and preferred a second copy. That was answered by what happened
// next: both copies carried the SAME `WriteVectored` defect -- a zero-byte success,
// which `SendAll` reads as a short write -- and this one was found only because a
// case here finally drove a vectored write through it, a day before the other was
// noticed at all. The cost of a shared fake is lines; the cost of three is that a
// bug fixed in one stays live in the others (#362).
/// A registrar with the fields every case below shares.
[[nodiscard]] WorkerRegistrar MakeRegistrar()
{
    return WorkerRegistrar { Unwatched(), "gcc-14", "10.0.0.2:6677", 4, Wire::CodecList {}, Wire::CapacityFields {} };
}

} // namespace

TEST_CASE("A NotLeader refusal reaches the node as an endpoint, not as prose", "[cc][registrar][notleader]")
{
    // The worker half of #237. `SchedulerService::Gate()` refuses EVERY verb off the
    // leader, `Register` included, so a node that could not read the redirect went on
    // announcing itself to the demoted scheduler and expired out of the new leader's
    // registry -- which then answered every lease `NoWorker`. The launcher meanwhile
    // followed the same refusal correctly and found an empty fleet.
    //
    // Asserted as a value rather than "an error happened": the whole fix is that the
    // endpoint survives the trip from the wire to something the caller can dial.
    auto registrar = MakeRegistrar();
    Testing::ScriptedSocket scheduler { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "10.0.0.7:6676") };

    auto const outcome = registrar.Register(scheduler);
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().leader.has_value());
    CHECK(Unwrap(outcome.error().leader) == "10.0.0.7:6676");
    // And the operator still gets the words, because the two answer different
    // questions and a redirect this node cannot follow must still be diagnosable.
    CHECK_FALSE(outcome.error().reason.empty());
}

TEST_CASE("A refusal that is not a redirect names no leader", "[cc][registrar][notleader]")
{
    // `NotAMember` is not a routing fact and following it would send this node's
    // registration to a second scheduler over a fault the first one already stated.
    auto registrar = MakeRegistrar();
    Testing::ScriptedSocket scheduler { Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "not in this cluster") };

    auto const outcome = registrar.Register(scheduler);
    REQUIRE_FALSE(outcome.has_value());
    CHECK_FALSE(outcome.error().leader.has_value());
}

TEST_CASE("A NotLeader whose message is prose is not a redirect", "[cc][registrar][notleader]")
{
    // The rule lives in `RedirectTarget` and this asserts it is REACHED, not that it
    // is right -- `CacheProtocol_test` owns the grammar. A second reading of the same
    // refusal here is exactly the drift that rule exists to prevent.
    auto registrar = MakeRegistrar();
    Testing::ScriptedSocket scheduler { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "no leader: try again") };

    auto const outcome = registrar.Register(scheduler);
    REQUIRE_FALSE(outcome.has_value());
    CHECK_FALSE(outcome.error().leader.has_value());
}

TEST_CASE("A heartbeat refused NotLeader keeps its worker id", "[cc][registrar][notleader]")
{
    // A redirect says this scheduler is the wrong one to ask -- not that the fleet
    // has forgotten this worker. The registry is replicated, so the leader named may
    // well be holding the very registration this id belongs to, and clearing it would
    // turn every election into a fleet-wide re-registration storm.
    //
    // `UnknownLease` is the refusal that DOES clear it, and the two must not be
    // conflated: one is "go elsewhere", the other "start again".
    auto registrar = MakeRegistrar();
    Testing::ScriptedSocket scheduler { Testing::Replies(
        { Wire::EncodeReply(Wire::Status::Ok, AsBytes(std::string_view { "w-1" })),
          Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "10.0.0.7:6676") }) };

    REQUIRE(registrar.Register(scheduler).has_value());
    REQUIRE(registrar.WorkerId() == "w-1");

    auto const beat = registrar.Heartbeat(scheduler, 0);
    REQUIRE_FALSE(beat.has_value());
    REQUIRE(beat.error().leader.has_value());
    CHECK(Unwrap(beat.error().leader) == "10.0.0.7:6676");
    CHECK(registrar.WorkerId() == "w-1");
}

TEST_CASE("A heartbeat refused UnknownLease forgets its worker id and names no leader", "[cc][registrar][notleader]")
{
    auto registrar = MakeRegistrar();
    Testing::ScriptedSocket scheduler { Testing::Replies(
        { Wire::EncodeReply(Wire::Status::Ok, AsBytes(std::string_view { "w-1" })),
          Wire::EncodeErrorReply(Wire::ErrorCode::UnknownLease, {}) }) };

    REQUIRE(registrar.Register(scheduler).has_value());
    auto const beat = registrar.Heartbeat(scheduler, 0);
    REQUIRE_FALSE(beat.has_value());
    CHECK_FALSE(beat.error().leader.has_value());
    CHECK(registrar.WorkerId().empty());
}

TEST_CASE("A credentialled client reaches a worker that has no AUTH and still gets its answer", "[worker-protocol]")
{
    // **The acceptance of #340, and deliberately not "the refusal code changed".**
    // That assertion passes the moment a constant is edited; this one fails unless
    // the two halves actually agree, because the refusal bytes come out of the real
    // `WorkerProtocol` and go into the real `Cc::CacheProtocol::Exchange`.
    //
    // What it regresses: with `FASTCACHE_TOKEN` set, a worker answering AUTH with
    // `DispatchNotPermitted` had that refusal returned in place of the answer to the
    // request the client actually sent -- so a credentialled worker was refused at
    // REGISTER and never joined the fleet at all. The machine was absent rather than
    // idle, which is harder to notice.
    Fixture fix;

    auto const refusal = fix.worker.Answer(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" }));
    REQUIRE(refusal.has_value());

    auto const stored = std::vector<std::byte> { std::byte { 0x7 } };
    Testing::ScriptedSocket client { Testing::Replies({ Unwrap(refusal), Wire::EncodeReply(Wire::Status::Ok, stored) }) };

    // A RECORDING notice, not the shared silent one: this case is about the
    // diagnostic as much as the flag. Before #363 the flag was set here and only the
    // launcher's cache path could say so, which is the whole defect.
    std::vector<std::string> said;
    Cc::CredentialNotice notice { [&said](std::string_view text) { said.emplace_back(text); } };

    auto const outcome = SyncRun(CacheFetch(&client, &notice, "k", Credential { .username = {}, .secret = "s3cret" }));

    // The command behind the credential is served. This is the half that was broken.
    REQUIRE(outcome.IsHit());
    CHECK(outcome.value == stored);

    // And the operator is still told their token went unchecked. Restoring the answer
    // must not also swallow the fact that nothing checked the credential.
    CHECK(outcome.credentialIgnored);
    // And it was SAID, which is the half that had no test at all.
    CHECK(said.size() == 1);
    CHECK(notice.Reported());
}

namespace
{

/// A runner that reports compiling something other than what it was handed.
///
/// The fixture #280 needs, and the one the tree could not previously build. A fake at
/// `IProcessRunner` sits BELOW the point where `CompileJobRunner` records what it is
/// about to compile, so it can produce a wrong object but never a wrong report -- that
/// is #279's half. Substituting the runner itself is the only way to make the
/// execution diverge from the record, which is the event a correlation exists to catch.
class LyingRunner final: public ICompileJobRunner
{
  public:
    /// @param correlation What this runner will claim it compiled.
    explicit LyingRunner(std::string correlation):
        _correlation { std::move(correlation) }
    {
    }

    /// @param job Recorded, then otherwise ignored.
    /// @return A successful compile carrying the claimed correlation.
    [[nodiscard]] std::expected<CompileOutcome, JobError> Run(CompileJob const& job) override
    {
        _saw = job;
        return CompileOutcome { .exitCode = 0,
                                .object = { std::byte { 'O' }, std::byte { 'B' }, std::byte { 'J' } },
                                .stdoutText = {},
                                .stderrText = {},
                                .correlation = _correlation };
    }

    /// @return The job this runner was actually asked for.
    [[nodiscard]] CompileJob const& Saw() const noexcept
    {
        return _saw;
    }

  private:
    std::string _correlation;
    CompileJob _saw;
};

} // namespace

TEST_CASE("A reply carries the runner's own correlation, not one recomputed here", "[worker-protocol][correlation]")
{
    // The discriminating case for #280, and the reason `WorkerProtocol` takes the
    // INTERFACE rather than the concrete runner. The sentinel could not have been
    // derived from the request by any computation, so a `WorkerProtocol` that folded
    // the digest itself from `fields` would overwrite it and this goes red -- while
    // every other case in this file, and the ticket's own acceptance criterion, would
    // still pass under that bug.
    //
    // That is the whole point. At this layer two crossed requests are both still
    // pristine, so a digest taken here agrees with whatever it is compared against and
    // catches nothing.
    constexpr std::string_view Sentinel = "ffffffffffffffffffffffffffffffff";

    LyingRunner runner { std::string { Sentinel } };
    AtomicMetricsSink metrics;
    WorkerProtocol worker { runner, UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics };

    auto const answer = worker.Answer(CompileFrame());
    REQUIRE(answer.has_value());

    auto const reply = Decode(Unwrap(answer));
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const result = Wire::DecodeCompileResult(reply.payload);
    REQUIRE(result.has_value());
    CHECK(Wire::AsStringView(Unwrap(result).correlation) == Sentinel);

    // And the runner really was asked for the job the frame described, so the sentinel
    // is the only artificial thing about this exchange.
    CHECK(runner.Saw().fingerprint == "gcc-13");
    CHECK(runner.Saw().preprocessed == "int main(){return 0;}");
}

TEST_CASE("The real runner is what a correlation comes from", "[worker-protocol][correlation]")
{
    // The wiring assertion. An interface whose only implementation is reached through a
    // test is the reclaimer-nothing-constructs shape in a new costume, so this drives
    // the REAL `CompileJobRunner` end to end and recomputes the digest the way a client
    // will -- from what it asked for, against what came back.
    StubRunner runner;
    FastCache::Testing::ScratchDirectory const scratch { "fc-wp-corr" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };
    AtomicMetricsSink metrics;
    WorkerProtocol worker { jobs, UncheckedLeaseValidator(), { Wire::IdentityCodec }, metrics };

    constexpr std::string_view Source = "int main(){return 0;}";
    auto const answer = worker.Answer(CompileFrame("gcc-13", Source));
    REQUIRE(answer.has_value());

    auto const reply = Decode(Unwrap(answer));
    REQUIRE(reply.status == Wire::Status::Ok);
    auto const result = Wire::DecodeCompileResult(reply.payload);
    REQUIRE(result.has_value());
    CHECK_FALSE(Unwrap(result).correlation.empty());

    // `FrameWithSource` sends no arguments and names the file `a.cpp`.
    CHECK(Wire::AsStringView(Unwrap(result).correlation)
          == CompileCorrelation(Source, std::span<std::string const> {}, "gcc-13", "a.cpp"));
}

namespace
{

/// Produces the reply to one whole framed request.
///
/// The seam that lets a scripted socket answer with something COMPUTED rather than
/// canned, which is the whole point of the case below: a worker's reply depends on
/// the request, so a fixture that replays fixed bytes cannot show the two ends
/// agreeing about anything.
class IFrameResponder
{
  public:
    IFrameResponder() = default;
    virtual ~IFrameResponder() = default;
    IFrameResponder(IFrameResponder const&) = delete;
    IFrameResponder& operator=(IFrameResponder const&) = delete;
    IFrameResponder(IFrameResponder&&) = delete;
    IFrameResponder& operator=(IFrameResponder&&) = delete;

    /// @param request One complete request frame, exactly as it was written.
    /// @return The reply frame.
    [[nodiscard]] virtual std::vector<std::byte> Answer(std::span<std::byte const> request) = 0;
};

/// A socket that collects what is written to it and answers through a responder.
///
/// The reply is produced on the first READ, by which point the whole request has
/// been written -- so nothing here has to know where a frame ends, and the framing
/// stays entirely under test rather than being reimplemented by the fixture.
class AnsweringPeer final: public ISocket
{
  public:
    /// @param responder Turns the written request into a reply; must outlive this.
    explicit AnsweringPeer(IFrameResponder& responder):
        _responder { responder }
    {
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
    {
        _request.insert(_request.end(), bytes.begin(), bytes.end());
        return IoAwaitable { IoResult { bytes.size() } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        std::size_t written = 0;
        for (auto const& segment: segments)
        {
            _request.insert(_request.end(), segment.begin(), segment.end());
            written += segment.size();
        }
        return IoAwaitable { IoResult { written } };
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        if (!_answered)
        {
            _reply = _responder.Answer(_request);
            _answered = true;
        }
        // A read of zero is EOF, which is how a peer that has said everything it has
        // to say tells `RecvExactly` the frame is over.
        auto const take = std::min(_reply.size() - _cursor, buffer.size());
        std::copy_n(_reply.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
        _cursor += take;
        return IoAwaitable { IoResult { take } };
    }

    void Close() noexcept override {}
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return "answering";
    }

  private:
    IFrameResponder& _responder;
    std::vector<std::byte> _request;
    std::vector<std::byte> _reply;
    std::size_t _cursor { 0 };
    bool _answered { false };
};

constexpr std::string_view SchedulerEndpoint = "sched:6675";
constexpr std::string_view WorkerEndpoint = "worker:6676";

/// A fleet of two: a minimal scheduler, and a REAL `WorkerProtocol` as the worker.
///
/// Everything between the client and the compiler is production code here -- the
/// request encoding, the codec envelope, the argument list's framing, the source
/// name, the worker's decode, the runner, and the reply's encoding. That is what
/// this fixture is for: a correlation is one value computed twice at two ends, and
/// the failure it must rule out is the two ends DISAGREEING. Both halves tested
/// separately against the same helper agree by construction and prove nothing.
class LiveFleet final: public IEndpointExchange, public IFrameResponder
{
  public:
    /// @param worker The worker every COMPILE is handed to; must outlive this.
    explicit LiveFleet(WorkerProtocol& worker):
        _worker { worker }
    {
    }

    CacheOutcome Exchange(std::string_view hostPort,
                          std::vector<std::byte> frame,
                          Credential const& credential,
                          ExchangeBudget /*budget*/) override
    {
        _current = std::string { hostPort };
        AnsweringPeer peer { *this };
        return SyncRun(ExchangeFramed(&peer, &Unwatched(), std::move(frame), credential));
    }

    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> request) override
    {
        if (_current == WorkerEndpoint)
            return _worker.Answer(request).value_or(std::vector<std::byte> {});

        auto const header = Wire::DecodeRequestHeader(request);
        if (!header.has_value())
            return {};
        auto const* const descriptor = Wire::FindOp(header->opRaw);
        if (descriptor != nullptr && descriptor->code == Wire::Op::Lease)
            return Wire::EncodeReply(Wire::Status::Ok,
                                     Wire::EncodeLeaseGrant(Wire::LeaseGrant { .endpoint = WorkerEndpoint,
                                                                               .leaseToken = "l1",
                                                                               .workerCodecs = { Wire::IdentityCodec } }));
        // The RELEASE, which `Dispatch` sends on every path out of a compile and
        // whose answer it deliberately ignores.
        return Wire::EncodeReply(Wire::Status::Ok, {});
    }

  private:
    WorkerProtocol& _worker;
    std::string _current;
};

} // namespace

TEST_CASE("A worker's reply is accepted by the client that asked for it", "[worker-protocol][correlation]")
{
    // The two ends meeting, end to end and in one process. The client encodes the
    // request, a real worker decodes it, compiles it and digests what it compiled,
    // and the client recomputes that digest from what it asked for.
    //
    // **This is the case that would catch the two ends disagreeing**, which is the
    // failure a correlation can fail with and no half-fixture can see: the worker
    // digests `job.args` as DECODED from the wire and the source name as SENT, while
    // the client digests what it holds. Anything that changes on the way -- an
    // argument list encoding that drops an empty element, a source name reduced to
    // its base name at one end only -- makes every honest compile refuse, which is a
    // fleet that silently stops distributing rather than a fleet that mis-serves.
    // Hence the deliberately awkward arguments and the path-shaped source name.
    Fixture fixture { { Wire::IdentityCodec } };
    LiveFleet fleet { fixture.worker };

    std::vector<std::string> const args { "", "-O2", "-DMSG=hello world" };
    auto const request = DispatchRequest { .schedulerEndpoint = SchedulerEndpoint,
                                           .fingerprint = "gcc-13",
                                           .objectKey = "objkey",
                                           .args = args,
                                           .preprocessed = "int main(){return 0;}",
                                           .sourceName = "/home/dev/checkout/src/Widget.cpp" };

    auto const result = Dispatch(fleet, request);

    INFO("dispatch said: " << result.detail);
    REQUIRE(result.status == DispatchStatus::Compiled);
    CHECK(result.exitCode == 0);
    CHECK_FALSE(result.object.empty());
}
