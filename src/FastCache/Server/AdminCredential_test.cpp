// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/AdminCredential.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using FastCache::AdminAuthSchemes;
using FastCache::AdminCredential;

TEST_CASE("An admin surface with no credential serves anybody who reaches it", "[admin][credential]")
{
    // The state that keeps `--admin-listen` byte-identical for every operator who
    // already points a scraper at it: no token file, no credential, nothing
    // changes. It is a constructed state rather than an empty secret, because an
    // empty secret would compare equal to a client that sent nothing.
    AdminCredential const open;
    CHECK_FALSE(open.Required());
    CHECK(open.Accepts(""));
    CHECK(open.Accepts("Bearer whatever"));
}

TEST_CASE("A bearer token is accepted only when it matches", "[admin][credential]")
{
    AdminCredential const guarded { "s3cret-token" };
    CHECK(guarded.Required());

    CHECK(guarded.Accepts("Bearer s3cret-token"));
    CHECK(guarded.Accepts("bearer s3cret-token")); // the scheme is case-insensitive
    CHECK_FALSE(guarded.Accepts("Bearer wrong"));
    CHECK_FALSE(guarded.Accepts("Bearer "));
    CHECK_FALSE(guarded.Accepts(""));
    CHECK_FALSE(guarded.Accepts("s3cret-token")); // no scheme at all
}

TEST_CASE("A browser's Basic credential is accepted whatever username it sends", "[admin][credential]")
{
    // Why `Basic` is a row at all: there is no browser prompt for `Bearer`, so a
    // page meant to be opened from a laptop would be unreachable without pasting a
    // token into a URL. The token file holds one secret, so the username half is
    // ignored -- demanding a matching one would be a second secret nobody was given.
    AdminCredential const guarded { "s3cret-token" };

    CHECK(guarded.Accepts("Basic OnMzY3JldC10b2tlbg=="));     // ":s3cret-token"
    CHECK(guarded.Accepts("Basic YWRtaW46czNjcmV0LXRva2Vu")); // "admin:s3cret-token"
    CHECK(guarded.Accepts("basic YWRtaW46czNjcmV0LXRva2Vu")); // case-insensitive
    CHECK_FALSE(guarded.Accepts("Basic YWRtaW46d3Jvbmc="));   // "admin:wrong"
    CHECK_FALSE(guarded.Accepts("Basic YWRtaW4="));           // "admin", no colon
}

TEST_CASE("A malformed credential is refused rather than retried as another scheme", "[admin][credential]")
{
    // `Basic` with unreadable base64 must not fall through and be compared
    // verbatim as a bearer token: that would let a malformed header reach the
    // secret comparison with attacker-chosen bytes.
    AdminCredential const guarded { "s3cret-token" };

    CHECK_FALSE(guarded.Accepts("Basic !!!not-base64!!!"));
    CHECK_FALSE(guarded.Accepts("Basic s3cret-token"));
    CHECK_FALSE(guarded.Accepts("Digest s3cret-token")); // a scheme with no row
}

TEST_CASE("Every auth scheme has a name and a way to read its parameter", "[admin][credential]")
{
    // Asserted over the table rather than against a list beside it, so a scheme
    // added without an extractor is a failing test rather than a silent refusal
    // of every credential presented under it.
    for (auto const& row: AdminAuthSchemes())
    {
        INFO("scheme " << row.name);
        CHECK_FALSE(row.name.empty());
        CHECK(row.extract != nullptr);
    }
}
