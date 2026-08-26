// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostMemory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_CASE("The default memory budget is a whole number of MiB", "[platform][hostmemory]")
{
    constexpr std::size_t Mib = 1024U * 1024U;
    auto const budget = FastCache::DefaultMaxMemoryBytes();

    // Not cosmetics. `FormatByteSize` is exact by design -- what it prints can be
    // handed straight back as a flag value -- so a byte-exact quarter of RAM has no
    // whole unit, and the startup line telling an operator how big their cache is
    // reads `4115271K` rather than `4020M`. A cache budget is a coarse quantity, and
    // byte precision about a fraction of a machine is precision about nothing.
    CHECK(budget % Mib == 0);
}

TEST_CASE("The default memory budget stays inside its bounds and never vanishes", "[platform][hostmemory]")
{
    auto const budget = FastCache::DefaultMaxMemoryBytes();

    // The clamp is what makes a fraction safe at both ends: a small laptop still
    // gets a cache worth having, and a very large build server does not silently
    // take a fixed share of its RAM resident for one. Never zero either -- zero is
    // how the store underneath spells *unbounded*, so a budget that fell to it would
    // turn a ceiling into its absence.
    // `ULL`, not `U`: 8 * 1024^3 overflows a 32-bit `unsigned int` and wraps to
    // zero, which turns the ceiling assertion into `budget <= 0` -- a check that
    // fails on every machine for a reason that has nothing to do with the budget.
    constexpr std::size_t Floor = 512ULL * 1024 * 1024;
    constexpr std::size_t Ceiling = 8ULL * 1024 * 1024 * 1024;
    CHECK(budget >= Floor);
    CHECK(budget <= Ceiling);
    CHECK(budget > 0U);

    // Memoised, which is why it is safe as a default member initialiser -- and every
    // `Config` and `NodeConfig` in the process is one, including the throwaway
    // `defaults` instance the service-spec emitters diff against.
    CHECK(FastCache::DefaultMaxMemoryBytes() == budget);
}
