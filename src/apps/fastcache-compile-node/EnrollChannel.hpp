// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "EnrollClient.hpp"

#include <cstddef>
#include <span>
#include <string_view>

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
    /// @param nonce What the caller drew for this one request: an admission is signed over it.
    /// @return What the answer means; `EnrollProgress::Fatal` naming the endpoint when it could not
    ///         be reached at all.
    [[nodiscard]] virtual EnrollReading Poll(std::string_view endpoint,
                                             JoinerIdentity const& self,
                                             std::span<std::byte const> nonce) = 0;
};

/// `IEnrollChannel` over a dialled connection: one connection, one request, one answer.
///
/// `RunEnrollClient`'s loop body for one request, with the loop left to the caller: the dial bounded
/// by `EnrollDialTimeout`, the exchange by `Cc::ExchangeFramed`'s own round-trip bound, so one poll
/// holds the calling thread for at most one exchange.
///
/// **It presents NO credential, and that is its signature rather than its configuration.** `Enroll`
/// is answered before authentication, and the endpoint it goes to is whatever a beacon, a DNS SRV
/// record or a seed named -- any machine on the network can be it. A `--requirepass` presented here
/// went out in the clear, pipelined ahead of the request and of any seal, once a beat, to whoever
/// answered. Nothing an approval decides depends on a password either: the joiner is admitted on
/// its key, and the answer is believed on a signature. So there is no `ICredentialSource` among the
/// constructor's parameters or the members, and presenting one again is a new parameter, never a
/// forgotten argument (`[node][credential][seam]` holds the node's sources to a list).
class DialledEnrollChannel final: public IEnrollChannel
{
  public:
    /// @param dialer How an endpoint is reached; must outlive this.
    explicit DialledEnrollChannel(IEndpointDialer& dialer);

    /// @copydoc IEnrollChannel::Poll
    [[nodiscard]] EnrollReading Poll(std::string_view endpoint,
                                     JoinerIdentity const& self,
                                     std::span<std::byte const> nonce) override;

  private:
    IEndpointDialer& _dialer;
};

} // namespace FastCache::Node
