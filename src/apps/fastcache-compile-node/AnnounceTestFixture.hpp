// SPDX-License-Identifier: Apache-2.0
#pragma once

// One heartbeat round over collaborators a case controls -- shared by the scripted announce cases
// (`NodeAnnounce_test.cpp`) and the ones that announce to a real proving endpoint
// (`FrameEndpoint_test.cpp`), so the round both drive is built in one place.

#include "CompileCapacity.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeProofClient.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/Unwrap.hpp>

namespace FastCache::Node::AnnounceTesting
{

/// The address a node in these cases advertises.
inline constexpr std::string_view ThisNode = "10.0.0.2:6677";

/// A registrar for @p fingerprint, optionally already accepted by a scheduler.
///
/// The id is what a withdrawal NAMES, so whether it is set is the whole of the
/// second clause under test rather than incidental setup.
/// @param fingerprint The toolchain it announces.
/// @param endpoint The address it announces, defaulted because only the endpoint cases
///        vary it -- and it is the other half of the key the adoption rule reads.
/// @return The registrar, never registered.
[[nodiscard]] inline Cc::WorkerRegistrar Registrar(std::string fingerprint, std::string_view endpoint = ThisNode)
{
    return Cc::WorkerRegistrar { std::move(fingerprint),
                                 std::string { endpoint },
                                 1U,
                                 CompileCacheWire::CodecList {},
                                 CompileCacheWire::CapacityFields {} };
}

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
[[nodiscard]] inline std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> FramesIn(std::span<std::byte const> sent)
{
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> frames;
    while (sent.size() >= CompileCacheWire::RequestHeaderSize)
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(sent);
        if (!header.has_value())
            break;
        auto const whole = CompileCacheWire::RequestHeaderSize + std::size_t { header->payloadLength };
        if (sent.size() < whole)
            break;
        auto const payload = sent.subspan(CompileCacheWire::RequestHeaderSize, header->payloadLength);
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
    CapturingLogger logger;
    core::platform::ManualClock clock;
    NodeConditions conditions;
    SchedulerReachability reachability { clock, &conditions };
    SilentLoadSampler loadSampler;
    /// What this machine answers on: one of each thing a report leaves out, a repeat, and two
    /// routable addresses listed out of order.
    Testing::ScriptedHostAddresses addresses {
        { "127.0.0.1", "192.168.1.20", "::1", "fe80::1", "169.254.3.4", "10.8.0.7", "10.8.0.7" }
    };
    /// What the round reports FROM: the production oracle over `addresses`, as `main` builds it,
    /// so a change to the machine reaches a report only at the oracle's refresh (`Moved`).
    CachedLocalityOracle const locality { addresses, clock };
    std::atomic<bool> addressCapNoticed { false };
    // The process singleton wall clock, for the reason `NodeCredential_test` gives beside
    // the same construction: the sampler keeps the ADDRESS and reads it from its own thread.
    CompileCapacity capacity { /*slots=*/1, /*byteBudget=*/1024ULL, std::chrono::seconds { 1 }, logger };
    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    std::vector<Cc::WorkerRegistrar> registrars;
    std::vector<Cc::WorkerRegistrar> withdrawals;
    /// How the round proves this machine. Null for the scripted cases, which are about the announce
    /// round itself against a fleet that serves no handshake (#178); a case against a real
    /// endpoint (`FrameEndpoint_test`'s `ProvingFleet`) points it at a `NodeProofClient`.
    NodeProofClient const* prover { nullptr };

    /// The machine now answers on @p now, and the oracle's interval has passed, so its next
    /// question refreshes -- as a VPN reconnect reaches a report in production.
    /// @param now The machine's addresses from here on.
    void Moved(std::vector<std::string> now)
    {
        addresses.Publish(std::move(now));
        clock.advance(CachedLocalityOracle::DefaultRefreshInterval);
    }

    AnnounceFixture()
    {
        cfg.schedulers = { "scheduler.example:6676" };
        registrars.push_back(Registrar("gcc-14"));
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
                                .locality = locality,
                                .addressCapNoticed = addressCapNoticed,
                                .cacheTier = nullptr,
                                .metrics = metrics,
                                .prover = prover,
                                .lease = lease,
                                .logger = logger,
                                .reachability = reachability };
    }

    /// Announce once to a scheduler answering @p replies.
    /// @param replies What the scheduler says, in order.
    /// @return Every request the round sent.
    [[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> AnnounceTo(std::vector<std::byte> replies)
    {
        FastCache::Testing::ScriptedSocket scheduler { std::move(replies) };
        (void) AnnounceOnce(Round(), scheduler, cfg.schedulers.front());
        return FramesIn(scheduler.Sent());
    }
};

/// The link a worker configured with @p schedulers holds.
/// @param schedulers A non-empty `--scheduler` list.
/// @return The link; the case fails when none could be built.
[[nodiscard]] inline SchedulerLink LinkOver(std::vector<std::string> const& schedulers)
{
    auto const link = SchedulerLink::For(schedulers);
    REQUIRE(link.has_value());
    return FastCache::Testing::Unwrap(link);
}

} // namespace FastCache::Node::AnnounceTesting
