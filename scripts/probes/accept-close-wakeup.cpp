// Does closing a listening socket wake a thread parked in `accept()` on this host?
//
// This is the measurement behind the platform split in `Net/BlockingSocket.hpp`'s
// `AcceptRaw` contract (#1238, #1286). It is committed so the claim can be re-run
// rather than re-argued -- the idiom `redis-eof-semantics.py` set -- and this is the
// highest-value kind of claim to make re-runnable, because the reader is by
// construction on ONE of the two platforms and cannot check the other half by reading
// anything at all.
//
// The two answers are opposite, and each is load-bearing for a different stop protocol:
//
//   * On WINDOWS the close IS the wakeup, so `RunMultiReactorWindows` closes first and
//     joins after. Joining first would hang forever.
//   * On POSIX the close does NOT wake a parked `accept`, so the remedy is
//     stop-join-then-close, and a caller that closes to stop an acceptor gets a thread
//     that never returns.
//
// Two correct designs with opposite reasoning. The defect #1238 found was that both
// contract sentences stated the Windows-only mechanism as a PORTABLE fact, on functions
// compiled for both platforms.
//
// == What the arms are, and why the second one alone proves nothing ==
//
//   A. `accept` CALLED on an already-closed handle  -- the wrong-order case.
//   B. `accept` PARKED, then the listener closed underneath it -- the stop case.
//
// **Arm A is the discrimination control and is the whole point.** Arm B on its own
// shows only that the parked call ended somehow. If A and B answer with the SAME code,
// then asserting that code cannot tell "the close unblocked a parked accept" from "the
// close happened first" -- it would be asserting something both sides produce. The
// verdict this probe prints is about the PAIR, never about either arm alone. Same
// reason `.agent/rules/wire-and-protocol.md` insists `Cancelled` stay distinguishable
// from `Silent`.
//
// The two platforms are distinguishable for DIFFERENT reasons, and flattening that
// would lose the finding: on Windows both arms return and the codes differ; on POSIX
// arm A returns and arm B never does. So the verdict has three values, not two, and a
// fourth for a run that did not establish its own premise.
//
// == Bounded, and the bound is MEASURED ==
//
// "Did not wake" is only a finding if the probe can say it OBSERVED none -- an expired
// wait otherwise reads as the answer. So the ceiling is compared against a steady clock
// and the MEASURED elapsed time is printed, never the sum of the sleeps that were
// requested. The original throwaway program this replaces counted `50 * 250ms` and
// called it "up to 12.5s", which is the method `Core/BoundedDrain.hpp` was extracted to
// stop being rewritten: a sleep costs at least one scheduler quantum more than it asked
// for, so a counted bound overruns by however many times round the loop it went. The
// figure recorded there is a 5 ms request costing ~15 ms on Windows -- three times --
// and a bound that reads as five seconds waiting fifteen.
//
// == Recorded results ==
//
// Conditions are part of the reading and are PINNED here rather than pointed at, because
// they describe the world at one instant: an edit that re-attributed these numbers to a
// later toolchain would be silently claiming a measurement nobody took.
//
//   Windows 11 Pro build 26200, MSVC cl (VS 18 Community), x64 -- 3 runs, identical:
//
//     A  called after close:  ret=-1  err=10038 (WSAENOTSOCK)
//     B  parked, then closed: ret=-1  err=10004 (WSAEINTR)   woke within ~50 ms
//     VERDICT: DISTINGUISHABLE -- the two arms answer with different codes
//
//   Linux 6.18.33.2-microsoft-standard-WSL2 x86_64, glibc 2.43, g++ -- 3 runs, identical:
//
//     A  called after close:  ret=-1  err=9 (EBADF)
//     B  parked, then closed: STILL PARKED after 12002-12045 ms
//     VERDICT: DISTINGUISHABLE -- one arm never returns
//
// Which is the platform split `AcceptRaw` documents, reproduced from scratch: the close
// is the wakeup on Windows and is nothing at all on POSIX.
//
// The "woke within ~50 ms" figure is an upper bound set by this probe's poll interval,
// not a latency -- see the print site.
//
// The POSIX elapsed figures are 12002-12045 ms against a 12 s ceiling. They differ from
// the original throwaway program's "12.5 s" because that program set a different bound,
// NOT because its number was wrong: 50 * 250 ms on Linux really is about 12.5 s. What
// was wrong was that the figure had never been observed -- it was the sleeps it asked
// for, added up -- and an unobserved elapsed is the one reading that cannot say whether
// the thread was still parked or the wait merely expired.
//
// == Build and run ==
//
//   POSIX:     c++ -std=c++17 -O0 -pthread -o /tmp/acceptprobe accept-close-wakeup.cpp && /tmp/acceptprobe
//   Windows:   cl /std:c++17 /EHsc accept-close-wakeup.cpp ws2_32.lib && accept-close-wakeup.exe
//              clang-cl /std:c++17 /EHsc accept-close-wakeup.cpp ws2_32.lib && accept-close-wakeup.exe
//
// Exit status: 0 = distinguishable (the property the code relies on holds here),
//              1 = NOT distinguishable (it does not hold here),
//              2 = inconclusive -- setup failed, arm B's thread never reported that it
//                  was about to call accept, or its accept returned before the close, so
//                  the run never established the premise it was measuring. The premise is
//                  checked BOTH ways round because failing it does not produce a quiet
//                  wrong answer: a thread that reaches accept only AFTER the close makes
//                  both arms answer the same code, and the pair then reads as 1, a
//                  confident denial of a property that holds.
//
// A parked `accept` cannot be joined, so on the POSIX path arm B's thread is DETACHED
// and the process exits with it still parked. That is deliberate and is what the
// measurement is; it is stated here because a detached thread is otherwise the kind of
// thing a reader corrects.

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <errno.h>
    #include <unistd.h>

    #include <netinet/in.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{

#if defined(_WIN32)
using Handle = SOCKET;
constexpr Handle InvalidHandle = INVALID_SOCKET;
constexpr char const* PlatformName = "Windows / Winsock";

int LastError()
{
    return WSAGetLastError();
}

void CloseListener(Handle s)
{
    ::closesocket(s);
}

/// The codes this measurement can produce, named. A bare number in a verdict is a
/// number the next reader has to go and look up, and looking it up is where the two
/// arms stop being compared.
char const* NameOf(int code)
{
    switch (code)
    {
        case 10004:
            return "WSAEINTR";
        case 10022:
            return "WSAEINVAL";
        case 10038:
            return "WSAENOTSOCK";
        case 10053:
            return "WSAECONNABORTED";
        case 10054:
            return "WSAECONNRESET";
        default:
            return "unnamed";
    }
}
#else
using Handle = int;
constexpr Handle InvalidHandle = -1;
constexpr char const* PlatformName = "POSIX / BSD sockets";

int LastError()
{
    return errno;
}

void CloseListener(Handle s)
{
    ::close(s);
}

/// See the Windows sibling.
char const* NameOf(int code)
{
    switch (code)
    {
        case EBADF:
            return "EBADF";
        case EINVAL:
            return "EINVAL";
        case EINTR:
            return "EINTR";
        case ECONNABORTED:
            return "ECONNABORTED";
        default:
            return "unnamed";
    }
}
#endif

/// A bound listener on an ephemeral loopback port, or `InvalidHandle`.
Handle MakeListener()
{
    Handle const s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == InvalidHandle)
        return InvalidHandle;

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(s, 4) != 0)
    {
        CloseListener(s);
        return InvalidHandle;
    }
    return s;
}

/// What a bounded wait saw, with the cost it actually paid.
struct WaitOutcome
{
    bool returned;    ///< Whether the flag was set before the ceiling.
    double elapsedMs; ///< MEASURED, never the sum of the requested sleeps.
};

/// Wait for @p done, bounded by @p ceiling, measuring against a steady clock.
WaitOutcome AwaitFlag(std::atomic<bool> const& done, std::chrono::milliseconds ceiling, std::chrono::milliseconds poll)
{
    auto const started = std::chrono::steady_clock::now();
    auto since = [started] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };

    while (!done.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() - started >= ceiling)
            return { false, since() };
        std::this_thread::sleep_for(poll);
    }
    return { true, since() };
}

/// How long arm B gives the parked accept to come back after the close.
constexpr auto Ceiling = std::chrono::seconds { 12 };

/// How often the wait looks. Granularity only; the bound above is measured.
constexpr auto Poll = std::chrono::milliseconds { 50 };

/// How long arm B lets the accept settle into its park before closing underneath it.
/// Without this the probe can close before the thread has reached `accept` at all,
/// which measures arm A a second time while looking like arm B.
///
/// **A sleep alone is not the premise, and getting this wrong is not an inconclusive
/// run -- it is a confident WRONG verdict.** A thread that has not reached `accept` by
/// the time the close lands calls it afterwards, both arms then answer the
/// close-happened-first code, and the pair prints `NOT DISTINGUISHABLE` and exits 1 on
/// a platform where the property holds. So the thread SAYS when it is about to make the
/// call (`reachedAccept`) and this wait is on that flag; the sleep stays afterwards,
/// because the flag is stored a few instructions before the syscall and only the sleep
/// covers that gap. Thread-start latency, which is what actually varies, is out of the
/// window entirely.
constexpr auto SettleFor = std::chrono::milliseconds { 250 };

/// Bound on waiting for the acceptor thread to say it is about to call `accept`. Only
/// thread start has to fit in here; a run that does not see it is INCONCLUSIVE, never a
/// verdict.
constexpr auto ReachCeiling = std::chrono::seconds { 5 };

} // namespace

int main()
{
#if defined(_WIN32)
    WSADATA wsa {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::puts("INCONCLUSIVE: WSAStartup failed");
        return 2;
    }
#endif

    std::printf("platform: %s\n", PlatformName);
    std::printf("ceiling:  %lld ms, measured against a steady clock\n",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Ceiling).count()));
    std::puts("");

    // --- Arm A: accept CALLED on an already-closed handle -------------------------
    //
    // The control. Nothing is parked here, so whatever this answers is what "the close
    // happened first" looks like -- and arm B only means something if it differs.
    Handle const closed = MakeListener();
    if (closed == InvalidHandle)
    {
        std::printf("INCONCLUSIVE: could not make a listener for arm A (%d %s)\n", LastError(), NameOf(LastError()));
        return 2;
    }
    CloseListener(closed);
    Handle const armAResult = ::accept(closed, nullptr, nullptr);
    int const armAError = LastError();
    std::printf(
        "A  called after close:  ret=%lld  err=%d (%s)\n", static_cast<long long>(armAResult), armAError, NameOf(armAError));

    // --- Arm B: accept PARKED, then the listener closed underneath it --------------
    static Handle parked = InvalidHandle;
    parked = MakeListener();
    if (parked == InvalidHandle)
    {
        std::printf("INCONCLUSIVE: could not make a listener for arm B (%d %s)\n", LastError(), NameOf(LastError()));
        return 2;
    }

    static std::atomic<bool> done { false };
    static std::atomic<bool> reachedAccept { false };
    static std::atomic<int> armBError { 0 };
    static std::atomic<long long> armBResult { 0 };

    std::thread acceptor { [] {
        reachedAccept.store(true, std::memory_order_release);
        Handle const accepted = ::accept(parked, nullptr, nullptr);
        armBError.store(LastError(), std::memory_order_relaxed);
        armBResult.store(static_cast<long long>(accepted), std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    } };

    // Wait to be TOLD, then settle. A run where the thread never reports is a run whose
    // premise was not established, so it says so rather than closing anyway.
    if (!AwaitFlag(reachedAccept, ReachCeiling, Poll).returned)
    {
        std::printf("INCONCLUSIVE: arm B's thread did not reach accept() within %lld ms, so nothing was parked\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(ReachCeiling).count()));
        CloseListener(parked);
        acceptor.detach();
        return 2;
    }
    std::this_thread::sleep_for(SettleFor);

    // **The premise, asserted rather than assumed.** If the accept has already come
    // back, nothing was parked and arm B measured arm A again -- which would print a
    // confident verdict about an experiment that did not happen.
    if (done.load(std::memory_order_acquire))
    {
        std::printf("INCONCLUSIVE: arm B's accept returned BEFORE the close (err=%d %s), so nothing was parked\n",
                    armBError.load(std::memory_order_relaxed),
                    NameOf(armBError.load(std::memory_order_relaxed)));
        acceptor.join();
        return 2;
    }

    CloseListener(parked);
    WaitOutcome const outcome = AwaitFlag(done, Ceiling, Poll);

    if (outcome.returned)
        // **`woke within` and not `woke after`.** The figure is bounded below by the poll
        // interval, so it is an UPPER bound on the wake and not a latency measurement:
        // this probe answers WHETHER the close wakes the accept, and anyone wanting how
        // FAST wants a condition variable rather than a poll. Printed with its
        // granularity beside it, because a bare number here reads as a latency.
        std::printf("B  parked, then closed: ret=%lld  err=%d (%s)   woke within %.1f ms (poll %lld ms -- an upper "
                    "bound, not a latency)\n",
                    armBResult.load(std::memory_order_relaxed),
                    armBError.load(std::memory_order_relaxed),
                    NameOf(armBError.load(std::memory_order_relaxed)),
                    outcome.elapsedMs,
                    static_cast<long long>(Poll.count()));
    else
        std::printf("B  parked, then closed: STILL PARKED after %.1f ms -- the close did not wake it\n", outcome.elapsedMs);

    // --- The verdict, which is about the PAIR ------------------------------------
    std::puts("");
    int status = 0;
    if (!outcome.returned)
    {
        std::puts("VERDICT: DISTINGUISHABLE -- one arm never returns.");
        std::puts("  Closing does NOT wake a parked accept here, so the close cannot be a stop");
        std::puts("  mechanism: a caller that closes to stop an acceptor gets a thread that never");
        std::puts("  returns. Stop-join-then-close is the remedy on this platform.");
    }
    else if (armAError != armBError.load(std::memory_order_relaxed))
    {
        std::puts("VERDICT: DISTINGUISHABLE -- the two arms answer with different codes.");
        std::puts("  Closing DOES wake a parked accept here, and the woken call is telling apart");
        std::puts("  from one that was called too late. The close is usable as a stop mechanism,");
        std::puts("  and joining before closing would hang.");
    }
    else
    {
        status = 1;
        std::puts("VERDICT: NOT DISTINGUISHABLE -- both arms answer with the same code.");
        std::puts("  A test asserting that code would be asserting something BOTH sides produce,");
        std::puts("  so it could not tell a woken parked accept from one called after the close.");
    }

    if (!outcome.returned)
    {
        // A parked `accept` cannot be joined. Deliberate; see the header.
        acceptor.detach();
    }
    else
    {
        acceptor.join();
    }
    return status;
}
