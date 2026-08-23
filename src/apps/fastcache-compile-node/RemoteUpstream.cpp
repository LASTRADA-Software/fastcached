// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <utility>

#include <ITcpClient.hpp>

namespace FastCache::Node
{

RemoteUpstream::RemoteUpstream(std::string endpoint, Cc::Credential credential, std::chrono::milliseconds ioTimeout) noexcept
    :
    _endpoint { std::move(endpoint) },
    _credential { std::move(credential) },
    _ioTimeout { ioTimeout }
{
}

std::optional<std::vector<std::byte>> RemoteUpstream::Fetch(std::string_view key)
{
    auto client = Cc::ConnectTcp(_endpoint, _ioTimeout);
    if (!client)
        // Unreachable is a miss. `LocalCache` documents why: the caller compiles
        // either way, and a build that could FAIL because a cache was down is the
        // one outcome this subsystem must never produce.
        return std::nullopt;

    auto outcome = Cc::CacheFetch(*client, key, _credential);
    if (!outcome.IsHit())
        return std::nullopt;
    return std::move(outcome.value);
}

bool RemoteUpstream::Store(std::string_view key, std::span<std::byte const> value)
{
    auto client = Cc::ConnectTcp(_endpoint, _ioTimeout);
    if (!client)
        return false;

    // The roots travel empty. A node is forwarding an object another machine already
    // canonicalized -- the launcher rewrote its own layout out of the value before it
    // ever reached this node -- so naming THIS node's roots would ask the daemon to
    // canonicalize a second time against paths that were never in the value. Storing
    // it verbatim is what keeps a relayed object identical to a directly stored one.
    auto const outcome = Cc::CacheStore(
        *client,
        CompileCacheWire::StoreRequest { .key = key, .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = value },
        _credential);
    // A STORE that succeeded comes back as `Hit`: the wire answers `Ok`, and
    // `CacheOutcomeKind` names the STATUS rather than the verb. Anything else --
    // a refusal, a transport failure -- is the fleet declining the object, which
    // `LocalCache` has already been told costs this machine nothing.
    return outcome.kind == Cc::CacheOutcomeKind::Hit;
}

} // namespace FastCache::Node
