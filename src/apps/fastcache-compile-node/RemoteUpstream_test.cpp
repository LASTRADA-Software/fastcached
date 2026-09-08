// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/IAsyncAddressResolver.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

constexpr std::chrono::milliseconds RefreshInterval { 30'000 };
constexpr std::chrono::milliseconds ConnectTimeout { 1'000 };
constexpr std::chrono::milliseconds IoTimeout { 5'000 };

/// A resolver that counts what it was asked, and answers with a fixed address.
///
/// The count IS the subject of every case here: the defect was one `getaddrinfo`
/// per cache operation, so what has to be observed is how many lookups a run of
/// operations costs -- not whether an operation succeeded, which it does either way.
class CountingResolver final: public IAsyncAddressResolver
{
  public:
    explicit CountingResolver(std::vector<ResolvedEndpoint> resolved) noexcept:
        answer { std::move(resolved) }
    {
    }

    [[nodiscard]] Task<ResolveResult> Resolve(std::string host, std::uint16_t port, IReactor* /*reactor*/) override
    {
        ++calls;
        lastHost = std::move(host);
        lastPort = port;
        if (fail)
            co_return std::unexpected(ResolveFailure(lastHost, port, "scripted failure"));
        co_return answer;
    }

    /// Every member is public and there is no private section, which is what keeps
    /// `cppcoreguidelines-non-private-member-variables-in-classes` quiet: it ignores a
    /// type whose member variables are ALL public, and fires on a type that mixes the
    /// two. A scripted fake is a bag of knobs, so all-public is also the honest shape.
    int calls { 0 };
    bool fail { false };
    std::string lastHost;
    std::uint16_t lastPort { 0 };
    std::vector<ResolvedEndpoint> answer;
};

/// A connector whose dial always fails.
///
/// Deliberate, and it is what makes the "never on a miss" case mean something: every
/// operation below therefore ends in the failure path -- `Fetch` reports a miss,
/// `Store` declines -- which is exactly the condition a miss-triggered refresh would
/// re-resolve on. A connector that succeeded would leave that path unexercised.
class FailingConnector final: public IConnector
{
  public:
    [[nodiscard]] Task<SocketResult> Connect(std::string host, std::uint16_t /*port*/, DialOptions /*options*/) override
    {
        ++dials;
        lastHost = std::move(host);
        co_return std::unexpected(NetError { .code = NetErrorCode::ConnRefused, .context = "scripted" });
    }

    int dials { 0 };
    std::string lastHost;
};

/// Resolve a literal through the real seam, so the fake answers with a genuine
/// `ResolvedEndpoint` rather than hand-built sockaddr bytes.
/// @return One endpoint for 127.0.0.1, or empty when the platform refused.
[[nodiscard]] std::vector<ResolvedEndpoint> LoopbackEndpoint()
{
    SystemAddressResolver resolver;
    auto resolved = resolver.Resolve("127.0.0.1", 6674);
    if (!resolved.has_value())
        return {};
    return *resolved;
}

struct Fixture
{
    std::vector<ResolvedEndpoint> answer { LoopbackEndpoint() };
    CountingResolver resolver { answer };
    FailingConnector connector;
    ManualClock clock;

    [[nodiscard]] RemoteUpstream Make(std::string endpoint = "cache.example:6674")
    {
        return RemoteUpstream {
            std::move(endpoint), Cc::Credential {}, [](std::string_view) {}, connector, nullptr, resolver, clock,
            ConnectTimeout,      IoTimeout,         RefreshInterval
        };
    }
};

} // namespace

TEST_CASE("RemoteUpstream resolves once per interval, not once per operation")
{
    Fixture fixture;
    if (fixture.answer.empty())
        SKIP("this host could not resolve 127.0.0.1, so there is no address to hold");

    auto upstream = fixture.Make();

    for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
    {
        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
        CHECK(SyncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
    }

    INFO("ten operations inside one interval");
    CHECK(fixture.resolver.calls == 1);
    CHECK(fixture.connector.dials == 10);
    CHECK(fixture.resolver.lastHost == "cache.example");
    CHECK(fixture.resolver.lastPort == 6674);
}

TEST_CASE("RemoteUpstream re-resolves once the interval has passed")
{
    Fixture fixture;
    if (fixture.answer.empty())
        SKIP("this host could not resolve 127.0.0.1, so there is no address to hold");

    auto upstream = fixture.Make();

    CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
    REQUIRE(fixture.resolver.calls == 1);

    // Just short of the interval is still the held address.
    fixture.clock.Advance(RefreshInterval - std::chrono::milliseconds { 1 });
    CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.resolver.calls == 1);

    // Reaching it re-resolves exactly once, however many operations follow.
    fixture.clock.Advance(std::chrono::milliseconds { 1 });
    CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
    CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.resolver.calls == 2);
}

TEST_CASE("RemoteUpstream does not re-resolve because a dial failed")
{
    // The rule this pins is not "resolution is cached" but "a miss is not a trigger".
    // A refresh driven by a failed dial hands a remote peer a free amplifier: one
    // forced lookup per request, simply by asking for keys this cache does not hold.
    // Every dial here fails, so a miss-triggered implementation would resolve twenty
    // times where this asserts one.
    Fixture fixture;
    if (fixture.answer.empty())
        SKIP("this host could not resolve 127.0.0.1, so there is no address to hold");

    auto upstream = fixture.Make();

    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());

    CHECK(fixture.resolver.calls == 1);
    CHECK(fixture.connector.dials == 20);
}

TEST_CASE("RemoteUpstream keeps serving while resolution is failing, and retries on the interval")
{
    Fixture fixture;
    if (fixture.answer.empty())
        SKIP("this host could not resolve 127.0.0.1, so there is no address to hold");

    fixture.resolver.fail = true;
    auto upstream = fixture.Make();

    // A failed lookup must not reset the timer either, or a resolver that is down is
    // retried once per operation -- the amplifier again, by the other door.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.resolver.calls == 1);

    // It still dialled: a failed lookup falls back to the configured name, which is
    // what this class did before it held an address at all.
    CHECK(fixture.connector.dials == 5);
}

TEST_CASE("RemoteUpstream resolves nothing for an endpoint that does not parse")
{
    Fixture fixture;
    auto upstream = fixture.Make("not-an-endpoint");

    CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());

    // Nothing to hold an address for, so the name goes to the dial exactly as before
    // and the resolver is never consulted.
    CHECK(fixture.resolver.calls == 0);
}

TEST_CASE("RemoteUpstream holds only a unique address, so a multi-answer name keeps its fallback")
{
    // `Detail::RunConnectFlow` tries EVERY candidate a name resolves to, and its own
    // comment names the case it exists for: an AAAA on a machine with no IPv6 route.
    // Handing the connector one pinned literal would destroy that for a whole refresh
    // interval -- and silently, since an unreachable upstream is reported as a cache
    // miss by design. So a name with more than one answer must still be dialled BY
    // NAME, and only a unique answer is held.
    Fixture fixture;
    if (fixture.answer.empty())
        SKIP("this host could not resolve 127.0.0.1, so there is no address to hold");

    SECTION("one answer is held, and the dial goes to the address")
    {
        auto upstream = fixture.Make();
        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
        CHECK(fixture.connector.lastHost == "127.0.0.1");
    }

    SECTION("two answers are not held, and the dial goes to the name")
    {
        auto twice = fixture.answer;
        twice.push_back(fixture.answer.front());
        CountingResolver multi { twice };
        RemoteUpstream upstream { "cache.example:6674", Cc::Credential {}, [](std::string_view) {},
                                  fixture.connector,    nullptr,           multi,
                                  fixture.clock,        ConnectTimeout,    IoTimeout,
                                  RefreshInterval };

        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
        CHECK(fixture.connector.lastHost == "cache.example");

        // Still only one lookup per interval: the name is dialled, but this class does
        // not ask the resolver again inside the window.
        CHECK_FALSE(SyncRun(upstream.Fetch("k")).has_value());
        CHECK(multi.calls == 1);
    }
}
