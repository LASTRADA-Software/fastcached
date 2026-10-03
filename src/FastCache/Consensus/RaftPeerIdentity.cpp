// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace FastCache::Consensus
{

LabelledMessage RaftPeerSignedMessage(RaftPeerSignature purpose, WireFields::FieldList transcript)
{
    return LabelledMessage::Of(RaftPeerSignatureLabels[static_cast<std::size_t>(purpose)].construction, transcript);
}

RaftPeerIdentity::RaftPeerIdentity(NodeId self, IRaftPeerKeys const& keys):
    _self { std::move(self) },
    _keys { keys }
{
}

NodeId const& RaftPeerIdentity::Self() const noexcept
{
    return _self;
}

Ed25519Signature RaftPeerIdentity::Sign(RaftPeerSignature purpose, WireFields::FieldList transcript) const
{
    return _keys.SignAsSelf(RaftPeerSignedMessage(purpose, transcript));
}

SignerVerdict RaftPeerIdentity::Verify(RaftPeerSignature purpose,
                                       NodeId const& signer,
                                       WireFields::FieldList transcript,
                                       Ed25519Signature const& presented) const
{
    auto const message = RaftPeerSignedMessage(purpose, transcript);
    auto const known = _keys.KeysOf(signer);

    // This node's own id is verified under this node's own key, never the roster's: it is the
    // one key this node holds rather than reads, and a peer that passes here holds this node's
    // private key -- a copied state directory, which is what the caller reports it as.
    auto const live = signer == _self ? std::optional { _keys.OwnPublicKey() } : known.live;
    if (live.has_value() && VerifyLabelled(*live, message, presented))
        return SignerVerdict { .check = SignerCheck::Verified, .key = *live };

    // Only a signature that VERIFIES under a revoked key is reported as one: the key is kept
    // whole for exactly this, so `Revoked` is a statement about who signed rather than about
    // which id was claimed -- and it is asked whatever the claim, this node's own id included.
    for (auto const& revoked: known.revoked)
        if (VerifyLabelled(revoked, message, presented))
            return SignerVerdict { .check = SignerCheck::Revoked, .key = revoked };

    return SignerVerdict { .check = live.has_value() ? SignerCheck::Forged : SignerCheck::Unknown, .key = {} };
}

bool RaftPeerIdentity::StillProves(NodeId const& peer, Ed25519PublicKey const& key) const
{
    auto const known = _keys.KeysOf(peer);
    return known.live.has_value() && *known.live == key;
}

} // namespace FastCache::Consensus
