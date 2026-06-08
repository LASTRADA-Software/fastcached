// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Auth/AuthPolicy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace FastCache;

TEST_CASE("ConstantTimeEquals matches and mismatches", "[auth]")
{
    CHECK(ConstantTimeEquals("", ""));
    CHECK(ConstantTimeEquals("secret", "secret"));
    CHECK_FALSE(ConstantTimeEquals("secret", "Secret"));
    CHECK_FALSE(ConstantTimeEquals("secret", "secre"));   // length mismatch (shorter)
    CHECK_FALSE(ConstantTimeEquals("secret", "secrett")); // length mismatch (longer)
    CHECK_FALSE(ConstantTimeEquals("", "x"));
    // Embedded NUL bytes are compared, not treated as terminators.
    CHECK(ConstantTimeEquals(std::string_view { "a\0b", 3 }, std::string_view { "a\0b", 3 }));
    CHECK_FALSE(ConstantTimeEquals(std::string_view { "a\0b", 3 }, std::string_view { "a\0c", 3 }));
}

TEST_CASE("AuthPolicy disabled when secret is empty", "[auth]")
{
    AuthPolicy const policy { "default", "" };
    CHECK_FALSE(policy.Enabled());
    CHECK(policy.Username() == "default");
}

TEST_CASE("AuthPolicy single-secret verify (requirepass form)", "[auth]")
{
    AuthPolicy const policy { "default", "hunter2" };
    CHECK(policy.Enabled());
    CHECK(policy.Verify("hunter2"));
    CHECK_FALSE(policy.Verify("hunter3"));
    CHECK_FALSE(policy.Verify(""));
}

TEST_CASE("AuthPolicy username/password verify (ACL / SASL form)", "[auth]")
{
    AuthPolicy const policy { "alice", "hunter2" };
    CHECK(policy.Verify("alice", "hunter2"));
    CHECK_FALSE(policy.Verify("alice", "wrong"));
    CHECK_FALSE(policy.Verify("bob", "hunter2"));
    CHECK_FALSE(policy.Verify("bob", "wrong"));
}
