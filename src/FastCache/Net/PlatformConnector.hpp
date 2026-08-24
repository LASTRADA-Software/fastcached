// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Selects the reactor-driven `IConnector` for this platform.
///
/// One `#if` ladder rather than one per consumer, matching
/// `Async/PlatformReactor.hpp` and `Net/PlatformListener.hpp`. A caller that
/// wants "the connector that does not block my reactor thread" names this and
/// gets whichever implementation the platform has.
///
/// The counterpart rule is worth stating here because it is the one that gets
/// broken: a component running on a reactor takes `PlatformConnector`, and a
/// component running on a thread that may legitimately block takes
/// `BlockingConnector`. Mixing them up is not a compile error -- both satisfy
/// `IConnector` -- and the symptom is either a stalled event loop or a
/// `SyncRun` that throws at runtime, which is why the blocking-only helpers take
/// a `BlockingConnector&` by type rather than an `IConnector&`.

#if defined(_WIN32)
    #include <FastCache/Net/IocpConnector.hpp>
#elif defined(__linux__)
    #include <FastCache/Net/EpollConnector.hpp>
#elif defined(__APPLE__)
    #include <FastCache/Net/KqueueConnector.hpp>
#endif

namespace FastCache
{

#if defined(_WIN32)
using PlatformConnector = IocpConnector;
#elif defined(__linux__)
using PlatformConnector = EpollConnector;
#elif defined(__APPLE__)
using PlatformConnector = KqueueConnector;
#endif

} // namespace FastCache
