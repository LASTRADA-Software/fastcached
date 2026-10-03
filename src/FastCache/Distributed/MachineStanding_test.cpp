// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/MachineStanding.hpp>
#include <FastCache/Distributed/MembershipWire.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;

namespace
{

/// A roster with a voter `v1`, a learner `l1`, and `gone`, whose key was revoked.
[[nodiscard]] Cluster::Roster OfficeRoster()
{
    auto roster = Cluster::Roster {};
    roster.members.push_back(Cluster::RosterMember { .id = "v1",
                                                     .raftEndpoint = "v1:6680",
                                                     .seat = Cluster::MemberSeat::Voter,
                                                     .publicKey = Testing::TestKeyPair("v1").PublicKey(),
                                                     .schedulerEndpoint = {} });
    roster.members.push_back(Cluster::RosterMember { .id = "l1",
                                                     .raftEndpoint = {},
                                                     .seat = Cluster::MemberSeat::Learner,
                                                     .publicKey = Testing::TestKeyPair("l1").PublicKey(),
                                                     .schedulerEndpoint = {} });
    roster.revoked.push_back(Cluster::RevokedKey { .id = "gone", .publicKey = Testing::TestKeyPair("gone").PublicKey() });
    return roster;
}

/// @param id A test machine.
/// @return Its key's text form, as an operator pastes it.
[[nodiscard]] std::string KeyText(std::string const& id)
{
    return FormatEd25519PublicKey(Testing::TestKeyPair(id).PublicKey());
}

} // namespace

TEST_CASE("A machine's standing is read from the roster, by id or by key", "[distributed][admission]")
{
    auto const roster = OfficeRoster();

    struct Row
    {
        std::string subject;
        bool pending;
        MachineStanding expected;
    };
    auto const rows = std::to_array<Row>({
        { .subject = "v1", .pending = false, .expected = MachineStanding::Voter },
        { .subject = "l1", .pending = false, .expected = MachineStanding::Learner },
        { .subject = "gone", .pending = false, .expected = MachineStanding::Revoked },
        { .subject = "new-pc", .pending = true, .expected = MachineStanding::Pending },
        { .subject = "new-pc", .pending = false, .expected = MachineStanding::Unknown },
        // A recorded machine is its seat whatever the window says: pending names an unrecorded id.
        { .subject = "l1", .pending = true, .expected = MachineStanding::Learner },
        { .subject = KeyText("l1"), .pending = false, .expected = MachineStanding::Learner },
        { .subject = KeyText("gone"), .pending = false, .expected = MachineStanding::Revoked },
        { .subject = KeyText("nobody"), .pending = false, .expected = MachineStanding::Unknown },
        // The window is keyed by id, so a KEY is never pending.
        { .subject = KeyText("nobody"), .pending = true, .expected = MachineStanding::Unknown },
    });
    for (auto const& row: rows)
    {
        INFO(row.subject << " pending=" << row.pending);
        CHECK(StandingOfMachine(roster, row.subject, row.pending) == row.expected);
    }

    // An id admitted again under a NEW key after a revocation is its seat, not revoked -- and the
    // OLD key is still revoked.
    auto readmitted = roster;
    readmitted.members.push_back(Cluster::RosterMember { .id = "gone",
                                                         .raftEndpoint = {},
                                                         .seat = Cluster::MemberSeat::Learner,
                                                         .publicKey = Testing::TestKeyPair("gone-2").PublicKey(),
                                                         .schedulerEndpoint = {} });
    CHECK(StandingOfMachine(readmitted, "gone", false) == MachineStanding::Learner);
    CHECK(StandingOfMachine(readmitted, KeyText("gone"), false) == MachineStanding::Revoked);
    CHECK(StandingOfMachine(readmitted, KeyText("gone-2"), false) == MachineStanding::Learner);
}

TEST_CASE("The key a machine question folds is the live key, the revoked key, or none", "[distributed][admission]")
{
    auto const roster = OfficeRoster();
    auto const identity = [](std::string const& id, std::string const& keyOf) {
        return std::optional { ProvenIdentity { .id = id, .key = Testing::TestKeyPair(keyOf).PublicKey() } };
    };

    CHECK(KeyOfMachine(roster, "v1") == identity("v1", "v1"));
    CHECK(KeyOfMachine(roster, KeyText("l1")) == identity("l1", "l1"));
    // The revoked key, under the id it was recorded under, so the fold answers `Forgotten`.
    CHECK(KeyOfMachine(roster, "gone") == identity("gone", "gone"));
    CHECK(KeyOfMachine(roster, KeyText("gone")) == identity("gone", "gone"));
    CHECK_FALSE(KeyOfMachine(roster, "new-pc").has_value());
    CHECK_FALSE(KeyOfMachine(roster, KeyText("nobody")).has_value());

    // Readmitted: the id is the NEW key; the old key is still the revoked one.
    auto readmitted = roster;
    readmitted.members.push_back(Cluster::RosterMember { .id = "gone",
                                                         .raftEndpoint = {},
                                                         .seat = Cluster::MemberSeat::Learner,
                                                         .publicKey = Testing::TestKeyPair("gone-2").PublicKey(),
                                                         .schedulerEndpoint = {} });
    CHECK(KeyOfMachine(readmitted, "gone") == identity("gone", "gone-2"));
    CHECK(KeyOfMachine(readmitted, KeyText("gone")) == identity("gone", "gone"));
}

TEST_CASE("Every standing travels as its own byte and comes back as itself", "[distributed][admission][wire]")
{
    for (auto const& row: MachineStandingWire)
    {
        CHECK(OnTheWire(row.standing) == row.tag);
        CHECK(StandingOnTheWire(row.tag) == std::optional { row.standing });
    }
    CHECK_FALSE(StandingOnTheWire(static_cast<CompileCacheWire::WireMachineStanding>(0x7F)).has_value());
}
