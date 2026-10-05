// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ChallengeIssuer.hpp>
#include <FastCache/Core/Nonce.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::Unwrap;

// An issued nonce is never copied -- a copy could check a second answer against one nonce -- and
// never made except by the issuer.
static_assert(!std::is_copy_constructible_v<IssuedNonce>);
static_assert(!std::is_copy_assignable_v<IssuedNonce>);
static_assert(std::is_nothrow_move_constructible_v<IssuedNonce>);
static_assert(!std::is_default_constructible_v<IssuedNonce>);
static_assert(!std::is_constructible_v<IssuedNonce, Nonce>);

TEST_CASE("An issued nonce carries the drawn bytes, and each is a fresh draw", "[cluster][formation][challenge]")
{
    auto random = ScriptedSecureRandom { ScriptedSecureRandom::Ascending(2 * NonceBytes, 0x10) };
    auto issuer = ChallengeIssuer { random };

    auto first = issuer.IssueNonce();
    REQUIRE(first.has_value());
    CHECK(first->wire.front() == std::byte { 0x10 });
    // What is sent and what an answer is verified against are the same nonce.
    REQUIRE(first->held.Held().has_value());
    CHECK(Unwrap(first->held.Held()) == first->wire);

    // The second holds the script's next bytes, not the first's.
    auto second = issuer.IssueNonce();
    REQUIRE(second.has_value());
    CHECK(second->wire.front() == static_cast<std::byte>(0x10 + NonceBytes));
    CHECK(second->wire != first->wire);
    CHECK(random.FillCount() == 2);
}

TEST_CASE("A draw that fails issues no nonce", "[cluster][formation][challenge]")
{
    auto random = ScriptedSecureRandom { ScriptedSecureRandom::DeniedFailure() };
    auto issuer = ChallengeIssuer { random };
    auto const refused = issuer.IssueNonce();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);
    CHECK(random.FillCount() == 1);
}

TEST_CASE("Moving an issued nonce out of its table leaves nothing behind", "[cluster][formation][challenge]")
{
    auto random = ScriptedSecureRandom { ScriptedSecureRandom::Ascending(NonceBytes, 0x40) };
    auto issuer = ChallengeIssuer { random };
    auto fresh = issuer.IssueNonce();
    REQUIRE(fresh.has_value());

    std::unordered_map<std::string, IssuedNonce> table;
    table.insert_or_assign("seed", std::move(fresh->held));
    auto const taken = std::move(table.at("seed"));
    REQUIRE(taken.Held().has_value());
    CHECK(Unwrap(taken.Held()).front() == std::byte { 0x40 });

    // The entry left behind is spent and holds no nonce: an array's move is a copy and an
    // optional's move leaves its source engaged, so this is the part a defaulted move gets wrong.
    CHECK(table.at("seed").Spent());
    CHECK_FALSE(table.at("seed").Held().has_value());
}
