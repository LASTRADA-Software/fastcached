// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/FleetHistory.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// A wall clock a case can place, so a bucket boundary is a decision rather than a
/// race with the machine this runs on.
class PlacedWallClock final: public IWallClock
{
  public:
    [[nodiscard]] std::chrono::system_clock::time_point Now() const noexcept override
    {
        std::scoped_lock const lock { _mutex };
        return _now;
    }

    /// @param at Where the clock should stand.
    void Set(std::chrono::system_clock::time_point at) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now = at;
    }

    /// @param by How far forward to move it.
    void Advance(std::chrono::seconds by) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now += by;
    }

  private:
    /// Guarded like `ManualClock`, and for the same reason: a placed clock is driven
    /// by the case's own thread while a production component reads it from one of
    /// its own. `FleetSampler`'s constructor starts a thread that samples
    /// immediately, so every case that injects this fake and then moves it races the
    /// sampler -- reported by ThreadSanitizer on master as a write in `Advance`
    /// against a read in `FleetHistory::NowMillis`. Thread-safety is a property of
    /// this seam, not of the one case that happened to be caught.
    mutable std::mutex _mutex;

    /// A round epoch offset, so a bucket start is easy to reason about in a failure
    /// message: 2026-01-01T00:00:00Z is a whole number of hours and of minutes.
    std::chrono::system_clock::time_point _now { std::chrono::seconds { 1'767'225'600 } };
};

/// A history sink that keeps every call, so a test can say who it was filed under.
///
/// Shared rather than written once per test file: the question these ask is always
/// the same one -- did the buckets arrive, and under which machine -- and two copies
/// of the answer drift the moment the interface grows a parameter.
class RecordingHistorySink final: public Distributed::IFleetHistorySink
{
  public:
    /// One call, kept whole.
    struct Call
    {
        std::string endpoint;                          ///< Who it was filed under.
        std::vector<Distributed::FleetBucket> buckets; ///< What arrived.
    };

    std::size_t AcceptHistory(std::string_view endpoint, std::span<Distributed::FleetBucket const> buckets) override
    {
        calls.push_back(Call { .endpoint = std::string { endpoint },
                               .buckets = std::vector<Distributed::FleetBucket> { buckets.begin(), buckets.end() } });
        return buckets.size();
    }

    std::vector<Call> calls; ///< In arrival order.
};

/// One closed bucket, as a node would hand it over.
///
/// A minute bucket holds exactly one sample, because its width IS the sample
/// interval -- which is why `coverage` is one and why a test can state a bucket
/// without driving a clock through a whole window to produce it.
/// @param startMillis Which window it belongs to.
/// @return The bucket, present and covered once.
[[nodiscard]] inline Distributed::FleetBucket ClosedBucket(std::int64_t startMillis)
{
    Distributed::FleetBucket bucket {};
    bucket.startMillis = startMillis;
    bucket.sampleMillis = startMillis;
    bucket.present = true;
    bucket.coverage = 1;
    return bucket;
}

/// The start of the minute-wide bucket an instant belongs to.
///
/// Derived from `FleetSampleInterval` rather than from a literal 60000, so a test
/// asserting a round trip does not quietly assert a bucket width nothing else agrees
/// with any more.
/// @param at The instant.
/// @return Its bucket's start, in milliseconds since the epoch.
[[nodiscard]] inline std::int64_t MinuteBucketStart(std::chrono::system_clock::time_point at)
{
    auto const width = std::chrono::milliseconds { Distributed::FleetSampleInterval }.count();
    auto const millis = std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    return (millis / width) * width;
}

} // namespace FastCache::Testing
