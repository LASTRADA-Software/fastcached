// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Distributed
{

/// One toolchain clients asked this scheduler for, as `unserved-toolchain` names it.
struct UnservedToolchain
{
    std::string fingerprint; ///< What the clients asked for; the identity every match is made on.
    /// What the most recent client that said anything called it, e.g. `cl 19.44.35207`; empty when
    /// none did. Display only, never compared.
    std::string label;
    std::uint64_t refusals { 0 }; ///< Leases refused `no-worker` for it since it was first recorded.
};

/// The toolchains this scheduler recently refused `no-worker`, for the leader's
/// `unserved-toolchain` condition.
///
/// A refusal reaches ONE client, and a fleet with no worker for a toolchain refuses every client
/// using it one at a time -- each compiling locally, each silently. This is the fleet-wide memory
/// of that, so the leader can name the compiler once.
///
/// **Both staleness directions, named.** Toward ATTENTION: a toolchain stays listed for up to
/// `Window` after the last client asked, even if every client has since moved on -- it fails loud
/// and heals by itself. Toward SILENCE: only through `Capacity` -- a seventeenth distinct unserved
/// toolchain evicts the one asked for longest ago, so a fleet refusing more toolchains than that
/// at once under-reports. An office fleet runs one to three; sixteen is far past a real fleet.
/// A toolchain a worker starts serving is filtered at READ (`SchedulerService::UnservedToolchainsNow`),
/// so the fix clears the row at once rather than after the window.
///
/// Everything recorded here is text a peer sent, so it is recorded only once it has passed the
/// gate `SchedulerService::Lease` refuses at -- this class stores what it is handed.
///
/// The clock is injected, so the window is testable without waiting. Thread-safe: `Lease` runs on a
/// reactor and the condition watch reads on its own thread.
class UnservedToolchains
{
  public:
    /// The most toolchains remembered at once.
    static constexpr std::size_t Capacity = 16;
    /// How long a toolchain stays listed after the last client asked for it.
    static constexpr std::chrono::minutes Window { 15 };

    /// Remember against @p clock.
    /// @param clock Where "now" comes from; must outlive this object.
    explicit UnservedToolchains(core::platform::IClock& clock) noexcept:
        _clock { clock }
    {
    }

    /// Note that a lease for @p fingerprint was refused because no worker serves it.
    /// @param fingerprint The toolchain the client asked for.
    /// @param label What the client called it; empty keeps whatever an earlier client said.
    void Refused(std::string_view fingerprint, std::string_view label);

    /// Every toolchain asked for within `Window`, whether or not something serves it now.
    /// @return Sorted by label, then fingerprint, so two readings compare.
    [[nodiscard]] std::vector<UnservedToolchain> Recent() const;

  private:
    /// One remembered toolchain, and when a client last asked for it.
    struct Entry
    {
        UnservedToolchain toolchain;                  ///< What is reported.
        core::platform::SteadyTimePoint lastAsked {}; ///< When a client last asked for it.
    };

    core::platform::IClock& _clock;
    mutable std::mutex _mutex;
    std::vector<Entry> _entries; ///< Guarded by `_mutex`; at most `Capacity`.
};

} // namespace FastCache::Distributed
