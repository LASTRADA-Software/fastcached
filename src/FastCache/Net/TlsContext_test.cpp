// SPDX-License-Identifier: Apache-2.0
//
// TLS tests are compiled only in TLS-enabled builds (FASTCACHED_ENABLE_TLS);
// in the default build this file is an empty translation unit.
#if defined(FC_TLS_ENABLED)

    #include <FastCache/Net/TlsContext.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <string>
    #include <vector>

    #include <openssl/ssl.h>
    #include <openssl/x509v3.h>

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

TEST_CASE("A generated self-signed certificate is valid for the names it was given", "[net][tls][self-signed]")
{
    // The names are what a client actually checks: every modern client ignores the
    // common name entirely, so a certificate whose SAN does not list the name an
    // operator types matches nothing however it is labelled.
    std::vector<std::string> const names { "localhost", "127.0.0.1", "::1", "buildnode-3.internal" };

    auto const context = FastCache::TlsContext::CreateSelfSigned(names);
    REQUIRE(context.has_value());
    REQUIRE((*context)->Native() != nullptr);

    auto* const cert = SSL_CTX_get0_certificate((*context)->Native());
    REQUIRE(cert != nullptr);

    // Asked the way a client asks, rather than by reading the extension back: what
    // matters is whether a peer would accept the name, not how it was encoded.
    CHECK(X509_check_host(cert, "localhost", 0, 0, nullptr) == 1);
    CHECK(X509_check_host(cert, "buildnode-3.internal", 0, 0, nullptr) == 1);
    CHECK(X509_check_ip_asc(cert, "127.0.0.1", 0) == 1);
    CHECK(X509_check_ip_asc(cert, "::1", 0) == 1);

    // And not valid for a name nobody asked for, which is the half that says the
    // SAN is a list rather than a wildcard.
    CHECK(X509_check_host(cert, "elsewhere.internal", 0, 0, nullptr) != 1);
}

TEST_CASE("A name is classified by what it parses as, not by how it looks", "[net][tls][self-signed]")
{
    // Guessing from the spelling -- a digit at the front, a colon somewhere --
    // gets a host called `10things` and the address `2001:db8::1` wrong in
    // opposite directions, and the certificate then silently does not match the
    // name an operator types.
    std::vector<std::string> const names { "10things", "2001:db8::1" };

    auto const context = FastCache::TlsContext::CreateSelfSigned(names);
    REQUIRE(context.has_value());
    auto* const cert = SSL_CTX_get0_certificate((*context)->Native());
    REQUIRE(cert != nullptr);

    CHECK(X509_check_host(cert, "10things", 0, 0, nullptr) == 1);
    CHECK(X509_check_ip_asc(cert, "2001:db8::1", 0) == 1);
}

TEST_CASE("A self-signed certificate with no name at all is refused", "[net][tls][self-signed]")
{
    // A certificate valid for no name matches nothing, so every client rejects it:
    // an encrypted surface nobody can reach, which reads as a network fault rather
    // than as a misconfiguration.
    std::vector<std::string> const none {};
    auto const refused = FastCache::TlsContext::CreateSelfSigned(none);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context.contains("subject name"));

    // And an entry that is only whitespace does not count as one.
    std::vector<std::string> const empty { "" };
    CHECK_FALSE(FastCache::TlsContext::CreateSelfSigned(empty).has_value());
}

TEST_CASE("A generated certificate reports a fingerprint, and a fresh one each time", "[net][tls][self-signed]")
{
    // The fingerprint is the ONLY thing that authenticates a self-signed
    // certificate -- nothing signs it -- so an operator compares what the node
    // logged against what their browser shows. That is the difference between no
    // authentication at all and authentication out of band.
    std::vector<std::string> const names { "localhost" };

    auto const first = FastCache::TlsContext::CreateSelfSigned(names);
    auto const second = FastCache::TlsContext::CreateSelfSigned(names);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    auto const one = (*first)->CertificateFingerprint();
    CHECK(one.size() == 64); // SHA-256, lower-case hex
    CHECK(one.find_first_not_of("0123456789abcdef") == std::string::npos);

    // Two contexts are two keypairs. This is also the documented cost of holding
    // the certificate in memory: a browser exception pinned to one does not
    // survive a restart.
    CHECK(one != (*second)->CertificateFingerprint());
}

TEST_CASE("A certificate loaded from a file also reports its fingerprint", "[net][tls][self-signed]")
{
    // The accessor is not special to generated certificates: an operator serving a
    // named certificate can check they are serving the one they think they are.
    auto const context = FastCache::TlsContext::Create(TlsFixture("server.crt"), TlsFixture("server.key"));
    REQUIRE(context.has_value());
    CHECK((*context)->CertificateFingerprint().size() == 64);
}

#endif // FC_TLS_ENABLED
