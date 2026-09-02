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
/// What a deadline closes, and where it records that it fired.
///
/// **The flag is the whole reason this is a struct rather than a bare `ISocket*`**
/// ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)). Expiry
/// closes the socket, so an exchange that ran out of budget and one whose peer went
/// away arrive at the caller as the same thing: a broken socket. Those are opposite
/// diagnoses -- one says the compile was too slow, the other says the machine is
/// gone -- and an operator seeing one sentence for both has nothing to act on. Only
/// the timer knows which happened, so only the timer can record it.
///
/// Caller-owned rather than returned beside the timer, because the timer captures
/// its address at construction: a flag living inside a returned value would be
/// pointed at through whatever storage the return expression used.
struct SocketDeadlineTarget
{
    /// Closed on expiry; must outlive the timer.
    ISocket* socket { nullptr };

    /// Set by the timer, and only by the timer. False means the exchange ended for
    /// its own reasons -- which, for a keepalive-armed dial, is how a dead peer
    /// looks.
    bool expired { false };
};

/// @param reactor Where to arm it, or nullptr for none.
/// @param ceiling How long the operation may take; non-positive means unbounded.
/// @param target What to close on expiry and where to record it; must outlive the
///        returned timer.
/// @return The armed timer, or `std::nullopt` when nothing was armed.
[[nodiscard]] inline std::optional<DeadlineTimer> ArmSocketDeadline(IReactor* reactor,
                                                                    std::chrono::milliseconds ceiling,
                                                                    SocketDeadlineTarget* target)
{
    if (reactor == nullptr || ceiling <= std::chrono::milliseconds::zero() || target == nullptr)
        return std::nullopt;

    return std::optional<DeadlineTimer> { std::in_place,
                                          *reactor,
                                          reactor->Clock().Now() + ceiling,
                                          [](void* state) {
                                              auto& fired = *static_cast<SocketDeadlineTarget*>(state);
                                              // Recorded BEFORE the close, so a
                                              // caller resumed by the close can
                                              // never observe a socket that shut
                                              // without a reason attached.
                                              fired.expired = true;
                                              fired.socket->Close();
                                          },
                                          target };
}

} // namespace FastCache
