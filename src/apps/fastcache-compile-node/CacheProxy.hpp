// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace FastCache::Node
{

/// Turns one `0xFC` request into one reply, for a node's own cache tier.
///
/// **Pure**: bytes in, bytes out, the same shape `Cc::WorkerProtocol` and
/// `Distributed::SchedulerProtocol` already have. It opens no socket, so the whole
/// verb surface is testable by handing it a frame.
///
/// ## Why a node answers cache verbs at all
///
/// So `fastcache-cc` can talk to **localhost** and still get the shared cache's
/// contents. The launcher used to dial the shared `fastcached` directly, which
/// means every miss and every hit crossed the network — including for objects this
/// machine compiled minutes ago. Pointing the launcher at its local node puts
/// `LocalCache` in that path, and that is what makes a rebuild on a slow link free.
///
/// A node answers `Store` and `Fetch` and nothing else. The scheduler's verbs
/// belong to `SchedulerProtocol` and `Compile` to `WorkerProtocol`; each refuses
/// the others with `DispatchNotPermitted`, as a reply rather than a close, so a
/// client that reached the wrong port learns which.
///
/// `Auth` is the exception, and refusing it correctly is a **wire contract**: it
/// is answered `CompileCacheWire::UnimplementedVerb`, because that is the one
/// refusal `Cc::CacheProtocol` reads as "this endpoint implements no credential"
/// and proceeds past. Any other code makes the launcher treat the exchange as
/// fatal and miss every subsequent compile in silence. See `RefusedVerbs` in the
/// implementation for why that is a table row rather than a comment.
///
/// **The name, not the enumerator it aliases.** `UnimplementedVerb` is today
/// `ErrorCode::UnknownOpcode`, and naming the alias here rather than the contract
/// is how three surfaces came to spell one refusal separately and then disagree
/// about it (#283, #340).
class CacheProxy
{
  public:
    /// @param cache The node's read-through tier; must outlive this.
    /// @param metrics Where this tier's refusals are counted; must outlive this.
    ///        Injected rather than reached for, because a refusal answered while
    ///        nothing rises is a probed port that looks unused (#326, #491) -- and
    ///        because `Cc::Refuse` takes a sink, so there is no way to spell a counted
    ///        refusal without one.
    CacheProxy(LocalCache& cache, IMetricsSink& metrics) noexcept:
        _cache { cache },
        _metrics { metrics }
    {
    }

    /// Answer one complete request frame.
    /// @param frame Header plus payload, exactly as received.
    /// @return The encoded reply; empty only when the peer is not speaking this
    ///         protocol at all, which is the one condition that must close.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame);

  private:
    LocalCache& _cache;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Node
