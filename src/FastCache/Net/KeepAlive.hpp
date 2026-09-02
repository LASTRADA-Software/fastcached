// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>

namespace FastCache
{

/// Whether a dialled connection should carry TCP keepalive probes.
///
/// **What it answers, and the question it does NOT answer.** Keepalive detects a
/// dead connection or a dead HOST -- powered off, cable pulled, VPN dropped, laptop
/// suspended -- and that is the common hard failure. It does **not** detect a peer
/// that is alive and simply not writing; that needs a liveness signal in the
/// protocol, which is
/// [#245](https://github.com/LASTRADA-Software/fastcached/issues/245), and this does
/// not make it unnecessary. The two are different questions and collapsing them is
/// how a slow peer gets killed and a dead one gets waited on.
///
/// **Why it is asked per dial rather than armed for the process.** The obvious home
/// is `Detail::ApplyHotSocketOptions`, which is the one function every socket this
/// process owns passes through -- and that is exactly why it is the wrong one.
/// Arming it there would also change when an idle memcached or Redis client
/// connection is dropped, and when a Raft peer link is torn down, fleet-wide, for a
/// change nobody asked for. The same rule the tree already applies to SIGPIPE:
/// suppressed per socket, never process-wide.
///
/// An enum rather than a `bool` because it appears in an API surface, per the
/// project guidelines -- `Connect(host, port, { .keepAlive = KeepAlive::Yes })`
/// reads as what it is, where a bare `true` at that position reads as nothing.
enum class KeepAlive : std::uint8_t
{
    No,  ///< Leave the platform default, which is off.
    Yes, ///< Probe, with the intervals in `KeepAliveSettings`.
};

/// How aggressively a keepalive-armed connection probes a silent peer.
///
/// **Bare `SO_KEEPALIVE` is worth nothing.** Without the intervals it inherits the
/// system default, which on Linux is two hours -- longer than any deadline this
/// would be protecting. So the flag is never set unless these are, and a platform
/// on which the intervals cannot be applied does not get the flag either.
///
/// **The values.** A dispatched compile's client sits waiting with nothing
/// outstanding, so the connection is idle from its send side and keepalive is the
/// mechanism that applies (a client with unacknowledged data in flight would be
/// bounded by retransmission timeouts instead, which is a different knob). The
/// budget these serve is
/// [#247](https://github.com/LASTRADA-Software/fastcached/issues/247)'s: a dead host
/// noticed in well under a minute, against a dispatch deadline that is minutes long
/// by design since #223 and must not be shortened.
///
/// | platform | probes stop at |
/// |---|---|
/// | Linux, macOS | `idle + count * interval` = **16 s** |
/// | Windows | `idle + 10 * interval` = **30 s** |
///
/// **The Windows row is not a typo and `count` is not honoured there.**
/// `SIO_KEEPALIVE_VALS` takes the idle time and the interval only; the probe count
/// has been fixed at 10 by the OS since Vista and there is no way to set it. Stated
/// rather than papered over -- taking a value that cannot be applied is how a
/// configuration option becomes a lie. Both rows are under the one-minute bar, which
/// is what the interval was chosen to make true on the slowest of the three.
struct KeepAliveSettings
{
    /// Quiet time before the first probe.
    std::chrono::milliseconds idle { 10'000 };

    /// Gap between probes once they start.
    std::chrono::milliseconds interval { 2'000 };

    /// Unanswered probes before the connection is declared dead.
    ///
    /// **Ignored on Windows**, where the OS fixes it at 10; see the class note.
    std::uint32_t count { 3 };
};

} // namespace FastCache
