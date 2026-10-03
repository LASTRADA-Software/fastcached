// SPDX-License-Identifier: Apache-2.0
#include "EnrollChannel.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <format>
#include <string>

#include <CacheProtocol.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

DialledEnrollChannel::DialledEnrollChannel(IEndpointDialer& dialer, Ed25519KeyPair const& identity):
    _dialer { dialer },
    _identity { identity }
{
}

EnrollReading DialledEnrollChannel::Poll(std::string_view endpoint,
                                         JoinerIdentity const& self,
                                         std::span<std::byte const> nonce)
{
    auto client = _dialer.Dial(endpoint, core::net::DialOptions { .connectTimeout = EnrollDialTimeout });
    if (client == nullptr)
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = std::format("cannot reach {}", endpoint),
                               .roster = {} };

    // Silent and never consulted: a notice reports a credential the peer ignored, and this exchange
    // presents none -- by name, never by a defaulted argument.
    auto notice = Cc::CredentialNotice::Silent();
    return ReadEnrollReply(core::async::syncRun(
        Cc::ExchangeFramed(client.get(), &notice, EncodeSignedEnroll(self, _identity, nonce), NoCredential())));
}

} // namespace FastCache::Node
