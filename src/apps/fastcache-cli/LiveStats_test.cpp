// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"
#include "LiveStats.hpp"
#include "ScriptedExchange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace FastCache::Cli::Testing;
using namespace std::chrono_literals;

namespace
{

/// A cache daemon, as the gatherer describes one.
/// @return The identification.
[[nodiscard]] EndpointIdentity Daemon()
{
    return EndpointIdentity { .kind = RemoteKind::FastcacheWireOnly,
                              .detail = "10.0.0.4:6674 speaks 0xFC and serves no node verbs, so it is not a compile node" };
}

/// A compile node, as the gatherer describes one.
/// @return The identification.
[[nodiscard]] EndpointIdentity CompileNode()
{
    return EndpointIdentity { .kind = RemoteKind::CompileNode, .detail = "10.0.0.4:6674 is a fastcache-compile-node" };
}

/// An endpoint nobody could ask.
/// @return The identification.
[[nodiscard]] EndpointIdentity NotAsked()
{
    return EndpointIdentity { .kind = std::nullopt, .detail = "no 0xFC connection was opened" };
}

/// A port that framed nothing.
/// @return The identification.
[[nodiscard]] EndpointIdentity Foreign()
{
    return EndpointIdentity { .kind = RemoteKind::NotFastcacheWire,
                              .detail = "10.0.0.4:6674 is not a compile node (the server closed the connection)" };
}

/// What one admission concluded, and how often it asked the endpoint.
struct Admission
{
    std::expected<LivePlan, Answer> result;
    int asks;
};

/// Admit a `live-stats` invocation against a scripted identity.
/// @param operands The positional arguments after the verb.
/// @param options The modifiers.
/// @param identity What the endpoint turns out to be.
/// @param resp The RESP connection, or null when none opened.
/// @return The conclusion and the ask count.
[[nodiscard]] Admission Admit(std::vector<std::string> const& operands,
                              VerbOptions options,
                              EndpointIdentity identity,
                              IExchange* resp = nullptr)
{
    ScriptedIdentity scripted { std::move(identity) };
    auto const context = VerbContext { .operands = operands, .options = options, .resp = resp, .identity = &scripted };
    auto result = AdmitLiveStats(context);
    return Admission { .result = std::move(result), .asks = scripted.Calls() };
}

/// Every advisory of a refusal, as one string a case can search.
/// @param admission A refused admission.
/// @return The advisories joined; empty when the admission was not refused.
[[nodiscard]] std::string RefusalText(Admission const& admission)
{
    return admission.result.has_value() ? std::string {} : AdvisoryText(admission.result.error());
}

/// The options with only an interval set.
/// @param interval The interval.
/// @return The options.
[[nodiscard]] VerbOptions WithInterval(std::chrono::milliseconds interval)
{
    return VerbOptions { .interval = interval };
}

} // namespace

TEST_CASE("an unnamed subject is inferred from what the endpoint is and nobody-could-ask is refused", "[cli][live]")
{
    // §9.7: THREE inference cases, because two collapse under the bug. The one that
    // collapses is the third -- defaulting an endpoint nobody could ask to `cache`, which
    // reports *INFO did not answer* against a port that may speak no RESP.
    auto const daemon = Admit({}, {}, Daemon());
    REQUIRE(daemon.result.has_value());
    CHECK(daemon.result->subject == LiveSubject::Cache);

    auto const node = Admit({}, {}, CompileNode());
    REQUIRE(node.result.has_value());
    CHECK(node.result->subject == LiveSubject::Node);

    auto const notAsked = Admit({}, {}, NotAsked());
    REQUIRE_FALSE(notAsked.result.has_value());
    CHECK(notAsked.result.error().outcome == Outcome::Unreachable);
    CHECK(RefusalText(notAsked).contains("no 0xFC connection was opened"));
    // The remedy must not be *name the subject*: a named subject meets this same refusal,
    // so that advice would send the operator straight back here.
    CHECK_FALSE(RefusalText(notAsked).contains("name one of"));

    // The fourth row §1.2's table lacks: a port that framed nothing recognisable. Refused
    // as a protocol it does not speak, and never inferred as anything.
    auto const foreign = Admit({}, {}, Foreign());
    REQUIRE_FALSE(foreign.result.has_value());
    CHECK(foreign.result.error().outcome == Outcome::Protocol);
    CHECK(RefusalText(foreign).contains("the server closed the connection"));
}

TEST_CASE("an endpoint that opened RESP and identified nothing over 0xFC is refused saying what was observed", "[cli][live]")
{
    // The address evidently reaches something, so the remedy *check that --addr reaches a
    // fastcached* would send an operator to verify an address that works. Refused all the
    // same (§9.7) -- only the sentence changes, and it changes to what was OBSERVED.
    //
    // Each observed case beside its neighbour with no RESP, which must keep its own
    // wording: a sentence that said *RESP opened* whatever happened would pass the first
    // half of every pair.
    auto resp = ScriptedExchange { {} };

    // 0xFC could not even be opened.
    auto const unopened = Admit({}, {}, NotAsked(), &resp);
    REQUIRE_FALSE(unopened.result.has_value());
    CHECK(unopened.result.error().outcome == Outcome::Unreachable);
    CHECK(RefusalText(unopened).contains("a RESP connection opened"));
    CHECK(RefusalText(unopened).contains("no 0xFC connection was opened"));
    CHECK(RefusalText(unopened).contains("not a fastcached, or one too old to identify itself"));
    CHECK_FALSE(RefusalText(unopened).contains("check that --addr"));

    auto const unreachable = Admit({}, {}, NotAsked());
    REQUIRE_FALSE(unreachable.result.has_value());
    CHECK(RefusalText(unreachable).contains("check that --addr"));
    CHECK_FALSE(RefusalText(unreachable).contains("RESP"));

    // 0xFC opened and answered in nothing this client reads.
    auto const silent = Admit({}, {}, Foreign(), &resp);
    REQUIRE_FALSE(silent.result.has_value());
    CHECK(silent.result.error().outcome == Outcome::Protocol);
    CHECK(RefusalText(silent).contains("a RESP connection opened, but 0xFC did not answer"));
    CHECK(RefusalText(silent).contains("the server closed the connection"));
    CHECK(RefusalText(silent).contains("not a fastcached, or one too old to identify itself"));

    auto const foreign = Admit({}, {}, Foreign());
    REQUIRE_FALSE(foreign.result.has_value());
    CHECK_FALSE(RefusalText(foreign).contains("RESP"));

    // The two observed cases are worded apart, and admission asked RESP nothing.
    CHECK(RefusalText(unopened) != RefusalText(silent));
    CHECK(resp.Sent().empty());
}

TEST_CASE("a named subject nobody could check is refused like an unnamed one", "[cli][live]")
{
    // An assertion about the endpoint that cannot be checked is not one the view may act
    // on -- and this is the case that shows the not-asked remedy is not *name a subject*.
    auto const named = Admit({ "cache" }, {}, NotAsked());
    REQUIRE_FALSE(named.result.has_value());
    CHECK(named.result.error().outcome == Outcome::Unreachable);
}

TEST_CASE("a named subject the endpoint cannot serve is refused naming what the endpoint is", "[cli][live]")
{
    // §9.8. WHAT DISTINGUISHES is the identification in the text: a bare *refused* passes
    // an outcome assertion and leaves the operator unable to tell whether the address or
    // the subject is wrong.
    struct Mismatch
    {
        std::string_view subject;
        EndpointIdentity identity;
        std::string_view whatItIs;
    };
    auto const mismatches = std::to_array<Mismatch>({
        { .subject = "cache", .identity = CompileNode(), .whatItIs = "is a fastcache-compile-node" },
        { .subject = "node", .identity = Daemon(), .whatItIs = "serves no node verbs" },
        { .subject = "fleet", .identity = Daemon(), .whatItIs = "serves no node verbs" },
    });

    for (auto const& mismatch: mismatches)
    {
        CAPTURE(mismatch.subject);
        auto const refused = Admit({ std::string { mismatch.subject } }, {}, mismatch.identity);
        REQUIRE_FALSE(refused.result.has_value());
        CHECK(refused.result.error().outcome == Outcome::Refused);
        CHECK(ExitCodeOf(refused.result.error().outcome) == 4);
        CHECK(RefusalText(refused).contains(mismatch.whatItIs));
        CHECK(RefusalText(refused).contains(mismatch.subject));
    }

    // The control: each subject against the endpoint that DOES serve it is admitted.
    // Without it, a refusal of every named subject passes the loop above.
    CHECK(Admit({ "cache" }, {}, Daemon()).result.has_value());
    CHECK(Admit({ "node" }, {}, CompileNode()).result.has_value());
    CHECK(Admit({ "fleet" }, {}, CompileNode()).result.has_value());
}

TEST_CASE("fleet is never inferred even from a node that could lead", "[cli][live]")
{
    // §9.9. A case checking only that `fleet` WORKS when named passes under an
    // implementation that infers it -- so the unnamed case against a node is the one that
    // distinguishes, and it must come back as the node.
    auto const unnamed = Admit({}, {}, CompileNode());
    REQUIRE(unnamed.result.has_value());
    CHECK(unnamed.result->subject == LiveSubject::Node);

    auto const named = Admit({ "fleet" }, {}, CompileNode());
    REQUIRE(named.result.has_value());
    CHECK(named.result->subject == LiveSubject::Fleet);
}

TEST_CASE("each subject's floor refuses one millisecond below it and accepts it exactly", "[cli][live]")
{
    // §9.15, per subject and derived over the table. A case using only `cache`'s floor
    // proves nothing about the table -- so the loop takes every row, and the first check
    // makes sure the rows actually differ, or a single shared floor would pass it.
    REQUIRE(LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].minInterval
            != LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Cache)].minInterval);

    for (auto const& row: LiveSubjectTable)
    {
        CAPTURE(row.key);
        auto const identity = EndpointIdentity { .kind = row.servedBy, .detail = "scripted" };
        auto const operands = std::vector<std::string> { std::string { row.key } };

        auto const below = Admit(operands, WithInterval(row.minInterval - 1ms), identity);
        REQUIRE_FALSE(below.result.has_value());
        CHECK(below.result.error().outcome == Outcome::Usage);
        CHECK(ExitCodeOf(below.result.error().outcome) == 2);
        CHECK(RefusalText(below).contains(std::format("{}ms", row.minInterval.count())));
        CHECK(RefusalText(below).contains(row.key));

        auto const at = Admit(operands, WithInterval(row.minInterval), identity);
        REQUIRE(at.result.has_value());
        CHECK(at.result->interval == row.minInterval);
    }
}

TEST_CASE("an inferred subject's floor applies once the subject is known", "[cli][live]")
{
    // The one floor refusal that costs a round trip: with no operand, which floor applies
    // is unknown until the endpoint is identified.
    auto const nodeFloor = LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Node)].minInterval;

    auto const below = Admit({}, WithInterval(nodeFloor - 1ms), CompileNode());
    REQUIRE_FALSE(below.result.has_value());
    CHECK(below.result.error().outcome == Outcome::Usage);
    CHECK(RefusalText(below).contains("`node`"));
    CHECK(below.asks == 1);
}

TEST_CASE("a refusal that needs nothing from the endpoint is decided without asking it", "[cli][live]")
{
    // A typo must cost no round trip. The ask COUNT is what distinguishes: every refusal
    // here would be reached, with the same text, by an implementation that identified
    // the endpoint first.
    auto const unknown = Admit({ "frob" }, {}, CompileNode());
    REQUIRE_FALSE(unknown.result.has_value());
    CHECK(unknown.result.error().outcome == Outcome::Usage);
    CHECK(RefusalText(unknown).contains("frob"));
    CHECK(RefusalText(unknown).contains("cache, node, fleet"));
    CHECK(unknown.asks == 0);

    auto const fleetFloor = LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].minInterval;
    auto const belowNamedFloor = Admit({ "fleet" }, WithInterval(fleetFloor - 1ms), CompileNode());
    REQUIRE_FALSE(belowNamedFloor.result.has_value());
    CHECK(belowNamedFloor.asks == 0);

    // The control: a named subject that passes its floor DOES ask, to check the assertion.
    CHECK(Admit({ "fleet" }, WithInterval(fleetFloor), CompileNode()).asks == 1);
}

TEST_CASE("an unset interval takes the subject's own default and an unset bound is none", "[cli][live]")
{
    auto const fleet = Admit({ "fleet" }, {}, CompileNode());
    REQUIRE(fleet.result.has_value());
    CHECK(fleet.result->interval == LiveSubjectTable[static_cast<std::size_t>(LiveSubject::Fleet)].defaultInterval);
    CHECK(fleet.result->samples == 0);

    auto const bounded = Admit({ "node" }, VerbOptions { .samples = 3 }, CompileNode());
    REQUIRE(bounded.result.has_value());
    CHECK(bounded.result->samples == 3);
}

TEST_CASE("the operand list names every subject the table holds", "[cli][live]")
{
    // Derived at compile time, so this is less a guard than a statement of what was
    // derived: each key present, joined once each.
    auto const* const verb = FindVerb("live-stats");
    REQUIRE(verb != nullptr);
    for (auto const& row: LiveSubjectTable)
        CHECK(verb->operands.contains(row.key));
    CHECK(std::ranges::count(verb->operands, '|') == static_cast<std::ptrdiff_t>(LiveSubjectTable.size()) - 1);
}

TEST_CASE("live-stats through the verb table answers its admission", "[cli][live]")
{
    // The synchronous door onto the same admission: a refusal comes back as one, and an
    // admitted plan as a record of what would be watched.
    auto const* const verb = FindVerb("live-stats");
    REQUIRE(verb != nullptr);

    ScriptedGatherer gatherer { {} };
    ScriptedIdentity node { CompileNode() };
    auto const admitted = RunVerb(*verb, VerbContext { .stats = &gatherer, .identity = &node });
    CHECK(admitted.outcome == Outcome::Affirmative);
    REQUIRE(admitted.value.shape == Shape::Record);
    auto const* const subject = FindField(admitted.value, "subject");
    REQUIRE(subject != nullptr);
    CHECK(subject->value.lexical == "node");

    ScriptedIdentity nobody { NotAsked() };
    CHECK(RunVerb(*verb, VerbContext { .stats = &gatherer, .identity = &nobody }).outcome == Outcome::Unreachable);
}
