// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <FastCache/Async/DeadlineTimer.hpp>

#include <memory>
#include <utility>

#include <EndpointDial.hpp>

namespace FastCache::Node
{

namespace
{

    /// Arm a deadline that closes `socket`, or nothing when there is no reactor.
    ///
    /// Returned as a `unique_ptr` so the caller holds it for the exchange's
    /// lifetime and a null reactor costs an empty pointer rather than a branch at
    /// each use.
    /// @param reactor Where to arm it, or nullptr for none.
    /// @param ceiling How long the exchange may take.
    /// @param socket What to close on expiry; must outlive the returned timer.
    /// @return The armed timer, or nullptr.
    [[nodiscard]] std::unique_ptr<DeadlineTimer> ArmExchangeDeadline(IReactor* reactor,
                                                                     std::chrono::milliseconds ceiling,
                                                                     ISocket* socket)
    {
        if (reactor == nullptr || ceiling <= std::chrono::milliseconds::zero())
            return nullptr;
        return std::make_unique<DeadlineTimer>(
            *reactor,
            reactor->Clock().Now() + ceiling,
            [](void* target) { static_cast<ISocket*>(target)->Close(); },
            socket);
    }

} // namespace

RemoteUpstream::RemoteUpstream(std::string endpoint,
                               Cc::Credential credential,
                               IConnector& connector,
                               IReactor* reactor,
                               std::chrono::milliseconds connectTimeout,
                               std::chrono::milliseconds ioTimeout) noexcept:
    _endpoint { std::move(endpoint) },
    _credential { std::move(credential) },
    _connector { connector },
    _reactor { reactor },
    _connectTimeout { connectTimeout },
    _ioTimeout { ioTimeout }
{
}

Task<std::optional<std::vector<std::byte>>> RemoteUpstream::Fetch(std::string_view key)
{
    auto client = co_await Cc::DialEndpoint(&_connector, _endpoint, _connectTimeout);
    if (client == nullptr)
        // Unreachable is a miss. `LocalCache` documents why: the caller compiles
        // either way, and a build that could FAIL because a cache was down is the
        // one outcome this subsystem must never produce.
        co_return std::nullopt;

    // Bounds the WHOLE exchange by closing the socket. Closing completes whatever
    // the exchange is parked on, so it reports a transport failure and this
    // reports a miss, which is what a caller of a best-effort upstream wants.
    //
    // Only with a reactor: over a blocking connector the socket's own
    // `SO_RCVTIMEO` is already the bound, and a second mechanism for one job is
    // how the two come to disagree.
    auto const bound = ArmExchangeDeadline(_reactor, _ioTimeout, client.get());

    auto outcome = co_await Cc::CacheFetch(client.get(), key, _credential);
    if (!outcome.IsHit())
        co_return std::nullopt;
    co_return std::move(outcome.value);
}

Task<UpstreamStore> RemoteUpstream::Store(std::string_view key, std::span<std::byte const> value)
{
    auto client = co_await Cc::DialEndpoint(&_connector, _endpoint, _connectTimeout);
    if (client == nullptr)
        // `Declined`, not `NotConfigured`: there IS a shared cache and this node
        // could not reach it, which is exactly the condition an operator wants the
        // failure counter to be counting.
        co_return UpstreamStore::Declined;

    auto const bound = ArmExchangeDeadline(_reactor, _ioTimeout, client.get());

    // The roots travel empty. A node is forwarding an object another machine already
    // canonicalized -- the launcher rewrote its own layout out of the value before it
    // ever reached this node -- so naming THIS node's roots would ask the daemon to
    // canonicalize a second time against paths that were never in the value. Storing
    // it verbatim is what keeps a relayed object identical to a directly stored one.
    //
    // The request is a LOCAL rather than a temporary in the call expression:
    // `CacheStore` takes it by reference and its frame outlives the expression, so
    // a temporary would dangle at the first suspend.
    CompileCacheWire::StoreRequest const request {
        .key = key, .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = value
    };
    auto const outcome = co_await Cc::CacheStore(client.get(), request, _credential);

    // A STORE that succeeded comes back as `Hit`: the wire answers `Ok`, and
    // `CacheOutcomeKind` names the STATUS rather than the verb. Anything else --
    // a refusal, a transport failure -- is the fleet declining the object, which
    // `LocalCache` has already been told costs this machine nothing.
    co_return outcome.kind == Cc::CacheOutcomeKind::Hit ? UpstreamStore::Stored : UpstreamStore::Declined;
}

} // namespace FastCache::Node
