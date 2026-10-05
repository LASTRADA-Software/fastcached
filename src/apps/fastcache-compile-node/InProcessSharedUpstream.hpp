// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "LocalCache.hpp"
#include "SharedCacheHost.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <core/async/Task.hpp>

namespace FastCache::Node
{

/// The private tier's upstream on the machine the fleet's `shared-cache` setting names: the shared
/// tier this process's `SharedCacheHost` publishes, read and written directly.
///
/// **Not a special case of `SharedCacheUpstream`, because none of its questions arise.** There is no
/// peer to prove -- the tier is in this process -- no credential to present and no admission to
/// ask, and a dial to this machine's own port would spend a handshake to ask this process something
/// it already knows. So it holds no connector, no dialer and no session: nothing here CAN dial.
///
/// **What it stores is already canonical.** The private tier's `CacheProxy` canonicalized the value
/// at the STORE that reached this node, as it does for everything `RemoteUpstream` forwards, so the
/// shared tier's `CacheProxy` -- which a frame would reach -- has nothing left to do and is not
/// reached: `SharedCacheTier::Store` takes the value as it is.
///
/// **It borrows the host for its whole life** (`SharedCacheHost::Borrow`): the host refuses to be
/// destroyed while this exists, so an owner that got the two in the wrong order is told so by name
/// at shutdown rather than reading a freed host -- or freeing the tier, and running its store's final
/// flush, on whichever thread finished the last call.
///
/// **It follows the setting at every call.** Each operation reads `SharedCacheHost::Current()` and
/// holds what it got for the whole call, so a move of the setting mid-read retires the tier rather
/// than freeing it under the read; once the setting names another machine, the next operation finds
/// no tier and answers as a node with no shared cache does. The move takes effect at the next apply
/// here, as it does on every other node.
///
/// **Safe from any thread, on a premise.** `Current()` is safe anywhere; what it hands out is then used
/// from the private tier's thread while the responder answers frames from the same tier on the
/// node's loop. That is safe because `DiskSharedTierOpener` gives the tier a store wrapped in
/// `ShareStorage` -- a single-shard `ShardedStorage`, which is the lock -- and `LocalCache` keeps no
/// mutable state of its own, counting into an atomic sink. A tier over a bare store, as the test
/// fakes build, is single-threaded only.
class InProcessSharedUpstream final: public ICacheUpstream
{
  public:
    /// @param host Publishes the shared tier while the setting names this machine; must outlive this.
    explicit InProcessSharedUpstream(SharedCacheHost const& host) noexcept;

    /// @copydoc ICacheUpstream::Fetch
    [[nodiscard]] core::async::Task<std::optional<std::vector<std::byte>>> Fetch(std::string_view key) override;

    /// @copydoc ICacheUpstream::Store
    [[nodiscard]] core::async::Task<UpstreamStore> Store(std::string_view key, std::span<std::byte const> value) override;

    /// @copydoc ICacheUpstream::Configured
    ///
    /// Exactly while the host publishes a tier: the same fact `Store` answers `NotConfigured` from.
    [[nodiscard]] bool Configured() const noexcept override;

  private:
    SharedCacheHost::Borrow _host;
};

} // namespace FastCache::Node
