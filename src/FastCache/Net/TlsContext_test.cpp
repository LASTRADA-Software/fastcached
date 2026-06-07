// SPDX-License-Identifier: Apache-2.0
//
// TLS tests are compiled only in TLS-enabled builds (FASTCACHED_ENABLE_TLS);
// in the default build this file is an empty translation unit.
#if defined(FC_TLS_ENABLED)

    #include <FastCache/Net/TlsContext.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <string>

namespace
{

/// Absolute path to a checked-in test fixture under testdata/tls/.
[[nodiscard]] std::string TlsFixture(char const* name)
{
    return std::string { FASTCACHED_TESTDATA_DIR } + "/tls/" + name;
}

} // namespace

TEST_CASE("TlsContext loads a valid certificate and key", "[tls]")
{
    auto context = FastCache::TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    REQUIRE((*context)->Native() != nullptr);
}

TEST_CASE("TlsContext fails on missing files", "[tls]")
{
    auto const context = FastCache::TlsContext::Create(TlsFixture("does-not-exist.crt"), TlsFixture("does-not-exist.key"));
    REQUIRE_FALSE(context.has_value());
}

TEST_CASE("TlsContext fails when the key does not match the certificate", "[tls]")
{
    // Feeding the certificate file where a private key is expected must fail
    // rather than silently producing a broken context.
    auto const context = FastCache::TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.crt"));
    REQUIRE_FALSE(context.has_value());
}

#endif // FC_TLS_ENABLED
