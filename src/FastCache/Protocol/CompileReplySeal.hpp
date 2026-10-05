// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <span>

namespace FastCache
{

/// What a worker signs a COMPILE reply with, and what the launcher checks it by (W-4): the
/// construction's label first (`IdentityKeyPurpose::CompileReply`), then the reply's correlation
/// -- the worker's statement of what it compiled -- and the SHA-256 of the object field as sent.
///
/// **One function for both ends**, for `Distributed::Detail::PackClaims`' reason: a signer and a
/// verifier that each spelled the field list would one day spell it differently, and every
/// dispatched compile in the fleet would then be refused as forged. Over the object's DIGEST rather
/// than the object, so the message is small whatever the object is, and over the object as SENT --
/// still enveloped -- so the launcher authenticates the bytes before it expands one of them.
/// @param correlation The reply's correlation field.
/// @param object The reply's object field, enveloped.
/// @return The message.
[[nodiscard]] inline LabelledMessage CompileReplyMessage(std::span<std::byte const> correlation,
                                                         std::span<std::byte const> object)
{
    auto const digest = Sha256::Hash(object);
    return LabelledMessage::Of(IdentityKeyPurpose::CompileReply, { correlation, std::span<std::byte const> { digest } });
}

/// Sign a COMPILE reply under the worker's identity key.
/// @param key The worker's identity key: the one it proved itself with at registration.
/// @param correlation The reply's correlation field.
/// @param object The reply's object field, enveloped.
/// @return The signature, for `CompileCacheWire::CompileResult::signature`.
[[nodiscard]] inline Ed25519Signature SealCompileReply(Ed25519KeyPair const& key,
                                                       std::span<std::byte const> correlation,
                                                       std::span<std::byte const> object)
{
    return SignLabelled(key, CompileReplyMessage(correlation, object));
}

/// Whether a COMPILE reply is the one the worker holding @p workerKey sent.
///
/// False -- never a guess -- for a key or a signature of any width but the right one: an empty
/// key is a grant that named none, an empty signature a worker that signed nothing, and either
/// leaves the reply unauthenticated, which the launcher answers by compiling locally.
/// @param workerKey The key the grant named.
/// @param correlation The reply's correlation field.
/// @param object The reply's object field, enveloped.
/// @param signature The reply's signature field.
/// @return True when the signature verifies under @p workerKey.
[[nodiscard]] inline bool CompileReplyIsSealedBy(std::span<std::byte const> workerKey,
                                                 std::span<std::byte const> correlation,
                                                 std::span<std::byte const> object,
                                                 std::span<std::byte const> signature)
{
    if (workerKey.size() != Ed25519PublicKeyBytes || signature.size() != Ed25519SignatureBytes)
        return false;
    Ed25519PublicKey key {};
    std::ranges::copy(workerKey, key.begin());
    Ed25519Signature presented {};
    std::ranges::copy(signature, presented.begin());
    return VerifyLabelled(key, CompileReplyMessage(correlation, object), presented);
}

static_assert(CompileCacheWire::IdentityPublicKeyBytes == Ed25519PublicKeyBytes,
              "a grant's worker key is an Ed25519 public key, and the wire header spells its width alone");
static_assert(CompileCacheWire::NodeSignatureBytes == Ed25519SignatureBytes,
              "a compile reply's signature is an Ed25519 signature, and the wire header spells its width alone");

} // namespace FastCache
