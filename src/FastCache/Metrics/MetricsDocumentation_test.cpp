// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

/// The daemon's documented per-series table against the exposition it renders.
///
/// ## Why this exists
///
/// `fastcached` exported two compile-cache refusal counters that appeared in **no
/// documentation at all**, while `fastcache-compile-node` documented its twin of one
/// of them ([#648](https://github.com/LASTRADA-Software/fastcached/issues/648)). An
/// operator watching a rolling upgrade follows the node series because it is the one
/// the documentation names, sees it settle, and reads a mixed fleet as a converged
/// one — which is the exact judgement those counters exist to support.
///
/// `.agent/rules/metrics-and-observability.md` already records the failure of an
/// operator being told to scrape a series that was never exported. This is the same
/// fact from the other end: a series exported that nobody was ever told to scrape.
/// Nothing connected the two — `MetricsCatalog` decides what is rendered, and the
/// prose is prose — so this is the connection, and it fails in **both** directions:
///
///   - a series the daemon renders and the page does not name, which is #648;
///   - a series the page names and the daemon does not render, which is what a
///     rename leaves behind and is the worse direction, because an operator then
///     alerts on a series that will read "no data" forever.
///
/// ## Why it renders rather than reading `CounterTable`
///
/// Reading the catalog would cover the counters and miss everything else: the
/// storage block, the per-tier series and `fastcached_uptime_seconds` are rendered
/// from `MetricsSnapshot` and appear in no table this test could walk. Rendering the
/// production formatter over a snapshot that has every optional part present asks
/// the question about the actual exposition instead.
///
/// ## What it does NOT cover, stated rather than left to be discovered
///
/// **The `fastcache_*` series** (no `d`). Every process that serves this endpoint
/// exports every counter, so a daemon's scrape also carries the worker, node and
/// scheduler series — flat at zero, because nothing in that binary moves them. They
/// are `fastcache-compile-node`'s and are documented on its page; documenting sixty
/// permanently-zero rows on the daemon's page would bury the rows that mean
/// something. The deployment page says so in prose, and that prose is outside what
/// this check can verify.
///
/// **Whether a description is right.** This compares names. A row whose sentence is
/// wrong passes.

namespace
{

/// The page that carries the daemon's per-series table.
constexpr std::string_view DeploymentPage = "docs/operations/deployment.md";

/// The heading the table lives under, and the level a sibling heading ends it at.
constexpr std::string_view MetricsHeading = "## Metrics";

/// Only series in the daemon's own namespace are checked. See the note above.
constexpr std::string_view DaemonPrefix = "fastcached_";

/// The page that carries the compile node's per-series tables.
constexpr std::string_view NodePage = "docs/tools/fastcache-compile-node.md";

/// The node's namespace.
///
/// It cannot collide with the daemon's despite the shared stem: `fastcached_` has a
/// `d` where this one has its separator, so no name matches both prefixes.
constexpr std::string_view NodePrefix = "fastcache_";

/// A snapshot with every optional part present, so the renderer emits every line
/// it is capable of emitting.
///
/// Absent parts are the point of several of those options — a process with no cache
/// renders no storage block, a cache with no disk tier renders no `tier="disk"`
/// sample — so a snapshot that left any of them out would make this check agree
/// with a page that had lost exactly those rows.
/// @return A snapshot that exercises every branch of `RenderPrometheus`.
[[nodiscard]] MetricsSnapshot FullySpecifiedSnapshot()
{
    MetricsSnapshot snapshot;
    snapshot.storage = StorageStats {};
    for (auto const& row: StorageTierTable)
        snapshot.storageTiers[static_cast<std::size_t>(row.tier)] = StorageStats {};
    snapshot.uptime = Uptime { std::chrono::seconds { 1 } };
    // Left absent deliberately: `host` and `upstreamConfigured` are what a compile
    // node answers and the daemon does not, and their series are the node's.
    snapshot.host = std::nullopt;
    snapshot.upstreamConfigured = std::nullopt;
    return snapshot;
}

/// The same, as a compile NODE answers it.
///
/// The two optionals the daemon leaves empty are populated here, because their
/// series are exactly the ones the node's page documents — and a snapshot without
/// them would let the page lose those rows unnoticed, which is the direction this
/// check exists for.
/// @return A snapshot exercising the node-only branches of `RenderPrometheus`.
[[nodiscard]] MetricsSnapshot NodeShapedSnapshot()
{
    auto snapshot = FullySpecifiedSnapshot();
    snapshot.host = HostCapacity {};
    snapshot.upstreamConfigured = true;
    return snapshot;
}

/// Every `fastcached_*` series name in a rendered exposition.
/// @param exposition Prometheus text exposition.
/// @return The distinct series names, sorted.
[[nodiscard]] std::set<std::string> SeriesIn(std::string_view exposition, std::string_view prefix)
{
    std::set<std::string> names;
    constexpr std::string_view Marker = "# HELP ";
    for (std::size_t at = exposition.find(Marker); at != std::string_view::npos; at = exposition.find(Marker, at + 1))
    {
        auto const nameStart = at + Marker.size();
        auto const nameEnd = exposition.find(' ', nameStart);
        if (nameEnd == std::string_view::npos)
            continue;
        auto const name = exposition.substr(nameStart, nameEnd - nameStart);
        if (name.starts_with(prefix))
            names.emplace(name);
    }
    return names;
}

/// Every `fastcached_*` series name written in `text`.
///
/// The tier series are written in the prose with their label
/// (`fastcached_tier_items{tier="memory"}`), so the token ends at the first
/// character an identifier cannot carry — which is the same name the exposition
/// emits its `# HELP` for.
///
/// A token ending in `_` is a PREFIX being discussed rather than a series being
/// documented — the page says "the `fastcached_dispatch_*` block" and "the
/// `fastcached_` prefix" — and no series name ends that way. Dropping them is
/// exact rather than a heuristic: it is a property of the name, not a guess about
/// the sentence around it.
/// @param text Markdown to scan.
/// @return The distinct names, sorted.
[[nodiscard]] std::set<std::string> DocumentedSeriesIn(std::string_view text, std::string_view prefix)
{
    std::set<std::string> names;
    for (std::size_t at = text.find(prefix); at != std::string_view::npos; at = text.find(prefix, at + 1))
    {
        auto end = at;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_'))
            ++end;
        auto const name = text.substr(at, end - at);
        if (name.size() > prefix.size() && !name.ends_with('_'))
            names.emplace(name);
    }
    return names;
}

/// The `## Metrics` section of the deployment page.
///
/// Scoped to the section rather than the whole file so an unrelated mention
/// elsewhere — a package filename, say — cannot be read as a series name. The
/// caller asserts the section was found: a scan over an empty string agrees with
/// every table perfectly, which is the shape this whole check exists to refuse.
/// @param page The whole document.
/// @return The section body, or empty when the heading is absent.
[[nodiscard]] std::string_view MetricsSectionOf(std::string_view page)
{
    auto const start = page.find(MetricsHeading);
    if (start == std::string_view::npos)
        return {};
    auto const bodyStart = start + MetricsHeading.size();
    auto const next = page.find("\n## ", bodyStart);
    return page.substr(bodyStart, next == std::string_view::npos ? std::string_view::npos : next - bodyStart);
}

/// Read a file whole.
/// @param path Absolute path.
/// @return Its bytes, or nullopt when it could not be opened.
[[nodiscard]] std::optional<std::string> ReadWhole(std::filesystem::path const& path)
{
    std::ifstream stream { path, std::ios::binary };
    if (!stream)
        return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// `a` minus `b`, as a comma-joined string for a failure message.
/// @param a Left set.
/// @param b Right set.
/// @return The names in `a` and not in `b`.
[[nodiscard]] std::string Missing(std::set<std::string> const& a, std::set<std::string> const& b)
{
    std::vector<std::string> difference;
    std::ranges::set_difference(a, b, std::back_inserter(difference));
    std::string joined;
    for (auto const& name: difference)
    {
        if (!joined.empty())
            joined += ", ";
        joined += name;
    }
    return joined;
}

} // namespace

TEST_CASE("Every fastcached_ series the daemon renders is documented, and vice versa", "[metrics][docs]")
{
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const read = ReadWhole(root / DeploymentPage);
    // A page that cannot be read is not a page with nothing wrong in it. Four
    // states, and "could not observe" must not collapse into "observed and fine".
    REQUIRE(read.has_value());
    auto const& page = Unwrap(read);

    auto const section = MetricsSectionOf(page);
    // Positive control. Without it, a renamed heading turns this into a comparison
    // against an empty set, which reports every rendered series as undocumented --
    // loud -- but a comparison in the OTHER direction that passes vacuously.
    REQUIRE_FALSE(section.empty());

    AtomicMetricsSink sink;
    auto const exposition = RenderPrometheus(sink, FullySpecifiedSnapshot());
    auto const rendered = SeriesIn(exposition, DaemonPrefix);
    auto const documented = DocumentedSeriesIn(section, DaemonPrefix);

    // A census returning zero is the absence of a verdict, not a verdict. Both sets
    // are asserted non-trivial before either difference is believed, and the floor
    // is deliberately well under the real figure so it pins "the scan works" rather
    // than becoming a count somebody has to maintain.
    REQUIRE(rendered.size() > 30);
    REQUIRE(documented.size() > 30);

    INFO("rendered " << rendered.size() << " series, the page names " << documented.size());
    // Named, not counted: "64 against 66" is arithmetic that is true and tells
    // nobody which rows to go and write.
    INFO("rendered and undocumented: [" << Missing(rendered, documented) << "]");
    INFO("documented and never rendered: [" << Missing(documented, rendered) << "]");
    CHECK(Missing(rendered, documented).empty()); // #648: exported and documented nowhere.
    CHECK(Missing(documented, rendered).empty()); // The other direction: documented and never exported.
}

TEST_CASE("The daemon's two compile-cache store refusals are named on the page", "[metrics][docs]")
{
    // #648 by name. The set comparison above would catch these along with every
    // other series, and that is precisely why they are also pinned individually: a
    // future weakening of the scan -- a heading rename, a prefix change -- takes the
    // general check down to a vacuous pass, and these two are the ones the ticket is
    // about. They are also the two an operator needs BOTH ends of, because the node
    // has an identically-shaped twin of the first and watching only that reads a
    // mixed fleet as a converged one.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const read = ReadWhole(root / DeploymentPage);
    REQUIRE(read.has_value());
    auto const& page = Unwrap(read);

    CHECK(page.contains("fastcached_cache_stores_refused_foreign_generation_total"));
    CHECK(page.contains("fastcached_cache_stores_refused_not_a_compile_value_total"));
    // And the node's twin is named beside it, or the page documents one end of a
    // two-ended question.
    CHECK(page.contains("fastcache_node_cache_requests_refused_foreign_generation_total"));
}

TEST_CASE("Every fastcache_ series the node renders is documented, and vice versa", "[metrics][docs]")
{
    // #553, the node's half of the same gap. Both directions are live drift, and
    // both are invisible: a counter exported and undocumented is one an operator
    // never learns exists, and a documented series never exported is one they are
    // told to scrape that will never appear -- which `AGENT.md`'s scar list already
    // names as one of the four failures the rulebook was written from.
    //
    // ## Why this compares against the RENDERER and not against `MetricsCatalog`
    //
    // The ticket proposes reading the catalog. Measured, that produces a wrong
    // answer in the dangerous direction: `fastcache_node_upstream_configured` is
    // documented, is exported, and is **not a `CounterTable` row** -- it is rendered
    // from `MetricsSnapshot::upstreamConfigured`. It appears in `MetricsCatalog.hpp`
    // only inside another row's help text. So a scan keyed on `prometheusName`
    // reports it as documented-but-never-exported, and the remedy that reading
    // suggests is deleting a correct row from the page.
    //
    // That is a check inventing a finding rather than missing one, which is the
    // worse direction and the one that gets acted on. `RenderPrometheus` is the
    // authority on what is exported; the catalog is the authority on what is
    // counted, and they are not the same set.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const read = ReadWhole(root / NodePage);
    // A page that cannot be read is not a page with nothing wrong in it.
    REQUIRE(read.has_value());
    auto const& page = Unwrap(read);

    AtomicMetricsSink sink;
    auto const exposition = RenderPrometheus(sink, NodeShapedSnapshot());
    auto const rendered = SeriesIn(exposition, NodePrefix);
    auto const documented = DocumentedSeriesIn(page, NodePrefix);

    // Both censuses non-trivial before either difference is believed: two empty
    // lists agree perfectly, which is `node-config-reference`'s rule and the one
    // this check would otherwise pass vacuously under the day somebody renames a
    // heading.
    REQUIRE(rendered.size() > 30);
    REQUIRE(documented.size() > 20);

    INFO("the node renders " << rendered.size() << " series, the page names " << documented.size());
    INFO("rendered and undocumented: [" << Missing(rendered, documented) << "]");
    INFO("documented and never rendered: [" << Missing(documented, rendered) << "]");
    CHECK(Missing(rendered, documented).empty());
    CHECK(Missing(documented, rendered).empty());
}

TEST_CASE("The node's page is scanned whole, not just its tables", "[metrics][docs]")
{
    // The daemon's check scopes itself to one `## Metrics` section because that page
    // mentions no series anywhere else. The node's page names them in prose, in
    // several tables, and in a sample transcript -- so scoping to one heading would
    // silently exclude most of them and the check would pass while guarding a
    // fraction.
    //
    // Asserted rather than left as a comment: the scan must see series from more
    // than one region of the page, or it has been narrowed without anybody noticing.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const read = ReadWhole(root / NodePage);
    REQUIRE(read.has_value());
    auto const& page = Unwrap(read);

    auto const all = DocumentedSeriesIn(page, NodePrefix);
    REQUIRE_FALSE(all.empty());

    // A series documented only in the refusal-counter table, and one documented only
    // in the cache-tier discussion. Neither is in the other's section.
    CHECK(all.contains("fastcache_worker_jobs_refused_no_slot_total"));
    CHECK(all.contains("fastcache_node_cache_hits_total"));
}
