// SPDX-License-Identifier: Apache-2.0
#include "FleetTextResponder.hpp"
#include "Responders.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The member every request below arrives from, unless a case says otherwise.
constexpr std::string_view Reader = "10.0.0.7";

/// Where the leader a follower names answers.
constexpr std::string_view LeaderEndpoint = "10.0.0.2:6674";

/// Admits exactly the peers it was given.
class ListedMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param members Who may ask.
    explicit ListedMembership(std::vector<std::string> members) noexcept:
        _members { std::move(members) }
    {
    }

    /// @copydoc Distributed::IMembershipOracle::Explain
    ///
    /// Names `FleetMemberList`, because that is the route this fake stands for: it models
    /// `--fleet-member`'s host list. Through `DecidedBy`, so a miss stays unattributed
    /// rather than claiming the list refused a host it never mentioned (#1471).
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view peerAddress) const override
    {
        return Distributed::DecidedBy(std::ranges::find(_members, peerAddress) != _members.end()
                                          ? Distributed::Membership::Member
                                          : Distributed::Membership::Outsider,
                                      Distributed::MembershipParticipant::FleetMemberList);
    }

  private:
    std::vector<std::string> _members;
};

/// Sources whose leadership and document a case scripts, and which record what was asked.
class ScriptedFleet final: public ILiveStatsSources
{
  public:
    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject /*subject*/) const override
    {
        return std::nullopt;
    }

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return leadership;
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "n1.test:6674";
    }

    /// @copydoc ILiveStatsSources::FleetText
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view section,
                                                                                std::string_view range) const override
    {
        ++reads;
        askedSection = section;
        askedRange = range;
        return answer;
    }

    /// Who leads; a leader by default, so a case states only what it changes.
    std::optional<LiveLeadership> leadership { LiveLeadership { .leads = true, .leaderEndpoint = {} } };

    /// What a read answers; a leader's document by default.
    std::expected<FleetTextDocument, FleetTextDeclined> answer { FleetTextDocument {
        .leads = true, .leaderEndpoint = {}, .body = "# kpi\nkey\tvalue\n" } };

    mutable std::size_t reads { 0 };     ///< How many reads reached the sources.
    mutable std::string askedSection {}; ///< The section the last read named.
    mutable std::string askedRange {};   ///< The range the last read named.
};

/// A FLEET-TEXT frame.
[[nodiscard]] std::vector<std::byte> FleetTextFrame(std::string section = {}, std::string range = {}, std::string token = {})
{
    return Wire::EncodeFleetTextRequest(Wire::FleetTextRequest {
        .section = std::move(section), .range = std::move(range), .dashboardToken = std::move(token) });
}

/// What a refusal said, beside its code.
[[nodiscard]] std::string DetailOf(std::span<std::byte const> reply)
{
    auto const refusal = Wire::DecodeErrorPayload(Testing::PayloadOf(reply));
    REQUIRE(refusal.has_value());
    return std::string { Unwrap(refusal).second };
}

/// @return Whether no counter in the table has moved.
[[nodiscard]] bool NothingCounted(IMetricsSink const& metrics)
{
    return std::ranges::all_of(CounterTable, [&metrics](auto const& row) { return metrics.Read(row.counter) == 0; });
}

/// Everything a case needs: a leader's sources, members, and a responder with no token file.
struct Rig
{
    AtomicMetricsSink metrics;
    ScriptedFleet sources;
    ListedMembership membership { { std::string { Reader }, "127.0.0.1" } };
    FleetTextResponder responder { sources, membership, AdminCredential {}, metrics };

    /// Ask @p on, from @p peer.
    [[nodiscard]] std::vector<std::byte> Ask(FleetTextResponder& on,
                                             std::vector<std::byte> const& frame,
                                             std::string_view peer)
    {
        return SyncRun(on.Answer(frame, std::string { peer })).bytes;
    }

    /// Ask the rig's responder from this machine, which a node with no token file admits.
    [[nodiscard]] std::vector<std::byte> AskLocally(std::vector<std::byte> const& frame)
    {
        return Ask(responder, frame, "127.0.0.1");
    }
};

} // namespace

TEST_CASE("A fleet read is answered with the document the sources rendered for the words asked", "[node][fleettext]")
{
    Rig rig;
    auto const reply = rig.AskLocally(FleetTextFrame("series", "7d"));

    REQUIRE(Testing::StatusOf(reply) == Wire::Status::Ok);
    CHECK(Wire::AsStringView(Testing::PayloadOf(reply)) == "# kpi\nkey\tvalue\n");

    // The words reach the sources as typed: the component parses no key of its own, so a section
    // it could refuse here would be a second table of what exists.
    CHECK(rig.sources.askedSection == "series");
    CHECK(rig.sources.askedRange == "7d");
    CHECK(NothingCounted(rig.metrics));
}

TEST_CASE("A stranger is refused a fleet read by name and counted once whichever door it used", "[node][fleettext]")
{
    Rig rig;
    auto const reply = rig.Ask(rig.responder, FleetTextFrame(), "192.0.2.1");
    CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotAMember);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember) == 1);
    CHECK(rig.sources.reads == 0);

    // `RefusePeer` is the one implementation; the door asking it counts once more, not twice.
    CHECK(rig.responder.RefusePeer("192.0.2.1", static_cast<std::uint8_t>(Wire::Op::FleetText)).has_value());
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedNotAMember) == 2);
    CHECK_FALSE(rig.responder.RefusePeer(Reader, static_cast<std::uint8_t>(Wire::Op::FleetText)).has_value());
}

TEST_CASE("The dashboard credential decides a fleet read and is counted against the fleet-text series", "[node][fleettext]")
{
    Rig rig;

    SECTION("with no token file a remote member is refused and this machine is not")
    {
        auto const remote = rig.Ask(rig.responder, FleetTextFrame(), Reader);
        CHECK(Testing::ErrorOf(remote) == Wire::ErrorCode::Unauthenticated);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedUnauthenticated) == 1);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated) == 0);
        CHECK(rig.sources.reads == 0);

        CHECK(Testing::StatusOf(rig.AskLocally(FleetTextFrame())) == Wire::Status::Ok);
    }

    SECTION("with a token file the token decides from anywhere")
    {
        FleetTextResponder guarded { rig.sources, rig.membership, AdminCredential { "s3cret" }, rig.metrics };
        CHECK(Testing::ErrorOf(rig.Ask(guarded, FleetTextFrame({}, {}, "guess"), "127.0.0.1"))
              == Wire::ErrorCode::Unauthenticated);
        CHECK(Testing::StatusOf(rig.Ask(guarded, FleetTextFrame({}, {}, "s3cret"), Reader)) == Wire::Status::Ok);
    }

    SECTION("and a follower asked without it names no leader")
    {
        rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint } };
        auto const reply = rig.Ask(rig.responder, FleetTextFrame(), Reader);
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::Unauthenticated);
        CHECK_FALSE(DetailOf(reply).contains(LeaderEndpoint));
    }
}

TEST_CASE("A follower redirects a fleet read to the leader and a worker says the fleet is elsewhere", "[node][fleettext]")
{
    Rig rig;

    SECTION("a follower: NotLeader whose words are the leader's endpoint, uncounted")
    {
        rig.sources.leadership = LiveLeadership { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint } };
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotLeader);
        CHECK(DetailOf(reply) == LeaderEndpoint);
        CHECK(rig.sources.reads == 0);
        CHECK(NothingCounted(rig.metrics));
    }

    SECTION("a node with no scheduler: served elsewhere, which is not unimplemented, uncounted")
    {
        rig.sources.leadership = std::nullopt;
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::DispatchNotPermitted);
        CHECK(rig.sources.reads == 0);
        CHECK(NothingCounted(rig.metrics));
    }

    SECTION("a document rendered after the leader stepped down is judged by that document")
    {
        // The gate read `leads` a moment before the render; the snapshot the body came from is the
        // later reading, and a follower's registry is a fraction presented as the whole.
        rig.sources.answer =
            FleetTextDocument { .leads = false, .leaderEndpoint = std::string { LeaderEndpoint }, .body = "# kpi\n" };
        auto const reply = rig.AskLocally(FleetTextFrame("kpi"));
        CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::NotLeader);
        CHECK(DetailOf(reply) == LeaderEndpoint);
        CHECK(NothingCounted(rig.metrics));
    }
}

TEST_CASE("A section or a range this build does not serve is refused by its own code and not counted", "[node][fleettext]")
{
    Rig rig;
    rig.sources.answer = std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::UnknownSelector,
                                                             .detail = "unknown range; this build serves: 1h, 2h\n" });

    auto const reply = rig.AskLocally(FleetTextFrame("kpi", "1fortnight"));
    CHECK(Testing::ErrorOf(reply) == Wire::ErrorCode::UnknownFleetSelector);
    CHECK(DetailOf(reply) == "unknown range; this build serves: 1h, 2h\n");
    CHECK(NothingCounted(rig.metrics));

    SECTION("while sources detached mid-read say the fleet is not served here")
    {
        rig.sources.answer =
            std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = "stopping" });
        CHECK(Testing::ErrorOf(rig.AskLocally(FleetTextFrame())) == Wire::ErrorCode::DispatchNotPermitted);
        CHECK(NothingCounted(rig.metrics));
    }
}

TEST_CASE("A malformed fleet read and a frame-ceiling probe are counted and an unknown opcode is not", "[node][fleettext]")
{
    Rig rig;

    auto const fields = WireFields::Encode({ Wire::AsBytes("kpi"), Wire::AsBytes("1h") });
    std::vector<std::byte> frame(Wire::RequestHeaderSize);
    WireFrame::PutHeader(frame,
                         Wire::Magic,
                         Wire::CurrentVersion,
                         static_cast<std::uint8_t>(Wire::Op::FleetText),
                         static_cast<std::uint32_t>(fields.size()));
    frame.insert(frame.end(), fields.begin(), fields.end());

    CHECK(Testing::ErrorOf(rig.AskLocally(frame)) == Wire::ErrorCode::MalformedFrame);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedMalformed) == 1);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed) == 0);
    CHECK(rig.sources.reads == 0);

    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::FleetText);
    CHECK(Testing::ErrorOf(rig.responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, opRaw, "512 MiB"))
          == Wire::ErrorCode::PayloadTooLarge);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge) == 1);

    CHECK(Testing::ErrorOf(rig.responder.RefusalReply(Wire::PrePayloadDecision::UnknownOpcode, opRaw, {}))
          == Wire::ErrorCode::UnknownOpcode);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedPayloadTooLarge) == 1);

    SECTION("and a busy surface says so against the fleet-text series, not the operator verbs'")
    {
        CHECK(Testing::ErrorOf(rig.responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, opRaw, {}))
              == Wire::ErrorCode::EndpointBusy);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::FleetTextRequestsRefusedEndpointBusy) == 1);
        CHECK(rig.metrics.Read(IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy) == 0);
    }
}

TEST_CASE("The fleet-text surface requires no credential of its own so a plain worker can answer", "[node][fleettext]")
{
    Rig rig;
    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::FleetText);
    CHECK_FALSE(rig.responder.AuthRequired(opRaw));
    CHECK(Wire::DecidePrePayload({ .opRaw = opRaw,
                                   .declaredLength = 0,
                                   .sessionCap = Wire::MaxControlPayload,
                                   .authRequired = rig.responder.AuthRequired(opRaw),
                                   .credentialAccepted = false })
          == Wire::PrePayloadDecision::Serve);

    // The control: the verb is not pre-auth, so a surface that DOES hold a policy makes it wait.
    CHECK(Wire::DecidePrePayload({ .opRaw = opRaw,
                                   .declaredLength = 0,
                                   .sessionCap = Wire::MaxControlPayload,
                                   .authRequired = true,
                                   .credentialAccepted = false })
          == Wire::PrePayloadDecision::Unauthenticated);
}

TEST_CASE("A fleet read through the slot is the attached sources' and a detached slot has no fleet", "[node][fleettext]")
{
    // The node's responder is built against the slot before the fleet exists, so the slot is the
    // one door the document is read through; a slot answering from nothing would serve every
    // reader "no fleet" on a leader.
    ScriptedFleet sources;
    LiveStatsSourceSlot slot;
    {
        auto const attached = slot.Attach(sources);
        auto const read = slot.FleetText("series", "7d");
        REQUIRE(read.has_value());
        CHECK(read->body == "# kpi\nkey\tvalue\n");
        CHECK(sources.askedSection == "series");
        CHECK(sources.askedRange == "7d");
    }

    auto const detached = slot.FleetText("series", "7d");
    REQUIRE_FALSE(detached.has_value());
    CHECK(detached.error().refusal == FleetTextRefusal::NoFleet);
    CHECK(sources.reads == 1);
}

TEST_CASE("MergedResponder routes the Fleet family to the fleet-text component and nowhere else", "[node][fleettext]")
{
    Rig rig;
    MergedResponder merged { SurfaceComponents { .fleet = &rig.responder } };

    CHECK(merged.OwnerOf(static_cast<std::uint8_t>(Wire::Op::FleetText)) == &rig.responder);
    CHECK(Testing::StatusOf(SyncRun(merged.Answer(FleetTextFrame(), "127.0.0.1")).bytes) == Wire::Status::Ok);

    // Routes rather than catching all: no other verb reaches it.
    for (auto const& row: Wire::OpTable)
        if (row.code != Wire::Op::FleetText)
        {
            INFO("verb " << row.name);
            CHECK(merged.OwnerOf(static_cast<std::uint8_t>(row.code)) != &rig.responder);
        }

    SECTION("and a node with no fleet-text component answers served-nowhere rather than crashing")
    {
        MergedResponder without { SurfaceComponents {} };
        CHECK(Testing::ErrorOf(SyncRun(without.Answer(FleetTextFrame(), "127.0.0.1")).bytes) == Wire::UnimplementedVerb);
    }

    SECTION("and a verb routed to it directly that it does not own is stepped over, uncounted")
    {
        std::vector<std::byte> fetch(Wire::RequestHeaderSize);
        WireFrame::PutHeader(fetch, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::Fetch), 0);
        CHECK(Testing::ErrorOf(rig.AskLocally(fetch)) == Wire::UnimplementedVerb);
        CHECK(NothingCounted(rig.metrics));
    }
}
