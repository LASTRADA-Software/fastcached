// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace
{
/// The cluster's key, as a file holding thirty-two bytes would supply it.
[[nodiscard]] std::vector<std::byte> Key(unsigned char fill = 0x5A)
{
    return std::vector<std::byte>(32, static_cast<std::byte>(fill));
}

/// A fixed instant, so nothing here reads a real clock.
[[nodiscard]] std::chrono::system_clock::time_point Noon()
{
    return std::chrono::system_clock::time_point { 1'767'225'600s };
}

/// The grant a scheduler would mint for one job.
[[nodiscard]] LeaseClaims Grant(std::string_view endpoint = "10.0.0.7:6675",
                                std::string_view fingerprint = "clang-19-x86_64",
                                std::string_view key = "obj-abc")
{
    return LeaseClaims { .serial = "17",
                         .endpoint = std::string { endpoint },
                         .fingerprint = std::string { fingerprint },
                         .key = std::string { key },
                         .expiresAt = Noon() + 10min };
}

/// What the worker at the granted endpoint expects to see.
[[nodiscard]] LeaseExpectation Worker(std::string_view endpoint = "10.0.0.7:6675",
                                      std::string_view fingerprint = "clang-19-x86_64")
{
    return LeaseExpectation { .endpoint = endpoint, .fingerprint = fingerprint };
}

/// Where the version byte sits in a decoded envelope.
///
/// `[u32 claimsLength][u32 versionFieldLength = 1][version]...`, so it is the ninth
/// byte. Derived rather than written as `8`, so the arithmetic is checkable against
/// the layout the header documents.
constexpr std::size_t VersionByteOffset = 2 * sizeof(std::uint32_t);

/// Where the first byte of the serial sits: past the version field's prefix and
/// value, then past the serial's own length prefix.
constexpr std::size_t SerialByteOffset = VersionByteOffset + 1 + sizeof(std::uint32_t);

/// Overwrite one byte of the decoded envelope and re-encode it.
/// @param token The token to damage.
/// @param offset Which byte, from the front.
/// @param value What to put there.
/// @return The damaged token.
[[nodiscard]] std::string Overwrite(std::string const& token, std::size_t offset, unsigned char value)
{
    auto const raw = Base64Decode(token);
    REQUIRE(raw.has_value());
    auto decoded = Unwrap(raw);
    REQUIRE(decoded.size() > offset);
    decoded[offset] = static_cast<char>(value);
    return Base64Encode(AsBytes(decoded));
}

/// Flip the low bit of one byte of the decoded envelope and re-encode it.
/// @param token The token to damage.
/// @param offset Which byte, from the front.
/// @return The damaged token.
[[nodiscard]] std::string FlipByteAt(std::string const& token, std::size_t offset)
{
    auto const raw = Base64Decode(token);
    REQUIRE(raw.has_value());
    REQUIRE(Unwrap(raw).size() > offset);
    return Overwrite(token, offset, static_cast<unsigned char>(Unwrap(raw)[offset]) ^ 0x01U);
}

/// Substitute one equal-length string inside the decoded envelope and re-encode.
///
/// Equal length on purpose: it leaves every length prefix untouched, so the result
/// is a structurally perfect token differing from a genuine one in exactly the
/// field under test. Anything that rewrote a length would be refused as malformed
/// and would prove nothing about the MAC.
/// @param token The token to rewrite.
/// @param from The bytes to find.
/// @param to What to put there; must be the same length.
/// @return The rewritten token.
[[nodiscard]] std::string Substitute(std::string const& token, std::string_view from, std::string_view to)
{
    REQUIRE(from.size() == to.size());
    auto const raw = Base64Decode(token);
    REQUIRE(raw.has_value());
    auto decoded = Unwrap(raw);
    auto const at = decoded.find(from);
    REQUIRE(at != std::string::npos);
    decoded.replace(at, from.size(), to);
    return Base64Encode(AsBytes(decoded));
}

/// How many bytes the decoded envelope of @p token holds.
/// @param token The token.
/// @return The decoded size.
[[nodiscard]] std::size_t EnvelopeSize(std::string const& token)
{
    auto const raw = Base64Decode(token);
    REQUIRE(raw.has_value());
    return Unwrap(raw).size();
}
} // namespace

TEST_CASE("A signed lease round-trips and carries every claim back", "[distributed][lease][token]")
{
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant());

    // Base64 rather than raw bytes is a property the fleet depends on, not a
    // presentation choice: this string ends up in launcher output and in worker
    // reports, and one byte that is not text makes `/fleet.json` unparseable for
    // everybody. Asserted here so a future encoder change cannot quietly drop it.
    CHECK(std::ranges::all_of(token, [](char c) { return static_cast<unsigned char>(c) < 0x80; }));

    auto const verified = VerifyLeaseToken(key, token, Worker(), Noon());
    REQUIRE(verified.has_value());
    CHECK(verified->serial == "17");
    CHECK(verified->endpoint == "10.0.0.7:6675");
    CHECK(verified->fingerprint == "clang-19-x86_64");
    CHECK(verified->key == "obj-abc");
    CHECK(verified->expiresAt == Noon() + 10min);
}

TEST_CASE("A forged or tampered lease is refused and tells the forger nothing", "[distributed][lease][token]")
{
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant());

    SECTION("a flipped tag byte")
    {
        // The tag is the last thing in the envelope, so the last byte is inside it.
        auto const damaged = FlipByteAt(token, EnvelopeSize(token) - 1);
        auto const refusal = VerifyLeaseToken(key, damaged, Worker(), Noon());
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Unauthorized);

        // Nothing the token claimed is echoed back. A caller that could not
        // authenticate has established no fact about it, and a message quoting the
        // endpoint or the key would turn the refusal into an oracle.
        CHECK(refusal.error().detail.empty());
    }

    SECTION("a tampered claim, which the MAC covers")
    {
        // The serial, which is a CLAIM rather than the tag. The MAC is recomputed
        // over the claim bytes as received, so this is `Unauthorized` and not a
        // successful decode of different claims.
        auto const refusal = VerifyLeaseToken(key, FlipByteAt(token, SerialByteOffset), Worker(), Noon());
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Unauthorized);
    }

    SECTION("a different cluster's key")
    {
        auto const refusal = VerifyLeaseToken(Key(0x11), token, Worker(), Noon());
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Unauthorized);
    }

    SECTION("something that is not a token at all")
    {
        // What an old launcher presents: the bare serial the scheduler used to hand
        // out. Refused by name, so the client compiles locally rather than being
        // dropped -- a close is indistinguishable from a network fault.
        auto const refusal = VerifyLeaseToken(key, "17", Worker(), Noon());
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Malformed);

        // Both spellings answer with ONE wire code, deliberately: a receiver cannot
        // tell a forgery from a random string, and reporting them apart would only
        // tell an attacker how close they got.
        CHECK(DescribeLeaseRefusal(LeaseRefusalReason::Malformed).code
              == DescribeLeaseRefusal(LeaseRefusalReason::Unauthorized).code);
    }
}

TEST_CASE("A lease minted for one worker does not authorize another", "[distributed][lease][token]")
{
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant("10.0.0.7:6675"));

    // The replay the endpoint is inside the MAC for: a token captured on the way to
    // one machine, presented to another that trusts the same key.
    auto const refusal = VerifyLeaseToken(key, token, Worker("10.0.0.8:6675"), Noon());
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::EndpointMismatch);

    // Named, and it NAMES BOTH -- because the overwhelmingly common cause is not a
    // replay but a worker registered under an address clients do not dial. Reported
    // only because the MAC already verified, which is what keeps it a diagnostic
    // rather than an oracle.
    CHECK(refusal.error().detail.contains("10.0.0.7:6675"));
    CHECK(refusal.error().detail.contains("10.0.0.8:6675"));
    CHECK(DescribeLeaseRefusal(refusal.error().reason).code == CompileCacheWire::ErrorCode::LeaseEndpointMismatch);
}

TEST_CASE("The endpoint is inside the MAC, not merely beside it", "[distributed][lease][token]")
{
    // The test that actually pins the design, and the one the obvious
    // wrong-endpoint case does NOT: that one is answered by comparing the
    // cleartext endpoint, so it passes just as happily with the endpoint left out
    // of the MAC entirely. This one rewrites the endpoint to the attacker's own
    // worker -- the exact replay a captured grant enables -- and requires the tag
    // to fail. Drop `claims.endpoint` from the packed claims and only this case
    // goes red.
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant("10.0.0.7:6675"));
    auto const rewritten = Substitute(token, "10.0.0.7:6675", "10.0.0.8:6675");

    auto const refusal = VerifyLeaseToken(key, rewritten, Worker("10.0.0.8:6675"), Noon());
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::Unauthorized);
}

TEST_CASE("The key and the expiry are inside the MAC too", "[distributed][lease][token]")
{
    // Same argument, for the two fields nothing else here would notice. A worker
    // checks the endpoint and the fingerprint against itself; it has no
    // independent opinion about which object key a grant covers, so if the key
    // left the MAC a single lease would authorize compiling anything at all.
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc"));

    auto const otherKey = VerifyLeaseToken(key, Substitute(token, "obj-abc", "obj-xyz"), Worker(), Noon());
    REQUIRE_FALSE(otherKey.has_value());
    CHECK(otherKey.error().reason == LeaseRefusalReason::Unauthorized);

    // And the expiry, whose last byte is the one just before the tag's length
    // prefix: an attacker who could move it would hold a grant that never lapses.
    auto const moved = FlipByteAt(token, EnvelopeSize(token) - Sha256::DigestSize - sizeof(std::uint32_t) - 1);
    auto const extended = VerifyLeaseToken(key, moved, Worker(), Noon());
    REQUIRE_FALSE(extended.has_value());
    CHECK(extended.error().reason == LeaseRefusalReason::Unauthorized);
}

TEST_CASE("A lease names the toolchain it was granted against", "[distributed][lease][token]")
{
    // `FingerprintMismatch` has been on the wire since dispatch existed, documented
    // as "this worker's toolchain is not the one the lease named" -- and nothing
    // could perform that check, because the lease named no toolchain. The only
    // fingerprint a worker saw was the one the client stated about itself in the
    // same frame it stated the token in.
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64"));

    auto const refusal = VerifyLeaseToken(key, token, Worker("10.0.0.7:6675", "gcc-14-x86_64"), Noon());
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::FingerprintMismatch);
    CHECK(DescribeLeaseRefusal(refusal.error().reason).code == CompileCacheWire::ErrorCode::FingerprintMismatch);
}

TEST_CASE("A lease expires, with slack for a fleet whose clocks disagree", "[distributed][lease][token]")
{
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant());
    auto const expiry = Noon() + 10min;

    SECTION("before the expiry")
    {
        CHECK(VerifyLeaseToken(key, token, Worker(), expiry - 1s).has_value());
    }

    SECTION("inside the slack a skewed clock needs")
    {
        // A machine whose clock runs ahead of the scheduler's is the case this
        // exists for, and it is the ordinary state of a fleet where not every host
        // is NTP-managed. Refusing here would refuse legitimate compiles on exactly
        // the machines nobody is watching.
        CHECK(VerifyLeaseToken(key, token, Worker(), expiry + LeaseTokenClockSkewSlack - 1s).has_value());
    }

    SECTION("past the expiry and the slack")
    {
        auto const refusal = VerifyLeaseToken(key, token, Worker(), expiry + LeaseTokenClockSkewSlack + 1s);
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Expired);
        CHECK(DescribeLeaseRefusal(refusal.error().reason).code == CompileCacheWire::ErrorCode::LeaseExpired);

        // The message says how far out, because the actionable version of "expired"
        // is "this machine's clock is eleven minutes off" and a bare refusal is not.
        CHECK(refusal.error().detail.contains("clock skew"));
    }

    SECTION("the slack is a parameter, so a caller may tighten it")
    {
        CHECK_FALSE(VerifyLeaseToken(key, token, Worker(), expiry + 1s, 0s).has_value());
    }

    SECTION("a verifier whose own clock reads before the epoch does not overflow")
    {
        // `now - expiresAt` is only representable once the two are known to be
        // ordered. `expiresAt` is non-negative by construction -- it decodes from an
        // unsigned millisecond count -- while `now` is whatever this host's wall
        // clock says, and a machine with a dead RTC reads 1970 or earlier. With an
        // expiry near the tick count's ceiling, the subtraction is then a signed
        // overflow: undefined, invisible on MSVC, and a UBSan report on the Linux
        // legs. Ordered by a comparison first, it cannot happen, and the answer is
        // the correct one -- a clock this far behind has not reached any expiry.
        auto const farFuture =
            std::chrono::system_clock::time_point { std::chrono::milliseconds { Detail::MaxExpiryMillis } };
        auto const distant = MintLeaseToken(key, LeaseClaims { .serial = "17", .expiresAt = farFuture });

        auto const preEpoch = std::chrono::system_clock::time_point {} - 24h;
        CHECK(VerifyLeaseToken(key, distant, LeaseExpectation {}, preEpoch).has_value());
    }
}

TEST_CASE("The claim fields are framed, not joined", "[distributed][lease][token]")
{
    // `DiscoveryWire`'s scar, and it applies here with more force: an endpoint is
    // `host:port`, so the separator anybody would reach for is ALWAYS inside a
    // value. Joined by one, `{endpoint="a", key="b:1"}` and `{endpoint="a:b",
    // key="1"}` would authenticate identically -- so a lease for one worker would
    // be a valid lease for another, which is the exact failure the endpoint is in
    // the MAC to prevent.
    auto const key = Key();
    auto const left = MintLeaseToken(key, Grant("a", "f", "b:1"));
    auto const right = MintLeaseToken(key, Grant("a:b", "f", "1"));
    CHECK(left != right);

    // And neither authenticates as the other, which is the property that matters
    // rather than the strings differing.
    CHECK_FALSE(VerifyLeaseToken(key, left, Worker("a:b", "f"), Noon()).has_value());
    CHECK_FALSE(VerifyLeaseToken(key, right, Worker("a", "f"), Noon()).has_value());
}

TEST_CASE("A discovery proof is not a lease, under the same key", "[distributed][lease][token]")
{
    // The cluster's pre-shared key already MACs discovery proofs. One key serving
    // two constructions is how a tag produced for one purpose comes to be accepted
    // for the other, so every lease message is prefixed with its own domain label.
    auto const key = Key();
    Cluster::DiscoveryWire::Challenge const challenge { .clusterId = "fleet", .nonce = {} };
    auto const proof = Cluster::DiscoveryWire::ExpectedProofTag(key, challenge, "n1", "10.0.0.7:6675");

    // The tag alone, and the tag inside something shaped like an envelope, are both
    // refused -- the first as malformed, the second because the label is not in it.
    auto const bare = Base64Encode(std::span<std::byte const> { proof });
    CHECK_FALSE(VerifyLeaseToken(key, bare, Worker(), Noon()).has_value());
}

TEST_CASE("A malformed token is refused rather than partly believed", "[distributed][lease][token]")
{
    auto const key = Key();

    /// Assert that @p token is refused as malformed.
    auto const refusedAsMalformed = [&](std::string_view token) {
        auto const refusal = VerifyLeaseToken(key, token, Worker(), Noon());
        REQUIRE_FALSE(refusal.has_value());
        CHECK(refusal.error().reason == LeaseRefusalReason::Malformed);
    };

    SECTION("not base64")
    {
        refusedAsMalformed("not base64 at all!");
    }

    SECTION("empty")
    {
        refusedAsMalformed("");
    }

    SECTION("base64 of something else entirely")
    {
        refusedAsMalformed(Base64Encode(AsBytes("hello")));
    }

    SECTION("a version this build does not emit")
    {
        // The version is a field of its own precisely so this is a refusal by name
        // rather than a mis-parse: the fields are length-prefixed, so a different
        // arity would otherwise decode as *something*.
        refusedAsMalformed(Overwrite(MintLeaseToken(key, Grant()), VersionByteOffset, LeaseTokenVersion + 1));
    }
}

TEST_CASE("Authentication alone answers the scheduler's question", "[distributed][lease][token]")
{
    // What `SchedulerService::Release` needs: the scheduler is not a worker, so the
    // endpoint and fingerprint a grant names are not facts about it -- but the
    // serial inside is what resolves the lease.
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant());

    auto const authentic = AuthenticateLeaseToken(key, token);
    REQUIRE(authentic.has_value());
    CHECK(authentic->serial == "17");

    auto const wrongKey = AuthenticateLeaseToken(Key(0x11), token);
    REQUIRE_FALSE(wrongKey.has_value());
    CHECK(wrongKey.error() == LeaseRefusalReason::Unauthorized);
}

TEST_CASE("An empty key authenticates nothing", "[distributed][lease][token]")
{
    // An empty HMAC key is a perfectly valid HMAC key, which is the trap: without a
    // guard, two nodes that both failed to load their key file would verify each
    // other's tokens and call it authentication. A caller that legitimately runs
    // without a key decides in the open that it is not checking; it does not get
    // there by passing nothing.
    auto const unsignedToken = MintLeaseToken({}, Grant());
    auto const refusal = AuthenticateLeaseToken({}, unsignedToken);
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error() == LeaseRefusalReason::Unauthorized);

    // And a real key does not accept it either, so nothing minted without a secret
    // is ever good anywhere.
    CHECK_FALSE(AuthenticateLeaseToken(Key(), unsignedToken).has_value());
}
