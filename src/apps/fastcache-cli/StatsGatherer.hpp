// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "RespClient.hpp"
#include "StatsSource.hpp"

#include <optional>
#include <string>
#include <vector>

namespace FastCache::Cli
{

/// Asks every stats source this build knows about.
///
/// The impure half of the ladder, kept apart from `StatsSource.hpp`'s decision so that
/// `ChooseStats` -- which is where all the interesting behaviour is -- needs no socket
/// to test. This class does the opposite: it is all socket and no decision.
class LadderGatherer final: public IStatsGatherer, public IAdminDocument
{
  public:
    /// @param admin Where `/metrics` is; an unconfigured endpoint means *do not ask*,
    ///        which is reported as `asked == false` rather than as a failure.
    /// @param timeouts How long to wait.
    /// @param bearer A credential for the admin surface, or nullopt. `/metrics` needs
    ///        none today -- it short-circuits above the credential gate -- but a
    ///        deployment may still sit behind something that does.
    /// @param resp The already-open cache connection, or null when there is none.
    /// @param node The already-open `0xFC` connection, or null when there is none. It
    ///        serves two rungs rather than one: it answers `NodeMetrics` itself, and it
    ///        is how the admin address is DISCOVERED when the operator named none --
    ///        which is what makes `--admin-addr` redundant rather than required.
    /// @param cache Where this invocation dialled. The HOST half is what a discovered
    ///        admin port is dialled on, and the PORT half is what a discovered one is
    ///        checked against -- see `ResolveAdmin`.
    LadderGatherer(Endpoint admin,
                   Endpoint cache,
                   DialTimeouts timeouts,
                   std::optional<std::string> bearer,
                   IExchange* resp,
                   INodeExchange* node) noexcept;

    [[nodiscard]] std::vector<StatsAttempt> Gather() override;

    /// Fetch one document from the admin surface this endpoint reported.
    ///
    /// Public because a VERB needs it, where `ResolveAdmin` below stays private: the
    /// address is this class's business and the document is the caller's. It reuses
    /// the identity `Identify()` already cached, so a verb pays no second `NodeStatus`
    /// round trip for asking.
    /// @param path An absolute path, query string included.
    /// @return The body, or why there is none.
    [[nodiscard]] std::expected<std::string, AdminError> FetchAdmin(std::string_view path) override;

  private:
    /// What the endpoint is, asked ONCE and remembered.
    ///
    /// **One round trip serves two rungs**, and that is not an optimisation -- it is
    /// what keeps the ladder quiet against a plain daemon. `/metrics` needs the admin
    /// PORT and `node-metrics` needs to know the verb will be answered at all, and both
    /// are settled by the same `NodeStatus`. Asking twice would send two requests a
    /// daemon cannot serve and report two failures for one fact.
    ///
    /// The three states are kept apart deliberately: a node that ANSWERED, an endpoint
    /// that is not a node, and a question nobody could ask because no `0xFC` connection
    /// was opened. The second is what makes `node-metrics` report *was not asked*
    /// rather than *did not answer* against a daemon -- an endpoint that cannot serve a
    /// verb was not consulted about it, and saying otherwise sends an operator to check
    /// a component that is not there.
    /// @return The node's own description, or why there is none.
    [[nodiscard]] std::expected<CompileCacheWire::NodeStatusFields, std::string> const& Identify();

    /// Where the admin surface is, asking the node when the operator named nowhere.
    ///
    /// **This is what retires `--admin-addr`.** A node already knows which port its
    /// admin surface bound and whether it is TLS, and it will say so over `0xFC` -- so
    /// an operator who can reach the node can reach its metrics without being told a
    /// second address. The flag stays as an override, for the deployments where the
    /// node is reached through something that rewrites ports.
    ///
    /// The HOST is never taken from the node. A node reports a PORT; the host to dial is
    /// the one this client already reached it on, because that is the only address known
    /// to route from here -- a node behind NAT would otherwise hand out an address only
    /// its own network can use.
    /// @return The endpoint, or why there is none.
    [[nodiscard]] std::expected<Endpoint, std::string> ResolveAdmin();

    /// Ask the admin surface for `/metrics`.
    /// @return What happened.
    [[nodiscard]] StatsAttempt AskMetrics();

    /// Ask the node for its own counters.
    /// @return What happened.
    [[nodiscard]] StatsAttempt AskNodeMetrics();

    /// Ask the cache for `INFO`.
    /// @return What happened.
    [[nodiscard]] StatsAttempt AskInfo();

    Endpoint _admin;
    Endpoint _cache;
    DialTimeouts _timeouts;
    std::optional<std::string> _bearer;
    IExchange* _resp;
    INodeExchange* _node;
    /// `Identify`'s answer, computed on first use. Cached because it is the SAME fact
    /// for both rungs and a second ask is a second request a daemon refuses.
    std::optional<std::expected<CompileCacheWire::NodeStatusFields, std::string>> _identity;
};

} // namespace FastCache::Cli
