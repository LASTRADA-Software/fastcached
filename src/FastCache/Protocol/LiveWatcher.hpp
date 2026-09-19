// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <optional>
#include <string>

namespace FastCache
{

/// Who a live subscription belongs to, for as long as it streams.
///
/// ## The same two facts every membership gate folds
///
/// A host and, when the connection proved one, the identity it proved --
/// `Distributed::ExplainConnection` takes exactly these, and the stream is re-gated on them every
/// tick ([#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428), #178). The identity
/// is carried whole rather than as a `bool`, because the question asked of it on every tick is
/// whether its KEY is still live in the roster: a key revoked while a dashboard watches ends that
/// dashboard's stream on the next tick, as the forgotten machine's.
///
/// So this is shareable: `fastcached` implements `ILiveGate` too and runs no node handshake, so
/// `proven` is honestly disengaged there rather than a plausible default it must invent.
///
/// ## Why it OWNS its host
///
/// `ILiveGate::Recheck` runs once per tick for the whole life of a subscription, inside a
/// coroutine, so anything it reads must outlive every suspension. A `std::string_view` here
/// would be a view into whatever the serve loop had on its stack when the subscription started
/// -- the use-after-free [#366](https://github.com/LASTRADA-Software/fastcached/issues/366)
/// already paid for on the request path, arriving on the stream path where the lifetime is far
/// longer.
struct LiveWatcher
{
    /// The peer host, as the kernel reported it.
    std::string host {};

    /// The identity this connection proved, or nothing when it proved none.
    std::optional<ProvenIdentity> proven {};
};

} // namespace FastCache
