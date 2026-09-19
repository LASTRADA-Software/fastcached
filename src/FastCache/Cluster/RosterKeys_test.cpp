// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
          Command { .kind = CommandKind::Forget,
                    .key = "n2",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = std::nullopt,
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

TEST_CASE("A forgotten machine is reported as itself whatever id it claims", "[cluster][roster][revocation]")
{
    // The id a revocation carries is whose the key WAS, and the removed machine may claim any id
    // it likes. A roster narrowing the revoked keys to the claimed id would call n3's proof a key
    // nobody gave -- or a forgery of n2 -- when it is the removed machine, which is what the
    // signed `KeyRevoked` verdict exists to tell it.
    auto const bootstrap = std::vector { Typed("n1", KeyOf("n1")), Typed("n2", KeyOf("n2")), Typed("n3", KeyOf("n3")) };
    RosterKeys roster { TestKeyPair("n1"), bootstrap };
    Consensus::RaftPeerIdentity const identity { "n1", roster };

    ClusterState state;
    state.members = bootstrap;
    Apply(state,
          Command { .kind = CommandKind::Forget,
                    .key = "n3",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = std::nullopt,
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

TEST_CASE("A forgotten member keeps its own key here until the configuration drops it, and only for itself",
          "[cluster][roster][revocation][forget]")
{
    // #1555. The forget revokes n3's key in the committed entry; the configuration drops n3 a
    // reconcile pass later. In between n3 is still counted, so cutting it off would take a
    // voter out of every quorum while nobody has yet proposed its removal -- and a cluster that
    // then loses one more voter can be left unable to elect anyone who would.
    auto const bootstrap = std::vector { Typed("n1", KeyOf("n1")), Typed("n2", KeyOf("n2")), Typed("n3", KeyOf("n3")) };
    RosterKeys roster { TestKeyPair("n1"), bootstrap };
    Consensus::RaftPeerIdentity const identity { "n1", roster };

    ClusterState state;
    state.members = bootstrap;
    Apply(state,
          Command { .kind = CommandKind::Forget,
                    .key = "n3",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = std::nullopt,
                    .role = std::nullopt });
    REQUIRE(state.IsRevoked(KeyOf("n3")));
    roster.Adopt(state);
    roster.AdoptConfiguration(Consensus::Configuration { .voters = { "n1", "n2", "n3" }, .learners = {} });

    // Counted: n3's own key is still n3's, and nothing reports it revoked FOR n3.
    CHECK(identity.StillProves("n3", KeyOf("n3")));
    CHECK(roster.KeysOf("n3").live == std::optional { KeyOf("n3") });
    CHECK_FALSE(std::ranges::contains(roster.KeysOf("n3").revoked, KeyOf("n3")));

    // Only for n3: the same key claiming another id is the revoked machine, as ever.
    CHECK(std::ranges::contains(roster.KeysOf("n2").revoked, KeyOf("n3")));
    CHECK_FALSE(identity.StillProves("n2", KeyOf("n3")));

    // As a learner too -- a configuration counts both sets.
    roster.AdoptConfiguration(Consensus::Configuration { .voters = { "n1", "n2" }, .learners = { "n3" } });
    CHECK(identity.StillProves("n3", KeyOf("n3")));

    // Dropped, and the key is refused at the next frame.
    roster.AdoptConfiguration(Consensus::Configuration { .voters = { "n1", "n2" }, .learners = {} });
    CHECK_FALSE(identity.StillProves("n3", KeyOf("n3")));
    CHECK(std::ranges::contains(roster.KeysOf("n3").revoked, KeyOf("n3")));
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

TEST_CASE("An applied forget withdraws the key from every session it proved, though --raft-peer still types it",
          "[cluster][roster][revocation][forget]")
{
    // The whole chain a session re-asks before each frame: the replicated command, the roster
    // adopting the state it produced, and the identity answering from the roster.
    //
    // #1555: n3 is TYPED into this node's `--raft-peer` with its key, which is what makes the
    // revocation load-bearing. Removing the record alone leaves the typed key standing -- the
    // state states no key for n3 any more, so the roster falls back to the command line -- and
    // the forgotten machine goes on proving itself here for as long as it runs.
    auto const bootstrap = std::vector { Typed("n1", KeyOf("n1")), Typed("n3", KeyOf("n3")) };
    RosterKeys roster { TestKeyPair("n1"), bootstrap };
    Consensus::RaftPeerIdentity const identity { "n1", roster };

    ClusterState state;
    state.members = bootstrap;
    roster.Adopt(state);
    REQUIRE(identity.StillProves("n3", KeyOf("n3")));

    Apply(state,
          Command { .kind = CommandKind::Forget,
                    .key = "n3",
                    .value = {},
                    .schedulerEndpoint = {},
                    .publicKey = std::nullopt,
                    .role = std::nullopt });
    REQUIRE(std::ranges::none_of(state.members, [](ClusterMember const& m) { return m.id == "n3"; }));
    roster.Adopt(state);
    // And the configuration no longer counts n3 -- the removal the forget leads to, which the
    // case below holds apart from the revocation.
    roster.AdoptConfiguration(Consensus::Configuration { .voters = { "n1" }, .learners = {} });

    CHECK_FALSE(identity.StillProves("n3", KeyOf("n3")));
    CHECK_FALSE(roster.KeysOf("n3").live.has_value());

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
