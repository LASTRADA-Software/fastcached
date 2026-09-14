// SPDX-License-Identifier: Apache-2.0
//
// What a re-survey owes the scheduler when a toolchain goes away
// ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)).
//
// `AdoptRegistrars` exists as a free function because clang-tidy refused it as a
// lambda inside `WorkerBody` -- cognitive complexity 66 against a threshold of 60 --
// and the analyser was right for a reason beyond arithmetic: `main.cpp` is in no test
// target (#909), so the rule *replacing the served set retires what left it* could
// only be checked by reading, at two call sites that must not diverge. This file is
// what the extraction bought.
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
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <tests/ScriptedSocket.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{
namespace Wire = FastCache::CompileCacheWire;

/// A registrar for @p fingerprint, optionally already accepted by a scheduler.
///
/// The id is what a withdrawal NAMES, so whether it is set is the whole of the
/// second clause under test rather than incidental setup.
/// @param notice Where an unchecked credential would be reported.
/// @param fingerprint The toolchain it announces.
/// @return The registrar, never registered.
[[nodiscard]] Cc::WorkerRegistrar Registrar(Cc::CredentialNotice& notice, std::string fingerprint)
{
    return Cc::WorkerRegistrar { notice, std::move(fingerprint), "10.0.0.2:6677",
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

/// A framed REGISTER reply accepting a worker under @p workerId.
/// @param workerId The id to assign.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> RegisterOk(std::string_view workerId)
{
    auto const payload =
        Wire::EncodeRegisterReply({ .workerId = std::string { workerId }, .clusterId = "fleet-a", .epoch = 1 });
    return Wire::EncodeReply(Wire::Status::Ok, payload);
}

/// A served set with the given fingerprints, values unused by the rule under test.
/// @param fingerprints What this node serves now.
/// @return Something answering `contains`.
[[nodiscard]] std::map<std::string, int> Served(std::vector<std::string> const& fingerprints)
{
    std::map<std::string, int> served;
    for (auto const& fingerprint: fingerprints)
        served.emplace(fingerprint, 0);
    return served;
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
    AdoptRegistrars(std::move(rebuilt), Served({ "gcc-14", "clang-20" }), current, withdrawals);

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
    AdoptRegistrars(std::move(rebuilt), Served({ "clang-20" }), current, withdrawals);

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
    AdoptRegistrars(std::move(rebuilt), Served({ "clang-20" }), current, withdrawals);

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
    AdoptRegistrars({}, Served({}), current, withdrawals);

    CHECK(current.empty());
    CHECK(withdrawals.empty());
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
    FleetSampler sampler { std::nullopt,
                           metrics,
                           [] {
                               return MetricsSnapshot { .storage = std::nullopt,
                                                        .storageTiers = {},
                                                        .host = HostCapacity { .configuredSlots = 1, .busySlots = 0 },
                                                        .upstreamConfigured = std::nullopt,
                                                        .uptime = {} };
                           },
                           DefaultSystemWallClock(),
                           HistoryPaths {},
                           logger };
    CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 1 }, logger };
    ConfiguredCredential credential { cfg, nullptr };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::atomic<bool> fleetMismatch { false };
    Cc::CredentialNotice notice = Cc::CredentialNotice::Silent();
    std::vector<Cc::WorkerRegistrar> registrars;
    std::vector<Cc::WorkerRegistrar> withdrawals;

    AnnounceFixture()
    {
        cfg.scheduler = "scheduler.example:6676";
        registrars.push_back(Registrar(notice, "gcc-14"));
    }

    /// Announce once to a scheduler answering @p replies.
    /// @param replies What the scheduler says, in order.
    /// @return Every request the round sent.
    [[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> AnnounceTo(std::vector<std::byte> replies)
    {
        HeartbeatRound const round { .cfg = cfg,
                                     .registrars = registrars,
                                     .withdrawals = withdrawals,
                                     .capacity = capacity,
                                     .loadSampler = loadSampler,
                                     .cacheTier = nullptr,
                                     .metrics = metrics,
                                     .sampler = sampler,
                                     .credential = credential,
                                     .lease = lease,
                                     .fleetMismatch = fleetMismatch,
                                     .logger = logger };
        Testing::ScriptedSocket scheduler { std::move(replies) };
        (void) AnnounceOnce(round, scheduler, cfg.scheduler);
        return FramesIn(scheduler.Sent());
    }
};

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
