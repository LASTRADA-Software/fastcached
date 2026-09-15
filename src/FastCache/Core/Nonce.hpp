// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>

namespace FastCache
{

/// How many bytes a handshake nonce carries.
///
/// One constant for every handshake the cluster key takes part in -- the discovery
/// challenge and both halves of the Raft peer handshake -- because a nonce's size is a
/// claim about how unlikely a repeat is, and two protocols sized apart are two such
/// claims that can drift (#1308). 256 bits puts the chance of any two nonces in a
/// fleet's lifetime colliding below anything worth naming, which is the property a
/// nonce is for.
inline constexpr std::size_t NonceBytes = 32;

static_assert(NonceBytes >= 32, "a nonce below 256 bits weakens the repeat bound every handshake relies on");
static_assert(NonceBytes % sizeof(std::uint64_t) == 0, "DrawNonce fills a nonce one 64-bit draw at a time");

/// A handshake nonce.
using Nonce = std::array<std::byte, NonceBytes>;

/// A fresh nonce, drawn through the randomness seam.
///
/// **What a nonce here needs is that it never REPEATS, not that nobody can predict
/// it**, and the difference is why this reaches `IRandomSource` rather than a
/// cryptographic generator. Every handshake that uses one is a MAC under the cluster
/// key over BOTH ends' nonces, answered live: a peer that guessed the next nonce could
/// ask a key holder for a tag over it ahead of time, and would then hold a tag that is
/// only ever accepted by a connection whose other half is that same key holder speaking
/// live -- a relay, which the network already is. A repeat is different: it lets a
/// recorded exchange be replayed whole. So the bound that matters is the collision one
/// `NonceBytes` states, and a 64-bit-seeded engine meets it.
///
/// Drawn here, once, rather than at each handshake, because the draw loop was written
/// inline in `DiscoveryService::IssueChallenge` and the Raft handshake needs the same
/// bytes the same way (#1308): a second loop over `UniformInRange` is a second place for
/// the size and the source to disagree.
/// @param random Where the bytes come from; a test scripts it to fix the nonce.
/// @return The nonce, each 64-bit draw written big-endian in turn.
[[nodiscard]] inline Nonce DrawNonce(IRandomSource& random)
{
    Nonce nonce {};
    for (auto const index: std::views::iota(std::size_t { 0 }, NonceBytes / sizeof(std::uint64_t)))
    {
        auto const draw = random.UniformInRange(0, std::numeric_limits<std::uint64_t>::max());
        WriteBigEndian<std::uint64_t>(std::span { nonce }.subspan(index * sizeof(std::uint64_t), sizeof(std::uint64_t)),
                                      draw);
    }
    return nonce;
}

} // namespace FastCache
