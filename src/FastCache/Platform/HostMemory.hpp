// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace FastCache
{

/// Query the host's total physical memory in bytes.
///
/// Implementation per platform:
///   - Windows: GlobalMemoryStatusEx → ullTotalPhys
///   - Linux:   /proc/meminfo, "MemTotal:" line (kB → bytes)
///   - macOS:   sysctlbyname("hw.memsize", ...)
///
/// Total (not available) memory is reported, because callers want a value
/// that is stable across daemon restarts: "50% of the host" should mean
/// the same thing whether the box is idle or under load.
///
/// @return Total physical RAM in bytes, or 0 if the query failed (in which
///         case the caller should treat percentage-style sizes as
///         unsupported).
[[nodiscard]] std::size_t QueryHostTotalMemoryBytes() noexcept;

} // namespace FastCache
