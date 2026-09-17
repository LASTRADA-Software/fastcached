// SPDX-License-Identifier: Apache-2.0
//
// What a re-survey owes the scheduler when a toolchain goes away
// ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)) -- and what a
// node owes it when the ADDRESS it is registered under moves
// ([#1279](https://github.com/LASTRADA-Software/fastcached/issues/1279)). Two causes,
// one rule: a registration is retired unless the new set re-registers exactly it, and
// "exactly" is the registry's own key, `(fingerprint, endpoint)`.
//
// `AdoptRegistrars` exists as a free function because clang-tidy refused it as a
// lambda inside `WorkerBody` -- cognitive complexity 66 against a threshold of 60 --
// and the analyser was right for a reason beyond arithmetic: `main.cpp` is in no test
// target (#909), so the rule *replacing the served set retires what left it* could
// only be checked by reading, at two call sites that must not diverge. This file is
// what the extraction bought.
#include "EndpointDialerTestUtils.hpp"
#include "NodeAnnounce.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScriptedSocket.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{
namespace Wire = FastCache::CompileCacheWire;

/// The address a node in these cases advertises.
inline constexpr std::string_view ThisNode = "10.0.0.2:6677";

/// Where it ends up once it learns its external address.
inline constexpr std::string_view MovedNode = "nat.example:7700";

/// A registrar for @p fingerprint, optionally already accepted by a scheduler.
///
/// The id is what a withdrawal NAMES, so whether it is set is the whole of the
/// second clause under test rather than incidental setup.
/// @param notice Where an unchecked credential would be reported.
/// @param fingerprint The toolchain it announces.
/// @param endpoint The address it announces, defaulted because only the endpoint cases
///        vary it -- and it is the other half of the key the adoption rule reads.
/// @return The registrar, never registered.
[[nodiscard]] Cc::WorkerRegistrar Registrar(Cc::CredentialNotice& notice,
                                            std::string fingerprint,
                                            std::string_view endpoint = ThisNode)
{
    return Cc::WorkerRegistrar { notice, std::move(fingerprint), std::string { endpoint },
                                 1U,     Wire::CodecList {},     Wire::CapacityFields {} };
}

/// The fingerprints of a registrar list, in order, so a case can assert WHICH
/// survived rather than how many.
/// @param registrars The list to read.
/// @return One name per entry.
[[nodiscard]] std::vector<std::string> NamesOf(std::vector<Cc::WorkerRegistrar> const& registrars)
{
    std::vector<std::string> names;
    names.reserve(registrars.size());
    for (auto const& registrar: registrars)
        names.push_back(registrar.Fingerprint());
    return names;
}

/// The endpoints of a registrar list, in order, beside `NamesOf` -- because since
/// #1279 a case that read only the fingerprints could not tell a list rebuilt at a new
/// address from one that never moved.
/// @param registrars The list to read.
/// @return One address per entry.
[[nodiscard]] std::vector<std::string> EndpointsOf(std::vector<Cc::WorkerRegistrar> const& registrars)
{
    std::vector<std::string> endpoints;
    endpoints.reserve(registrars.size());
    for (auto const& registrar: registrars)
        endpoints.push_back(registrar.Endpoint());
    return endpoints;
}

/// A framed REGISTER reply accepting a worker under @p workerId.
/// @param workerId The id to assign.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> RegisterOk(std::string_view workerId)
{
    auto const payload =
        Wire::EncodeRegisterReply({ .workerId = std::string { workerId }, .clusterId = "fleet-a", .epoch = 1 });
    return Wire::EncodeReply(Wire::Status::Ok, payload);
}

} // namespace

TEST_CASE("Adopting a served set retires only the registrations that left it", "[node][announce]")
{
    // The registrars a re-survey drops carry the scheduler-issued `WorkerId` a
    // withdrawal names, and rebuilding the list destroys them. They are moved aside
    // instead.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));
    current.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "gcc-14"));
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    // Nothing is retired, because neither registrar was ever accepted -- an empty
    // `WorkerId()` means there is no id to name it with and nothing on the other end.
    // This is the clause that makes the case below say something: without it, a
    // version that retired every departing registrar would pass on the names alone.
    CHECK(withdrawals.empty());

    // Replaced rather than merged, which is the same rule `ReplaceToolchains` follows
    // one line above it in production.
    CHECK(NamesOf(current) == std::vector<std::string> { "gcc-14", "clang-20" });
}

TEST_CASE("Adopting a served set retires a registration the scheduler accepted", "[node][announce]")
{
    // The positive direction, and the one the three cases around it cannot reach: they
    // all assert `withdrawals.empty()`, so on their own they pass equally well against
    // a function that retires NOTHING -- which is the version this whole change exists
    // to replace. A registrar only carries the `WorkerId` a withdrawal names once a
    // scheduler has accepted it, so the case has to register one for real.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));
    current.push_back(Registrar(notice, "clang-20"));

    Testing::ScriptedSocket scheduler { Testing::Replies({
        RegisterOk("w-gcc"),
        RegisterOk("w-clang"),
    }) };
    REQUIRE(current[0].Register(scheduler).has_value());
    REQUIRE(current[1].Register(scheduler).has_value());
    REQUIRE(current[0].WorkerId() == "w-gcc");

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    // Exactly the one that left, carrying the id the scheduler issued -- which is the
    // only thing `Op::Withdraw` can name, and the thing rebuilding the list destroys.
    REQUIRE(withdrawals.size() == 1);
    CHECK(withdrawals.front().Fingerprint() == "gcc-13");
    CHECK(withdrawals.front().WorkerId() == "w-gcc");

    // And the sibling is untouched: still registered, still holding its own id.
    CHECK(NamesOf(current) == std::vector<std::string> { "clang-20" });
}

TEST_CASE("Adopting a served set leaves the toolchains it still carries alone", "[node][announce]")
{
    // The control, and the direction that is expensive to get wrong: a node serving
    // several toolchains re-surveys routinely, and retiring a registration it still
    // serves would take that machine out of the fleet for a fingerprint it can
    // honour -- silently, since the entry simply stops being heartbeated.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    CHECK(withdrawals.empty());
    CHECK(NamesOf(current) == std::vector<std::string> { "clang-20" });
}

TEST_CASE("Adopting an empty served set retires nothing that was never registered", "[node][announce]")
{
    // A machine that loses every toolchain keeps running and keeps saying nothing,
    // rather than exiting -- the compiler may come back with the next package. What
    // it must not do is invent withdrawals for entries no scheduler ever accepted.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars({}, current, withdrawals);

    CHECK(current.empty());
    CHECK(withdrawals.empty());
}

TEST_CASE("A node that moves the address it advertises retires every registration under the old one",
          "[node][announce][advertise]")
{
    // **#1279's second clause, which is the one without which the ticket closes on a
    // change that fixed nothing.** Late-binding the read gets the new address into the
    // registrars; it is this that gets the OLD entry out of the scheduler. Left behind,
    // that entry goes on being dispatched to and its grants go on verifying -- a lease
    // token's MAC covers the endpoint, so what the fleet hands out is a valid credential
    // for an address nobody answers, and every counter reads normal.
    //
    // Two toolchains, because a node serving several is the production shape and the
    // rule is per ENTRY: a version that retired the first and kept the rest would leave
    // the machine half-registered at an address it has left.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));
    current.push_back(Registrar(notice, "clang-20"));

    Testing::ScriptedSocket scheduler { Testing::Replies({
        RegisterOk("w-gcc"),
        RegisterOk("w-clang"),
    }) };
    REQUIRE(current[0].Register(scheduler).has_value());
    REQUIRE(current[1].Register(scheduler).has_value());

    // The same fingerprints -- nothing about what this machine SERVES has changed, which
    // is exactly why the fingerprint alone could not answer this.
    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "gcc-13", MovedNode));
    rebuilt.push_back(Registrar(notice, "clang-20", MovedNode));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    // Both old entries, carrying the ids the scheduler issued -- the only thing
    // `Op::Withdraw` can name, and what rebuilding the list would otherwise destroy.
    REQUIRE(withdrawals.size() == 2);
    CHECK(NamesOf(withdrawals) == std::vector<std::string> { "gcc-13", "clang-20" });
    CHECK(EndpointsOf(withdrawals) == std::vector<std::string> { std::string { ThisNode }, std::string { ThisNode } });
    CHECK(withdrawals[0].WorkerId() == "w-gcc");
    CHECK(withdrawals[1].WorkerId() == "w-clang");

    // And what is in force is the same toolchains at the new address, unregistered --
    // so the round that follows registers them rather than heartbeating entries the
    // scheduler has never been told about.
    CHECK(NamesOf(current) == std::vector<std::string> { "gcc-13", "clang-20" });
    CHECK(EndpointsOf(current) == std::vector<std::string> { std::string { MovedNode }, std::string { MovedNode } });
    CHECK(current[0].WorkerId().empty());
}

TEST_CASE("A node re-adopting the same set at the same address retires nothing", "[node][announce][advertise]")
{
    // **The control for the case above, and it is not the one three cases up.** Those
    // assert `withdrawals.empty()` over registrars no scheduler ever accepted, so they
    // pass equally well against a version that retires everything it can. This one has
    // an ACCEPTED registration and nothing moved, which is the state a node is in on
    // every heartbeat of its life: re-adopting must be free.
    //
    // Without it, keying the rule on the pair reads as proven by a case that would also
    // pass if the pair comparison were replaced by `false`.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));

    Testing::ScriptedSocket scheduler { Testing::Replies({ RegisterOk("w-gcc") }) };
    REQUIRE(current[0].Register(scheduler).has_value());

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "gcc-13"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    CHECK(withdrawals.empty());
    CHECK(NamesOf(current) == std::vector<std::string> { "gcc-13" });
    CHECK(EndpointsOf(current) == std::vector<std::string> { std::string { ThisNode } });
}

TEST_CASE("The endpoint a heartbeat re-announces is the DERIVED one, not the flag", "[node][announce][advertise]")
{
    // `AdvertisedEndpointChange` is what decides whether a round re-announces at all,
    // and it is wrong silently in both directions: missed, the fleet keeps leasing an
    // address nobody answers; fired spuriously, every worker re-registers because
    // somebody saved a file. Both directions are here.
    SECTION("no configuration file means no second moment")
    {
        // The arm `ConfiguredCredential` has as a null reloader. A worker started with
        // no file has nothing that could publish a new value, and a change function that
        // answered anything here would be describing a configuration that cannot arrive.
        CHECK_FALSE(AdvertisedEndpointChange(ThisNode, nullptr).has_value());
    }

    SECTION("a moved address is reported, and the line names both")
    {
        auto live = std::make_shared<NodeConfig>();
        live->advertise = std::string { MovedNode };

        auto const moved = AdvertisedEndpointChange(ThisNode, live);
        REQUIRE(moved.has_value());
        // `Unwrap` and not `moved->`, which is this repository's rule for an optional in
        // a test and a build failure otherwise. A reference, safely, because `moved` is a
        // named local rather than the temporary that form warns about.
        auto const& change = Unwrap(moved);
        CHECK(change.endpoint == MovedNode);
        // Both addresses, because a line naming only the new one cannot be told from a
        // startup line -- and the minutes after this is logged are when somebody is
        // reading it to explain a burst of endpoint-mismatch refusals.
        CHECK(change.announcement.contains(MovedNode));
        CHECK(change.announcement.contains(ThisNode));
    }

    SECTION("the same address is not a change, however it is spelled")
    {
        // The direction that decides whether this is safe to run every beat. A file that
        // spells out the value already in force moves the `--advertise` ROW -- which is
        // what the reload machinery compares -- and moves nothing the scheduler keys on.
        // Comparing the row here would re-register a fleet for a no-op edit.
        auto live = std::make_shared<NodeConfig>();
        auto const derived = AdvertisedEndpoint(*live);
        REQUIRE_FALSE(derived.empty());

        auto explicitlySpelled = std::make_shared<NodeConfig>();
        explicitlySpelled->advertise = derived;

        CHECK_FALSE(AdvertisedEndpointChange(derived, live).has_value());
        CHECK_FALSE(AdvertisedEndpointChange(derived, explicitlySpelled).has_value());
    }
}

// --- The cordon reaches the scheduler (#1303) -------------------------------

namespace
{

/// A load sampler that reports nothing, so a round reads no host at all.
class SilentLoadSampler final: public IHostLoadSampler
{
  public:
    [[nodiscard]] HostLoad Sample() override
    {
        return HostLoad {};
    }
};

/// Every framed request in @p sent, in order, by its declared length.
/// @param sent What a scripted socket was written.
/// @return One op byte and payload per whole frame.
[[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> FramesIn(std::span<std::byte const> sent)
{
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> frames;
    while (sent.size() >= Wire::RequestHeaderSize)
    {
        auto const header = Wire::DecodeRequestHeader(sent);
        if (!header.has_value())
            break;
        auto const whole = Wire::RequestHeaderSize + std::size_t { header->payloadLength };
        if (sent.size() < whole)
            break;
        auto const payload = sent.subspan(Wire::RequestHeaderSize, header->payloadLength);
        frames.emplace_back(header->opRaw, std::vector<std::byte> { payload.begin(), payload.end() });
        sent = sent.subspan(whole);
    }
    return frames;
}

/// One heartbeat round over a worker this case can cordon, announcing to a scripted
/// scheduler.
struct AnnounceFixture
{
    NodeConfig cfg;
    AtomicMetricsSink metrics;
    NullLogger logger;
    SilentLoadSampler loadSampler;
    // The process singleton wall clock, for the reason `NodeCredential_test` gives beside
    // the same construction: the sampler keeps the ADDRESS and reads it from its own thread.
    CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 1 }, logger };
    ConfiguredCredential credential { cfg, nullptr };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::atomic<bool> fleetMismatch { false };
    Cc::CredentialNotice notice = Cc::CredentialNotice::Silent();
    std::vector<Cc::WorkerRegistrar> registrars;
    std::vector<Cc::WorkerRegistrar> withdrawals;

    AnnounceFixture()
    {
        cfg.schedulers = { "scheduler.example:6676" };
        registrars.push_back(Registrar(notice, "gcc-14"));
    }

    /// The round production builds, over this fixture's collaborators.
    /// @return The round.
    [[nodiscard]] HeartbeatRound Round()
    {
        return HeartbeatRound { .cfg = cfg,
                                .registrars = registrars,
                                .withdrawals = withdrawals,
                                .capacity = capacity,
                                .loadSampler = loadSampler,
                                .cacheTier = nullptr,
                                .metrics = metrics,
                                .credential = credential,
                                .notice = notice,
                                // Nothing to prove and nothing to present: every case in this
                                // file is about the announce round itself, and a round with no
                                // cluster key is the ordinary single-machine shape.
                                .proofKey = nullptr,
                                .nodeId = {},
                                .lease = lease,
                                .fleetMismatch = fleetMismatch,
                                .logger = logger };
    }

    /// Announce once to a scheduler answering @p replies.
    /// @param replies What the scheduler says, in order.
    /// @return Every request the round sent.
    [[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> AnnounceTo(std::vector<std::byte> replies)
    {
        Testing::ScriptedSocket scheduler { std::move(replies) };
        (void) AnnounceOnce(Round(), scheduler, cfg.schedulers.front());
        return FramesIn(scheduler.Sent());
    }
};

/// A HEARTBEAT the scheduler accepted.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> HeartbeatOk()
{
    return Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {});
}

/// A `NotLeader` refusal naming @p leader.
/// @param leader Where the refusing scheduler says the leader is.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> NotLeaderNaming(std::string_view leader)
{
    return Wire::EncodeErrorReply(Wire::ErrorCode::NotLeader, leader);
}

/// The op of every frame sent on dial @p index.
/// @param dialer The dialer the round used.
/// @param index Which dial.
/// @return One op byte per frame, in order.
[[nodiscard]] std::vector<std::uint8_t> OpsSentOn(Testing::ScriptedDialer const& dialer, std::size_t index)
{
    std::vector<std::uint8_t> ops;
    for (auto const& frame: FramesIn(dialer.SentOn(index)))
        ops.push_back(frame.first);
    return ops;
}

constexpr std::string_view FirstScheduler = "scheduler-a.example:6676";
constexpr std::string_view SecondScheduler = "scheduler-b.example:6676";
constexpr std::string_view NamedLeader = "10.0.0.7:6676";
constexpr auto RegisterOp = static_cast<std::uint8_t>(Wire::Op::Register);
constexpr auto HeartbeatOp = static_cast<std::uint8_t>(Wire::Op::Heartbeat);

/// Whether @p frame is a HEARTBEAT whose load says cordoned.
/// @param frame An op byte and its payload.
/// @return The load's cordon, or nullopt when this is not a heartbeat that decodes.
[[nodiscard]] std::optional<bool> CordonedBeat(std::pair<std::uint8_t, std::vector<std::byte>> const& frame)
{
    if (frame.first != static_cast<std::uint8_t>(Wire::Op::Heartbeat))
        return std::nullopt;
    auto const decoded = Wire::DecodeHeartbeatPayload(frame.second);
    if (!decoded.has_value())
        return std::nullopt;
    return decoded->load.cordoned;
}

/// The link a worker configured with @p schedulers holds.
/// @param schedulers A non-empty `--scheduler` list.
/// @return The link; the case fails when none could be built.
[[nodiscard]] SchedulerLink LinkOver(std::vector<std::string> const& schedulers)
{
    auto const link = SchedulerLink::For(schedulers);
    REQUIRE(link.has_value());
    return Unwrap(link);
}

} // namespace

TEST_CASE("A heartbeat carries the worker's cordon, and a lifted one carries serving", "[node][announce][cordon]")
{
    // The scheduler learns a cordon from this and from nothing else -- nothing replicates
    // it -- so a round that sampled the load without it is a machine still handed work.
    AnnounceFixture fix;
    auto const registered = fix.AnnounceTo(RegisterOk("w-7"));
    REQUIRE_FALSE(registered.empty());

    (void) fix.capacity.Cordon(true);
    auto const beat = fix.AnnounceTo(Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}));
    REQUIRE(beat.size() == 1);
    CHECK(CordonedBeat(beat[0]) == std::optional { true });

    (void) fix.capacity.Cordon(false);
    auto const lifted = fix.AnnounceTo(Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}));
    REQUIRE(lifted.size() == 1);
    CHECK(CordonedBeat(lifted[0]) == std::optional { false });
}

TEST_CASE("A cordoned worker that has just registered says so at once, and a serving one does not",
          "[node][announce][cordon]")
{
    // A registration carries no load, so a scheduler that has just admitted a cordoned
    // worker -- a new leader, most often -- would believe it serving for a whole interval.
    // The control is the serving worker: a round that heartbeated after EVERY registration
    // would pass the first section and fail this one.
    SECTION("cordoned: the registration is followed by a heartbeat saying so")
    {
        AnnounceFixture fix;
        (void) fix.capacity.Cordon(true);
        auto const sent = fix.AnnounceTo(
            Testing::Replies({ RegisterOk("w-7"), Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {}) }));

        REQUIRE(sent.size() == 2);
        CHECK(sent[0].first == static_cast<std::uint8_t>(Wire::Op::Register));
        CHECK(CordonedBeat(sent[1]) == std::optional { true });
    }

    SECTION("serving: the registration alone")
    {
        AnnounceFixture fix;
        auto const sent = fix.AnnounceTo(RegisterOk("w-7"));

        REQUIRE(sent.size() == 1);
        CHECK(sent[0].first == static_cast<std::uint8_t>(Wire::Op::Register));
    }
}

TEST_CASE("A heartbeat round whose first scheduler is unreachable registers with the second, in the same round",
          "[node][announce][fallback]")
{
    // #1310's discrimination, at the seam production dials through. One `--scheduler`
    // value always worked, so a round whose first entry answers proves nothing about a
    // list: the first dial FAILS here, and the case asserts which endpoint then took the
    // registration, and that it was the same round rather than the next one.
    AnnounceFixture fix;
    fix.cfg.schedulers = { std::string { FirstScheduler }, std::string { SecondScheduler } };
    auto link = LinkOver(fix.cfg.schedulers);

    SECTION("the first is unreachable: the second is dialled and registers the worker")
    {
        Testing::ScriptedDialer dialer { { {}, RegisterOk("w-7"), HeartbeatOk() } };

        CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);
        REQUIRE(dialer.Dialed()
                == std::vector<std::string> { std::string { FirstScheduler }, std::string { SecondScheduler } });
        CHECK(dialer.SentOn(0).empty());
        CHECK(OpsSentOn(dialer, 1) == std::vector<std::uint8_t> { RegisterOp });
        CHECK(fix.registrars.front().WorkerId() == "w-7");

        // And the next round opens where the registration landed, rather than paying the
        // dead first entry's connect timeout on every heartbeat.
        CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);
        CHECK(dialer.Dialed().back() == SecondScheduler);
        CHECK(OpsSentOn(dialer, 2) == std::vector<std::uint8_t> { HeartbeatOp });
    }

    SECTION("control: a first scheduler that answers is the only one dialled")
    {
        // A round that dialled every entry, or always the last, would pass the section
        // above; it cannot pass this one.
        Testing::ScriptedDialer dialer { { RegisterOk("w-7") } };

        CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);
        CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler } });
        CHECK(OpsSentOn(dialer, 0) == std::vector<std::uint8_t> { RegisterOp });
    }

    SECTION("every configured scheduler unreachable: the round gives up after one dial each")
    {
        Testing::ScriptedDialer dialer { { {}, {} } };

        CHECK(AnnounceRound(fix.Round(), link, dialer) == 0);
        CHECK(dialer.Dialed().size() == 2);
    }
}

TEST_CASE("A NotLeader is followed to the endpoint it names, not to the next configured scheduler",
          "[node][announce][fallback]")
{
    // #1310 acceptance 4. A redirect is an instruction, and a list that started being
    // consulted for it would dial `SecondScheduler` here and register with a follower
    // that refuses every verb.
    AnnounceFixture fix;
    fix.cfg.schedulers = { std::string { FirstScheduler }, std::string { SecondScheduler } };
    auto link = LinkOver(fix.cfg.schedulers);
    Testing::ScriptedDialer dialer { { NotLeaderNaming(NamedLeader), RegisterOk("w-7") } };

    CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);
    CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler }, std::string { NamedLeader } });
    CHECK(OpsSentOn(dialer, 1) == std::vector<std::uint8_t> { RegisterOp });
}

TEST_CASE("A worker whose remembered leader stops answering falls back through the configured schedulers in that round",
          "[node][announce][fallback]")
{
    // #1310 acceptance 4, second half: forgetting the leader reaches the configured SET,
    // in the SAME round -- past a first entry that is itself unreachable.
    AnnounceFixture fix;
    fix.cfg.schedulers = { std::string { FirstScheduler }, std::string { SecondScheduler } };
    auto link = LinkOver(fix.cfg.schedulers);
    Testing::ScriptedDialer dialer { {
        NotLeaderNaming(NamedLeader),
        RegisterOk("w-7"), // round one: the leader takes the registration and is remembered
        {},                // round two: the remembered leader is gone...
        {},                // ...and so is the first configured scheduler...
        HeartbeatOk(),     // ...and the second answers
    } };

    REQUIRE(AnnounceRound(fix.Round(), link, dialer) == 1);

    CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);
    CHECK(dialer.Dialed()
          == std::vector<std::string> { std::string { FirstScheduler },
                                        std::string { NamedLeader },
                                        std::string { NamedLeader },
                                        std::string { FirstScheduler },
                                        std::string { SecondScheduler } });
    CHECK(OpsSentOn(dialer, 4) == std::vector<std::uint8_t> { HeartbeatOp });
}
