// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "CompileCapacity.hpp"
#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"
#include "NodeProofClient.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>
#include <core/net/ISocket.hpp>

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

/// This machine's build, as every capacity record it sends carries it -- REGISTER's
/// through the worker registrar and NODE-ANNOUNCE's through the presence loop alike.
///
/// One spelling asked from both call sites rather than each stamping `VersionString` into
/// its own `CapacityFields` in its own words, which is how a node running no worker came
/// to advertise none (#1440's second half): `NodePresenceTier` built one straight from
/// `Distributed::CapacityToWire`, which knows nothing of the version because it is derived
/// from `NodeCapacity`, a record with no version field of its own to read. See
/// `AnnouncedCapacity`, which is `NodePresenceTier`'s answer to that.
/// @return This build's version, compiled in and never configurable.
[[nodiscard]] constexpr std::string_view AdvertisedVersion() noexcept
{
    static_assert(VersionString.size() * 2 <= CompileCacheWire::MaxNodeVersionBytes,
                  "a scheduler must record twice the version this node sends");
    return VersionString;
}

/// This machine's capacity record, as NODE-ANNOUNCE carries it: `capacity`, converted, with
/// this build's version set on it.
///
/// `Distributed::CapacityToWire` alone answers for cores, memory, class and cache -- it is
/// derived from `NodeCapacity`, which has no version field -- so a caller that stopped at
/// its answer would carry an absent version, exactly the shape #1440's second half found. A
/// free function rather than a line repeated at each of `NodePresenceTier`'s call sites,
/// answering both questions together so neither can be asked without the other.
/// @param capacity What this machine is.
/// @return The wire record NODE-ANNOUNCE sends, version included.
[[nodiscard]] CompileCacheWire::CapacityFields AnnouncedCapacity(Distributed::NodeCapacity const& capacity);

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

// Out of `main.cpp` since #404, so a test can drive the round against a scripted socket and
// read the bytes that went out.
//
// It holds NO credential, and that is the fix rather than an omission. #404 gave it the
// `--requirepass` seam so a rotation reached registrations too; but a scheduler checks no
// password, and presenting one handed the upstream cache's secret, in the clear, to every
// scheduler this node dialled and every endpoint a `NotLeader` named. This machine's
// credential with a scheduler is its PROOF (`prover`), and `Cc::ExchangeWithScheduler` takes
// no credential at all.

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
    /// What this machine answers on, asked every round: the locality oracle's own set
    /// (`ILocalityOracle::Addresses`), the one this node's ticket audience and cache surface
    /// answer from.
    ///
    /// **One value with the audience, never a second acquisition.** A grant's dial hint is
    /// derived from this report, and the ticket the client mints for the hint is spent here only
    /// if the audience accepts the hint's host; reported from a probe of its own, a VPN reconnect
    /// is hinted before the audience knows it, and every hinted compile in that window is
    /// refused. Stale in either direction it fails safe -- see `CachedLocalityOracle::Addresses`.
    ///
    /// The seam and never a list, for #404's reason: a list here would be a copy taken when the
    /// round was built, and a worker whose VPN reconnected would never report the new address.
    ILocalityOracle const& locality;
    /// Set once this process has said that it answers on more addresses than a report
    /// carries. Never lowered: the machine's interface count is a property of the machine,
    /// and repeating the line every heartbeat would bury it rather than say it.
    std::atomic<bool>& addressCapNoticed;
    CacheTier const* cacheTier;  ///< Null on a node with no cache.
    IMetricsSink const& metrics; ///< Where the cache figures are read.

    /// How this machine proves WHICH machine it is on every connection the round dials (#178),
    /// or null where nothing proves -- a test whose scripted fleet serves no handshake.
    ///
    /// Never null on a node that announces itself for real: every node holds an identity key
    /// in its state directory (`NodeStateDirectory`), and every verb a joining machine sends
    /// is refused without one.
    NodeProofClient const* prover;
    /// Where the fleet this node was admitted to is recorded, so the lease check can
    /// read it. Registration is the only place that fact arrives (#401).
    Distributed::WorkerLeaseState& lease;
    ILogger& logger; ///< Where a refusal is named.
    /// How loudly a scheduler that does not answer, or refuses, is said across rounds. SHARED with
    /// the presence loop -- `main` owns one per process -- so a machine says each transition once.
    ///
    /// A registration or heartbeat is filed under its toolchain's FINGERPRINT, never under the
    /// empty subject: that place is where the presence loop's refusal lives, and a worker's success
    /// filed there would announce "recorded this machine again" about a machine still refused.
    SchedulerReachability& reachability;
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

/// Retire EVERY registration this worker holds and start over with @p rebuilt.
///
/// The suspend's counterpart of `AdoptRegistrars`: that one keeps a registration the new set
/// re-registers exactly, this one keeps none -- a sleeping machine is filed nowhere, and the
/// first round after it wakes registers afresh (the rebuilt registrars carry no id).
/// @param rebuilt Fresh registrars for the served set.
/// @param current Registrars in force; replaced by @p rebuilt.
/// @param withdrawals Where every accepted registrar is appended.
inline void RetireAllRegistrations(std::vector<Cc::WorkerRegistrar> rebuilt,
                                   std::vector<Cc::WorkerRegistrar>& current,
                                   std::vector<Cc::WorkerRegistrar>& withdrawals)
{
    for (auto& registrar: current)
        if (!registrar.WorkerId().empty())
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
[[nodiscard]] AnnounceOutcome AnnounceOnce(HeartbeatRound const& round,
                                           core::net::ISocket& client,
                                           std::string_view endpoint);

/// Ceiling on OPENING the heartbeat's connection to a scheduler, name resolution
/// included.
///
/// Short, and separate from the exchange's I/O bound: ten seconds is a reasonable
/// ceiling on an exchange and a very long time to wait for a TCP handshake. It is
/// also what a retired first `--scheduler` costs every round that starts there, which
/// is why a round's walk starts at the endpoint that last accepted (`SchedulerLink`).
inline constexpr std::chrono::milliseconds HeartbeatConnectTimeout { 1'000 };

/// Per-call send/recv ceiling on the heartbeat's own connection to the scheduler.
///
/// Was ten seconds passed as BOTH the dial bound and the I/O bound, which is the
/// collapse `Cc::DialEndpoint` used to make: ten seconds is a reasonable ceiling
/// on an exchange and a very long time to wait for a TCP handshake.
inline constexpr std::chrono::milliseconds HeartbeatIoTimeout { 10'000 };

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
/// How often a node says anything to a scheduler -- a worker's heartbeat and a machine's
/// presence announcement alike.
///
/// **ONE number rather than two that can disagree.** The scheduler's heartbeat timeout is what
/// decides when either goes stale, so a presence loop on its own cadence would make a
/// machine's fleet row expire on a schedule nothing else in this process knew about -- and the
/// two loops run in the same process against the same timeout (#1440).
constexpr std::chrono::seconds NodeAnnounceInterval { 20 };

// Strictly below the cadence a setback lasts silently at, or a machine that only ever
// announces once a cadence would see every round as due for a reminder -- "still" would
// stop meaning "still being asked" and start meaning "asked once, a while ago".
static_assert(NodeAnnounceInterval < SchedulerUnreachableCadence);

/// The load record describing this MACHINE, sampled once.
///
/// Extracted rather than copied because both announcements describe ONE host: sampling twice in
/// a process gives two readings a moment apart that disagree, and the CPU figure is a
/// DIFFERENCE between successive readings, so a second sampler reports nothing meaningful at
/// all rather than something slightly stale.
///
/// **It carries no history, and that absence is the design rather than an omission.** History
/// is filed under the MACHINE, so it rides NODE-ANNOUNCE -- the one verb every node sends,
/// including one that runs no worker and therefore never registers anything to hang it on
/// ([#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440)). Two callers of
/// `FleetSampler::NextHistoryBatch` would each take a batch and each advance the cursor, so the
/// buckets would be split between two verbs and a batch taken by one and acknowledged by the
/// other would be lost. One carrier, and it is the one that always exists.
/// @param loadSampler Where the host figures come from.
/// @param cacheTier Null on a node with no cache.
/// @param metrics Where the cache figures are read.
/// @param inFlight Jobs this node is running for the fleet.
/// @param cordoned Whether this node is refusing new work.
/// @return The wire record, with an empty history.
[[nodiscard]] CompileCacheWire::LoadFields SampleMachineLoad(IHostLoadSampler& loadSampler,
                                                             CacheTier const* cacheTier,
                                                             IMetricsSink const& metrics,
                                                             std::uint32_t inFlight,
                                                             bool cordoned);

/// Why an interface address is left out of what a worker reports. PRIVATE: never
/// transmitted, never persisted.
enum class UnreportedAddress : std::uint8_t
{
    Uncarried,
    Loopback,
    LinkLocal,
    Last,
};

/// One reason to leave an address out, and the predicate that raises it.
struct UnreportedAddressRow
{
    UnreportedAddress reason;
    /// @param address One entry as `IHostAddressSource` spells it. @return True to leave it out.
    bool (*applies)(std::string_view address);
};

/// What a report leaves out, every row asked of every entry.
///
/// Each row is an address the scheduler could never hand a client as a dial hint, so
/// carrying it would only spend the `MaxInterfaceAddresses` budget that a routable address
/// needs on a machine with many virtual adapters:
///   - **Uncarried** is the wire's own per-entry rule, `IsCarriedInterfaceAddress`, asked
///     rather than restated: an entry the decoder refuses loses the WHOLE record it rides in.
///   - **Loopback** is never hinted: a peer observed there is on the scheduler's machine.
///   - **LinkLocal** is vetoed by the scheduler (`HintVeto::LinkLocalObserved`), because a
///     zone-less `fe80::` names a different machine on every link.
inline constexpr EnumTable<UnreportedAddress, UnreportedAddressRow> UnreportedAddresses { {
    { .reason = UnreportedAddress::Uncarried,
      .applies = [](std::string_view address) { return !CompileCacheWire::IsCarriedInterfaceAddress(address.size()); } },
    { .reason = UnreportedAddress::Loopback, .applies = [](std::string_view address) { return IsLoopbackHost(address); } },
    { .reason = UnreportedAddress::LinkLocal, .applies = [](std::string_view address) { return IsLinkLocalHost(address); } },
} };

static_assert(RowsInEnumeratorOrder(UnreportedAddresses, &UnreportedAddressRow::reason),
              "UnreportedAddresses must hold one row per UnreportedAddress, in order");

/// The addresses one report carries, and how many the cap left out.
struct ReportableAddresses
{
    /// What survives `UnreportedAddresses`, sorted, unique, at most `MaxInterfaceAddresses`.
    std::vector<std::string> addresses;
    /// Entries that survived every row and were dropped by the cap; zero on almost every
    /// machine, and the number a person needs to read when it is not.
    std::size_t overCap = 0;
};

/// The addresses a worker reports on REGISTER and HEARTBEAT, out of what the machine has.
///
/// Filtered before the cap, so a host with thirty loopback aliases still reports its one
/// routable address. **Sorted**, so equal sets encode to equal bytes whatever order the
/// platform enumerated them in, and so that which entries a cap drops is decided by the
/// set rather than by the adapter order of the moment. An empty input -- the platform would
/// not say -- is an empty report, which the scheduler reads as no hint and never as a
/// refusal.
/// @param addresses What `IHostAddressSource` answered, unordered and possibly repeated.
/// @return The report, and how many the cap left out.
[[nodiscard]] ReportableAddresses ReportableInterfaceAddresses(std::vector<std::string> addresses);

/// One announcement, on a connection somebody else dialled.
///
/// **The seam that lets a node with NO WORKER reach the fleet.** Everything about *which*
/// scheduler to talk to -- the `--scheduler` list walked at most once per round, a `NotLeader`
/// followed to the endpoint it names, a remembered leader that stops answering falling back in
/// the SAME round, the bound that stops two nodes naming each other forever -- lives in
/// `DialAndAnnounce` below and must live in exactly one place. A second loop with its own copy
/// of those rules is the shape this repository keeps paying for, and the rules would then have
/// two answers depending on whether the node happened to run a worker
/// ([#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440)).
///
/// So what varies is only what is SAID once a connection exists, which is this.
class IAnnouncement
{
  public:
    IAnnouncement() = default;
    IAnnouncement(IAnnouncement const&) = delete;
    IAnnouncement(IAnnouncement&&) = delete;
    IAnnouncement& operator=(IAnnouncement const&) = delete;
    IAnnouncement& operator=(IAnnouncement&&) = delete;
    virtual ~IAnnouncement() = default;

    /// Say it, on this connection.
    /// @param client The dialled connection.
    /// @param endpoint Where it was dialled, for the messages that name it.
    /// @return What was learned: how many entries landed, and any leader redirect.
    [[nodiscard]] virtual AnnounceOutcome Attempt(core::net::ISocket& client, std::string_view endpoint) = 0;
};

/// How a round proves this machine on each connection it dials (#178).
///
/// A pointer, because the proof is absent only where nothing proves -- a test's scripted fleet --
/// and a null reference is not a thing. No password rides beside it: the proof IS this machine's
/// credential with a scheduler.
struct AnnounceProof
{
    NodeProofClient const* prover; ///< Who this machine is; null where nothing proves.
};

/// Dial a scheduler, prove this machine to it, and make @p announcement, following a redirect
/// and falling back.
///
/// The rules named on `IAnnouncement`, applied once. Callers differ only in what they say.
///
/// **The proof is part of reaching a scheduler, not of what is said**, so both announcements
/// prove through the same lines (#178): a connection the proof did not seal is one on which no
/// joining verb can be heard, and it is treated exactly as an endpoint that did not answer --
/// the next `--scheduler` is tried in the same round.
/// @param link Which endpoint to try, and what an answer teaches it.
/// @param reachability How loudly an endpoint that does not answer, or refuses the proof, is said:
///        on the transition, then on a cadence, never per round. One per process, shared by every loop.
/// @param dialer How a connection is made.
/// @param logger Where an unreachable endpoint, a refused proof and a redirect are named.
/// @param announcement What to say.
/// @param proof How this machine proves itself on each connection.
/// @return How many entries the endpoint that answered accepted.
[[nodiscard]] std::size_t DialAndAnnounce(SchedulerLink& link,
                                          SchedulerReachability& reachability,
                                          IEndpointDialer& dialer,
                                          ILogger& logger,
                                          IAnnouncement& announcement,
                                          AnnounceProof const& proof);

[[nodiscard]] std::size_t AnnounceRound(HeartbeatRound const& round, SchedulerLink& link, IEndpointDialer& dialer);

/// The connect bound of the one dial a suspend makes; inside `SuspendWithdrawBudget`.
inline constexpr std::chrono::milliseconds SuspendDialTimeout { 1'000 };

/// Withdraw what @p round queued, at the endpoint @p link names, in ONE dial. Never registers,
/// heartbeats, follows a redirect or falls back: the machine is about to sleep. A failure is logged
/// and the queue dropped -- expiry closes the rest. With nothing queued nothing is dialled.
///
/// **Only the CONNECT is bounded by `SuspendDialTimeout`.** The exchange after it -- the proof and
/// the withdrawals -- is bounded by @p dialer's own I/O ceiling (`HeartbeatIoTimeout` on the
/// heartbeat's), not by `SuspendWithdrawBudget`. That is safe rather than merely tolerated: the
/// suspend handler stops waiting at its budget whatever this is doing, a withdrawal the sleep
/// freezes leaves the entry to the scheduler's expiry exactly as before, and one that finishes
/// after the machine wakes only un-files it until the next round, which registers afresh because
/// the rebuilt registrars carry no id. `core::net::DialOptions` has no per-dial I/O bound, so a
/// tighter one would need a second dialer.
///
/// **Deliberately NOT through `round.reachability`**: a machine going to sleep is not a
/// scheduler outage. Counted there, a failed pre-sleep dial would spend that scheduler's Warn
/// and then announce a false *reachable again* at wake, so both failures are said here at Info.
/// @param round What to withdraw and where to log.
/// @param link Names the endpoint; read, never advanced.
/// @param dialer How the connection is made.
/// @return How many registrations the scheduler retired.
[[nodiscard]] std::size_t WithdrawOnce(HeartbeatRound const& round, SchedulerLink const& link, IEndpointDialer& dialer);

} // namespace FastCache::Node
