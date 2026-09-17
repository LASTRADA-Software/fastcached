// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace FastCache
{

/// Who a live subscription belongs to, for as long as it streams.
///
/// ## Why this is not the connection full identity
///
/// A node knows more about its caller than this: `Node::PeerIdentity` carries the node id a
/// caller PROVED with the cluster key, which the node logs and records. A gate may not have
/// that, and should not: under a shared key every holder can mint any id tag, so the label
/// authenticates nothing and a gate that read it would be deciding on a value it cannot check.
/// What a gate acts on is the two facts below, and `Distributed::ExplainConnection` -- the one
/// function every membership gate folds through -- takes exactly a host and a `bool` for the
/// same reason ([#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428)).
///
/// So this is a NARROWING rather than a second spelling of the same thing, and the narrowing is
/// what makes it shareable: `fastcached` implements `ILiveGate` too and holds no cluster key, so
/// `provedClusterKey` is honestly `false` there rather than a plausible default it must invent.
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

    /// Whether this connection proved the cluster key.
    ///
    /// A `bool` and never the proven id: an id is legitimately EMPTY -- one is minted into
    /// `--cluster-dir` only by a node that runs consensus -- so a field carrying the label
    /// could not tell an ordinary keyed worker from a caller that proved nothing.
    bool provedClusterKey = false;
};

} // namespace FastCache
