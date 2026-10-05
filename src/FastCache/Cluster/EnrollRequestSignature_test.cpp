// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/EnrollRequestSignature.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace Wire = CompileCacheWire;

namespace
{
/// The nonce every request here draws.
constexpr auto RequestNonce = std::array<std::byte, Wire::NodeChallengeBytes> { std::byte { 0x3C } };

/// What the laptop states, under its own key.
/// @return The claim.
[[nodiscard]] EnrollRequestClaim LaptopClaim()
{
    return EnrollRequestClaim { .nodeId = "n-laptop",
                                .nodeEndpoint = "laptop:6674",
                                .role = Wire::EnrollRole::Learner,
                                .publicKey = Testing::TestKeyPair("n-laptop").PublicKey(),
                                .nonce = RequestNonce,
                                .challenge = {} };
}
} // namespace

TEST_CASE("An enroll request's signature covers every field it states, so none can be swapped under it",
          "[cluster][enrollment][enroll-signature]")
{
    auto const laptop = Testing::TestKeyPair("n-laptop");
    auto const signature = SignEnrollRequest(laptop, LaptopClaim());
    REQUIRE(VerifyEnrollRequest(LaptopClaim(), signature));

    // Each field changed alone, with the others as signed: the one that changed is what fails it. The
    // leader's challenge among them, answered or not -- a first ask's signature refreshes no row.
    auto const otherNonce = std::array<std::byte, Wire::NodeChallengeBytes> { std::byte { 0x3D } };
    auto const challenge = std::array<std::byte, Wire::NodeChallengeBytes> { std::byte { 0x5E } };
    // The role is signed too, but a learner is the one role a request may state -- a retired byte is
    // refused by the decoder before any signature is asked -- so no second role can be tried here.
    auto const changes = std::array<std::function<void(EnrollRequestClaim&)>, 5> {
        [](EnrollRequestClaim& c) { c.nodeId = "n-desk"; },
        [](EnrollRequestClaim& c) { c.nodeEndpoint = "attacker:6674"; },
        [](EnrollRequestClaim& c) { c.publicKey = Testing::TestKeyPair("n-desk").PublicKey(); },
        [&otherNonce](EnrollRequestClaim& c) { c.nonce = otherNonce; },
        [&challenge](EnrollRequestClaim& c) { c.challenge = challenge; },
    };
    for (auto const& change: changes)
    {
        auto claim = LaptopClaim();
        change(claim);
        CHECK_FALSE(VerifyEnrollRequest(claim, signature));
    }
}

TEST_CASE("An enroll request is signed under the signer's own key, whatever key the claim was handed",
          "[cluster][enrollment][enroll-signature]")
{
    // A forger holding only its own key cannot make a request under the laptop's verify: the key the
    // signature covers is the signer's, so a claim naming the laptop's key does not verify under it.
    auto const forger = Testing::TestKeyPair("n-forger");
    auto const forged = SignEnrollRequest(forger, LaptopClaim());
    CHECK_FALSE(VerifyEnrollRequest(LaptopClaim(), forged));

    auto asForger = LaptopClaim();
    asForger.publicKey = forger.PublicKey();
    CHECK(VerifyEnrollRequest(asForger, forged)); // the control: it is the forger's own request
}

TEST_CASE("An enroll request's label is its own construction's and comes first", "[cluster][enrollment][enroll-signature]")
{
    auto const message = EnrollRequestMessage(LaptopClaim());
    CHECK(message.Purpose() == IdentityKeyPurpose::EnrollRequest);
    auto const fields = WireFields::SplitAll(message.Bytes());
    REQUIRE(fields.has_value());
    auto const& parts = Testing::Unwrap(fields);
    REQUIRE(parts.size() == 7); // the label, then the six fields the request states
    CHECK(WireFields::AsStringView(parts[0]) == LabelOf(IdentityKeyPurpose::EnrollRequest));
    CHECK(LabelOf(IdentityKeyPurpose::EnrollRequest) == "fastcache-enroll-request-v1");
}
