// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace FastCache
{

/// Pin the **calling** thread to a single logical CPU core.
///
/// Binding each reactor worker to its own core keeps that worker's hot state
/// (its connections' buffers, its storage shards' cache lines) resident in one
/// core's caches instead of bouncing across cores as the scheduler migrates
/// the thread — which costs cross-core cache-line invalidations on every
/// migration. Combined with SO_REUSEPORT connection pinning, a connection and
/// the thread serving it then stay on one core for their lifetime.
///
/// Per platform:
///   - Linux:   pthread_setaffinity_np over a single-CPU cpu_set_t
///   - macOS:   thread_policy_set with THREAD_AFFINITY_POLICY (a hint; Darwin
///              does not honour hard pinning, so this nudges the scheduler)
///   - Windows: SetThreadAffinityMask with a one-bit mask
///
/// Best-effort: a failure (or an unsupported platform) is reported via the
/// return value and leaves the thread unpinned — the caller logs and carries
/// on. The core index is taken modulo the online CPU count by the caller.
///
/// @param coreIndex Zero-based logical core to bind the calling thread to.
/// @return True if the thread was pinned; false if pinning is unsupported or
///         the OS call failed.
[[nodiscard]] bool PinCallingThreadToCpu(std::size_t coreIndex) noexcept;

/// @return The number of logical CPUs available to the process for pinning,
///         or 0 if it cannot be determined (caller should then skip pinning).
[[nodiscard]] std::size_t OnlineCpuCount() noexcept;

} // namespace FastCache
