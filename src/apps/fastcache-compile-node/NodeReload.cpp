// SPDX-License-Identifier: Apache-2.0
#include "NodeReload.hpp"

#include <utility>

namespace FastCache::Node
{

void ApplyReloadRequest(NodeReloader* reloader, NodeMembership& membership, ILogger& logger)
{
    if (reloader == nullptr)
    {
        // Not silence: an operator who sent SIGHUP believes this worker has a file to
        // re-read, and the useful answer is that it has none.
        logger.Logf(LogLevel::Warn, "reload requested, but this worker was started with no configuration file");
        return;
    }

    // Taken BEFORE the swap, because "did the advertised set change" cannot be asked
    // afterwards: `Reload()` replaces the snapshot, and the previous one is then only
    // reachable through a reference somebody kept. Each snapshot is immutable, so
    // holding this one costs nothing and stays valid however the swap goes.
    auto const before = reloader->Current();

    auto const outcome = reloader->Reload();
    if (!outcome.has_value())
    {
        // One line for both refusals -- a file that would not parse and a file that
        // changed something immutable -- because the operator's situation is the same:
        // they saved once, and NOTHING was applied.
        logger.Logf(LogLevel::Warn, "reload declined, configuration unchanged: {}", outcome.error().ToString());
        return;
    }

    auto const current = reloader->Current();
    logger.SetMinLevel(current->logLevel);

    // **Unconditional, and that is the guard.** `Adopt` is idempotent and costs a
    // vector copy, so there is no branch here for a later reader to get wrong -- and
    // the branch is exactly what would be got wrong, since the expensive-looking half
    // is the one that must never be skipped. What decides whether anything is SAID is
    // `AdmissionAnnouncement`, which is pure and tested; what decides whether anything
    // takes effect is nothing at all.
    membership.Adopt(*current);

    // At WARN, which is the level a credential change is logged at, and for the same
    // reason: this is the setting that decides which machines this worker will spend
    // its CPU on, and an incident is read against what was in force at the time.
    if (auto const said = AdmissionAnnouncement(*before, *current); said.has_value())
        logger.Log(LogLevel::Warn, *said);

    // Asked here only to say the right thing to the operator, at the moment they
    // acted: the re-survey is the expensive part (`AdvertisedClaimsDiffer` carries
    // what it costs and why it is not run unconditionally), and somebody who saved a
    // file should know it was accepted before it finishes. The heartbeat thread asks
    // the same question again for itself, from the same function.
    if (AdvertisedClaimsDiffer(*before, *current))
        logger.Logf(LogLevel::Info,
                    "configuration reloaded; log level is now {}. What this worker serves has changed, so it will "
                    "re-derive its toolchains and re-register on the next heartbeat",
                    ToStringView(current->logLevel));
    else
        logger.Logf(LogLevel::Info, "configuration reloaded; log level is now {}", ToStringView(current->logLevel));
}

} // namespace FastCache::Node
