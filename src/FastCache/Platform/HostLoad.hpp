// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace FastCache
{

/// What a machine is doing right now, as a scheduler needs to know it.
///
/// The other half of `HostFacts`, and split from it for the reason that header
/// states: those are facts stable for the life of a process, while these move
/// while it runs. A copy of these captured once would be a number a scheduler kept
/// believing long after it stopped being true, which is worse than not having it.
///
/// Every field is an `optional` rather than a zero, because **absent is not zero**
/// and the two lead to opposite decisions. A machine that could not read its CPU
/// utilization must be scheduled on its other properties; one that read it and got
/// zero is idle and should be given work. Collapsing them would make an
/// unimplementable platform look permanently idle -- or, with the opposite
/// convention, permanently overloaded.
struct HostLoad
{
    /// Host-wide CPU busy over the interval since the previous sample, in parts per
    /// thousand of total capacity. **Includes this process's own compile jobs**: no
    /// platform separates them, so a consumer that needs the external share
    /// subtracts what it knows it is running.
    ///
    /// Permille rather than a percentage because a percentage of a 128-thread
    /// machine quantizes to more than a core, and rather than a float because this
    /// value crosses a wire and has to compare equal on both sides of it.
    std::optional<std::uint32_t> cpuBusyPermille;

    /// Memory a new process could actually get, in bytes.
    ///
    /// "Available", not "free": Linux's `MemFree` excludes the page cache, which the
    /// kernel will hand back on demand, so a healthy machine reports almost none of
    /// it and a consumer reading that would conclude every long-running host is out
    /// of memory.
    std::optional<std::uint64_t> availableMemoryBytes;

    /// Room left where this process writes its working files.
    ///
    /// Reported for a *path* rather than for "the disk" because a worker's scratch
    /// directory and its cache can be on different filesystems and only the one it
    /// writes to matters. Absent when the query failed, which is emphatically not
    /// the same as zero: a machine whose disk cannot be read must be scheduled on
    /// its other properties, where one reporting zero cannot compile at all.
    std::optional<std::uint64_t> freeScratchBytes;
};

/// Two cumulative CPU counters, as every platform reports them.
///
/// Public because the arithmetic over them is public: see `CpuBusyPermille`.
struct CpuTicks
{
    std::uint64_t busy { 0 };  ///< Ticks spent doing anything but idling.
    std::uint64_t total { 0 }; ///< Ticks accounted for at all.
};

/// Utilization over the interval between two readings.
///
/// **Pure, and public for that reason.** The platform half of this file cannot be
/// tested without a real kernel; this half is where every rule that could be *wrong*
/// lives, and lifting it out means each of them is a deterministic assertion rather
/// than something a test tries to provoke by waiting. A test that slept to make a
/// tick elapse would be asserting about the machine it happened to run on, and would
/// be slow and occasionally wrong at the same time.
///
/// The two refusals are the substance:
///
///   - **A counter that went backwards** is what a suspended VM or a re-plugged CPU
///     produces. Unsigned subtraction would turn it into an enormous busy delta and
///     pin a healthy machine at 1000 permille until the counters caught up.
///   - **A zero-length interval is not an idle one.** Two readings inside one tick
///     differ by nothing, and answering 0 permille would tell a scheduler that a
///     saturated machine was free.
///
/// Both answer `nullopt`, which the caller turns into "this machine would not say"
/// rather than into a number — absent is not zero, here as everywhere in this file.
/// @param previous The earlier reading.
/// @param now The later reading.
/// @return Busy parts per thousand, or nullopt when the pair says nothing.
[[nodiscard]] constexpr std::optional<std::uint32_t> CpuBusyPermille(CpuTicks const& previous, CpuTicks const& now) noexcept
{
    if (now.total <= previous.total || now.busy < previous.busy)
        return std::nullopt;

    auto const busy = now.busy - previous.busy;
    auto const total = now.total - previous.total;
    return static_cast<std::uint32_t>((busy * 1000) / total);
}

/// The raw platform readings a sampler turns into a `HostLoad`.
///
/// The seam, and the reason it exists rather than the sampler calling the platform
/// directly: everything interesting about `IHostLoadSampler` is *stateful* — it
/// keeps a baseline, declines to answer on the first call, and declines again when a
/// counter goes backwards — and none of that could be tested against a real kernel
/// without waiting for one to move. A test that slept to provoke a tick would be
/// asserting about the machine it happened to run on, slowly and occasionally
/// wrongly; a scripted source makes every one of those rules an exact assertion.
///
/// It is the project's inject-every-ambient-dependency rule applied to the one place
/// in this file that touches the machine at all. Each reading answers `nullopt` for
/// "this platform would not say", which the sampler passes through unchanged.
class IHostCounterSource
{
  public:
    IHostCounterSource() = default;
    IHostCounterSource(IHostCounterSource const&) = delete;
    IHostCounterSource& operator=(IHostCounterSource const&) = delete;
    IHostCounterSource(IHostCounterSource&&) = delete;
    IHostCounterSource& operator=(IHostCounterSource&&) = delete;
    virtual ~IHostCounterSource() = default;

    /// This machine's cumulative CPU counters.
    /// @return The counters, or nullopt when the platform would not say.
    [[nodiscard]] virtual std::optional<CpuTicks> Cpu() = 0;

    /// Memory a new process could actually obtain.
    /// @return The byte count, or nullopt when the platform would not say.
    [[nodiscard]] virtual std::optional<std::uint64_t> AvailableMemoryBytes() = 0;

    /// Room where this process writes its working files.
    /// @return The byte count, or nullopt when there is nothing to ask or no answer.
    [[nodiscard]] virtual std::optional<std::uint64_t> FreeScratchBytes() = 0;
};

/// A counter source reading the real machine.
///
/// Per platform:
///   - Linux:   /proc/stat aggregate ticks, /proc/meminfo "MemAvailable:"
///   - Windows: GetSystemTimes, GlobalMemoryStatusEx -> ullAvailPhys
///   - macOS:   host_statistics(HOST_CPU_LOAD_INFO), host_statistics64(HOST_VM_INFO64)
///
/// Free space comes from `QueryDiskSpace` on every platform. Anything the platform
/// will not answer comes back absent rather than as a guess. A cgroup-capped
/// container is a known gap and is deliberately not papered over: `/proc/stat`
/// reports the host's CPU inside one, so a consumer that must not be misled by that
/// should weigh the slot count -- which `QueryHostTotalMemoryBytes` already makes
/// cgroup-aware -- rather than this.
/// @param scratchRoot Any path on the filesystem this process writes working files
///        to; need not exist. Empty leaves `FreeScratchBytes` absent, which is the
///        honest answer for a process with no scratch area rather than a zero that
///        would read as "full".
/// @return The source; never null.
[[nodiscard]] std::unique_ptr<IHostCounterSource> MakeSystemCounterSource(std::filesystem::path scratchRoot = {});

/// Samples the machine's live load.
///
/// An interface, and **stateful**, which is what makes it one rather than a free
/// function: every platform reports CPU as monotonically increasing tick counters,
/// so utilization is a *difference between two samples* and something has to hold
/// the previous one. That state is per-sampler, so two consumers get two samplers
/// and neither disturbs the other's interval -- where a free function with a hidden
/// static would have them silently stealing each other's baselines.
///
/// It is also the seam the project's dependency-injection rule asks for: a
/// scheduler weighing load is tested against a scripted sampler, never against the
/// machine the tests happen to run on.
class IHostLoadSampler
{
  public:
    IHostLoadSampler() = default;
    IHostLoadSampler(IHostLoadSampler const&) = delete;
    IHostLoadSampler& operator=(IHostLoadSampler const&) = delete;
    IHostLoadSampler(IHostLoadSampler&&) = delete;
    IHostLoadSampler& operator=(IHostLoadSampler&&) = delete;
    virtual ~IHostLoadSampler() = default;

    /// Read the machine's load since the previous call.
    ///
    /// The **first** call after construction cannot report CPU utilization: there is
    /// no earlier sample to difference against. It says so with `nullopt` rather
    /// than reporting zero, which would tell a scheduler that a machine under full
    /// load was idle for exactly as long as it takes to be believed.
    /// @return What this machine is doing, with anything unavailable left absent.
    [[nodiscard]] virtual HostLoad Sample() = 0;
};

/// A sampler over any counter source.
///
/// Ordinary use is `MakeHostLoadSampler(MakeSystemCounterSource(scratch))`; spelling
/// both halves at the call site rather than hiding the system source behind a
/// default is what keeps the seam visible where somebody is choosing to use it.
/// @param counters Where the readings come from; taken by ownership. Must not be null.
/// @return A sampler; never null.
[[nodiscard]] std::unique_ptr<IHostLoadSampler> MakeHostLoadSampler(std::unique_ptr<IHostCounterSource> counters);

} // namespace FastCache
