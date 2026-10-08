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
#include "AnnounceTestFixture.hpp"
#include "EndpointDialerTestUtils.hpp"
#include "NodeAnnounce.hpp"
#include "NodeFormation.hpp"
#include "NodePresenceTier.hpp"
#include "NodeRoster.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <core/platform/Clock.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/NodeProofFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;
using namespace FastCache::Node::AnnounceTesting;

namespace
{
namespace Wire = FastCache::CompileCacheWire;

/// Where it ends up once it learns its external address.
inline constexpr std::string_view MovedNode = "nat.example:7700";

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("gcc-13"));
    current.push_back(Registrar("clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("gcc-14"));
    rebuilt.push_back(Registrar("clang-20"));

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("gcc-13"));
    current.push_back(Registrar("clang-20"));

    Testing::ScriptedSocket scheduler { Testing::Replies({
        RegisterOk("w-gcc"),
        RegisterOk("w-clang"),
    }) };
    REQUIRE(current[0].Register(scheduler, {}).has_value());
    REQUIRE(current[1].Register(scheduler, {}).has_value());
    REQUIRE(current[0].WorkerId() == "w-gcc");

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("clang-20"));

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("clang-20"));

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("gcc-13"));

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("gcc-13"));
    current.push_back(Registrar("clang-20"));

    Testing::ScriptedSocket scheduler { Testing::Replies({
        RegisterOk("w-gcc"),
        RegisterOk("w-clang"),
    }) };
    REQUIRE(current[0].Register(scheduler, {}).has_value());
    REQUIRE(current[1].Register(scheduler, {}).has_value());

    // The same fingerprints -- nothing about what this machine SERVES has changed, which
    // is exactly why the fingerprint alone could not answer this.
    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("gcc-13", MovedNode));
    rebuilt.push_back(Registrar("clang-20", MovedNode));

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

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar("gcc-13"));

    Testing::ScriptedSocket scheduler { Testing::Replies({ RegisterOk("w-gcc") }) };
    REQUIRE(current[0].Register(scheduler, {}).has_value());

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("gcc-13"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), current, withdrawals);

    CHECK(withdrawals.empty());
    CHECK(NamesOf(current) == std::vector<std::string> { "gcc-13" });
    CHECK(EndpointsOf(current) == std::vector<std::string> { std::string { ThisNode } });
}

TEST_CASE("The line a worker logs for a moved endpoint names both addresses", "[node][announce][advertise]")
{
    // WHETHER the endpoint moved is `EndpointResolver`'s to decide now, from the DERIVED endpoint
    // rather than the flag -- `EndpointResolver_test.cpp` holds that half. What stays here is what
    // is SAID: both addresses, because a line naming only the new one cannot be told from a
    // startup line -- and the minutes after this is logged are when somebody is reading it to
    // explain a burst of endpoint-mismatch refusals.
    auto const said = DescribeEndpointMove(ThisNode, MovedNode);
    CHECK(said.contains(MovedNode));
    CHECK(said.contains(ThisNode));
    CHECK(said.starts_with(std::format("now advertising {} instead of {}", MovedNode, ThisNode)));
}

// --- The cordon reaches the scheduler (#1303) -------------------------------

namespace
{

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

/// How many lines @p logger captured at exactly @p level whose text contains @p phrase.
/// @param logger What a round logged into.
/// @param level The level to count.
/// @param phrase A substring every counted line carries; empty counts every line at @p level.
/// @return The count.
[[nodiscard]] std::ptrdiff_t LinesAt(CapturingLogger const& logger, LogLevel level, std::string_view phrase)
{
    auto const records = logger.Snapshot();
    return std::ranges::count_if(records, [level, phrase](CapturingLogger::Record const& record) {
        return record.level == level && record.message.contains(phrase);
    });
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
    fix.cfg = Testing::LearnerRegisteringWith(fix.cfg, { std::string { FirstScheduler }, std::string { SecondScheduler } });
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));

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
    fix.cfg = Testing::LearnerRegisteringWith(fix.cfg, { std::string { FirstScheduler }, std::string { SecondScheduler } });
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
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
    fix.cfg = Testing::LearnerRegisteringWith(fix.cfg, { std::string { FirstScheduler }, std::string { SecondScheduler } });
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
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

namespace
{

/// The addresses a report keeps out of @p addresses.
/// @param addresses What the machine answers on.
/// @return The report's list alone.
[[nodiscard]] std::vector<std::string> Kept(std::vector<std::string> addresses)
{
    return ReportableInterfaceAddresses(std::move(addresses)).addresses;
}

/// The interface addresses a REGISTER frame carries.
/// @param frame An op byte and its payload.
/// @return The list, or nullopt when this is not a REGISTER that decodes.
[[nodiscard]] std::optional<std::vector<std::string>> RegisteredAddresses(
    std::pair<std::uint8_t, std::vector<std::byte>> const& frame)
{
    if (frame.first != RegisterOp)
        return std::nullopt;
    auto const decoded = Wire::DecodeRegisterPayload(frame.second);
    if (!decoded.has_value())
        return std::nullopt;
    return decoded->capacity.interfaceAddresses;
}

/// The interface addresses a HEARTBEAT frame carries.
/// @param frame An op byte and its payload.
/// @return The list, or nullopt when this is not a HEARTBEAT that decodes.
[[nodiscard]] std::optional<std::vector<std::string>> BeatAddresses(
    std::pair<std::uint8_t, std::vector<std::byte>> const& frame)
{
    if (frame.first != HeartbeatOp)
        return std::nullopt;
    auto const decoded = Wire::DecodeHeartbeatPayload(frame.second);
    if (!decoded.has_value())
        return std::nullopt;
    return decoded->load.interfaceAddresses;
}

/// Routable addresses from `10.0.0.100` upwards, which sort as they count.
/// @param count How many.
/// @return The addresses, in order.
[[nodiscard]] std::vector<std::string> RoutableAddresses(std::size_t count)
{
    std::vector<std::string> many;
    many.reserve(count);
    for (auto const i: std::views::iota(std::size_t { 0 }, count))
        many.push_back(std::format("10.0.0.{}", 100 + i));
    return many;
}

/// How many times the cap has been said.
/// @param logger The fixture's log.
/// @return The count of Info lines naming the cap.
[[nodiscard]] std::size_t CapNotices(CapturingLogger const& logger)
{
    auto const records = logger.Snapshot();
    return static_cast<std::size_t>(std::ranges::count_if(records, [](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Info && record.message.contains("a report carries at most");
    }));
}

/// The routable address each filter case keeps beside the entries it drops, so a filter that
/// dropped everything reads as a failure rather than as the row under test holding.
constexpr std::string_view Routable = "10.8.0.7";

} // namespace

TEST_CASE("A report leaves out loopback, in every spelling", "[node][announce][dialhint]")
{
    CHECK(Kept({ std::string { Routable }, "127.0.0.1", "127.3.4.5", "::1", "::ffff:127.0.0.2" })
          == std::vector<std::string> { std::string { Routable } });
}

TEST_CASE("A report leaves out link-local addresses, which the scheduler never hints", "[node][announce][dialhint]")
{
    CHECK(Kept({ std::string { Routable }, "fe80::1", "febf::9", "169.254.3.4", "::ffff:169.254.0.9" })
          == std::vector<std::string> { std::string { Routable } });
    // Just outside fe80::/10 on both sides, and a unique-local address: none is link-local.
    CHECK(Kept({ "fec0::1", "fd00::7", "fe7f::1" }) == std::vector<std::string> { "fd00::7", "fe7f::1", "fec0::1" });
}

TEST_CASE("A report leaves out what the wire would refuse, and keeps the longest entry it carries",
          "[node][announce][dialhint]")
{
    auto const longest = std::string(Wire::MaxInterfaceAddressBytes, 'a');
    CHECK(Kept({ std::string { Routable }, "", longest + "a", longest })
          == std::vector<std::string> { std::string { Routable }, longest });
}

TEST_CASE("A report is sorted and unique, so one set always encodes to one list", "[node][announce][dialhint]")
{
    auto const report = ReportableInterfaceAddresses({ "10.8.0.9", "10.8.0.7", "10.8.0.9", "10.8.0.7" });
    CHECK(report.addresses == std::vector<std::string> { "10.8.0.7", "10.8.0.9" });
    CHECK(report.overCap == 0);
    CHECK(ReportableInterfaceAddresses({}).addresses.empty());
}

TEST_CASE("A report stops at the wire's cap after filtering, and says how many the cap left out",
          "[node][announce][dialhint]")
{
    auto const over = ReportableInterfaceAddresses(RoutableAddresses(Wire::MaxInterfaceAddresses + 8));
    CHECK(over.addresses.size() == Wire::MaxInterfaceAddresses);
    CHECK(over.overCap == 8);
    // The ones that sort first are kept, whatever order they were enumerated in.
    CHECK(over.addresses.front() == "10.0.0.100");
    CHECK(over.addresses.back() == std::format("10.0.0.{}", 100 + Wire::MaxInterfaceAddresses - 1));

    auto const exact = ReportableInterfaceAddresses(RoutableAddresses(Wire::MaxInterfaceAddresses));
    CHECK(exact.addresses.size() == Wire::MaxInterfaceAddresses);
    CHECK(exact.overCap == 0);

    // Filtered FIRST: a cap spent on loopback aliases would leave the routable one out.
    std::vector<std::string> aliases;
    for (auto const i: std::views::iota(std::size_t { 0 }, Wire::MaxInterfaceAddresses + 4))
        aliases.push_back(std::format("127.0.1.{}", i));
    aliases.emplace_back("10.200.0.1");
    auto const filtered = ReportableInterfaceAddresses(aliases);
    CHECK(filtered.addresses == std::vector<std::string> { "10.200.0.1" });
    CHECK(filtered.overCap == 0);
}

TEST_CASE("A registration and every heartbeat carry the addresses this machine answers on now", "[node][announce][dialhint]")
{
    AnnounceFixture fix;
    auto const expected = std::vector<std::string> { "10.8.0.7", "192.168.1.20" };

    auto const registered = fix.AnnounceTo(RegisterOk("w-7"));
    REQUIRE(registered.size() == 1);
    CHECK(RegisteredAddresses(registered[0]) == std::optional { expected });

    auto const beat = fix.AnnounceTo(HeartbeatOk());
    REQUIRE(beat.size() == 1);
    CHECK(BeatAddresses(beat[0]) == std::optional { expected });

    // A VPN reconnect between two rounds. Until the oracle's refresh the beat still reports what
    // the ticket audience accepts -- the set it answers from -- so no hint can name an address
    // this node would refuse; the first beat after it reports the new set.
    fix.addresses.Publish({ "10.8.0.42", "127.0.0.1" });
    auto const unrefreshed = fix.AnnounceTo(HeartbeatOk());
    REQUIRE(unrefreshed.size() == 1);
    CHECK(BeatAddresses(unrefreshed[0]) == std::optional { expected });
    fix.clock.advance(CachedLocalityOracle::DefaultRefreshInterval);
    auto const moved = fix.AnnounceTo(HeartbeatOk());
    REQUIRE(moved.size() == 1);
    CHECK(BeatAddresses(moved[0]) == std::optional { std::vector<std::string> { "10.8.0.42" } });
}

TEST_CASE("A machine whose addresses cannot be read still registers and heartbeats, reporting none",
          "[node][announce][dialhint]")
{
    AnnounceFixture fix;
    fix.Moved({});

    auto const registered = fix.AnnounceTo(RegisterOk("w-7"));
    REQUIRE(registered.size() == 1);
    CHECK(RegisteredAddresses(registered[0]) == std::optional { std::vector<std::string> {} });
    CHECK(fix.lease.fleet.Pinned() == std::optional<std::string> { "fleet-a" });

    auto const beat = fix.AnnounceTo(HeartbeatOk());
    REQUIRE(beat.size() == 1);
    CHECK(BeatAddresses(beat[0]) == std::optional { std::vector<std::string> {} });
}

TEST_CASE("A machine with more addresses than a report carries says so once, not every heartbeat",
          "[node][announce][dialhint]")
{
    SECTION("over the cap: one line across a registration and two heartbeats, naming the dropped count")
    {
        AnnounceFixture fix;
        fix.Moved(RoutableAddresses(Wire::MaxInterfaceAddresses + 8));

        (void) fix.AnnounceTo(RegisterOk("w-7"));
        (void) fix.AnnounceTo(HeartbeatOk());
        auto const beat = fix.AnnounceTo(HeartbeatOk());

        REQUIRE(beat.size() == 1);
        CHECK(BeatAddresses(beat[0]).value_or(std::vector<std::string> {}).size() == Wire::MaxInterfaceAddresses);
        CHECK(CapNotices(fix.logger) == 1);
        auto const records = fix.logger.Snapshot();
        CHECK(std::ranges::any_of(
            records, [](CapturingLogger::Record const& record) { return record.message.contains("the 8 that sort last"); }));
    }

    SECTION("at the cap: nothing to say")
    {
        AnnounceFixture fix;
        fix.Moved(RoutableAddresses(Wire::MaxInterfaceAddresses));

        (void) fix.AnnounceTo(RegisterOk("w-7"));
        (void) fix.AnnounceTo(HeartbeatOk());

        CHECK(CapNotices(fix.logger) == 0);
    }
}

TEST_CASE("An hour of unreachable heartbeat rounds warns once and reminds on the cadence", "[node][announce][reachability]")
{
    // At the seam production dials through. An hour of a dead scheduler is 180 rounds of this loop;
    // what it says about them is one Warn when the dials start failing, an Info reminder per cadence
    // while they go on failing, and a Warn when the scheduler answers again -- everything else Debug.
    AnnounceFixture fix;
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));

    constexpr std::size_t Rounds = 180;
    std::vector<std::vector<std::byte>> script(Rounds); // every dial fails...
    script.push_back(RegisterOk("w-7"));                // ...and then the scheduler answers
    Testing::ScriptedDialer dialer { std::move(script) };

    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, Rounds))
    {
        CHECK(AnnounceRound(fix.Round(), link, dialer) == 0);
        fix.clock.advance(NodeAnnounceInterval);
    }
    CHECK(AnnounceRound(fix.Round(), link, dialer) == 1);

    constexpr auto Reminders = ((static_cast<long long>(Rounds) - 1) * NodeAnnounceInterval) / SchedulerUnreachableCadence;
    static_assert(Reminders == 5);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "scheduler.example:6676 unreachable") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Info, "unreachable -- still, after") == Reminders);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "scheduler.example:6676 reachable again after 3600s unreachable") == 1);
}

TEST_CASE("Two announce loops sharing one reachability say the transition once", "[node][announce][reachability]")
{
    // Two WORKER rounds per interval, each over a link of its own and both reporting into one tracker.
    // They stand in for the worker's heartbeat and the machine's presence loop, which reach an
    // unreachable scheduler through the same arm of `DialAndAnnounce` -- only what is SAID after a
    // connect differs between them -- and which `main` lends one tracker, so a machine running both
    // says a loss once. What the presence loop itself says is the case below and `NodePresenceTier_test`.
    AnnounceFixture fix;
    auto firstLink = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    auto secondLink = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    Testing::ScriptedDialer dialer { std::vector<std::vector<std::byte>>(6) };

    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
    {
        CHECK(AnnounceRound(fix.Round(), firstLink, dialer) == 0);
        CHECK(AnnounceRound(fix.Round(), secondLink, dialer) == 0);
        fix.clock.advance(NodeAnnounceInterval);
    }

    CHECK(LinesAt(fix.logger, LogLevel::Warn, "unreachable") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Debug, "unreachable (for") == 5);
}

TEST_CASE("A registration refused round after round is one Warn and not one per round", "[node][announce][reachability]")
{
    // Three refused rounds say exactly one Warn line between them: the refusal's transition. The
    // `0 of 1 toolchain(s) registered` summary is Debug on every round, because the refusal it counts
    // is said on its own.
    AnnounceFixture fix;
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    auto const refused = Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "");
    Testing::ScriptedDialer dialer { { refused, refused, refused } };

    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
    {
        CHECK(AnnounceRound(fix.Round(), link, dialer) == 0);
        fix.clock.advance(NodeAnnounceInterval);
    }

    CHECK(LinesAt(fix.logger, LogLevel::Warn, "") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "did not register gcc-14") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Debug, "0 of 1 toolchain(s) registered") == 3);
}

TEST_CASE("A registration exchange that stalls after connecting is unreachable, not a refused registration",
          "[node][announce][reachability][conditions]")
{
    // A scheduler that ACCEPTED the connection and then never finished answering never told this
    // node anything about the toolchain: it is the scheduler being unreachable, not a refused
    // registration. Counting it as `RegistrationRefused` sends an operator hunting for a
    // fingerprint problem that was never the issue, and leaves `scheduler-unreachable` unraised
    // for exactly the outage it exists to report.
    AnnounceFixture fix;
    // Empty: the peer accepted the write and closed without a byte back, which is EOF on the
    // very first read -- what a stall this node's own deadline eventually cuts off, or a lost
    // peer, produces on the read side.
    (void) fix.AnnounceTo({});

    CHECK(fix.conditions.StateOf(NodeCondition::SchedulerUnreachable) == CompileCacheWire::ConditionState::Raised);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "unreachable") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "did not register") == 0);
}

TEST_CASE("A registration refusal the scheduler actually answered never raises scheduler-unreachable",
          "[node][announce][reachability][conditions]")
{
    // The control for the case above: a real `RegistrationRefused` -- the scheduler answered,
    // plainly -- must leave the condition exactly where the constructor left it, `clear`, because
    // the remedy for `NotAMember` names this node's membership, never the network.
    AnnounceFixture fix;
    (void) fix.AnnounceTo(Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, ""));

    CHECK(fix.conditions.StateOf(NodeCondition::SchedulerUnreachable) == CompileCacheWire::ConditionState::Clear);
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "did not register") == 1);
}

TEST_CASE("A learner's next round registers where its applied state records a voter NOW, without a reform",
          "[node][announce][presence][fleet]")
{
    // T26's carry, end to end through the round: the formation remembers the voter's endpoint as the
    // approval carried it; the voter moves and announces its new one, proven, and the leader records
    // it; the learner applies that state. Its NEXT presence round dials the new endpoint -- the link
    // re-reads `AppliedSchedulers` at `BeginRound` -- rather than the remembered one until a restart.
    AnnounceFixture fix;
    auto const remembered = SchedulersOf(fix.cfg, AsConfigured);
    REQUIRE(remembered.size() == 1);
    AppliedSchedulers schedulers { fix.cfg, AsConfigured };
    auto built = SchedulerLink::Over(schedulers);
    REQUIRE(built.has_value());
    auto link = Testing::Unwrap(built);

    auto const recorded = Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {});
    Testing::ScriptedDialer dialer { { recorded, recorded } };
    auto const capacity = Wire::CapacityFields {};
    auto const load = Wire::LoadFields {};
    auto const announce = [&] {
        return AnnouncePresence(PresenceMessage { .endpoint = ThisNode,
                                                  .capacity = capacity,
                                                  .load = load,
                                                  .logger = fix.logger,
                                                  .prover = nullptr,
                                                  .reachability = fix.reachability,
                                                  .joinMemos = {} },
                                link,
                                dialer);
    };
    REQUIRE(announce());

    // The voter moved: the state the learner applies records it at its new endpoint.
    constexpr std::string_view Moved = "scheduler-moved.example:6676";
    auto state = Cluster::ClusterState {};
    state.members.push_back(
        Cluster::ClusterMember { .id = "office",
                                 .raftEndpoint = "office.example:6680",
                                 .schedulerEndpoint = std::string { Moved },
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                 .seat = Cluster::MemberSeat::Voter,
                                 .publicKey = {} });
    schedulers.Applied(state);
    CHECK(announce());
    CHECK(dialer.Dialed() == std::vector<std::string> { remembered.front(), std::string { Moved } });
}

TEST_CASE("A worker's success at a scheduler does not end the presence loop's refusal there",
          "[node][announce][presence][reachability]")
{
    // Both loops report into one tracker, and at one scheduler they share the place `(endpoint, "")`:
    // a dial, a proof and this machine's presence are all filed there. A worker's registration and
    // heartbeat are filed under their FINGERPRINT instead, so neither can end the presence loop's
    // refusal -- which, filed under the empty subject, would log "recorded this machine again" about
    // a machine that scheduler is still refusing. The worker's dial succeeding does not end it
    // either: a dial is an earlier stage than the announcement that is being refused.
    AnnounceFixture fix;
    auto workerLink = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    auto presenceLink = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    auto const refused = Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember, "");
    auto const recorded = Wire::EncodeReply(Wire::Status::Ok, std::vector<std::byte> {});
    Testing::ScriptedDialer dialer { { refused, RegisterOk("w-7"), HeartbeatOk(), refused, recorded } };

    auto const capacity = Wire::CapacityFields {};
    auto const load = Wire::LoadFields {};
    auto const announcePresence = [&] {
        return AnnouncePresence(PresenceMessage { .endpoint = ThisNode,
                                                  .capacity = capacity,
                                                  .load = load,
                                                  .logger = fix.logger,
                                                  .prover = nullptr,
                                                  .reachability = fix.reachability,
                                                  .joinMemos = {} },
                                presenceLink,
                                dialer);
    };

    CHECK_FALSE(announcePresence());
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "did not record this machine at 10.0.0.2:6677") == 1);

    // The worker registers, and a round later heartbeats: two successes at the same scheduler.
    CHECK(AnnounceRound(fix.Round(), workerLink, dialer) == 1);
    fix.clock.advance(NodeAnnounceInterval);
    CHECK(AnnounceRound(fix.Round(), workerLink, dialer) == 1);
    CHECK(OpsSentOn(dialer, 2) == std::vector<std::uint8_t> { HeartbeatOp });

    // Neither said the machine was recorded again, at any level.
    auto const records = fix.logger.Snapshot();
    CHECK_FALSE(std::ranges::any_of(records, [](CapturingLogger::Record const& record) {
        return record.message.contains("recorded this machine again");
    }));

    // The next refusal is the SAME setback lasting, not a new one: Debug, and timed from the first.
    CHECK_FALSE(announcePresence());
    CHECK(LinesAt(fix.logger, LogLevel::Debug, "did not record this machine at 10.0.0.2:6677") == 1);
    CHECK(LinesAt(fix.logger, LogLevel::Debug, "(for 20s)") == 1);

    // And only the presence loop's own success ends it, at the level its loss was said at.
    CHECK(announcePresence());
    CHECK(LinesAt(fix.logger, LogLevel::Warn, "scheduler.example:6676 recorded this machine again after 20s") == 1);
}

namespace
{

/// One line of a source file, as code.
struct CodeLine
{
    std::size_t number; ///< 1-based.
    std::string code;   ///< The line with any `//` comment cut.
};

/// @p line with a `//` comment cut off. A `//` inside a string literal is not a comment. A `/* */`
/// comment is left in place: a scan reading the result then errs toward refusing a mention in it.
/// @param line One source line.
/// @return Its code.
[[nodiscard]] std::string WithoutLineComment(std::string line)
{
    auto inString = false;
    for (auto const at: std::views::iota(std::size_t { 0 }, line.size()))
    {
        if (line[at] == '"' && (at == 0 || line[at - 1] != '\\'))
            inString = !inString;
        else if (!inString && line.compare(at, 2, "//") == 0)
        {
            line.resize(at);
            break;
        }
    }
    return line;
}

/// The lines of @p path that carry code, numbered as the file numbers them.
/// @param path A source file.
/// @return Every line with code left once its comment is cut; blank lines are dropped.
[[nodiscard]] std::vector<CodeLine> CodeLinesOf(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    std::ostringstream contents;
    contents << in.rdbuf();
    std::istringstream text { std::move(contents).str() };
    std::vector<CodeLine> lines;
    std::string line;
    std::size_t number = 0;
    while (std::getline(text, line))
    {
        ++number;
        auto code = WithoutLineComment(line);
        if (code.find_first_not_of(" \t") != std::string::npos)
            lines.push_back(CodeLine { .number = number, .code = std::move(code) });
    }
    return lines;
}

/// Whether one mention of `SchedulerReachability` is a spelling that can never make an instance.
///
/// An ALLOWLIST: the include, a reference, a `const` reference and a qualified member anywhere; the
/// class head, the constructors and the destructor in the type's own two files. Everything else is
/// not harmless, whether or not it makes an instance.
/// @param code The whole line.
/// @param before The line's text before the mention.
/// @param after The line's text after it.
/// @param definesTheType Whether the line is in `SchedulerReachability.hpp` or `.cpp`.
/// @return True when harmless.
[[nodiscard]] bool HarmlessMention(std::string const& code,
                                   std::string const& before,
                                   std::string const& after,
                                   bool definesTheType)
{
    if (std::regex_search(code, std::regex { R"(^\s*#include "SchedulerReachability\.hpp"\s*$)" }))
        return true;
    if (std::regex_search(after, std::regex { R"(^(&|::|\s*const\s*&))" }))
        return true;
    if (!definesTheType)
        return false;
    // The class head, or a `(` after the name with a constructor's or the destructor's lead-in before it.
    return std::regex_search(code, std::regex { R"(^\s*class SchedulerReachability\s*$)" })
           || (after.starts_with('(') && std::regex_search(before, std::regex { R"((^\s*(explicit\s+)?|~|::)$)" }));
}

/// Whether @p name is one of this directory's test-support headers, by the suffix they all carry.
/// @param name A file name.
/// @return True for `...TestFixture.hpp` and `...TestUtils.hpp`.
[[nodiscard]] bool NamesTestSupportHeader(std::string_view name)
{
    return name.ends_with("TestFixture.hpp") || name.ends_with("TestUtils.hpp");
}

/// Whether the scan below skips @p name as test code: a case file, or a test-support header.
/// @param name A file name.
/// @return True when skipped.
[[nodiscard]] bool IsTestCode(std::string_view name)
{
    return name.ends_with("_test.cpp") || NamesTestSupportHeader(name);
}

/// The header @p code includes, if it is an include, in either spelling and under any directory
/// prefix. The ONE parse: the scan's count of includes read and its verdict both go through it.
/// @param code One line of code, its comment cut.
/// @return The header's file name, or nullopt when the line includes nothing.
[[nodiscard]] std::optional<std::string> IncludedHeaderOf(std::string const& code)
{
    std::regex const include { R"(^\s*#\s*include\s*["<]([^">]*/)?([^">/]+)[">])" };
    std::smatch included;
    if (!std::regex_search(code, included, include))
        return std::nullopt;
    return included[2].str();
}

/// The test-support header @p code includes, if it includes one.
/// @param code One line of code, its comment cut.
/// @return The header's file name, or nullopt when the line includes none.
[[nodiscard]] std::optional<std::string> TestSupportIncludeOf(std::string const& code)
{
    auto header = IncludedHeaderOf(code);
    if (!header.has_value() || !NamesTestSupportHeader(*header))
        return std::nullopt;
    return header;
}

/// Every include of a test-support header in @p lines, as `line: code` -- what the scan below
/// flags per file, and what its self-test drives over a planted one.
/// @param lines A file's code lines, as `CodeLinesOf` read them.
/// @return One entry per offending line.
[[nodiscard]] std::vector<std::string> TestSupportIncludesIn(std::span<CodeLine const> lines)
{
    std::vector<std::string> found;
    for (auto const& [number, code]: lines)
        if (TestSupportIncludeOf(code).has_value())
            found.push_back(std::format("{}: {}", number, code));
    return found;
}

} // namespace

TEST_CASE("The test-support include check fires on a planted include and passes production's",
          "[node][announce][reachability]")
{
    // The self-test of the premise the one-tracker scan skips test-support headers on: a check that
    // cannot be seen to fire proves nothing by staying quiet.
    CHECK(TestSupportIncludeOf(R"(#include "AnnounceTestFixture.hpp")")
          == std::optional<std::string> { "AnnounceTestFixture.hpp" });
    CHECK(TestSupportIncludeOf(R"(  #  include <apps/fastcache-compile-node/EndpointDialerTestUtils.hpp>)")
          == std::optional<std::string> { "EndpointDialerTestUtils.hpp" });
    CHECK_FALSE(TestSupportIncludeOf(R"(#include "NodeAnnounce.hpp")").has_value());
    CHECK_FALSE(TestSupportIncludeOf(R"(#include <tests/ScriptedSocket.hpp>)").has_value());
    CHECK_FALSE(TestSupportIncludeOf(R"(auto const name = "WorkerTierTestFixture.hpp";)").has_value());
    // The parse underneath both, which the scan's count of includes read goes through too.
    CHECK(IncludedHeaderOf(R"(#include "NodeAnnounce.hpp")") == std::optional<std::string> { "NodeAnnounce.hpp" });
    CHECK_FALSE(IncludedHeaderOf("auto const included = true;").has_value());

    // And through the file reader the scan uses, over a planted production file: a commented-out
    // include is no include, a live one is found at its line.
    FastCache::Testing::ScratchDirectory const scratch { "fc-test-support-include" };
    auto const planted = scratch.Path() / "Planted.cpp";
    {
        // Binary, so the planted file is LF as every file in this tree is (`.gitattributes`); text mode
        // would write CRLF on Windows and plant a `\r` no real source carries.
        std::ofstream out { planted, std::ios::binary };
        out << "#include \"NodeAnnounce.hpp\"\n"
               "// #include \"WorkerTierTestFixture.hpp\"\n"
               "#include \"WorkerTierTestFixture.hpp\"\n";
    }
    CHECK(TestSupportIncludesIn(CodeLinesOf(planted))
          == std::vector<std::string> { R"(3: #include "WorkerTierTestFixture.hpp")" });
}

TEST_CASE("The node lends ONE SchedulerReachability to both announce loops", "[node][announce][reachability]")
{
    // The case above is the property in miniature; this is the wiring it depends on. `main.cpp` is in
    // no test target, so the wiring is read from the source, and each loop takes a REFERENCE, which binds
    // to any instance -- nothing else compels one tracker. A second one, as a member of either loop or a
    // second local, is a machine that says every loss twice.
    //
    // **The scan fails CLOSED: it lists what may be spelled, never what may not.** Every mention of the
    // type in the node's non-test code is one `HarmlessMention` allows, or it is the ONE declaration,
    // in `main.cpp`, in the one shape `declaration` accepts. Anything else -- `auto x =
    // SchedulerReachability { ... }`, a temporary, a value member, a second declarator on the
    // declaration's line -- is refused by `file:line`, whether or not it makes a second tracker: a
    // spelling this scan cannot classify is exactly the one it would otherwise vouch for.
    //
    // Its blind spot, and the direction it fails in: a second instance reached without writing the
    // type's NAME at all -- `decltype(schedulerReachability)`, a deduced `auto` alias, a template
    // parameter -- leaves no `SchedulerReachability` token for `mention` to find, so this scan sees
    // nothing and reports clean. That failure is OPEN, the opposite of the one this test proves shut.
    //
    // A second blind spot, by NAME and failing open the same way: this directory's test-support
    // headers (`...TestFixture.hpp`, `...TestUtils.hpp`) are test code, compiled into no production
    // target, and a fixture holds a tracker of its own by design. A production header given one of
    // those suffixes would be skipped too. The premise is ASSERTED rather than assumed: no file this
    // scan reads includes such a header (`TestSupportIncludeOf`, self-tested above). Its reach is this
    // directory, where the headers live.
    std::filesystem::path const nodeDir =
        std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node";
    REQUIRE(std::filesystem::is_directory(nodeDir));

    std::regex const mention { R"(\bSchedulerReachability\b)" };
    // The ONE declaration: alone on its line, one declarator, braced from one clock and the announce gate.
    std::regex const declaration {
        R"(^\s*(Node::)?SchedulerReachability ([A-Za-z_]\w*) \{ [A-Za-z_]\w*, [A-Za-z_]\w* \? &[A-Za-z_]\w* : nullptr \};$)"
    };

    struct Mention
    {
        std::string file;
        std::size_t line;
        std::string code;
    };
    std::vector<Mention> unclassified;
    std::size_t scanned = 0;
    std::size_t harmless = 0;
    std::vector<std::string> mainLines;
    std::vector<std::string> testSupport;
    std::vector<std::string> productionIncludesOfTestSupport;
    std::size_t includesRead = 0;

    for (auto const& entry: std::filesystem::directory_iterator { nodeDir })
    {
        auto const name = entry.path().filename().string();
        auto const extension = entry.path().extension().string();
        if (extension != ".cpp" && extension != ".hpp")
            continue;
        if (IsTestCode(name))
        {
            testSupport.push_back(name);
            continue;
        }
        auto const lines = CodeLinesOf(entry.path());
        REQUIRE_FALSE(lines.empty());
        ++scanned;
        auto const definesTheType = name.starts_with("SchedulerReachability.");
        // The skip's premise, through the self-tested helper, over the lines already read.
        for (auto const& offending: TestSupportIncludesIn(lines))
            productionIncludesOfTestSupport.push_back(std::format("{}:{}", name, offending));
        for (auto const& [number, code]: lines)
        {
            if (name == "main.cpp")
                mainLines.push_back(code);
            if (IncludedHeaderOf(code).has_value())
                ++includesRead;
            if (!code.contains("SchedulerReachability"))
                continue;
            auto const found = std::sregex_iterator { code.begin(), code.end(), mention };
            for (auto const& match: std::ranges::subrange { found, std::sregex_iterator {} })
            {
                if (HarmlessMention(code, match.prefix().str(), match.suffix().str(), definesTheType))
                    ++harmless;
                else
                    unclassified.push_back(Mention { .file = name, .line = number, .code = code });
            }
        }
    }

    // The positive controls. A scan over the wrong directory, or one whose pattern stopped matching,
    // finds nothing unclassified and would read as a tree with no tracker at all; so it must have read
    // the tree, and recognised the harmless spellings the loops' own declarations use.
    CHECK(scanned > 20);
    CHECK(harmless > 10);
    REQUIRE_FALSE(mainLines.empty());
    // And the test-support rule matched the header that needed it, and left production alone.
    CHECK(std::ranges::count(testSupport, std::string { "AnnounceTestFixture.hpp" }) == 1);
    CHECK(std::ranges::count(testSupport, std::string { "NodeAnnounce.hpp" }) == 0);
    // The skip's premise: nothing compiled into the node includes a test-support header. Measured over
    // includes the verdict's own parse recognised, or an empty list would say nothing.
    CHECK(includesRead > 50);
    INFO("production files including a test-support header: " << [&] {
        std::string joined;
        for (auto const& offending: productionIncludesOfTestSupport)
            joined += "\n  " + offending;
        return joined;
    }());
    CHECK(productionIncludesOfTestSupport.empty());

    INFO("every mention of SchedulerReachability that is not a spelling that cannot make an instance: " << [&] {
        std::string joined;
        for (auto const& unknown: unclassified)
            joined += std::format("\n  {}:{}: {}", unknown.file, unknown.line, unknown.code);
        return joined;
    }());
    REQUIRE(unclassified.size() == 1);
    CHECK(unclassified.front().file == "main.cpp");
    std::smatch declared;
    REQUIRE(std::regex_match(unclassified.front().code, declared, declaration));

    auto const tracker = declared[2].str();
    auto const linesMatching = [&mainLines](std::regex const& pattern) {
        return std::ranges::count_if(mainLines,
                                     [&pattern](std::string const& line) { return std::regex_search(line, pattern); });
    };
    INFO("the one tracker is " << tracker);
    CHECK(linesMatching(std::regex { R"(->Launch\(.*\b)" + tracker + R"(\b)" }) == 1);
    CHECK(linesMatching(std::regex { R"(\.reachability\s*=\s*)" + tracker + R"(\b)" }) == 1);
}

namespace
{
constexpr auto WithdrawOp = static_cast<std::uint8_t>(Wire::Op::Withdraw);

/// Register the fixture's one registrar for real, then retire everything as a suspend does.
/// @param fix The fixture.
void RegisterThenRetireForSuspend(AnnounceFixture& fix)
{
    Testing::ScriptedSocket scheduler { Testing::Replies({ RegisterOk("w-7") }) };
    REQUIRE(fix.registrars.front().Register(scheduler, {}).has_value());

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("gcc-14"));
    RetireAllRegistrations(std::move(rebuilt), fix.registrars, fix.withdrawals);
    REQUIRE(fix.withdrawals.size() == 1);
    REQUIRE(fix.withdrawals.front().WorkerId() == "w-7");
    REQUIRE(fix.registrars.front().WorkerId().empty());
}
} // namespace

TEST_CASE("A suspend withdraws every registration in one short dial, and registers nothing", "[node][announce][host-events]")
{
    AnnounceFixture fix;
    fix.cfg = Testing::LearnerRegisteringWith(fix.cfg, { std::string { FirstScheduler }, std::string { SecondScheduler } });
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    RegisterThenRetireForSuspend(fix);

    SECTION("the endpoint the link names is dialled once, with the suspend's own connect bound")
    {
        Testing::ScriptedDialer dialer { { HeartbeatOk() } };
        CHECK(WithdrawOnce(fix.Round(), link, dialer) == 1);
        CHECK(dialer.Dialed() == std::vector<std::string> { std::string { FirstScheduler } });
        CHECK(dialer.OptionsOn(0).connectTimeout == SuspendDialTimeout);
        CHECK(OpsSentOn(dialer, 0) == std::vector<std::uint8_t> { WithdrawOp });
        CHECK(fix.withdrawals.empty());
    }

    SECTION("an unreachable scheduler is not walked past: one dial, and the withdrawals are dropped")
    {
        // The bound is the point: a fallback walk costs a connect timeout per configured
        // scheduler, and the machine is going to sleep in two seconds.
        Testing::ScriptedDialer dialer { { {} } };
        CHECK(WithdrawOnce(fix.Round(), link, dialer) == 0);
        CHECK(dialer.Dialed().size() == 1);
        CHECK(fix.withdrawals.empty());
    }
}

TEST_CASE("After a suspend, the next ORDINARY round registers again, with no host event at all",
          "[node][announce][host-events]")
{
    // Recovery must not depend on any power event: a Modern Standby PC may never say it resumed.
    // The retired registrars carry no id, so the first round after waking REGISTERS rather than
    // heartbeating a dead entry.
    AnnounceFixture fix;
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    RegisterThenRetireForSuspend(fix);
    Testing::ScriptedDialer before { { HeartbeatOk() } };
    REQUIRE(WithdrawOnce(fix.Round(), link, before) == 1);

    Testing::ScriptedDialer after { { RegisterOk("w-8") } };
    CHECK(AnnounceRound(fix.Round(), link, after) == 1);
    CHECK(OpsSentOn(after, 0) == std::vector<std::uint8_t> { RegisterOp });
    CHECK(fix.registrars.front().WorkerId() == "w-8");
    // WHAT registered, not only that something did: a set left in place after its registrars were
    // moved onto the withdrawal queue holds husks with no id either, and would register them --
    // under no fingerprint at all.
    CHECK(fix.registrars.front().Fingerprint() == "gcc-14");
    CHECK(fix.registrars.front().Endpoint() == ThisNode);
}

TEST_CASE("A suspend with nothing registered dials nobody", "[node][announce][host-events]")
{
    // A worker still surveying, or one whose scheduler never accepted it, has nothing to retire;
    // a dial would spend up to a second of the machine's sleep saying nothing.
    AnnounceFixture fix;
    auto link = LinkOver(SchedulersOf(fix.cfg, AsConfigured));
    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar("gcc-14"));
    RetireAllRegistrations(std::move(rebuilt), fix.registrars, fix.withdrawals);
    REQUIRE(fix.withdrawals.empty());

    // An empty script: a dial is a FAIL in the fake's own voice, not a quiet failed connect.
    Testing::ScriptedDialer dialer { {} };
    CHECK(WithdrawOnce(fix.Round(), link, dialer) == 0);
    CHECK(dialer.Dialed().empty());
}

namespace
{

/// The id of the node these cases announce as.
constexpr std::string_view SelfNode = "self-01";

/// A server trust that checks nothing: what these cases decide happens before any server is met.
class UncheckedServerTrust final: public IServerTrust
{
  public:
    /// @copydoc IServerTrust::StandingOf
    [[nodiscard]] ServerStanding StandingOf(std::string_view /*serverId*/,
                                            Ed25519PublicKey const& /*serverKey*/) const override
    {
        return ServerStanding::Unchecked;
    }

    /// @copydoc IServerTrust::Expected
    [[nodiscard]] std::string_view Expected() const override
    {
        return "any server";
    }
};

/// A consensus node's proving half: its key, and the roster its own consensus publishes into.
struct OwnRecordFixture
{
    Ed25519KeyPair const key = Testing::TestKeyPair(std::string { SelfNode });
    UncheckedServerTrust trust;
    Distributed::KeyRosterMembership ownCluster;
    NodeConditions conditions;
    Testing::ScriptedSecureRandom random { Testing::CallerHandshakeScript() };
    NodeProofClient client { std::string { SelfNode }, key, trust, &ownCluster, &conditions, random };
    CapturingLogger logger;

    /// @return How many records were captured at @p level and contain @p text.
    [[nodiscard]] std::size_t Said(LogLevel level, std::string_view text) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(logger.Snapshot(), [level, text](auto const& record) {
            return record.level == level && record.message.contains(text);
        }));
    }
};

/// An announcement no case here may reach.
class UnreachableAnnouncement final: public IAnnouncement
{
  public:
    /// @copydoc IAnnouncement::Attempt
    [[nodiscard]] AnnounceOutcome Attempt(core::net::ISocket& /*client*/, std::string_view /*endpoint*/) override
    {
        FAIL("a held round reached its announcement");
        return {};
    }
};

} // namespace

TEST_CASE("A node its own cluster has not recorded dials nothing and says so once then louder then when it clears",
          "[node][announce][self-record][conditions]")
{
    // Held rounds are the ordinary moment before an election at the start of every consensus node
    // that names --scheduler, so they are said ONCE at Info rather than per round -- and louder once
    // at Warn when they outlast what an election explains, and once more when the record lands.
    OwnRecordFixture fix;
    auto link = LinkOver({ "10.0.0.1:6674" });
    Testing::ScriptedDialer dialer { {} };
    UnreachableAnnouncement announcement;
    core::platform::ManualClock reachabilityClock;
    SchedulerReachability reachability { reachabilityClock };
    auto const proof = AnnounceProof { .prover = &fix.client };

    CHECK(fix.client.OwnRecordNow() == OwnRecord::Awaited);
    // Evaluated from construction, so `Settle` never finds the row undecided on a consensus node.
    CHECK(fix.conditions.StateOf(NodeCondition::OwnRecordAwaited) == Wire::ConditionState::Clear);
    for (auto const ask: std::views::iota(std::size_t { 0 }, OwnRecordPatience + 2))
    {
        INFO("ask " << ask);
        CHECK(DialAndAnnounce(link, reachability, dialer, fix.logger, announcement, proof) == 0);
        // Raised with the Warn, not before: the ordinary hold before an election is no condition.
        auto const raised = ask + 1 >= OwnRecordPatience;
        CHECK((fix.conditions.StateOf(NodeCondition::OwnRecordAwaited) == Wire::ConditionState::Raised) == raised);
    }
    CHECK(dialer.Dialed().empty());
    CHECK(fix.Said(LogLevel::Info, "not announcing to the fleet yet") == 1);
    CHECK(fix.Said(LogLevel::Warn, "still not announcing to the fleet") == 1);
    CHECK(fix.logger.Snapshot().size() == 2);

    Testing::PublishKeyRoster(fix.ownCluster, { std::string { SelfNode } });
    CHECK(fix.client.OwnRecordNow() == OwnRecord::Recorded);
    CHECK_FALSE(fix.client.HoldUntilRecorded(fix.logger));
    CHECK_FALSE(fix.client.HoldUntilRecorded(fix.logger));
    CHECK(fix.Said(LogLevel::Info, "has recorded it; announcing") == 1);
    CHECK(fix.conditions.StateOf(NodeCondition::OwnRecordAwaited) == Wire::ConditionState::Clear);
}

TEST_CASE("A node whose own cluster records its id under another key is held and told why at once",
          "[node][announce][self-record][conditions]")
{
    // The id this machine minted survived and its node-key did not: its cluster records the id
    // under the OLD key, so every scheduler refuses the new one -- and "not recorded yet" would send
    // an operator waiting for a record that already exists. Said at Warn on the FIRST held ask,
    // raised at once, and never repeated.
    OwnRecordFixture fix;
    auto const oldKey = Testing::TestKeyPair("self-01-before-its-key-was-replaced").PublicKey();
    fix.ownCluster.Publish({ { std::string { SelfNode }, oldKey } }, {});
    CHECK(fix.client.OwnRecordNow() == OwnRecord::OtherKey);

    CHECK(fix.client.HoldUntilRecorded(fix.logger));
    CHECK(fix.client.HoldUntilRecorded(fix.logger));
    CHECK(fix.Said(LogLevel::Warn, "under another identity key") == 1);
    CHECK(fix.Said(LogLevel::Warn, "--cluster-forget=self-01") == 1);
    CHECK(fix.Said(LogLevel::Info, "not announcing to the fleet yet") == 0);
    CHECK(fix.conditions.StateOf(NodeCondition::OwnRecordAwaited) == Wire::ConditionState::Raised);
    auto const rows = fix.conditions.Snapshot();
    auto const* const row = core::findIfOrNull(rows, [](auto const& fields) { return fields.id == "own-record-awaited"; });
    REQUIRE(row != nullptr);
    CHECK(row->detail.contains("node-key was replaced"));

    // The control: the key this machine holds, recorded, releases the round and clears the row.
    Testing::PublishKeyRoster(fix.ownCluster, { std::string { SelfNode } });
    CHECK(fix.client.OwnRecordNow() == OwnRecord::Recorded);
    CHECK_FALSE(fix.client.HoldUntilRecorded(fix.logger));
    CHECK(fix.conditions.StateOf(NodeCondition::OwnRecordAwaited) == Wire::ConditionState::Clear);
}

TEST_CASE("Only a consensus node whose own cluster has no opinion of its key is held", "[node][announce][self-record]")
{
    SECTION("a node that runs no consensus is never held: its admission is an operator's, and the scheduler says so")
    {
        auto const key = Testing::TestKeyPair(std::string { SelfNode });
        UncheckedServerTrust const trust;
        Testing::ScriptedSecureRandom random { Testing::CallerHandshakeScript() };
        NodeProofClient const client { std::string { SelfNode }, key, trust, nullptr, nullptr, random };
        CapturingLogger logger;
        CHECK(client.OwnRecordNow() == OwnRecord::NotAsked);
        CHECK_FALSE(client.HoldUntilRecorded(logger));
        CHECK(logger.Snapshot().empty());
        CHECK(NextAnnounceWait(&client) == NodeAnnounceInterval);
    }

    SECTION("a REVOKED key is an opinion: the scheduler refuses it by name, so the round is not held back")
    {
        OwnRecordFixture fix;
        Testing::PublishKeyRoster(fix.ownCluster, {}, { std::string { SelfNode } });
        CHECK(fix.client.OwnRecordNow() == OwnRecord::Recorded);
        CHECK_FALSE(fix.client.HoldUntilRecorded(fix.logger));
    }

    SECTION("another machine's record is not this one's")
    {
        OwnRecordFixture fix;
        Testing::PublishKeyRoster(fix.ownCluster, { "other-02" });
        CHECK(fix.client.OwnRecordNow() == OwnRecord::Awaited);
    }
}

TEST_CASE("A deferred proof is asked again on a short backoff that doubles up to the ordinary interval",
          "[node][announce][boot-order]")
{
    // Batch 3's M3: a scheduler that has just started cannot judge this machine for an election or
    // its leader's first word, and a whole interval of waiting is a machine nobody can use for no
    // reason. Bounded by the ordinary interval, so one that never catches up is asked no more often
    // than any other scheduler.
    CHECK(DeferredProofWait(1) == RecordAwaitedInterval);
    CHECK(DeferredProofWait(2) == 2 * RecordAwaitedInterval);
    CHECK(DeferredProofWait(3) == 4 * RecordAwaitedInterval);
    CHECK(DeferredProofWait(4) == 8 * RecordAwaitedInterval);
    CHECK(DeferredProofWait(5) == NodeAnnounceInterval);
    CHECK(DeferredProofWait(1'000'000) == NodeAnnounceInterval);
    // The arithmetic the doublings rest on, stated rather than assumed.
    STATIC_REQUIRE(8 * RecordAwaitedInterval < NodeAnnounceInterval);
    STATIC_REQUIRE(16 * RecordAwaitedInterval >= NodeAnnounceInterval);
}

TEST_CASE("Both announcing loops wait the short interval while this node's own record is awaited",
          "[node][announce][self-record]")
{
    OwnRecordFixture fix;
    auto const shortWait = std::chrono::duration_cast<std::chrono::seconds>(RecordAwaitedInterval);
    REQUIRE(shortWait < NodeAnnounceInterval);

    CHECK(NextAnnounceWait(&fix.client) == shortWait);
    CHECK(NextAnnounceWait(nullptr) == NodeAnnounceInterval);

    Testing::PublishKeyRoster(fix.ownCluster, { std::string { SelfNode } });
    CHECK(NextAnnounceWait(&fix.client) == NodeAnnounceInterval);
}
