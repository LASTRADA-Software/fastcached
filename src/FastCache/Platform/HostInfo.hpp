// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace FastCache
{

/// What a machine is, as a scheduler needs to know it.
///
/// The facts issue #81 enumerates for a compile node, minus the ones this tree
/// already answers: `OnlineCpuCount()` gives the cores and
/// `QueryHostTotalMemoryBytes()` the memory, both with the container-awareness
/// their own headers document, and neither is restated here.
///
/// ## Why these are *facts* and utilization is not here
///
/// Everything below is stable for the life of a process: an operator does not
/// hot-swap a CPU architecture. Live CPU and memory *utilization* are the other
/// half of what #81 lists, and they are deliberately absent until PR 8 — the
/// resource-aware scheduling policy — needs them. A sampler with no consumer is a
/// component whose behaviour nothing checks, which is the mistake
/// `PathCanon::CanonError` already records under a different name.
struct HostFacts
{
    /// Operating system name, e.g. "Linux", "Windows", "macOS".
    std::string osName;

    /// Operating system version as the platform reports it, e.g. "6.8.0-51-generic",
    /// "10.0.26100", "14.5". Free-form on purpose: it is shown to an operator and
    /// compared for equality, never parsed.
    std::string osVersion;

    /// CPU architecture, e.g. "x86_64", "aarch64".
    ///
    /// Taken from the **compiler**, not from the OS. A scheduler matches a worker
    /// to a job by what it can *run*, and an x86-64 process on an arm64 host under
    /// emulation runs x86-64 code however the kernel describes the machine —
    /// reporting the host's answer there would advertise a capability this binary
    /// does not have. Rosetta and WOW64 make that a live case, not a hypothetical.
    std::string architecture;
};

/// The facts about the machine this process is running on.
///
/// Memoised: nothing here changes while the process lives, and two of the three
/// cost a syscall.
/// @return The facts, with empty strings for anything the platform would not say.
[[nodiscard]] HostFacts const& QueryHostFacts();

/// How much room a filesystem has, in bytes.
///
/// Reported for a *path* rather than for "the disk", because a worker's scratch
/// directory and its cache can be on different filesystems and only the one it
/// writes to matters. Uses `std::filesystem::space`, so there is no platform
/// branch to get wrong.
struct DiskSpace
{
    std::uintmax_t capacityBytes { 0 }; ///< Total size of the filesystem.
    std::uintmax_t freeBytes { 0 };     ///< Bytes available to an unprivileged process.
};

/// Query the space on the filesystem holding `path`.
///
/// Zeroes on failure rather than an error, and that is the right shape here: this
/// feeds a scheduling weight and a metric, and a node that cannot report its disk
/// should be scheduled on its other properties rather than refuse to start. A
/// caller that needs to distinguish "no space" from "no answer" has both fields
/// at zero, which no real filesystem reports.
/// @param path Any path on the filesystem of interest; need not exist.
/// @return Its capacity and free space, or zeroes.
[[nodiscard]] DiskSpace QueryDiskSpace(std::filesystem::path const& path) noexcept;

/// What a machine is, behind a seam.
///
/// The free functions above are the implementation; this is what code that has to
/// be *tested* asks. The project's rule is that anything touching the environment
/// arrives through an interface, and "how many cores has this machine" is exactly
/// that: a derivation from it — a node's slot count, say — is otherwise assertable
/// only against whatever machine the suite happens to run on, which means it is not
/// really assertable at all.
///
/// It carries the static facts only. Live utilization is `IHostCounterSource` in
/// `Platform/HostLoad.hpp`, split for the reason `HostFacts` records: these do not
/// change while a process runs and those do, and one interface answering both would
/// invite a caller to cache what it must not.
///
/// A startup path reading one of these as a *default* — the daemon's `--threads`,
/// a percentage memory budget — may still call the free function directly. Nobody
/// needs to fake a machine to check that a default was applied, and a seam nothing
/// substitutes for is a seam nobody has checked.
class IHostFactsSource
{
  public:
    IHostFactsSource() = default;
    IHostFactsSource(IHostFactsSource const&) = delete;
    IHostFactsSource& operator=(IHostFactsSource const&) = delete;
    IHostFactsSource(IHostFactsSource&&) = delete;
    IHostFactsSource& operator=(IHostFactsSource&&) = delete;
    virtual ~IHostFactsSource() = default;

    /// What this machine is: OS, version, architecture.
    [[nodiscard]] virtual HostFacts const& Facts() const = 0;

    /// Hardware threads available to this process.
    ///
    /// Never zero: a machine that would not say is reported as one core, because a
    /// zero would have every caller writing the same clamp and one of them
    /// forgetting it.
    [[nodiscard]] virtual std::uint32_t LogicalCores() const = 0;

    /// Physical memory, in bytes, or 0 when the platform would not say.
    [[nodiscard]] virtual std::uint64_t TotalMemoryBytes() const = 0;

    /// Space on the filesystem holding `path`; zeroes when it could not be read.
    /// @param path Any path on the filesystem of interest; need not exist.
    /// @return Its capacity and free space.
    [[nodiscard]] virtual DiskSpace SpaceOn(std::filesystem::path const& path) const = 0;
};

/// A facts source reading the real machine, through the functions above.
/// @return The source; never null.
[[nodiscard]] std::unique_ptr<IHostFactsSource> MakeSystemHostFacts();

} // namespace FastCache
