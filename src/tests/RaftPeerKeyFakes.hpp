// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// @file RaftPeerKeyFakes.hpp
/// The one test roster for the Raft peer wire's identities (#178).
///
/// Shared rather than written per file, for `MembershipFakes.hpp`'s reason: every case that
/// reaches for these asserts that one machine's key proves its id and another's does not, so
/// the FACTS a copy must get right -- which key an id has, what a revocation leaves behind --
/// are exactly what a wrong copy would get wrong while every case went on passing.

/// The key pair a test gives a machine: derived from @p machine, so every case and every node in
/// a case agrees on it without passing it around.
///
/// A MACHINE rather than an id, because the whole subject is the two coming apart: n3 claiming
/// n2's id still signs with n3's key.
/// @param machine Which machine.
/// @return Its key pair.
[[nodiscard]] inline Ed25519KeyPair TestKeyPair(std::string const& machine)
{
    auto const seed = Sha256::Hash(WireFields::AsBytes("fastcache-test-node-key:" + machine));
    auto pair = Ed25519KeyPair::FromSeed(seed);
    if (!pair.has_value())
        throw std::logic_error { "a 32-byte seed must make a key pair" };
    return *std::move(pair);
}

/// What every node of one test believes about everybody's keys: a cluster's roster, shared.
///
/// Thread-safe, because the production roster is: a case that revokes while a reactor reads
/// must not be testing a race of its own.
class SharedRoster
{
  public:
    /// A roster naming each of @p ids under its own test key.
    /// @param ids The members.
    /// @return The roster.
    [[nodiscard]] static std::shared_ptr<SharedRoster> Of(std::vector<Consensus::NodeId> const& ids)
    {
        auto roster = std::make_shared<SharedRoster>();
        for (auto const& id: ids)
            roster->Admit(id, TestKeyPair(id).PublicKey());
        return roster;
    }

    /// Record @p key as @p id's live key, replacing whatever was there.
    /// @param id The member.
    /// @param key Its key.
    void Admit(Consensus::NodeId const& id, Ed25519PublicKey const& key)
    {
        std::scoped_lock const lock { _lock };
        _live[id] = key;
    }

    /// Revoke @p id's live key: it is no longer the id's, and it is remembered as revoked --
    /// what an applied `Cluster::CommandKind::Forget` does to the key its record held, in the
    /// one shape a verifier reads.
    /// @param id The member.
    void Revoke(Consensus::NodeId const& id)
    {
        std::scoped_lock const lock { _lock };
        auto const live = _live.find(id);
        if (live == _live.end())
            throw std::logic_error { "no live key to revoke for " + id };
        _revoked.push_back(live->second);
        _live.erase(live);
    }

    /// @param id A member id.
    /// @return What the roster says about it now: its live key, and EVERY revoked key, as
    ///         `Cluster::RosterKeys` answers -- a fake narrowing the list to the id would pass
    ///         the cases where the two agree and hide the ones where they do not.
    [[nodiscard]] Consensus::PeerKeys KeysOf(Consensus::NodeId const& id) const
    {
        std::scoped_lock const lock { _lock };
        auto keys = Consensus::PeerKeys {};
        if (auto const live = _live.find(id); live != _live.end())
            keys.live = live->second;
        keys.revoked = _revoked;
        return keys;
    }

  private:
    mutable std::mutex _lock;
    std::map<Consensus::NodeId, Ed25519PublicKey> _live;
    std::vector<Ed25519PublicKey> _revoked;
};

/// One machine's key material over a shared roster: `IRaftPeerKeys` as a node's own key and the
/// cluster's view of everybody else.
class RosterPeerKeys final: public Consensus::IRaftPeerKeys
{
  public:
    /// @param own This machine's key pair.
    /// @param roster What it believes about everybody else; kept alive by this.
    RosterPeerKeys(Ed25519KeyPair own, std::shared_ptr<SharedRoster const> roster):
        _own { std::move(own) },
        _roster { std::move(roster) }
    {
    }

    [[nodiscard]] Ed25519PublicKey OwnPublicKey() const override
    {
        return _own.PublicKey();
    }

    [[nodiscard]] Ed25519Signature SignAsSelf(std::span<std::byte const> message) const override
    {
        return _own.Sign(message);
    }

    [[nodiscard]] Consensus::PeerKeys KeysOf(Consensus::NodeId const& peer) const override
    {
        return _roster->KeysOf(peer);
    }

  private:
    Ed25519KeyPair _own;
    std::shared_ptr<SharedRoster const> _roster;
};

/// The production identity over a machine's own keys, owning them: what a case hands a server,
/// a transport or the cluster harness.
///
/// The PRODUCTION `RaftPeerIdentity` inside, never a stand-in, so a case exercises the signing and
/// the verifying it claims to.
class TestPeerIdentity final: public Consensus::IRaftPeerIdentity
{
  public:
    /// @param self The id this machine proves itself as -- or CLAIMS to, when it is not its own.
    /// @param own The key it signs with.
    /// @param roster What it believes about everybody else.
    TestPeerIdentity(Consensus::NodeId self, Ed25519KeyPair own, std::shared_ptr<SharedRoster const> roster):
        _keys { std::make_unique<RosterPeerKeys>(std::move(own), std::move(roster)) },
        _identity { std::move(self), *_keys }
    {
    }

    /// The honest machine @p self, signing with its own test key.
    /// @param self Its id.
    /// @param roster What it believes about everybody else.
    /// @return The identity.
    [[nodiscard]] static std::unique_ptr<TestPeerIdentity const> Honest(Consensus::NodeId const& self,
                                                                        std::shared_ptr<SharedRoster const> roster)
    {
        return std::make_unique<TestPeerIdentity const>(self, TestKeyPair(self), std::move(roster));
    }

    [[nodiscard]] Consensus::NodeId const& Self() const noexcept override
    {
        return _identity.Self();
    }

    [[nodiscard]] Ed25519Signature Sign(Consensus::RaftPeerSignature purpose,
                                        WireFields::FieldList transcript) const override
    {
        return _identity.Sign(purpose, transcript);
    }

    [[nodiscard]] Consensus::SignerVerdict Verify(Consensus::RaftPeerSignature purpose,
                                                  Consensus::NodeId const& signer,
                                                  WireFields::FieldList transcript,
                                                  Ed25519Signature const& presented) const override
    {
        return _identity.Verify(purpose, signer, transcript, presented);
    }

    [[nodiscard]] bool StillProves(Consensus::NodeId const& peer, Ed25519PublicKey const& key) const override
    {
        return _identity.StillProves(peer, key);
    }

  private:
    std::unique_ptr<RosterPeerKeys const> _keys;
    Consensus::RaftPeerIdentity _identity;
};

} // namespace FastCache::Testing
