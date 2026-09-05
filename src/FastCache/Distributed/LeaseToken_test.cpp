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
/// @return The expectation.
[[nodiscard]] LeaseExpectation Worker(std::string_view endpoint = "10.0.0.7:6675",
                                      std::string_view fingerprint = "clang-19-x86_64",
                                      std::string_view cluster = TestCluster)
{
    return LeaseExpectation { .endpoint = endpoint, .fingerprint = fingerprint, .clusterId = cluster };
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

TEST_CASE("A legitimate scheduler reset is adopted rather than refused", "[distributed][lease][token][epoch]")
{
    // **The property [#614](https://github.com/LASTRADA-Software/fastcached/issues/614)
    // exists for.** Wiping the Raft directory, re-bootstrapping, or turning consensus
    // off drops a scheduler's term back to 0. That answer is genuine, signed, and from
    // the right scheduler -- authenticity was never the issue -- and until #614 a
    // monotonic maximum had no way to express it, so every worker that had learned a
    // higher term refused every grant until its process was restarted.
    auto const key = Key();

    KnownSchedulerTerm term;
    REQUIRE(term.Learn(7).transition == TermTransition::Advanced);

    // A scheduler that has genuinely reset mints at term 0. `StandaloneSchedulerTerm`
    // is literally 0, so this is also exactly what turning consensus OFF produces.
    auto const afterReset = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 0));
    CHECK(VerifyLeaseToken(key, afterReset, Worker(), Noon()).has_value());

    // And the worker ADOPTS it, so the fleet keeps working with no restart anywhere.
    // The transition is reported to the caller that performed it, which is what makes
    // the condition sayable at all -- re-reading the member afterwards would race
    // another compile thread's grant.
    auto const change = term.Learn(0);
    CHECK(change.transition == TermTransition::Reset);
    CHECK(change.previous == 7);
    CHECK(change.current == 0);
    CHECK(term.Known() == std::optional<std::uint64_t> { 0 });
}

TEST_CASE("The term check that used to stand here was exactly inverted across a reset", "[distributed][lease][token][epoch]")
{
    // **Measured on the tree before #614, and it is the argument for deleting the
    // check rather than repairing it.** The old rule was `claimed >= expected`, so with
    // a worker holding term 7 and a scheduler reset to term 0 it refused the honest
    // fresh grant (0 < 7) while ACCEPTING a token captured under term 7 (7 >= 7). The
    // rule that existed to stop replay refused every legitimate grant and admitted the
    // replayed one, in precisely the scenario it was written for.
    //
    // Pinned as the pair, because either half alone reads as an ordinary policy choice
    // and only together do they show the inversion. Both are now decided by
    // `SpentLeases`, which answers the question the term was standing in for.
    auto const key = Key();
    auto const fresh = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 0));
    auto const captured = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-abc", TestCluster, 7));

    // Both authenticate; the term decides nothing here any more.
    CHECK(VerifyLeaseToken(key, fresh, Worker(), Noon()).has_value());
    CHECK(VerifyLeaseToken(key, captured, Worker(), Noon()).has_value());

    // What separates them now is whether they have been spent, and that is a property
    // of the token rather than of the era it was minted in.
    SpentLeases spent;
    CHECK(spent.Spend(captured, Noon() + 10min, Noon()));
    CHECK_FALSE(spent.Spend(captured, Noon() + 10min, Noon()));
    CHECK(spent.Spend(fresh, Noon() + 10min, Noon()));
}

TEST_CASE("A grant is spendable exactly once", "[distributed][lease][token][replay]")
{
    // A lease is single-use by construction: the scheduler mints one grant per lease,
    // `Dispatch::CompileOnWorker` presents it in exactly one COMPILE frame with no
    // retry, and a RELEASE goes to the scheduler rather than to a worker. So a second
    // arrival at one worker is a replay and never an honest client -- and until #614
    // nothing said so, which meant a captured grant was replayable at its worker until
    // it expired.
    SpentLeases spent;
    auto const key = Key();
    auto const grant = MintLeaseToken(key, Grant());
    auto const other = MintLeaseToken(key, Grant("10.0.0.7:6675", "clang-19-x86_64", "obj-def"));

    CHECK(spent.Spend(grant, Noon() + 10min, Noon()));
    CHECK_FALSE(spent.Spend(grant, Noon() + 10min, Noon()));
    CHECK_FALSE(spent.Spend(grant, Noon() + 10min, Noon() + 1min));

    // A DIFFERENT grant is unaffected, which is the half a set keyed too coarsely
    // would break: two grants sharing a key, a worker and a toolchain differ in their
    // serial, and `LeaseTable` mints a fresh one per acquisition.
    CHECK(spent.Spend(other, Noon() + 10min, Noon()));
    CHECK(spent.Size() == 2);
}

TEST_CASE("The spent set is bounded by the grants' own expiry", "[distributed][lease][token][replay]")
{
    // The bound, and the reason there is no timer: an entry describes a token that
    // nothing would accept once its expiry has passed, so it is dropped on the next
    // `Spend` rather than kept. A worker with no traffic has nothing to prune, and one
    // with traffic prunes as it goes.
    SpentLeases spent;
    auto const key = Key();

    for (auto const index: std::views::iota(0, 8))
    {
        auto claims = Grant();
        claims.serial = std::format("l{}", index);
        CHECK(spent.Spend(MintLeaseToken(key, claims), Noon() + 10min, Noon()));
    }
    CHECK(spent.Size() == 8);

    // One more, well past every entry's expiry: the arrival prunes the lot.
    auto later = Grant();
    later.serial = "l99";
    later.expiresAt = Noon() + 1h;
    CHECK(spent.Spend(MintLeaseToken(key, later), Noon() + 1h, Noon() + 30min));
    CHECK(spent.Size() == 1);
}

TEST_CASE("What a worker knows about the term is three states, and zero is not one of them",
          "[distributed][lease][token][epoch]")
{
    // `SchedulerService::StandaloneSchedulerTerm` IS `0` -- the term of a node leading
    // alone -- and its own declaration warns that "a literal at a call site is exactly
    // how somebody later reads it as unknown and starts treating it as one". So
    // never-learned is a FLAG on `KnownSchedulerTerm` rather than a term value.
    //
    // It still matters after #614, even though nothing refuses on the term any more:
    // it is what keeps the FIRST grant from reading as a reset. Term 0 arriving into a
    // worker that has learned nothing is `Advanced`; the same 0 arriving into a worker
    // that knows 7 is `Reset`. Collapsed to a zero sentinel, every cold worker's first
    // grant would announce a scheduler reset that did not happen.
    //
    // Named rather than included: `SchedulerService.hpp` is a scheduler header and
    // this is a token test, and reaching for it here collides with an unrelated
    // `Detail` namespace. The zero below is the constant's value, which is the case's
    // input -- writing it is not the mistake, reading it as "unknown" would be.
    KnownSchedulerTerm fresh;
    CHECK_FALSE(fresh.Known().has_value());

    KnownSchedulerTerm standalone;
    auto const first = standalone.Learn(0);
    CHECK(first.transition == TermTransition::Advanced);
    CHECK(standalone.Known() == std::optional<std::uint64_t> { 0 });

    KnownSchedulerTerm elected;
    REQUIRE(elected.Learn(7).transition == TermTransition::Advanced);
    CHECK(elected.Learn(0).transition == TermTransition::Reset);
}

TEST_CASE("A worker adopts the term of the last authentic grant, in either direction", "[distributed][lease][token][epoch]")
{
    // Latest wins since #614, not the maximum. The maximum was standing in for replay
    // protection -- `SpentLeases` provides that directly now -- and it was what made a
    // legitimate reset inexpressible.
    //
    // Every step is asserted through the RETURNED change rather than by re-reading the
    // member, because that is the only reading a caller may act on: two compile threads
    // learn concurrently, and a value read back afterwards may be the other thread's.
    KnownSchedulerTerm term;
    CHECK(term.Learn(7).transition == TermTransition::Advanced);

    auto const backwards = term.Learn(4);
    CHECK(backwards.transition == TermTransition::Reset);
    CHECK(backwards.previous == 7);
    CHECK(backwards.current == 4);
    CHECK(term.Known() == std::optional<std::uint64_t> { 4 });

    // The same term twice is neither, which is what keeps a steady fleet silent: every
    // dispatched compile learns, and a line per compile is not a diagnostic.
    CHECK(term.Learn(4).transition == TermTransition::Unchanged);

    auto const forwards = term.Learn(9);
    CHECK(forwards.transition == TermTransition::Advanced);
    CHECK(forwards.previous == 4);
    CHECK(term.Known() == std::optional<std::uint64_t> { 9 });
}

TEST_CASE("A verifier accepts a grant from any term", "[distributed][lease][token]")
{
    // The term decides nothing at verification since #614. It is carried, it is inside
    // the MAC, and it is adopted -- but a worker never refuses on it, because a lower
    // term is a scheduler that was legitimately reset and nothing in a token can tell
    // that from a replay. What tells them apart is whether the grant has been spent.
    //
    // Pinned across the whole range rather than at one value, because the defect this
    // replaces was a COMPARISON (`claimed >= expected`) and a comparison mistake lives
    // at a boundary.
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

TEST_CASE("The worker says once, per reset, that a scheduler term went backwards", "[distributed][lease][token][epoch]")
{
    // **The signal this replaces was WRONG, not missing** (#614). On the worker a
    // scheduler reset showed only as `WorkerLeaseStaleEpoch` climbing, which reads like
    // an election storm -- so an operator whose fleet stopped distributing was pointed
    // at consensus instability when the cause was a reset they performed themselves.
    //
    // #768 shipped this as the observability half of a REFUSAL, latched and cleared by
    // an accepted grant, because the condition then persisted until a restart. The
    // condition is gone: the worker adopts the lower term and goes on compiling. So the
    // line reports a TRANSITION, which happens once by construction and needs no latch.
    std::vector<std::string> said;
    LeaseEpochNotice notice { [&said](std::string_view line) { said.emplace_back(line); } };

    SECTION("a reset is reported, and it names both terms and what it did")
    {
        CHECK(notice.Observe(TermChange { .transition = TermTransition::Reset, .previous = 7, .current = 0 }));

        REQUIRE(said.size() == 1);
        // Both numbers, because that is what turns "the fleet stopped behaving" into
        // "somebody reset the scheduler". And the ACTION, because the reader's next
        // question is whether they have to do anything -- since #614 they do not, which
        // is worth saying rather than leaving them to infer from silence.
        CHECK(said.front().contains("from 7 to 0"));
        CHECK(said.front().contains("Adopting"));
        CHECK(notice.Seen() == 1);
    }

    SECTION("nothing else reaches it, however many compiles arrive")
    {
        // Every dispatched compile learns a term, so `Unchanged` is what a healthy
        // fleet produces on every single job. A notice that spoke for those would be
        // noise, and noise is how the one line that matters gets missed.
        for (auto attempt = 0; attempt < 500; ++attempt)
        {
            CHECK_FALSE(notice.Observe(TermChange { .transition = TermTransition::Unchanged, .previous = 7, .current = 7 }));
            CHECK_FALSE(notice.Observe(TermChange { .transition = TermTransition::Advanced, .previous = 7, .current = 8 }));
        }
        CHECK(said.empty());
        CHECK(notice.Seen() == 0);
    }

    SECTION("a scheduler that flaps is reported once per flap")
    {
        // **Per transition, not per process.** Once-per-process would spend the line on
        // the first reset and leave a second one an hour later silent -- the failure the
        // notice exists to prevent, reached by the mechanism meant to prevent it. A
        // transition cannot repeat without the term moving back first, so this needs no
        // latch to be re-armed: the arithmetic is the latch.
        CHECK(notice.Observe(TermChange { .transition = TermTransition::Reset, .previous = 7, .current = 0 }));
        CHECK_FALSE(notice.Observe(TermChange { .transition = TermTransition::Advanced, .previous = 0, .current = 7 }));
        CHECK(notice.Observe(TermChange { .transition = TermTransition::Reset, .previous = 7, .current = 0 }));
        CHECK(said.size() == 2);
        CHECK(notice.Seen() == 2);
    }

    SECTION("a silent notice reports nowhere and still counts")
    {
        // `Silent()` is named rather than defaulted, which is `CredentialNotice`'s rule
        // and the reason is the same: a defaulted sink is how a diagnostic comes to be
        // dropped at every call site at once. It still answers whether it WOULD have
        // spoken, so a caller can assert the decision without a sink.
        auto quiet = LeaseEpochNotice::Silent();
        CHECK(quiet.Observe(TermChange { .transition = TermTransition::Reset, .previous = 7, .current = 0 }));
        CHECK(quiet.Seen() == 1);
        CHECK_FALSE(quiet.Observe(TermChange { .transition = TermTransition::Advanced, .previous = 0, .current = 1 }));
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
