// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace FastCache::Node
{

/// Reports how fast the toolchain hash is going, while it is going.
///
/// **The instrument
/// [#354](https://github.com/LASTRADA-Software/fastcached/issues/354) has been
/// waiting eight sightings for.** That phase has been observed running past 300 s
/// on a CI runner without finishing, against a recorded cold cost of 5.00 ms per
/// file that would put 5136 files at ~26 s serial and less than that 16-wide. The
/// phase is now known; what is not known is *why*, and three candidates remain with
/// three different fixes: cold page cache, an antivirus in the path of every first
/// read, and per-file open cost dominating.
///
/// A ninth artefact of the same shape separates none of them. A **rate** does:
///
/// | what the rate does | what it points at |
/// | --- | --- |
/// | flat and slow from the first interval | per-file open cost, or a scanner |
/// | fast, then degrading | cache or memory pressure |
///
/// So this reports per-INTERVAL rather than cumulative. A cumulative figure cannot
/// answer a question about *now* -- 5000 files in 300 s and 5000 files in the first
/// 20 s followed by a wedge are the same average and opposite diagnoses -- and it is
/// exactly the reading `AGENT.md` records as having already cost this project a
/// session. The cumulative figure travels too, as evidence, never as the verdict.
///
/// **It reports files, not bytes, and says so.** The sizes are not available here:
/// this counts slice completions, one layer above the code that opens each file, and
/// inventing a byte figure from an average would be a number that looks measured and
/// is not. Where the bytes matter -- distinguishing "the digesting is the cost" from
/// "the opening is" -- the total is knowable separately and the comparison is made
/// against it rather than smuggled in here.
///
/// Thread-safe, and it must be: every slice thread observes into it. Exactly one
/// thread emits each line, chosen by a compare-exchange on the next deadline, so a
/// 16-wide walk produces one line per interval rather than sixteen.
class ToolchainHashProgress
{
  public:
    /// How often a line is emitted while the hash runs.
    ///
    /// Ten seconds, which is a compromise between two failure modes rather than a
    /// round number. Shorter and a normal warm walk -- two seconds for a whole
    /// Windows toolchain -- would emit nothing anyway while a slow one filled a CI
    /// log with tens of lines nobody reads. Longer and the 300 s budget yields too
    /// few samples to tell flat from degrading, which is the whole question.
    static constexpr std::chrono::milliseconds DefaultInterval { 10'000 };

    /// @param total How many files the hash will cover; may be zero.
    /// @param interval How often to emit a line.
    /// @param clock Time source; must outlive this.
    /// @param logger Where the lines go; must outlive this.
    ToolchainHashProgress(std::size_t total,
                          std::chrono::milliseconds interval,
                          IClock const& clock,
                          ILogger& logger) noexcept:
        _total { total },
        _interval { interval },
        _clock { clock },
        _logger { logger },
        _startedAt { clock.Now() },
        _windowAt { clock.Now() },
        _nextAt { clock.Now() + interval }
    {
    }

    ~ToolchainHashProgress() = default;

    ToolchainHashProgress(ToolchainHashProgress const&) = delete;
    ToolchainHashProgress& operator=(ToolchainHashProgress const&) = delete;
    ToolchainHashProgress(ToolchainHashProgress&&) = delete;
    ToolchainHashProgress& operator=(ToolchainHashProgress&&) = delete;

    /// One more file has been hashed.
    ///
    /// Called from every slice thread. Reads the clock each time rather than every
    /// Nth file: a `steady_clock::now()` is tens of nanoseconds against a per-file
    /// open measured in MILLISECONDS on the machine this exists to diagnose, so
    /// sampling it would trade an unmeasurable saving for a rate that skips.
    void Observe()
    {
        auto const done = _done.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto const now = _clock.Now();
        auto deadline = _nextAt.load(std::memory_order_acquire);
        if (now < deadline)
            return;

        // Exactly one thread past this point per interval. The loser of the exchange
        // returns rather than retrying: its line would carry the same numbers.
        if (!_nextAt.compare_exchange_strong(deadline, now + _interval, std::memory_order_acq_rel))
            return;

        auto const windowAt = _windowAt.load(std::memory_order_acquire);
        auto const windowDone = _windowDone.load(std::memory_order_acquire);
        _windowAt.store(now, std::memory_order_release);
        _windowDone.store(done, std::memory_order_release);

        _logger.Logf(LogLevel::Info,
                     "hashing toolchain files: {} of {} done, {} file/s over the last {}s ({} file/s since the start)",
                     done,
                     _total,
                     RatePerSecond(done - windowDone, now - windowAt),
                     std::chrono::duration_cast<std::chrono::seconds>(now - windowAt).count(),
                     RatePerSecond(done, now - _startedAt));
    }

    /// @return How many files have been observed.
    [[nodiscard]] std::size_t Done() const noexcept
    {
        return _done.load(std::memory_order_acquire);
    }

  private:
    /// Files per second over a window, rounded to a whole number.
    ///
    /// Whole numbers because the question is an order of magnitude -- "20 file/s or
    /// 2000" -- and a rate printed to two decimals invites a reader to compare two
    /// runs that were never comparable. Zero elapsed answers 0 rather than dividing:
    /// a `ManualClock` that has not been advanced is an ordinary thing for a test to
    /// present, and a rate over no time is not a large rate.
    /// @param count How many files in the window.
    /// @param elapsed How long the window was.
    /// @return Files per second.
    [[nodiscard]] static std::uint64_t RatePerSecond(std::size_t count, Duration elapsed) noexcept
    {
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (ms <= 0)
            return 0;
        return static_cast<std::uint64_t>((static_cast<std::int64_t>(count) * 1000) / ms);
    }

    std::size_t _total;
    std::chrono::milliseconds _interval;
    IClock const& _clock;
    ILogger& _logger;
    TimePoint _startedAt;

    std::atomic<std::size_t> _done { 0 };
    // The start of the window the next line will report on, and how many files were
    // done then. Written only by the thread that won the exchange below, which is
    // what makes plain atomics enough: the next winner cannot run until the clock has
    // passed a deadline this one already published.
    std::atomic<TimePoint> _windowAt;
    std::atomic<std::size_t> _windowDone { 0 };
    std::atomic<TimePoint> _nextAt;
};

} // namespace FastCache::Node
