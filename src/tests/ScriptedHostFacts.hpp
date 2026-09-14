// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace FastCache::Testing
{

/// A machine a test can describe, standing in for the one it runs on.
///
/// Shared because a node snapshot is assembled in more than one test file, and the scrape and the
/// `NodeMetrics` verb must describe one machine: the system facts move between two reads (the disk
/// fills while the suite builds), so two doors read through them can disagree for a reason neither
/// door has. `NodeConfig_test`'s `FakeHost` stays its own, since what it needs to see is a host
/// that reports no disk at all.
class ScriptedHostFacts final: public IHostFactsSource
{
  public:
    /// @copydoc IHostFactsSource::Facts
    [[nodiscard]] HostFacts const& Facts() const override
    {
        return _facts;
    }

    /// @copydoc IHostFactsSource::LogicalCores
    [[nodiscard]] std::uint32_t LogicalCores() const override
    {
        return 4;
    }

    /// @copydoc IHostFactsSource::TotalMemoryBytes
    [[nodiscard]] std::uint64_t TotalMemoryBytes() const override
    {
        return 8ULL << 30;
    }

    /// @copydoc IHostFactsSource::SpaceOn
    [[nodiscard]] DiskSpace SpaceOn(std::filesystem::path const& /*path*/) const override
    {
        return DiskSpace { .capacityBytes = 1000, .freeBytes = 400 };
    }

  private:
    HostFacts _facts;
};

/// A machine's moving figures, held still: every read answers the same counters.
///
/// Beside `ScriptedHostFacts` for the same reason: a case comparing two readings of one node asks
/// both doors in turn, and a real counter source advances between them.
class FixedHostCounters final: public IHostCounterSource
{
  public:
    /// @copydoc IHostCounterSource::Cpu
    [[nodiscard]] std::optional<CpuTicks> Cpu() override
    {
        return CpuTicks { .busy = 700, .total = 1000 };
    }

    /// @copydoc IHostCounterSource::AvailableMemoryBytes
    [[nodiscard]] std::optional<std::uint64_t> AvailableMemoryBytes() override
    {
        return 5ULL << 30;
    }

    /// @copydoc IHostCounterSource::FreeScratchBytes
    [[nodiscard]] std::optional<std::uint64_t> FreeScratchBytes() override
    {
        return 400;
    }
};

} // namespace FastCache::Testing
