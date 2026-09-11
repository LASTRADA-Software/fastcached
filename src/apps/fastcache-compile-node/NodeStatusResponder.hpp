// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

struct NodeConfig;

/// @file NodeStatusResponder.hpp
/// The operator verbs: what this node IS, and what its own counters say.
///
/// ## Why this is a component of its own
///
/// Every other family names something a node may or may not run -- a cache tier, a
/// worker, a scheduler -- and these verbs must not depend on **which** of them is
/// running. A worker with no cache tier and no scheduler is an ordinary deployment, and
/// *what do you serve?* has to work there; filing these under any one component would
/// make the answer depend on that component being present. So `MergedResponder` gains a
/// fourth owner rather than a special case, and on a built node this owner is never null
/// the way the other three legitimately are.
///
/// **Not** *answerable by a node running nothing*: `StartNodeSurfaceOrExplain` serves no
/// `0xFC` port at all in that case, and this responder is deliberately not one of the
/// conditions it tests -- including it would make that guard unsatisfiable and open a
/// port on a node that serves nothing.
///
/// ## Why it exists at all
///
/// `fastcache-compile-node` speaks `0xFC` and nothing else, so a client pointed at one
/// got a silent close and no way to find out why. Measured against the running node:
/// RESP `PING`, RESP `INFO` and memcached `version` each closed having sent nothing,
/// while a `0xFC` header got a proper refusal. `fastcache-cli version` therefore reported
/// *the server closed the connection without answering* -- true, and useless.

/// What this node knows about itself.
///
/// A seam, because every field of it is ambient: the version is compiled in, the surfaces
/// come from the resolved configuration, the components from which tiers actually started,
/// and the uptime from a clock. A responder that reached for any of those directly would
/// be untestable by construction, which is the whole reason this interface exists rather
/// than a struct built in `main`.
class INodeStatusSource
{
  public:
    INodeStatusSource() = default;
    INodeStatusSource(INodeStatusSource const&) = delete;
    INodeStatusSource(INodeStatusSource&&) = delete;
    INodeStatusSource& operator=(INodeStatusSource const&) = delete;
    INodeStatusSource& operator=(INodeStatusSource&&) = delete;
    virtual ~INodeStatusSource() = default;

    /// This node, as the wire reports it.
    ///
    /// Asked per request rather than captured once: uptime moves, and a tier can finish
    /// starting after the surface is already serving -- a node SERVES while it identifies
    /// its toolchains, so a snapshot taken at construction would report a worker that has
    /// no fingerprint yet as running no worker at all.
    /// @return The fields to encode.
    [[nodiscard]] virtual CompileCacheWire::NodeStatusFields Describe() const = 0;
};

/// Answers `NodeStatus` and `NodeMetrics`.
///
/// **Gated on membership, not on locality.** The cache tier is served to this machine
/// only because it IS this machine's build output; these verbs report configuration
/// rather than hand anything over, and an operator diagnosing a fleet is by construction
/// not sitting on every node in it. Membership is the same posture the cluster verbs
/// take, and it is what `--fleet-member` is for.
///
/// It still reports a **port map**, which is worth saying out loud rather than treating
/// as harmless: that is why the gate is here at all rather than the verbs being
/// pre-auth. The `OpTable` rows say `RequiresAuth`, so a credentialled surface demands
/// one as well.
class NodeStatusResponder final: public IFrameResponder
{
  public:
    /// @param identity What this node knows about itself; must outlive this.
    /// @param membership Who may ask; must outlive this. Bound once, by reference,
    ///        the way every surface binds it -- an implementation that re-asked for
    ///        an oracle per request could never see `--fleet-open` change, and a test
    ///        that re-acquired it would pass under exactly that defect.
    /// @param metrics Where a refusal is counted; must outlive this.
    NodeStatusResponder(INodeStatusSource const& identity,
                        Distributed::IMembershipOracle const& membership,
                        IMetricsSink& metrics) noexcept:
        _identity { identity },
        _membership { membership },
        _metrics { metrics }
    {
    }

    /// @copydoc IFrameResponder::Answer
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// The one implementation of the rule, called by `Answer` as well as by the door, so
    /// the early refusal and the authoritative one cannot disagree and the counter moves
    /// exactly once per refused request whichever path reached it.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No, and membership is the whole gate.** Three reasons, and the first is the
    /// one that decides it: the credential on this listener belongs to the SCHEDULER --
    /// `MergedResponder` routes `CheckCredential` there -- so a node running no
    /// scheduler has none to check. `NoPolicy` answers `Ok` and marks NOTHING, so
    /// answering `true` here would leave every operator verb permanently
    /// `Unauthenticated` on a plain worker, which is the deployment these verbs exist
    /// for. A surface must not require a secret it cannot verify.
    ///
    /// Second, what is handed over is strictly less than what this node already serves
    /// unauthenticated: `/metrics` short-circuits above `AdminHttpServer`'s route loop
    /// and needs no `AdminCredential` at all, and the port map is what
    /// `--print-surfaces` prints. Gating on membership makes the `0xFC` route the
    /// STRICTER of the two rather than a new door.
    ///
    /// Third, #289's argument does not transfer: it is about verbs that spend this
    /// machine's CPU or hand over its objects, and these spend a `Describe()` and one
    /// walk of the counter table.
    ///
    /// **The `RequiresAuth` in the `OpTable` rows is a different question** and is not
    /// contradicted. That column says these verbs are not on the pre-auth allowlist, so
    /// on a surface that DOES hold a policy they wait for one; this answers whether
    /// this surface holds one.
    ///
    /// Verb-blind, like every sibling below: `MergedResponder` routes by verb FAMILY,
    /// so every verb arriving here is a node verb, and answering per-verb would restate
    /// the family table somewhere it could disagree with itself.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy` for every payload, per `AuthRequired` above. Unreachable in practice
    /// -- `AUTH` is a `Session` verb and `MergedResponder` sends the credential to the
    /// scheduler -- and written down rather than inherited, for the reason the
    /// interface is pure virtual: a surface that inherits an answer inherits an open
    /// door by saying nothing.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(nullptr, payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The endpoint's own header window. These verbs read nothing and compute nothing:
    /// `Describe()` resolves a handful of surface rows and `node-metrics` walks a fixed
    /// table, so a window sized for a cache round trip is already generous. Anything
    /// longer would hand a slow-loris the compile window for a verb that cannot use it,
    /// which is the trade `MergedResponder::RequestTimeout` declines to make
    /// surface-wide.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    ///
    /// The control cap, which is what the `OpTable` rows already bound these verbs to.
    /// Both are FIELDLESS, so the honest per-verb answer is smaller still -- and stating
    /// that here would be wrong rather than tight: this is the SESSION cap the endpoint
    /// folds across owners with `Largest`, and `OpPayloadCap` is what applies the
    /// per-verb bound. A surface claiming less than it can serve narrows nothing today
    /// and would narrow a payload-bearing sibling the day this is the only owner.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload;
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// Modest, and deliberately not the cache's hundreds: an operator tool opens one
    /// connection and closes it. Folded with `Largest`, so it cannot narrow a busier
    /// sibling's ceiling -- what it decides is the ceiling on a node serving nothing
    /// else, where a diagnostic port with hundreds of descriptors is a port worth
    /// exhausting.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 32;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    ///
    /// A handful of control payloads. Folded with `Largest`, so on any node holding a
    /// tier the cache's budget governs and this contributes nothing; it matters only
    /// where this is the largest owner, and there the right budget is the one these
    /// verbs can actually spend.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload * 16;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **No.** There is no per-verb reservation here the way `CompileCapacity` charges
    /// a declared footprint, so the endpoint keeps its own. `false` is the answer that
    /// accounts for a buffer rather than the one that assumes somebody else will.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Not watched.** A watch exists for a verb that runs long enough for the client
    /// to leave while nothing reads the socket -- #223 measured a compile at 84 MB
    /// written into a socket nobody was reading. These answer in microseconds with
    /// kilobytes, so a watch would cost an arm and a counter to catch nothing.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// **Not pulsed**, the exact converse of `PeerWatchCounter` above: a pulse exists so
    /// a client can tell a working server from a stopped one across minutes of silence,
    /// and there is no silence here to interpret.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

  private:
    INodeStatusSource const& _identity;
    Distributed::IMembershipOracle const& _membership;
    IMetricsSink& _metrics;
};

/// What this node runs, as `main` observes it.
///
/// A plain record rather than four constructor parameters, so a call site that learns a
/// fifth component adds a field instead of an argument nobody can tell from its
/// neighbours -- every one of these is a `bool`.
struct NodeComponents
{
    bool cacheTier { false }; ///< A cache tier started.
    bool worker { false };    ///< This node compiles.
    bool scheduler { false }; ///< This node schedules.
    bool consensus { false }; ///< This node participates in Raft.
};

/// What one reading says about this worker's toolchain survey.
///
/// **The three travel together or they lie.** Read separately, a reader can catch
/// `Serving` beside a count from the previous publication -- and the count is the whole
/// of what `Surveying` means (*n of m*), so a torn pair is not a slightly stale reading
/// but a sentence with two subjects.
struct ToolchainReading
{
    /// How far the survey has got.
    CompileCacheWire::ToolchainState state { CompileCacheWire::ToolchainState::Surveying };

    /// How many toolchains are served under that state.
    std::uint32_t served { 0 };

    /// How many candidates the cheap half of the survey found to walk.
    std::uint32_t discovered { 0 };
};

/// Where the worker publishes what it is doing, for a reader on another thread.
///
/// **A publication seam, and it is not an incidental one.** The served-toolchain map is
/// a `WorkerBody` local (`main.cpp`) written by the heartbeat thread under **no lock at
/// all**, and that one-writer discipline is load-bearing and documented where it lives:
/// a dedicated survey thread "would be a second writer to all three and would need a
/// lock that the re-survey path has never needed". `NodeStatusResponder` answers **per
/// request**, on a reactor thread, so reaching into that map -- or bolting a mutex onto
/// it at the call site -- would make every `node-status` request a second reader of
/// state whose safety rests on there being exactly one.
///
/// So the heartbeat thread PUSHES a reading here when it has one, and `Describe()` reads
/// only what was pushed. Nothing on a reactor thread ever touches the map, the map keeps
/// its single writer, and the two threads share exactly one small object whose whole
/// contract is being shared.
///
/// **No default constructor**, for `Cc::ToolchainSurvey`'s reason: a defaulted value
/// would answer the question by omission, and the answer it would give -- some count,
/// some state -- is a reading nobody took. The first honest reading exists before this
/// object does, because the cheap half of the survey has already run by then, so there
/// is no moment this has to invent one.
class NodeRuntimeState
{
  public:
    /// @param initial What is true before any survey has finished -- ordinarily
    ///        `Surveying`, with `discovered` naming what the cheap half found.
    explicit NodeRuntimeState(ToolchainReading initial) noexcept:
        _toolchains { initial }
    {
    }

    NodeRuntimeState() = delete;
    NodeRuntimeState(NodeRuntimeState const&) = delete;
    NodeRuntimeState(NodeRuntimeState&&) = delete;
    NodeRuntimeState& operator=(NodeRuntimeState const&) = delete;
    NodeRuntimeState& operator=(NodeRuntimeState&&) = delete;
    ~NodeRuntimeState() = default;

    /// Record what the survey now says.
    ///
    /// Called from the heartbeat thread, once per survey that concluded something.
    /// @param reading The three facts, as one publication.
    void PublishToolchains(ToolchainReading reading)
    {
        std::scoped_lock const guard { _mutex };
        _toolchains = reading;
    }

    /// @return The last published reading, whole.
    [[nodiscard]] ToolchainReading Toolchains() const
    {
        std::scoped_lock const guard { _mutex };
        return _toolchains;
    }

  private:
    /// Guards `_toolchains`, and `mutable` for the reason
    /// `SchedulerService::LeaderEndpoint` is: the read is `const` and the lock is not
    /// part of what the caller is asking about. A mutex rather than atomics because the
    /// three facts must be published and read as ONE -- and because this is a cold path
    /// on both sides: one write per survey, one read per `node-status` request.
    mutable std::mutex _mutex;

    ToolchainReading _toolchains;
};

/// Where `ConfiguredNodeStatus` reads this node's LIVE facts from.
///
/// A record rather than loose constructor parameters, for `NodeComponents`' reason: a
/// call site that learns a further source adds a named field instead of another pointer
/// nobody can tell from its neighbours. It is expected to grow -- everything a node
/// reports about what it is *doing* arrives through here.
///
/// **Every member is nullable and null means ABSENT, never zero.** A node that publishes
/// no runtime facts must be distinguishable from one whose worker serves nothing, and
/// the wire models that with a disengaged field rather than a `0` that renders as a real
/// reading.
struct NodeRuntimeSources
{
    /// What the worker publishes about its toolchain survey; null when nothing does.
    NodeRuntimeState const* runtime { nullptr };
};

/// The production `INodeStatusSource`: config for the surfaces, a clock for the uptime.
///
/// Its own type rather than a lambda in `main`, for the reason `main` holds nothing
/// testable: it is in no test target, so a `Describe()` written there is a rule nothing
/// can be held to -- and the two things most worth pinning here are exactly the kind
/// that read as obviously right and are not. A surface is reported only when the
/// configuration actually resolves it, and `Describe()` re-reads the clock per call.
class ConfiguredNodeStatus final: public INodeStatusSource
{
  public:
    /// @param cfg What the operator asked for; the surface rows resolve against it.
    ///        Must outlive this.
    /// @param clock Where uptime comes from; must outlive this.
    /// @param startedAt When this process began serving.
    /// @param version The compiled-in version string.
    /// @param nodeId This node's minted identity, or empty when it runs no consensus.
    /// @param components What actually started -- observed, never inferred from flags,
    ///        which is the same rule `NodeCapacityOf` holds: a `--cache-dir` that would
    ///        not open has already stopped startup, and a tier a flag asked for but that
    ///        does not exist would be reported as running.
    /// @param sources Where the LIVE facts are read from, per call. Each member must
    ///        outlive this. Defaulted to nothing wired, which reports every runtime
    ///        field ABSENT rather than inventing a reading -- that is the honest answer
    ///        for a caller that publishes none, and it is what makes *nothing was wired*
    ///        distinguishable from *the worker serves nothing*.
    ConfiguredNodeStatus(NodeConfig const& cfg,
                         IClock const& clock,
                         TimePoint startedAt,
                         std::string version,
                         std::string nodeId,
                         NodeComponents components,
                         NodeRuntimeSources sources = {}) noexcept;

    /// @copydoc INodeStatusSource::Describe
    [[nodiscard]] CompileCacheWire::NodeStatusFields Describe() const override;

  private:
    NodeConfig const& _cfg;
    IClock const& _clock;
    TimePoint _startedAt;
    std::string _version;
    std::string _nodeId;
    NodeComponents _components;
    NodeRuntimeSources _sources;
};

} // namespace FastCache::Node
