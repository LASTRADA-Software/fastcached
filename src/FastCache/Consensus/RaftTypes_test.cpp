// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace FastCache::Testing;

namespace
{

// Two enums differing ONLY by an appended enumerator, and neither names a bound
// anywhere. This is the pair that distinguishes #197's fix from what it replaced:
// under the `WireEnumBound<E>::Highest` trait neither of these compiles at all
// without a specialization naming its last enumerator, and appending `C` to the
// second would need a second edit that nothing reminds you to make.
//
// They are declared here rather than reusing a production enum on purpose. A test
// that appends to a real wire enum to observe the bound move would be changing the
// wire, and one that does not append cannot observe anything -- the old and new
// bounds agree exactly for every enum in the tree today, which is why this is not
// a correctness bug and why no runtime assertion on a production enum can separate
// them.
enum class TwoValued : std::uint8_t
{
    A,
    B,
    Last,
};

enum class ThreeValued : std::uint8_t
{
    A,
    B,
    C,
    Last,
};

} // namespace

TEST_CASE("DecodeWireEnum bounds on the enum's own Last", "[consensus][wire][enum]")
{
    SECTION("the bound is derived, so an appended enumerator needs no second edit")
    {
        // The only difference between these two lines is which enum was declared
        // with one more enumerator. Nothing states a bound for either.
        CHECK_FALSE(DecodeWireEnum<TwoValued>(2).has_value());

        // Held in a variable and read through `Unwrap`: a bare `*optional` is
        // refused by `bugprone-unchecked-optional-access` however obviously the
        // REQUIRE above guards it, because the analyser sees a SECOND call.
        auto const appended = DecodeWireEnum<ThreeValued>(2);
        REQUIRE(appended.has_value());
        CHECK(Unwrap(appended) == ThreeValued::C);
    }

    SECTION("Last itself never decodes")
    {
        // The comparison is `>=`, and this is the case that says so: with `>` --
        // which is what a bound naming the last enumerator spells -- `Last`'s own
        // ordinal would decode into an enumerator that is not a value, on every
        // enum in the tree. Neuter the fix to `>` and this section is what fails.
        CHECK_FALSE(DecodeWireEnum<TwoValued>(static_cast<std::uint8_t>(TwoValued::Last)).has_value());
        CHECK_FALSE(DecodeWireEnum<EntryKind>(static_cast<std::uint8_t>(EntryKind::Last)).has_value());
        CHECK_FALSE(DecodeWireEnum<VoteDecision>(static_cast<std::uint8_t>(VoteDecision::Last)).has_value());
        CHECK_FALSE(DecodeWireEnum<AppendResult>(static_cast<std::uint8_t>(AppendResult::Last)).has_value());
        CHECK_FALSE(DecodeWireEnum<Cluster::CommandKind>(static_cast<std::uint8_t>(Cluster::CommandKind::Last)).has_value());
    }

    SECTION("every enumerator that does travel decodes to itself")
    {
        // The positive control. A guard nobody has watched ACCEPT is not known to
        // work, and every other section here asserts a refusal -- so a
        // `DecodeWireEnum` that returned `nullopt` unconditionally would pass all
        // of them.
        REQUIRE(DecodeWireEnum<EntryKind>(0) == EntryKind::Command);
        REQUIRE(DecodeWireEnum<EntryKind>(1) == EntryKind::NoOp);
        REQUIRE(DecodeWireEnum<EntryKind>(2) == EntryKind::Configuration);

        REQUIRE(DecodeWireEnum<VoteDecision>(0) == VoteDecision::Denied);
        REQUIRE(DecodeWireEnum<VoteDecision>(1) == VoteDecision::Granted);

        REQUIRE(DecodeWireEnum<AppendResult>(0) == AppendResult::Rejected);
        REQUIRE(DecodeWireEnum<AppendResult>(1) == AppendResult::Accepted);
    }

    SECTION("a byte naming no enumerator is refused rather than cast")
    {
        // Including the two that a `signed char` platform or a sloppy widening
        // would turn into something small and plausible.
        CHECK_FALSE(DecodeWireEnum<EntryKind>(3).has_value());
        CHECK_FALSE(DecodeWireEnum<EntryKind>(127).has_value());
        CHECK_FALSE(DecodeWireEnum<EntryKind>(128).has_value());
        CHECK_FALSE(DecodeWireEnum<EntryKind>(255).has_value());
    }
}
