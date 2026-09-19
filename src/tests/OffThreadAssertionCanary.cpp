// SPDX-License-Identifier: Apache-2.0
//
// A Catch2 program that must DIE: it asserts on a helper thread, which
// `OffThreadAssertionGuard` (#1211) ends the process for. Read by
// `scripts/off-thread-assertion-gate.cmake`, never run as an ordinary test.
//
// **It watches the guard ACCEPT first.** A guard that ended the process at every assertion
// would pass a refusal-only gate and break every test binary in the tree, so the case
// asserts on its own thread, and runs a helper thread that asserts nothing, before it
// asserts anywhere else -- and says so on stderr, in that order, for the gate to read.
//
// It links `Catch2::Catch2WithMain` and names no guard of its own, so the guard arrives the
// way it reaches every other test binary -- through the link -- and a canary that survived
// would also be saying the attachment is gone.
#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <print>
#include <thread>

TEST_CASE("An assertion off the case's thread ends the process", "[canary]")
{
    CHECK(true);
    std::jthread { [] {} }.join();
    std::println(std::cerr, "off-thread-assertion-canary: an assertion on the case's thread was ACCEPTED");

    std::println(std::cerr, "off-thread-assertion-canary: asserting from a helper thread");
    std::jthread { [] { CHECK(true); } }.join();

    std::println(std::cerr, "off-thread-assertion-canary: SURVIVED an assertion off the case's thread");
}
