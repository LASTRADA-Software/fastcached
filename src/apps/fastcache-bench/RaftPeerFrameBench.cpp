// SPDX-License-Identifier: Apache-2.0
/// What authenticating a Raft peer frame costs
/// ([#1308](https://github.com/LASTRADA-Software/fastcached/issues/1308)).
///
/// Every frame on a proven connection is sealed by the sender and opened by the receiver: one
/// HMAC-SHA256 each, over both session nonces, the frame's position, its header and its
/// payload. A heartbeat is a few dozen bytes and costs next to nothing; the frame worth
/// measuring is the LARGEST the server accepts, `PeerServerOptions::maxFrameBytes`, which is
/// what one AppendEntries carrying a snapshot-sized batch can reach. The number answers whether
/// the tag is a cost anybody would see beside the replication it guards.
///
/// **It measures the shipped seam, not a stand-in**: `FrameSealer` and `FrameOpener` over a
/// `PskRaftPeerCredential`, the objects `RaftPeerTransport` and `RaftPeerServer` hold. The frame
/// size is printed with the figures; the BUILD is printed once for the whole binary by
/// `BuildBannerListener.cpp`, because a digest's throughput at `-O0` and at `-O2` differ by an
/// order of magnitude and a figure without that condition invites exactly the wrong comparison.
///
/// This file used to carry its own `BuildKind` constant reading `NDEBUG`, and it called such a
/// build *optimised* -- which `NDEBUG` does not say, since `-O0 -DNDEBUG` defines it and
/// optimises nothing (#1439).

#include <FastCache/Cluster/PskRaftPeerCredential.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;

namespace
{

/// A frame of @p payloadBytes: a real header, and a payload of arbitrary bytes, since the MAC
/// does not read them as anything.
/// @param payloadBytes How long the payload is.
/// @return The frame's bytes.
[[nodiscard]] std::vector<std::byte> FrameOf(std::size_t payloadBytes)
{
    return std::vector<std::byte>(RaftWire::HeaderSize + payloadBytes, std::byte { 0xA5 });
}

} // namespace

TEST_CASE("bench: sealing and opening a Raft peer frame", "[!benchmark][raftframe]")
{
    Cluster::PskRaftPeerCredential const credential { SecureByteBuffer(32, std::byte { 0x5A }) };
    // The nonces' VALUES cost nothing a MAC can see, so they come from where production draws
    // them rather than from a seed (#1527).
    SystemSecureRandom random;
    SessionNonces const nonces { .acceptor = DrawNonce(random).value(), .dialler = DrawNonce(random).value() };

    for (auto const payloadBytes: { std::size_t { 64 }, PeerServerOptions {}.maxFrameBytes })
    {
        auto const frame = FrameOf(payloadBytes);
        auto const bytes = std::span<std::byte const> { frame };
        std::cerr << "raft frame MAC: " << payloadBytes << "-byte payload\n";

        BENCHMARK(std::format("seal {} bytes", payloadBytes))
        {
            // A fresh sealer per run, so every run seals position 0 -- the same work each time.
            FrameSealer sealer { credential, nonces };
            return sealer.Seal(bytes);
        };

        auto const tag = FrameSealer { credential, nonces }.Seal(bytes);
        BENCHMARK(std::format("open {} bytes", payloadBytes))
        {
            FrameOpener opener { credential, nonces };
            return opener.Open(bytes.first(RaftWire::HeaderSize), bytes.subspan(RaftWire::HeaderSize), tag);
        };

        // Where a seal's time goes, so a figure worth changing says WHICH change: the
        // length-prefixed copy `SignFields` makes of every field, and the digest over it.
        // A seal is roughly one of each plus an allocation; the two together should
        // account for it.
        BENCHMARK(std::format("encode the fields of {} bytes", payloadBytes))
        {
            auto const position = WireFields::ToBigEndian<std::uint64_t>(0);
            return WireFields::Encode({ std::span<std::byte const> { nonces.acceptor },
                                        std::span<std::byte const> { nonces.dialler },
                                        std::span<std::byte const> { position },
                                        bytes.first(RaftWire::HeaderSize),
                                        bytes.subspan(RaftWire::HeaderSize) });
        };
        BENCHMARK(std::format("sha-256 over {} bytes", payloadBytes))
        {
            return Sha256::Hash(bytes);
        };
    }
}
