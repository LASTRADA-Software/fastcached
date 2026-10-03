// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "EnrollChannel.hpp"
#include "EnrollClient.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

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

/// A fleet answering an `Enroll` with @p outcome.
/// @param outcome What it decided.
/// @param roster The roster it hands over, for an approval.
/// @return The framed reply.
[[nodiscard]] std::vector<std::byte> Answer(Wire::EnrollOutcome outcome, std::span<std::byte const> roster = {})
{
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(outcome, roster));
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

/// One channel over @p dialer, as `main` builds it.
struct Channel
{
    /// @param dialer How it reaches an endpoint.
    explicit Channel(Testing::ScriptedDialer& dialer):
        channel { dialer, credential, logger }
    {
    }

    NodeConfig cfg {};
    ConfiguredCredential credential { cfg, nullptr };
    CapturingLogger logger;
    DialledEnrollChannel channel;
};
} // namespace

TEST_CASE("An enroll poll reads one exchange with the endpoint it names", "[node][formation][enroll-channel]")
{
    auto const roster = Testing::OfficeRosterWith("n-laptop");
    Testing::ScriptedDialer dialer { {
        Answer(Wire::EnrollOutcome::Pending),
        Answer(Wire::EnrollOutcome::Rejected),
        Answer(Wire::EnrollOutcome::Approved, roster),
    } };
    Channel poll { dialer };

    CHECK(poll.channel.Poll("office:6674", Laptop()).progress == EnrollProgress::Waiting);
    CHECK(poll.channel.Poll("office:6674", Laptop()).progress == EnrollProgress::Refused);
    auto const admitted = poll.channel.Poll("office:6674", Laptop());
    CHECK(admitted.progress == EnrollProgress::Admitted);
    CHECK(std::ranges::equal(admitted.roster, roster));

    // One connection per poll, each to the endpoint it named, each carrying who asks.
    CHECK(dialer.Dialed() == std::vector<std::string> { "office:6674", "office:6674", "office:6674" });
    CHECK(Carries(dialer.SentOn(0), "n-laptop"));
    CHECK(Carries(dialer.SentOn(0), "laptop:6674"));
}

TEST_CASE("An enroll poll of an endpoint that cannot be reached is fatal and names it", "[node][formation][enroll-channel]")
{
    Testing::ScriptedDialer dialer { { {} } };
    Channel poll { dialer };

    auto const reading = poll.channel.Poll("office:6674", Laptop());
    CHECK(reading.progress == EnrollProgress::Fatal);
    CHECK(reading.detail.contains("office:6674"));
    CHECK(reading.roster.empty());
}

TEST_CASE("An enroll poll answered NotLeader redirects to the endpoint the answer names",
          "[node][formation][enroll-channel]")
{
    Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, "office-b:6674") } };
    Channel poll { dialer };

    auto const reading = poll.channel.Poll("office:6674", Laptop());
    CHECK(reading.progress == EnrollProgress::Redirect);
    CHECK(reading.detail == "office-b:6674");
}
