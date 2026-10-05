// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/InheritedListener.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#if !defined(_WIN32)
    #include <fcntl.h>
    #include <unistd.h>
#endif

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

TEST_CASE("A copy of an inherited descriptor is the same open file that closes apart and that no child inherits",
          "[socket-activation]")
{
    // What a process keeping the supervisor's socket hands each user: a second descriptor for the same
    // open file, which its taker may close without closing the original, and which a spawned compiler
    // does not inherit -- the original's close-on-exec, kept.
    SystemInheritedDescriptors const descriptors;
#if defined(_WIN32)
    // Nothing is ever handed over here, so there is nothing to copy -- and asking says so.
    CHECK_FALSE(descriptors.Duplicate(3).has_value());
#else
    auto ends = std::array<int, 2> {};
    REQUIRE(::pipe(ends.data()) == 0);
    auto const [readEnd, writeEnd] = ends;

    auto const copy = descriptors.Duplicate(readEnd);
    REQUIRE(copy.has_value());
    CHECK(*copy != readEnd);
    CHECK((::fcntl(*copy, F_GETFD) & FD_CLOEXEC) != 0);

    // The same open file: what is written to the pipe is read through the copy.
    auto const byte = std::array<char, 1> { 'x' };
    REQUIRE(::write(writeEnd, byte.data(), byte.size()) == 1);
    auto read = std::array<char, 1> {};
    CHECK(::read(*copy, read.data(), read.size()) == 1);
    CHECK(read[0] == 'x');

    // Closed apart: the original still reads once the copy is gone.
    descriptors.Close(*copy);
    REQUIRE(::write(writeEnd, byte.data(), byte.size()) == 1);
    CHECK(::read(readEnd, read.data(), read.size()) == 1);

    descriptors.Close(readEnd);
    descriptors.Close(writeEnd);
#endif
}
