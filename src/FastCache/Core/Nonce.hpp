// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/ISecureRandom.hpp>

#include <array>
#include <cstddef>
#include <expected>

namespace FastCache
{

/// How many bytes a handshake nonce carries.
///
/// One constant for every handshake a node proves itself in -- the discovery challenge,
/// the node proof and both halves of the Raft peer handshake -- because a nonce's size is a
/// claim about how unlikely a repeat is, and two protocols sized apart are two such
/// claims that can drift (#1308). 256 bits puts the chance of any two nonces in a
/// fleet's lifetime colliding below anything worth naming, which is the property a
/// nonce is for.
inline constexpr std::size_t NonceBytes = 32;

static_assert(NonceBytes >= 32, "a nonce below 256 bits weakens the repeat bound every handshake relies on");

/// A handshake nonce.
using Nonce = std::array<std::byte, NonceBytes>;

/// A fresh nonce, drawn from the operating system's generator.
///
/// **What a nonce here needs is that it never REPEATS, not that nobody can predict it.**
/// Every handshake that uses one is answered live, with a signature or a MAC by a key holder
/// over the nonces it was given: a peer that guessed the next nonce could ask a key holder for
/// a proof over it ahead of time, and would then hold a proof that is only ever accepted by a
/// connection whose other half is that same key holder speaking live -- a relay, which the
/// network already is. A repeat is different: it lets a recorded exchange be replayed whole. So the
/// bound that matters is the collision one `NonceBytes` states.
///
/// **And that bound needs an ENTROPY SOURCE, not a seeded engine** (#1527). This drew from
/// `IRandomSource` until then, on the argument that "a 64-bit-seeded engine meets it" -- which
/// holds only while the seed is random, and nothing checked that it was. The engine was seeded
/// from `std::random_device`, which on the host #1507 was measured on answers zero for 57% of
/// draws (measured). Both seed halves zero is then about a third of processes (0.57 squared,
/// inferred rather than reproduced), each drawing the SAME nonce stream -- so an acceptor that
/// restarts there can re-issue its previous run's challenges. Hence `ISecureRandom`, whose
/// production implementation reads the operating system's generator directly.
///
/// **A draw that fails is a refusal**, and the result says so rather than handing back a
/// nonce: a handshake that cannot draw one is closed rather than run with a weak one, and
/// there is no fallback to anything else.
///
/// Drawn here, once, rather than at each handshake, because the draw was written inline in
/// `DiscoveryService::IssueChallenge` and the Raft handshake needs the same bytes the same way
/// (#1308): a second draw site is a second place for the size and the source to disagree.
/// @param random Where the bytes come from; a test scripts it to fix the nonce, or to fail.
/// @return The nonce, or why no nonce could be drawn.
[[nodiscard]] inline std::expected<Nonce, SecureRandomError> DrawNonce(ISecureRandom& random)
{
    Nonce nonce {};
    return random.Fill(nonce).transform([&nonce] { return nonce; });
}

} // namespace FastCache
