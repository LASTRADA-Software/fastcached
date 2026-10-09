// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NetworkChangeMessages.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace FastCache
{

namespace
{
    /// How one OS change socket frames its messages, and which of their types report a change.
    /// @tparam Length The unsigned type of the header's length field.
    /// @tparam Type The unsigned type of the header's type field.
    /// @tparam Counted How many types report a change.
    template <typename Length, typename Type, std::size_t Counted>
    struct MessageFormat
    {
        std::size_t headerSize {};            ///< The smallest a message can be.
        std::size_t lengthAt {};              ///< Byte offset of the length field.
        std::size_t typeAt {};                ///< Byte offset of the type field.
        std::size_t alignment {};             ///< What a message's length is rounded up to.
        std::array<Type, Counted> counted {}; ///< The types that report a change.
    };

    /// Read a native-endian field at @p offset; the caller has checked it is in @p buffer.
    /// @param buffer The message.
    /// @param offset Where the field begins.
    /// @return The field's value.
    template <typename T>
    [[nodiscard]] T ReadNative(std::span<std::byte const> buffer, std::size_t offset) noexcept
    {
        std::array<std::byte, sizeof(T)> raw {};
        std::ranges::copy(buffer.subspan(offset, sizeof(T)), raw.begin());
        return std::bit_cast<T>(raw);
    }

    /// Walk @p buffer message by message, by each one's length field.
    /// @param format How the socket frames its messages.
    /// @param buffer What one read returned.
    /// @return True when a message held whole reports a change.
    template <typename Length, typename Type, std::size_t Counted>
    [[nodiscard]] bool ReportsChange(MessageFormat<Length, Type, Counted> const& format,
                                     std::span<std::byte const> buffer) noexcept
    {
        while (buffer.size() >= format.headerSize)
        {
            auto const length = static_cast<std::size_t>(ReadNative<Length>(buffer, format.lengthAt));
            if (length < format.headerSize || length > buffer.size())
                return false;
            if (std::ranges::contains(format.counted, ReadNative<Type>(buffer, format.typeAt)))
                return true;
            auto const advance = (length + format.alignment - 1) / format.alignment * format.alignment;
            buffer = buffer.subspan(std::min(advance, buffer.size()));
        }
        return false;
    }

    /// `nlmsghdr`: u32 len, u16 type, u16 flags, u32 seq, u32 pid; lengths aligned to four.
    /// Counted: RTM_NEWLINK, RTM_DELLINK, RTM_NEWADDR, RTM_DELADDR, RTM_NEWROUTE, RTM_DELROUTE.
    constexpr auto Netlink = MessageFormat<std::uint32_t, std::uint16_t, 6> {
        .headerSize = 16,
        .lengthAt = 0,
        .typeAt = 4,
        .alignment = 4,
        .counted = { 16, 17, 20, 21, 24, 25 },
    };

    /// The `rt_msghdr` prefix: u16 msglen, u8 version, u8 type; lengths unaligned.
    /// Counted: RTM_ADD, RTM_DELETE, RTM_NEWADDR, RTM_DELADDR, RTM_IFINFO.
    constexpr auto RouteSocket = MessageFormat<std::uint16_t, std::uint8_t, 5> {
        .headerSize = 4,
        .lengthAt = 0,
        .typeAt = 3,
        .alignment = 1,
        .counted = { 0x1, 0x2, 0xc, 0xd, 0xe },
    };
} // namespace

bool NetlinkReportsChange(std::span<std::byte const> buffer) noexcept
{
    return ReportsChange(Netlink, buffer);
}

bool RouteSocketReportsChange(std::span<std::byte const> buffer) noexcept
{
    return ReportsChange(RouteSocket, buffer);
}

} // namespace FastCache
