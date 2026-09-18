// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Distributed
{

/// A grant signer holding a node's identity key pair (#178).
///
/// What a scheduler signs with in production and in every test: the key pair a node's start
/// resolved out of its state directory, and the member id it runs as. Header-only, because it
/// is a pair of members and two forwarding calls.
class KeyPairLeaseSigner final: public ILeaseSigner
{
  public:
    /// @param signerId The member id grants name as their signer.
    /// @param key This node's identity key pair; its secret half never leaves this object.
    KeyPairLeaseSigner(std::string signerId, Ed25519KeyPair key):
        _signerId { std::move(signerId) },
        _key { std::move(key) }
    {
    }

    [[nodiscard]] std::string_view SignerId() const override
    {
        return _signerId;
    }

    [[nodiscard]] Ed25519PublicKey PublicKey() const override
    {
        return _key.PublicKey();
    }

    [[nodiscard]] Ed25519Signature Sign(std::span<std::byte const> message) const override
    {
        return _key.Sign(message);
    }

  private:
    std::string _signerId;
    Ed25519KeyPair _key;
};

} // namespace FastCache::Distributed
