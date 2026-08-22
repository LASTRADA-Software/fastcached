// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"
#include "ScratchPathTestSupport.hpp"
#include "WorkerProtocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
}

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
    std::filesystem::path scratch;
    CompileJobRunner jobs;
    WorkerProtocol worker;

    Fixture():
        scratch { Test::UniqueScratchPath("fc-wp") },
        jobs { runner, (std::filesystem::create_directories(scratch), scratch), { { "gcc-13", "g++" } } },
        worker { jobs, [](std::string_view, std::string_view) { return true; }, { Wire::IdentityCodec } }
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

/// A well-formed COMPILE frame.
[[nodiscard]] std::vector<std::byte> CompileFrame(std::string_view fingerprint = "gcc-13",
                                                  std::string_view source = "int main(){return 0;}")
{
    auto const enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(source.size()), Wire::AsBytes(source));
    return Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "l1",
                                                      .fingerprint = fingerprint,
                                                      .args = {},
                                                      .source = enveloped,
                                                      .acceptedCodecs = { Wire::IdentityCodec },
                                                      .sourceName = "a.cpp" });
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
    auto const scratch = std::filesystem::temp_directory_path() / "fc-wp-deny";
    std::filesystem::create_directories(scratch);
    CompileJobRunner jobs { runner, scratch, { { "gcc-13", "g++" } } };
    WorkerProtocol worker { jobs, [](std::string_view, std::string_view) { return false; }, { Wire::IdentityCodec } };

    auto const answer = worker.Answer(CompileFrame());
    REQUIRE(answer.has_value());
    CHECK(ErrorOf(Unwrap(answer)) == Wire::ErrorCode::UnknownLease);

    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
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
