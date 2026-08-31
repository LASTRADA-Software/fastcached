// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <optional>

namespace FastCache
{

/// Arm a deadline that closes `socket` when it expires, or arm nothing.
///
/// One policy, three decisions, and it existed twice — once in the node's
/// `RemoteUpstream` and once inline in the launcher's `ReactorExchange` — with only
/// one of the two covered by a regression test
/// ([#248](https://github.com/LASTRADA-Software/fastcached/issues/248)).
///
/// **A non-positive ceiling arms NOTHING**, and that is the decision worth having in
/// one place. The arithmetic says the opposite of what the value means: a zero
/// ceiling puts the deadline at `Now()`, so an exchange dies on the reactor's next
/// turn — a knob documented as "turn the ceiling off" that turns the *cache* off
/// instead, silently, because every caller answers a transport failure by compiling.
/// This tree has been bitten by the same shape elsewhere: `--cache-memory 0` means
/// *no tier*, while zero is how `InMemoryLruStorage` spells *unbounded*.
///
/// A null reactor also arms nothing. That arm has no counterpart on the launcher
/// side, which always has a reactor, and it is why this takes `IReactor*` rather than
/// `IReactor&` — a helper written against the launcher's assumptions would not serve
/// the node.
///
/// `std::optional` rather than `std::unique_ptr`: a timer that is not armed is
/// honestly spelled by an empty optional, and it costs no allocation. `DeadlineTimer`
/// is immovable, so the value is constructed in place and returned as a prvalue.
///
/// Not to be confused with `Cc::ExchangeBudget::BoundsTotal()`, which asks a
/// different question at a different level — *does this budget bound anything*, for
/// an app's own control flow. This is the socket-level act of arming one.
///
/// @param reactor Where to arm it, or nullptr for none.
/// @param ceiling How long the operation may take; non-positive means unbounded.
/// @param socket What to close on expiry; must outlive the returned timer.
/// @return The armed timer, or `std::nullopt` when nothing was armed.
[[nodiscard]] inline std::optional<DeadlineTimer> ArmSocketDeadline(IReactor* reactor,
                                                                    std::chrono::milliseconds ceiling,
                                                                    ISocket* socket)
{
    if (reactor == nullptr || ceiling <= std::chrono::milliseconds::zero())
        return std::nullopt;

    return std::optional<DeadlineTimer> { std::in_place,
                                          *reactor,
                                          reactor->Clock().Now() + ceiling,
                                          [](void* target) { static_cast<ISocket*>(target)->Close(); },
                                          socket };
}

} // namespace FastCache
