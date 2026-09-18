// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::TestKeyPair;

namespace
{

/// A member as a command line names it, with the key typed for it -- or none.
[[nodiscard]] ClusterMember Typed(std::string const& id, std::optional<Ed25519PublicKey> key)
{
    return ClusterMember { .id = id,
                           .raftEndpoint = "10.0.0.1:6680",
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = MemberSeat::Voter,
                           .publicKey = key };
}

/// The key a test gives @p machine.
[[nodiscard]] Ed25519PublicKey KeyOf(std::string const& machine)
{
    return TestKeyPair(machine).PublicKey();
}

/// n1's roster, over a command line naming n1, n2 with its key, and n3 with none.
[[nodiscard]] std::vector<ClusterMember> Bootstrap()
{
    return { Typed("n1", KeyOf("n1")), Typed("n2", KeyOf("n2")), Typed("n3", std::nullopt) };
}

} // namespace

TEST_CASE("A roster answers from the command line until the cluster says anything", "[cluster][roster]")
{
    // Before a cluster has elected, the typed keys are the only ones anything can be verified
    // against -- so a member typed without one is a member nothing can verify yet.
    auto const bootstrap = Bootstrap();
    RosterKeys const roster { TestKeyPair("n1"), bootstrap };

    CHECK(roster.KeysOf("n2").live == KeyOf("n2"));
    CHECK(roster.KeysOf("n2").revoked.empty());
    CHECK_FALSE(roster.KeysOf("n3").live.has_value());
    CHECK_FALSE(roster.KeysOf("n9").live.has_value());
}

TEST_CASE("The replicated state wins wherever it states a key", "[cluster][roster]")
{
    auto const bootstrap = Bootstrap();
    RosterKeys roster { TestKeyPair("n1"), bootstrap };

    // n2 re-admitted under a new key a year after its command line was written, and n3's key
    // arriving from the cluster where the command line had none.
    ClusterState state;
    state.members = { Typed("n2", KeyOf("n2-reinstalled")), Typed("n3", KeyOf("n3")) };
    roster.Adopt(state);

    CHECK(roster.KeysOf("n2").live == KeyOf("n2-reinstalled"));
    CHECK(roster.KeysOf("n3").live == KeyOf("n3"));

    // A member the state records with NO key falls back to the typed one: admitted before it
    // stated one, which is not a statement that it has none.
    ClusterState unstated;
    unstated.members = { Typed("n2", std::nullopt) };
    roster.Adopt(unstated);
    CHECK(roster.KeysOf("n2").live == KeyOf("n2"));
}

TEST_CASE("A typed key the cluster revoked is revoked, whatever the command line says", "[cluster][roster]")
{
    // The removal direction, which fails OPEN if it is got wrong: a node restarted with the
    // command line it was always given must not bring a revoked member's key back.
    auto const bootstrap = Bootstrap();
    RosterKeys roster { TestKeyPair("n1"), bootstrap };

    ClusterState state;
    state.members = { Typed("n2", KeyOf("n2")) };
    Apply(state,
          Command { .kind = CommandKind::RevokeKey,
                    .key = "n2",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = KeyOf("n2"),
                    .role = std::nullopt });
    REQUIRE(state.IsRevoked(KeyOf("n2")));
    roster.Adopt(state);

    auto const keys = roster.KeysOf("n2");
    CHECK_FALSE(keys.live.has_value());
    REQUIRE(keys.revoked.size() == 1);
    CHECK(keys.revoked.front() == KeyOf("n2"));

    // And handed back whatever id is asked about: the id a revocation carries is a label, so
    // which key signed is the signature's question, never the claim's.
    CHECK(roster.KeysOf("n3").revoked == std::vector { KeyOf("n2") });
}

TEST_CASE("A revoked machine is reported as itself whatever the revocation's label and whatever id it claims",
          "[cluster][roster][revocation]")
{
    // `RevokeKey` takes the key from whatever holds it, and the id it names is a LABEL. So
    // revoke n3's key under a label that is no member's id: a roster narrowing the revoked keys
    // to the claimed id would call n3's proof a key nobody gave -- or a forgery of n2 -- when it
    // is the removed machine, which is what the signed `KeyRevoked` verdict exists to tell it.
    auto const bootstrap = std::vector { Typed("n1", KeyOf("n1")), Typed("n2", KeyOf("n2")), Typed("n3", KeyOf("n3")) };
    RosterKeys roster { TestKeyPair("n1"), bootstrap };
    Consensus::RaftPeerIdentity const identity { "n1", roster };

    ClusterState state;
    state.members = bootstrap;
    Apply(state,
          Command { .kind = CommandKind::RevokeKey,
                    .key = "rack-4-decommissioned",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = KeyOf("n3"),
                    .role = std::nullopt });
    REQUIRE(state.IsRevoked(KeyOf("n3")));
    roster.Adopt(state);

    auto const fields = std::vector { WireFields::AsBytes("a proof") };
    auto const signedBy = [&fields](std::string const& machine) {
        RosterKeys const keys { TestKeyPair(machine), {} };
        return Consensus::RaftPeerIdentity { machine, keys }.Sign(Consensus::RaftPeerSignature::DiallerProof,
                                                                  WireFields::FieldList { fields });
    };
    auto const verify = [&identity, &fields](std::string const& claimed, Ed25519Signature const& signature) {
        return identity.Verify(
            Consensus::RaftPeerSignature::DiallerProof, claimed, WireFields::FieldList { fields }, signature);
    };

    // n3 claiming its own id, another member's, and this node's own: the removed machine each
    // time, named by the key that signed.
    auto const byN3 = signedBy("n3");
    for (auto const& claimed: std::vector<std::string> { "n3", "n2", "n1" })
    {
        CAPTURE(claimed);
        auto const verdict = verify(claimed, byN3);
        CHECK(verdict.check == Consensus::SignerCheck::Revoked);
        CHECK(verdict.key == KeyOf("n3"));
    }

    // The control, under the same roster: a key nobody revoked still reads as what it is -- a
    // key nobody gave for an id with none live, and a forgery of an id with one.
    auto const byStranger = signedBy("n9");
    CHECK(verify("n3", byStranger).check == Consensus::SignerCheck::Unknown);
    CHECK(verify("n2", byStranger).check == Consensus::SignerCheck::Forged);
    CHECK(verify("n1", byStranger).check == Consensus::SignerCheck::Forged);
}

TEST_CASE("A principal is a stranger on the Raft peer wire", "[cluster][roster]")
{
    // A principal never joins consensus, so its admitted key proves nothing here.
    auto const bootstrap = Bootstrap();
    RosterKeys roster { TestKeyPair("n1"), bootstrap };

    ClusterState state;
    state.principals = { ClusterPrincipal { .id = "w1", .publicKey = KeyOf("w1"), .role = PrincipalRole::Worker } };
    roster.Adopt(state);

    CHECK_FALSE(roster.KeysOf("w1").live.has_value());
}

TEST_CASE("A roster signs as its own key and nothing else", "[cluster][roster]")
{
    auto const bootstrap = Bootstrap();
    RosterKeys const roster { TestKeyPair("n1"), bootstrap };

    CHECK(roster.OwnPublicKey() == KeyOf("n1"));
    auto const message = WireFields::AsBytes("a transcript");
    auto const signature = roster.SignAsSelf(message);
    CHECK(Ed25519Verify(KeyOf("n1"), message, signature));
    CHECK_FALSE(Ed25519Verify(KeyOf("n2"), message, signature));
}

TEST_CASE("An applied RevokeKey withdraws the key from every session it proved", "[cluster][roster][revocation]")
{
    // The whole chain a session re-asks before each frame: the replicated command, the roster
    // adopting the state it produced, and the identity answering from the roster.
    auto const bootstrap = std::vector { Typed("n1", KeyOf("n1")), Typed("n3", KeyOf("n3")) };
    RosterKeys roster { TestKeyPair("n1"), bootstrap };
    Consensus::RaftPeerIdentity const identity { "n1", roster };

    ClusterState state;
    state.members = bootstrap;
    roster.Adopt(state);
    REQUIRE(identity.StillProves("n3", KeyOf("n3")));

    Apply(state,
          Command { .kind = CommandKind::RevokeKey,
                    .key = "n3",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = KeyOf("n3"),
                    .role = std::nullopt });
    roster.Adopt(state);

    CHECK_FALSE(identity.StillProves("n3", KeyOf("n3")));

    // And a signature n3 makes now -- through its own identity, with the key it always had --
    // is reported as the removed machine, not as a forgery.
    RosterKeys const n3Keys { TestKeyPair("n3"), {} };
    Consensus::RaftPeerIdentity const n3 { "n3", n3Keys };
    auto const fields = std::vector { WireFields::AsBytes("n3's proof") };
    auto const signature = n3.Sign(Consensus::RaftPeerSignature::DiallerProof, WireFields::FieldList { fields });
    CHECK(
        identity.Verify(Consensus::RaftPeerSignature::DiallerProof, "n3", WireFields::FieldList { fields }, signature).check
        == Consensus::SignerCheck::Revoked);
}
