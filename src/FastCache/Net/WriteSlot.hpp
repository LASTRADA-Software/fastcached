// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>

#include <cassert>

namespace FastCache::Detail
{

/// Take a socket's single write-op slot for an operation that is about to park.
///
/// **The read-slot rule's missing half.** `Net/ReadSlot.hpp` states it for reads --
/// a socket has ONE read operation, `Read` and `WaitReadable` share it, and arming
/// either over a parked one drops that coroutine with no assertion, no error and no
/// log (#663). Every reactor socket keeps one `awaitable` pointer *per direction*,
/// so the identical failure is reachable on the write side, and until this existed
/// it was reachable with none of the machinery that makes it observable on the read
/// side (#893).
///
/// The rule was written about reads because reads are where it was first observed,
/// not because writes are exempt.
///
/// **How it is reached.** `TlsSocket::CancelRead()` forwards to `_raw->CancelRead()`,
/// which retires a pump parked on the READ slot. A pump suspended in
/// `FlushOutgoing`'s `_raw->Write(...)` is not retired at all, because `CancelRead`
/// touches only the read slot -- reachable whenever the outgoing BIO holds bytes at
/// the moment of cancellation: a TLS 1.3 KeyUpdate, a session ticket, an alert.
/// `RunBlockingRead` then proceeds to `co_await write()`, which starts a SECOND
/// `PumpWrite` over the same raw write slot.
///
/// **Assignment and check are one expression, for the reason `ClaimReadSlot` gives.**
/// Each arm site spelled the claim as a bare `writeOp.awaitable = nullptr;`, which is
/// a line to forget the guard on -- at the seventh site as at the first. There is no
/// such line left: the clear happens here or it does not happen. A guard folded INTO
/// the operation is self-enforcing; a guard called ALONGSIDE one needs a scan.
///
/// **Debug-only, deliberately**, and the same trade as the read side: in release this
/// is one store. Refusing the new operation instead would turn a silent leak into a
/// broken connection on a live path, which is the worse trade.
///
/// **There is deliberately no `CancelWrite` counterpart yet.** `ISocket::CancelRead`
/// exists because a caller needed a spelling of *abandon* short of `Close()`; no
/// caller needs that on the write side today, and inventing the verb before a caller
/// needs it would be seven transports writing `{}` with no reason beside it -- which
/// is *forgot* in the vocabulary of *decided* (#892). This is the tripwire only.
///
/// Watched refusing by `ctest -R write-slot-guard-canary`, which double-arms a REAL
/// socket -- the call site, not this function -- and must die.
///
/// @param slot The op's `awaitable` pointer, cleared by this call.
inline void ClaimWriteSlot(IoAwaitable*& slot) noexcept
{
    assert(slot == nullptr
           && "a write operation was armed over a parked one: a socket has a single write-op slot, so this "
              "drops the parked coroutine, which is then never resumed and never freed "
              "(see FastCache/Net/WriteSlot.hpp and issue #893)");
    slot = nullptr;
}

} // namespace FastCache::Detail
