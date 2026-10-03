// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "EnrollClient.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Core/Logger.hpp>

#include <string_view>

#include <CacheProtocol.hpp>

namespace FastCache::Node
{

/// @file EnrollChannel.hpp
/// One `Enroll` exchange with one endpoint: what a pending node asks, once per beat, of the fleet it
/// asked to join.

/// Asks one endpoint to admit this node, once, and reads the answer.
///
/// A seam, so a formation controller decides from a scripted reading in a test and from a dialled
/// exchange in production. One exchange per call and no loop: the caller owns the cadence, the
/// redirects and the give-up, so none of them is a sleep inside something a test cannot reach.
class IEnrollChannel
{
  public:
    IEnrollChannel() = default;
    IEnrollChannel(IEnrollChannel const&) = delete;
    IEnrollChannel& operator=(IEnrollChannel const&) = delete;
    IEnrollChannel(IEnrollChannel&&) = delete;
    IEnrollChannel& operator=(IEnrollChannel&&) = delete;
    virtual ~IEnrollChannel() = default;

    /// Ask @p endpoint to admit @p self, once.
    /// @param endpoint The `host:port` of the `0xFC` port to ask.
    /// @param self Who this node asks to be admitted as.
    /// @return What the answer means; `EnrollProgress::Fatal` naming the endpoint when it could not
    ///         be reached at all.
    [[nodiscard]] virtual EnrollReading Poll(std::string_view endpoint, JoinerIdentity const& self) = 0;
};

/// `IEnrollChannel` over a dialled connection: one connection, one request, one answer.
///
/// `RunEnrollClient`'s loop body for one request, with the loop left to the caller: the dial bounded
/// by `EnrollDialTimeout`, the exchange by `Cc::ExchangeFramed`'s own round-trip bound, so one poll
/// holds the calling thread for at most one exchange.
class DialledEnrollChannel final: public IEnrollChannel
{
  public:
    /// @param dialer How an endpoint is reached; must outlive this.
    /// @param credential What to present, read where it is presented; must outlive this.
    /// @param logger Where a credential the fleet ignored is reported, once; must outlive this.
    DialledEnrollChannel(IEndpointDialer& dialer, ICredentialSource const& credential, ILogger& logger);

    /// @copydoc IEnrollChannel::Poll
    [[nodiscard]] EnrollReading Poll(std::string_view endpoint, JoinerIdentity const& self) override;

  private:
    IEndpointDialer& _dialer;
    ICredentialSource const& _credential;
    Cc::CredentialNotice _notice; ///< Held across polls, so an ignored credential is said once, not once a beat.
};

} // namespace FastCache::Node
