// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ThrottledReport.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <system_error>
#include <vector>

#include <core/net/IDatagramSocket.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Cluster
{

/// Where this node's beacons go, asked at every beacon.
///
/// A seam rather than one address, because the default is not one: a node nobody configured
/// beacons on every link it has, and which links those are changes while it runs.
class IBeaconDestinations
{
  public:
    IBeaconDestinations() = default;
    IBeaconDestinations(IBeaconDestinations const&) = delete;
    IBeaconDestinations& operator=(IBeaconDestinations const&) = delete;
    IBeaconDestinations(IBeaconDestinations&&) = delete;
    IBeaconDestinations& operator=(IBeaconDestinations&&) = delete;
    virtual ~IBeaconDestinations() = default;

    /// @return Every address the next beacon goes to; empty when there is nowhere to send one.
    [[nodiscard]] virtual std::vector<core::net::DatagramAddress> Destinations() const = 0;
};

/// One address, exactly as named: an operator's `--discovery`, or a test's segment.
class FixedBeaconDestination final: public IBeaconDestinations
{
  public:
    /// @param address Where every beacon goes.
    explicit FixedBeaconDestination(core::net::DatagramAddress address):
        _address { std::move(address) }
    {
    }

    [[nodiscard]] std::vector<core::net::DatagramAddress> Destinations() const override
    {
        return { _address };
    }

  private:
    core::net::DatagramAddress _address;
};

/// The directed broadcast of every eligible IPv4 interface, on one port: where a node nobody
/// configured beacons.
///
/// **Not the limited broadcast.** `255.255.255.255` leaves by whichever interface the stack infers,
/// and on a Windows host with a VPN-type adapter that was the adapter -- measured: source
/// `172.31.255.2`, not the LAN's `192.168.86.24/24` -- so a default node beaconed where no LAN peer
/// ever heard it. A directed broadcast is ON-LINK: it leaves by its own interface, with that
/// interface's address as its source, which is also what keeps a discovery cookie's host binding
/// true of an honest peer (`ChallengeCookies`): it answers from the address it was challenged at.
///
/// **Re-enumerated on an INTERVAL, never per beacon**: the enumeration costs a syscall, two orders
/// of magnitude dearer on Windows (`QueryLocalAddresses`' measurement), and a beacon is sent every
/// few seconds. Stale in both directions, and both degrade safely:
/// - A link that came UP since the last refresh is not beaconed on until the next: for up to one
///   interval peers there do not hear THIS node, while it still hears them -- receiving is on the
///   wildcard, whatever this answers.
/// - A link that went DOWN, or changed address, is still beaconed at its old broadcast until the
///   next: datagrams to nobody, or to a subnet this node has left. A beacon is an unauthenticated
///   announcement, and whatever answers it is proven, so a stale send costs a datagram.
///
/// **Never a silent no-op**: a refresh that finds no eligible interface says so, naming each
/// interface and why it was passed over, with the remedy -- and says again when one comes back.
///
/// **A walk that FAILED is not a machine with no interfaces.** It keeps the destinations it had --
/// the staleness above, which degrades safely -- and says so naming the error: counted per new
/// error, said at most once per `WalkFailureReportInterval`. Its end is said -- where beacons go,
/// whether or not that changed -- only once no walk has failed for a whole interval, so a walk
/// that fails every other time is one line, not a Warn and an Info per two refreshes.
class DirectedBroadcastDestinations final: public IBeaconDestinations
{
  public:
    /// How often the interfaces are asked again.
    static constexpr std::chrono::seconds DefaultRefreshInterval { 30 };

    /// How often a failed walk is said, at most, and how long no walk may fail before its end is.
    static constexpr std::chrono::seconds WalkFailureReportInterval { 60 };

    /// Enumerate now, and say where beacons will go.
    /// @param source This machine's interfaces; must outlive this.
    /// @param clock When a refresh is due; must outlive this.
    /// @param port The beacon port every destination carries.
    /// @param logger Where a change of destinations, and none at all, are said; must outlive this.
    /// @param refreshInterval How long an enumeration is used for.
    DirectedBroadcastDestinations(IInterfaceAddressSource const& source,
                                  core::platform::IClock& clock,
                                  std::uint16_t port,
                                  ILogger& logger,
                                  std::chrono::milliseconds refreshInterval = DefaultRefreshInterval);

    [[nodiscard]] std::vector<core::net::DatagramAddress> Destinations() const override;

  private:
    /// Enumerate, and say what changed. Called with `_mutex` held.
    void Refresh() const;

    IInterfaceAddressSource const& _source;
    core::platform::IClock& _clock;
    ILogger& _logger;
    std::chrono::milliseconds _refreshInterval;
    std::uint16_t _port;

    mutable std::mutex _mutex;
    mutable std::vector<core::net::DatagramAddress> _destinations;
    mutable core::platform::SteadyTimePoint _sampledAt;
    mutable bool _reported { false }; ///< Whether the destinations have been said once.
    /// The error the last failed walk was counted under, kept until the end is said, so a walk that
    /// fails the same way every other time is ONE error rather than one per failure.
    mutable std::optional<std::error_code> _walkFailure {};
    /// When a walk last failed; empty once its end was said.
    mutable std::optional<core::platform::SteadyTimePoint> _lastWalkFailure {};
    /// New walk errors, counted and said at most once an interval.
    mutable ThrottledReport _walkFailures { WalkFailureReportInterval };
};

} // namespace FastCache::Cluster
