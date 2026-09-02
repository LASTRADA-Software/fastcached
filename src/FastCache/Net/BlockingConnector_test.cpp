// SPDX-License-Identifier: Apache-2.0
//
// The outbound half of the network seam. What matters here is not that a
// successful dial works -- a listener two lines away proves that -- but that
// every way it can *fail* is reported as a distinguishable, bounded outcome.
// A connector that hangs cannot be shut down, and one that reports every
// failure as "system error" tells an operator nothing about a peer that is
// refusing versus one that is unreachable.
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A resolver that answers with whatever it was told to, so a test can dial an
/// endpoint without a name lookup — and can produce the "resolution failed"
/// branch, which no real hostname reliably reproduces.
class ScriptedResolver final: public IAddressResolver
{
  public:
    /// @param inner Real resolver used to build genuine endpoints.
    explicit ScriptedResolver(IAddressResolver& inner) noexcept:
        _inner { inner }
    {
    }

    /// Make the next resolution fail.
    /// @param reason What to report.
    void FailWith(std::string reason)
    {
        _failure = std::move(reason);
    }

    /// Prepend a candidate that will be tried before the real one.
    /// @param host Host to resolve for the extra candidate.
    /// @param port Port for the extra candidate.
    void PrependCandidate(std::string_view host, std::uint16_t port)
    {
        auto const extra = _inner.Resolve(host, port);
        if (extra.has_value() && !extra->empty())
            _prefix.push_back(extra->front());
    }

    /// @copydoc IAddressResolver::Resolve
    [[nodiscard]] std::expected<std::vector<ResolvedEndpoint>, std::string> Resolve(std::string_view host,
                                                                                    std::uint16_t port) override
    {
        if (!_failure.empty())
            return std::unexpected { _failure };

        auto resolved = _inner.Resolve(host, port);
        if (!resolved.has_value())
            return resolved;

        std::vector<ResolvedEndpoint> out { _prefix };
        out.insert(out.end(), resolved->begin(), resolved->end());
        return out;
    }

  private:
    IAddressResolver& _inner;
    std::string _failure;
    std::vector<ResolvedEndpoint> _prefix;
};

/// Bind a listener on an ephemeral port.
/// @return The listener, or nullptr when the platform would not bind.
[[nodiscard]] std::unique_ptr<BlockingListener> BindEphemeral()
{
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return nullptr;
    return listener;
}

} // namespace

TEST_CASE("A connector reaches a listener that is up", "[net][connector]")
{
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }

    BlockingConnector connector;
    auto const socket = SyncRun(connector.Connect("127.0.0.1", listener->BoundPort(), DialOptions { .connectTimeout = 2s }));
    REQUIRE(socket.has_value());
    CHECK(*socket != nullptr);
    CHECK_FALSE((*socket)->IsClosed());
}

TEST_CASE("A dial that cannot succeed fails within its timeout", "[net][connector]")
{
    // The property the non-blocking implementation exists for, and the only one
    // that is the same everywhere. A plain blocking connect is governed by the
    // kernel's own retry schedule -- minutes on some systems -- so a caller that
    // wants to notice a dead peer, or simply to shut down, cannot be made to
    // wait for it.
    //
    // What the failure is called is deliberately NOT asserted exactly. A closed
    // loopback port answers with a reset on a bare host and is silently dropped
    // where a host firewall is in the way, so the same code is ConnRefused on
    // one machine and Timeout on the next; pinning either makes the suite fail
    // for a reason that is about the machine. What must hold on both is that it
    // fails, quickly, and says which of the two happened.
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }
    auto const port = listener->BoundPort();
    listener.reset();

    constexpr auto Timeout = 300ms;
    BlockingConnector connector;

    auto const started = std::chrono::steady_clock::now();
    auto const socket = SyncRun(connector.Connect("127.0.0.1", port, DialOptions { .connectTimeout = Timeout }));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(socket.has_value());

    // Generously bounded -- the assertion is "it returned rather than parking",
    // not a latency measurement, and a loaded CI runner is allowed to be slow.
    CHECK(elapsed < Timeout * 20);

    CAPTURE(socket.error().context, socket.error().systemCode);
    auto const code = socket.error().code;
    CHECK((code == NetErrorCode::ConnRefused || code == NetErrorCode::Timeout));

    // Whichever it was, it is actionable: "connection refused" sends an operator
    // to the peer's configuration, "timed out" to the network between them.
    CHECK_FALSE(socket.error().context.empty());
}

TEST_CASE("A resolution failure is reported without dialling", "[net][connector]")
{
    ScriptedResolver resolver { DefaultAddressResolver() };
    resolver.FailWith("scripted resolution failure");

    BlockingConnector connector { resolver };
    auto const socket = SyncRun(connector.Connect("example.invalid", 1, DialOptions { .connectTimeout = 2s }));
    REQUIRE_FALSE(socket.has_value());
    CHECK(socket.error().code == NetErrorCode::AddressNotAvail);

    // The reason travels with the refusal. A bare code would leave an operator
    // unable to tell a typo in a peer address from a DNS outage.
    CHECK(socket.error().context.contains("scripted resolution failure"));
}

TEST_CASE("A dead first candidate does not condemn the host", "[net][connector]")
{
    // The property a single-candidate dial silently loses. A peer whose name
    // resolves to several addresses -- an AAAA on a machine with no IPv6 route
    // is the ordinary case -- must be reached through whichever one works, or a
    // healthy peer is reported down for a reason that is about this machine.
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }

    auto dead = BindEphemeral();
    if (dead == nullptr)
    {
        SUCCEED("no second loopback listener available on this host");
        return;
    }
    auto const deadPort = dead->BoundPort();
    dead.reset();

    ScriptedResolver resolver { DefaultAddressResolver() };
    resolver.PrependCandidate("127.0.0.1", deadPort);

    BlockingConnector connector { resolver };
    auto const socket = SyncRun(connector.Connect("127.0.0.1", listener->BoundPort(), DialOptions { .connectTimeout = 2s }));
    // Report the reason rather than just "false": a dial has several ways to
    // fail and a bare assertion names none of them, which is the difference
    // between a diagnosis and an investigation.
    INFO("dial outcome: " << (socket.has_value() ? std::string { "connected" } : socket.error().ToString()));
    REQUIRE(socket.has_value());
    CHECK(*socket != nullptr);
}

TEST_CASE("A connector's failure carries the port it could not reach", "[net][connector]")
{
    ScriptedResolver resolver { DefaultAddressResolver() };
    resolver.FailWith("nope");

    BlockingConnector connector { resolver };
    auto const socket = SyncRun(connector.Connect("some-host", 6674, DialOptions { .connectTimeout = 1s }));
    REQUIRE_FALSE(socket.has_value());
    CHECK(socket.error().context.contains("some-host"));
    CHECK(socket.error().context.contains("6674"));
}
