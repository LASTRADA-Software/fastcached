// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <tests/LeaseRosterFakes.hpp>
#include <tests/LocalityFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::FixedLeaseRoster;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;
using namespace std::chrono_literals;

namespace
{

constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// The latest expiry a ticket can carry and still decode on this host.
constexpr auto ExpiryCeiling = std::chrono::system_clock::time_point { std::chrono::seconds {
    static_cast<std::int64_t>(MaxMachineTicketExpirySeconds) } };

[[nodiscard]] std::uint64_t UnixSeconds(std::chrono::system_clock::time_point at)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count());
}

/// An audience that is exactly one endpoint.
class OneEndpoint final: public IAudience
{
  public:
    explicit OneEndpoint(std::string endpoint):
        _endpoint { std::move(endpoint) }
    {
    }
    [[nodiscard]] bool Matches(std::string_view audience) const override
    {
        return audience == _endpoint;
    }

  private:
    std::string _endpoint;
};

struct TicketSpec
{
    std::string claimed { "pc-07" };
    std::string signer { "pc-07" };
    std::string audience { "office.corp:6674" };
    std::chrono::system_clock::time_point expires { Noon + MachineTicketLifetime };
    std::uint8_t nonce { 1 };
};

[[nodiscard]] std::vector<std::byte> Ticket(TicketSpec const& spec)
{
    auto nonce = MachineTicketNonce {};
    nonce.fill(static_cast<std::byte>(spec.nonce));
    auto const secure = MintMachineTicket(TestKeyPair(spec.signer),
                                          MachineTicketClaims { .machineId = spec.claimed,
                                                                .audience = spec.audience,
                                                                .expiresAtUnixSeconds = UnixSeconds(spec.expires),
                                                                .nonce = nonce });
    auto const view = WireFields::AsBytes(secure.View());
    return { view.begin(), view.end() };
}

/// The refusal a verification answered, or nullopt when it accepted: a case expecting a refusal
/// then FAILS on an acceptance, where reading `.error()` of a value would end the process.
/// @param result What `Verify` answered.
/// @return Its refusal, if it refused.
[[nodiscard]] std::optional<TicketRefusal> RefusalOf(std::expected<ProvenIdentity, TicketRejection> const& result)
{
    return result.has_value() ? std::nullopt : std::optional { result.error().Reason() };
}

/// The id a refusal's revoked-key evidence names, or nullopt when it carries none -- including
/// when the verification ACCEPTED, so a case expecting evidence fails rather than ends the process.
/// @param result What `Verify` answered.
/// @return The forgotten machine's id, if the refusal named one.
[[nodiscard]] std::optional<std::string> RevokedIdOf(std::expected<ProvenIdentity, TicketRejection> const& result)
{
    if (result.has_value() || !result.error().Revoked().has_value())
        return std::nullopt;
    return std::string { result.error().Revoked()->Id() };
}

/// A rejection for @p reason, built as `Verify` builds one: from a constant, or -- for a revoked
/// key, the one reason that cannot be built without it -- from pc-07's evidence.
/// @param reason Why.
/// @return The rejection.
[[nodiscard]] TicketRejection RejectionFor(TicketRefusal reason)
{
    switch (reason)
    {
        case TicketRefusal::Malformed:
            return TicketRejection { TicketRefusal::Malformed };
        case TicketRefusal::NotUtf8:
            return TicketRejection { TicketRefusal::NotUtf8 };
        case TicketRefusal::NoRoster:
            return TicketRejection { TicketRefusal::NoRoster };
        case TicketRefusal::UnknownMachine:
            return TicketRejection { TicketRefusal::UnknownMachine };
        case TicketRefusal::Forged:
            return TicketRejection { TicketRefusal::Forged };
        case TicketRefusal::Revoked:
            return TicketRejection::RevokedKey(
                RevokedKeyEvidence { ProvenIdentity { .id = "pc-07", .key = Testing::TestKeyPair("pc-07").PublicKey() } });
        case TicketRefusal::WrongAudience:
            return TicketRejection { TicketRefusal::WrongAudience };
        case TicketRefusal::Expired:
            return TicketRejection { TicketRefusal::Expired };
        case TicketRefusal::Replayed:
            return TicketRejection { TicketRefusal::Replayed };
        case TicketRefusal::SpentSetFull:
            return TicketRejection { TicketRefusal::SpentSetFull };
        case TicketRefusal::Last:
            break;
    }
    std::unreachable(); // `Last` is a count, and `TicketRefusalTable` holds no row for it.
}

struct Verifying
{
    FixedLeaseRoster roster { { "scheduler" } };
    OneEndpoint audience { "office.corp:6674" };
    SpentTickets spent;
    TicketVerifier verifier { &roster, audience, spent };

    Verifying()
    {
        roster.AdmitMachine("pc-07");
    }
};

} // namespace

TEST_CASE("A ticket from a machine the roster admits names that machine and the key it verified under",
          "[distributed][ticket]")
{
    Verifying fix;
    auto const verified = fix.verifier.Verify(Ticket({}), Noon);
    REQUIRE(verified.has_value());
    CHECK(verified->id == "pc-07");
    CHECK(verified->key == TestKeyPair("pc-07").PublicKey());
}

TEST_CASE("Each way a ticket fails is refused by its own name", "[distributed][ticket]")
{
    struct Row
    {
        char const* what;
        TicketSpec spec;
        TicketRefusal expected;
        char const* admit = nullptr; ///< A machine the roster admits for this row alone.
    };
    auto const rows = std::to_array<Row>({
        { .what = "signed by another key under this machine's id",
          .spec = { .signer = "impostor" },
          .expected = TicketRefusal::Forged },
        { .what = "a machine the roster never admitted",
          .spec = { .claimed = "stranger", .signer = "stranger" },
          .expected = TicketRefusal::UnknownMachine },
        { .what = "an endpoint that is not this node",
          .spec = { .audience = "other.corp:6674" },
          .expected = TicketRefusal::WrongAudience },
        { .what = "past its expiry and the slack",
          .spec = { .expires = Noon - LeaseTokenClockSkewSlack - 1s },
          .expected = TicketRefusal::Expired },
        { .what = "further ahead than any minter issues",
          .spec = { .expires = Noon + MachineTicketLifetime + LeaseTokenClockSkewSlack + 1s },
          .expected = TicketRefusal::Expired },
        { .what = "at the latest expiry this host's clock can hold",
          .spec = { .expires = ExpiryCeiling },
          .expected = TicketRefusal::Expired },
        { .what = "an admitted machine whose id is not text, signed by the key it names",
          .spec = { .claimed = "pc-\xff", .signer = "pc-\xff" },
          .expected = TicketRefusal::NotUtf8,
          .admit = "pc-\xff" },
        { .what = "a stranger whose id is not text: a stranger, like any other",
          .spec = { .claimed = "pc-\xff", .signer = "pc-\xff" },
          .expected = TicketRefusal::UnknownMachine },
        { .what = "an audience that is not text",
          .spec = { .audience = "office.corp\xff:6674" },
          .expected = TicketRefusal::NotUtf8 },
    });
    for (auto const& row: rows)
    {
        INFO(row.what);
        Verifying fix;
        if (row.admit != nullptr)
            fix.roster.AdmitMachine(row.admit);
        CHECK(RefusalOf(fix.verifier.Verify(Ticket(row.spec), Noon)) == std::optional { row.expected });
    }

    SECTION("a machine the cluster forgot")
    {
        Verifying fix;
        fix.roster.Revoke("pc-07");
        auto const refused = fix.verifier.Verify(Ticket({}), Noon);
        CHECK(RefusalOf(refused) == std::optional { TicketRefusal::Revoked });
        // And it SAYS whose key it was, which is what refuses the forgotten machine's later verbs.
        CHECK(RevokedIdOf(refused) == std::optional<std::string> { "pc-07" });
    }
    SECTION("a machine whose key the roster answers live AND revoked")
    {
        // The revoked list is asked outright, never trusted to have been filtered out of `live`:
        // a roster that still admits the member while revoking its key has been told it is gone.
        Verifying fix;
        fix.roster.RevokeKeyOnly("pc-07");
        REQUIRE(fix.roster.MachineKeysOf("pc-07").live.has_value()); // the premise: it still answers live
        auto const refused = fix.verifier.Verify(Ticket({}), Noon);
        CHECK(RefusalOf(refused) == std::optional { TicketRefusal::Revoked });
        CHECK(RevokedIdOf(refused) == std::optional<std::string> { "pc-07" });
        // And a revoked live key is no live key: another key's signature is no forgery of it -- and
        // carries no revoked evidence, since no revoked key signed it.
        auto const impostor = fix.verifier.Verify(Ticket({ .signer = "impostor", .nonce = 2 }), Noon);
        CHECK(RefusalOf(impostor) == std::optional { TicketRefusal::UnknownMachine });
        CHECK_FALSE(RevokedIdOf(impostor).has_value());
    }
    SECTION("bytes that are no ticket")
    {
        Verifying fix;
        auto const garbage = std::vector<std::byte>(40, std::byte { 7 });
        CHECK(RefusalOf(fix.verifier.Verify(garbage, Noon)) == std::optional { TicketRefusal::Malformed });
    }
    SECTION("the same ticket twice inside its window, and again after it")
    {
        Verifying fix;
        auto const once = Ticket({});
        REQUIRE(fix.verifier.Verify(once, Noon).has_value());
        CHECK(RefusalOf(fix.verifier.Verify(once, Noon + 10s)) == std::optional { TicketRefusal::Replayed });
        CHECK(fix.verifier.Verify(Ticket({ .nonce = 2 }), Noon).has_value()); // a fresh nonce is fine
        // The same ticket past the window is EXPIRED, never replayed: the window refuses it first.
        auto const past = Noon + MachineTicketLifetime + LeaseTokenClockSkewSlack + 1s;
        CHECK(RefusalOf(fix.verifier.Verify(once, past)) == std::optional { TicketRefusal::Expired });
    }
    SECTION("a full spent set refuses by name, and a refused ticket takes no room")
    {
        Verifying fix;
        SpentTickets tiny { 1 };
        TicketVerifier bounded { &fix.roster, fix.audience, tiny };
        CHECK(RefusalOf(bounded.Verify(Ticket({ .signer = "impostor" }), Noon)) == std::optional { TicketRefusal::Forged });
        REQUIRE(bounded.Verify(Ticket({ .nonce = 4 }), Noon).has_value()); // the forged one took no room
        CHECK(RefusalOf(bounded.Verify(Ticket({ .nonce = 5 }), Noon)) == std::optional { TicketRefusal::SpentSetFull });
    }
    SECTION("a node holding no roster at all, and one whose state records no voter yet")
    {
        Verifying fix;
        auto const rosterless = TicketVerifier { nullptr, fix.audience, fix.spent };
        CHECK(RefusalOf(rosterless.Verify(Ticket({}), Noon)) == std::optional { TicketRefusal::NoRoster });
        fix.roster.SetStanding(RosterStanding::Absent);
        CHECK(RefusalOf(fix.verifier.Verify(Ticket({}), Noon)) == std::optional { TicketRefusal::NoRoster });
        // Recovery needs nothing but a voter applied: no power event, no restart.
        fix.roster.SetStanding(RosterStanding::Current);
        CHECK(fix.verifier.Verify(Ticket({ .nonce = 3 }), Noon).has_value());
    }
}

TEST_CASE("A genuine ticket whose claims are not text is refused before they are compared", "[distributed][ticket]")
{
    // The audience is compared with this node's endpoints, so a claim that is not text reaching
    // that comparison would answer wrong-audience. It is refused as not-utf8 first.
    Verifying fix;
    CHECK(RefusalOf(fix.verifier.Verify(Ticket({ .audience = "office.corp\xff:6674" }), Noon))
          == std::optional { TicketRefusal::NotUtf8 });
    // The control: a text audience naming another node is the comparison's refusal.
    CHECK(RefusalOf(fix.verifier.Verify(Ticket({ .audience = "other.corp:6674", .nonce = 2 }), Noon))
          == std::optional { TicketRefusal::WrongAudience });
}

TEST_CASE("Nothing is reported about a ticket whose signature did not verify", "[distributed][ticket]")
{
    // Forged AND for another audience AND long expired: the signature is read first, and the
    // other claims are an attacker's to choose, so they must not steer the answer.
    Verifying fix;
    auto const refused =
        fix.verifier.Verify(Ticket({ .signer = "impostor", .audience = "other.corp:6674", .expires = Noon - 24h }), Noon);
    CHECK(RefusalOf(refused) == std::optional { TicketRefusal::Forged });
    // Not even whether its claims are text: asked first, a stranger would move not-utf8 at will.
    CHECK(RefusalOf(fix.verifier.Verify(Ticket({ .signer = "impostor", .audience = "x\xff:6674" }), Noon))
          == std::optional { TicketRefusal::Forged });
}

TEST_CASE("The acceptance window adds and subtracts only on this machine's clock", "[distributed][ticket]")
{
    // A constant evaluation that overflows a signed tick count is not a constant expression, so
    // writing the slack on the presenter's side (`expiresAt + slack`) fails to COMPILE here: the
    // ceiling is within one second of the clock's maximum on every standard library.
    STATIC_REQUIRE_FALSE(TicketAcceptable(ExpiryCeiling, Noon));
    STATIC_REQUIRE(TicketAcceptable(Noon - LeaseTokenClockSkewSlack, Noon));
    STATIC_REQUIRE_FALSE(TicketAcceptable(Noon - LeaseTokenClockSkewSlack - 1s, Noon));
    STATIC_REQUIRE(TicketAcceptable(Noon + MachineTicketLifetime + LeaseTokenClockSkewSlack, Noon));
    STATIC_REQUIRE_FALSE(TicketAcceptable(Noon + MachineTicketLifetime + LeaseTokenClockSkewSlack + 1s, Noon));
}

TEST_CASE("A spent ticket is held exactly as long as it could be accepted, and no longer", "[distributed][ticket]")
{
    SpentTickets spent { 8 };
    auto const ticket = std::vector<std::byte>(32, std::byte { 1 });
    auto const expires = Noon + MachineTicketLifetime;
    REQUIRE(spent.Spend(ticket, expires, Noon) == SpendOutcome::Spent);
    // The last instant the verifier would still accept it: still held.
    REQUIRE(TicketAcceptable(expires, expires + LeaseTokenClockSkewSlack));
    CHECK(spent.Spend(ticket, expires, expires + LeaseTokenClockSkewSlack) == SpendOutcome::AlreadySpent);
    // One second past it: the verifier refuses the ticket, and the set lets it go by the SAME predicate.
    auto const past = expires + LeaseTokenClockSkewSlack + 1s;
    REQUIRE_FALSE(TicketAcceptable(expires, past));
    REQUIRE(spent.Spend(std::vector<std::byte>(32, std::byte { 2 }), past + MachineTicketLifetime, past)
            == SpendOutcome::Spent);
    CHECK(spent.Size() == 1);
}

TEST_CASE("The spent set is bounded by count, refuses when full, and frees room as the window passes",
          "[distributed][ticket]")
{
    SpentTickets spent { 2 };
    auto const at = [](std::uint8_t fill) {
        return std::vector<std::byte>(32, std::byte { fill });
    };
    auto const expires = Noon + MachineTicketLifetime;
    CHECK(spent.Spend(at(1), expires, Noon) == SpendOutcome::Spent);
    CHECK(spent.Spend(at(2), expires, Noon) == SpendOutcome::Spent);
    CHECK(spent.Spend(at(3), expires, Noon) == SpendOutcome::Full);
    CHECK(spent.Size() == 2); // nothing live was dropped to make room
    auto const later = expires + LeaseTokenClockSkewSlack + 1s;
    CHECK(spent.Spend(at(3), later + MachineTicketLifetime, later) == SpendOutcome::Spent);
    CHECK(spent.Size() == 1);
}

TEST_CASE("A clock stepped forward and then corrected cannot replay a spend the set let go of", "[distributed][ticket]")
{
    SpentTickets spent { 8 };
    auto const ticket = std::vector<std::byte>(32, std::byte { 1 });
    auto const expires = Noon + MachineTicketLifetime;
    REQUIRE(spent.Spend(ticket, expires, Noon) == SpendOutcome::Spent);
    // The clock jumps past the ticket's window; the next spend lets the entry go.
    auto const ahead = expires + LeaseTokenClockSkewSlack + 1s;
    REQUIRE(spent.Spend(std::vector<std::byte>(32, std::byte { 2 }), ahead + MachineTicketLifetime, ahead)
            == SpendOutcome::Spent);
    REQUIRE(spent.Size() == 1); // the premise: the first entry really is gone
    // Corrected, the ticket is inside its window again -- and refused, never spent a second time.
    REQUIRE(TicketAcceptable(expires, Noon));
    CHECK(spent.Spend(ticket, expires, Noon) == SpendOutcome::WindowPassed);

    SECTION("through the verifier, as expired")
    {
        Verifying fix;
        auto const once = Ticket({});
        REQUIRE(fix.verifier.Verify(once, Noon).has_value());
        auto const later = Ticket({ .expires = ahead + MachineTicketLifetime, .nonce = 2 });
        REQUIRE(fix.verifier.Verify(later, ahead).has_value());
        CHECK(RefusalOf(fix.verifier.Verify(once, Noon)) == std::optional { TicketRefusal::Expired });
        // And it heals: minted a second after the correction, a ticket expires past everything let go of.
        CHECK(fix.verifier.Verify(Ticket({ .expires = Noon + 1s + MachineTicketLifetime, .nonce = 3 }), Noon + 1s)
                  .has_value());
    }
}

TEST_CASE("A clock stepped back keeps what it spent, so stepping forward again replays nothing", "[distributed][ticket]")
{
    SpentTickets spent { 8 };
    auto const ticket = std::vector<std::byte>(32, std::byte { 1 });
    auto const expires = Noon + MachineTicketLifetime;
    REQUIRE(spent.Spend(ticket, expires, Noon) == SpendOutcome::Spent);
    // Two hours back, the entry is far AHEAD of the clock: the verifier would refuse its ticket
    // now, and the set keeps it anyway.
    auto const behind = Noon - 2h;
    REQUIRE_FALSE(TicketAcceptable(expires, behind));
    REQUIRE(spent.Spend(std::vector<std::byte>(32, std::byte { 2 }), behind + MachineTicketLifetime, behind)
            == SpendOutcome::Spent);
    CHECK(spent.Size() == 2);
    // Forward again: the ticket is acceptable, and still spent.
    CHECK(spent.Spend(ticket, expires, Noon) == SpendOutcome::AlreadySpent);
}

TEST_CASE("A ticket spent at one node is not spent at another", "[distributed][ticket]")
{
    Verifying first;
    Verifying second;
    auto const ticket = Ticket({});
    CHECK(first.verifier.Verify(ticket, Noon).has_value());
    CHECK(second.verifier.Verify(ticket, Noon).has_value());
}

TEST_CASE("A ticket's audience is this node's name or address at a port it serves, and never loopback",
          "[distributed][ticket][audience]")
{
    auto const own = OwnAudience { .names = { "office.corp", "OFFICE" }, .ports = { 6674 } };
    auto const locality = Testing::ThisMachineIs { "10.0.0.5" };
    struct Row
    {
        char const* audience;
        bool matches;
    };
    auto const rows = std::to_array<Row>({
        { .audience = "office.corp:6674", .matches = true },
        { .audience = "Office.Corp:6674", .matches = true }, // DNS names are case-insensitive
        { .audience = "office:6674", .matches = true },
        { .audience = "10.0.0.5:6674", .matches = true },     // an address of this machine: a dial hint
        { .audience = "office.corp:6675", .matches = false }, // a port this node does not serve
        { .audience = "other.corp:6674", .matches = false },
        { .audience = "127.0.0.1:6674", .matches = false }, // every node is its own loopback
        { .audience = "[::1]:6674", .matches = false },
        { .audience = "[::ffff:127.0.0.1]:6674", .matches = false },
        { .audience = "localhost:6674", .matches = false },
        { .audience = "LocalHost:6674", .matches = false },
        { .audience = "0.0.0.0:6674", .matches = false }, // the wildcard names no machine
        { .audience = "[::]:6674", .matches = false },
        { .audience = "6674", .matches = false },
        { .audience = "office.corp", .matches = false },
        { .audience = "", .matches = false },
    });
    for (auto const& row: rows)
    {
        INFO(row.audience);
        CHECK(AudienceMatches(row.audience, own, locality) == row.matches);
    }

    SECTION("loopback is refused even where the locality oracle would call it this machine")
    {
        auto const loopbackIsLocal = Testing::ThisMachineIs { "127.0.0.1" };
        CHECK_FALSE(AudienceMatches("127.0.0.1:6674", own, loopbackIsLocal));
        CHECK(AudienceMatches("office.corp:6674", own, loopbackIsLocal)); // the control
    }
}

namespace
{

/// The message a refusal reply carries; REQUIREs a `TicketRefused` error that decodes.
/// @param reply The reply bytes.
/// @return Its message.
[[nodiscard]] std::string TicketRefusedMessageOf(std::span<std::byte const> reply)
{
    REQUIRE(CompileCacheWire::DecodeReplyHeader(reply).has_value());
    auto const error = CompileCacheWire::DecodeErrorPayload(reply.subspan(CompileCacheWire::ReplyHeaderSize));
    REQUIRE(error.has_value());
    REQUIRE(Unwrap(error).first == CompileCacheWire::ErrorCode::TicketRefused);
    return std::string { Unwrap(error).second };
}

} // namespace

TEST_CASE("Every ticket refusal travels as ticket-refused, says what its row allows and moves its own counter",
          "[distributed][ticket][metrics]")
{
    // The three refusals that depend on the roster's entry for the claimed id say one thing; every
    // other one says its name. Pinned as a SET, so a row moved between the two is a red here.
    auto const rosterDependent =
        std::to_array({ TicketRefusal::UnknownMachine, TicketRefusal::Forged, TicketRefusal::Revoked });
    for (auto const& row: TicketRefusalTable)
    {
        INFO(row.name);
        AtomicMetricsSink metrics;
        auto const message = TicketRefusedMessageOf(AnswerTicketRefusal(metrics, row.reason));
        CHECK(message == (std::ranges::contains(rosterDependent, row.reason) ? TicketNotAdmittedMessage : row.name));
        CHECK(metrics.Read(row.counter) == 1);
        CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 0);
        for (auto const& other: TicketRefusalTable)
            if (other.reason != row.reason)
                CHECK(metrics.Read(other.counter) == 0);
    }
}

TEST_CASE("A stranger cannot tell forged, unknown-machine and revoked apart, and each still moves its own counter",
          "[distributed][ticket][metrics]")
{
    // AUTH is answered to any address. A stranger signing a ticket for pc-07 with its OWN key would
    // learn from `forged` that pc-07 is admitted here, and from `unknown-machine` that `nobody` is
    // not; anyone holding a captured ticket of a forgotten machine would learn from `revoked` that
    // it was forgotten. Each is told the same bytes, through the production verification.
    struct Probe
    {
        char const* what;
        TicketSpec spec;
        TicketRefusal expected;
    };
    auto const probes = std::to_array<Probe>({
        { .what = "an admitted id, signed by the prober's own key",
          .spec = { .claimed = "pc-07", .signer = "prober" },
          .expected = TicketRefusal::Forged },
        { .what = "an id nobody admitted, signed by the prober's own key",
          .spec = { .claimed = "nobody", .signer = "prober" },
          .expected = TicketRefusal::UnknownMachine },
        { .what = "a forgotten machine's captured ticket",
          .spec = { .claimed = "gone", .signer = "gone" },
          .expected = TicketRefusal::Revoked },
    });

    Verifying fix;
    fix.roster.AdmitMachine("gone");
    fix.roster.Revoke("gone");
    AtomicMetricsSink metrics;
    auto replies = std::vector<std::vector<std::byte>> {};
    for (auto const& probe: probes)
    {
        INFO(probe.what);
        auto verified = fix.verifier.Verify(Ticket(probe.spec), Noon);
        CHECK(RefusalOf(verified) == std::optional { probe.expected }); // the operator's reading
        replies.push_back(AnswerTicket(metrics, std::move(verified)).refusalReply);
        CHECK(metrics.Read(DescribeTicketRefusal(probe.expected).counter) == 1);
    }
    REQUIRE(replies.size() == probes.size());
    CHECK(replies[0] == replies[1]);
    CHECK(replies[1] == replies[2]);
    CHECK(TicketRefusedMessageOf(replies[0]) == TicketNotAdmittedMessage);

    // The control: a reason the caller can check for itself still names itself, so the three are
    // folded rather than every refusal being silenced.
    auto const late =
        AnswerTicket(metrics, fix.verifier.Verify(Ticket({ .expires = Noon - LeaseTokenClockSkewSlack - 1s }), Noon));
    CHECK(TicketRefusedMessageOf(late.refusalReply) == "expired");
}

TEST_CASE("An accepted ticket is counted exactly once and a refused one never", "[distributed][ticket][metrics]")
{
    // ONE answer counts both outcomes, so neither half can be forgotten beside the verification.
    auto const machine = ProvenIdentity { .id = "pc-07", .key = Testing::TestKeyPair("pc-07").PublicKey() };

    SECTION("accepted: the machine, the accepted counter once, and no refusal row")
    {
        AtomicMetricsSink metrics;
        auto const answer = AnswerTicket(metrics, machine);
        CHECK(answer.machine == std::optional { machine });
        CHECK(answer.refusalReply.empty());
        CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);
        for (auto const& row: TicketRefusalTable)
        {
            INFO(row.name);
            CHECK(metrics.Read(row.counter) == 0);
        }
    }

    SECTION("refused: no machine, its own row once, and never the accepted counter")
    {
        for (auto const& row: TicketRefusalTable)
        {
            INFO(row.name);
            AtomicMetricsSink metrics;
            auto const answer = AnswerTicket(metrics, std::unexpected { RejectionFor(row.reason) });
            CHECK_FALSE(answer.machine.has_value());
            // The revoked key's evidence rides out with its refusal, and with no other.
            CHECK(answer.revoked.has_value() == (row.reason == TicketRefusal::Revoked));
            REQUIRE_FALSE(answer.refusalReply.empty());
            CHECK(TicketRefusedMessageOf(answer.refusalReply) == TicketRefusalMessage(row));
            CHECK(metrics.Read(row.counter) == 1);
            CHECK(metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 0);
        }
    }
}

TEST_CASE("One predicate says which audiences name one machine, for the minter and the verifier alike",
          "[distributed][ticket][audience]")
{
    // The minter refuses to sign what the verifier refuses to spend, so both ask this -- parse
    // included -- rather than each composing its own.
    for (auto const* one: { "office.corp:6674", "10.0.0.5:6674", "[2001:db8::5]:6674" })
    {
        INFO(one);
        CHECK(AudienceNamesOneMachine(one));
    }
    for (auto const* every: { "127.0.0.1:6674",
                              "[::1]:6674",
                              "localhost:6674",
                              "LOCALHOST:6674",
                              "0.0.0.0:6674",
                              "[::]:6674",
                              "[::ffff:127.0.0.1]:6674",
                              "6674",
                              "office.corp",
                              "" })
    {
        INFO(every);
        CHECK_FALSE(AudienceNamesOneMachine(every));
    }
}

TEST_CASE("The claims another machine reads must be text, by one check at both ends", "[distributed][ticket]")
{
    auto claims =
        MachineTicketClaims { .machineId = "pc-07", .audience = "office.corp:6674", .expiresAtUnixSeconds = 1, .nonce = {} };
    CHECK_FALSE(FirstClaimNotText(claims).has_value());
    claims.audience = "office.corp\xff:6674";
    CHECK(FirstClaimNotText(claims) == std::optional<std::string_view> { "audience" });
    claims.machineId = "pc\xfe";
    CHECK(FirstClaimNotText(claims) == std::optional<std::string_view> { "machine id" });
}
