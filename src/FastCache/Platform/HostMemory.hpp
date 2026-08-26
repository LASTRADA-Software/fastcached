// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace FastCache
{

/// Query the memory budget this process should size itself against, in bytes.
///
/// Implementation per platform:
///   - Windows: GlobalMemoryStatusEx → ullTotalPhys
///   - Linux:   the smaller of /proc/meminfo "MemTotal:" and the cgroup limit
///              (v2 memory.max, else v1 memory.limit_in_bytes)
///   - macOS:   sysctlbyname("hw.memsize", ...)
///
/// The cgroup limit is consulted because a container's /proc/meminfo reports the
/// HOST's RAM: sizing against that in a memory-capped container asks for a
/// budget the process is never allowed to reach, and the OOM killer arrives
/// first. Whichever ceiling binds first is the honest answer.
///
/// Total (not available) memory is reported, because callers want a value
/// that is stable across daemon restarts: "50% of the host" should mean
/// the same thing whether the box is idle or under load.
///
/// @return The budget in bytes, or 0 if the query failed (in which case the
///         caller should treat percentage-style sizes as unsupported).
[[nodiscard]] std::size_t QueryHostTotalMemoryBytes() noexcept;

/// The default in-memory cache budget: a quarter of QueryHostTotalMemoryBytes(),
/// rounded down to a whole MiB and clamped to [512 MiB, 8 GiB]. Falls back to the
/// floor when the query fails.
///
/// A fraction rather than a constant so the default tracks the machine it runs
/// on: the same build serves an 8 GB laptop and a 96 GB workstation, and a
/// compile cache sized for the former is close to useless on the latter. The
/// ceiling keeps a large build server from defaulting to a resident set nobody
/// asked for -- past a point, more cache buys less than the RAM is worth
/// elsewhere, and an operator who wants it can say so explicitly.
///
/// Memoised, so it is cheap to use as a default member initialiser.
///
/// @return The default budget in bytes; never 0.
[[nodiscard]] std::size_t DefaultMaxMemoryBytes() noexcept;

} // namespace FastCache
