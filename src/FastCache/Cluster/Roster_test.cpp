// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::TestKeyPair;

namespace
{

/// A member of @p seat, under the test key of @p id.
[[nodiscard]] ClusterMember Member(std::string const& id, MemberSeat seat)
{
    return ClusterMember { .id = id,
                           .raftEndpoint = id + ".example:6680",
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = TestKeyPair(id).PublicKey() };
}

/// Three voters, a learner, a worker principal and one revocation.
[[nodiscard]] ClusterState SampleState()
{
    ClusterState state;
    state.members = { Member("n1", MemberSeat::Voter),
                      Member("n2", MemberSeat::Voter),
                      Member("n3", MemberSeat::Voter),
                      Member("n4", MemberSeat::Learner) };
    state.principals = { ClusterPrincipal {
        .id = "w1", .publicKey = TestKeyPair("w1").PublicKey(), .role = PrincipalRole::Worker } };
    state.revokedKeys = { RevokedKey { .id = "n0", .publicKey = TestKeyPair("n0").PublicKey() } };
    return state;
}

/// A small-order point that is NOT the all-zero key: the identity, y = 1.
///
/// The all-zero key is small-order too, but this project reads it first as a key nobody named --
/// what a command built without one carries -- and refuses it as "no identity key" before the curve
/// is asked. So a case about the SMALL-ORDER refusal names a point only that refusal can answer.
/// @return The key.
[[nodiscard]] Ed25519PublicKey SmallOrderIdentityPoint()
{
    auto key = Ed25519PublicKey {};
    key.front() = std::byte { 0x01 };
    return key;
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
    CHECK(roster.members[2].publicKey == TestKeyPair("n3").PublicKey());
    CHECK(roster.members[3].seat == MemberSeat::Learner);
    CHECK(roster.principals == state.principals);
    CHECK(roster.revoked == state.revokedKeys);
}

TEST_CASE("A roster survives its own encoding", "[cluster][roster]")
{
    auto const roster = ProjectRoster(SampleState());
    auto const decoded = DecodeRoster(EncodeRoster(roster));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == roster);
}

TEST_CASE("A roster's digest moves with every fact it carries, the recorded 0xFC endpoint included, and with no bookkeeping",
          "[cluster][roster]")
{
    // The recorded `0xFC` endpoint is what a joiner remembers its fleet's voters at, so the roster
    // carries it and its digest moves with it -- affordable since the certified roster, which voters
    // endorsed per digest, retired. The endpoint's HISTORY is bookkeeping the joiner has no use for,
    // and moves nothing.
    auto const state = SampleState();
    auto const digest = DigestOfRoster(ProjectRoster(state));

    auto moved = state;
    moved.members[0].schedulerEndpoint = "n1.example:6677";
    CHECK(DigestOfRoster(ProjectRoster(moved)) != digest);
    CHECK(ProjectRoster(moved).members[0].schedulerEndpoint == "n1.example:6677");

    auto history = state;
    history.members[0].schedulerEndpointHistory = SchedulerEndpointHistory::Announced;
    REQUIRE(history.members[0].schedulerEndpointHistory != state.members[0].schedulerEndpointHistory);
    CHECK(DigestOfRoster(ProjectRoster(history)) == digest);

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

    // The byte, pinned as well as the name: the version is the first field's only byte. 2 since a
    // member carries its recorded `0xFC` endpoint: a roster is PERSISTED (a learner's formation record
    // keeps its approval's), so a layout change without a bump would read an old record as damage.
    CHECK(fields[0][0] == std::byte { 0x02 });
    static_assert(RosterFormatVersion == 2);

    SECTION("the previous layout, a record written before members carried an endpoint")
    {
        // Built by hand as version 1 wrote it -- four fields a member -- and refused by NAME, so a
        // learner whose record holds one is told which build wrote it, never that its file is damaged.
        auto const member = WireFields::Encode({ WireFields::AsBytes(std::string_view { "n1" }),
                                                 WireFields::AsBytes(std::string_view { "n1:6680" }),
                                                 std::span<std::byte const> { std::array { std::byte { 0x00 } } },
                                                 std::span<std::byte const> {} });
        auto const members = WireFields::Encode({ std::span<std::byte const> { member } });
        auto const empty = WireFields::Encode(WireFields::FieldList {});
        auto const old = WireFields::Encode({ std::span<std::byte const> { std::array { std::byte { 0x01 } } },
                                              std::span<std::byte const> { members },
                                              std::span<std::byte const> { empty },
                                              std::span<std::byte const> { empty } });
        auto const decoded = DecodeRoster(old);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);
        CHECK(decoded.error().context.contains("roster encoding version 1 (this build reads 2)"));
    }

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

TEST_CASE("A roster holding a small-order or non-canonical live key is refused, naming its holder",
          "[cluster][roster][identity][security]")
{
    // A roster is what a worker checks grants and endorsements against, so a small-order voter key
    // in one is a voter anybody can sign as -- under it a small-order signature verifies every
    // message. Refused on decode by the holder's name, as a member's key and as a principal's; the
    // control is the same key REVOKED, which grants nothing and is kept.
    auto nonCanonical = Ed25519PublicKey {};
    nonCanonical.fill(std::byte { 0xFF });
    nonCanonical.back() = std::byte { 0x7F };

    for (auto const& [key, fault]: { std::pair { SmallOrderIdentityPoint(), PublicKeyFault::SmallOrder },
                                     std::pair { nonCanonical, PublicKeyFault::NonCanonical } })
    {
        INFO("key " << FormatEd25519PublicKey(key));
        auto asMember = ProjectRoster(SampleState());
        asMember.members[1].publicKey = key;
        auto asPrincipal = ProjectRoster(SampleState());
        asPrincipal.principals[0].publicKey = key;

        for (auto const& [roster, holder]:
             { std::pair { asMember, std::string_view { "n2" } }, std::pair { asPrincipal, std::string_view { "w1" } } })
        {
            auto const decoded = DecodeRoster(EncodeRoster(roster));
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
            CHECK(decoded.error().context.contains(std::string { holder } + "'s key"));
            CHECK(decoded.error().context.contains(DescribePublicKeyFault(fault)));
        }

        auto revoked = ProjectRoster(SampleState());
        revoked.revoked.push_back(RevokedKey { .id = "gone", .publicKey = key });
        auto const kept = DecodeRoster(EncodeRoster(revoked));
        REQUIRE(kept.has_value());
        CHECK(*kept == revoked);
    }
}

TEST_CASE("A roster member with an empty or all-zero key is refused by name", "[cluster][roster]")
{
    // A member holds a key by type, so no `Roster` can carry one without -- and the bytes still
    // can, since the field is a length-prefixed run like every other. Built from the bytes: one
    // member, with no recorded `0xFC` endpoint, no principals, no revocations.
    auto const version = std::array { static_cast<std::byte>(RosterFormatVersion) };
    auto const seat = std::array { static_cast<std::byte>(MemberSeat::Voter) };
    auto const key = TestKeyPair("n1").PublicKey();
    auto const encodeWith = [&](std::span<std::byte const> keyField) {
        auto const member = WireFields::Encode({ WireFields::AsBytes(std::string_view { "n1" }),
                                                 WireFields::AsBytes(std::string_view { "n1.example:6680" }),
                                                 std::span<std::byte const> { seat },
                                                 keyField,
                                                 std::span<std::byte const> {} });
        auto const members = WireFields::Encode({ std::span<std::byte const> { member } });
        return WireFields::Encode({ std::span<std::byte const> { version },
                                    std::span<std::byte const> { members },
                                    std::span<std::byte const> {},
                                    std::span<std::byte const> {} });
    };

    // WHAT DISTINGUISHES: the same bytes with the key in place decode, so the key field is the
    // whole cause of the refusals below.
    auto const keyed = DecodeRoster(encodeWith(std::span<std::byte const> { key }));
    REQUIRE(keyed.has_value());
    CHECK(Testing::Unwrap(keyed).members.at(0).publicKey == key);

    // Named apart from every other malformed entry, as `DecodeState` names its own: a roster that
    // holds one says so in its own words.
    auto const zero = Ed25519PublicKey {};
    for (auto const keyField: { std::span<std::byte const> {}, std::span<std::byte const> { zero } })
    {
        INFO("key field of " << keyField.size() << " bytes");
        auto const refused = DecodeRoster(encodeWith(keyField));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::MalformedFrame);
        CHECK(refused.error().context.contains("a roster member holds no identity key"));
    }

    // And a member entry malformed some other way is not reported as keyless.
    auto const shortKey = std::span<std::byte const> { key }.first(Ed25519PublicKeyBytes - 1);
    auto const damaged = DecodeRoster(encodeWith(shortKey));
    REQUIRE_FALSE(damaged.has_value());
    CHECK(damaged.error().context.contains("a roster entry is malformed"));
}

TEST_CASE("A roster principal with an empty or all-zero key is refused by name", "[cluster][roster]")
{
    // The state never records a principal without a key or under the all-zero one, so no roster
    // projected out of one carries it -- and a kept roster that does is named, as a member is.
    // Built from the bytes: no members, one principal, no revocations.
    auto const version = std::array { static_cast<std::byte>(RosterFormatVersion) };
    auto const role = std::array { static_cast<std::byte>(PrincipalRole::Worker) };
    auto const key = TestKeyPair("w1").PublicKey();
    auto const encodeWith = [&](std::span<std::byte const> keyField) {
        auto const principal = WireFields::Encode(
            { WireFields::AsBytes(std::string_view { "w1" }), keyField, std::span<std::byte const> { role } });
        auto const principals = WireFields::Encode({ std::span<std::byte const> { principal } });
        return WireFields::Encode({ std::span<std::byte const> { version },
                                    std::span<std::byte const> {},
                                    std::span<std::byte const> { principals },
                                    std::span<std::byte const> {} });
    };

    // WHAT DISTINGUISHES: the same bytes under a real key decode.
    REQUIRE(DecodeRoster(encodeWith(std::span<std::byte const> { key })).has_value());

    auto const zero = Ed25519PublicKey {};
    for (auto const keyField: { std::span<std::byte const> {}, std::span<std::byte const> { zero } })
    {
        INFO("key field of " << keyField.size() << " bytes");
        auto const refused = DecodeRoster(encodeWith(keyField));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ConsensusErrorCode::MalformedFrame);
        CHECK(refused.error().context.contains("a roster principal holds no identity key"));
    }

    // And a principal entry malformed some other way is not reported as keyless.
    auto const damaged = DecodeRoster(encodeWith(std::span<std::byte const> { key }.first(Ed25519PublicKeyBytes - 1)));
    REQUIRE_FALSE(damaged.has_value());
    CHECK(damaged.error().context.contains("a roster entry is malformed"));
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
