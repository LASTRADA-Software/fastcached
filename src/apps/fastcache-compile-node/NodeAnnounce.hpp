// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "CompileCapacity.hpp"
#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// The endpoint this worker is announcing, now.
///
/// The production `Cc::IAdvertisedEndpointSource`, and the one object the registration
/// and the lease check both read -- which is what makes *the endpoint the scheduler
/// signs and the endpoint this worker verifies are one fact* a property of the type
/// system rather than a sentence in `main`.
///
/// ## Why this is not `ConfiguredCredential`'s shape
///
/// `Node::ConfiguredCredential` answers from the live configuration snapshot on every
/// call, and that is right for a secret: a rotation should reach every presentation the
/// instant the operator's file is accepted, and nothing downstream has to be told.
///
/// An endpoint is not like that, because a second party holds a copy. The scheduler
/// files this worker under `(fingerprint, endpoint)` and signs that endpoint into every
/// grant, so a value that changed the moment a snapshot was published would leave this
/// worker refusing authentic grants for the address the fleet still has -- and it would
/// do so for however long the heartbeat interval is, with `LeaseEndpointMismatch`
/// rising and no configuration anywhere being wrong. Reading the snapshot per call is
/// the shape that looks most correct and is the one that breaks.
///
/// So the value changes at ONE point: the heartbeat thread publishes it as part of
/// re-announcing, after the old registration has been queued for withdrawal and the new
/// registrars built. `WorkerTier::AnnounceAs` is that point, and it is the only caller
/// of `Publish`.
///
/// ## The window this still has, stated rather than hidden
///
/// A grant signed for the old endpoint and presented after the change is refused, since
/// one endpoint is expected at a time. That is bounded by the grant's own lifetime,
/// counted (`LeaseEndpointMismatch`), and costs the client a local compile -- against
/// which the alternative, accepting any recently advertised address, widens the window
/// in which a captured token is replayable at this worker. Widening a replay window to
/// save a handful of dispatches is a decision this ticket does not get to make quietly.
class AnnouncedEndpoint final: public Cc::IAdvertisedEndpointSource
{
  public:
    /// @param startup What this worker was started advertising -- `AdvertisedEndpoint`
    ///        of the configuration it came up with, so the first registration and every
    ///        lease check before any reload read exactly what `main` reported.
    explicit AnnouncedEndpoint(std::string_view startup):
        _endpoint { startup }
    {
    }

    /// @copydoc Cc::IAdvertisedEndpointSource::Current
    [[nodiscard]] std::string Current() const override
    {
        auto const lock = std::scoped_lock { _mutex };
        return _endpoint;
    }

    /// Make @p endpoint what this worker advertises from now on.
    ///
    /// Called by the heartbeat thread alone, and only alongside the re-registration
    /// that tells the scheduler -- see the class comment for why this is not a setter
    /// anybody may reach for.
    /// @param endpoint The new endpoint; must be non-empty, since nothing can be
    ///        registered under an empty one.
    void Publish(std::string endpoint)
    {
        // A precondition rather than a refusal, because an empty endpoint here is a
        // programmer error and not a configuration: `AdvertisedEndpointChange` is what
        // decides, and it answers nullopt for an empty candidate. Publishing one would
        // withdraw every registration and re-register under no address at all -- a node
        // that disappears from the fleet with every counter reading normal.
        assert(!endpoint.empty());
        auto const lock = std::scoped_lock { _mutex };
        _endpoint = std::move(endpoint);
    }

  private:
    /// A mutex rather than an atomic pointer, because the readers are a compile thread
    /// per job and the writer is one thread per heartbeat interval: the contention is
    /// nil and the cost is paid beside a process spawn.
    mutable std::mutex _mutex;
    std::string _endpoint;
};

/// A move of the endpoint this worker advertises: the new value, and what to say.
///
/// Two strings, named, rather than a pair: both halves are text and a `.first` deciding
/// what gets REGISTERED while `.second` only gets logged is a positional contract with
/// no compiler behind it.
struct EndpointChange
{
    /// What to advertise from now on. Never empty.
    std::string endpoint;

    /// The line to log, naming both addresses -- an operator reading only the new one
    /// cannot tell a change from a restart.
    std::string announcement;
};

/// Whether the endpoint this worker advertises has moved, and what to say about it.
///
/// A pure function over the two facts for `RecheckDepthFor`'s reason: the heartbeat
/// loop is reached by no test, so a rule left as an expression there can only be
/// checked by reading it -- and this one is wrong silently in both directions. Missed,
/// the fleet keeps leasing an address nobody answers; fired spuriously, every worker in
/// a fleet re-registers because somebody saved a file.
///
/// **Compares the DERIVED endpoint, never the `--advertise` field.**
/// `AdvertisedEndpoint` folds the flag with the `Node` surface's resolved address, and
/// what the scheduler keys on is the result -- so a save that clears a flag whose value
/// equalled the fallback has changed nothing to announce, and the row-level comparison
/// the reload machinery uses (`AddressReloadableFlags`) would call it a change. The
/// classification list is what forces the decision at the table; this is what decides
/// whether anything happens.
///
/// @param inForce The endpoint being advertised now.
/// @param live The configuration in force this beat, or null when this worker has no
///        configuration file and therefore no second moment at which anything could
///        change.
/// @return The new endpoint and the line to log, or nullopt when nothing moved.
[[nodiscard]] std::optional<EndpointChange> AdvertisedEndpointChange(std::string_view inForce,
                                                                     std::shared_ptr<NodeConfig const> const& live);

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
    CompileCapacity const& capacity; ///< For the in-flight count and the cordon.
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
/// **A registration is retired unless the new set re-registers exactly it, and
/// "exactly" is `(fingerprint, endpoint)` -- the registry's own key.** This asked
/// whether the served map still contained the fingerprint, which is the right question
/// for the case it was written for and half a key: a node that changes the address it
/// advertises rebuilds every registrar with the SAME fingerprints, so nothing was ever
/// retired and the scheduler kept an entry naming an address this worker no longer
/// answers on -- handing out leases whose MAC covers it
/// ([#1279](https://github.com/LASTRADA-Software/fastcached/issues/1279)).
///
/// Derived from @p rebuilt rather than from the served map, which is what deletes the
/// `Served` parameter: @p rebuilt IS the new set, one registrar per served fingerprint
/// carrying the endpoint now in force, so asking it about a pair answers both halves at
/// once. Asking the map could only ever answer one.
///
/// @param rebuilt The registrars for the new set, already built.
/// @param current Registrars in force; replaced by @p rebuilt.
/// @param withdrawals Where the departing registrars are appended.
inline void AdoptRegistrars(std::vector<Cc::WorkerRegistrar> rebuilt,
                            std::vector<Cc::WorkerRegistrar>& current,
                            std::vector<Cc::WorkerRegistrar>& withdrawals)
{
    auto const reRegistered = [&rebuilt](Cc::WorkerRegistrar const& was) {
        return std::ranges::any_of(rebuilt, [&was](Cc::WorkerRegistrar const& is) {
            return is.Fingerprint() == was.Fingerprint() && is.Endpoint() == was.Endpoint();
        });
    };

    for (auto& registrar: current)
        if (!registrar.WorkerId().empty() && !reRegistered(registrar))
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

/// Ceiling on OPENING the heartbeat's connection to a scheduler, name resolution
/// included.
///
/// Short, and separate from the exchange's I/O bound: ten seconds is a reasonable
/// ceiling on an exchange and a very long time to wait for a TCP handshake. It is
/// also what a retired first `--scheduler` costs every round that starts there, which
/// is why a round's walk starts at the endpoint that last accepted (`SchedulerLink`).
inline constexpr std::chrono::milliseconds HeartbeatConnectTimeout { 1'000 };

/// Announce this machine once, following `NotLeader` to wherever it points and falling
/// back through the configured `--scheduler` list when an endpoint cannot be reached.
///
/// Out of `main.cpp` for #1310, whose acceptance is a FALLBACK: a first scheduler that
/// does not answer and a second that does, asserted by which one took the request.
/// `main.cpp` is in no test target (#909), and a case that lists two endpoints and
/// finds the first tried passes under every defect this could have -- one value always
/// worked. Here the dial is a seam and the replies are scripted.
///
/// Every *decision* still belongs to `SchedulerLink`, which is pure and tested:
/// which endpoint, whether the chain is spent, whether to fall back, what to
/// remember. What lives here is the dialling, and the logging of what the link
/// decided.
///
/// A free function rather than a block inside `WorkerBody` for a second reason as
/// well: that function sits at the cognitive-complexity ceiling the build enforces --
/// this loop pushed it to 88 against a threshold of 60 when it was first written.
/// @param round What to announce and where to read it from.
/// @param link Where this node believes the leader is; advanced across the round.
/// @param dialer Dials each endpoint the link names.
/// @return How many entries a scheduler ACCEPTED this round. Zero covers every way a
///         round can achieve nothing -- nobody reachable, everybody refusing, a
///         redirect chain that ran out -- which are one answer to the only question
///         the caller asks of it: is this node getting through to a scheduler.
[[nodiscard]] std::size_t AnnounceRound(HeartbeatRound const& round, SchedulerLink& link, IEndpointDialer& dialer);

} // namespace FastCache::Node
