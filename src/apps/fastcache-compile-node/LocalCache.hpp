// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

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
    [[nodiscard]] virtual Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) = 0;

    /// Offer one object to the shared cache.
    ///
    /// Best-effort by contract: the local tier has already accepted it, so a failure
    /// here costs the *fleet* a shared entry and costs this machine nothing.
    /// @param key The object key.
    /// @param value The encoded compile value.
    /// @return Whether the shared cache took it.
    [[nodiscard]] virtual Task<bool> Store(std::string_view key, std::span<std::byte const> value) = 0;
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
    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view /*key*/) override
    {
        co_return std::nullopt;
    }

    /// @copydoc ICacheUpstream::Store
    [[nodiscard]] Task<bool> Store(std::string_view /*key*/, std::span<std::byte const> /*value*/) override
    {
        co_return false;
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
    LocalCache(IStorage& local, ICacheUpstream& upstream, IClock& clock, IMetricsSink& metrics) noexcept;

    /// Look one key up, reading through to the shared cache on a local miss.
    /// @param key The object key.
    /// @return The value, or nullopt when neither tier has it.
    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key);

    /// Store one object locally, then offer it to the shared cache.
    /// @param key The object key.
    /// @param value The encoded compile value.
    /// @return Whether the LOCAL write succeeded. The upstream's answer is counted,
    ///         not returned: a client that retried on it would be retrying something
    ///         that is already durable where it matters.
    [[nodiscard]] Task<bool> Store(std::string_view key, std::span<std::byte const> value);

  private:
    IStorage& _local;
    ICacheUpstream& _upstream;
    IClock& _clock;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Node
