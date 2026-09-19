// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>

#include <string>

namespace FastCache
{

/// Which machine a connection PROVED it is (#178): the node id it claimed and the identity key it
/// signed the handshake with.
///
/// **Both, and the KEY is the fact.** The signature verified under this key, so the key is what the
/// connection holds; the id is what it claimed, and a roster says whether that key is the one the
/// cluster admitted under that id, one it REVOKED, or neither. Admission is re-asked of the roster
/// on every verb (`Distributed::IMembershipOracle::ExplainKey`) rather than frozen at the
/// handshake, so a key revoked while the connection is open refuses its very next verb.
///
/// Engaged on a connection only after its proof VERIFIED -- including under a revoked key, which is
/// what lets every later verb on that connection be refused as the forgotten machine's rather than
/// judged by its address. Owned: a gate reads it once per tick of a subscription, inside a
/// coroutine, for as long as the connection lives.
struct ProvenIdentity
{
    std::string id;       ///< The node id the connection claimed, inside the signature.
    Ed25519PublicKey key; ///< The key the signature verified under.

    [[nodiscard]] friend bool operator==(ProvenIdentity const&, ProvenIdentity const&) = default;
};

} // namespace FastCache
