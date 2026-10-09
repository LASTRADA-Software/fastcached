// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>

namespace FastCache
{

/// Whether a buffer read from a Linux `NETLINK_ROUTE` socket reports an interface, address or
/// route change.
///
/// The buffer is a sequence of `nlmsghdr` (native endian: u32 len, u16 type, u16 flags, u32 seq,
/// u32 pid), each advanced over by its length rounded up to four bytes. `RTM_NEWLINK`,
/// `RTM_DELLINK`, `RTM_NEWADDR`, `RTM_DELADDR`, `RTM_NEWROUTE` and `RTM_DELROUTE` count;
/// `NLMSG_NOOP`, `NLMSG_ERROR`, `NLMSG_DONE` and anything else do not. The walk stops at a header
/// that is truncated, shorter than a header, or longer than what is left, and answers for the
/// messages it saw whole before it. The type values are this project's own table, not the
/// kernel's headers, so the classifier and its fixtures compile on every platform.
/// @param buffer What one read returned.
/// @return True when a whole message in it reports a change.
[[nodiscard]] bool NetlinkReportsChange(std::span<std::byte const> buffer) noexcept;

/// Whether a buffer read from a BSD/macOS `PF_ROUTE` socket reports an interface, address or
/// route change.
///
/// The buffer is a sequence of messages that each begin with the `rt_msghdr` prefix (native
/// endian: u16 msglen, u8 version, u8 type), advanced over by `msglen`. `RTM_ADD`, `RTM_DELETE`
/// (a route, the default route's included), `RTM_NEWADDR`, `RTM_DELADDR` and `RTM_IFINFO` count;
/// `RTM_MISS` and anything else do not. The walk stops as `NetlinkReportsChange`'s does.
/// @param buffer What one read returned.
/// @return True when a whole message in it reports a change.
[[nodiscard]] bool RouteSocketReportsChange(std::span<std::byte const> buffer) noexcept;

} // namespace FastCache
