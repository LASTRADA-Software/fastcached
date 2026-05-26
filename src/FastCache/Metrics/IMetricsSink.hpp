// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Counter-style metrics sink. The cache engine, server, and protocol
/// handlers increment counters via this interface. Snapshot() is read by
/// the `stats` family of commands.
///
/// Designed thin on purpose: only counters today; histograms and gauges
/// come later if/when needed. Implementations must be thread-safe.
class IMetricsSink
{
  public:
    IMetricsSink() = default;
    IMetricsSink(IMetricsSink const&) = delete;
    IMetricsSink(IMetricsSink&&) = delete;
    IMetricsSink& operator=(IMetricsSink const&) = delete;
    IMetricsSink& operator=(IMetricsSink&&) = delete;
    virtual ~IMetricsSink() = default;

    enum class Counter : unsigned
    {
        CmdGet = 0,
        CmdSet,
        CmdDelete,
        GetHits,
        GetMisses,
        Evictions,
        ConnectionsTotal,
        ConnectionsAdmissionRejected,
        BytesIn,
        BytesOut,
        Last,
    };

    /// Increment the named counter by 1 (or `by`).
    virtual void Increment(Counter counter, std::uint64_t by = 1) noexcept = 0;

    /// Read the current value of a counter.
    [[nodiscard]] virtual std::uint64_t Read(Counter counter) const noexcept = 0;
};

/// Default atomic-counter sink.
class AtomicMetricsSink final: public IMetricsSink
{
  public:
    void Increment(Counter counter, std::uint64_t by = 1) noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return;
        _counters[idx].fetch_add(by, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Read(Counter counter) const noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return 0;
        return _counters[idx].load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> _counters[static_cast<std::size_t>(Counter::Last)] {};
};

/// Convert a counter to its canonical stats-line name (used by memcached
/// `stats` and Redis `INFO`).
/// @param counter Counter id.
/// @return Lowercase canonical name (never empty).
[[nodiscard]] constexpr std::string_view ToStringView(IMetricsSink::Counter counter) noexcept
{
    switch (counter)
    {
        case IMetricsSink::Counter::CmdGet:                       return "cmd_get";
        case IMetricsSink::Counter::CmdSet:                       return "cmd_set";
        case IMetricsSink::Counter::CmdDelete:                    return "cmd_delete";
        case IMetricsSink::Counter::GetHits:                      return "get_hits";
        case IMetricsSink::Counter::GetMisses:                    return "get_misses";
        case IMetricsSink::Counter::Evictions:                    return "evictions";
        case IMetricsSink::Counter::ConnectionsTotal:             return "connections_total";
        case IMetricsSink::Counter::ConnectionsAdmissionRejected: return "connections_rejected";
        case IMetricsSink::Counter::BytesIn:                      return "bytes_in";
        case IMetricsSink::Counter::BytesOut:                     return "bytes_out";
        case IMetricsSink::Counter::Last:                         return "<last>";
    }
    return "<unknown>";
}

} // namespace FastCache
