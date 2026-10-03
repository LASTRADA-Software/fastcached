// SPDX-License-Identifier: Apache-2.0
#include "SchedulerLink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
constexpr std::string_view Configured = "scheduler.example:6676";
constexpr std::string_view Second = "scheduler-b.example:6676";
constexpr std::string_view Third = "scheduler-c.example:6676";
constexpr std::string_view Leader = "10.0.0.7:6676";
constexpr std::string_view Other = "10.0.0.9:6676";

/// A link configured with @p endpoints, in order.
/// @param endpoints The `--scheduler` values.
/// @return The link, before its first round.
[[nodiscard]] SchedulerLink LinkTo(std::initializer_list<std::string_view> endpoints)
{
    std::vector<std::string> configured;
    for (auto const endpoint: endpoints)
        configured.emplace_back(endpoint);
    auto const link = SchedulerLink::For(std::move(configured));
    REQUIRE(link.has_value());
    return Unwrap(link);
}

/// Where the next round opens, which is the only thing a remembered leader DOES.
///
/// Asserted instead of reading a "following" flag back: a flag can be set correctly
/// while the round still opens in the wrong place, and the opening is the behaviour.
/// @param link The link; a round is begun on it.
/// @return The first target of that round.
[[nodiscard]] std::string NextRoundOpensAt(SchedulerLink& link)
{
    link.BeginRound();
    return link.Target();
}
} // namespace

TEST_CASE("A link is built only from a list that names a scheduler", "[node][schedulerlink]")
{
    // #206 made an empty `--scheduler` list a legal configuration -- a node running no
    // worker names none -- so the link is not a thing that exists for it. The factory
    // answers nothing rather than a link over nothing, whose first round would read the
    // first element of an empty list.
    CHECK_FALSE(SchedulerLink::For({}).has_value());

    // The control, and what a real link opens at.
    auto link = SchedulerLink::For({ std::string { Configured }, std::string { Second } });
    REQUIRE(link.has_value());
    CHECK(Unwrap(link).Target() == Configured);
}

TEST_CASE("A node starts at the first endpoint it was configured with", "[node][schedulerlink]")
{
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();

    CHECK(link.Target() == Configured);
}

TEST_CASE("A NotLeader redirect moves this round's target, never into the configured list", "[node][schedulerlink]")
{
    // The whole of #237's worker half: before this, the refusal was logged and the
    // next dial went back to the demoted scheduler, so a worker announced itself to
    // a node that refuses `Register` and expired out of the real leader's registry.
    //
    // And a redirect is an INSTRUCTION (#1310): with a second endpoint configured, the
    // target is still the one the refusal named, not the next entry of the list.
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();

    REQUIRE(link.Redirect(std::string { Leader }));
    CHECK(link.Target() == Leader);
}

TEST_CASE("The redirect chain is bounded, so two schedulers naming each other cost one round", "[node][schedulerlink]")
{
    // Not this node's chain to trust. A partition healing, or a stale `_knownLeader`
    // on either side, can have two schedulers name each other indefinitely.
    auto link = LinkTo({ Configured });
    link.BeginRound();

    for ([[maybe_unused]] auto const hop: std::views::iota(0, MaxAnnounceRedirects))
        CHECK(link.Redirect(std::string { Leader }));

    // Spent: the caller gives up until the next round rather than looping.
    CHECK_FALSE(link.Redirect(std::string { Other }));
}

TEST_CASE("The budget is per round, not per process", "[node][schedulerlink]")
{
    // A fleet that re-elects once an hour should spend one redirect an hour. A
    // lifetime ceiling would follow redirects for a while and then silently stop,
    // which is the same outage as never following one -- arriving later.
    auto link = LinkTo({ Configured });

    link.BeginRound();
    for ([[maybe_unused]] auto const hop: std::views::iota(0, MaxAnnounceRedirects))
        REQUIRE(link.Redirect(std::string { Leader }));
    REQUIRE_FALSE(link.Redirect(std::string { Other }));

    link.BeginRound();
    CHECK(link.Redirect(std::string { Leader }));
}

TEST_CASE("A leader is remembered only once a round has been accepted there", "[node][schedulerlink]")
{
    // An endpoint some scheduler NAMED is a lead; an endpoint that took this node's
    // registration is a leader. Committing on the name alone would let one bad
    // redirect become the endpoint every future round starts at.
    SECTION("followed, never accepted: the next round opens at the configured endpoint")
    {
        auto link = LinkTo({ Configured });
        link.BeginRound();
        REQUIRE(link.Redirect(std::string { Leader }));

        CHECK(NextRoundOpensAt(link) == Configured);
    }

    SECTION("followed and accepted: the next round opens at the leader")
    {
        // Which is what stops a steady-state fleet paying a redirect on every single
        // heartbeat.
        auto link = LinkTo({ Configured });
        link.BeginRound();
        REQUIRE(link.Redirect(std::string { Leader }));
        link.Accepted();

        CHECK(NextRoundOpensAt(link) == Leader);
    }
}

TEST_CASE("A redirect that is followed and then refused does not become the next round's start", "[node][schedulerlink]")
{
    // The endpoint answered -- it was reachable -- but refused for its own reasons:
    // not a member, a fingerprint it will not take. That is not a leader, and
    // starting there every round would pin this node to it.
    auto link = LinkTo({ Configured });

    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));

    // Nothing further this round: the one configured endpoint already answered it,
    // with the redirect that led here, and would name the same leader again.
    // Redialling it is a spin bounded only by the hop budget, which is what this was
    // before #1310 made each configured endpoint a once-per-round fallback.
    CHECK_FALSE(link.Lost().has_value());

    CHECK(NextRoundOpensAt(link) == Configured);
}

TEST_CASE("A remembered leader that stops answering falls back inside the same round", "[node][schedulerlink]")
{
    // Not a heartbeat interval later. This machine is absent from the fleet for as
    // long as this takes, and the configured endpoint is the one still standing
    // after an election the remembered leader lost.
    auto link = LinkTo({ Configured });
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();

    link.BeginRound();
    REQUIRE(link.Target() == Leader);

    auto const fallback = link.Lost();
    REQUIRE(fallback.has_value());
    CHECK(Unwrap(fallback) == Configured);
    CHECK(link.Target() == Configured);

    // And forgotten, not merely stepped around for one round.
    CHECK(NextRoundOpensAt(link) == Configured);
}

TEST_CASE("Losing the configured endpoint offers nothing further, rather than spinning", "[node][schedulerlink]")
{
    // There is nowhere further back to fall. Returning the same endpoint again
    // would have the caller redial it inside one round forever.
    auto link = LinkTo({ Configured });
    link.BeginRound();

    CHECK_FALSE(link.Lost().has_value());
    CHECK(link.Target() == Configured);
}

TEST_CASE("Being accepted back at the configured endpoint forgets the remembered leader", "[node][schedulerlink]")
{
    // A fleet that re-elects back to the original scheduler must stop opening its
    // rounds anywhere else. Storing the configured endpoint as a remembered leader
    // would open there too, which is why the last line asserts what a FAILURE there
    // does: remembered, the whole configured list would still be untried behind it,
    // and the same endpoint would be offered again inside the round.
    auto link = LinkTo({ Configured });
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();
    REQUIRE(NextRoundOpensAt(link) == Leader);

    REQUIRE(link.Redirect(std::string { Configured }));
    link.Accepted();

    CHECK(NextRoundOpensAt(link) == Configured);
    CHECK_FALSE(link.Lost().has_value());
}

TEST_CASE("An unreachable first scheduler falls back to the second in the same round", "[node][schedulerlink][fallback]")
{
    // The discrimination #1310 asks for: one value always worked, so a list whose
    // first entry answers proves nothing. The first is lost here and the second must
    // be offered in the SAME round, and be the one the round is then dialling.
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();
    REQUIRE(link.Target() == Configured);

    auto const fallback = link.Lost();
    REQUIRE(fallback.has_value());
    CHECK(Unwrap(fallback) == Second);
    CHECK(link.Target() == Second);

    // Nothing after the second: every configured endpoint has been tried this round.
    CHECK_FALSE(link.Lost().has_value());
}

TEST_CASE("A round opens at the configured endpoint that last accepted, and still wraps to the first",
          "[node][schedulerlink][fallback]")
{
    // A retired first entry would otherwise cost its connect timeout on every
    // heartbeat forever. The wrap is the other half: an operator's first choice that
    // comes back is still tried, inside a round that began further down.
    auto link = LinkTo({ Configured, Second, Third });
    link.BeginRound();
    REQUIRE(link.Lost() == std::optional { std::string { Second } });
    link.Accepted();

    link.BeginRound();
    CHECK(link.Target() == Second);
    CHECK(link.Lost() == std::optional { std::string { Third } });
    CHECK(link.Lost() == std::optional { std::string { Configured } });
    CHECK_FALSE(link.Lost().has_value());
}

TEST_CASE("A redirect target that fails falls back to a configured endpoint not yet tried",
          "[node][schedulerlink][fallback]")
{
    // Never to the one that issued the redirect -- it answered a moment ago -- and
    // never past the end of the list.
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));

    CHECK(link.Lost() == std::optional { std::string { Second } });
    CHECK_FALSE(link.Lost().has_value());
}

TEST_CASE("A remembered leader that stops answering walks the whole configured list", "[node][schedulerlink][fallback]")
{
    // The same-round fallback of #237, over a list: the remembered leader is not a
    // configured endpoint, so every configured one is still there to try.
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();

    link.BeginRound();
    REQUIRE(link.Target() == Leader);
    CHECK(link.Lost() == std::optional { std::string { Configured } });
    CHECK(link.Lost() == std::optional { std::string { Second } });
    CHECK_FALSE(link.Lost().has_value());
}

namespace
{
/// Where a node registers, as a case scripts it: what `Current` answers until the case moves it.
class ScriptedSchedulers final: public ISchedulerEndpointSource
{
  public:
    /// @param endpoints What it answers first.
    explicit ScriptedSchedulers(std::vector<std::string> endpoints):
        _endpoints { std::move(endpoints) }
    {
    }

    [[nodiscard]] std::vector<std::string> Current() const override
    {
        ++reads;
        return _endpoints;
    }

    /// Answer @p endpoints from now on.
    /// @param endpoints The new list.
    void Move(std::vector<std::string> endpoints)
    {
        _endpoints = std::move(endpoints);
    }

    mutable int reads { 0 }; ///< How many times the link asked.

  private:
    std::vector<std::string> _endpoints;
};
} // namespace

TEST_CASE("A link over a source re-reads it at every round, and a moved list is walked from the next one",
          "[node][schedulerlink][retarget]")
{
    // T26's carry: a voter that moves its 0xFC endpoint is recorded in the applied state, and the next
    // round -- not the next reform -- dials it. The read is in `BeginRound`, which every round makes.
    ScriptedSchedulers source { { std::string { Configured } } };
    auto built = SchedulerLink::Over(source);
    REQUIRE(built.has_value());
    auto link = Unwrap(std::move(built));
    CHECK(NextRoundOpensAt(link) == Configured);
    link.Accepted();
    auto const readsAfterFirst = source.reads;

    source.Move({ std::string { Second } });
    CHECK(NextRoundOpensAt(link) == Second);
    CHECK(source.reads == readsAfterFirst + 1);
    CHECK(link.Configured() == std::vector<std::string> { std::string { Second } });

    // A source that knows nothing for a moment forgets nothing the link knew.
    source.Move({});
    CHECK(NextRoundOpensAt(link) == Second);
    CHECK_FALSE(SchedulerLink::Over(source).has_value());
}

TEST_CASE("A retargeted walk keeps its place by endpoint, and keeps a remembered leader", "[node][schedulerlink][retarget]")
{
    // The endpoint that last accepted stays where a round opens when it is still listed, wherever it
    // now sits; one that is gone hands the opening to the list's first entry.
    auto link = LinkTo({ Configured, Second });
    link.BeginRound();
    REQUIRE(link.Lost() == std::optional { std::string { Second } });
    link.Accepted();

    link.Retarget({ std::string { Third }, std::string { Second } });
    CHECK(NextRoundOpensAt(link) == Second);
    CHECK(link.Lost() == std::optional { std::string { Third } });

    link.Retarget({ std::string { Third }, std::string { Configured } });
    CHECK(NextRoundOpensAt(link) == Third);

    // A remembered leader is no configured endpoint, and a moved list says nothing about who leads.
    link.BeginRound();
    REQUIRE(link.Redirect(std::string { Leader }));
    link.Accepted();
    link.Retarget({ std::string { Other } });
    CHECK(NextRoundOpensAt(link) == Leader);
    CHECK(link.Lost() == std::optional { std::string { Other } });
}

TEST_CASE("DescribeAnnounceRound: a steady heartbeat round does not claim a registration", "[node][scheduler]")
{
    using namespace FastCache::Node;

    // The defect itself (#999): six heartbeats reported "6 of 6 toolchain(s)
    // registered", every interval, forever. Asserted on the WORDING and not only the
    // level, because that is what was wrong -- a test checking the level alone passes
    // with the sentence still collapsed.
    auto const steady = DescribeAnnounceRound(6, 0, 6);
    CHECK(steady.level == FastCache::LogLevel::Trace);
    CHECK(!steady.message.contains("registered,"));
    CHECK(steady.message.contains("still registered"));
    CHECK(steady.message.contains("6"));
}

TEST_CASE("DescribeAnnounceRound: a real registration is an event and says so", "[node][scheduler]")
{
    using namespace FastCache::Node;

    auto const fresh = DescribeAnnounceRound(0, 6, 6);
    CHECK(fresh.level == FastCache::LogLevel::Info);

    // The ACCEPTED form is a fixture contract: `E2eRegisteredMarker` is
    // "1 of 1 toolchain(s) registered" and distinguishes an accepted worker from one
    // the scheduler turned away (#445). Pinned here as bytes, because rewording it
    // breaks three e2e fixtures by TIMEOUT rather than by a failed assertion.
    CHECK(DescribeAnnounceRound(0, 1, 1).message == "1 of 1 toolchain(s) registered");
    CHECK(fresh.message.contains("6 of 6 toolchain(s) registered"));

    // And it must be distinguishable from the steady round, which is the whole point.
    CHECK(fresh.message != DescribeAnnounceRound(6, 0, 6).message);
    CHECK(fresh.level != DescribeAnnounceRound(6, 0, 6).level);
}

TEST_CASE("DescribeAnnounceRound: a heartbeat that fell through to a register is its own outcome", "[node][scheduler]")
{
    using namespace FastCache::Node;

    // The case worth catching: the scheduler forgot this worker and it re-announced.
    // Fully accepted, so a count-based check calls it healthy -- and it is an event.
    auto const mixed = DescribeAnnounceRound(4, 2, 6);
    CHECK(mixed.level == FastCache::LogLevel::Info);
    CHECK(mixed.message.contains("2 of 6 toolchain(s) re-registered"));

    // Three outcomes, three sentences. None may collide with another.
    auto const steady = DescribeAnnounceRound(6, 0, 6);
    auto const fresh = DescribeAnnounceRound(0, 6, 6);
    CHECK(mixed.message != steady.message);
    CHECK(mixed.message != fresh.message);
}

TEST_CASE("DescribeAnnounceRound: a shortfall is Debug, because each refusal it counts is said on its own",
          "[node][scheduler]")
{
    using namespace FastCache::Node;

    // A shortfall is registrars a scheduler refused or redirected. Each refusal is said by
    // `SchedulerReachability` -- at Warn on its transition, then quietly -- and each redirect as the
    // round follows it, so the per-round count of them is Debug whatever caused it.
    CHECK(DescribeAnnounceRound(0, 0, 1).level == FastCache::LogLevel::Debug);

    // The SHORTFALL form is still a fixture contract as bytes: the e2e helpers' self-test stages
    // "0 of 1 toolchain(s) registered" as the line a turned-away worker writes.
    CHECK(DescribeAnnounceRound(0, 0, 1).message == "0 of 1 toolchain(s) registered");
    CHECK(DescribeAnnounceRound(3, 1, 6).message == "4 of 6 toolchain(s) registered");
}
