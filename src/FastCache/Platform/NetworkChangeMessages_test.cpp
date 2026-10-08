// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NetworkChangeMessages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using FastCache::NetlinkReportsChange;
using FastCache::RouteSocketReportsChange;

namespace
{

/// The netlink message types the fixtures spell, by value -- the kernel's own, not its headers.
namespace Netlink
{
    constexpr std::uint16_t Done = 3;
    constexpr std::uint16_t NewLink = 16;
    constexpr std::uint16_t NewAddr = 20;
    constexpr std::uint16_t DelAddr = 21;
    constexpr std::uint16_t NewRoute = 24;
    constexpr std::size_t HeaderSize = 16;
    constexpr std::size_t Alignment = 4;
} // namespace Netlink

/// The route-socket message types the fixtures spell, by value.
namespace RouteSocket
{
    constexpr std::uint8_t Add = 1;
    constexpr std::uint8_t Miss = 7;
    constexpr std::uint8_t NewAddr = 0xc;
    constexpr std::uint8_t IfInfo = 0xe;
    constexpr std::uint8_t Version = 5;
    constexpr std::size_t PrefixSize = 4;
} // namespace RouteSocket

/// Append @p value's native-endian bytes to @p out.
/// @param out The buffer being built.
/// @param value The field.
template <typename T>
void AppendNative(std::vector<std::byte>& out, T value)
{
    auto const bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Append one netlink message of @p type carrying @p payload zero bytes, padded to four.
/// @param out The buffer being built.
/// @param type The `nlmsg_type`.
/// @param payload How many payload bytes follow the header.
void AppendNetlink(std::vector<std::byte>& out, std::uint16_t type, std::size_t payload = 8)
{
    auto const length = Netlink::HeaderSize + payload;
    AppendNative(out, static_cast<std::uint32_t>(length));
    AppendNative(out, type);
    AppendNative(out, std::uint16_t { 0 }); // flags
    AppendNative(out, std::uint32_t { 1 }); // seq
    AppendNative(out, std::uint32_t { 0 }); // pid
    auto const padded = (length + Netlink::Alignment - 1) / Netlink::Alignment * Netlink::Alignment;
    out.resize(out.size() + (padded - Netlink::HeaderSize), std::byte { 0 });
}

/// Append one route-socket message of @p type carrying @p payload zero bytes after its prefix.
/// @param out The buffer being built.
/// @param type The `rtm_type`.
/// @param payload How many bytes follow the four-byte prefix.
void AppendRouteMessage(std::vector<std::byte>& out, std::uint8_t type, std::size_t payload = 12)
{
    AppendNative(out, static_cast<std::uint16_t>(RouteSocket::PrefixSize + payload));
    AppendNative(out, RouteSocket::Version);
    AppendNative(out, type);
    out.resize(out.size() + payload, std::byte { 0 });
}

} // namespace

TEST_CASE("A netlink address message reports a change", "[platform][network]")
{
    for (auto const type: { Netlink::NewAddr, Netlink::DelAddr, Netlink::NewLink, Netlink::NewRoute })
    {
        INFO("nlmsg_type " << type);
        std::vector<std::byte> buffer;
        AppendNetlink(buffer, type, 7); // a payload that needs padding
        CHECK(NetlinkReportsChange(buffer));
    }
}

TEST_CASE("A netlink done message alone reports no change", "[platform][network]")
{
    std::vector<std::byte> buffer;
    AppendNetlink(buffer, Netlink::Done, 4);
    CHECK_FALSE(NetlinkReportsChange(buffer));
}

TEST_CASE("A netlink change after a message of another type is still seen", "[platform][network]")
{
    std::vector<std::byte> buffer;
    AppendNetlink(buffer, Netlink::Done, 5);
    AppendNetlink(buffer, Netlink::NewAddr);
    CHECK(NetlinkReportsChange(buffer));
}

TEST_CASE("A truncated netlink buffer reports what it holds and nothing more", "[platform][network]")
{
    std::vector<std::byte> whole;
    AppendNetlink(whole, Netlink::Done);
    auto const doneEnd = whole.size();
    AppendNetlink(whole, Netlink::NewAddr);

    // Cut inside the address message's header, and inside its payload: neither is held whole.
    for (auto const cut: { doneEnd + 6, whole.size() - 1 })
    {
        INFO("cut at " << cut << " of " << whole.size());
        CHECK_FALSE(NetlinkReportsChange(std::span { whole }.first(cut)));
    }
    CHECK_FALSE(NetlinkReportsChange({}));
}

TEST_CASE("A netlink header claiming less than a header ends the walk", "[platform][network]")
{
    std::vector<std::byte> buffer;
    AppendNetlink(buffer, Netlink::Done);
    std::vector<std::byte> zeroLength(Netlink::HeaderSize, std::byte { 0 });
    buffer.insert(buffer.end(), zeroLength.begin(), zeroLength.end());
    AppendNetlink(buffer, Netlink::NewAddr);
    CHECK_FALSE(NetlinkReportsChange(buffer));
}

TEST_CASE("A route-socket address message reports a change", "[platform][network]")
{
    for (auto const type: { RouteSocket::NewAddr, RouteSocket::IfInfo, RouteSocket::Add })
    {
        INFO("rtm_type " << int { type });
        std::vector<std::byte> buffer;
        AppendRouteMessage(buffer, RouteSocket::Miss);
        AppendRouteMessage(buffer, type);
        CHECK(RouteSocketReportsChange(buffer));
    }
}

TEST_CASE("A route-socket miss message reports no change", "[platform][network]")
{
    std::vector<std::byte> buffer;
    AppendRouteMessage(buffer, RouteSocket::Miss);
    CHECK_FALSE(RouteSocketReportsChange(buffer));
}

TEST_CASE("A truncated route-socket buffer reports what it holds and nothing more", "[platform][network]")
{
    std::vector<std::byte> whole;
    AppendRouteMessage(whole, RouteSocket::Miss);
    auto const missEnd = whole.size();
    AppendRouteMessage(whole, RouteSocket::NewAddr);

    for (auto const cut: { missEnd + 3, whole.size() - 1 })
    {
        INFO("cut at " << cut << " of " << whole.size());
        CHECK_FALSE(RouteSocketReportsChange(std::span { whole }.first(cut)));
    }

    std::vector<std::byte> zeroLength(RouteSocket::PrefixSize, std::byte { 0 });
    AppendRouteMessage(zeroLength, RouteSocket::NewAddr);
    CHECK_FALSE(RouteSocketReportsChange(zeroLength));
}
