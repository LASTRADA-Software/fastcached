// SPDX-License-Identifier: Apache-2.0
#include "SocketExchange.hpp"
#include "StatsGatherer.hpp"

#include <format>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The admin surface's Prometheus route.
    constexpr std::string_view MetricsPath = "/metrics";
} // namespace

LadderGatherer::LadderGatherer(Endpoint admin,
                               DialTimeouts timeouts,
                               std::optional<std::string> bearer,
                               IExchange* resp) noexcept:
    _admin { std::move(admin) },
    _timeouts { timeouts },
    _bearer { std::move(bearer) },
    _resp { resp }
{
}

StatsAttempt LadderGatherer::AskMetrics()
{
    StatsAttempt attempt { .origin = StatsOrigin::Metrics };

    if (!_admin.Configured())
    {
        // Not asked, and it says so. Reporting "did not answer" for an endpoint
        // nothing dialled would send an operator to check a listener that was never
        // contacted.
        attempt.note = "no admin address is known; pass --admin-addr or set $FASTCACHE_ADMIN_ADDR";
        return attempt;
    }

    attempt.asked = true;
    auto const response = HttpGet(_admin, MetricsPath, _timeouts, _bearer);
    if (!response.has_value())
    {
        attempt.note = response.error().detail;
        return attempt;
    }
    if (response->status != 200)
    {
        attempt.note = std::format("{} answered HTTP {}", MetricsPath, response->status);
        return attempt;
    }

    auto record = ParsePrometheus(response->body);
    if (record.fields.empty())
    {
        // A 200 with nothing parseable in it is not a reading. Saying so keeps this
        // from being reported as an empty but successful scrape, which a dashboard
        // would draw as a fleet of zeroes.
        attempt.note = std::format("{} answered with no series", MetricsPath);
        return attempt;
    }
    attempt.record = std::move(record);
    return attempt;
}

StatsAttempt LadderGatherer::AskInfo()
{
    StatsAttempt attempt { .origin = StatsOrigin::Info };

    if (_resp == nullptr)
    {
        attempt.note = "no connection to the cache was opened";
        return attempt;
    }

    attempt.asked = true;
    auto const reply = Call(*_resp, { "INFO" });
    if (!reply.has_value())
    {
        attempt.note = reply.error().detail;
        return attempt;
    }
    if (IsError(*reply))
    {
        attempt.note = reply->text;
        return attempt;
    }
    if (reply->type != RespType::BulkString && reply->type != RespType::Verbatim)
    {
        attempt.note = std::format("INFO answered with {} rather than a text payload", RespTypeName(reply->type));
        return attempt;
    }

    auto record = ParseInfo(reply->text);
    if (record.fields.empty())
    {
        attempt.note = "INFO answered with no fields";
        return attempt;
    }
    attempt.record = std::move(record);
    return attempt;
}

std::vector<StatsAttempt> LadderGatherer::Gather()
{
    // Every source is reported on, including the ones that were not asked, because
    // `ChooseStats` cannot distinguish *not asked* from *not mentioned* if a source
    // simply goes missing from the list.
    return std::vector<StatsAttempt> { AskMetrics(), AskInfo() };
}

} // namespace FastCache::Cli
