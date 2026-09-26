// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <optional>
#include <string>

namespace FastCache::Node
{

/// Who is at the other end of a connection, as an admission policy sees it.
///
/// Two facts rather than one string, and the second is why this type exists: admission used to
/// be decided from the peer's ADDRESS alone, which is a stand-in for *this is one of our nodes*
/// and stops being one the moment an address is not stable
/// ([#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428),
/// [#178](https://github.com/LASTRADA-Software/fastcached/issues/178) item 1).
///
/// A STRUCT rather than a second parameter, because the two travel together everywhere and a
/// policy that took them apart could read one and forget the other -- which is a gate widened
/// or narrowed silently.
///
/// **It OWNS both facts and is deliberately not named `*View`** (#366). `Answer` is a
/// coroutine, so its parameters must outlive every suspension in it and a view into the serve
/// loop's locals is exactly the use-after-free that rule exists for -- which is why `Answer`
/// took `std::string peer` by value to begin with. The same type serves the pre-payload path,
/// where the copy was already accepted: measured against a real `Register` round trip, one
/// owned peer id costs 4.5-4.7 ns against 1.4-2.0 us, and `proven` is disengaged on every
/// connection that proves nothing, which is no allocation at all.
///
/// And the two mistakes are not commensurable, which is #366's own tie-breaker: a wrong view
/// here is an ADMISSION decided from freed memory, and a wrong copy is nanoseconds.
struct PeerIdentity
{
    /// The kernel's peer HOST, as `getpeername` reported it. Never a name the peer chose, and
    /// never an endpoint: a peer dials from an ephemeral source port, so there is no port here.
    ///
    /// It can legitimately be empty -- `core::net::formatPeerAddress` answers that for a peer whose family
    /// is unknown or whose `getpeername` failed -- so a reader decides what an unnameable
    /// caller means rather than assuming a host.
    std::string host {};

    /// Engaged exactly when this connection PROVED an identity: the id it claimed and the key its
    /// signature verified under (#178).
    ///
    /// **Engaged for a REVOKED key too**, and that is the point of carrying the key rather than a
    /// flag: the roster is asked about it on every verb (`Distributed::ExplainConnection`), so a
    /// connection that proved a revoked key is refused as the forgotten machine's whatever its
    /// address says, and one whose key is revoked while it is open is refused from its next verb.
    ///
    /// Disengaged is the ordinary case -- every launcher, every operator's one-shot verb -- and a
    /// policy then decides exactly as it did before identities existed.
    std::optional<ProvenIdentity> proven {};
};

} // namespace FastCache::Node
