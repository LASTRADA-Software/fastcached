// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Core/Ed25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
constexpr auto Office = std::string_view { "0123456789abcdef0123456789abcdef" };

/// @param machine Whose test key.
/// @return Its public key.
[[nodiscard]] Ed25519PublicKey KeyOf(std::string const& machine)
{
    return Testing::TestKeyPair(machine).PublicKey();
}
} // namespace

TEST_CASE("A pin admits its cluster only under a pinned voter's key, and an absent pin admits all",
          "[cluster][pin][security]")
{
    auto const office = KeyOf("n-office");
    auto const desk = KeyOf("n-desk");
    auto const rogue = KeyOf("n-rogue");
    auto const pin =
        FleetPin { .fleet = PinnedFleet { .clusterId = std::string { Office }, .voterKeys = { office, desk } } };

    CHECK(AdmitsFleet(pin, Office, office));
    CHECK(AdmitsFleet(pin, Office, desk));                                       // a second pinned voter
    CHECK_FALSE(AdmitsFleet(pin, Office, rogue));                                // the id copied, signed by an impostor
    CHECK_FALSE(AdmitsFleet(pin, "fedcba9876543210fedcba9876543210", office));   // the key, another cluster
    CHECK(AdmitsFleet(pin, Office, std::array { rogue, desk }));                 // ANY of the signers
    CHECK_FALSE(AdmitsFleet(pin, Office, std::span<Ed25519PublicKey const> {})); // nobody vouches

    // Trust on first use, the control: everything is admitted.
    CHECK(AdmitsFleet(FleetPin {}, "anything", rogue));
    CHECK(AdmitsFleet(FleetPin {}, "anything", std::span<Ed25519PublicKey const> {}));
}

TEST_CASE("A pin round-trips through its text, and its text is refused by name when it is not one", "[cluster][pin]")
{
    auto const fleet =
        PinnedFleet { .clusterId = std::string { Office }, .voterKeys = { KeyOf("n-office"), KeyOf("n-desk") } };
    auto const text = FormatPinnedFleet(fleet);
    CHECK(text
          == std::format(
              "{}@{},{}", Office, FormatEd25519PublicKey(KeyOf("n-office")), FormatEd25519PublicKey(KeyOf("n-desk"))));
    auto const back = ParsePinnedFleet(text);
    REQUIRE(back.has_value());
    CHECK(Unwrap(back) == fleet);
    CHECK(PinText(FleetPin {}) == "none");
    CHECK(PinText(FleetPin { .fleet = fleet }) == text);

    // An id alone is NEVER a pin by name.
    for (auto const& idOnly: { std::string { Office }, std::format("{}@", Office) })
    {
        INFO(idOnly);
        auto const refused = ParsePinnedFleet(idOnly);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("no voter key"));
        CHECK(refused.error().contains("fastcache-cli node"));
    }

    // More keys than any office runs.
    auto many = PinnedFleet { .clusterId = std::string { Office }, .voterKeys = {} };
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxPinnedVoterKeys + 1))
        many.voterKeys.push_back(KeyOf(std::format("n-{}", index)));
    auto const tooMany = ParsePinnedFleet(FormatPinnedFleet(many));
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().contains(std::format("more than {} voter keys", MaxPinnedVoterKeys)));
    many.voterKeys.pop_back();
    CHECK(ParsePinnedFleet(FormatPinnedFleet(many)).has_value());
}
