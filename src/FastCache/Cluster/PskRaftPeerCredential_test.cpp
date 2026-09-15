// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Cluster/PskRaftPeerCredential.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

using namespace FastCache;
using namespace FastCache::Cluster;
using Consensus::RaftPeerMac;

namespace
{
/// A cluster key of thirty-two bytes of @p fill.
[[nodiscard]] SecureByteBuffer Key(unsigned char fill = 0x5A)
{
    return SecureByteBuffer(32, std::byte { fill });
}

/// Two fields a handshake might carry.
[[nodiscard]] std::array<std::span<std::byte const>, 2> Fields()
{
    return { WireFields::AsBytes(std::string_view { "n1" }), WireFields::AsBytes(std::string_view { "n2" }) };
}
} // namespace

TEST_CASE("A Raft peer MAC is the cluster key's construction in that MAC's own domain", "[cluster][signing][raft]")
{
    // Pinned against `SignFields` in the named domain rather than against the
    // credential's own `Sign`, so a mapping that sent two purposes to one domain -- or
    // every purpose to the discovery domain -- cannot pass by agreeing with itself.
    auto const key = Key();
    PskRaftPeerCredential const credential { Key() };
    auto const fields = Fields();

    CHECK(credential.Sign(RaftPeerMac::DiallerProof, fields) == SignFields(key, SigningDomain::RaftPeerDialler, fields));
    CHECK(credential.Sign(RaftPeerMac::AcceptorVerdict, fields) == SignFields(key, SigningDomain::RaftPeerVerdict, fields));
    CHECK(credential.Sign(RaftPeerMac::Frame, fields) == SignFields(key, SigningDomain::RaftPeerFrame, fields));
}

TEST_CASE("A Raft peer tag verifies only for its own purpose and its own key", "[cluster][signing][raft]")
{
    PskRaftPeerCredential const credential { Key() };
    PskRaftPeerCredential const stranger { Key(0x33) };
    auto const fields = Fields();

    auto const purposes = std::array { RaftPeerMac::DiallerProof, RaftPeerMac::AcceptorVerdict, RaftPeerMac::Frame };
    for (auto const signer: purposes)
    {
        auto const tag = credential.Sign(signer, fields);
        for (auto const verifier: purposes)
            CHECK(credential.Verify(verifier, fields, tag) == (signer == verifier));

        // A holder of a different key is refused whatever the purpose, and signs
        // nothing the real key accepts.
        CHECK_FALSE(stranger.Verify(signer, fields, tag));
        CHECK_FALSE(credential.Verify(signer, fields, stranger.Sign(signer, fields)));
    }
}
