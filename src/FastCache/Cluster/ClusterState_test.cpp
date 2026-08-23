// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// A command, spelled once so the cases below vary only what they are about.
/// @param kind What it does.
/// @param key The member id or setting name.
/// @param value The endpoint or setting value.
/// @return The command.
[[nodiscard]] Command Cmd(CommandKind kind, std::string key, std::string value = {})
{
    return Command { .kind = kind, .key = std::move(key), .value = std::move(value) };
}
} // namespace

TEST_CASE("A member is admitted with an endpoint, never without one", "[cluster][state]")
{
    // The residual `RaftMembership` recorded, closed here. Consensus carries ids
    // alone -- correct, since an id is all a quorum count needs -- which left a node
    // the cluster had agreed to admit unreachable until something else supplied its
    // address. Admitting and addressing are one decision now, replicated together.
    CHECK(Validate(Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675")).has_value());

    // Refused at the PROPOSER, which is the only place a change can be refused: an
    // entry is applied after it is committed, so by then there is nobody left to
    // report to and no way to un-commit it.
    auto const addressless = Validate(Cmd(CommandKind::AddMember, "n1"));
    REQUIRE_FALSE(addressless.has_value());
    CHECK(addressless.error().context.find("endpoint") != std::string::npos);

    CHECK_FALSE(Validate(Cmd(CommandKind::AddMember, "", "10.0.0.1:6675")).has_value());
}

TEST_CASE("A setting this build does not know is refused, not stored", "[cluster][state]")
{
    // A key nobody knows would otherwise be replicated to every node, snapshotted and
    // carried across restarts while doing nothing -- and the only symptom would be
    // that the thing the operator configured did not happen.
    CHECK(Validate(Cmd(CommandKind::SetSetting, "upstream", "cache.internal:6674")).has_value());
    CHECK(Validate(Cmd(CommandKind::SetSetting, "fleet-open", "1")).has_value());

    auto const unknown = Validate(Cmd(CommandKind::SetSetting, "upsteam", "typo"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().context.find("upsteam") != std::string::npos);

    // The table is the only source of truth for what is a setting, so a local flag
    // that describes ONE machine is not one -- replicating `--slots` would impose one
    // host's size on all of them.
    CHECK(FindSetting("slots") == nullptr);
}

TEST_CASE("Applying is total, and admitting a known member moves it", "[cluster][state]")
{
    // `Apply` cannot fail, which is a property consensus needs rather than a
    // convenience: it runs after commitment, when refusing is no longer an option.
    ClusterState state;

    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    REQUIRE(state.members.size() == 2);

    // Sorted by id, so two nodes that applied the same entries hold byte-identical
    // state whatever order the entries arrived in.
    CHECK(state.members[0].id == "n1");
    CHECK(state.members[1].id == "n2");

    // One verb for "join" and "moved": a node that moved has the same identity and a
    // new address, and making an operator remove it first would leave a window in
    // which the cluster has agreed it does not exist.
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.9:6675"));
    CHECK(state.members.size() == 2);
    CHECK(Unwrap(state.EndpointOf("n1")) == "10.0.0.9:6675");

    Apply(state, Cmd(CommandKind::RemoveMember, "n1"));
    CHECK(state.members.size() == 1);
    CHECK_FALSE(state.EndpointOf("n1").has_value());

    // Removing something that is not there is a no-op rather than a fault, for the
    // same reason: by the time this runs the cluster has already agreed to it.
    Apply(state, Cmd(CommandKind::RemoveMember, "n1"));
    CHECK(state.members.size() == 1);
}

TEST_CASE("A command round-trips, and an unknown verb is refused", "[cluster][state][wire]")
{
    // Every field a different value, so a transposition cannot survive.
    auto const original = Cmd(CommandKind::SetSetting, "upstream", "cache.internal:6674");
    auto const decoded = DecodeCommand(Encode(original));
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded) == original);

    // A verb byte this build does not know is refused rather than applied as
    // whichever enumerator it happens to alias -- which would change the cluster's
    // state in a way nobody wrote down.
    auto bytes = Encode(original);
    // The verb sits in the second byte of the first field, after that field's u32
    // length prefix.
    bytes[5] = static_cast<std::byte>(0xEE);
    CHECK_FALSE(DecodeCommand(bytes).has_value());

    // And a payload that is not a command at all.
    CHECK_FALSE(DecodeCommand({}).has_value());
}

TEST_CASE("A whole state round-trips, members and settings apart", "[cluster][state][wire]")
{
    // The snapshot format, and the case that matters is the boundary: members and
    // settings are both `(string, string)` pairs, so a count read wrongly would
    // silently turn a member into a setting or the reverse -- a cluster that had
    // agreed on three members recovering with two.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::SetSetting, "upstream", "cache.internal:6674"));

    auto const restored = DecodeState(Encode(state));
    REQUIRE(restored.has_value());
    CHECK(Unwrap(restored) == state);
    CHECK(Unwrap(restored).members.size() == 2);
    CHECK(Unwrap(restored).settings.size() == 1);

    // An empty state is a legitimate one -- a cluster that has agreed nothing yet --
    // and must survive the round trip rather than being read as malformed.
    ClusterState const empty;
    auto const emptyBack = DecodeState(Encode(empty));
    REQUIRE(emptyBack.has_value());
    CHECK(Unwrap(emptyBack) == empty);
}

TEST_CASE("A truncated snapshot is refused rather than half-read", "[cluster][state][wire]")
{
    // Read as far as it goes, a truncated snapshot yields a member with an empty
    // endpoint -- which this node would then replicate onward as an address nobody
    // can dial, and which the fleet would lease out.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));

    auto bytes = Encode(state);
    REQUIRE(bytes.size() > 8);
    bytes.resize(bytes.size() - 4);
    CHECK_FALSE(DecodeState(bytes).has_value());

    // A member count larger than the pairs present is the same fault reached by a
    // different route, and must fail the same way.
    auto overcounted = Encode(state);
    overcounted[8] = static_cast<std::byte>(0xFF);
    CHECK_FALSE(DecodeState(overcounted).has_value());
}

TEST_CASE("Endpoints come back in the order a membership oracle wants", "[cluster][state]")
{
    // `Distributed::ClusterMembership` takes endpoints and keys on hosts, so this is
    // the shape the two layers meet in.
    ClusterState state;
    Apply(state, Cmd(CommandKind::AddMember, "n2", "10.0.0.2:6675"));
    Apply(state, Cmd(CommandKind::AddMember, "n1", "10.0.0.1:6675"));

    CHECK(state.Endpoints() == std::vector<std::string> { "10.0.0.1:6675", "10.0.0.2:6675" });
}
