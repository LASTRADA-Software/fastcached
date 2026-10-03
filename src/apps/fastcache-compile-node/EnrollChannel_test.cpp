// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "EnrollChannel.hpp"
#include "EnrollClient.hpp"

#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace Wire = CompileCacheWire;

namespace
{
/// Who the laptop asks to be admitted as.
/// @return Its identity, under `TestKeyPair("n-laptop")`.
[[nodiscard]] JoinerIdentity Laptop()
{
    return JoinerIdentity { .nodeId = "n-laptop",
                            .nodeEndpoint = "laptop:6674",
                            .role = Wire::EnrollRole::Learner,
                            .publicKey = Testing::TestKeyPair("n-laptop").PublicKey() };
}

/// The nonce every poll here is sent with: one value, so a case can find it in what was sent.
/// @return It.
[[nodiscard]] std::array<std::byte, Wire::NodeChallengeBytes> PollNonce()
{
    auto nonce = std::array<std::byte, Wire::NodeChallengeBytes> {};
    std::ranges::fill(nonce, std::byte { 0x5A });
    return nonce;
}

/// A fleet answering an `Enroll` with @p outcome.
/// @param outcome What it decided.
/// @param roster The roster it hands over, for an approval.
/// @param signature What it signs its answer with.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> Answer(Wire::EnrollOutcome outcome,
                                            std::span<std::byte const> roster = {},
                                            std::optional<Wire::EnrollReplySignature> const& signature = {})
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(outcome, roster, {}, signature));
}

/// Whether @p sent carries @p text as a run of its bytes.
/// @param sent What was written to the endpoint.
/// @param text What to look for.
/// @return True when it does.
[[nodiscard]] bool Carries(std::span<std::byte const> sent, std::string_view text)
{
    auto const bytes = std::as_bytes(std::span { text.data(), text.size() });
    return !std::ranges::search(sent, bytes).empty();
}

/// One channel over @p dialer, as `main` builds it: from the dialer alone, since it presents nothing.
struct Channel
{
    /// @param dialer How it reaches an endpoint.
    explicit Channel(Testing::ScriptedDialer& dialer):
        channel { dialer }
    {
    }

    DialledEnrollChannel channel;
};

/// The opcode of every request frame in @p sent, in the order they were written.
///
/// Reads the WHOLE of what reached the endpoint, frame by frame, because a credential is not a field
/// of the request: `Cc::ExchangeFramed` pipelines an `Auth` frame AHEAD of it, so a check of the
/// request alone would pass with a password sitting in front of it.
/// @param sent What was written to the endpoint.
/// @return One raw opcode per frame.
[[nodiscard]] std::vector<std::uint8_t> RequestOpsOf(std::span<std::byte const> sent)
{
    auto ops = std::vector<std::uint8_t> {};
    while (sent.size() >= Wire::RequestHeaderSize)
    {
        auto const header = Wire::DecodeRequestHeader(sent.first(Wire::RequestHeaderSize));
        REQUIRE(header.has_value());
        ops.push_back(Testing::Unwrap(header).opRaw);
        auto const frameBytes = Wire::RequestHeaderSize + Testing::Unwrap(header).payloadLength;
        REQUIRE(frameBytes <= sent.size());
        sent = sent.subspan(frameBytes);
    }
    CHECK(sent.empty()); // nothing but whole frames went out
    return ops;
}
} // namespace

TEST_CASE("An enroll poll reads one exchange with the endpoint it names", "[node][formation][enroll-channel]")
{
    auto const roster = Testing::OfficeRosterWith("n-laptop");
    auto const nonce = PollNonce();
    auto const signedAs = [&nonce](Wire::EnrollOutcome outcome, std::span<std::byte const> signedRoster) {
        return Cluster::SignAdmission(Testing::TestKeyPair("n-office"),
                                      Cluster::AdmissionClaim { .nonce = nonce,
                                                                .joinerId = "n-laptop",
                                                                .joinerKey = Laptop().publicKey,
                                                                .clusterId = "c-office",
                                                                .outcome = outcome,
                                                                .roster = signedRoster });
    };
    auto const signature = signedAs(Wire::EnrollOutcome::Approved, roster);
    auto const refusal = signedAs(Wire::EnrollOutcome::Rejected, {});
    Testing::ScriptedDialer dialer { {
        Answer(Wire::EnrollOutcome::Pending),
        Answer(Wire::EnrollOutcome::Rejected, {}, refusal),
        Answer(Wire::EnrollOutcome::Approved, roster, signature),
    } };
    Channel poll { dialer };

    auto const waiting = poll.channel.Poll("office:6674", Laptop(), nonce);
    CHECK(waiting.progress == EnrollProgress::Waiting);
    CHECK_FALSE(waiting.signature.has_value()); // none sent, none invented
    auto const refused = poll.channel.Poll("office:6674", Laptop(), nonce);
    CHECK(refused.progress == EnrollProgress::Refused);
    CHECK(refused.signature == std::optional { refusal }); // a refusal's signature is handed on too
    auto const admitted = poll.channel.Poll("office:6674", Laptop(), nonce);
    CHECK(admitted.progress == EnrollProgress::Admitted);
    CHECK(std::ranges::equal(admitted.roster, roster));
    CHECK(admitted.signature == std::optional { signature }); // handed on, for the caller to judge

    // One connection per poll, each to the endpoint it named, each carrying who asks and the nonce
    // the caller drew for it.
    CHECK(dialer.Dialed() == std::vector<std::string> { "office:6674", "office:6674", "office:6674" });
    CHECK(Carries(dialer.SentOn(0), "n-laptop"));
    CHECK(Carries(dialer.SentOn(0), "laptop:6674"));
    auto const sent = Wire::DecodeEnrollPayload(dialer.SentOn(0).subspan(Wire::RequestHeaderSize));
    REQUIRE(sent.has_value());
    CHECK(Testing::Unwrap(sent).nonce == nonce);
}

TEST_CASE("An enroll poll of an endpoint that cannot be reached is fatal and names it", "[node][formation][enroll-channel]")
{
    Testing::ScriptedDialer dialer { { {} } };
    Channel poll { dialer };

    auto const reading = poll.channel.Poll("office:6674", Laptop(), PollNonce());
    CHECK(reading.progress == EnrollProgress::Fatal);
    CHECK(reading.detail.contains("office:6674"));
    CHECK(reading.roster.empty());
}

TEST_CASE("An enroll poll answered NotLeader redirects to the endpoint the answer names",
          "[node][formation][enroll-channel]")
{
    Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "office-b:6674") } };
    Channel poll { dialer };

    auto const reading = poll.channel.Poll("office:6674", Laptop(), PollNonce());
    CHECK(reading.progress == EnrollProgress::Redirect);
    CHECK(reading.detail == "office-b:6674");
}

TEST_CASE("An enroll poll presents no credential and the one frame it writes is the Enroll",
          "[node][formation][enroll-channel][credential]")
{
    // `Enroll` is answered before authentication, and the endpoint is whatever a beacon, a DNS SRV
    // record or a seed named: any machine on the network. A `--requirepass` presented here went out
    // in the clear, in an `Auth` frame pipelined ahead of the request, once a beat. The channel takes
    // no credential to present, and this reads what actually left: one frame, and it is the request.
    Testing::ScriptedDialer dialer { { Answer(Wire::EnrollOutcome::Pending) } };
    Channel poll { dialer };

    auto const reading = poll.channel.Poll("office:6674", Laptop(), PollNonce());
    CHECK(reading.progress == EnrollProgress::Waiting);

    REQUIRE(dialer.Dialed() == std::vector<std::string> { "office:6674" });
    CHECK(RequestOpsOf(dialer.SentOn(0)) == std::vector<std::uint8_t> { std::to_underlying(Wire::Op::Enroll) });
}
