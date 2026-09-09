// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "CompileCapacity.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

// Out of `main.cpp` since #404, and the credential is what forced it.
//
// The round used to hold a `Cc::Credential` copied once in `WorkerBody`, so a rotated
// `--requirepass` reached the shared cache and the cluster verbs and never the
// scheduler -- and the one translation unit no test can reach is where that could not
// be shown. It now holds the SEAM, which cannot go stale, and the round lives where a
// test can drive it against a scripted socket and read the bytes that went out. Both
// halves matter: the type makes the defect unwritable, the move makes the fix
// demonstrable.

/// Everything one heartbeat round reads, so the round itself is a function rather
/// than a hundred lines nested three deep inside `WorkerBody`.
///
/// References throughout: every one of these outlives the heartbeat thread, which is
/// joined by its `jthread` before any of them goes.
struct HeartbeatRound
{
    NodeConfig const& cfg;                        ///< Where the scheduler is.
    std::vector<Cc::WorkerRegistrar>& registrars; ///< One per toolchain this node serves.

    /// Registrars for toolchains this node has STOPPED serving, awaiting withdrawal.
    ///
    /// A re-survey replaces `registrars` wholesale, which throws away the very thing a
    /// withdrawal needs -- the scheduler-issued `WorkerId` of the entry being dropped.
    /// So the dropped registrars are moved here instead of destroyed, and this round
    /// retires them ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)).
    ///
    /// **Drained on every round, whatever each attempt answered.** A withdrawal that
    /// failed is not retried: the heartbeat expiry it is an optimisation over is
    /// already running, and a queue that kept retrying would outlive the fact it
    /// describes and grow without bound on a node whose scheduler is unreachable.
    std::vector<Cc::WorkerRegistrar>& withdrawals;
    CompileCapacity const& capacity; ///< For the in-flight count.
    IHostLoadSampler& loadSampler;   ///< CPU, memory and scratch.
    CacheTier const* cacheTier;      ///< Null on a node with no cache.
    IMetricsSink const& metrics;     ///< Where the cache figures are read.
    FleetSampler& sampler;           ///< This machine's own series.
    /// What this worker PRESENTS to the scheduler, asked at each exchange.
    ///
    /// The seam and never a value, which is the whole of #404 in one member
    /// declaration: a `Cc::Credential` here is a copy taken when `WorkerBody` built
    /// the round, so an operator who rotated `--requirepass` and reloaded got a
    /// worker whose cache tier presented the new secret and whose registrations went
    /// on presenting the old one -- until it was restarted, which is the thing the
    /// reload exists to avoid. There is no field here for a stale secret to sit in.
    ICredentialSource const& credential;
    /// Where the fleet this node was admitted to is recorded, so the lease check can
    /// read it. Registration is the only place that fact arrives (#401).
    Distributed::WorkerLeaseState& lease;
    /// Raised when a scheduler registers this node into a fleet other than the one
    /// `--cluster-id` asserts. Never lowered: the answer will not change by itself.
    std::atomic<bool>& fleetMismatch;
    ILogger& logger; ///< Where a refusal is named.
};

/// What one announcement learned, beyond how many entries landed.
struct AnnounceOutcome
{
    /// Registrars this scheduler accepted.
    std::size_t accepted = 0;
    /// Where it said the leader is, when it refused `NotLeader` naming somewhere
    /// usable. The caller redials rather than waiting a whole heartbeat interval:
    /// `SchedulerService::Gate()` refuses **every** verb off the leader, `Register`
    /// included, so a node that merely logged this would keep announcing itself to
    /// a demoted scheduler and expire out of the real one's registry.
    std::optional<std::string> leader;
};

/// Adopt a new served set, keeping what a withdrawal needs.
///
/// Rebuilding the registrars destroys the scheduler-issued `WorkerId` of every entry
/// being dropped, which is the one thing `Op::Withdraw` names -- so a re-survey used
/// to leave the scheduler dispatching to a fingerprint this worker had already stopped
/// serving until the entry aged out
/// ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)). The dropped
/// registrars are moved onto @p withdrawals instead of destroyed.
///
/// **A free function rather than a lambda in `WorkerBody`, and clang-tidy is what
/// said so**: inlined there it took that function's cognitive complexity to 66 against
/// a threshold of 60 and failed the build. The analyser was right for a reason beyond
/// arithmetic -- `main.cpp` is in no test target (#909), so a rule left as an
/// expression in the heartbeat loop can only be checked by reading it, and this one
/// has two call sites that must not diverge.
///
/// Only a registrar the scheduler ACCEPTED is retired: an empty `WorkerId()` means
/// there is nothing on the other end to retire and no id to name it with.
///
/// @tparam Served Anything answering `contains(std::string const&)` -- the served map.
/// @param rebuilt The registrars for the new set, already built.
/// @param served The new set, asked whether it still carries a fingerprint.
/// @param current Registrars in force; replaced by @p rebuilt.
/// @param withdrawals Where the departing registrars are appended.
template <typename Served>
void AdoptRegistrars(std::vector<Cc::WorkerRegistrar> rebuilt,
                     Served const& served,
                     std::vector<Cc::WorkerRegistrar>& current,
                     std::vector<Cc::WorkerRegistrar>& withdrawals)
{
    for (auto& registrar: current)
        if (!registrar.WorkerId().empty() && !served.contains(registrar.Fingerprint()))
            withdrawals.push_back(std::move(registrar));
    current = std::move(rebuilt);
}

/// Announce this machine to every scheduler entry it serves, once.
///
/// Registration and heartbeating are one concern: a worker is registered exactly as
/// long as it keeps saying so, and a scheduler that has forgotten it answers the
/// heartbeat by telling it to register again. Splitting them would need the two
/// halves to agree about which owns recovery.
/// @param round What to announce and where to read it from.
/// @param client A connected scheduler.
/// @param endpoint Where `client` is connected, for the diagnostics -- which must
///        name the endpoint actually reached, not the configured one, once a
///        redirect can have moved it.
/// @return What landed, and where to go next if anywhere.
[[nodiscard]] AnnounceOutcome AnnounceOnce(HeartbeatRound const& round, ISocket& client, std::string_view endpoint);

} // namespace FastCache::Node
