// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)
    #include <FastCache/Async/IocpReactor.hpp>
#elif defined(__linux__)
    #include <FastCache/Async/EpollReactor.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Async/KqueueReactor.hpp>
#else
    #error "No reactor implementation for this platform"
#endif

namespace FastCache
{

/// The reactor this platform uses.
///
/// One `#if` ladder rather than one per consumer. It lived inside
/// `Server/ReactorServerLoop.cpp` while the server was the only thing that needed a
/// reactor; the moment a second component wants one -- consensus, whose driver ticks
/// its election timers on a reactor -- the choice is either shared or copied, and a
/// copied platform ladder is how a port to a fourth platform comes to build one half
/// of a binary and not the other.
///
/// The matching listener is `Net/PlatformListener.hpp`, and it was hoisted a commit
/// later than this one -- on the mistaken reasoning, recorded there, that consensus
/// could accept on a blocking listener.
#if defined(_WIN32)
using PlatformReactor = IocpReactor;
#elif defined(__linux__)
using PlatformReactor = EpollReactor;
#elif defined(__APPLE__)
using PlatformReactor = KqueueReactor;
#endif

} // namespace FastCache
