// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/HostPort.hpp>
#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsContext.hpp>
#endif
#include <FastCache/Distributed/FleetChart.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <utility>
#include <vector>

namespace FastCache::Node
{

AdminHttpServer::SnapshotProvider MakeNodeSnapshotProvider(NodeScrapeSources sources,
                                                           std::chrono::steady_clock::time_point startedAt)
{
    return [sources = std::move(sources), startedAt] {
        auto const disk = sources.host->SpaceOn(sources.scratchRoot);
        return MetricsSnapshot {
            // The node's own cache, when it has one. It usually does --
            // `--listen-cache` is on by default and a local tier is what this
            // program is FOR -- and this scrape reported `std::nullopt` regardless,
            // so a node holding a quarter of a gigabyte of objects and one holding
            // none produced the same bytes. Null only when the operator turned
            // every half of the tier off, and then absent IS the truth.
            .storage = sources.cache != nullptr ? std::optional { sources.cache->Snapshot() } : std::nullopt,
            // And the halves that merged view cannot show apart: with `--cache-dir`
            // the composite reports the on-disk store alone, so the in-memory tier
            // an operator sized with `--cache-memory` would otherwise be invisible.
            .storageTiers = sources.cache != nullptr ? sources.cache->SnapshotTiers() : TieredStorageStats {},
            .host = HostCapacity { .logicalCores = sources.host->LogicalCores(),
                                   .configuredSlots = sources.slots,
                                   .totalMemoryBytes = sources.host->TotalMemoryBytes(),
                                   .diskCapacityBytes = static_cast<std::uint64_t>(disk.capacityBytes),
                                   .diskFreeBytes = static_cast<std::uint64_t>(disk.freeBytes),
                                   .busySlots = sources.busySlots() },
            .uptime =
                Uptime { std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt) },
        };
    };
}

std::expected<AdminCredential, std::string> ReadDashboardToken(std::filesystem::path const& path)
{
    std::ifstream file { path, std::ios::binary };
    if (!file)
        return std::unexpected { std::format("cannot read '{}'", path.string()) };

    // Via the stream buffer rather than `std::istreambuf_iterator`, which is the
    // workaround this codebase has already had to reach for twice (see
    // `Cc::ReadBytes` and `DefaultConfigPath_test`'s `ReadFile`): GCC at -O3
    // inlines the iterator far enough to see a path where the buffer pointer
    // could be null and rejects it under `-Werror=null-dereference`, which for an
    // `ifstream` it never is. Inserting a `streambuf*` handles null by setting
    // failbit, so there is nothing left for it to complain about.
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto secret = std::move(buffer).str();

    // Trailing whitespace is trimmed because every editor adds a newline, and an
    // operator should not have to know that a secret which looks right is one byte
    // longer than the one they typed. Leading whitespace is NOT trimmed: it is not
    // something an editor adds, and a secret that legitimately begins with a space
    // would otherwise be silently a different secret.
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();

    if (secret.empty())
        return std::unexpected { std::format("'{}' is empty; a credential file nobody can fail to match is "
                                             "worse than none, because the surface looks guarded",
                                             path.string()) };

    return AdminCredential { std::move(secret) };
}

namespace
{
#if defined(FC_TLS_ENABLED)
    /// Names a generated certificate should be valid for.
    ///
    /// Every modern client ignores a certificate's common name, so this list is
    /// what decides whether a browser warns once or twice: an unknown issuer is one
    /// warning an operator can accept, and a name mismatch on top of it is a second
    /// one that is much harder to click past.
    ///
    /// Loopback always, because that is where the surface binds by default; the
    /// machine's own network name, because that is what an operator types to reach
    /// another node; and the admin bind address when it names a particular
    /// interface rather than every one of them -- a wildcard is not a name anybody
    /// can dial, so putting it in a certificate would say nothing.
    /// @param cfg The parsed configuration.
    /// @param host Where the machine's own facts come from.
    /// @return The subject names, loopback first.
    [[nodiscard]] std::vector<std::string> SelfSignedSubjectNames(NodeConfig const& cfg, IHostFactsSource const& host)
    {
        std::vector<std::string> names { "localhost", "127.0.0.1", "::1" };

        if (auto const& hostName = host.Facts().hostName; !hostName.empty())
            names.push_back(hostName);

        if (auto const endpoint = ParseEndpoint(cfg.adminListen, AdminListenDefaultHost); endpoint.has_value())
        {
            constexpr std::array<std::string_view, 3> Wildcards { "0.0.0.0", "::", "[::]" };
            auto const& bindHost = endpoint->first;
            if (!std::ranges::contains(Wildcards, bindHost) && !std::ranges::contains(names, bindHost))
                names.push_back(bindHost);
        }

        return names;
    }
#endif

    /// The challenge an unauthorised caller is answered with.
    ///
    /// `Basic` is named first because it is the one a browser can prompt for, and
    /// the page exists to be opened in one. Without a `WWW-Authenticate` header at
    /// all a browser shows the body and no prompt, which reads as a broken page
    /// rather than as a credential being required.
    [[nodiscard]] AdminResponse Unauthorised(std::string_view contentType, std::string body)
    {
        return AdminResponse { .status = "401 Unauthorized",
                               .contentType = contentType,
                               .body = std::move(body),
                               .extraHeaders = { R"(WWW-Authenticate: Basic realm="fastcache fleet")" } };
    }

    /// One `name=value` out of a query string.
    ///
    /// A parser rather than a dependency: this surface reads two names and would
    /// otherwise gain a URL library to do it. `&`-separated, first match wins, and
    /// an absent name is empty -- which every caller here distinguishes from a
    /// present-but-unrecognised one, because those mean different things.
    /// @param query Whatever followed the `?`.
    /// @param name The parameter to find.
    /// @return Its raw value, or empty when the query does not name it.
    [[nodiscard]] std::string_view QueryValue(std::string_view query, std::string_view name)
    {
        while (!query.empty())
        {
            auto const separator = query.find('&');
            auto const pair = query.substr(0, separator);
            if (auto const equals = pair.find('='); equals != std::string_view::npos && pair.substr(0, equals) == name)
                return pair.substr(equals + 1);
            if (separator == std::string_view::npos)
                break;
            query.remove_prefix(separator + 1);
        }
        return {};
    }

    /// A range this surface does not serve.
    ///
    /// Refused rather than defaulted, deliberately, and it is the one place these
    /// routes are stricter than the theme parameter: a range quietly substituted
    /// puts a reader on a different axis than the one they asked for, with nothing
    /// on the page saying so. A theme quietly substituted costs them nothing.
    [[nodiscard]] AdminResponse BadRange(std::string_view contentType, std::string body)
    {
        return AdminResponse { .status = "400 Bad Request", .contentType = contentType, .body = std::move(body) };
    }

    /// Every range key this build serves, comma-separated.
    ///
    /// Walked out of the table rather than written into the message, because a
    /// refusal that hand-lists what it accepts goes stale the first time a row is
    /// added -- and it goes stale in the one place a reader who just guessed wrong is
    /// looking. Two windows were named here while eight were served.
    /// @return The keys, in the order the control renders them.
    [[nodiscard]] std::string KnownRangeKeys()
    {
        std::string out;
        for (auto const& row: Distributed::FleetRangeTable)
        {
            if (!out.empty())
                out += ", ";
            out += row.key;
        }
        return out;
    }

    /// A chart tail that names nothing in the table.
    [[nodiscard]] AdminResponse NoSuchChart()
    {
        return AdminResponse { .status = "404 Not Found", .contentType = "text/plain", .body = "no such chart\n" };
    }

    /// Answer a conditional GET, rendering only when the client's copy is stale.
    ///
    /// The `ETag` is the sampler's own bucket counter rather than a hash of the
    /// body: it moves exactly when a rendered chart would, costs nothing to compute,
    /// and is byte-exact rather than probabilistic. `Cache-Control` runs only to the
    /// end of the bucket being drawn, because a fixed max-age would leave a viewer a
    /// whole bucket behind for the rest of it.
    /// @param request The request, for its `If-None-Match`.
    /// @param history Where the generation and the bucket clock come from.
    /// @param range Which range is being drawn.
    /// @param identity What distinguishes this resource from the others.
    /// @param status What a fresh answer's status would be.
    /// @param contentType What the body is.
    /// @param render Produces the body; called only when it will be sent.
    /// @return The rendered answer, or a bodyless `304`.
    template <typename Render>
    [[nodiscard]] AdminResponse Conditional(AdminRequest const& request,
                                            IFleetHistoryView const* history,
                                            Distributed::FleetRange range,
                                            std::string const& identity,
                                            std::string_view status,
                                            std::string_view contentType,
                                            Render&& render)
    {
        if (history == nullptr)
            return AdminResponse { .status = "503 Service Unavailable",
                                   .contentType = "text/plain",
                                   .body = "this node keeps no fleet history\n" };

        auto const tag = std::format(R"("{}-{}")", identity, history->Generation());
        // Bounded by the SAMPLE interval, not only by the bucket. The newest bucket is
        // always still open and gains a reading every sample, so "until this bucket
        // closes" is how long the chart's shape is settled -- not how long it is
        // current. On a five-minute bucket the difference was four minutes of
        // staleness; on the twelve-month view, whose buckets are a day wide, it is a
        // chart frozen in the browser for twenty-four hours while the fleet moves.
        // Revalidation is cheap: the ETag makes it a bodyless 304 whenever nothing
        // closed.
        auto const settled = history->UntilBucketCloses(range);
        auto const maxAge = std::min(settled, Distributed::FleetSampleInterval).count();
        std::vector<std::string> headers { std::format("ETag: {}", tag),
                                           std::format("Cache-Control: max-age={}, must-revalidate", maxAge) };

        if (request.Header(AdminHeader::IfNoneMatch) == tag)
            // Bodyless, and the validators go with it: without them the client has
            // nothing to revalidate against next time.
            return AdminResponse { .status = "304 Not Modified", .extraHeaders = std::move(headers) };

        return AdminResponse { .status = status,
                               .contentType = contentType,
                               .body = std::forward<Render>(render)(),
                               .extraHeaders = std::move(headers) };
    }
} // namespace

std::vector<AdminRoute> MakeFleetRoutes(Distributed::FleetSources sources,
                                        AdminCredential const& credential,
                                        unsigned refreshSeconds,
                                        IFleetHistoryView const* history)
{
    // Every route reads the fleet the same way and is gated the same way; only what
    // it renders differs. **The gate is written once and the renderer is the
    // parameter**, so a route added later cannot be one that forgot to check --
    // which here would mean the fleet's whole history readable through an image URL
    // while `/fleet` itself stays locked.
    auto const gated = [sources, credential](auto refuse, auto render) {
        return [sources, credential, refuse, render](AdminRequest const& request) -> AdminResponse {
            if (!credential.Accepts(request.Header(AdminHeader::Authorization)))
                return refuse();
            return render(Distributed::CollectFleet(sources), request);
        };
    };

    // A follower answers 503 rather than 200: its registry holds whatever registered
    // against it rather than the fleet, so a 200 would be a partial picture
    // presented as the whole one. It is `Gate()`'s `NotLeader` in HTTP's vocabulary
    // -- not me, and here is who.
    auto const statusFor = [](Distributed::FleetSnapshot const& snapshot) {
        return Distributed::LeadsTheFleet(snapshot) ? "200 OK" : "503 Service Unavailable";
    };

    /// What the reader asked for, or the default -- never a silent substitution.
    auto const rangeAsked = [](std::string_view query) -> std::optional<Distributed::FleetRange> {
        auto const asked = QueryValue(query, "range");
        if (asked.empty())
            return Distributed::FleetRange::Day;
        return Distributed::FleetRangeFromKey(asked);
    };

    auto const viewFor = [history](Distributed::FleetRange range) {
        Distributed::FleetHistoryView view { .range = range,
                                             .buckets = {},
                                             .durable = history != nullptr && history->Durable() };
        if (history != nullptr)
            view.buckets = history->Buckets(range);
        return view;
    };

    std::vector<AdminRoute> routes;

    routes.push_back(AdminRoute {
        .path = "/fleet",
        .handler = gated(
            [] {
                return Unauthorised("text/html; charset=utf-8",
                                    "<!doctype html><title>fastcache fleet</title>"
                                    "<p>This page needs the credential named by "
                                    "<code>--dashboard-token-file</code>.</p>");
            },
            [refreshSeconds, statusFor, rangeAsked, viewFor](Distributed::FleetSnapshot const& snapshot,
                                                             AdminRequest const& request) -> AdminResponse {
                auto const range = rangeAsked(request.query);
                if (!range.has_value())
                    return BadRange("text/html; charset=utf-8",
                                    std::format("<!doctype html><title>fastcache fleet</title>"
                                                "<p>Unknown <code>range</code>. Try one of: <code>{}</code>.</p>",
                                                KnownRangeKeys()));
                return AdminResponse { .status = statusFor(snapshot),
                                       .contentType = "text/html; charset=utf-8",
                                       .body = Distributed::RenderFleetHtml(snapshot, viewFor(*range), refreshSeconds) };
            }),
    });

    routes.push_back(AdminRoute {
        .path = "/fleet.json",
        .handler = gated([] { return Unauthorised("application/json", R"({"error":"credential required"})"); },
                         [statusFor](Distributed::FleetSnapshot const& snapshot, AdminRequest const&) {
                             return AdminResponse { .status = statusFor(snapshot),
                                                    .contentType = "application/json",
                                                    .body = Distributed::RenderFleetJson(snapshot) };
                         }),
    });

    routes.push_back(AdminRoute {
        .path = std::string_view { Distributed::FleetSeriesPath },
        .handler = gated(
            [] { return Unauthorised("application/json", R"({"error":"credential required"})"); },
            [history, statusFor, rangeAsked, viewFor](Distributed::FleetSnapshot const& snapshot,
                                                      AdminRequest const& request) -> AdminResponse {
                auto const range = rangeAsked(request.query);
                if (!range.has_value())
                    return BadRange("application/json",
                                    std::format(R"({{"error":"unknown range","known":"{}"}})", KnownRangeKeys()));
                auto const view = viewFor(*range);
                return Conditional(request,
                                   history,
                                   *range,
                                   std::format("s-{}", Distributed::FleetRangeTable[static_cast<std::size_t>(*range)].key),
                                   statusFor(snapshot),
                                   "application/json",
                                   [&view, &range] { return Distributed::RenderSeriesJson(view.buckets, *range); });
            }),
    });

    routes.push_back(AdminRoute {
        .path = std::string_view { Distributed::FleetChartPrefix },
        .handler = gated([] { return Unauthorised("text/plain", "credential required\n"); },
                         [history, statusFor, rangeAsked, viewFor](Distributed::FleetSnapshot const& snapshot,
                                                                   AdminRequest const& request) -> AdminResponse {
                             constexpr std::string_view Extension = ".svg";
                             auto tail = request.path.substr(Distributed::FleetChartPrefix.size());
                             if (!tail.ends_with(Extension))
                                 return NoSuchChart();
                             tail.remove_suffix(Extension.size());
                             auto const chart = Distributed::FleetChartFromKey(tail);
                             if (!chart.has_value())
                                 return NoSuchChart();

                             auto const range = rangeAsked(request.query);
                             if (!range.has_value())
                                 return BadRange("text/plain", std::format("unknown range; known: {}\n", KnownRangeKeys()));
                             auto const theme = Distributed::FleetThemeFromKey(QueryValue(request.query, "theme"));
                             auto const& row = Distributed::FleetChartTable[static_cast<std::size_t>(*chart)];

                             auto const view = viewFor(*range);
                             return Conditional(
                                 request,
                                 history,
                                 *range,
                                 std::format("{}-{}-{}",
                                             row.key,
                                             Distributed::FleetRangeTable[static_cast<std::size_t>(*range)].key,
                                             Distributed::FleetThemeKey(theme)),
                                 statusFor(snapshot),
                                 "image/svg+xml",
                                 [&row, &view, &range, theme] {
                                     return Distributed::RenderChartSvg(row, view.buckets, *range, theme);
                                 });
                         }),
        // A prefix route, so one row here covers every chart the table names: a
        // route per chart would put the chart table's contents in a second place.
        .match = AdminRouteMatch::Prefix,
    });

    return routes;
}

EnumTable<Distributed::FleetMetric, std::uint64_t> SampleFrom(Distributed::FleetSnapshot const& snapshot)
{
    EnumTable<Distributed::FleetMetric, std::uint64_t> values {};
    auto const put = [&values](Distributed::FleetMetric metric, std::uint64_t value) {
        values[static_cast<std::size_t>(metric)] = value;
    };

    // The five dispatch counters keep `LeaseOutcomeTable`'s order rather than being
    // named one by one, so a sixth outcome lands here by being added to that table.
    static constexpr std::array<Distributed::FleetMetric, 5> dispatchSlots {
        Distributed::FleetMetric::DispatchGranted,    Distributed::FleetMetric::DispatchNoWorker,
        Distributed::FleetMetric::DispatchNoCapacity, Distributed::FleetMetric::DispatchWithdrawn,
        Distributed::FleetMetric::DispatchDuplicate,
    };
    static_assert(dispatchSlots.size() == Distributed::LeaseOutcomeTable.size(),
                  "every lease outcome needs a slot, or a refusal reason silently stops being recorded");
    for (auto const index: std::views::iota(std::size_t { 0 }, dispatchSlots.size()))
        if (index < snapshot.leases.size())
            put(dispatchSlots[index], snapshot.leases[index]);

    // Summed over `NodeReports()`, never over `LiveWorkers()`: a node started with
    // two --toolchain flags is two registry entries carrying one machine's cache,
    // and summing there counts that cache once per toolchain.
    //
    // Every slot, including the ones a machine can also answer for itself. This is
    // the FLEET series and a leader can answer for all nine -- these are fleet-wide
    // sums, which is a different number from any one machine's and the one this page
    // draws. `FleetMetricScope` is about what a NODE may claim about itself, not
    // about what belongs here.
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    for (auto const& node: snapshot.nodes)
    {
        hits += node.load.cache.hits.value_or(0);
        misses += node.load.cache.misses.value_or(0);
    }
    put(Distributed::FleetMetric::CacheHits, hits);
    put(Distributed::FleetMetric::CacheMisses, misses);

    auto const totals = Distributed::TotalsFor(snapshot);
    put(Distributed::FleetMetric::OfferableSlots, totals.free);
    put(Distributed::FleetMetric::JobsInFlight, totals.inFlight);
    return values;
}

EnumTable<Distributed::FleetMetric, std::uint64_t> NodeSampleFrom(IMetricsSink const& metrics,
                                                                  MetricsSnapshot const& snapshot)
{
    EnumTable<Distributed::FleetMetric, std::uint64_t> values {};
    auto const put = [&values](Distributed::FleetMetric metric, std::uint64_t value) {
        auto const& row = Distributed::FleetMetricTable[static_cast<std::size_t>(metric)];
        if (row.scope != Distributed::FleetMetricScope::Node)
            return;
        values[static_cast<std::size_t>(metric)] = value;
    };

    // From the SAME counters `CacheTier::Snapshot` reports to the fleet, not from
    // `StorageStats::getHits`. Those are a different number -- the store's own GET
    // tally, which also counts everything served over the cache port -- and using
    // them here would have one page showing two disagreeing answers for one machine's
    // cache, with nothing saying which was which.
    //
    // A node with no cache reports zero and zero, which is the truth about it: unlike
    // `/metrics`, where absent and zero are different claims, a history slot has no
    // way to say "no cache" and a rate across two zeroes is zero either way.
    put(Distributed::FleetMetric::CacheHits, metrics.Read(IMetricsSink::Counter::NodeCacheHits));
    put(Distributed::FleetMetric::CacheMisses, metrics.Read(IMetricsSink::Counter::NodeCacheMisses));

    // Free slots, not configured ones: the series is about what a compile could have
    // started on, and a machine with every slot busy is offering nothing however many
    // it advertises. Saturating, because a scrape landing between the two figures
    // moving must not wrap into billions.
    //
    // A process reporting no host capacity is not a worker, and zero offered with
    // zero running is the truth about it rather than a gap this format could express.
    if (snapshot.host.has_value())
    {
        auto const busy = snapshot.host->busySlots;
        auto const configured = snapshot.host->configuredSlots;
        put(Distributed::FleetMetric::OfferableSlots, configured > busy ? configured - busy : 0);
        put(Distributed::FleetMetric::JobsInFlight, busy);
    }
    return values;
}

std::filesystem::path HistoryPathFor(NodeConfig const& cfg, HistoryFile which)
{
    // One row per file, in enumerator order, so a fourth file is a row rather than a
    // path spelled somewhere nobody looks. The fleet file keeps the name it always
    // had, so an existing install's fleet series survives the upgrade that split it
    // from the others.
    struct FileNameRow
    {
        HistoryFile which;     ///< The file this row names.
        std::string_view name; ///< What it is called.
    };
    // A row that states its own enumerator, and the guard every enumerator-indexed
    // table in this tree carries. Filled positionally it compiled either way, and
    // swapping two rows would have swapped two files -- silently for the pair that
    // share a format, since each would then load the other's readings without
    // complaint.
    static constexpr EnumTable<HistoryFile, FileNameRow> fileNames {
        FileNameRow { .which = HistoryFile::Node, .name = "node-history.bin" },
        FileNameRow { .which = HistoryFile::Fleet, .name = "fleet-history.bin" },
        FileNameRow { .which = HistoryFile::Received, .name = "received-history.bin" },
    };
    static_assert(RowsInEnumeratorOrder(fileNames, &FileNameRow::which));
    auto const name = fileNames[static_cast<std::size_t>(which)].name;
    if (!cfg.clusterDir.empty())
        return cfg.clusterDir / name;
    if (!cfg.cacheDir.empty())
        return cfg.cacheDir / name;
    return {};
}

HistoryPaths HistoryPaths::For(NodeConfig const& cfg)
{
    return HistoryPaths { .fleet = HistoryPathFor(cfg, HistoryFile::Fleet),
                          .node = HistoryPathFor(cfg, HistoryFile::Node),
                          .received = HistoryPathFor(cfg, HistoryFile::Received) };
}

std::filesystem::path FleetHistoryPath(NodeConfig const& cfg)
{
    return HistoryPathFor(cfg, HistoryFile::Fleet);
}

FleetSampler::FleetSampler(std::optional<Distributed::FleetSources> sources,
                           IMetricsSink const& metrics,
                           AdminHttpServer::SnapshotProvider node,
                           IWallClock const& wall,
                           HistoryPaths paths,
                           ILogger& logger):
    _sources { sources },
    _metrics { metrics },
    _nodeFacts { std::move(node) },
    _fleet { wall },
    _node { wall },
    _received { wall },
    // Every store is a row, so restoring and saving are one loop each. The received
    // store is here rather than beside the loop because a store restored and never
    // saved is a year of readings discovered missing at the next restart -- and what
    // it holds is what makes a fleet's record survive an election, which a node
    // advancing its own watermark and never resending cannot rebuild.
    _stores { Store { .path = std::move(paths.fleet),
                      .what = "fleet",
                      .load = [this](auto const& at) { return _fleet.Load(at); },
                      .save = [this](auto const& at) { return _fleet.Save(at); },
                      .readOnly = [this] { return _fleet.ReadOnly(); },
                      .worthWriting = [this] { return !_fleet.Empty(); } },
              Store { .path = std::move(paths.node),
                      .what = "node",
                      .load = [this](auto const& at) { return _node.Load(at); },
                      .save = [this](auto const& at) { return _node.Save(at); },
                      .readOnly = [this] { return _node.ReadOnly(); },
                      .worthWriting = [this] { return !_node.Empty(); } },
              Store { .path = std::move(paths.received),
                      .what = "received",
                      .load = [this](auto const& at) { return _received.Load(at); },
                      .save = [this](auto const& at) { return _received.Save(at); },
                      .readOnly = [this] { return _received.ReadOnly(); },
                      .worthWriting = [this] { return _received.Count() > 0; } } },
    _logger { logger }
{
    // Every failure to read starts empty and says so once. History is a
    // convenience, and no state of any of these files may keep a node from starting.
    for (auto const& each: _stores)
    {
        if (each.path.empty())
            continue;
        if (each.load(each.path))
            _logger.Logf(LogLevel::Info, "{} history restored from {}", each.what, each.path.string());
        else if (each.readOnly())
            // Named at WARN and named specifically, because this one is not the
            // ordinary "nothing to restore": a later build wrote that file, this one
            // cannot read it, and the reason it is not being replaced is that doing
            // so would destroy readings the other build could still use. An operator
            // who saw only "starts empty" would reasonably delete it.
            _logger.Logf(LogLevel::Warn,
                         "{} history at {} was written by a NEWER build; starting empty and leaving it alone. "
                         "Sampling continues, nothing is persisted, and the file is safe for that build to read again",
                         each.what,
                         each.path.string());
        else
            _logger.Logf(LogLevel::Info, "{} history starts empty at {}", each.what, each.path.string());
    }

    _thread = std::jthread { [this](std::stop_token stop) {
        auto sinceSave = std::chrono::steady_clock::duration::zero();
        while (!stop.stop_requested())
        {
            SampleOnce();
            if (sinceSave >= SaveInterval)
            {
                Persist();
                sinceSave = std::chrono::steady_clock::duration::zero();
            }

            // Interruptible, rather than a sleep this loop would have to wake from
            // on its own schedule. A stop that had to wait out a full interval makes
            // teardown look hung, which this repository has already paid for once as
            // a `systemctl stop` that escalated to SIGKILL.
            auto guard = std::unique_lock { _wakeMutex };
            (void) _wake.wait_for(
                guard, stop, Distributed::FleetHistory::SampleInterval, [&stop] { return stop.stop_requested(); });
            sinceSave += Distributed::FleetHistory::SampleInterval;
        }
    } };
}

FleetSampler::~FleetSampler()
{
    _thread.request_stop();
    _wake.notify_all();
    if (_thread.joinable())
        _thread.join();
    // One last write, so a clean shutdown does not throw away up to five minutes of
    // what the page will be asked about the moment the node comes back.
    Persist();
}

std::vector<Distributed::FleetBucket> FleetSampler::NextHistoryBatch(std::size_t limit) const
{
    return _node.ClosedBucketsAfter(_handedThrough.load(std::memory_order_relaxed), limit);
}

void FleetSampler::HistoryHandedThrough(std::int64_t startMillis) noexcept
{
    _handedThrough.store(startMillis, std::memory_order_relaxed);
}

std::vector<Distributed::FleetBucket> FleetSampler::Buckets(Distributed::FleetRange range) const
{
    auto view = _fleet.Buckets(range);
    _received.BackfillInto(view, range);
    return view;
}

bool FleetSampler::SampleOnce()
{
    // ALWAYS, whoever leads. What this machine's cache did and how many of its slots
    // were busy are facts about the machine, and a node that stopped recording them
    // the moment it lost an election is a node whose year of history belongs to
    // whichever peer happened to be leading at the time. This is the half that
    // survives an election, and in Phase 5 it is the half that rebuilds the leader's.
    if (_nodeFacts)
        _node.Record(NodeSampleFrom(_metrics, _nodeFacts()));

    // A node with no scheduler has no fleet to answer for, and says so by recording
    // nothing rather than by recording zeroes. It still keeps the series above,
    // which is the whole point: a pure worker is exactly the machine whose windows
    // the fleet would otherwise be missing.
    if (!_sources.has_value())
        return false;

    auto const snapshot = Distributed::CollectFleet(*_sources);
    // The fleet series stays leader-only, and for the original reason: a follower's
    // registry holds whatever registered against IT, so a fleet-scoped sample there
    // records a fraction as though it were the whole -- and the chart would show the
    // fleet shrinking whenever leadership moved, which is the opposite of what
    // happened. What changed is that this no longer stops the node recording itself.
    if (!Distributed::LeadsTheFleet(snapshot))
        return false;
    _fleet.Record(SampleFrom(snapshot));
    return true;
}

void FleetSampler::Persist()
{
    for (auto const& each: _stores)
    {
        if (each.path.empty() || !each.worthWriting())
            continue;
        // A refusal to overwrite a newer file is not a write failure, and reporting
        // it as one every save interval would bury the single WARN at startup that
        // explains it under a stream of "could not write" saying the opposite of
        // what is true.
        if (each.readOnly())
            continue;
        if (!each.save(each.path))
            _logger.Logf(LogLevel::Warn, "could not write {} history to {}", each.what, each.path.string());
    }
}

std::expected<AdminSurface, std::string> StartAdminSurfaceOrExplain(NodeConfig const& cfg,
                                                                    [[maybe_unused]] IHostFactsSource const& host,
                                                                    IMetricsSink& metrics,
                                                                    AdminHttpServer::SnapshotProvider snapshot,
                                                                    std::optional<Distributed::FleetSources> fleet,
                                                                    FleetSampler const* sampler,
                                                                    ILogger& logger)
{
    AdminSurface surface;

    // Off unless asked for, like every other surface this program serves. Nothing
    // else here runs, so a node with no `--admin-listen` also reads no certificate
    // and no token -- the flags that would be a silent no-op are refused by
    // `StartupPolicyRejection` long before this.
    if (cfg.adminListen.empty())
        return surface;

    // TLS is on by naming material or by asking for material to be made, never by
    // a bare boolean: neither spelling can reach a state where TLS was requested
    // and this node has nothing to serve it with.
    if (cfg.tlsSelfSigned || !cfg.tlsCertFile.empty())
    {
#if defined(FC_TLS_ENABLED)
        auto created = cfg.tlsSelfSigned ? TlsContext::CreateSelfSigned(SelfSignedSubjectNames(cfg, host))
                                         // `.string()` because `TlsContext::Create` takes
                                         // `string_view`, and a `std::filesystem::path` only
                                         // converts to one implicitly where `string_type` IS
                                         // `std::string` -- which is POSIX and not Windows.
                                         : TlsContext::Create(cfg.tlsCertFile.string(), cfg.tlsKeyFile.string());
        if (!created.has_value())
            return std::unexpected { std::format(
                "{}: {}", cfg.tlsSelfSigned ? "--tls-self-signed" : "--tls-cert/--tls-key", created.error().ToString()) };
        surface.tls = std::move(*created);
#else
        // Refused rather than warned about, and the daemon answers the same way: a
        // node that started in the clear after being told to serve TLS is one an
        // operator believes is encrypted.
        return std::unexpected { std::format("{} requested but this build has no TLS support "
                                             "(rebuild with -DFASTCACHED_ENABLE_TLS=ON)",
                                             cfg.tlsSelfSigned ? "--tls-self-signed" : "--tls-cert") };
#endif
    }

    // Read once at startup. A file that cannot be read must not become "no
    // credential": that is the single failure that turns a guarded fleet map into
    // an open one.
    AdminCredential credential;
    if (!cfg.dashboardTokenFile.empty())
    {
        auto read = ReadDashboardToken(cfg.dashboardTokenFile);
        if (!read.has_value())
            return std::unexpected { std::format("--dashboard-token-file {}", read.error()) };
        credential = std::move(*read);
    }

    // Contributed only when the operator asked AND there is a fleet to read. A
    // node with no scheduler passes nullopt, and `/fleet` is then a plain 404
    // rather than a route answering with an empty fleet.
    std::vector<AdminRoute> routes;
    // The sampler is reached as an `IFleetHistoryView`, which is the ONLY door:
    // the two halves of a leader's record -- what it sampled while leading, and
    // what the other machines handed over for the windows it missed -- meet
    // behind it, and no route can reach the raw series past it.
    if (cfg.dashboard && fleet.has_value())
        routes = MakeFleetRoutes(*fleet, credential, DashboardRefreshSeconds, sampler);

    auto started = AdminEndpoint::Start(cfg.adminListen,
                                        AdminListenDefaultHost,
                                        metrics,
                                        std::move(snapshot),
                                        logger,
                                        std::move(routes),
#if defined(FC_TLS_ENABLED)
                                        surface.tls.get());
#else
                                        nullptr);
#endif
    if (!started.has_value())
        return std::unexpected { std::format("--admin-listen {}", started.error()) };

    surface.endpoint = std::move(*started);

    auto const* const scheme =
#if defined(FC_TLS_ENABLED)
        surface.tls ? "https" : "http";
#else
        "http";
#endif
#if defined(FC_TLS_ENABLED)
    // Printed because with a generated certificate this is the ONLY thing that
    // authenticates the node: nothing signs it, so an operator compares what is
    // logged here against what their browser shows and knows they reached the
    // machine rather than something in between.
    if (surface.tls && cfg.tlsSelfSigned)
        logger.Logf(LogLevel::Info,
                    "admin TLS uses a self-signed certificate generated at startup; SHA-256 fingerprint {} "
                    "(it changes on every restart)",
                    surface.tls->CertificateFingerprint());
#endif
    logger.Logf(
        LogLevel::Info, "metrics endpoint on {}://{}/metrics (and /healthz)", scheme, surface.endpoint->BoundEndpoint());
    if (cfg.dashboard && fleet.has_value())
        logger.Logf(LogLevel::Info,
                    "fleet dashboard on {}://{}/fleet (and /fleet.json, /fleet/series.json, /fleet/chart/*.svg){}",
                    scheme,
                    surface.endpoint->BoundEndpoint(),
                    credential.Required() ? "" : ", with no credential");

    return surface;
}

AdminEndpoint::AdminEndpoint(std::unique_ptr<BlockingListener> listener,
                             IMetricsSink& metrics,
                             AdminHttpServer::SnapshotProvider snapshot,
                             std::string boundEndpoint,
                             ILogger& logger,
                             std::vector<AdminRoute> routes,
                             TlsContext* tls):
    _listener { std::move(listener) },
    _server { std::make_unique<AdminHttpServer>(*_listener, metrics, std::move(snapshot), logger, std::move(routes), tls) },
    _boundEndpoint { std::move(boundEndpoint) },
    _thread { [server = _server.get()] { SyncRun(server->Run()); } }
{
}

AdminEndpoint::~AdminEndpoint()
{
    // Order, not tidiness: closing the listener is what returns `Run()`, and the
    // jthread destructor that follows joins a loop which would otherwise still be
    // parked in `accept()`.
    _server->Shutdown();
}

std::expected<std::unique_ptr<AdminEndpoint>, std::string> AdminEndpoint::Start(std::string_view listenSpec,
                                                                                std::string_view defaultHost,
                                                                                IMetricsSink& metrics,
                                                                                AdminHttpServer::SnapshotProvider snapshot,
                                                                                ILogger& logger,
                                                                                std::vector<AdminRoute> routes,
                                                                                TlsContext* tls)
{
    auto const endpoint = ParseEndpoint(listenSpec, defaultHost);
    if (!endpoint.has_value())
        return std::unexpected { std::format("'{}' is not [<address>:]<port>", listenSpec) };

    auto listener = BlockingListener::Bind(endpoint->first, endpoint->second);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{} ({})",
                                             endpoint->first,
                                             endpoint->second,
                                             listener ? listener->BindError() : std::string_view { "null listener" }) };

    // The endpoint's own values, not this caller's: the daemon serves the same
    // server on the same terms, and two spellings of one decision drift.
    listener->SetTimeouts(AdminHttpServer::AcceptPoll, AdminHttpServer::RequestTimeout);

    // `new` rather than `make_unique` because the constructor is private: the two
    // ways to reach it are this factory, which has already proved the listener is
    // bound, and nothing else.
    return std::unique_ptr<AdminEndpoint> { new AdminEndpoint { std::move(listener),
                                                                metrics,
                                                                std::move(snapshot),
                                                                std::format("{}:{}", endpoint->first, endpoint->second),
                                                                logger,
                                                                std::move(routes),
                                                                tls } };
}

} // namespace FastCache::Node
