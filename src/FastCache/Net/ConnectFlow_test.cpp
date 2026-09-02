// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/ConnectFlow.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{

/// A resolver that answers from a script, so a case can shape the candidate list
/// without DNS.
class ListResolver final: public FastCache::IAsyncAddressResolver
{
  public:
    explicit ListResolver(std::size_t candidates) noexcept:
        _candidates { candidates }
    {
    }

    [[nodiscard]] FastCache::Task<FastCache::ResolveResult> Resolve(std::string host,
                                                                    std::uint16_t port,
                                                                    FastCache::IReactor* /*reactor*/) override
    {
        _calls += 1;
        _lastHost = std::move(host);
        if (_fail)
            co_return std::unexpected(FastCache::ResolveFailure(_lastHost, port, "scripted"));

        std::vector<FastCache::ResolvedEndpoint> out;
        out.resize(_candidates);
        co_return out;
    }

    void Fail() noexcept
    {
        _fail = true;
    }

    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::size_t _candidates;
    int _calls { 0 };
    std::string _lastHost;
    bool _fail { false };
};

/// What each candidate attempt was given, so a case can assert the budget split.
struct DialLog
{
    FastCache::ManualClock* clock { nullptr };
    std::vector<std::chrono::milliseconds> allowances;
    /// What each candidate was told about keepalive.
    ///
    /// Recorded per ATTEMPT rather than once, because the property is that the
    /// caller's answer reaches EVERY candidate: a flow that forwarded it only to
    /// the first would leave a two-record host dialling its second address
    /// unarmed, which is invisible in a case that only ever has one candidate.
    std::vector<FastCache::KeepAlive> keepAlives;
    std::chrono::milliseconds consume { 0 };
    std::size_t succeedAt { 999 };
};

/// No candidate succeeds.
constexpr std::size_t NeverSucceeds = 999;

/// Build a log without a designated initializer per case: the struct has four
/// fields and most cases care about one, and skipping the rest is a warning.
/// @param clock Clock the dial reads and may advance.
/// @param consume How much clock each attempt burns; 0 for an instant answer.
/// @param succeedAt Index of the attempt that succeeds, or NeverSucceeds.
/// @return The log.
[[nodiscard]] DialLog MakeLog(FastCache::ManualClock& clock,
                              std::chrono::milliseconds consume = 0ms,
                              std::size_t succeedAt = NeverSucceeds)
{
    DialLog log;
    log.clock = &clock;
    log.consume = consume;
    log.succeedAt = succeedAt;
    return log;
}

/// A scripted `DialStep`: records its allowance, optionally burns clock, and
/// succeeds only at a chosen index.
FastCache::Task<FastCache::SocketResult> ScriptedDial(void* state,
                                                      FastCache::ResolvedEndpoint /*endpoint*/,
                                                      FastCache::TimePoint deadline,
                                                      FastCache::KeepAlive keepAlive)
{
    auto& log = *static_cast<DialLog*>(state);
    log.allowances.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - log.clock->Now()));
    log.keepAlives.push_back(keepAlive);

    if (log.consume > 0ms)
        log.clock->Advance(log.consume);

    if (log.allowances.size() - 1 == log.succeedAt)
        co_return FastCache::SocketResult { nullptr };

    co_return std::unexpected(FastCache::NetError {
        .code = FastCache::NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted dial refusal" });
}

} // namespace

TEST_CASE("An empty host is refused without touching the resolver", "[net][connectflow]")
{
    // An empty host resolves to the wildcard address, which is a BIND target and
    // not a dial target -- and connecting to it reaches localhost on Linux rather
    // than failing, so the mistake would be silent rather than loud.
    FastCache::ManualClock clock;
    ListResolver resolver { 1 };
    auto log = MakeLog(clock);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(
        &resolver, nullptr, &clock, "", 6674, FastCache::DialOptions { .connectTimeout = 1s }, &ScriptedDial, &log));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == FastCache::NetErrorCode::AddressNotAvail);
    CHECK(resolver.Calls() == 0);
    CHECK(log.allowances.empty());
}

TEST_CASE("The last candidate's failure is the one reported", "[net][connectflow]")
{
    // A peer whose name has several addresses is reachable through whichever one
    // works; reporting the first failure would describe this machine's routing
    // rather than the peer.
    FastCache::ManualClock clock;
    ListResolver resolver { 3 };
    auto log = MakeLog(clock);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 1s },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().context == "scripted dial refusal");
    CHECK(log.allowances.size() == 3);
}

TEST_CASE("A later candidate succeeds after an earlier one fails", "[net][connectflow]")
{
    FastCache::ManualClock clock;
    ListResolver resolver { 3 };
    auto log = MakeLog(clock, 0ms, 1);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 1s },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE(result.has_value());
    // Stopped at the one that worked rather than working through the rest.
    CHECK(log.allowances.size() == 2);
}

TEST_CASE("A resolution failure is reported without dialling", "[net][connectflow]")
{
    FastCache::ManualClock clock;
    ListResolver resolver { 2 };
    resolver.Fail();
    auto log = MakeLog(clock);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "nowhere.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 1s },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == FastCache::NetErrorCode::AddressNotAvail);
    CHECK(log.allowances.empty());
}

TEST_CASE("The budget is shared across candidates, not handed to each", "[net][connectflow]")
{
    // Giving every candidate the full budget means a caller asking for 900ms can
    // wait 2700ms -- a bound that multiplies by however many addresses a name
    // happens to have is not a bound.
    //
    // Each attempt here spends exactly its share, so the division is observable.
    // With a dial that consumed nothing the later candidates would legitimately
    // get MORE, because unspent budget is handed on; that is correct behaviour
    // and says nothing about how the budget was divided.
    FastCache::ManualClock clock;
    ListResolver resolver { 3 };
    auto log = MakeLog(clock, 300ms);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 900ms },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(log.allowances.size() == 3);
    CHECK(log.allowances[0] == 300ms);
    CHECK(log.allowances[1] == 300ms);
    CHECK(log.allowances[2] == 300ms);
}

TEST_CASE("No single candidate may spend the whole budget", "[net][connectflow]")
{
    // Stated separately from the division because it is the half that matters
    // when a candidate black-holes: whatever else happens, the first one must not
    // be able to consume everything and leave the rest untried.
    FastCache::ManualClock clock;
    ListResolver resolver { 3 };
    auto log = MakeLog(clock);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 900ms },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(log.allowances.empty());
    CHECK(log.allowances[0] < 900ms);
}

TEST_CASE("A candidate that black-holes cannot starve the ones after it", "[net][connectflow]")
{
    // The other half of the same tension, and a real regression: on a Windows
    // host where a closed loopback port is silently DROPPED rather than reset,
    // the first dial consumed its entire allowance -- and with an undivided
    // budget the candidate that would have worked was never tried at all. That is
    // precisely the case trying every candidate exists for, so the first one must
    // not be able to spend the whole thing.
    FastCache::ManualClock clock;
    ListResolver resolver { 2 };
    auto log = MakeLog(clock, 500ms, 1);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 1s },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE(result.has_value());
    REQUIRE(log.allowances.size() == 2);
    CHECK(log.allowances[0] == 500ms);
    // The second still got everything the first did not spend.
    CHECK(log.allowances[1] == 500ms);
}

TEST_CASE("An exhausted budget reports a timeout naming the endpoint", "[net][connectflow]")
{
    FastCache::ManualClock clock;
    ListResolver resolver { 3 };
    auto log = MakeLog(clock, 400ms);

    auto result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                       nullptr,
                                                                       &clock,
                                                                       "host.example.com",
                                                                       6674,
                                                                       FastCache::DialOptions { .connectTimeout = 600ms },
                                                                       &ScriptedDial,
                                                                       &log));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == FastCache::NetErrorCode::Timeout);
    CHECK(result.error().context.contains("host.example.com"));
    CHECK(result.error().context.contains("6674"));
    // Stopped rather than working through the rest of the list.
    CHECK(log.allowances.size() < 3);
}

TEST_CASE("The dial's keepalive answer reaches every candidate", "[net][connect][keepalive]")
{
    // The flow forwards what the CALLER asked, to each candidate it tries. It is a
    // per-call option precisely so it cannot be decided anywhere else
    // ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)), and a
    // flow that dropped it would leave the dispatched compile -- the one exchange
    // whose deadline is minutes long -- unarmed, with nothing anywhere saying so.
    //
    // Two candidates, not one. A flow that forwarded the answer only to the FIRST
    // address would still pass a single-candidate case, and a host with both an AAAA
    // and an A record is the ordinary shape rather than an exotic one.
    FastCache::ManualClock clock;
    ListResolver resolver { 2 };
    auto log = MakeLog(clock);

    auto const result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(
        &resolver,
        nullptr,
        &clock,
        "host.example.com",
        6674,
        FastCache::DialOptions { .connectTimeout = 1s, .keepAlive = FastCache::KeepAlive::Yes },
        &ScriptedDial,
        &log));
    std::ignore = result;

    REQUIRE(log.keepAlives.size() == 2);
    CHECK(log.keepAlives[0] == FastCache::KeepAlive::Yes);
    CHECK(log.keepAlives[1] == FastCache::KeepAlive::Yes);
}

TEST_CASE("A dial that does not ask for keepalive does not get it", "[net][connect][keepalive]")
{
    // The default, asserted rather than assumed. Every caller in this tree except the
    // dispatched compile relies on it, and a `DialOptions` whose default flipped
    // would arm the daemon's Raft links and the launcher's cache round trips too --
    // the fleet-wide change the option exists to avoid.
    FastCache::ManualClock clock;
    ListResolver resolver { 1 };
    auto log = MakeLog(clock);

    auto const result = FastCache::SyncRun(FastCache::Detail::RunConnectFlow(&resolver,
                                                                             nullptr,
                                                                             &clock,
                                                                             "host.example.com",
                                                                             6674,
                                                                             FastCache::DialOptions { .connectTimeout = 1s },
                                                                             &ScriptedDial,
                                                                             &log));
    std::ignore = result;

    REQUIRE(log.keepAlives.size() == 1);
    CHECK(log.keepAlives[0] == FastCache::KeepAlive::No);
}
