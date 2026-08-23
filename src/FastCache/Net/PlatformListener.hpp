// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)
    #include <FastCache/Net/IocpSocket.hpp>
#elif defined(__linux__)
    #include <FastCache/Net/EpollSocket.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Net/KqueueSocket.hpp>
#else
    #error "No reactor-backed listener for this platform"
#endif

namespace FastCache
{

/// The listener that pairs with `PlatformReactor`.
///
/// One `#if` ladder rather than one per consumer, and it is here for the reason
/// `PlatformReactor` is -- with the same correction attached. Both aliases lived
/// inside `Server/ReactorServerLoop.cpp` while the server was the only thing
/// accepting on a reactor, and when consensus arrived only the reactor was hoisted:
/// the listener stayed put, on the stated reasoning that consensus "takes a plain
/// blocking `IListener`".
///
/// **That reasoning was wrong, and the way it was wrong is worth keeping.** A
/// blocking listener makes `co_await Accept()` and `co_await Read()` complete
/// synchronously, so `RaftPeerServer`'s per-connection task -- written as a
/// `DetachedTask` precisely so several peers can be read at once -- runs to
/// completion inline. The accept loop therefore serves the first peer that connects
/// and never accepts another. In a three-node cluster that means each node reads
/// from exactly one of its two peers: votes and heartbeats from the third simply
/// never arrive, and the cluster never elects anybody. Nothing crashes and nothing
/// logs a fault; the fleet just never becomes ready.
///
/// So consensus accepts on the reactor too, which is what `RaftPeerServer`'s own
/// documentation said it did all along.
#if defined(_WIN32)
using PlatformListener = IocpListener;
#elif defined(__linux__)
using PlatformListener = EpollListener;
#elif defined(__APPLE__)
using PlatformListener = KqueueListener;
#endif

} // namespace FastCache
