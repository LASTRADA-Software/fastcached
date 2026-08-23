// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostLoad.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{
/// A counter source that answers from a script.
///
/// The reason `IHostCounterSource` exists. Everything worth asserting about a
/// sampler is what it does *between* two readings — it keeps a baseline, declines on
/// the first call, declines again across a discontinuity — and against a real kernel
/// none of that is reachable without waiting for counters to move. A test that slept
/// to provoke a tick would assert about the machine it happened to run on, slowly
/// and occasionally wrongly. Here every rule is an exact assertion and the suite
/// never waits for anything.
class ScriptedCounters final: public IHostCounterSource
{
  public:
    /// @param cpu Readings to return in order; the last repeats once exhausted.
    explicit ScriptedCounters(std::vector<std::optional<CpuTicks>> cpu):
        _cpu { std::move(cpu) }
    {
    }

    [[nodiscard]] std::optional<CpuTicks> Cpu() override
    {
        if (_cpu.empty())
            return std::nullopt;
        auto const at = _next < _cpu.size() ? _next : _cpu.size() - 1;
        ++_next;
        return _cpu[at];
    }

    [[nodiscard]] std::optional<std::uint64_t> AvailableMemoryBytes() override
    {
        return _memory;
    }

    [[nodiscard]] std::optional<std::uint64_t> FreeScratchBytes() override
    {
        return _scratch;
    }

    /// Set what the next reads report.
    /// @param memory Available memory to report, or absent.
    /// @param scratch Free scratch space to report, or absent.
    void Report(std::optional<std::uint64_t> memory, std::optional<std::uint64_t> scratch) noexcept
    {
        _memory = memory;
        _scratch = scratch;
    }

  private:
    std::optional<std::uint64_t> _memory;
    std::optional<std::uint64_t> _scratch;
    std::vector<std::optional<CpuTicks>> _cpu;
    std::size_t _next { 0 };
};

/// A sampler over a script, with the script still reachable.
struct Scripted
{
    /// @param cpu Readings to return in order.
    explicit Scripted(std::vector<std::optional<CpuTicks>> cpu)
    {
        auto owned = std::make_unique<ScriptedCounters>(std::move(cpu));
        counters = owned.get();
        sampler = MakeHostLoadSampler(std::move(owned));
    }

    ScriptedCounters* counters { nullptr }; ///< Borrowed; owned by `sampler`.
    std::unique_ptr<IHostLoadSampler> sampler;
};
} // namespace

TEST_CASE("Two readings become a utilization, and the arithmetic is pure", "[platform][hostload]")
{
    // Half of one interval busy is 500 permille, whatever the counters started at:
    // the ratio is over the DELTA, which is why the two counters are kept together
    // rather than pre-divided. Dividing each reading first and subtracting after
    // would be subtracting two lifetime averages.
    CHECK(CpuBusyPermille(CpuTicks { .busy = 1000, .total = 4000 }, CpuTicks { .busy = 1500, .total = 5000 }) == 500U);
    CHECK(CpuBusyPermille(CpuTicks { .busy = 0, .total = 0 }, CpuTicks { .busy = 100, .total = 100 }) == 1000U);
    CHECK(CpuBusyPermille(CpuTicks { .busy = 7, .total = 9 }, CpuTicks { .busy = 7, .total = 109 }) == 0U);
}

TEST_CASE("A counter that went backwards is refused, not wrapped", "[platform][hostload]")
{
    // What a suspended VM or a re-plugged CPU produces. Unsigned subtraction would
    // turn it into an enormous busy delta and pin a healthy machine at 1000 permille
    // until the counters caught up -- taking it out of the fleet for a fault that
    // never happened.
    CHECK_FALSE(
        CpuBusyPermille(CpuTicks { .busy = 500, .total = 2000 }, CpuTicks { .busy = 500, .total = 1000 }).has_value());
    CHECK_FALSE(
        CpuBusyPermille(CpuTicks { .busy = 500, .total = 1000 }, CpuTicks { .busy = 400, .total = 2000 }).has_value());
}

TEST_CASE("A zero-length interval is not an idle one", "[platform][hostload]")
{
    // Two readings inside one tick differ by nothing. Answering 0 permille would tell
    // a scheduler that a saturated machine was free -- and it is the answer a naive
    // guard against division by zero would give.
    CHECK_FALSE(CpuBusyPermille(CpuTicks { .busy = 5, .total = 10 }, CpuTicks { .busy = 5, .total = 10 }).has_value());
}

TEST_CASE("The first sample cannot report CPU, and says so", "[platform][hostload]")
{
    // Not zero. There is nothing to difference against, and answering zero would tell
    // a scheduler that a saturated machine was idle for exactly as long as it takes
    // to be believed -- which on a 20-second heartbeat is a whole interval of piling
    // work onto a machine that has none to spare.
    Scripted scripted { { CpuTicks { .busy = 100, .total = 1000 }, CpuTicks { .busy = 600, .total = 2000 } } };

    CHECK_FALSE(scripted.sampler->Sample().cpuBusyPermille.has_value());

    // And the second call has a baseline, so it answers: 500 of 1000 new ticks busy.
    auto const second = scripted.sampler->Sample();
    REQUIRE(second.cpuBusyPermille.has_value());
    CHECK(Unwrap(second.cpuBusyPermille) == 500U);
}

TEST_CASE("A refused reading still becomes the next baseline", "[platform][hostload]")
{
    // The subtle half of the state machine. After a discontinuity the sampler must
    // difference against the NEW counters, not against the ones from before it:
    // keeping the older baseline would difference across the very jump that was
    // refused, and report one impossible figure on the next call instead of none.
    Scripted scripted { {
        CpuTicks { .busy = 1000, .total = 5000 }, // baseline
        CpuTicks { .busy = 10, .total = 50 },     // counters reset; refused
        CpuTicks { .busy = 260, .total = 1050 },  // 250 of 1000 against the reset
    } };

    CHECK_FALSE(scripted.sampler->Sample().cpuBusyPermille.has_value());
    CHECK_FALSE(scripted.sampler->Sample().cpuBusyPermille.has_value());

    auto const third = scripted.sampler->Sample();
    REQUIRE(third.cpuBusyPermille.has_value());
    CHECK(Unwrap(third.cpuBusyPermille) == 250U);
}

TEST_CASE("A platform that reports no CPU at all never reports one", "[platform][hostload]")
{
    // Absent rather than zero, on every call. A node whose platform will not answer
    // must be scheduled on its other properties; reading it as idle would pile work
    // onto whichever machines happen to be least introspectable.
    Scripted scripted { { std::nullopt, std::nullopt } };

    CHECK_FALSE(scripted.sampler->Sample().cpuBusyPermille.has_value());
    CHECK_FALSE(scripted.sampler->Sample().cpuBusyPermille.has_value());
}

TEST_CASE("Memory and scratch pass through untouched, absence included", "[platform][hostload]")
{
    // They are point readings rather than differences, so the sampler owes them
    // nothing but honesty -- including reporting "did not say" as absent rather than
    // as a zero a scheduler would read as "this machine is full".
    Scripted scripted { { CpuTicks { .busy = 0, .total = 1 } } };

    auto const silent = scripted.sampler->Sample();
    CHECK_FALSE(silent.availableMemoryBytes.has_value());
    CHECK_FALSE(silent.freeScratchBytes.has_value());

    scripted.counters->Report(8ULL << 30, 40ULL << 30);

    auto const reported = scripted.sampler->Sample();
    REQUIRE(reported.availableMemoryBytes.has_value());
    CHECK(Unwrap(reported.availableMemoryBytes) == (8ULL << 30));
    REQUIRE(reported.freeScratchBytes.has_value());
    CHECK(Unwrap(reported.freeScratchBytes) == (40ULL << 30));
}

TEST_CASE("The real counter source answers this machine", "[platform][hostload][smoke]")
{
    // A smoke test, and the reason it exists at all: an interface with only a fake
    // behind it is an interface nobody has checked -- the lesson `OpenUdpSocket` is
    // recorded for, where the real implementation returned null on Windows and no
    // fake would ever have shown it.
    //
    // What it asserts is that the platform answered and that the answer is shaped
    // like an answer, never a number: what this machine is doing while the suite runs
    // is not a fact a test may claim to know, and asserting one would make the suite
    // report on its host rather than on the code.
    auto const source = MakeSystemCounterSource(std::filesystem::current_path());
    REQUIRE(source != nullptr);

    if (auto const cpu = source->Cpu(); cpu.has_value())
        CHECK(Unwrap(cpu).total >= Unwrap(cpu).busy);

    if (auto const memory = source->AvailableMemoryBytes(); memory.has_value())
    {
        // Zero would mean this process could allocate nothing, which is false
        // wherever this test can run at all -- so a zero here is an implementation
        // that read the wrong field. The ceiling catches the mirror-image slip of
        // reporting TOTAL memory as available, which no running system can claim.
        CHECK(Unwrap(memory) > 0);
        CHECK(Unwrap(memory) < (1ULL << 50));
    }

    // The current directory exists and has some room, or this test could not have
    // been built.
    auto const scratch = source->FreeScratchBytes();
    REQUIRE(scratch.has_value());
    CHECK(Unwrap(scratch) > 0);
}

TEST_CASE("A source with no scratch path reports no scratch space", "[platform][hostload][smoke]")
{
    // Absent rather than zero, and the distinction is the whole reason the field is
    // an optional: a zero would tell a scheduler this machine has a full disk and
    // must never be sent work, when the truth is that nobody asked it about a disk.
    auto const source = MakeSystemCounterSource();
    CHECK_FALSE(source->FreeScratchBytes().has_value());
}
