// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace FastCache
{

/// @file ListenQueue.hpp
/// How deep an accept queue every listener this project binds asks for, and how a platform's
/// `::listen` is told (#1668, #1679).
///
/// Its own header, beside `NativeListen.hpp` rather than in it, so `Config/Config.hpp` can default
/// the daemon's `--listen-backlog` to the same figure without pulling core-cpp's socket headers into
/// every file that reads a configuration. `NativeListen.hpp` includes it; nothing here binds.

/// The depth of the kernel's queue of completed handshakes every listener here asks for: the
/// node's framed surfaces, its consensus listener, the daemon's client listeners (whose
/// `--listen-backlog` defaults to it) and the admin endpoints `BlockingListener::Bind` opens.
///
/// **A full queue is not a refusal, and that is why it matters.** A connect that arrives while the
/// queue is full has its SYN dropped (Linux, with `tcp_abort_on_overflow` at its default) or
/// answered late (Windows), and the client's kernel retransmits after a second, so a launcher
/// holding a short connect bound reports a node that is UP as unreachable -- on the machine whose
/// build is largest. The framed surfaces took core-cpp's default of 128, and the scheduler on them
/// is the door every launcher of the fleet leases through -- a `-j 60` build per client, several
/// exchanges per unit, all against the leader. Measured on Windows 11 26200, 32 logical cores, a
/// Debug node on loopback answering LEASE, with no CPU load: bursts of N simultaneous connects from
/// ONE host's Python client, five rounds per cell (#1668):
///
///   backlog                    N = 100      N = 300                N = 600
///   128 (default)              max 24 ms    p99 520, max 1,036 ms  --
///   SOMAXCONN (INT_MAX)        max 12 ms    p99 517, max 528 ms    p50 510, max 1,025 ms
///   SOMAXCONN_HINT(4096)       max 3 ms     p99 10, max 23 ms      p99 20, max 25 ms
///
/// connect latency; every reply that started took at most 90 ms in all of them. The stalls are SYN
/// retransmits off a full queue -- INFERRED from where they land, on the 0.5 s and 1 s steps a
/// retransmitted SYN waits, not observed on the wire -- and past the launcher's 1 s connect bound
/// they are `Unreached`, the half #1668 could not name. CPU saturation alone (32 burners on 32
/// cores, sequential LEASEs) never passed 3.6 ms connect nor 46 ms reply.
///
/// That table is the FRAMED SURFACES'. The consensus listener (core-cpp's 128) and the daemon's
/// client listeners (`--listen-backlog`, then 511, which a plain Windows `::listen` holds to about
/// 200) take the same figure by the same reasoning rather than by a burst measured against them
/// (#1679): a Raft peer set dials rarely, while the daemon is the other door a `make -jN` driving
/// the launcher hits at once. The admin endpoints take it because they bind through the same
/// `BlockingListener::Bind` default, and an entry costs nothing until a connection fills it.
///
/// On POSIX the kernel clamps the figure to its own ceiling -- `net.core.somaxconn` (Linux, 4096 by
/// default since 5.4) or `kern.ipc.somaxconn` (BSD, macOS) -- so it asks for the same depth and
/// changes nothing where that ceiling is 128: macOS's default, and Linux's before 5.4. **A
/// socket-activated node does not reach this constant**: it adopts a listener systemd already
/// made, whose depth is the unit's `Backlog=` (`packaging/linux/fastcache-compile-node.socket`,
/// 4096 for this reason).
inline constexpr int ListenQueueDepth = 4096;

/// How a platform's `::listen` reads its backlog argument.
///
/// PRIVATE to this process: never transmitted or persisted, so no enumerator carries a value.
enum class BacklogSpelling : std::uint8_t
{
    /// The argument is the depth, clamped by the kernel to its own ceiling. POSIX.
    Depth,
    /// A positive argument is held to a small fixed range -- about 200, `SOMAXCONN` included,
    /// INFERRED from the table above, whose `SOMAXCONN` row overflows between 100 and 300 -- and a
    /// deeper queue is asked for as `SOMAXCONN_HINT(depth)`, which `winsock2.h` defines as
    /// `-(depth)` and the stack clamps to [200, 65535]. Windows.
    Hint,
};

/// The spelling this build's `::listen` reads.
inline constexpr BacklogSpelling ThisPlatformBacklogSpelling =
#if defined(_WIN32)
    BacklogSpelling::Hint;
#else
    BacklogSpelling::Depth;
#endif

/// The `::listen` argument that asks a platform reading @p spelling for a queue @p depth deep.
///
/// EVERY depth is spelled as a hint where the platform reads hints, the operator's too: a plain
/// argument there is capped near 200 whatever it says, so a `--listen-backlog=1024` passed through
/// verbatim would ask for less than it reads as -- and the hint's floor of 200 means a depth below
/// it is raised to 200 rather than honoured, which only ever makes a queue deeper. Written out as
/// `-depth` because `SOMAXCONN_HINT` lives in a header this one must not include; the tests compare
/// the two on Windows.
///
/// The spelling is a parameter rather than an `#if` here, so both are tested on every platform;
/// the one place that picks this build's is `ThisPlatformBacklogSpelling`.
///
/// core-cpp passes `ListenOptions::backlog` to `::listen` verbatim on both platforms and has no
/// spelling of its own for a deep queue yet; https://github.com/contour-terminal/core-cpp/issues/74
/// asks for one. When it lands, this function is what goes, and every caller already states a
/// depth.
/// @param depth The queue depth wanted; positive.
/// @param spelling How the platform reads the argument.
/// @return The argument to hand `::listen`, directly or through `core::net::ListenOptions::backlog`.
[[nodiscard]] constexpr int ListenBacklogArgument(int depth, BacklogSpelling spelling) noexcept
{
    return spelling == BacklogSpelling::Hint ? -depth : depth;
}

} // namespace FastCache
