// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <CacheProtocol.hpp>
#include <Dispatch.hpp>
#include <TicketCredentials.hpp>

namespace FastCache::Node
{

/// @file OperatorCredentials.hpp
/// What an operator's one-shot verb presents to each endpoint it dials: a machine ticket minted by
/// this machine's own node, the one password to the one endpoint it belongs to, or nothing.
///
/// The same `Cc::TicketCredentials` the launcher uses, so the decision is `Cc::ChooseCredential`'s
/// and the mint source is `Cc::TicketSourceFor`'s -- never a node twin of either.

/// What a refused exchange costs an operator's verb, which ends a failed mint's line
/// (`Cc::TicketCredentials`): unlike the launcher, it has no local fallback.
inline constexpr std::string_view RefusedVerbFails = "a refused exchange fails the verb";

/// One exchange over a connection this dialer opens for it.
///
/// `Cc::TicketCredentials` mints over a `Cc::IEndpointExchange`, and a one-shot verb runs on the
/// process main thread with no reactor to run `Cc::MakeTcpExchange` on. So this dials through the
/// seam every other one-shot dial takes and exchanges with `Cc::ExchangeFramed`.
///
/// **The CONNECT is the budget's**, because a dial enforces its own connect ceiling: that is what
/// lets `Cc::MintBudget` reach an operator verb's mint, which dialled under a fixed 2 s of its own
/// until #1669 -- on Windows the whole wait for a closed loopback port. The budget's total and idle
/// bounds need a reactor, which a one-shot verb does not run, so the dialer's per-call ceiling stands
/// in for them.
class DialedExchange final: public Cc::IEndpointExchange
{
  public:
    /// @param dialer How each endpoint is reached. Borrowed.
    /// @param notice Where "your credential went unchecked" is reported. Borrowed.
    DialedExchange(IEndpointDialer& dialer, Cc::CredentialNotice& notice) noexcept;

    /// @copydoc Cc::IEndpointExchange::Exchange
    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& credential,
                                            Cc::ExchangeBudget budget) override;

  private:
    IEndpointDialer& _dialer;
    Cc::CredentialNotice& _notice;
};

/// Where this machine's own node answers `MINT-TICKET`, as a verb this process runs and exits sees
/// it: `Cc::TicketSourceFor` of the first endpoint `--listen-node` resolves to under @p cfg -- a
/// loopback literal at that port, whatever host the node binds.
///
/// The operator's machine runs a node configured like this invocation far more often than one on a
/// default, which is the launcher's reasoning for `FASTCACHE_ADDR`'s port as well.
/// @param cfg The configuration the verb was invoked with.
/// @return `host:port` to mint from, or nullopt when @p cfg serves no node port.
[[nodiscard]] std::optional<std::string> OwnNodeTicketSource(NodeConfig const& cfg);

/// What an operator's one-shot verb presents to each endpoint it dials.
///
/// **Asked per endpoint** (`Cc::ICredentialFor`), so a verb that follows a `NotLeader` redirect
/// presents a ticket naming the leader it was sent to, never the scheduler it asked first. The
/// password -- `--requirepass` -- goes to `--upstream` alone, the one endpoint it belongs to. It is
/// read once, at construction: these verbs run before any reloader exists and exit without serving.
class OperatorCredentials
{
  public:
    /// @param cfg The configuration the verb was invoked with.
    /// @param dialer How this machine's node is reached for a mint. Borrowed.
    /// @param say Where a failed mint, or an unchecked credential, is said.
    OperatorCredentials(NodeConfig const& cfg, IEndpointDialer& dialer, std::function<void(std::string_view)> say);

    OperatorCredentials(OperatorCredentials const&) = delete;
    OperatorCredentials& operator=(OperatorCredentials const&) = delete;
    OperatorCredentials(OperatorCredentials&&) = delete;
    OperatorCredentials& operator=(OperatorCredentials&&) = delete;
    ~OperatorCredentials() = default;

    /// @return What each endpoint is shown.
    [[nodiscard]] Cc::ICredentialFor& Credentials() noexcept
    {
        return _tickets;
    }

  private:
    Cc::CredentialNotice _notice;
    DialedExchange _raw;
    Cc::TicketCredentials _tickets;
};

} // namespace FastCache::Node
