// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

/// @file DiscoveryBounds.hpp
/// Every bound on what an unauthenticated datagram can make discovery hold or sign, as ONE table.
///
/// A beacon is unauthenticated by construction: anything on the segment can send one, naming any
/// cluster and any node, from any source address. So every table a beacon grows is BOUNDED, and the
/// bound is spent in one way everywhere: **what the roster or an answered challenge vouches for is
/// kept; at the bound, the OLDEST entry nothing vouches for is displaced.** Refusing the newcomer
/// instead would let a few spoofed datagrams a second hold a table full forever and keep every real
/// peer or fleet out of it; displacing the oldest makes a spoofer race the real peer's round trip.
/// Every displacement and every drop is counted (`DiscoveryBeaconsOverBound`).
///
/// **No table holds an outstanding challenge**: a challenge's nonce is a cookie this node
/// recomputes from the proof (`ChallengeCookies`), so a flood of invented ids has nothing to
/// displace. What a key epoch DOES keep is a two-bit state for every cookie it issued -- answered,
/// forged at, spent -- so it issues at most `MaxEpochChallenges` before a new key is drawn, and its
/// record is never full for a cookie it issued.
///
/// The WORK BUDGETS bound signature work rather than memory (`WorkBudgets`): answering a challenge
/// costs this node a signing, and checking a proof whose cookie holds costs it a verification. Each
/// is rate-limited twice -- per SOURCE host, in a table of at most `MaxBudgetedSources` hosts, then
/// all together. Past either, the work is not done, and the next beacon round asks again.
namespace FastCache::Cluster
{

/// Which bound a row describes.
///
/// **Private**: never transmitted or persisted.
enum class DiscoveryBound : std::uint8_t
{
    EpochChallenges,       ///< Challenges one key epoch issues, each with a place in its state record.
    ForgeriesPerChallenge, ///< Failed signature checks one challenge's cookie buys before it is exhausted.
    ForeignFleets,         ///< Other fleets remembered at once.
    UnrosteredPeers,       ///< This cluster's peers the roster holds no key for, remembered at once.
    BudgetedSources,       ///< Source hosts whose signature work is budgeted apart, remembered at once, per kind.
    Last,
};

/// One bound: its size and what it keeps from growing.
struct DiscoveryBoundRow
{
    DiscoveryBound bound;   ///< The enumerator this row describes.
    std::size_t limit;      ///< How many entries it holds at most.
    std::string_view holds; ///< What it bounds, for a reader and a log line.
};

/// Every bound, in enumerator order.
///
/// What the roster vouches for is bounded by the roster, which only an operator grows, and is
/// never counted here: a spoofer can name a rostered id, but the directory keeps one entry per id.
inline constexpr EnumTable<DiscoveryBound, DiscoveryBoundRow> DiscoveryBounds { {
    // 2^16: a two-bit state each is a 16 KiB record per epoch, two held at once. An honest segment
    // issues one challenge per beacon heard, some hundreds a round at most; and forcing the TWO
    // rotations that would expire an honest cookie inside its round trip takes 2^17 challenges --
    // beacons this node answers one each, at an HMAC apiece, never a record anybody can fill.
    { .bound = DiscoveryBound::EpochChallenges,
      .limit = std::size_t { 1 } << 16U,
      .holds = "challenges one key epoch issues, each with a two-bit state" },
    // Two: a cookie is not spent by a forgery, so without a cap one live cookie buys a signature
    // check per datagram from as many source addresses as a flood can type. Past it the cookie is
    // exhausted, which a forger racing the honest answer can arrange -- as it could make a spoofed
    // proof arrive first under any design -- and the next beacon round asks again.
    { .bound = DiscoveryBound::ForgeriesPerChallenge,
      .limit = 2,
      .holds = "failed signature checks one challenge buys before it is exhausted" },
    { .bound = DiscoveryBound::ForeignFleets, .limit = 16, .holds = "other fleets remembered" },
    { .bound = DiscoveryBound::UnrosteredPeers,
      .limit = 64,
      .holds = "this cluster's peers the roster holds no key for, remembered" },
    { .bound = DiscoveryBound::BudgetedSources,
      .limit = 64,
      .holds = "source hosts whose signature work is budgeted apart, per kind of work" },
} };
static_assert(RowsInEnumeratorOrder(DiscoveryBounds, &DiscoveryBoundRow::bound),
              "DiscoveryBounds must hold one row per DiscoveryBound, in enumerator order");

/// The limit of @p bound.
/// @param bound Which bound.
/// @return Its limit.
[[nodiscard]] constexpr std::size_t LimitOf(DiscoveryBound bound) noexcept
{
    return DiscoveryBounds[static_cast<std::size_t>(bound)].limit;
}

/// How many challenges one key epoch issues before the next challenge draws a new key.
inline constexpr std::size_t MaxEpochChallenges = LimitOf(DiscoveryBound::EpochChallenges);

/// How many failed signature checks one challenge buys before it is exhausted.
inline constexpr std::size_t MaxForgeriesPerChallenge = LimitOf(DiscoveryBound::ForgeriesPerChallenge);

/// How many other fleets a directory remembers at once.
inline constexpr std::size_t MaxForeignFleets = LimitOf(DiscoveryBound::ForeignFleets);

/// How many of this cluster's unrostered peers a directory remembers at once.
inline constexpr std::size_t MaxUnrosteredPeers = LimitOf(DiscoveryBound::UnrosteredPeers);

/// How many source hosts one work budget keeps a bucket for at once.
inline constexpr std::size_t MaxBudgetedSources = LimitOf(DiscoveryBound::BudgetedSources);

/// The longest id -- a cluster's or a node's -- a datagram may name, where an id enters: a beacon's
/// summary, a challenge's asker, a seed's answer, and this node's own formation record.
///
/// The grammar's bound rather than a row of the table above -- it is a length, not a count of
/// entries -- and named here so every bound on what an unauthenticated datagram can make
/// discovery hold is in this file. Defined where the summary's codec is, which the launcher
/// compiles in and which may include nothing from this layer.
using CompileCacheWire::MaxIdBytes;

/// A kind of signature work an unauthenticated datagram can make discovery do.
///
/// **Private**: never transmitted or persisted.
enum class DiscoveryWork : std::uint8_t
{
    Answer,     ///< Sign a proof, answering a challenge from anybody.
    ProofCheck, ///< Verify a proof whose cookie holds -- a live challenge, which a forgery does not spend.
    Last,
};

/// What one kind of signature work may cost: a shared burst and rate, and one source host's.
struct WorkBudgetRow
{
    DiscoveryWork work;                          ///< The enumerator this row describes.
    std::size_t burst;                           ///< Done at once, by everybody together.
    std::chrono::microseconds refillEvery;       ///< How long one more takes once the burst is spent.
    std::size_t sourceBurst;                     ///< Done at once for one source host.
    std::chrono::microseconds sourceRefillEvery; ///< How long one more takes for that host.
    std::string_view spends;                     ///< What one unit costs, for a reader and a log line.
};

/// Every kind of signature work, in enumerator order, each budget sized from what the work COSTS.
///
/// Measured rather than assumed, with `fastcache-bench "[discovery]" --benchmark-samples 200`, two
/// runs. The conditions are pinned here so a later reading can be compared rather than quoted:
///
/// - Build: MSVC 19.51 Release, /O2 /Ob2 -- the optimiser read from `build.ninja`'s flags, since
///   MSVC states no macro the benchmark banner could confirm.
/// - Host: Ryzen 9 9950X3D, Windows 11, 70-78% loaded by other builds.
/// - Sign a discovery proof: 28.1 us and 33.2 us per operation.
/// - Verify a discovery proof: 97.1 us and 81.1 us per operation.
/// - MAC a challenge cookie: 0.23 us and 0.25 us per operation.
///
/// Each shared rate holds a flood to a few percent of one core: a thousand signings a second at
/// ~33 us is ~33 ms of signing a second, and a check every 3 ms at ~97 us is ~32 ms of checking a
/// second. A thousand answers a second also bounds what a victim receives -- a proof each, no larger
/// than the challenge that asked. The honest load is one challenge from every node that hears a
/// beacon, and one proof from every node this node challenged, per beacon round: far inside each
/// burst, which comes back within a round (`DiscoveryService.hpp` asserts it).
///
/// Per SOURCE HOST first, and the host's share is small: an honest host sends one challenge per
/// beacon it hears, and one proof per challenge it is sent, per node it runs -- a few a round -- so
/// a quarter of a second each is generous, while one host that floods takes its own burst and then
/// four a second, never the budget everybody else's work comes out of. Keyed by HOST, not host and
/// port: a port is as cheap to vary as nothing. A flood that varies its source ADDRESS gets a fresh
/// bucket per address and meets the shared budget instead, which is the bound on work.
inline constexpr EnumTable<DiscoveryWork, WorkBudgetRow> WorkBudgets { {
    { .work = DiscoveryWork::Answer,
      .burst = 128,
      .refillEvery = std::chrono::microseconds { 1'000 },
      .sourceBurst = 8,
      .sourceRefillEvery = std::chrono::microseconds { 250'000 },
      .spends = "a signing, answering a challenge" },
    { .work = DiscoveryWork::ProofCheck,
      .burst = 128,
      .refillEvery = std::chrono::microseconds { 3'000 },
      .sourceBurst = 8,
      .sourceRefillEvery = std::chrono::microseconds { 250'000 },
      .spends = "a signature check, of a proof whose cookie holds" },
} };
static_assert(RowsInEnumeratorOrder(WorkBudgets, &WorkBudgetRow::work),
              "WorkBudgets must hold one row per DiscoveryWork, in enumerator order");

/// The budget of @p work.
/// @param work Which kind of work.
/// @return Its row.
[[nodiscard]] constexpr WorkBudgetRow const& BudgetOf(DiscoveryWork work) noexcept
{
    return WorkBudgets[static_cast<std::size_t>(work)];
}

// One source host's burst must leave the rest of the shared one for everybody else, or a source
// that spends it takes the whole budget and the per-source bound bounds nothing.
static_assert(std::ranges::all_of(WorkBudgets,
                                  [](WorkBudgetRow const& row) {
                                      return row.sourceBurst >= 1 && row.sourceBurst < row.burst;
                                  }),
              "one source's burst must be a share of the shared burst, never all of it");

// An epoch is half a challenge lifetime -- inside one default beacon interval -- and issues one
// challenge per beacon heard, so its cap must sit far above a round's honest challenges, or honest
// load alone would rotate keys early. Every remembered peer and fleet beaconing twice is a floor
// three orders of magnitude below it.
static_assert(MaxEpochChallenges >= 2 * (MaxUnrosteredPeers + MaxForeignFleets),
              "one epoch must issue every remembered peer and fleet its challenges twice over");
static_assert(MaxForgeriesPerChallenge >= 1, "a cookie buys at least one check, or no proof is ever checked");

} // namespace FastCache::Cluster
