// SPDX-License-Identifier: Apache-2.0
#include "SocketExchange.hpp"
#include "StatsGatherer.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The admin surface's Prometheus route.
    constexpr std::string_view MetricsPath = "/metrics";
} // namespace

LadderGatherer::LadderGatherer(Endpoint admin,
                               Endpoint cache,
                               DialTimeouts timeouts,
                               std::optional<std::string> bearer,
                               IExchange* resp,
                               INodeExchange* node) noexcept:
    _admin { std::move(admin) },
    _cache { std::move(cache) },
    _timeouts { timeouts },
    _bearer { std::move(bearer) },
    _resp { resp },
    _node { node }
{
}

std::expected<CompileCacheWire::NodeStatusFields, std::string> const& LadderGatherer::Identify()
{
    if (_identity.has_value())
        return *_identity;

    if (_node == nullptr)
    {
        _identity = std::unexpected(std::string { "no 0xFC connection was opened" });
        return *_identity;
    }

    auto const reply = _node->Send(CompileCacheWire::EncodeNodeStatusRequest());
    if (!reply.has_value())
    {
        // Whatever is on that port did not frame a reply. Worded as what it IS rather
        // than as a failed scrape: this is a fact about the endpoint, and the rungs
        // below turn it into *was not asked*.
        _identity = std::unexpected(std::format("{} is not a compile node ({})", _node->Address(), reply.error().detail));
        return *_identity;
    }
    if (reply->status != CompileCacheWire::Status::Ok)
    {
        // It framed a refusal, so it speaks `0xFC` -- and has no operator component.
        // That is the ORDINARY answer from `fastcached`, which serves the compile-cache
        // verbs on this same wire, so it must not read as a fault.
        _identity = std::unexpected(
            std::format("{} speaks 0xFC and serves no node verbs, so it is not a compile node", _node->Address()));
        return *_identity;
    }

    auto fields = CompileCacheWire::DecodeNodeStatus(reply->payload);
    if (!fields.has_value())
    {
        _identity =
            std::unexpected(std::format("{} answered node-status with a body this client cannot read", _node->Address()));
        return *_identity;
    }

    _identity = *std::move(fields);
    return *_identity;
}

std::expected<Endpoint, std::string> LadderGatherer::ResolveAdmin()
{
    // The operator's own answer wins, for the reason every override in this tree does:
    // they can see a topology this client cannot, and a discovery that outranked them
    // would be undefeatable.
    if (_admin.Configured())
        return _admin;

    auto const& identity = Identify();
    if (!identity.has_value())
        return std::unexpected(std::format("no admin address is known and none could be discovered: {}", identity.error()));

    auto const admin = std::ranges::find(
        identity->surfaces, CompileCacheWire::WireSurface::Admin, &CompileCacheWire::SurfaceReport::surface);

    // **Absent, and that is an ANSWER rather than a failure to get one.** A node that
    // opened no admin surface said so; telling an operator the scrape *failed* would
    // send them to check a listener that does not exist.
    if (admin == identity->surfaces.end())
        return std::unexpected(std::format("{} runs no admin surface", _node->Address()));

    // This client speaks plain HTTP. A TLS admin surface is refused BY NAME rather than
    // dialled and failed: an HTTP request into a TLS listener produces a transport error
    // that reads as the surface being down, which is the misdiagnosis the `tls` field on
    // the wire exists to prevent.
    if (admin->tls)
        return std::unexpected(
            std::format("{} serves its admin surface over TLS, which this client cannot scrape", _node->Address()));

    // **The reported port must not be the one already dialled.** Both surfaces are on
    // this host and a node that named its own 0xFC port would have an HTTP request sent
    // into a 0xFC listener -- which answers a framing refusal, not an HTTP error, so the
    // failure would read as a corrupt scrape rather than as a wrong port. Cheap to ask,
    // and it is the exact disagreement a peer session flagged as worth deciding.
    auto const port = static_cast<std::uint16_t>(admin->port);
    if (port == _cache.port)
        return std::unexpected(std::format("{} reports its admin surface on the port this client is already talking "
                                           "0xFC to; refusing to scrape it",
                                           _node->Address()));

    return Endpoint { .host = _cache.host, .port = port };
}

StatsAttempt LadderGatherer::AskMetrics()
{
    StatsAttempt attempt { .origin = StatsOrigin::Metrics };

    auto const admin = ResolveAdmin();
    if (!admin.has_value())
    {
        // Not asked, and it says so. Reporting "did not answer" for an endpoint nothing
        // dialled would send an operator to check a listener that was never contacted --
        // and `ResolveAdmin` is what distinguishes *nowhere to ask* from *asked and
        // silent*, which is the three-state rule this whole struct exists for.
        attempt.note = admin.error();
        return attempt;
    }

    attempt.asked = true;
    auto const response = HttpGet(*admin, MetricsPath, _timeouts, _bearer);
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

StatsAttempt LadderGatherer::AskNodeMetrics()
{
    StatsAttempt attempt { .origin = StatsOrigin::NodeMetrics };

    // **NOT ASKED, and that is the whole point of asking `Identify` first.** An endpoint
    // that serves no node verbs has not declined to answer this -- it was never
    // consulted about it, and reporting *did not answer* would send an operator to check
    // a component that is not there. Against a plain `fastcached` this is the ordinary
    // path and must be quiet.
    if (auto const& identity = Identify(); !identity.has_value())
    {
        attempt.note = identity.error();
        return attempt;
    }

    attempt.asked = true;
    auto const reply = _node->Send(CompileCacheWire::EncodeNodeMetricsRequest());
    if (!reply.has_value())
    {
        attempt.note = reply.error().detail;
        return attempt;
    }
    if (reply->status != CompileCacheWire::Status::Ok)
    {
        attempt.note = ExplainRefusal("node-metrics", _node->Address(), *reply);
        return attempt;
    }

    auto record = DecodeNodeCounters(reply->payload);
    if (!record.has_value())
    {
        attempt.note = std::format("{} answered node-metrics with a body this client cannot read", _node->Address());
        return attempt;
    }
    if (record->fields.empty())
    {
        // A successful reply with no rows is not a reading. Saying so keeps it from
        // being reported as an empty but successful scrape, which a dashboard would draw
        // as a node of zeroes.
        attempt.note = std::format("{} answered node-metrics with no counters", _node->Address());
        return attempt;
    }
    attempt.record = std::move(*record);
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
    return std::vector<StatsAttempt> { AskMetrics(), AskNodeMetrics(), AskInfo() };
}

} // namespace FastCache::Cli
