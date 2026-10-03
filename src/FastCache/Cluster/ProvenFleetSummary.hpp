// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ChallengeIssuer.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <optional>
#include <span>
#include <utility>

/// @file ProvenFleetSummary.hpp
/// A fleet summary whose signature verified: the only kind a formation decision may read.
///
/// A beacon carries a `FleetSummary` too, and nothing about the struct says whether anybody
/// checked it. So what a yield is decided on is a type of its own, and the one way to hold one is
/// to verify a signature: a raw beacon cannot even be passed where a proven summary is asked for.
namespace FastCache::Cluster
{

/// A fleet summary, and the identity key that signed it over a nonce this node chose.
///
/// **What this proves is POSSESSION, never truth.** Its holder knows that the holder of `Key()`
/// said `Summary()` in answer to this node's own question -- not that the fleet it describes
/// exists, is established, or is as old as it claims. A fresh key costs nothing, so a summary from
/// a key nobody knows is proven in exactly this sense and in no other; which keys a node should
/// believe is a separate question, asked of whatever pins them.
///
/// **Constructed only by `ChallengeCookies::Verify` and `VerifyAnswer`**, which check the signature
/// themselves: there is no public constructor, so a caller cannot state that a summary verified,
/// only have it verified. A discovery proof is verified by the cookie jar that issued its challenge
/// -- the one party that can say the nonce is its own and unspent -- and a signed `FleetSummary`
/// answer from a seed here.
class ProvenFleetSummary
{
  public:
    /// Verify a `FleetSummary` answer against the nonce this node sent with the question, spending
    /// that nonce, and hold what it says only when its signature holds.
    ///
    /// For a seed asked over the `0xFC` port rather than a peer challenged on the segment. The nonce
    /// is what makes a recorded answer worthless to a replayer, so it is an `IssuedNonce` taken BY
    /// VALUE: only `ChallengeIssuer` makes one, no recorded bytes convert to one, and this call
    /// consumes it whatever it decides. One question per nonce is the whole of a probe's state, so
    /// a type carries it here where discovery's cookies carry it in a spent record.
    /// @param issued The nonce this node drew and sent with the question; a spent one verifies
    ///        nothing.
    /// @param reply What came back.
    /// @return The proven summary, or nullopt when the nonce was spent or the signature does not
    ///         verify over it.
    [[nodiscard]] static std::optional<ProvenFleetSummary> VerifyAnswer(IssuedNonce issued,
                                                                        CompileCacheWire::FleetSummaryReply const& reply)
    {
        auto const& nonce = issued.Held();
        if (!nonce.has_value() || !VerifyFleetSummarySignature(*nonce, reply))
            return std::nullopt;
        return ProvenFleetSummary { reply.summary, reply.publicKey };
    }

    /// What the signer said.
    /// @return The summary, every field of it covered by the signature.
    [[nodiscard]] constexpr CompileCacheWire::FleetSummary const& Summary() const noexcept
    {
        return _summary;
    }

    /// Who signed it.
    /// @return The identity key the signature verified under.
    [[nodiscard]] constexpr Ed25519PublicKey const& Key() const noexcept
    {
        return _key;
    }

    /// Field-wise equality: the same summary under the same key. Comparing two proven summaries
    /// asserts nothing about how either was verified, only that both were.
    [[nodiscard]] friend bool operator==(ProvenFleetSummary const&, ProvenFleetSummary const&) = default;

  private:
    friend class ChallengeCookies;

    /// Hold what a discovery proof says, when its signature holds over @p asked.
    ///
    /// Private, and reached only through `ChallengeCookies::Verify`: a `DiscoveryWire::Challenge` is
    /// plain bytes anybody can build, so what makes @p asked one this node issued and has not spent
    /// is the cookie jar's check, which runs first.
    /// @param asked The challenge the proof answers, as this node issued it.
    /// @param proof What came back.
    /// @return The proven summary, or nullopt when the signature does not verify.
    [[nodiscard]] static std::optional<ProvenFleetSummary> FromProof(DiscoveryWire::Challenge const& asked,
                                                                     DiscoveryWire::Proof const& proof)
    {
        // Before anything else: `VerifyProofSignature` reconstructs the signed transcript
        // through `EncodeFleetSummaryFields`, whose precondition assert refuses to encode an
        // empty cluster id (#1598). A peer's summary is unvalidated wire input, so an empty id
        // is a malformed proof to refuse here rather than a precondition this call may rely on.
        if (proof.summary.clusterId.empty() || !DiscoveryWire::VerifyProofSignature(asked, proof))
            return std::nullopt;
        return ProvenFleetSummary { proof.summary, proof.publicKey };
    }

    /// Hold a summary a signature check has passed.
    /// @param summary What the signer said.
    /// @param key Who signed it.
    ProvenFleetSummary(CompileCacheWire::FleetSummary summary, Ed25519PublicKey const& key):
        _summary { std::move(summary) },
        _key { key }
    {
    }

    CompileCacheWire::FleetSummary _summary;
    Ed25519PublicKey _key;
};

} // namespace FastCache::Cluster
