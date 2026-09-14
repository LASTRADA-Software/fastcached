// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScriptedSocket.hpp>
#include <tests/SocketDecorator.hpp>

namespace FastCache::Testing
{

/// Hands back one scripted socket per dial, and records where it was asked to go.
///
/// The seam `RunEnrollClient`, `RunEnrollAdmin`, `RunClusterAdmin` and `AnnounceRound`
/// take, standing in for `BlockingEndpointDialer`. A fresh socket per dial is what
/// production does -- a poll re-dials, and a redirect or a fallback moves the endpoint
/// -- so one script per dial is the shape, not a convenience.
///
/// **One fake for every caller of the seam**, for `ScriptedSocket.hpp`'s reason: it
/// began as a copy inside `EnrollClient_test.cpp`, and a second copy for the heartbeat
/// would have been a fake nothing else exercises, drifting in silence.
///
/// **A script that RAN OUT and a dial that FAILED are different facts, and this fake
/// refuses to spell them the same way.** Production answers `nullptr` for a failed
/// dial and the callers report it as *cannot reach ...*. If exhaustion answered
/// `nullptr` too, a case that dialled once more than its author anticipated would
/// redden with that same sentence -- pointing a reader at the connector, at
/// `BlockingEndpointDialer`, at anything but the property the case is named for. That
/// is a true failure carrying a false diagnosis, relocated into the scaffolding where
/// nothing looks wrong because a fake is always more convenient than the thing it
/// stands for.
///
/// So the two are separated: an EMPTY entry is a dial this script fails deliberately,
/// and running past the end is a `FAIL` in the fake's own voice naming how far the
/// code under test got.
///
/// **The sockets outlive the dial.** Each connected dial hands out a decorator over a
/// `ScriptedSocket` this fake keeps, so a case can read what was SENT to the endpoint
/// that answered after the caller has dropped its connection -- which is how a fallback
/// case asserts WHICH endpoint took the request rather than only that one did.
class ScriptedDialer final: public Node::IEndpointDialer
{
  public:
    /// @param replies One framed reply per dial, in order; an empty entry fails that dial.
    explicit ScriptedDialer(std::vector<std::vector<std::byte>> replies):
        _replies { std::move(replies) }
    {
    }

    /// @copydoc Node::IEndpointDialer::Dial
    [[nodiscard]] std::unique_ptr<ISocket> Dial(std::string_view endpoint, DialOptions /*options*/) override
    {
        _dialed.emplace_back(endpoint);
        if (_next >= _replies.size())
        {
            FAIL("scripted dialer exhausted: the code under test dialled "
                 << _dialed.size() << " time(s) against a script of " << _replies.size()
                 << ". That is this fixture running out, NOT a dial failure -- read it as the loop "
                    "going further than this case anticipated");
            return nullptr;
        }

        auto const& frame = _replies[_next++];
        if (frame.empty())
        {
            // A dial this script fails on purpose, which production spells the same way.
            _sockets.emplace_back(nullptr);
            return nullptr;
        }
        _sockets.push_back(std::make_unique<ScriptedSocket>(frame));
        return std::make_unique<SocketDecorator>(*_sockets.back());
    }

    /// @return Every endpoint dialled, in order.
    [[nodiscard]] std::vector<std::string> const& Dialed() const noexcept
    {
        return _dialed;
    }

    /// What the endpoint of dial @p index was sent.
    /// @param index Which dial, counting from zero, failed dials included.
    /// @return The bytes written to it; empty for a dial that failed.
    [[nodiscard]] std::span<std::byte const> SentOn(std::size_t index) const
    {
        REQUIRE(index < _sockets.size());
        if (_sockets[index] == nullptr)
            return {};
        return _sockets[index]->Sent();
    }

  private:
    std::vector<std::vector<std::byte>> _replies;
    std::vector<std::string> _dialed;
    /// One per dial, null where the dial failed, so an index is a dial's index.
    std::vector<std::unique_ptr<ScriptedSocket>> _sockets;
    std::size_t _next { 0 };
};

} // namespace FastCache::Testing
