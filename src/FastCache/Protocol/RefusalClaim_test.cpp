// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RefusalClaim.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string_view>

using namespace FastCache;

namespace
{

// The three row SHAPES this tree actually has, reduced to what the policy reads. Two
// carry the claim flat; the cache's nests it behind `policy`. They are reproduced here
// rather than included, because what is under test is that ONE predicate serves shapes
// that differ -- including one this tree does not have yet.
struct FlatRow
{
    std::optional<int> answer;
    std::string_view rationale;
};

struct NestedPolicy
{
    std::optional<int> counter;
    std::string_view rationale;
};

struct NestedRow
{
    NestedPolicy policy;
};

constexpr Cc::RefusalClaim ClaimOfFlat(FlatRow const& row) noexcept
{
    return Cc::RefusalClaim { .counted = row.answer.has_value(), .rationale = row.rationale };
}

constexpr Cc::RefusalClaim ClaimOfNested(NestedRow const& row) noexcept
{
    return Cc::RefusalClaim { .counted = row.policy.counter.has_value(), .rationale = row.policy.rationale };
}

// ---------------------------------------------------------------------------
// The predicate, both directions and both failures.
//
// A row asserting NEITHER is the one that matters most: it is what a guard
// short-circuiting on the absent counter passes vacuously, shipping a new refusal
// uncounted and unexplained. A row asserting BOTH is an author who could not choose.
static_assert(Cc::StatesOneRefusalClaim(true, ""), "a counted row with no rationale states one claim");
static_assert(Cc::StatesOneRefusalClaim(false, "because"), "an uncounted row with a reason states one claim");
static_assert(!Cc::StatesOneRefusalClaim(false, ""), "a row asserting NEITHER must be refused");
static_assert(!Cc::StatesOneRefusalClaim(true, "because"), "a row asserting BOTH must be refused");

// ---------------------------------------------------------------------------
// The table form, over both shapes, in both directions.
//
// Asserted here rather than only through the three production tables, which are all
// compliant: a check exercised solely on a clean corpus has never been seen to refuse,
// and every one of these tables would go on compiling if the predicate inside the loop
// were replaced by `true`.
constexpr std::array<FlatRow, 2> CompliantFlat { { { .answer = 1, .rationale = "" },
                                                   { .answer = std::nullopt, .rationale = "deliberately uncounted" } } };
constexpr std::array<FlatRow, 2> NeitherFlat { { { .answer = 1, .rationale = "" },
                                                 { .answer = std::nullopt, .rationale = "" } } };
constexpr std::array<FlatRow, 2> BothFlat { { { .answer = 1, .rationale = "" },
                                              { .answer = 1, .rationale = "and also a reason" } } };

static_assert(Cc::RowsStateOneRefusalClaim(CompliantFlat, ClaimOfFlat));
static_assert(!Cc::RowsStateOneRefusalClaim(NeitherFlat, ClaimOfFlat),
              "a table holding a row that asserts neither must be refused -- the vacuous pass");
static_assert(!Cc::RowsStateOneRefusalClaim(BothFlat, ClaimOfFlat),
              "a table holding a row that asserts both must be refused");

constexpr std::array<NestedRow, 1> CompliantNested {
    { { .policy = { .counter = std::nullopt, .rationale = "deliberately uncounted" } } }
};
constexpr std::array<NestedRow, 1> NeitherNested { { { .policy = { .counter = std::nullopt, .rationale = "" } } } };

static_assert(Cc::RowsStateOneRefusalClaim(CompliantNested, ClaimOfNested));
static_assert(!Cc::RowsStateOneRefusalClaim(NeitherNested, ClaimOfNested),
              "the nested shape must be judged by the same rule as the flat one");

} // namespace

TEST_CASE("A refusal row states exactly one claim, and the table form says so for every shape", "[protocol][refusal]")
{
    // The rules above are `static_assert`s: this file failing to COMPILE is the real
    // verdict, and a case is registered so a reader looking for the coverage finds a
    // test rather than concluding there is none. Catch2 will not report a compile-time
    // assertion, so the case restates the four corners at run time -- cheaply, and
    // without pretending to be where the enforcement lives.
    CHECK(Cc::StatesOneRefusalClaim(true, ""));
    CHECK(Cc::StatesOneRefusalClaim(false, "because"));
    CHECK_FALSE(Cc::StatesOneRefusalClaim(false, ""));
    CHECK_FALSE(Cc::StatesOneRefusalClaim(true, "because"));

    CHECK(Cc::RowsStateOneRefusalClaim(CompliantFlat, ClaimOfFlat));
    CHECK_FALSE(Cc::RowsStateOneRefusalClaim(NeitherFlat, ClaimOfFlat));
    CHECK_FALSE(Cc::RowsStateOneRefusalClaim(BothFlat, ClaimOfFlat));
    CHECK(Cc::RowsStateOneRefusalClaim(CompliantNested, ClaimOfNested));
    CHECK_FALSE(Cc::RowsStateOneRefusalClaim(NeitherNested, ClaimOfNested));
}
