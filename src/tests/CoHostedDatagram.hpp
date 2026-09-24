// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <core/net/IDatagramSocket.hpp>
#include <core/net/SharedPortDatagram.hpp>
#include <core/net/testing/InMemoryDatagram.hpp>

namespace FastCache::Testing
{

/// The port every node in a test segment listens for beacons on.
///
/// Shared rather than declared per suite, because it is half of the distinction
/// every co-hosted case turns on -- "the port everybody bound" against "the
/// address only I hold" -- and two suites that drifted to different numbers would
/// each still pass while describing different segments.
inline constexpr std::uint16_t TestBeaconPort = 6681;

/// A socket for a node sharing a host, and therefore a beacon port, with others.
///
/// Two halves, which is the point: the shared one hears the broadcast every node
/// on the segment listens for, and the private one is what this node sends from
/// -- so the address a peer replies to names this node rather than the machine it
/// happens to share. It is what `DiscoveryTier::Start` builds over real sockets;
/// see `Net/SharedPortDatagram`.
/// @param bus The segment.
/// @param host The machine; co-hosted nodes pass the same one.
/// @param ownPort The port only this node holds.
/// @return The pair, as one socket; null when the segment would not open either half, which the
///         case's own use of it then reports.
[[nodiscard]] inline std::unique_ptr<core::net::IDatagramSocket> CoHostedDatagramSocket(core::net::testing::DatagramBus& bus,
                                                                                        std::string_view host,
                                                                                        std::uint16_t ownPort)
{
    return core::net::answerFromOwnAddress(
               bus.open(core::net::DatagramAddress { .host = std::string { host }, .port = TestBeaconPort }),
               bus.open(core::net::DatagramAddress { .host = std::string { host }, .port = ownPort }))
        .value_or(nullptr);
}

} // namespace FastCache::Testing
