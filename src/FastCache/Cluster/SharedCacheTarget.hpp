// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cluster
{

/// What resolving the `shared-cache` setting against the applied state came to.
///
/// **PRIVATE: persisted and transmitted nowhere.** It is recomputed at every apply from
/// state a node already holds, never stored and never sent -- `--node-status` maps it to
/// its own wire enumeration (`WireSharedCacheState`) rather than sending this one, so its
/// ordinals carry no wire contract and appending a reason moves nothing else.
enum class SharedCacheResolution : std::uint8_t
{
    Unset,          ///< The setting names no machine: there is no shared cache to dial.
    Resolved,       ///< A machine, its live key and its endpoint are all in hand.
    ThisMachine,    ///< The setting names this node, which serves the tier rather than dialling it.
    UnknownMachine, ///< The setting names a machine this cluster no longer holds -- forgotten, or never admitted.
    KeyRevoked,     ///< The named machine's recorded key is revoked.
    NoEndpoint,     ///< The named machine has not yet announced a `0xFC` endpoint.
    Last,           ///< Not a resolution, and never travels. See `RowsInEnumeratorOrder`.
};

/// How one `SharedCacheResolution` is spelled, and what an operator or a caller may do with it.
struct SharedCacheResolutionRow
{
    SharedCacheResolution resolution; ///< The resolution this row describes.
    std::string_view word;            ///< Its one spelling: a log line, a node-status detail and a condition alike.
    bool dials;                       ///< Whether an operation may dial the resolved endpoint.

    /// Whether the shared cache is a store this node is CONFIGURED to use, whatever became of it.
    ///
    /// `ICacheUpstream::Configured()`'s answer: a store this node was told to use and that then
    /// fails -- an unknown machine, a revoked key, no endpoint yet -- is `Declined`,
    /// never `NotConfigured`. Only `Unset` (nobody named a shared cache) and `ThisMachine` (there
    /// is no remote store to be configured with) are not configured.
    bool configured;

    /// The sentence an operator reads, naming what to do; empty for `Resolved` and `Unset`, which
    /// need no remedy.
    std::string_view why;
};

/// One row per `SharedCacheResolution`, in enumerator order.
inline constexpr EnumTable<SharedCacheResolution, SharedCacheResolutionRow> SharedCacheResolutionTable { {
    { .resolution = SharedCacheResolution::Unset, .word = "unset", .dials = false, .configured = false, .why = {} },
    { .resolution = SharedCacheResolution::Resolved, .word = "resolved", .dials = true, .configured = true, .why = {} },
    { .resolution = SharedCacheResolution::ThisMachine,
      .word = "this-machine",
      .dials = false,
      .configured = false,
      .why = "the fleet's shared-cache setting names this machine, which serves the shared tier and reads it in "
             "process rather than dialling itself" },
    { .resolution = SharedCacheResolution::UnknownMachine,
      .word = "unknown-machine",
      .dials = false,
      .configured = true,
      .why = "the shared-cache setting names a machine this cluster no longer holds -- forgotten, or never "
             "admitted; set it to a current machine, or to empty" },
    { .resolution = SharedCacheResolution::KeyRevoked,
      .word = "key-revoked",
      .dials = false,
      .configured = true,
      .why = "the named machine's key is revoked; nothing it proves is trusted again" },
    { .resolution = SharedCacheResolution::NoEndpoint,
      .word = "no-endpoint",
      .dials = false,
      .configured = true,
      .why = "the cluster records no 0xFC endpoint for the named machine yet; it is recorded when that machine "
             "joins or next announces itself" },
} };

static_assert(RowsInEnumeratorOrder(SharedCacheResolutionTable, &SharedCacheResolutionRow::resolution),
              "SharedCacheResolutionTable must hold one row per SharedCacheResolution, in enumerator order");

static_assert(std::ranges::all_of(SharedCacheResolutionTable,
                                  [](SharedCacheResolutionRow const& row) {
                                      return !row.word.empty()
                                             && (row.why.empty()
                                                 == (row.resolution == SharedCacheResolution::Unset
                                                     || row.resolution == SharedCacheResolution::Resolved));
                                  }),
              "every resolution has a word, and every one that is not a plain answer says why");

static_assert(std::ranges::all_of(SharedCacheResolutionTable,
                                  [](SharedCacheResolutionRow const& row) {
                                      return row.dials == (row.resolution == SharedCacheResolution::Resolved);
                                  }),
              "only a resolved target is dialled; every other answer fails closed");

/// The row for @p resolution, which every reader of a resolution reaches the table through.
/// @param resolution The resolution; `Last` is a count, not a resolution, and passing it is a programmer error.
/// @return Its row.
[[nodiscard]] constexpr SharedCacheResolutionRow const& RowOf(SharedCacheResolution resolution) noexcept
{
    return SharedCacheResolutionTable[static_cast<std::size_t>(resolution)];
}

/// What the `shared-cache` setting resolved to, against the state a node already holds.
struct ResolvedSharedCache
{
    /// Which of `SharedCacheResolutionTable`'s rows this is.
    SharedCacheResolution resolution { SharedCacheResolution::Unset };

    /// The id the setting names, or empty for `Unset` alone -- every other resolution names one,
    /// even the ones that could not be reached, so a report can say WHICH machine failed and how.
    std::string machineId;

    /// The named machine's `0xFC` endpoint; engaged meaningfully only for `Resolved`.
    std::string endpoint;

    /// The key the peer must prove to be trusted as the shared cache; engaged only for `Resolved`.
    std::optional<Ed25519PublicKey> key;

    [[nodiscard]] friend bool operator==(ResolvedSharedCache const&, ResolvedSharedCache const&) = default;
};

/// Resolve the fleet's `shared-cache` setting against @p state, for @p selfId.
///
/// **Pure over what a node already holds, and recomputed at every apply** -- nothing here is
/// cached, because a stale resolution is a wrong endpoint dialled with confidence rather than a
/// miss (the caching rule in AGENT.md). Reads `ClusterState::SettingOf` for the name,
/// `ClusterState::SchedulerEndpointOf` for the member's advertised `0xFC` endpoint -- recorded for
/// every member, learners included, since the fleet verbs the shared cache serves ride that same
/// surface -- `LiveKeyOf` for the key it must prove, and `ClusterState::IsRevoked` for whether that
/// key still counts.
/// @param state The replicated state.
/// @param selfId This node's own id.
/// @return The resolution, one row of `SharedCacheResolutionTable`.
[[nodiscard]] ResolvedSharedCache ResolveSharedCache(ClusterState const& state, std::string_view selfId);

} // namespace FastCache::Cluster
