// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InheritedListener.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache;

namespace
{
/// The pid these cases pretend to be.
constexpr std::uint64_t SelfPid = 4242;

[[nodiscard]] ActivationHandoff Parse(std::optional<std::string_view> pid, std::optional<std::string_view> fds)
{
    return ParseSocketActivation(pid, fds, SelfPid);
}
} // namespace

TEST_CASE("A handoff addressed to this process is adopted", "[socket-activation]")
{
    auto const handoff = Parse("4242", "2");
    CHECK(handoff.Any());
    CHECK(handoff.count == 2);
    // Fixed by systemd's protocol: 0/1/2 are stdio, so a handoff starts at 3.
    CHECK(handoff.firstDescriptor == 3);
}

TEST_CASE("A handoff addressed to another process is refused", "[socket-activation]")
{
    // The security-relevant rule. These variables survive fork and exec, so any
    // grandchild of an activated service sees them -- and adopting on their
    // strength alone means treating whatever the parent left on descriptor 3 as a
    // listening socket. That could be a log file, a database connection, or the
    // read end of a pipe, and the daemon would then "accept" on it forever.
    auto const handoff = Parse("9999", "2");
    CHECK(!handoff.Any());
    CHECK(handoff.count == 0);
    // Zero rather than 3, so a caller that ignored `count` still cannot mistake
    // the descriptor for a usable one.
    CHECK(handoff.firstDescriptor == 0);
}

TEST_CASE("Neither variable alone is a handoff", "[socket-activation]")
{
    CHECK(!Parse("4242", std::nullopt).Any());
    CHECK(!Parse(std::nullopt, "2").Any());
    CHECK(!Parse(std::nullopt, std::nullopt).Any());
}

TEST_CASE("A zero count is not a handoff", "[socket-activation]")
{
    CHECK(!Parse("4242", "0").Any());
}

TEST_CASE("A value that is not exactly a number is refused", "[socket-activation]")
{
    // Whole-string parsing, not a prefix: reading "3x" as 3 would adopt
    // descriptors on the strength of an environment this code has just proved it
    // does not understand.
    CHECK(!Parse("4242", "2x").Any());
    CHECK(!Parse("4242", " 2").Any());
    CHECK(!Parse("4242", "").Any());
    CHECK(!Parse("42x42", "2").Any());
    CHECK(!Parse("", "2").Any());
    CHECK(!Parse("-1", "2").Any());
    CHECK(!Parse("4242", "-1").Any());
}

TEST_CASE("An implausible count is refused", "[socket-activation]")
{
    // Not a policy limit -- systemd passes a handful. A count in the thousands
    // means the variable is not what it claims to be, and adopting that many
    // descriptors would wrap whatever this process legitimately had open above
    // fd 3 in listeners that then close them.
    CHECK(!Parse("4242", "100000").Any());
    CHECK(Parse("4242", "1024").Any());
    CHECK(!Parse("4242", "1025").Any());
}

TEST_CASE("A single descriptor is the ordinary case", "[socket-activation]")
{
    auto const handoff = Parse("4242", "1");
    REQUIRE(handoff.Any());
    CHECK(handoff.count == 1);
    CHECK(handoff.firstDescriptor == 3);
}
