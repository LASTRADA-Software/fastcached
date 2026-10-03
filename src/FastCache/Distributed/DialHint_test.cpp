// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/DialHint.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Distributed;

namespace
{
struct Row
{
    std::string_view name;
    std::string_view advertised;
    std::string_view observed;
    std::vector<std::string> interfaces;
    std::string_view hint;        ///< Expected hint; empty when vetoed.
    std::optional<HintVeto> veto; ///< WHICH veto, when one applies.
};
} // namespace

TEST_CASE("A dial hint is the observed host at the advertised port, and each refusal says which", "[distributed][dialhint]")
{
    std::vector<Row> const rows {
        { .name = "the stale-DNS case",
          .advertised = "laptop.corp:6676",
          .observed = "10.8.0.7",
          .interfaces = { "10.8.0.7", "fe80::1" },
          .hint = "10.8.0.7:6676",
          .veto = std::nullopt },
        { .name = "dual-stack peer, plain report",
          .advertised = "laptop.corp:6676",
          .observed = "::ffff:10.8.0.7",
          .interfaces = { "10.8.0.7" },
          .hint = "10.8.0.7:6676",
          .veto = std::nullopt },
        { .name = "an IPv6 observation is bracketed",
          .advertised = "laptop.corp:6676",
          .observed = "2001:db8::7",
          .interfaces = { "2001:db8::7" },
          .hint = "[2001:db8::7]:6676",
          .veto = std::nullopt },
        { .name = "nothing observed",
          .advertised = "laptop.corp:6676",
          .observed = "",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::NoObservedHost },
        { .name = "a bare mapped prefix is nothing",
          .advertised = "laptop.corp:6676",
          .observed = "::ffff:",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::NoObservedHost },
        { .name = "loopback",
          .advertised = "laptop.corp:6676",
          .observed = "127.0.0.1",
          .interfaces = { "127.0.0.1" },
          .hint = "",
          .veto = HintVeto::LoopbackObserved },
        { .name = "advertise with no port",
          .advertised = "laptop.corp",
          .observed = "10.8.0.7",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::AdvertiseUnparsable },
        { .name = "advertise that is prose",
          .advertised = "no leader: try again",
          .observed = "10.8.0.7",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::AdvertiseUnparsable },
        { .name = "an IP-literal advertise",
          .advertised = "10.8.0.5:6676",
          .observed = "10.8.0.7",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::AdvertiseIsLiteral },
        { .name = "a bracketed v6 advertise",
          .advertised = "[2001:db8::5]:6676",
          .observed = "10.8.0.7",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::AdvertiseIsLiteral },
        { .name = "a link-local observation the worker does report",
          .advertised = "laptop.corp:6676",
          .observed = "fe80::1",
          .interfaces = { "fe80::1" },
          .hint = "",
          .veto = HintVeto::LinkLocalObserved },
        { .name = "behind a NAT the worker does not answer on",
          .advertised = "laptop.corp:6676",
          .observed = "203.0.113.9",
          .interfaces = { "10.8.0.7" },
          .hint = "",
          .veto = HintVeto::NotAReportedInterface },
        { .name = "a worker that reported nothing",
          .advertised = "laptop.corp:6676",
          .observed = "10.8.0.7",
          .interfaces = {},
          .hint = "",
          .veto = HintVeto::NotAReportedInterface },
        // Two vetoes apply at once; enumerator order decides which is reported.
        // `LoopbackObserved` (index 1) is checked before `NotAReportedInterface` (index 5),
        // even though an empty interface list would fail that veto too.
        { .name = "a loopback observation from a worker that reported nothing",
          .advertised = "laptop.corp:6676",
          .observed = "127.0.0.1",
          .interfaces = {},
          .hint = "",
          .veto = HintVeto::LoopbackObserved },
        // `LinkLocalObserved` (index 2) is checked before `NotAReportedInterface` (index 5),
        // even though an empty interface list would fail that veto too.
        { .name = "a link-local observation from a worker that reported nothing",
          .advertised = "laptop.corp:6676",
          .observed = "fe80::1",
          .interfaces = {},
          .hint = "",
          .veto = HintVeto::LinkLocalObserved },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const decision = DecideDialHint(DialHintInputs {
            .advertised = row.advertised, .observedHost = row.observed, .interfaceAddresses = row.interfaces });
        CHECK(decision.endpoint == row.hint);
        CHECK(decision.veto == row.veto);
        CHECK(DialHintFor(DialHintInputs {
                  .advertised = row.advertised, .observedHost = row.observed, .interfaceAddresses = row.interfaces })
              == row.hint);
    }
}

TEST_CASE("Every veto has a name", "[distributed][dialhint]")
{
    for (auto const& row: HintVetoes)
        CHECK_FALSE(row.name.empty());
}
