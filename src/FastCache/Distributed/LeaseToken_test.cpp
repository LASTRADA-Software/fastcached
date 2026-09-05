// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>
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
/// The fleet these grants belong to.
///
/// Named rather than empty, so every case exercises a real comparison. Empty on both
/// sides passes whether the check runs or not, which is exactly the shape that would
/// let a verifier ship without looking (#322).
inline constexpr std::string_view TestCluster = "fleet-alpha";

/// A second fleet, provisioned from the SAME key -- which is what copying a working
/// configuration to another site produces, and the whole scenario #322 is about.
inline constexpr std::string_view OtherCluster = "fleet-beta";

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
                                std::string_view key = "obj-abc",
                                std::string_view cluster = TestCluster,
                                std::uint64_t epoch = 7)
{
    return LeaseClaims { .serial = "17",
                         .endpoint = std::string { endpoint },
                         .fingerprint = std::string { fingerprint },
                         .key = std::string { key },
                         .expiresAt = Noon() + 10min,
                         .clusterId = std::string { cluster },
                         .epoch = epoch };
}

/// What the worker at the granted endpoint expects to see.
/// @param endpoint The address this worker answers on.
/// @param fingerprint The toolchain it is about to run.
/// @param cluster The fleet it belongs to.
/// @param epoch What it knows about the current scheduler term.
/// @return The expectation.
[[nodiscard]] LeaseExpectation Worker(std::string_view endpoint = "10.0.0.7:6675",
                                      std::string_view fingerprint = "clang-19-x86_64",
                                      std::string_view cluster = TestCluster,
                                      LeaseEpochCheck epoch = LeaseEpochCheck::NotKnownHere())
{
    return LeaseExpectation { .endpoint = endpoint, .fingerprint = fingerprint, .clusterId = cluster, .epoch = epoch };
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
        auto const distant = MintLeaseToken(key,
                                            LeaseClaims { .serial = "17",
                                                          .endpoint = "10.0.0.7:6675",
                                                          .fingerprint = "clang-19-x86_64",
                                                          .key = "obj-abc",
                                                          .expiresAt = farFuture,
                                                          .clusterId = std::string { TestCluster } });

        auto const preEpoch = std::chrono::system_clock::time_point {} - 24h;
        CHECK(VerifyLeaseToken(key, distant, Worker(), preEpoch).has_value());
    }
}

TEST_CASE("A grant from another fleet sharing the key is refused", "[distributed][lease][token]")
{
    // #322's whole scenario, and the reason it is not exotic: two clusters
    // provisioned from the same `--cluster-key-file` is what copying a working
    // configuration to a second site produces, or cloning staging from production.
    // The MAC verifies, the endpoint matches, the fingerprint matches and the expiry
    // is in the future -- so before the cluster id went inside the MAC, cluster B's
    // worker compiled work leased by a scheduler that was not its own.
    auto const key = Key();
    auto const foreign = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", OtherCluster));

    auto const refusal = VerifyLeaseToken(key, foreign, Worker(), Noon());
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::ClusterMismatch);

    // Both fleets named, because the operator action is to look at two configurations
    // and find out which one is wrong. This is reported only AFTER the MAC verified,
    // so it tells a forger nothing it did not already hand over.
    CHECK(refusal.error().detail.contains(OtherCluster));
    CHECK(refusal.error().detail.contains(TestCluster));
}

TEST_CASE("Two nodes that name no cluster still agree", "[distributed][lease][token]")
{
    // The one-machine deployment, and it must keep working. The comparison is for
    // EQUALITY rather than for presence, so "neither named one" is a match -- a rule
    // spelled as "the grant must name a cluster" would refuse every node that never
    // configured one.
    auto const key = Key();
    auto const grant = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", ""));

    CHECK(VerifyLeaseToken(key, grant, Worker("10.0.0.7:6675", "clang-19-x86_64", ""), Noon()).has_value());
}

TEST_CASE("A scheduler reset is refused permanently and the refusal says so", "[distributed][lease][token]")
{
    // **What a legitimate reset looks like from a worker**
    // ([#614](https://github.com/LASTRADA-Software/fastcached/issues/614)). Wiping the
    // Raft directory, re-bootstrapping, or turning consensus off drops a scheduler's
    // term back to 0. The term-0 answer is then genuine, signed and from the right
    // scheduler -- authenticity is not the issue -- but a monotonic maximum has no way
    // to express it, so every worker that has learned a higher term refuses every
    // grant until its process is restarted.
    //
    // This case pins the two halves separately, because the ticket is right about one
    // and out of date about the other.
    auto const key = Key();

    KnownSchedulerTerm term;
    term.Learn(7);

    // A scheduler that has genuinely reset mints at term 0. `StandaloneSchedulerTerm`
    // is literally 0, so this is also exactly what turning consensus OFF produces.
    auto const afterReset = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 0));
    auto const refusal =
        VerifyLeaseToken(key, afterReset, Worker("10.0.0.7:6675", "clang-19-x86_64", TestCluster, term.Check()), Noon());

    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::EpochMismatch);

    // **The diagnostic the ticket says is missing is already here**, and it names both
    // numbers -- which is what turns "the fleet stopped distributing" into "the
    // scheduler was reset and these workers need restarting". It reaches the operator
    // through the client: `WorkerProtocol` returns the refusal whole, deliberately, and
    // the launcher renders it in the decline line. Asserted so a later edit cannot
    // quietly drop it back to the bare reason.
    CHECK(refusal.error().detail.contains("term 0"));
    CHECK(refusal.error().detail.contains("term 7"));

    // **And there is no downward path, which is the half that stands.** Learning the
    // reset term changes nothing: `Learn` takes the maximum, so the worker goes on
    // expecting 7 and refusing every honest grant. This is monotonicity working as
    // designed -- it is what stops a captured token talking a worker backwards -- and
    // it is also why a legitimate reset has no expression. Pinned rather than
    // corrected: which shape closes it is a design decision the ticket declines to
    // make, and a test that quietly permitted a lower term would make that decision by
    // accident.
    term.Learn(0);
    CHECK(term.Check().Expected() == 7);

    auto const stillRefused =
        VerifyLeaseToken(key, afterReset, Worker("10.0.0.7:6675", "clang-19-x86_64", TestCluster, term.Check()), Noon());
    REQUIRE_FALSE(stillRefused.has_value());
    CHECK(stillRefused.error().reason == LeaseRefusalReason::EpochMismatch);
}

TEST_CASE("A grant from a superseded scheduler term is refused where the term is known", "[distributed][lease][token]")
{
    // The second half of #322: the cluster id closes the door between two FLEETS, the
    // epoch closes it between two leaders of the same fleet. Without it a token
    // captured before an election stays good after one, because nothing in the grant
    // said which term issued it.
    auto const key = Key();
    auto const old = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 6));

    auto const refusal = VerifyLeaseToken(
        key, old, Worker("10.0.0.7:6675", "clang-19-x86_64", TestCluster, LeaseEpochCheck::NotOlderThan(7)), Noon());
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error().reason == LeaseRefusalReason::EpochMismatch);

    // And the current term is served.
    auto const current = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 7));
    CHECK(
        VerifyLeaseToken(
            key, current, Worker("10.0.0.7:6675", "clang-19-x86_64", TestCluster, LeaseEpochCheck::NotOlderThan(7)), Noon())
            .has_value());

    // And a LATER term is served too, which is the half #421 added and the half a
    // `MustEqual` reading would have refused. A worker whose heartbeat is stale is the
    // one that is behind; refusing here would make it reject the new leader, which is
    // the fleet ceasing to distribute right after an election. Pinned one term either
    // side of the boundary, because that is where a comparison mistake lives.
    auto const later = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 8));
    CHECK(VerifyLeaseToken(
              key, later, Worker("10.0.0.7:6675", "clang-19-x86_64", TestCluster, LeaseEpochCheck::NotOlderThan(7)), Noon())
              .has_value());
}

TEST_CASE("What a worker knows about the term is three states, and zero is not one of them",
          "[distributed][lease][token][epoch]")
{
    // `SchedulerService::StandaloneSchedulerTerm` IS `0` -- the term of a node leading
    // alone -- and its own declaration warns that "a literal at a call site is exactly
    // how somebody later reads it as unknown and starts treating it as one". So
    // never-learned is a FLAG on `KnownSchedulerTerm` rather than a term value, and
    // this pins the difference the warning is about: a worker that has learned term 0
    // is checking, and one that has learned nothing is not. The two differ in
    // `Checked()` and in nothing else a caller can see.
    //
    // Named rather than included: `SchedulerService.hpp` is a scheduler header and
    // this is a token test, and reaching for it here collides with an unrelated
    // `Detail` namespace. The zero below is the constant's value, which is the case's
    // input -- writing it is not the mistake, reading it as "unknown" would be.
    KnownSchedulerTerm fresh;
    CHECK_FALSE(fresh.Check().Checked());
    CHECK(fresh.Check().Accepts(0));
    CHECK(fresh.Check().Accepts(9'999));

    KnownSchedulerTerm standalone;
    standalone.Learn(0);
    CHECK(standalone.Check().Checked());
    CHECK(standalone.Check().Expected() == 0);
    CHECK(standalone.Check().Accepts(0));
}

TEST_CASE("A worker's knowledge of the term only ever moves forward", "[distributed][lease][token][epoch]")
{
    // Monotonic, so the order two channels happen to write in cannot matter: a
    // heartbeat reply and an authentic grant both teach this, on different threads,
    // and a stale message overtaking a fresh one must not walk the expectation
    // backwards. Raft terms only increase within a cluster and the MAC binds the
    // cluster, so a lower number arriving is late rather than authoritative.
    KnownSchedulerTerm term;
    term.Learn(7);
    CHECK(term.Check().Expected() == 7);

    term.Learn(4);
    CHECK(term.Check().Expected() == 7);
    CHECK_FALSE(term.Check().Accepts(6));

    term.Learn(9);
    CHECK(term.Check().Expected() == 9);
    CHECK_FALSE(term.Check().Accepts(8));
    CHECK(term.Check().Accepts(9));
    CHECK(term.Check().Accepts(10));
}

TEST_CASE("A verifier that cannot know the term accepts any of them", "[distributed][lease][token]")
{
    // `NotKnownHere` is an answer rather than a placeholder, and this pins what it
    // means so that changing it is a decision somebody makes rather than a default
    // somebody inherits. A worker learns the current term from nowhere -- the only
    // term it sees is the one inside the token it is checking. #421 gave it a channel
    // -- the heartbeat reply, and any authentic grant naming a later term -- but a
    // worker that has used neither yet is in exactly this state, and a worker that
    // refused here could not cold-start.
    auto const key = Key();
    for (auto const epoch: { std::uint64_t { 0 }, std::uint64_t { 1 }, std::uint64_t { 9'999 } })
    {
        INFO("epoch " << epoch);
        auto const grant = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, epoch));
        CHECK(VerifyLeaseToken(key, grant, Worker(), Noon()).has_value());
    }
}

TEST_CASE("The cluster and the epoch are inside the MAC, not beside it", "[distributed][lease][token]")
{
    // The property the whole ticket rests on: an attacker holding a token cannot edit
    // either field, because both are covered. Asserted by MINTING two grants that
    // differ only in those fields and checking the tags differ -- a field that were
    // merely carried would produce the same tag twice, which is what "beside it" would
    // look like and would pass every functional case above.
    auto const key = Key();
    auto const base = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 7));
    auto const otherCluster = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", OtherCluster, 7));
    auto const otherEpoch = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 8));

    CHECK(base != otherCluster);
    CHECK(base != otherEpoch);
    CHECK(otherCluster != otherEpoch);
}

TEST_CASE("A version-1 token no longer authenticates", "[distributed][lease][token]")
{
    // The cost of covering two more fields, stated rather than discovered. Every
    // outstanding grant minted by an older build stops verifying, which is exactly why
    // #322 argued for doing this while the format is new: after a fleet is deployed it
    // is a flag day, and nothing is deployed.
    //
    // Refused as `Malformed` rather than `Unauthorized`, which is the version field
    // doing its job: the arity differs, so without it the fields would decode as
    // *something* and the refusal would name the wrong problem.
    CHECK(LeaseTokenVersion == 2);

    auto const key = Key();
    auto const claims = Grant();
    auto const packedV1 = Detail::PackClaims(1, claims);
    auto const tag =
        Cluster::SignFields(key, Cluster::SigningDomain::LeaseToken, { std::span<std::byte const> { packedV1 } });
    auto const envelope =
        WireFields::Encode({ std::span<std::byte const> { packedV1 }, std::span<std::byte const> { tag } });

    auto const refusal = AuthenticateLeaseToken(key, Base64Encode(envelope));
    REQUIRE_FALSE(refusal.has_value());
    CHECK(refusal.error() == LeaseRefusalReason::Malformed);
}

TEST_CASE("The worker says once that it is refusing every grant on the term", "[distributed][lease][token]")
{
    // **The signal this replaces is WRONG, not missing** (#614). On the worker a
    // scheduler reset showed only as `WorkerLeaseStaleEpoch` climbing, which reads like
    // an election storm -- so an operator whose fleet stopped distributing was pointed
    // at consensus instability when the cause was a reset they performed themselves.
    //
    // It settles nothing about the downward path, which is still open. This is the
    // observability half, true under every shape that question could be answered with.
    std::vector<std::string> said;
    LeaseEpochNotice notice { [&said](std::string_view line) { said.emplace_back(line); } };

    auto const reset = LeaseRefusal { .reason = LeaseRefusalReason::EpochMismatch,
                                      .detail = "this lease was issued under scheduler term 0; this worker has "
                                                "seen term 7" };

    SECTION("it reports once, however many compiles arrive")
    {
        // A worker refusing thousands of compiles must say this once. The whole point
        // is a line an operator reads, and a line per refused compile is not one.
        CHECK(notice.Observe(reset));
        for (auto attempt = 0; attempt < 500; ++attempt)
            CHECK_FALSE(notice.Observe(reset));

        REQUIRE(said.size() == 1);

        // It carries the refusal's own numbers -- which is what turns "the fleet
        // stopped" into "the scheduler was reset" -- and names the remedy, because an
        // operator who has just learned the condition still needs to be told that it
        // clears on a restart and not on its own.
        CHECK(said.front().contains("term 0"));
        CHECK(said.front().contains("term 7"));
        CHECK(said.front().contains("restart"));
    }

    SECTION("an accepted grant re-arms it, so a later reset is not silent")
    {
        // **Latched per CONDITION, not per process**, and this is the case that
        // separates the two. Once-per-process would spend the line on the first stale
        // token to arrive and leave a real reset an hour later unreported -- the exact
        // failure the notice exists to prevent, reached by the mechanism meant to
        // prevent it.
        CHECK(notice.Observe(reset));
        CHECK(said.size() == 1);

        notice.Accepted();
        CHECK_FALSE(notice.Reported());

        CHECK(notice.Observe(reset));
        CHECK(said.size() == 2);
    }

    SECTION("no other refusal reaches it")
    {
        // Every other refusal here is about ONE grant and is already answered per
        // request. This one is a condition that persists until the process restarts,
        // which is what makes it worth a line -- so a notice that spoke for all of them
        // would be back to noise.
        for (auto const reason: { LeaseRefusalReason::Malformed,
                                  LeaseRefusalReason::Unauthorized,
                                  LeaseRefusalReason::ClusterMismatch,
                                  LeaseRefusalReason::EndpointMismatch,
                                  LeaseRefusalReason::FingerprintMismatch,
                                  LeaseRefusalReason::Expired })
        {
            INFO("reason " << static_cast<int>(reason));
            CHECK_FALSE(notice.Observe(LeaseRefusal { .reason = reason, .detail = "irrelevant" }));
        }
        CHECK(said.empty());
    }

    SECTION("a silent notice reports nowhere and still latches")
    {
        // `Silent()` is named rather than defaulted, which is `CredentialNotice`'s rule
        // and the reason is the same: a defaulted sink is how a diagnostic comes to be
        // dropped at every call site at once. It still answers whether it WOULD have
        // spoken, so a caller can assert the decision without a sink.
        auto quiet = LeaseEpochNotice::Silent();
        CHECK(quiet.Observe(reset));
        CHECK(quiet.Reported());
        CHECK_FALSE(quiet.Observe(reset));
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

    // The seam-level property -- one field list, two domains, two tags -- is
    // `ClusterSigning_test.cpp`'s and is deliberately not restated here. This case
    // asserts the protocol-level consequence with a real proof and a real token,
    // which is the part `LeaseToken` owns.
}

TEST_CASE("Moving the lease onto the shared seam changed none of its bytes", "[distributed][lease][token]")
{
    // The one thing a consolidation must be able to say for itself. The lease
    // already carried `fastcache-lease-v1` ahead of its packed claims, so routing
    // it through `Cluster::SignFields` had to reproduce that message exactly --
    // and "the tests still pass" cannot show it, because the tests moved with the
    // code. The pre-seam construction is therefore written out here as a literal:
    // `HmacSha256` over `Encode({ "fastcache-lease-v1", packedClaims })`, with the
    // label spelled rather than read from the table it now lives in.
    auto const key = Key();
    auto const token = MintLeaseToken(key, Grant());

    auto const decoded = Unwrap(Base64Decode(token));
    auto const outer = Unwrap(WireFields::SplitExactly(AsBytes(decoded), 2));
    auto const packed = outer[0];
    auto const tag = outer[1];

    auto const preSeam = HmacSha256(key, WireFields::Encode({ AsBytes(std::string_view { "fastcache-lease-v1" }), packed }));

    REQUIRE(tag.size() == Sha256::DigestSize);
    Sha256::Digest presented {};
    std::ranges::copy(tag, presented.begin());
    CHECK(ConstantTimeEquals(preSeam, presented));
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
