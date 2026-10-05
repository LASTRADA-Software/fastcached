// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheTierProfile.hpp"

#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

// Declared, not included: an upstream hands one out and says nothing else about it.
class ISharedCacheStatusSource;

/// What became of one object offered to the shared cache.
///
/// Three outcomes, and the `bool` this replaced had two values for them. `false`
/// meant both "there is a shared cache and it declined this" and "there is no shared
/// cache", and `LocalCache` counted every one of them as a failure -- so a machine
/// with no upstream reported a **100 % upstream store failure rate**, 1800 failures
/// against 1800 sets, which reads exactly like a shared cache that is down (#214).
///
/// The distinction has to exist at the seam rather than at the call site. A caller
/// told "false" cannot recover which of the two it was, and the next caller would
/// read it the same wrong way.
enum class UpstreamStore : std::uint8_t
{
    Stored,        ///< The shared cache took it.
    Declined,      ///< There is one, and it did not take it. An operator's problem.
    NotConfigured, ///< There is no shared cache. Not a failure, and not an event.
    Last
};

/// What became of one removal from this node's own tier.
///
/// PRIVATE: never transmitted and never persisted -- `CacheProxy` maps each to a reply
/// status on the spot -- so its enumerators carry no explicit values.
///
/// Three outcomes, because two replies and a refusal hang off them and a `bool` would fold
/// the refusal into one of the answers: `Absent` is an ordinary answer an idempotent repair
/// gives on its second run, and `Failed` is this machine's disk, which is not the caller's
/// key being missing.
enum class CacheDropOutcome : std::uint8_t
{
    Removed, ///< This call removed it.
    Absent,  ///< There was nothing to remove.
    Failed,  ///< The tier could not persist the removal.
};

/// The shared cache this node reads through to, as a seam.
///
/// A network client in production and a scripted double in tests, for the reason
/// every other I/O dependency here is injected: the read-through *rules* below are
/// where the mistakes live, and they must be assertable without a daemon, a socket
/// or a sleep.
class ICacheUpstream
{
  public:
    virtual ~ICacheUpstream() = default;

    ICacheUpstream() = default;
    ICacheUpstream(ICacheUpstream const&) = default;
    ICacheUpstream& operator=(ICacheUpstream const&) = default;
    ICacheUpstream(ICacheUpstream&&) = default;
    ICacheUpstream& operator=(ICacheUpstream&&) = default;

    /// Ask the shared cache for one key.
    /// @param key The object key.
    /// @return The value on a hit; nullopt on a miss **or on any failure**.
    ///
    /// A miss and an unreachable upstream are deliberately the same answer. The
    /// caller's response is identical -- compile it -- and a client that could tell
    /// them apart would have nothing useful to do with the distinction. Whether the
    /// upstream was *reachable* is reported separately, as a counter, because that
    /// is an operator's question rather than a build's.
    [[nodiscard]] virtual core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) = 0;

    /// Offer one object to the shared cache.
    ///
    /// Best-effort by contract: the local tier has already accepted it, so a failure
    /// here costs the *fleet* a shared entry and costs this machine nothing.
    /// @param key The object key.
    /// @param value The encoded compile value.
    /// @return Which of the three outcomes it was. An implementation that has no
    ///         shared cache answers `NotConfigured` and is never counted as having
    ///         failed at something it did not attempt.
    [[nodiscard]] virtual core::async::Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) = 0;

    /// Whether there is a shared cache behind this at all.
    ///
    /// The same fact `UpstreamStore::NotConfigured` reports, asked before a store
    /// rather than about one -- and it has to be askable that way, because the store
    /// counters are cumulative: a node with no upstream and a node with one it has
    /// not written to yet both report zero, so neither counter can answer it. Both
    /// are answered from the same fact -- fixed at construction for most upstreams,
    /// the current target for the shared cache's, which follows the replicated
    /// setting -- so they cannot drift apart: an implementation answering `false` here
    /// answers `NotConfigured` there for the same target.
    /// @return True when a shared cache is configured.
    [[nodiscard]] virtual bool Configured() const noexcept = 0;

    /// Told that the cluster just applied a state, AFTER whatever this upstream reads its target
    /// from was told the same.
    ///
    /// For an upstream whose target follows the replicated state, which re-judges what it reports
    /// now rather than at its next operation: a live condition that lagged the state it describes
    /// until a build happened to miss would be a stale answer on the surface an operator reads.
    /// On the consensus apply callback: never blocks.
    ///
    /// **Who cares, today: only the fleet half.** `SharedCacheUpstream` re-judges
    /// `shared-cache-unproven` from the directory -- out of the setting, unresolved, or a newly
    /// resolved machine -- reached through the node's `ProvenSharedCacheUpstream` and, where this
    /// machine may hold the shared tier itself, `SwitchingSharedUpstream`, which forward it. The
    /// rest take the default, each for its own reason: `RemoteUpstream` is built on its `--upstream`
    /// endpoint, which no applied state moves; `NoUpstream` has none; and `InProcessSharedUpstream` reports no
    /// condition and asks its host for the published tier on every call, so there is no judgement
    /// of its own to go stale. An upstream that keeps a verdict about a target the replicated state
    /// can move is the next implementer.
    virtual void StateApplied() {}

    /// How reaching the fleet's shared cache went, for `--node-status` -- where this upstream keeps
    /// such a verdict.
    ///
    /// **A seam rather than a downcast**: the half that knows is `SharedCacheUpstream`, and the
    /// node never holds one bare -- it sits behind `ProvenSharedCacheUpstream` and, where this
    /// machine may hold the tier itself, `SwitchingSharedUpstream`, which forward this as they
    /// forward `StateApplied`. Every other upstream keeps no such verdict and answers null:
    /// `RemoteUpstream` proves nothing, `NoUpstream` reaches nothing, and `InProcessSharedUpstream`
    /// is this machine's own tier, whose state the host reports.
    /// @return The fleet half's status, or null; it lives as long as this upstream.
    [[nodiscard]] virtual ISharedCacheStatusSource const* SharedCacheStatus() const noexcept
    {
        return nullptr;
    }
};

/// An upstream that is not there.
///
/// The honest shape for a node configured with no shared cache -- one developer's
/// machine, or a fleet that has not been given one yet. A named type rather than a
/// null pointer, so every call site is spared a branch and "there is no upstream"
/// is a decision somebody made rather than a pointer nobody set.
class NoUpstream final: public ICacheUpstream
{
  public:
    /// @copydoc ICacheUpstream::Fetch
    ///
    /// A coroutine that never suspends, so a node with no shared cache pays one
    /// small frame allocation per local miss and no round trip. Kept as a real
    /// implementation rather than reverting to a null pointer, for the reason
    /// this class exists: "there is no upstream" should be a decision somebody
    /// made, not a pointer nobody set.
    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view /*key*/) override
    {
        co_return std::nullopt;
    }

    /// @copydoc ICacheUpstream::Store
    ///
    /// `NotConfigured`, which is the whole point of the enum: this used to answer
    /// `false` and be counted as a failed store on every single local write.
    [[nodiscard]] core::async::Task<UpstreamStore> Store(std::string_view /*key*/,
                                                         std::span<std::byte const> /*value*/) override
    {
        co_return UpstreamStore::NotConfigured;
    }

    /// @copydoc ICacheUpstream::Configured
    [[nodiscard]] bool Configured() const noexcept override
    {
        return false;
    }
};

/// A node-local cache in front of the shared one.
///
/// ## Why a node caches at all
///
/// The shared `fastcached` already holds every object, so a second copy looks
/// redundant. It is not, and the reason is the one this whole architecture was
/// reshaped for: a **local rebuild on a slow or bad network should not go to the
/// wire at all**. A developer who rebuilds the same tree twenty times a day pays
/// the round trip twenty times for objects that never left their machine, and on a
/// link that is slow or lossy that cost is the difference between a cache that
/// helps and one that hurts.
///
/// ## The rules, and why each is not the obvious one
///
/// **A local hit does not consult the upstream at all.** That is the entire point;
/// an implementation that revalidated would have moved the round trip rather than
/// removed it. Object keys are content-addressed -- the key is a digest over the
/// preprocessed text, the arguments, the compiler identity and the dependency set --
/// so a key that matches names the same object by construction. There is nothing an
/// upstream could tell us about it that we do not already know.
///
/// **A local miss populates the local tier from the upstream.** Otherwise the second
/// build is as slow as the first, and a "cache" that never fills is a proxy.
///
/// **A store writes local FIRST, then offers upstream.** The local write is the one
/// that must not be lost: it is what makes this machine's next build fast, and it
/// cannot fail for a reason the network chose. Offering upstream afterwards is
/// best-effort by contract -- a fleet that cannot be reached costs the fleet a
/// shared entry and costs this machine nothing.
///
/// **An unreachable upstream is a miss, not an error.** Every caller's answer to
/// both is "compile it", so distinguishing them at this layer would buy nothing and
/// would give the build a failure mode it does not need. The distinction is kept
/// where it is actionable: a counter an operator can read.
class LocalCache
{
  public:
    /// @param local The node's own tier; must outlive this.
    /// @param upstream The shared cache; must outlive this.
    /// @param clock Time source for the local tier's expiry; must outlive this.
    /// @param metrics Where hits, misses and upstream outcomes are counted.
    /// @param profile Which tier this is, and so which counters it moves. Copied: the row is a
    ///        handful of bytes, and a copy cannot outlive what it was read from. It must count
    ///        EVERY upstream outcome (`UpstreamCountingOf`): an upstream that can read or store
    ///        paired with a profile counting none of it would do both in silence.
    /// @throws std::invalid_argument When @p profile does not count every upstream outcome.
    LocalCache(IStorage& local,
               ICacheUpstream& upstream,
               core::platform::IClock& clock,
               IMetricsSink& metrics,
               CacheTierProfile const& profile);

    /// A tier that reads through to nothing -- the fleet's shared tier, which is the top of the
    /// fleet's cache.
    /// @param local The tier; must outlive this.
    /// @param upstream Nothing, by type; must outlive this.
    /// @param clock Time source for the tier's expiry; must outlive this.
    /// @param metrics Where hits, misses and refusals are counted.
    /// @param profile Which tier this is. It may count every upstream outcome or none -- none of
    ///        them happens here -- but not some.
    /// @throws std::invalid_argument When @p profile counts some upstream outcomes and not others.
    LocalCache(IStorage& local,
               NoUpstream& upstream,
               core::platform::IClock& clock,
               IMetricsSink& metrics,
               CacheTierProfile const& profile);

    /// Look one key up, reading through to the shared cache on a local miss.
    /// @param key The object key.
    /// @return The value, or nullopt when neither tier has it.
    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key);

    /// Store one object locally, then offer it to the shared cache.
    /// @param key The object key.
    /// @param value The encoded compile value.
    /// @return Whether the LOCAL write succeeded. The upstream's answer is counted,
    ///         not returned: a client that retried on it would be retrying something
    ///         that is already durable where it matters.
    [[nodiscard]] core::async::Task<bool> Store(std::string_view key, std::span<std::byte const> value);

    /// Remove one key from this node's own tier, and from nowhere else.
    ///
    /// **The upstream is not asked**, which is `Op::CacheDrop`'s rule rather than a gap
    /// here: a destructive verb reaches the endpoint its sender named. A shared cache still
    /// holding the key refills it on this tier's next miss, and that is the operator's
    /// second command to run rather than this one's side effect.
    ///
    /// Uncounted here, because the storage counts it: `deleteHits` and `deleteMisses` are
    /// what `/metrics` renders for this tier, and a second tally of one number is what
    /// `IMetricsSink` refuses to carry. A failure to persist the removal is reported by
    /// `WriteErrorReportingStorage`, which every node tier sits under.
    /// @param key The object key.
    /// @return Which of the three things happened.
    [[nodiscard]] CacheDropOutcome Drop(std::string_view key);

    /// Which tier this is: the row it was built with.
    ///
    /// Read by the `CacheProxy` in front of it rather than passed to that proxy a second time,
    /// so the verbs a tier answers and the counters its storage moves come from ONE row and
    /// cannot name two different tiers.
    /// @return The profile.
    [[nodiscard]] CacheTierProfile const& Profile() const noexcept
    {
        return _counters;
    }

  private:
    IStorage& _local;
    ICacheUpstream& _upstream;
    core::platform::IClock& _clock;
    IMetricsSink& _metrics;
    CacheTierProfile _counters;
};

} // namespace FastCache::Node
