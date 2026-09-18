// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using namespace std::chrono_literals;
using FastCache::Testing::TestKeyPair;

namespace
{

/// The instant every case is decided at.
constexpr auto Now = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// The clock skew every case tolerates.
constexpr auto Slack = std::chrono::seconds { 30 };

/// A roster of voters named by @p voters, each with its test key unless it is in @p keyless.
[[nodiscard]] Roster RosterOf(std::vector<std::string> const& voters,
                              std::vector<std::string> const& keyless = {},
                              std::vector<std::string> const& revoked = {})
{
    Roster roster;
    for (auto const& id: voters)
    {
        auto const hasKey = std::ranges::find(keyless, id) == keyless.end();
        roster.members.push_back(
            RosterMember { .id = id,
                           .raftEndpoint = id + ":6680",
                           .seat = MemberSeat::Voter,
                           .publicKey = hasKey ? std::optional { TestKeyPair(id).PublicKey() } : std::nullopt });
    }
    for (auto const& id: revoked)
        roster.revoked.push_back(RevokedKey { .id = id, .publicKey = TestKeyPair(id).PublicKey() });
    return roster;
}

/// @p machine's endorsement of @p roster at @p version, lapsing at @p notAfter.
[[nodiscard]] RosterEndorsement Endorse(std::string const& machine,
                                        std::vector<std::byte> const& roster,
                                        std::uint64_t version,
                                        std::chrono::system_clock::time_point notAfter,
                                        std::string const& cluster = "fleet")
{
    auto const key = TestKeyPair(machine);
    return SignEndorsement(RosterEndorsement { .clusterId = cluster,
                                               .version = version,
                                               .rosterDigest = DigestOfRoster(roster),
                                               .notAfter = notAfter,
                                               .endorser = machine,
                                               .signature = {} },
                           [&key](std::span<std::byte const> message) { return key.Sign(message); });
}

/// @p roster at @p version, with @p endorsements.
[[nodiscard]] CertifiedRoster Offer(Roster const& roster,
                                    std::uint64_t version,
                                    std::vector<std::string> const& endorsers,
                                    std::chrono::system_clock::time_point notAfter = Now + 1h)
{
    auto certified =
        CertifiedRoster { .clusterId = "fleet", .version = version, .roster = EncodeRoster(roster), .endorsements = {} };
    for (auto const& endorser: endorsers)
        certified.endorsements.push_back(Endorse(endorser, certified.roster, version, notAfter));
    return certified;
}

/// Decide @p offered against @p voters, holding nothing yet.
[[nodiscard]] std::expected<AdoptedRoster, RosterRefusal> Decide(CertifiedRoster const& offered,
                                                                 CertifyingVoters const& voters,
                                                                 std::uint64_t minimumVersion = 0,
                                                                 std::optional<RosterDigest> held = std::nullopt)
{
    return CertifyRoster(offered,
                         CertificationInput { .clusterId = "fleet",
                                              .voters = voters,
                                              .minimumVersion = minimumVersion,
                                              .held = held,
                                              .now = Now,
                                              .slack = Slack });
}

} // namespace

TEST_CASE("An endorsement verifies under its endorser's key and under no other, over every claim", "[cluster][roster]")
{
    auto const roster = EncodeRoster(RosterOf({ "n1", "n2", "n3" }));
    auto const endorsement = Endorse("n1", roster, 7, Now + 1h);
    CHECK(VerifyEndorsement(endorsement, TestKeyPair("n1").PublicKey()));
    CHECK_FALSE(VerifyEndorsement(endorsement, TestKeyPair("n2").PublicKey()));

    // Every claim is under the signature: changing any one of them is a different statement.
    auto cluster = endorsement;
    cluster.clusterId = "other";
    auto version = endorsement;
    version.version = 8;
    auto digest = endorsement;
    digest.rosterDigest[0] ^= std::byte { 1 };
    auto lapse = endorsement;
    lapse.notAfter += 1h;
    auto endorser = endorsement;
    endorser.endorser = "n2";
    for (auto const& tampered: { cluster, version, digest, lapse, endorser })
        CHECK_FALSE(VerifyEndorsement(tampered, TestKeyPair("n1").PublicKey()));
}

TEST_CASE("An endorsement, a certified roster and a kept roster survive their encodings", "[cluster][roster]")
{
    auto const certified = Offer(RosterOf({ "n1", "n2", "n3" }), 4, { "n1", "n3" });

    auto const endorsement = DecodeEndorsement(EncodeEndorsement(certified.endorsements[0]));
    REQUIRE(endorsement.has_value());
    CHECK(*endorsement == certified.endorsements[0]);

    auto const decoded = DecodeCertifiedRoster(EncodeCertifiedRoster(certified));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == certified);

    auto const kept = PersistedRoster { .certificate = certified, .certifiedUntil = Now + 45min };
    auto const restored = DecodePersistedRoster(EncodePersistedRoster(kept));
    REQUIRE(restored.has_value());
    CHECK(*restored == kept);
}

TEST_CASE("A certified roster or a kept one another build laid out is refused by name", "[cluster][roster]")
{
    auto const certified = Offer(RosterOf({ "n1" }), 1, { "n1" });

    // Each layout's version is its first field's only byte, pinned as a byte and not only as a
    // name, and a different one is `UnsupportedVersion` rather than damage.
    auto const refusedByName = [](std::vector<std::byte> bytes, auto decode) {
        auto const split = WireFields::SplitAll(bytes);
        REQUIRE(split.has_value());
        auto const& fields = Testing::Unwrap(split);
        REQUIRE(fields[0].size() == 1);
        CHECK(fields[0][0] == std::byte { 0x01 });
        auto const offset = static_cast<std::size_t>(fields[0].data() - bytes.data());
        bytes[offset] = std::byte { 0x02 };
        auto const decoded = decode(bytes);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);
    };
    static_assert(CertifiedRosterFormatVersion == 1 && PersistedRosterFormatVersion == 1);

    refusedByName(EncodeCertifiedRoster(certified),
                  [](std::span<std::byte const> bytes) { return DecodeCertifiedRoster(bytes); });
    refusedByName(EncodePersistedRoster(PersistedRoster { .certificate = certified, .certifiedUntil = Now }),
                  [](std::span<std::byte const> bytes) { return DecodePersistedRoster(bytes); });
}

TEST_CASE("A roster a strict majority of the trusted voters endorse is adopted until that majority lapses",
          "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }));
    auto offered = Offer(RosterOf({ "n1", "n2", "n3", "n4" }), 5, {});
    offered.endorsements.push_back(Endorse("n1", offered.roster, 5, Now + 50min));
    offered.endorsements.push_back(Endorse("n2", offered.roster, 5, Now + 20min));
    offered.endorsements.push_back(Endorse("n3", offered.roster, 5, Now + 40min));

    auto const adopted = Decide(offered, voters);
    REQUIRE(adopted.has_value());
    CHECK(adopted->roster.members.size() == 4);
    CHECK(adopted->digest == DigestOfRoster(offered.roster));
    // Two of three make the majority, so it lapses when the second-latest does.
    CHECK(adopted->certifiedUntil == Now + 40min);
}

TEST_CASE("A roster short of a strict majority is uncertified, and a lapsed majority is expired", "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }));

    SECTION("one of three")
    {
        auto const refused = Decide(Offer(RosterOf({ "n1" }), 2, { "n1" }), voters);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == RosterRefusal::Uncertified);
    }

    SECTION("a majority, every one of them lapsed past the slack")
    {
        auto const refused = Decide(Offer(RosterOf({ "n1" }), 2, { "n1", "n2" }, Now - Slack - 1s), voters);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == RosterRefusal::Expired);
    }

    SECTION("a majority lapsed by less than the slack still counts")
    {
        CHECK(Decide(Offer(RosterOf({ "n1" }), 2, { "n1", "n2" }, Now - Slack + 1s), voters).has_value());
    }

    SECTION("an endorsement claiming more than a lifetime ahead counts for nothing")
    {
        auto const refused =
            Decide(Offer(RosterOf({ "n1" }), 2, { "n1", "n2" }, Now + RosterEndorsementLifetime + Slack + 1s), voters);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == RosterRefusal::Uncertified);
    }
}

TEST_CASE("A voter the trusted roster revoked cannot help certify its successor", "[cluster][roster]")
{
    // n2 is revoked in the roster the worker holds. Its endorsement verifies -- it still holds
    // its key -- and counts for nothing; the majority is still of all three voters.
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }, {}, { "n2" }));
    CHECK(voters.voters == 3);
    CHECK(voters.endorsers.size() == 2);

    auto const refused = Decide(Offer(RosterOf({ "n1", "n2" }), 9, { "n1", "n2" }), voters);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterRefusal::Uncertified);

    // The control: the same roster endorsed by the two voters still trusted is adopted.
    CHECK(Decide(Offer(RosterOf({ "n1", "n2" }), 9, { "n1", "n3" }), voters).has_value());
}

TEST_CASE("A keyless voter counts toward the majority it can never help reach", "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3", "n4" }, { "n3", "n4" }));
    CHECK(voters.voters == 4);

    auto const refused = Decide(Offer(RosterOf({ "n1" }), 3, { "n1", "n2" }), voters);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterRefusal::Uncertified);
}

TEST_CASE("One voter is one vote, however often it endorses", "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }));
    auto const refused = Decide(Offer(RosterOf({ "n1" }), 3, { "n1", "n1", "n1" }), voters);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterRefusal::Uncertified);
}

TEST_CASE("An endorsement counts only for the roster, version and fleet it names", "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }));
    auto offered = Offer(RosterOf({ "n1", "n2" }), 6, { "n1" });

    SECTION("another roster's bytes")
    {
        auto const elsewhere = EncodeRoster(RosterOf({ "n9" }));
        offered.endorsements.push_back(Endorse("n2", elsewhere, 6, Now + 1h));
    }
    SECTION("another version")
    {
        offered.endorsements.push_back(Endorse("n2", offered.roster, 7, Now + 1h));
    }
    SECTION("another fleet")
    {
        offered.endorsements.push_back(Endorse("n2", offered.roster, 6, Now + 1h, "other"));
    }
    SECTION("another voter's id over its own key")
    {
        auto claimed = Endorse("n2", offered.roster, 6, Now + 1h);
        claimed.endorser = "n3";
        offered.endorsements.push_back(claimed);
    }

    auto const refused = Decide(offered, voters);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterRefusal::Uncertified);
}

TEST_CASE("An offer is judged against what is held: older is stale, the same version must match", "[cluster][roster]")
{
    auto const voters = VotersOf(RosterOf({ "n1", "n2", "n3" }));
    auto const offered = Offer(RosterOf({ "n1", "n2" }), 6, { "n1", "n2" });
    auto const digest = DigestOfRoster(offered.roster);

    auto const stale = Decide(offered, voters, 7);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == RosterRefusal::Stale);

    auto const conflicting = Decide(offered, voters, 6, DigestOfRoster(EncodeRoster(RosterOf({ "n9" }))));
    REQUIRE_FALSE(conflicting.has_value());
    CHECK(conflicting.error() == RosterRefusal::Conflicting);

    // The same roster re-endorsed is a refresh, and is adopted.
    CHECK(Decide(offered, voters, 6, digest).has_value());

    auto other = offered;
    other.clusterId = "other";
    auto const wrongCluster = Decide(other, voters);
    REQUIRE_FALSE(wrongCluster.has_value());
    CHECK(wrongCluster.error() == RosterRefusal::WrongCluster);

    auto malformed = offered;
    malformed.roster = { std::byte { 0xFF } };
    malformed.endorsements = { Endorse("n1", malformed.roster, 6, Now + 1h), Endorse("n2", malformed.roster, 6, Now + 1h) };
    auto const damaged = Decide(malformed, voters);
    REQUIRE_FALSE(damaged.has_value());
    CHECK(damaged.error() == RosterRefusal::Malformed);
}

TEST_CASE("Anchors certify a first roster by key alone, each counted once", "[cluster][roster]")
{
    // An anchor names nobody: it is a key typed on a command line, so an endorsement is counted
    // for whichever anchor its signature verifies under, whatever id it claims.
    auto const anchors = std::vector { TestKeyPair("n1").PublicKey(),
                                       TestKeyPair("n2").PublicKey(),
                                       TestKeyPair("n1").PublicKey(),
                                       TestKeyPair("n3").PublicKey() };
    auto const voters = AnchorVoters(anchors);
    CHECK(voters.voters == 3);

    CHECK(Decide(Offer(RosterOf({ "n1", "n2", "n3" }), 1, { "n2", "n3" }), voters).has_value());

    auto const refused = Decide(Offer(RosterOf({ "n1", "n2", "n3" }), 1, { "n3", "n9" }), voters);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == RosterRefusal::Uncertified);
}
