// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/NetError.hpp>

#include <cassert>
#include <expected>

namespace FastCache::Detail
{

/// Retire a parked read operation on a reactor socket: detach it, drop the interest,
/// and complete it with `Cancelled`.
///
/// **The counterpart to `ClaimReadSlot`, and it lives here for the same reason.** That
/// one is the discipline's tripwire; this is the only way a caller can honour it
/// without closing the socket (`ISocket::CancelRead`, #710). Both are about the single
/// read-op slot, so both belong beside it rather than in whichever transport was
/// written first.
///
/// **Shared because `EpollSocket` and `KqueueSocket` spell it identically**, down to
/// the ordering, and this is the second copy of the detach-then-complete discipline in
/// this directory -- `Close()` in each of them is the first, and those two are already
/// byte-identical duplicates that predate this. A third copy would be a defect by this
/// project's own data-driven rule; a template over the `Impl` is the one generic that
/// takes both without inventing a type.
///
/// **Detached FIRST and completed LAST, with no member touched afterwards.** That is
/// not tidiness: `Complete` resumes the parked coroutine, and a coroutine that OWNS
/// this socket runs to its end and destroys it before `Complete` returns -- so
/// `updateInterest` and every field access must already have happened. `EpollSocket::Close`
/// records the ASan report that established this.
///
/// `IocpSocket` deliberately does NOT use it: the kernel owns that op's `OVERLAPPED`,
/// so its retirement retracts rather than completes, and sharing a body between the two
/// shapes would hide exactly the difference #884 is about.
///
/// @tparam Impl The socket's `Impl`, which must expose `readOp` and `UpdateInterest()`.
/// @param impl The socket's implementation block.
template <typename Impl>
void RetireParkedRead(Impl& impl) noexcept
{
    auto* const parked = impl.readOp.awaitable;
    if (parked == nullptr)
        return;
    impl.readOp.awaitable = nullptr;
    impl.readOp.readBuffer = {};
    impl.readOp.readPeekOnly = false;
    // Recomputed from `readOp.awaitable`, which is now null, so the read interest comes
    // back down: the operation is gone and a readable edge would find nothing to
    // complete.
    impl.UpdateInterest();
    parked->Complete(std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
}

/// Take a socket's single read-op slot for an operation that is about to park.
///
/// **A socket has ONE read operation, and `Read` and `WaitReadable` share it.**
/// Every reactor socket keeps one `awaitable` pointer per direction, and both read
/// verbs begin by clearing it. So arming either while the other is parked drops the
/// parked awaitable's pointer: that coroutine is never resumed and never freed --
/// one leaked frame, plus everything it captured, per occurrence. No assertion, no
/// error, no log, and a leak proportional to traffic on whatever path did it
/// ([#663](https://github.com/LASTRADA-Software/fastcached/issues/663)).
///
/// **The rule was invisible from the place it must be obeyed.** The one correct
/// statement of it in this tree was a comment in `RedisResp.cpp` describing that
/// file's own watcher; somebody writing a second `WaitReadable` user reads
/// `ISocket::WaitReadable`, which documents what the call does and used to say
/// nothing about exclusivity. The rule now sits on the interface, and this is what
/// makes it detectable rather than a matter of having read the right file.
///
/// **Assignment and check are one expression on purpose.** Each call site used to
/// spell the claim as a bare `awaitable = nullptr;`, which is a line to forget the
/// guard on, at the seventh site as at the first. There is no such line left: the
/// clear happens here or it does not happen, which is the same shape as
/// `SigningDomain` leaving no argument to pass a bare label to.
///
/// **Debug-only, deliberately.** In release this is one store, exactly as before:
/// the alternative -- refusing the new operation -- would turn today's silent leak
/// into a broken connection on a path that is live right now
/// ([#710](https://github.com/LASTRADA-Software/fastcached/issues/710)), which is a
/// worse trade than the leak it replaces. So the fix for a caller that does this is
/// at the caller; what belongs here is the tripwire that names it.
///
/// **The reason for that last sentence CHANGED, and the old one is still quoted
/// elsewhere.** This header used to say that completing the dropped awaitable would
/// *"resume a coroutine that may already have lost the socket it holds: a
/// use-after-free where today there is a leak"*. That claim is recorded as one of
/// two positions on #710, and another lane withdrew a cancellation primitive on the
/// strength of it -- so a reader arriving from either needs to know which half moved.
///
/// **It was overstated, and the counter-evidence is in this directory.**
/// `EpollSocket::Close` and `KqueueSocket::Close` already detach a parked awaitable
/// and then `Complete` it with `Cancelled`, which is exactly the mechanism that
/// sentence called unsafe -- and they do it to `RedisResp`'s disconnect trampoline on
/// every close that finds one parked. Were it a use-after-free for that caller, this
/// tree would be crashing on ordinary disconnects. It is not a hypothesis to weigh;
/// it is the current behaviour. The general caution was real and was applied to a
/// specific caller without being checked against it.
///
/// **The hazard is the SITE, not ownership**, and that survives the correction:
///
///  - At `Close()` the socket is being torn down, every caller already expects an
///    error, and there is no half-armed slot. Safe, and in use.
///  - At the ARM site -- inside `Read`, before it re-points the slot -- the resumed
///    coroutine runs on a socket whose read slot is mid-claim, and is free to call
///    `Read` again.
///  - And cancelling a trampoline whose iteration is still LIVE is a false
///    disconnect: it resolves that iteration's waiter as *the peer went away* and
///    drops a healthy client. **At the arm site a socket cannot tell a stale parked
///    wait from a live one. The caller can**, because it knows when its own iteration
///    ended -- which is why the cancel belongs there and not here.
///
/// **And the caller now has a spelling for it.** `ISocket::CancelRead()` retires a
/// parked read-side operation without closing the socket, which is what the paragraph
/// above says belongs at the caller -- until it existed, *abandon* had no spelling
/// short of tearing the connection down, so the discipline this file states was one no
/// caller could actually follow. It changes nothing here: this assertion still fires on
/// a double-arm, and a caller that arms over a parked wait is still wrong.
///
/// Two callers double-armed this slot and needed different fixes.
/// [#710](https://github.com/LASTRADA-Software/fastcached/issues/710)
/// (`RunBlockingRead`'s trampoline, re-armed per blocking iteration) is FIXED -- it
/// keeps one watch, re-targets it per pass and retires it through `CancelRead`.
/// [#755](https://github.com/LASTRADA-Software/fastcached/issues/755)
/// (`RearmReadable`'s watcher surviving the transition out of subscribe mode) is still
/// OPEN, and aborts at the assertion below -- which names the slot and not the caller,
/// so a fixture for it that wanders through a blocking read's path still produces a red
/// that attributes the wrong defect. That sentence survives one of its two subjects
/// going away, which is the point of keeping it.
///
/// It is watched refusing by `read-slot-guard-canary`, which double-arms a REAL
/// socket -- the call site, not this function -- and must die.
///
/// @param slot The op's `awaitable` pointer, cleared by this call.
inline void ClaimReadSlot(IoAwaitable*& slot) noexcept
{
    assert(slot == nullptr
           && "a read operation was armed over a parked one: Read and WaitReadable share the socket's single "
              "read-op slot, so this drops the parked coroutine, which is then never resumed and never freed "
              "(see FastCache/Net/ReadSlot.hpp and issue #663)");
    slot = nullptr;
}

} // namespace FastCache::Detail
