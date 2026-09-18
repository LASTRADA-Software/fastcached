// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::TestKeyPair;

namespace
{

/// A member of @p seat, with the test key of @p id or none.
[[nodiscard]] ClusterMember Member(std::string const& id, MemberSeat seat, bool keyed)
{
    return ClusterMember { .id = id,
                           .raftEndpoint = id + ".example:6680",
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = keyed ? std::optional { TestKeyPair(id).PublicKey() } : std::nullopt };
}

/// Two keyed voters, a keyless voter, a learner, a worker principal and one revocation.
[[nodiscard]] ClusterState SampleState()
{
    ClusterState state;
    state.members = { Member("n1", MemberSeat::Voter, true),
                      Member("n2", MemberSeat::Voter, true),
                      Member("n3", MemberSeat::Voter, false),
                      Member("n4", MemberSeat::Learner, true) };
    state.principals = { ClusterPrincipal {
        .id = "w1", .publicKey = TestKeyPair("w1").PublicKey(), .role = PrincipalRole::Worker } };
    state.revokedKeys = { RevokedKey { .id = "n0", .publicKey = TestKeyPair("n0").PublicKey() } };
    return state;
}

} // namespace

TEST_CASE("A roster is projected from the state it describes, seats and keys included", "[cluster][roster]")
{
    auto const state = SampleState();
    auto const roster = ProjectRoster(state);

    REQUIRE(roster.members.size() == 4);
    CHECK(roster.members[0].id == "n1");
    CHECK(roster.members[0].raftEndpoint == "n1.example:6680");
    CHECK(roster.members[0].publicKey == TestKeyPair("n1").PublicKey());
    CHECK_FALSE(roster.members[2].publicKey.has_value());
    CHECK(roster.members[3].seat == MemberSeat::Learner);
    CHECK(roster.principals == state.principals);
    CHECK(roster.revoked == state.revokedKeys);
}

TEST_CASE("A roster survives its own encoding, a keyless member included", "[cluster][roster]")
{
    auto const roster = ProjectRoster(SampleState());
    auto const decoded = DecodeRoster(EncodeRoster(roster));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == roster);
}

TEST_CASE("A roster's digest moves with who may vouch for whom, and with nothing an election moves", "[cluster][roster]")
{
    // What distinguishes: a scheduler endpoint changes every time a member leads, so a digest
    // that moved with it would need a fresh endorsement per election; a key, a seat or a
    // revocation is exactly what an endorsement vouches for.
    auto const state = SampleState();
    auto const digest = DigestOfRoster(ProjectRoster(state));

    auto elected = state;
    elected.members[0].schedulerEndpoint = "n1.example:6677";
    elected.members[0].schedulerEndpointHistory = SchedulerEndpointHistory::Announced;
    CHECK(DigestOfRoster(ProjectRoster(elected)) == digest);

    auto rekeyed = state;
    rekeyed.members[1].publicKey = TestKeyPair("elsewhere").PublicKey();
    CHECK(DigestOfRoster(ProjectRoster(rekeyed)) != digest);

    auto reseated = state;
    reseated.members[3].seat = MemberSeat::Voter;
    CHECK(DigestOfRoster(ProjectRoster(reseated)) != digest);

    auto revoked = state;
    revoked.revokedKeys.push_back(RevokedKey { .id = "n2", .publicKey = TestKeyPair("n2").PublicKey() });
    CHECK(DigestOfRoster(ProjectRoster(revoked)) != digest);

    // And the two entry points agree: the digest of the bytes a worker received is the digest
    // of the roster they encode.
    CHECK(DigestOfRoster(EncodeRoster(ProjectRoster(state))) == digest);
}

TEST_CASE("A roster another build laid out is refused by name, and a damaged one as damage", "[cluster][roster]")
{
    auto bytes = EncodeRoster(ProjectRoster(SampleState()));
    auto const split = WireFields::SplitAll(bytes);
    REQUIRE(split.has_value());
    auto const& fields = Testing::Unwrap(split);
    REQUIRE(fields[0].size() == 1);

    // The byte, pinned as well as the name: the version is the first field's only byte.
    CHECK(fields[0][0] == std::byte { 0x01 });
    static_assert(RosterFormatVersion == 1);

    SECTION("another layout version")
    {
        auto const offset = static_cast<std::size_t>(fields[0].data() - bytes.data());
        bytes[offset] = std::byte { RosterFormatVersion + 1 };
        auto const decoded = DecodeRoster(bytes);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);
    }

    SECTION("a truncated encoding")
    {
        bytes.pop_back();
        auto const decoded = DecodeRoster(bytes);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }
}

TEST_CASE("A roster fingerprint is the whole digest, in one spelling", "[cluster][roster]")
{
    auto const state = SampleState();
    auto const text = RenderRosterFingerprint(DigestOfRoster(ProjectRoster(state)));
    CHECK(text.starts_with("SHA256:"));
    CHECK(text.size() == std::string_view { "SHA256:" }.size() + 43);

    auto other = state;
    other.principals.clear();
    CHECK(RenderRosterFingerprint(DigestOfRoster(ProjectRoster(other))) != text);
}
