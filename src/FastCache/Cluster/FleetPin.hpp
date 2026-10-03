// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file FleetPin.hpp
/// The one fleet an operator allowed this node to belong to (`--fleet-id`), and the one predicate
/// every formation decision asks of it.
///
/// **Zero-config discovery is trust-on-first-use.** A fleet's proof binds a KEY, never the truth of
/// its summary (`ProvenFleetSummary`), and a key costs nothing: anybody on the segment can mint one
/// and prove "established, created at 0", and every solitary node that hears it would yield to it,
/// enroll, and on its approval adopt its roster -- dispatching its launchers' source to that fleet's
/// workers and compiling that fleet's jobs.
///
/// **The pin is a KEY pin, because a cluster id is a name.** Every beacon carries the id, so a pin by
/// name alone stops nobody who can hear the fleet it names: a machine on that very segment proves a
/// fleet CLAIMING the id under a key of its own. So a pin names the cluster AND the identity keys of
/// its voters -- an office has one or two, so listing them is cheap and no chain of trust is needed --
/// and what a pinned node accepts on its way INTO its fleet is what one of those keys SIGNED: the
/// summary it yields to, every answer to its `Enroll`, the leader it is redirected to, the admission
/// its record keeps. **It anchors the JOIN**: once joined, the fleet's applied state is the authority,
/// as for any member, so a capture of the fleet's quorum after the join is not something a pin sees.
namespace FastCache::Cluster
{

/// How many voter keys one pin may name: more than any office runs, and a bound on the text a pin
/// travels as (`NODE-STATUS`, a service registration, an installer property).
inline constexpr std::size_t MaxPinnedVoterKeys = 16;

/// A cluster and the voters a pinned node takes its word from.
struct PinnedFleet
{
    std::string clusterId;                   ///< The cluster, as a mint spells it; compared WHOLE.
    std::vector<Ed25519PublicKey> voterKeys; ///< Never empty: a pin by name alone is refused where typed.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(PinnedFleet const&, PinnedFleet const&) = default;
};

/// The fleet an operator pinned this node to, or none.
///
/// A type rather than a bare `optional`, so every decision that must ask it takes it as a REQUIRED
/// parameter a caller cannot omit and cannot confuse with some other optional.
struct FleetPin
{
    std::optional<PinnedFleet> fleet; ///< Absent when the node trusts on first use.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FleetPin const&, FleetPin const&) = default;
};

/// Whether @p pin lets this node take @p clusterId's word, as one of @p signers signed it: always when
/// nothing is pinned; otherwise exactly when the ids are the same string -- never a prefix, never
/// case-folded -- AND one of @p signers is a pinned voter key.
/// @param pin The node's pin.
/// @param clusterId The cluster a decision is about to commit this node to.
/// @param signers The keys that vouch for it: whoever signed the summary, the answer or the order.
/// @return True when the pin admits it.
[[nodiscard]] constexpr bool AdmitsFleet(FleetPin const& pin,
                                         std::string_view clusterId,
                                         std::span<Ed25519PublicKey const> signers) noexcept
{
    if (!pin.fleet.has_value())
        return true;
    if (std::string_view { pin.fleet->clusterId } != clusterId)
        return false;
    return std::ranges::any_of(
        signers, [&pin](Ed25519PublicKey const& key) { return std::ranges::contains(pin.fleet->voterKeys, key); });
}

/// `AdmitsFleet` for the one key that signed what is being decided on.
/// @param pin The node's pin.
/// @param clusterId The cluster a decision is about to commit this node to.
/// @param signer The key that signed it.
/// @return True when the pin admits it.
[[nodiscard]] constexpr bool AdmitsFleet(FleetPin const& pin,
                                         std::string_view clusterId,
                                         Ed25519PublicKey const& signer) noexcept
{
    return AdmitsFleet(pin, clusterId, std::span<Ed25519PublicKey const> { &signer, 1 });
}

/// Spell @p fleet as `--fleet-id` takes it: `<cluster-id>@<key>[,<key>...]`, every key through the
/// one encoder. What `fastcache-cli node` prints for an operator to paste, so it round-trips through
/// `ParsePinnedFleet`.
/// @param fleet The pinned fleet.
/// @return Its text.
[[nodiscard]] std::string FormatPinnedFleet(PinnedFleet const& fleet);

/// What @p pin says where an operator reads it: `FormatPinnedFleet`, or `none`.
/// @param pin The node's pin.
/// @return The text.
[[nodiscard]] std::string PinText(FleetPin const& pin);

/// Read `--fleet-id`'s value.
///
/// The cluster id is held to the grammar a mint spells (`IsMintedClusterId`); every key to the one
/// key parser; at least one key and at most `MaxPinnedVoterKeys`, none twice. **An id with no key is
/// refused by name, never read as a pin by name**: that would be a confident wrong signal of safety,
/// since every beacon carries the id.
/// @param text The value.
/// @return The pinned fleet, or the sentence saying why the text is not one.
[[nodiscard]] std::expected<PinnedFleet, std::string> ParsePinnedFleet(std::string_view text);

} // namespace FastCache::Cluster
