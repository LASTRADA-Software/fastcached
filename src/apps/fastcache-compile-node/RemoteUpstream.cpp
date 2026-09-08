// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/SocketAddress.hpp>
#include <FastCache/Net/SocketDeadline.hpp>

#include <format>
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
                               IAsyncAddressResolver& resolver,
                               IClock& clock,
                               std::chrono::milliseconds connectTimeout,
                               std::chrono::milliseconds ioTimeout,
                               std::chrono::milliseconds addressRefreshInterval):
    _endpoint { std::move(endpoint) },
    _credential { std::move(credential) },
    _notice { std::move(noticeSink) },
    _connector { connector },
    _reactor { reactor },
    _resolver { resolver },
    _clock { clock },
    _connectTimeout { connectTimeout },
    _ioTimeout { ioTimeout },
    _addressRefreshInterval { addressRefreshInterval }
{
    // Split ONCE. `_endpoint` is fixed for this object's life, so re-parsing it per
    // operation would be the same shape of waste the resolution itself was.
    // `ParseDialEndpoint`, which is the predicate `--upstream` is already validated
    // with -- not `SplitHostPort`, whose port is text and which deliberately keeps an
    // unsplittable value whole for the membership matching it serves instead.
    if (auto const split = ParseDialEndpoint(_endpoint); split.has_value())
    {
        _host = split->first;
        _port = split->second;
    }
}

Task<std::string> RemoteUpstream::DialTarget()
{
    // Nothing to hold an address FOR: an endpoint that did not parse is handed to
    // `DialEndpoint` exactly as before, so its failure stays where it was.
    //
    // No "is it already a literal" branch. A literal resolves for free, so the branch
    // would buy nothing and would be a second thing to be wrong about -- the resolver
    // hands a literal straight back and the interval simply never matters for one.
    if (_host.empty())
        co_return _endpoint;

    // INTERVAL, never miss-triggered. This is the whole guard: a refresh driven by a
    // failed dial or a cache miss lets a remote peer force one lookup per request
    // just by asking for keys this cache does not hold.
    auto const now = _clock.Now();
    if (_lastLookupAt.has_value() && now - *_lastLookupAt < _addressRefreshInterval)
        co_return _resolved.value_or(_endpoint);

    // Stamped BEFORE the lookup and regardless of how it goes, so a failing resolver
    // is retried once per interval rather than once per operation.
    _lastLookupAt = now;

    auto resolved = co_await _resolver.Resolve(_host, _port, _reactor);
    if (!resolved.has_value() || resolved->empty())
    {
        // A failed lookup does NOT reset the timer, so a resolver that is down is
        // retried once per interval rather than once per operation. Keep serving the
        // previous address if there is one -- it worked more recently than this
        // attempt did -- and otherwise fall back to the name, which is exactly what
        // this class did before it held anything.
        co_return _resolved.value_or(_endpoint);
    }

    // **Only a UNIQUE candidate is held**, and this is a correctness bound rather than
    // caution. `Detail::RunConnectFlow` tries EVERY candidate a name resolves to, and
    // its own comment names the case: an AAAA on a machine with no IPv6 route, which
    // is exactly what trying every candidate exists for. Pinning the first would hand
    // the connector one address and destroy that fallback for a whole interval -- and
    // silently, because an unreachable upstream is reported as a cache miss by design.
    //
    // So a multi-answer name keeps dialling by NAME, which costs what it always cost
    // and behaves exactly as it always did. The saving applies to the single-answer
    // case, which is what an `--upstream` naming one cache host normally is.
    if (resolved->size() != 1)
        co_return _endpoint;

    auto const host = FormatPeerAddress(resolved->front());

    // `FormatPeerAddress` renders no `sin6_scope_id`, so a link-local answer would
    // come back as a zone-less `fe80::...` that cannot be dialled at all. The name
    // carries the zone through the resolver, so hand the name back instead.
    if (host.empty() || host.starts_with("fe80:"))
        co_return _endpoint;

    // `FormatHostPort` rather than a fourth author of the bracket-a-v6-host rule --
    // its own header exists so that rule cannot drift between call sites.
    _resolved = FormatHostPort(host, _port);
    co_return *_resolved;
}

Task<std::optional<std::vector<std::byte>>> RemoteUpstream::Fetch(std::string_view key)
{
    auto const dialTarget = co_await DialTarget();
    auto client = co_await Cc::DialEndpoint(&_connector, dialTarget, DialOptions { .connectTimeout = _connectTimeout });
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
    auto const dialTarget = co_await DialTarget();
    auto client = co_await Cc::DialEndpoint(&_connector, dialTarget, DialOptions { .connectTimeout = _connectTimeout });
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
