// SPDX-License-Identifier: Apache-2.0
#include "RemoteUpstream.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/SocketDecorator.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/ScriptedSocket.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

namespace Wire = FastCache::CompileCacheWire;

constexpr std::chrono::milliseconds RefreshInterval { 30'000 };
constexpr std::chrono::milliseconds ConnectTimeout { 1'000 };
constexpr std::chrono::milliseconds IoTimeout { 5'000 };

/// How long a failed exchange is believed here. Short against `RefreshInterval` on purpose: the
/// dial-failure case steps the clock by this twenty times and must stay inside ONE refresh.
constexpr std::chrono::milliseconds RetryInterval { 1'000 };
static_assert((20 * RetryInterval) < RefreshInterval);

/// The bounds every `RemoteUpstream` here is built with.
constexpr UpstreamTimings Timings { .connectTimeout = ConnectTimeout,
                                    .ioTimeout = IoTimeout,
                                    .addressRefreshInterval = RefreshInterval,
                                    .unreachableRetryInterval = RetryInterval };

/// A resolver that counts what it was asked, and answers with a fixed address.
///
/// The count IS the subject of every case here: the defect was one `getaddrinfo`
/// per cache operation, so what has to be observed is how many lookups a run of
/// operations costs -- not whether an operation succeeded, which it does either way.
class CountingResolver final: public core::net::IAsyncAddressResolver
{
  public:
    explicit CountingResolver(std::vector<core::net::ResolvedEndpoint> resolved) noexcept:
        answer { std::move(resolved) }
    {
    }

    [[nodiscard]] core::async::Task<core::net::ResolveResult> resolve(std::string host,
                                                                      std::uint16_t port,
                                                                      core::net::EventLoop* /*loop*/) override
    {
        ++calls;
        lastHost = std::move(host);
        lastPort = port;
        if (fail)
            co_return std::unexpected(core::net::resolveFailure(lastHost, port, "scripted failure"));
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
    std::vector<core::net::ResolvedEndpoint> answer;
};

/// A connector that counts its dials and either refuses every one or answers each with a `Miss`.
///
/// **Answering is the default, and since the unreachable memo that is what keeps the address
/// cases meaning anything.** A refused dial is now believed for `RetryInterval`, so a run of
/// operations over a refusing connector reaches `DialTarget` once, and every later operation
/// would assert nothing about resolution. An answering connector sends every operation through
/// the lookup again -- and a `Miss` is still the failure path a miss-triggered refresh would fire
/// on, so "never on a miss" keeps its teeth.
///
/// Each connected dial gets a socket of its own, kept here, because `RemoteUpstream` destroys the
/// one it was handed when the operation ends.
///
/// **Two ways a connected exchange goes unanswered, and they are not the same fixture.**
/// `ClosesUnanswered` is a peer that closes after reading the request: the first read is EOF, at
/// once, on any runner. `Stalls` is a peer that keeps the connection open and never answers, so
/// the read PARKS until the per-operation deadline closes the socket -- which needs a reactor to arm
/// that deadline on, so only a case that builds its `RemoteUpstream` over a `TestLoop` may use it.
///
/// **`dialCompletesOn` makes a dial take time.** Set, the dial is counted and then suspended onto
/// that executor, and completes only when the case drains it: the interleaving a reactor produces
/// when a second operation arrives while the first is still connecting.
///
/// Test-private, never transmitted or persisted, so the enumerators carry no explicit values.
enum class Peer : std::uint8_t
{
    Answers,          ///< Connects, and answers each request as a reachable cache: a FETCH `Miss`, a STORE declined.
    Refuses,          ///< The dial fails.
    ClosesUnanswered, ///< Connects, takes the request, and closes: the read is EOF.
    Stalls,           ///< Connects, takes the request, and never answers or closes.
};

/// A reachable cache's answer to whichever verb was sent: `Miss` to a FETCH, and to a STORE a
/// declined write (`StorageWriteFailed`) -- the "no object" each verb's `legalStatuses` admits.
///
/// One answer cannot serve both, because a reply is judged against the verb that was ASKED: a
/// `Miss` to a STORE is a status that verb never sends, so the launcher's exchange reads it as a
/// transport failure, and the unreachable memo then believes a live cache dead. The request is
/// written to a `ScriptedSocket` first, and the reply is chosen off its opcode at the first read.
class AnswersEachVerb final: public core::net::testing::SocketDecorator
{
  public:
    /// @param written Where the request lands; must outlive this.
    explicit AnswersEachVerb(Testing::ScriptedSocket& written) noexcept:
        SocketDecorator { written },
        _written { written }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        if (_reply == nullptr)
        {
            auto const header = Wire::DecodeRequestHeader(_written.Sent());
            auto const store = header.has_value() && header->opRaw == static_cast<std::uint8_t>(Wire::Op::Store);
            _reply = std::make_unique<Testing::ScriptedSocket>(
                store ? Wire::EncodeErrorReply(Wire::ErrorCode::StorageWriteFailed, "declined")
                      : Wire::EncodeReply(Wire::Status::Miss, std::vector<std::byte> {}));
        }
        return _reply->read(buffer);
    }

  private:
    Testing::ScriptedSocket& _written;
    std::unique_ptr<Testing::ScriptedSocket> _reply;
};

class CountingConnector final: public core::net::IConnector
{
  public:
    [[nodiscard]] core::async::Task<core::net::SocketResult> connect(std::string host,
                                                                     std::uint16_t /*port*/,
                                                                     core::net::DialOptions /*options*/) override
    {
        ++dials;
        lastHost = std::move(host);
        if (dialCompletesOn != nullptr)
            co_await core::async::ResumeOn { *dialCompletesOn };
        if (peer == Peer::Refuses)
            co_return std::unexpected(
                core::net::NetError { .code = core::net::NetErrorCode::ConnRefused, .context = "scripted" });
        if (peer == Peer::Stalls)
        {
            // The accepted end is kept and never written to or closed: the peer took the
            // connection and went quiet.
            auto pair = core::net::testing::InMemorySocketPair::create();
            stalledPeers.push_back(std::move(pair.server));
            co_return core::net::SocketResult { std::move(pair.client) };
        }
        sockets.push_back(std::make_unique<Testing::ScriptedSocket>(std::vector<std::byte> {}));
        if (peer == Peer::Answers)
            co_return core::net::SocketResult { std::make_unique<AnswersEachVerb>(*sockets.back()) };
        co_return core::net::SocketResult { std::make_unique<core::net::testing::SocketDecorator>(*sockets.back()) };
    }

    /// All public, for `CountingResolver`'s reason: a scripted fake is a bag of knobs.
    int dials { 0 };
    Peer peer { Peer::Answers };
    core::async::IExecutor* dialCompletesOn { nullptr };
    std::string lastHost;
    std::vector<std::unique_ptr<Testing::ScriptedSocket>> sockets;
    std::vector<std::unique_ptr<core::net::testing::InMemorySocket>> stalledPeers;
};

/// Resolve a literal through the real seam, so the fake answers with a genuine
/// `core::net::ResolvedEndpoint` rather than hand-built sockaddr bytes.
/// @return One endpoint for 127.0.0.1, or empty when the platform refused.
[[nodiscard]] std::vector<core::net::ResolvedEndpoint> LoopbackEndpoint()
{
    core::net::SystemAddressResolver resolver;
    auto resolved = resolver.resolve("127.0.0.1", 6674);
    if (!resolved.has_value())
        return {};
    return *resolved;
}

struct Fixture
{
    std::vector<core::net::ResolvedEndpoint> answer { LoopbackEndpoint() };
    CountingResolver resolver { answer };
    CountingConnector connector;
    core::platform::ManualClock clock;

    /// A node with no `--requirepass`, which is what every case here is about: these
    /// are the address-holding cases, and the credential is `NodeCredential_test`'s
    /// subject. `ConfiguredCredential` over an empty configuration and no reloader is
    /// production's own "no credential", rather than a second spelling of it.
    NodeConfig unauthenticated;
    ConfiguredCredential credential { unauthenticated, nullptr };

    /// @param endpoint What `--upstream` names.
    /// @param reactor Where the per-operation deadline is armed; null, as everywhere but the
    ///        stall case, arms none.
    /// @return An upstream over this fixture's fakes.
    [[nodiscard]] RemoteUpstream Make(std::string endpoint = "cache.example:6674", core::net::EventLoop* reactor = nullptr)
    {
        return RemoteUpstream {
            std::move(endpoint), credential, [](std::string_view) {}, connector, reactor, resolver, clock, Timings
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
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(core::async::syncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
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

    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    REQUIRE(fixture.resolver.calls == 1);

    // Just short of the interval is still the held address.
    fixture.clock.advance(RefreshInterval - std::chrono::milliseconds { 1 });
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.resolver.calls == 1);

    // Reaching it re-resolves exactly once, however many operations follow.
    fixture.clock.advance(std::chrono::milliseconds { 1 });
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
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

    // Every dial REFUSED, and the clock stepped one retry interval after each: an unreachable
    // upstream is now believed for that long, so without the step only the first fetch would
    // dial and nothing below would be about resolution. Twenty steps stay inside one refresh
    // interval (`static_assert`ed beside `RetryInterval`), so a refresh driven by a failed dial
    // would resolve twenty times where this asserts one.
    fixture.connector.peer = Peer::Refuses;
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
    {
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        fixture.clock.advance(RetryInterval);
    }

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
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.resolver.calls == 1);

    // It still dialled: a failed lookup falls back to the configured name, which is
    // what this class did before it held an address at all.
    CHECK(fixture.connector.dials == 5);
}

TEST_CASE("RemoteUpstream resolves nothing for an endpoint that does not parse")
{
    Fixture fixture;
    auto upstream = fixture.Make("not-an-endpoint");

    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());

    // Nothing to hold an address for, so the name goes to the dial exactly as before
    // and the resolver is never consulted.
    CHECK(fixture.resolver.calls == 0);
}

TEST_CASE("RemoteUpstream holds only a unique address, so a multi-answer name keeps its fallback")
{
    // `core::net::detail::runConnectFlow` tries EVERY candidate a name resolves to, and its own
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
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(fixture.connector.lastHost == "127.0.0.1");
    }

    SECTION("two answers are not held, and the dial goes to the name")
    {
        auto twice = fixture.answer;
        twice.push_back(fixture.answer.front());
        CountingResolver multi { twice };
        RemoteUpstream upstream { "cache.example:6674",
                                  fixture.credential,
                                  [](std::string_view) {},
                                  fixture.connector,
                                  nullptr,
                                  multi,
                                  fixture.clock,
                                  Timings };

        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(fixture.connector.lastHost == "cache.example");

        // Still only one lookup per interval: the name is dialled, but this class does
        // not ask the resolver again inside the window.
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(multi.calls == 1);
    }
}

TEST_CASE("An unreachable upstream is dialled once per retry interval and never once per miss", "[node][cache][upstream]")
{
    // Twenty local misses and twenty stores against a shared cache that refuses every dial, with
    // no time passing: the first pays the dial and every later one is answered from the memo.
    // Without it this was forty dials -- up to forty connect ceilings where the network drops
    // rather than refuses.
    Fixture fixture;
    fixture.connector.peer = Peer::Refuses;
    auto upstream = fixture.Make();

    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
    {
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(core::async::syncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
    }
    CHECK(fixture.connector.dials == 1);

    // One interval on, exactly one operation probes; the next inside that probe's window does not.
    fixture.clock.advance(RetryInterval);
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.connector.dials == 2);
}

TEST_CASE("An upstream that answers again is dialled for every operation from then on", "[node][cache][upstream]")
{
    Fixture fixture;
    fixture.connector.peer = Peer::Refuses;
    auto upstream = fixture.Make();
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    REQUIRE(fixture.connector.dials == 1);

    // The cache is back, and the memo does not know yet: the STALE-UNREACHABLE direction. It
    // costs a miss for at most one interval, which is the answer an unreachable cache already gave.
    fixture.connector.peer = Peer::Answers;
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.connector.dials == 1);

    // The probe connects, and from then on every operation dials with no clock movement at all.
    fixture.clock.advance(RetryInterval);
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(core::async::syncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
    CHECK(fixture.connector.dials == 4);
}

TEST_CASE("An upstream that accepts and then closes without answering is remembered like one that refused the dial",
          "[node][cache][upstream]")
{
    // The connection opens, the request goes out, and the peer closes without a reply: an exchange
    // that CONNECTED and was never answered counts exactly as a failed dial does. The stall that
    // the I/O deadline ends is the case after this one. Both staleness directions stay safe -- a
    // miss, or one wasted exchange.
    Fixture fixture;
    fixture.connector.peer = Peer::ClosesUnanswered;
    auto upstream = fixture.Make();

    for ([[maybe_unused]] auto const _: std::views::iota(0, 10))
    {
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(core::async::syncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
    }
    CHECK(fixture.connector.dials == 1);

    // One interval on, the probe finds the cache answering again, and the next operation dials too.
    fixture.clock.advance(RetryInterval);
    fixture.connector.peer = Peer::Answers;
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    CHECK(fixture.connector.dials == 3);
}

namespace
{

/// What one `Fetch` driven on a loop came back with, recorded by the flow itself.
struct FetchOnLoop
{
    bool finished { false };
    bool hit { false };
};

/// Run one `Fetch` on @p loop, recording into @p out when it ends.
///
/// Pointers rather than references: the flow outlives the call expression, and a coroutine
/// parameter by reference would bind to whatever the caller's expression held.
/// @param loop Where the flow runs, and where `RemoteUpstream` arms its deadline.
/// @param upstream The upstream under test; must outlive the flow.
/// @param out Where the outcome is recorded; must outlive the flow.
/// @return The detached flow.
core::async::DetachedTask FetchOn(core::net::testing::TestLoop* loop, RemoteUpstream* upstream, FetchOnLoop* out)
{
    co_await core::async::ResumeOn { *loop };
    auto const fetched = co_await upstream->Fetch("k");
    out->hit = fetched.has_value();
    out->finished = true;
}

} // namespace

TEST_CASE("An upstream that accepts and then stalls until the I/O deadline closes it is remembered like one "
          "that refused the dial",
          "[node][cache][upstream]")
{
    // The costliest failure to repeat per miss: the connection opens, the request goes out, and
    // nothing comes back until the per-operation deadline CLOSES the socket -- up to the connect
    // and I/O ceilings together. Driven on a real loop over the fixture's `ManualClock`, so what
    // ends the exchange is the deadline, not a scripted EOF.
    Fixture fixture;
    core::net::testing::TestLoop reactor { fixture.clock };
    fixture.connector.peer = Peer::Stalls;
    auto upstream = fixture.Make("cache.example:6674", &reactor);

    FetchOnLoop probe;
    FetchOn(&reactor, &upstream, &probe);
    (void) reactor.drain();
    // Dialled, wrote its request, and is parked on a read the peer will never answer.
    auto const parkedOnTheRead = !probe.finished;

    // ONE jump well past the deadline, for the reason `ReactorExchange_test`'s quiet-peer case
    // gives: the deadline is a bounded poll, and a single jump lets it finish inside one drain
    // without this case knowing the poll interval.
    fixture.clock.advance(IoTimeout * 4);
    (void) reactor.drain();
    auto const endedByTheDeadline = probe.finished;

    // Retrieve a flow the deadline did not end BEFORE asserting, so a red case cannot leave a
    // frame parked on a socket whose owner is about to be destroyed: the peer's close is EOF.
    if (!probe.finished)
    {
        for (auto const& peer: fixture.connector.stalledPeers)
            peer->close();
        (void) reactor.drain();
    }

    INFO("parkedOnTheRead=" << parkedOnTheRead << " endedByTheDeadline=" << endedByTheDeadline
                            << " dials=" << fixture.connector.dials);
    CHECK(parkedOnTheRead);
    CHECK(endedByTheDeadline);
    CHECK_FALSE(probe.hit);
    CHECK(fixture.connector.dials == 1);

    // Remembered: nothing dials until the interval has run. The connector REFUSES from here, so a
    // memo that forgot the stall is counted in dials rather than parked on a second one.
    fixture.connector.peer = Peer::Refuses;
    for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
    {
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
        CHECK(core::async::syncRun(upstream.Store("k", {})) == UpstreamStore::Declined);
    }
    CHECK(fixture.connector.dials == 1);
}

TEST_CASE("An operation arriving while the probe is still dialling does not dial too", "[node][cache][upstream]")
{
    // The probe is stamped when it is GRANTED, before its dial suspends. Here the dial really does
    // suspend -- the connector parks it on an executor this case drains by hand -- so a second
    // `Fetch` and a `Store` run while the first is mid-dial: the interleaving a reactor full of
    // misses produces, and one a synchronous run cannot reach.
    Fixture fixture;
    fixture.connector.peer = Peer::Refuses;
    auto upstream = fixture.Make();
    CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());
    fixture.clock.advance(RetryInterval);

    core::async::testing::ManualExecutor dialInFlight;
    fixture.connector.dialCompletesOn = &dialInFlight;
    auto probe = upstream.Fetch("k");
    probe.handle().resume();
    auto const probeParkedMidDial = !probe.done();
    auto const dialsWhileProbing = fixture.connector.dials;

    fixture.connector.dialCompletesOn = nullptr;
    auto const overlappingFetch = core::async::syncRun(upstream.Fetch("k"));
    auto const overlappingStore = core::async::syncRun(upstream.Store("k", {}));
    auto const dialsAfterOverlap = fixture.connector.dials;

    // The probe's dial completes BEFORE anything is asserted, so a red case cannot leave its frame
    // parked on an executor that is about to be destroyed.
    (void) dialInFlight.drain();
    REQUIRE(probe.done());

    CHECK(probeParkedMidDial);
    CHECK(dialsWhileProbing == 2);
    CHECK_FALSE(overlappingFetch.has_value());
    CHECK(overlappingStore == UpstreamStore::Declined);
    CHECK(dialsAfterOverlap == 2);
    CHECK_FALSE(probe.result().has_value());
}

TEST_CASE("A cache miss from a reachable upstream is not remembered as unreachable", "[node][cache][upstream]")
{
    // "Never on a miss", from the memo's side: every dial CONNECTS and every answer is a Miss.
    // A memo keyed on the answer rather than on the dial would stop dialling after the first.
    Fixture fixture;
    auto upstream = fixture.Make();

    for ([[maybe_unused]] auto const _: std::views::iota(0, 5))
        CHECK_FALSE(core::async::syncRun(upstream.Fetch("k")).has_value());

    CHECK(fixture.connector.dials == 5);
}

TEST_CASE("UpstreamReachability believes a failed exchange for one interval and stamps the probe before it runs",
          "[node][cache][upstream]")
{
    core::platform::ManualClock clock;
    UpstreamReachability reachability { clock, RetryInterval };

    // Nothing known yet: dial.
    CHECK(reachability.ShouldDial());

    reachability.Unanswered();
    CHECK_FALSE(reachability.ShouldDial());
    clock.advance(RetryInterval - std::chrono::milliseconds { 1 });
    CHECK_FALSE(reachability.ShouldDial());
    clock.advance(std::chrono::milliseconds { 1 });

    // The probe is granted -- and stamped as it is granted, so a second operation arriving while
    // that probe is still connecting (up to the connect ceiling, on the reactor) does not dial too.
    CHECK(reachability.ShouldDial());
    CHECK_FALSE(reachability.ShouldDial());

    // It connected: every operation dials again.
    reachability.Answered();
    CHECK(reachability.ShouldDial());
    CHECK(reachability.ShouldDial());
}
