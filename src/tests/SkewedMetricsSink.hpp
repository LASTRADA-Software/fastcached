// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>

#include <cstdint>

namespace FastCache::Testing
{

/// A sink that cannot carry one catalogue row: the in-process shape of #1353's build skew.
///
/// What a skewed build looks like to everything that reads a sink, reproduced in one translation
/// unit: `Carries(x) == false` while the catalogue still carries a row for `x`. That is the seam the
/// scrape, the stream and the node's `counter-table-skew` condition all ask (#1364), and driving it is
/// what lets a case about any of them fail.
///
/// Deliberately NOT a sink that merely reads zero: a zero is what the defect produces and what an
/// honest idle counter produces, so a case asserting on the VALUE could not tell them apart.
///
/// Shared, because a fake is a helper like any other: three suites held their own copy of this.
class SkewedMetricsSink final: public IMetricsSink
{
  public:
    /// @param missing The one counter this sink has no slot for.
    explicit SkewedMetricsSink(Counter missing) noexcept:
        _missing { missing }
    {
    }

    void Increment(Counter counter, std::uint64_t by = 1) noexcept override
    {
        _inner.Increment(counter, by);
    }

    [[nodiscard]] std::uint64_t Read(Counter counter) const noexcept override
    {
        return _inner.Read(counter);
    }

    [[nodiscard]] bool Carries(Counter counter) const noexcept override
    {
        return counter != _missing;
    }

  private:
    AtomicMetricsSink _inner;
    Counter _missing;
};

} // namespace FastCache::Testing
