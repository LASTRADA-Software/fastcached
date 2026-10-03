// SPDX-License-Identifier: Apache-2.0
#include "NodeCredential.hpp"
#include "NodeSurfaces.hpp"
#include "OperatorCredentials.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/TicketChoice.hpp>

#include <utility>

#include <core/async/SyncRun.hpp>

namespace FastCache::Node
{

DialedExchange::DialedExchange(IEndpointDialer& dialer,
                               Cc::CredentialNotice& notice,
                               core::net::DialOptions options) noexcept:
    _dialer { dialer },
    _notice { notice },
    _options { options }
{
}

Cc::CacheOutcome DialedExchange::Exchange(std::string_view hostPort,
                                          std::vector<std::byte> frame,
                                          Cc::Credential const& credential,
                                          Cc::ExchangeBudget /*budget*/)
{
    auto client = _dialer.Dial(hostPort, _options);
    if (client == nullptr)
        return Cc::CacheOutcome {}; // `Transport`, `Unreached`: nothing was sent
    return core::async::syncRun(Cc::ExchangeFramed(client.get(), &_notice, std::move(frame), credential));
}

std::optional<std::string> OwnNodeTicketSource(NodeConfig const& cfg)
{
    auto const bound = RowFor(NodeSurface::Node).Resolve(cfg);
    if (bound.empty())
        return std::nullopt;
    return Cc::TicketSourceFor(FormatHostPort(bound.front().host, bound.front().port));
}

OperatorCredentials::OperatorCredentials(NodeConfig const& cfg,
                                         IEndpointDialer& dialer,
                                         std::function<void(std::string_view)> say):
    _notice { say },
    _raw { dialer, _notice, core::net::DialOptions { .connectTimeout = MintDialTimeout } },
    // The password through the one seam that derives a credential from the configuration, and to
    // `--upstream` alone: every other endpoint admits a machine by its ticket or its node proof
    // and checks no password, so presenting it there only hands the upstream's secret away.
    _tickets { _raw,         OwnNodeTicketSource(cfg), ConfiguredCredential { cfg, nullptr }.Current(),
               cfg.upstream, Cc::ExchangeBudget {},    std::move(say) }
{
}

} // namespace FastCache::Node
