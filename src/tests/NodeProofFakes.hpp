// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/NodeProof.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>

namespace FastCache::Testing
{

/// @file NodeProofFakes.hpp
/// What every node-identity case scripts (#178): who holds which key, and one caller's half of a
/// handshake.
///
/// Shared rather than written per file, because a fake is a shared helper too: every case here
/// asserts that one machine's key is admitted and another's is not, so the FACTS a copy must get
/// right -- which key a machine signs with, what a revocation leaves behind, which end draws which
/// bytes -- are exactly what a wrong copy would get wrong while every case went on passing. Three
/// files drive these: `NodeProofResponder_test.cpp`, `FrameEndpoint_test.cpp` (through a real
/// socket) and `NodeProof_test.cpp`.
///
/// Machines sign with `TestKeyPair(machine)`, the one test key the Raft peer wire and the lease
/// grants use, so a machine is the same machine to every surface a case reaches.

/// The bytes a SERVER's random source draws: its nonce, then its ephemeral secret.
///
/// Two draws of 32, and the script is 64 long so the two differ -- a script one draw long would
/// hand the server the same bytes as its nonce and its secret, which is legal and hides nothing,
/// but a case reading either could not say which it read.
/// @return The script.
[[nodiscard]] inline std::vector<std::byte> ServerHandshakeScript()
{
    return ScriptedSecureRandom::Ascending(2 * NonceBytes, 0x00);
}

/// The bytes a CALLER's random source draws, disjoint from the server's.
///
/// Disjoint because the session keys are derived from BOTH ephemeral keys in a fixed order: two
/// ends drawing identical bytes would agree on a key even under a derivation that swapped the
/// caller's key for the server's, and the case meant to catch that swap would pass.
/// @return The script.
[[nodiscard]] inline std::vector<std::byte> CallerHandshakeScript()
{
    return ScriptedSecureRandom::Ascending(2 * NonceBytes, 0x80);
}

/// Publish a key roster: each of @p live under its own test key, each of @p revoked's test key
/// revoked -- exactly the shape `NodeMembership::PublishCluster` gives the applied state.
/// @param roster The participant to publish into.
/// @param live The machines whose keys the cluster holds live, each under its own id.
/// @param revoked The machines whose keys the cluster has revoked.
inline void PublishKeyRoster(Distributed::KeyRosterMembership& roster,
                             std::vector<std::string> const& live,
                             std::vector<std::string> const& revoked = {})
{
    auto keys = std::map<std::string, Ed25519PublicKey, std::less<>> {};
    for (auto const& machine: live)
        keys.emplace(machine, TestKeyPair(machine).PublicKey());
    auto tombstones = std::vector<Ed25519PublicKey> {};
    for (auto const& machine: revoked)
        tombstones.push_back(TestKeyPair(machine).PublicKey());
    roster.Publish(std::move(keys), std::move(tombstones));
}

/// One caller's half of a handshake: what it sends, and the secret it keeps and a relay never sees.
struct CallerHandshake
{
    CompileCacheWire::NodeChallengeRequest request; ///< Sent as `NodeChallenge`.
    SecureByteBuffer secret;                        ///< This caller's ephemeral secret.
};

/// Open a handshake the way `Node::NodeProofClient` does: a nonce, then an ephemeral key.
/// @param random The caller's random source.
/// @return The opening.
[[nodiscard]] inline CallerHandshake OpenHandshake(ISecureRandom& random)
{
    auto const nonce = DrawNonce(random);
    auto ephemeral = Distributed::DrawNodeEphemeral(random);
    if (!nonce.has_value() || !ephemeral.has_value())
        throw std::logic_error { "a scripted random source must draw a handshake" };
    auto opening = CallerHandshake { .request = {}, .secret = std::move(ephemeral->secret) };
    std::ranges::copy(*nonce, opening.request.nonce.begin());
    std::ranges::copy(ephemeral->publicKey, opening.request.ephemeral.begin());
    return opening;
}

/// A request's payload, without its header.
/// @param frame The framed request.
/// @return The payload.
[[nodiscard]] inline std::vector<std::byte> RequestPayloadOf(std::span<std::byte const> frame)
{
    return std::vector<std::byte> { frame.begin() + CompileCacheWire::RequestHeaderSize, frame.end() };
}

/// A `ProveNode` payload: @p machine proves @p claimedId over one handshake.
/// @param machine Whose test key signs.
/// @param claimedId The id the proof names.
/// @param request The caller's opening.
/// @param reply The server's signed answer.
/// @return The payload, without its header.
[[nodiscard]] inline std::vector<std::byte> ProofPayload(std::string const& machine,
                                                         std::string_view claimedId,
                                                         CompileCacheWire::NodeChallengeRequest const& request,
                                                         CompileCacheWire::NodeChallengeReply const& reply)
{
    return RequestPayloadOf(
        CompileCacheWire::EncodeProveNode(Distributed::MintNodeProof(TestKeyPair(machine), claimedId, request, reply)));
}

} // namespace FastCache::Testing
