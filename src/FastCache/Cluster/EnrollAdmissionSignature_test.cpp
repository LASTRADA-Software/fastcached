// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using Testing::TestKeyPair;

namespace
{
/// A nonce every byte of which is @p fill.
/// @param fill The byte.
/// @return The nonce.
[[nodiscard]] std::array<std::byte, CompileCacheWire::NodeChallengeBytes> NonceOf(std::byte fill)
{
    auto nonce = std::array<std::byte, CompileCacheWire::NodeChallengeBytes> {};
    std::ranges::fill(nonce, fill);
    return nonce;
}

/// What the office admits n-laptop with, over @p nonce.
struct Admission
{
    std::array<std::byte, CompileCacheWire::NodeChallengeBytes> nonce; ///< The joiner's nonce.
    std::vector<std::byte> roster;                                     ///< The roster it is handed.
};

/// @param admission The admission.
/// @return The claim over its own bytes: n-laptop, under its key, admitted to c-office.
[[nodiscard]] AdmissionClaim ClaimOf(Admission const& admission)
{
    return AdmissionClaim { .nonce = admission.nonce,
                            .joinerId = "n-laptop",
                            .joinerKey = TestKeyPair("n-laptop").PublicKey(),
                            .clusterId = "c-office",
                            .outcome = CompileCacheWire::EnrollOutcome::Approved,
                            .roster = admission.roster };
}

/// @return The office's admission of n-laptop over a nonce of 0x11s.
[[nodiscard]] Admission OfficeAdmission()
{
    return Admission { .nonce = NonceOf(std::byte { 0x11 }), .roster = Testing::OfficeRosterWith("n-laptop") };
}
} // namespace

TEST_CASE("An admission signed over the joiner's request by the key it proved is verified", "[cluster][formation][enroll]")
{
    auto const office = TestKeyPair("n-office");
    auto const admission = OfficeAdmission();
    auto const signature = SignAdmission(office, ClaimOf(admission));
    CHECK(signature.publicKey == office.PublicKey());
    CHECK(VerifyAdmission(ClaimOf(admission), signature, office.PublicKey()) == AdmissionSignature::Verified);
}

TEST_CASE("An admission is refused by name when nothing signed it", "[cluster][formation][enroll]")
{
    auto const admission = OfficeAdmission();
    CHECK(VerifyAdmission(ClaimOf(admission), std::nullopt, TestKeyPair("n-office").PublicKey())
          == AdmissionSignature::Unsigned);
}

TEST_CASE("A genuine admission by a key the joiner never proved for that fleet is unproven", "[cluster][formation][enroll]")
{
    // The copied-public-facts attack: n-evil lists the office's key and the joiner in a roster of its
    // own, and signs it honestly with its own key. The signature is genuine; the KEY is not one the
    // joiner proved for c-office.
    auto const admission = OfficeAdmission();
    auto const signature = SignAdmission(TestKeyPair("n-evil"), ClaimOf(admission));
    CHECK(VerifyAdmission(ClaimOf(admission), signature, TestKeyPair("n-office").PublicKey())
          == AdmissionSignature::Unproven);
}

TEST_CASE("An admission's signature covers the nonce, the joiner, its key, the cluster, the outcome and the roster",
          "[cluster][formation][enroll]")
{
    // Each field is a way a genuine signature could be spent somewhere it was not given: replayed to
    // a later ask, handed to another machine, carried into another fleet, or wrapped around another
    // roster. One case per field, each ONE change from a signature that verifies.
    auto const office = TestKeyPair("n-office");
    auto const admission = OfficeAdmission();
    auto const signature = SignAdmission(office, ClaimOf(admission));
    REQUIRE(VerifyAdmission(ClaimOf(admission), signature, office.PublicKey()) == AdmissionSignature::Verified);

    auto const otherNonce = NonceOf(std::byte { 0x22 });
    auto replayed = ClaimOf(admission);
    replayed.nonce = otherNonce;
    CHECK(VerifyAdmission(replayed, signature, office.PublicKey()) == AdmissionSignature::Forged);

    auto otherJoiner = ClaimOf(admission);
    otherJoiner.joinerId = "n-desk";
    CHECK(VerifyAdmission(otherJoiner, signature, office.PublicKey()) == AdmissionSignature::Forged);

    auto otherKey = ClaimOf(admission);
    otherKey.joinerKey = TestKeyPair("n-desk").PublicKey();
    CHECK(VerifyAdmission(otherKey, signature, office.PublicKey()) == AdmissionSignature::Forged);

    auto otherCluster = ClaimOf(admission);
    otherCluster.clusterId = "c-home";
    CHECK(VerifyAdmission(otherCluster, signature, office.PublicKey()) == AdmissionSignature::Forged);

    // One answer's signature is never another's: an approval's, held to a refusal or a "not yet"
    // over the same request, is forged -- or a joiner told "not yet" could be turned away with it.
    // The OUTCOME is the one change: the roster stays the approval's, so a digest that differs cannot
    // be what refuses it.
    for (auto const outcome: { CompileCacheWire::EnrollOutcome::Rejected, CompileCacheWire::EnrollOutcome::Pending })
    {
        auto otherOutcome = ClaimOf(admission);
        otherOutcome.outcome = outcome;
        CHECK(VerifyAdmission(otherOutcome, signature, office.PublicKey()) == AdmissionSignature::Forged);
    }

    auto const evilRoster = Testing::RosterWith("n-evil", "n-laptop");
    auto otherRoster = ClaimOf(admission);
    otherRoster.roster = evilRoster;
    CHECK(VerifyAdmission(otherRoster, signature, office.PublicKey()) == AdmissionSignature::Forged);
}

TEST_CASE("A refusal and a not-yet are signed and verified as an admission is", "[cluster][formation][enroll]")
{
    // Every outcome moves a joiner, so every outcome is held to the key it proved: each verifies under
    // that key, is unproven under another's, and is unsigned when nothing signed it.
    auto const office = TestKeyPair("n-office");
    auto const admission = OfficeAdmission();
    for (auto const outcome: { CompileCacheWire::EnrollOutcome::Rejected, CompileCacheWire::EnrollOutcome::Pending })
    {
        INFO(static_cast<int>(outcome));
        auto claim = ClaimOf(admission);
        claim.outcome = outcome;
        claim.roster = {};
        CHECK(VerifyAdmission(claim, SignAdmission(office, claim), office.PublicKey()) == AdmissionSignature::Verified);
        CHECK(VerifyAdmission(claim, SignAdmission(TestKeyPair("n-evil"), claim), office.PublicKey())
              == AdmissionSignature::Unproven);
        CHECK(VerifyAdmission(claim, std::nullopt, office.PublicKey()) == AdmissionSignature::Unsigned);
    }
}

TEST_CASE("A forged admission is reported as forged whoever it names", "[cluster][formation][enroll]")
{
    // The signature is judged before the key: a signature that does not verify names nobody, so it is
    // not reported as a proven key's -- or an unproven one's -- words.
    auto const office = TestKeyPair("n-office");
    auto const admission = OfficeAdmission();
    auto signature = SignAdmission(office, ClaimOf(admission));
    signature.signature[0] ^= std::byte { 0x01 };
    CHECK(VerifyAdmission(ClaimOf(admission), signature, office.PublicKey()) == AdmissionSignature::Forged);
    CHECK(VerifyAdmission(ClaimOf(admission), signature, TestKeyPair("n-evil").PublicKey()) == AdmissionSignature::Forged);
}

TEST_CASE("Every admission verdict has words of its own", "[cluster][formation][enroll]")
{
    for (auto const& row: AdmissionSignatureTable)
    {
        CHECK_FALSE(row.words.empty());
        CHECK(std::ranges::count(AdmissionSignatureTable, row.words, &AdmissionSignatureRow::words) == 1);
    }
}
