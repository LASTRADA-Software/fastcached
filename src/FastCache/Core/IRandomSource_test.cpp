// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/IRandomSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

using namespace FastCache;

TEST_CASE("A scripted source serves its draws in order and then cycles", "[core][random]")
{
    ScriptedRandomSource source { { 10, 20, 30 } };

    CHECK(source.UniformInRange(0, 100) == 10);
    CHECK(source.UniformInRange(0, 100) == 20);
    CHECK(source.UniformInRange(0, 100) == 30);
    CHECK(source.UniformInRange(0, 100) == 10);
    CHECK(source.DrawCount() == 4);
}

TEST_CASE("A scripted draw is clamped into the requested range", "[core][random]")
{
    // Clamping is what makes the two cases a test actually wants expressible
    // without knowing the range where the script is written.
    ScriptedRandomSource source { { 0, ScriptedRandomSource::Highest(), 55 } };

    CHECK(source.UniformInRange(10, 90) == 10); // 0 clamps to the low bound
    CHECK(source.UniformInRange(10, 90) == 90); // Highest() clamps to the high bound
    CHECK(source.UniformInRange(10, 90) == 55); // in range, passed through
}

TEST_CASE("An empty script always draws the low bound", "[core][random]")
{
    ScriptedRandomSource source;

    CHECK(source.UniformInRange(7, 9) == 7);
    CHECK(source.UniformInRange(0, 1000) == 0);
    CHECK(source.DrawCount() == 2);
}

TEST_CASE("An inverted range collapses to the low bound rather than misbehaving", "[core][random]")
{
    // `assert` is compiled out in every shipped preset, so without this the
    // inverted range would reach std::uniform_int_distribution and std::clamp,
    // both of which are undefined when the bounds cross -- libstdc++ underflows
    // `high - low` to nearly 2^64 and returns a value outside the requested range.
    // Reached from configuration (a Raft election-timeout minimum above its
    // maximum), so it is a caller bug that arrives from a file.
    ScriptedRandomSource scripted { { 500 } };
    SystemRandomSource system { 4242 };

    CHECK(scripted.UniformInRange(90, 10) == 90);

    for (auto i = 0; i < 100; ++i)
        CHECK(system.UniformInRange(90, 10) == 90);
}

TEST_CASE("Equal bounds yield that one value", "[core][random]")
{
    ScriptedRandomSource scripted { { 4 } };
    SystemRandomSource system { 12345 };

    CHECK(scripted.UniformInRange(42, 42) == 42);
    CHECK(system.UniformInRange(42, 42) == 42);
}

TEST_CASE("The system source stays inside the requested range", "[core][random]")
{
    SystemRandomSource source { 0xC0FFEE };

    for (auto i = 0; i < 500; ++i)
    {
        auto const drawn = source.UniformInRange(100, 110);
        CHECK(drawn >= 100);
        CHECK(drawn <= 110);
    }
}

TEST_CASE("The system source is reproducible from a seed", "[core][random]")
{
    // Which is what lets a failure that depended on a particular draw sequence be
    // replayed outside the scripted double.
    SystemRandomSource first { 99 };
    SystemRandomSource second { 99 };

    auto const drawsOf = [](SystemRandomSource& source) {
        auto values = std::vector<std::uint64_t> {};
        for (auto i = 0; i < 16; ++i)
            values.push_back(source.UniformInRange(0, 1'000'000));
        return values;
    };

    CHECK(drawsOf(first) == drawsOf(second));
}

TEST_CASE("The system source actually varies across a range", "[core][random]")
{
    // A source that always returned the low bound would satisfy every bound check
    // above while making Raft's randomized election timeouts useless -- split
    // votes would then repeat indefinitely rather than resolving.
    SystemRandomSource source { 7 };
    auto seen = std::set<std::uint64_t> {};

    for (auto i = 0; i < 200; ++i)
        seen.insert(source.UniformInRange(0, 99));

    CHECK(seen.size() > 50);
}
