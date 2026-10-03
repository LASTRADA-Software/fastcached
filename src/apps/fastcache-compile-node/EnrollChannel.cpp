// SPDX-License-Identifier: Apache-2.0
#include "EnrollChannel.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <format>
#include <string>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

DialledEnrollChannel::DialledEnrollChannel(IEndpointDialer& dialer, ICredentialSource const& credential, ILogger& logger):
    _dialer { dialer },
    _credential { credential },
    _notice { [&logger](std::string_view text) { logger.Logf(LogLevel::Warn, "formation: {}", text); } }
{
}

EnrollReading DialledEnrollChannel::Poll(std::string_view endpoint, JoinerIdentity const& self)
{
    auto client = _dialer.Dial(endpoint, core::net::DialOptions { .connectTimeout = EnrollDialTimeout });
    if (client == nullptr)
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = std::format("cannot reach {}", endpoint),
                               .roster = {},
                               .certificate = {} };

    return ReadEnrollReply(core::async::syncRun(Cc::ExchangeFramed(
        client.get(),
        &_notice,
        Wire::EncodeEnroll(Wire::EnrollRequest {
            .nodeId = self.nodeId, .nodeEndpoint = self.nodeEndpoint, .role = self.role, .publicKey = self.publicKey }),
        _credential.Current())));
}

} // namespace FastCache::Node
