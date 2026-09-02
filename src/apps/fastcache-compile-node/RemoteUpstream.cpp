// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <FastCache/Net/SocketDeadline.hpp>

#include <memory>
#include <utility>

#include <EndpointDial.hpp>

namespace FastCache::Node
{

namespace
{

} // namespace

RemoteUpstream::RemoteUpstream(std::string endpoint,
                               Cc::Credential credential,
                               Cc::CredentialNotice::Sink noticeSink,
                               IConnector& connector,
                               IReactor* reactor,
                               std::chrono::milliseconds connectTimeout,
                               std::chrono::milliseconds ioTimeout) noexcept:
    _endpoint { std::move(endpoint) },
    _credential { std::move(credential) },
    _notice { std::move(noticeSink) },
    _connector { connector },
    _reactor { reactor },
    _connectTimeout { connectTimeout },
    _ioTimeout { ioTimeout }
{
}

Task<std::optional<std::vector<std::byte>>> RemoteUpstream::Fetch(std::string_view key)
{
    auto client = co_await Cc::DialEndpoint(&_connector, _endpoint, DialOptions { .connectTimeout = _connectTimeout });
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
    SocketDeadlineTarget target { .socket = client.get() };
    auto const bound = ArmSocketDeadline(_reactor, _ioTimeout, &target);

    auto outcome = co_await Cc::CacheFetch(client.get(), &_notice, key, _credential);
    if (!outcome.IsHit())
        co_return std::nullopt;
    co_return std::move(outcome.value);
}

Task<UpstreamStore> RemoteUpstream::Store(std::string_view key, std::span<std::byte const> value)
{
    auto client = co_await Cc::DialEndpoint(&_connector, _endpoint, DialOptions { .connectTimeout = _connectTimeout });
    if (client == nullptr)
        // `Declined`, not `NotConfigured`: there IS a shared cache and this node
        // could not reach it, which is exactly the condition an operator wants the
        // failure counter to be counting.
        co_return UpstreamStore::Declined;

    SocketDeadlineTarget target { .socket = client.get() };
    auto const bound = ArmSocketDeadline(_reactor, _ioTimeout, &target);

    // The roots travel empty, and what makes that correct is NOT what this comment
    // used to say. It claimed "the launcher rewrote its own layout out of the value
    // before it ever reached this node", which is false: a launcher sends its roots
    // and lets the server canonicalize. While that premise stood, a node forwarded a
    // value carrying the producer's absolute paths under empty roots -- so the
    // daemon's own canonicalization matched nothing and the poison reached the shared
    // cache intact (#319).
    //
    // It is correct now because `CacheProxy` canonicalizes at the STORE that reaches
    // this node, so what is forwarded is already tokens, and empty roots ask the
    // daemon to rewrite nothing rather than to rewrite against the wrong layout. That
    // is the invariant this seam depends on and it is one call away, so state it
    // here: anything else that reaches `LocalCache::Store` must have gone through
    // `CanonicalStoredValue` first, or this forward relays the same defect again.
    //
    // The request is a LOCAL rather than a temporary in the call expression:
    // `CacheStore` takes it by reference and its frame outlives the expression, so
    // a temporary would dangle at the first suspend.
    CompileCacheWire::StoreRequest const request {
        .key = key, .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = value
    };
    auto const outcome = co_await Cc::CacheStore(client.get(), &_notice, request, _credential);

    // A STORE that succeeded comes back as `Hit`: the wire answers `Ok`, and
    // `CacheOutcomeKind` names the STATUS rather than the verb. Anything else --
    // a refusal, a transport failure -- is the fleet declining the object, which
    // `LocalCache` has already been told costs this machine nothing.
    co_return outcome.kind == Cc::CacheOutcomeKind::Hit ? UpstreamStore::Stored : UpstreamStore::Declined;
}

} // namespace FastCache::Node
