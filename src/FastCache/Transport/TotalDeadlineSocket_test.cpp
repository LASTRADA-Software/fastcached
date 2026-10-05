// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Transport/TotalDeadlineSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <core/net/ISocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ScriptedSocket.hpp>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{
/// A socket shaped like a TLS one where it matters here: its handshake READS from the peer, so
/// what bounds that read is whatever receive deadline was armed when the handshake began.
class TlsShapedSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param inner The transport the handshake would read from; must outlive this.
    explicit TlsShapedSocket(core::net::ISocket& inner):
        SocketDecorator { inner }
    {
    }

    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        _armed = deadline;
        SocketDecorator::setReceiveDeadline(deadline);
    }

    [[nodiscard]] core::net::ResultAwaitable<void> handshakeIfNeeded() override
    {
        ++_handshakes;
        _armedAtHandshake = _armed;
        return SocketDecorator::handshakeIfNeeded();
    }

    /// @return The receive deadline in force when the handshake began; nullopt when none was armed.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ArmedAtHandshake() const noexcept
    {
        return _armedAtHandshake;
    }

    /// @return How many handshakes reached this socket.
    [[nodiscard]] int Handshakes() const noexcept
    {
        return _handshakes;
    }

  private:
    std::optional<std::chrono::milliseconds> _armed;
    std::optional<std::chrono::milliseconds> _armedAtHandshake;
    int _handshakes { 0 };
};
} // namespace

TEST_CASE("A TLS handshake is bounded by the exchange's deadline, as a read is", "[transport][deadline][net]")
{
    // The handshake reads from the peer, so a peer that dribbles its half of it would hold the
    // exchange past the one deadline over the whole of it -- unless the handshake is armed with
    // the time left exactly as a read is.
    core::platform::ManualClock clock;
    Testing::ScriptedSocket scripted { std::vector<std::byte> {} };
    auto tls = std::make_unique<TlsShapedSocket>(scripted);
    auto const& seen = *tls;
    auto socket = TotalDeadlineSocket { std::move(tls), clock, clock.now() + 5s };

    clock.advance(2s);
    (void) socket.handshakeIfNeeded();
    REQUIRE(seen.Handshakes() == 1);
    CHECK(seen.ArmedAtHandshake() == std::optional { std::chrono::milliseconds { 3000 } });
    CHECK_FALSE(socket.Expired());

    // Past the deadline, a handshake is refused outright and never reaches the peer.
    clock.advance(3s);
    (void) socket.handshakeIfNeeded();
    CHECK(seen.Handshakes() == 1);
    CHECK(socket.Expired());
}
