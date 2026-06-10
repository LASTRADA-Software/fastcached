// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Cache/SetCodec.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/IPubSubRegistry.hpp>
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/RedisRespDetail.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <format>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    constexpr std::size_t MaxLineBytes = 65536;
    constexpr std::size_t MaxPayloadBytes = 64 * 1024 * 1024;
    constexpr std::string_view Crlf = "\r\n";

    /// MULTI queue caps — guard against a malicious client that streams an
    /// unbounded number (or size) of QUEUED commands without ever sending
    /// EXEC, exhausting the daemon's memory before storage eviction kicks in.
    /// Once either cap is breached the transaction is marked dirty (the
    /// matching EXEC replies `-EXECABORT`) and the queue is cleared to free
    /// memory immediately. The numbers are conservative defaults sized for
    /// realistic transaction batches; clients that legitimately need more can
    /// always split into multiple MULTI/EXEC rounds.
    constexpr std::size_t MaxQueuedCommands = 65'536;
    constexpr std::size_t MaxQueuedBytes = 256ULL * 1024ULL * 1024ULL;

    /// The RESP protocol version negotiated for a connection. A connection
    /// starts in RESP2 and may upgrade to RESP3 via `HELLO 3`. The reply writers
    /// branch on this so a single code path serves both wire formats.
    enum class RespVersion : std::uint8_t
    {
        Resp2 = 2,
        Resp3 = 3,
    };

    /// Per-connection subscriber: the bridge between the process-wide pub/sub
    /// registry (which calls Deliver from any reactor thread) and this
    /// connection's command loop (which runs on its own pinned reactor thread).
    ///
    /// Deliver enqueues a message under `_mu` and, if the loop is parked waiting
    /// for a push, wakes it via the reactor's thread-safe Submit. The loop drains
    /// the queue on its own thread, so the socket is only ever written there.
    class Subscriber final: public ISubscriber
    {
      public:
        /// @param reactor The reactor this connection is pinned to, or nullptr
        ///        for non-reactor transports (delivery then resumes inline).
        explicit Subscriber(IReactor* reactor) noexcept:
            _reactor { reactor }
        {
        }

        void Deliver(PushMessage message) override
        {
            std::shared_ptr<WakeLatch> latch;
            {
                std::scoped_lock const lock { _mu };
                _queue.push_back(std::move(message));
                latch = std::exchange(_latch, {});
            }
            // Wake the parked loop through the shared latch (the readable arm and
            // this push arm race; the latch ensures a single resume). The resume
            // is marshalled onto the loop's own reactor thread — we never touch
            // the socket here.
            if (latch)
                latch->WakeOnce(_reactor);
        }

        /// Move out every queued message (called on the owner's reactor thread).
        [[nodiscard]] std::deque<PushMessage> DrainQueue()
        {
            std::scoped_lock const lock { _mu };
            return std::exchange(_queue, {});
        }

        /// @return True if at least one message is queued.
        [[nodiscard]] bool HasPending() const
        {
            std::scoped_lock const lock { _mu };
            return !_queue.empty();
        }

        /// Shared one-shot latch coordinating the two arms of WaitForPushOrReadable
        /// (a delivered push vs. the socket becoming readable). Whichever arm
        /// fires first flips `resolved` and resumes the parked loop exactly once;
        /// the losing arm sees `resolved` already set and does nothing. Held by
        /// shared_ptr so it outlives the detached readable-watcher task even if
        /// the push arm won and the loop moved on.
        struct WakeLatch
        {
            std::mutex mu;
            std::coroutine_handle<> handle {};
            bool resolved { false };

            /// Resume the parked loop if no arm has resolved yet. Thread-safe.
            /// @param reactor Reactor to marshal the resume onto, or nullptr to
            ///        resume inline (non-reactor transports / same thread).
            void WakeOnce(IReactor* reactor)
            {
                std::coroutine_handle<> toWake {};
                {
                    std::scoped_lock const lock { mu };
                    if (resolved)
                        return;
                    resolved = true;
                    toWake = std::exchange(handle, {});
                }
                if (toWake)
                {
                    if (reactor != nullptr)
                        reactor->Submit(toWake);
                    else
                        toWake.resume();
                }
            }
        };

        /// Awaitable suspending the command loop until EITHER a message is
        /// delivered OR the socket becomes readable. The push arm is the
        /// subscriber (Deliver -> WakeLatch); the readable arm is a single
        /// long-lived watcher coroutine (started once, see StartReadableWatcher)
        /// that trips the current latch. Exactly one resume happens per wait.
        struct PushOrReadable
        {
            Subscriber* self;

            [[nodiscard]] bool await_ready() const noexcept
            {
                return self->HasPending() || self->_readablePending;
            }
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const
            {
                std::scoped_lock const lock { self->_mu };
                if (!self->_queue.empty() || self->_readablePending)
                    return false; // raced: work is already available.
                auto const latch = std::make_shared<WakeLatch>();
                latch->handle = handle;
                self->_latch = latch;
                return true;
            }
            void await_resume() const noexcept {}
        };

        /// Suspend the loop until a push arrives or the socket is readable.
        /// Requires StartReadableWatcher() to have been called once first.
        [[nodiscard]] PushOrReadable WaitForPushOrReadable() noexcept
        {
            return PushOrReadable { this };
        }

        /// Start the single, long-lived readable-watcher coroutine for this
        /// connection. It owns the ONLY outstanding socket->WaitReadable() at any
        /// time (so the socket's single read-op slot is never double-armed), sets
        /// `_readablePending` and trips the current latch whenever the socket
        /// becomes readable, and re-arms after the command loop has consumed the
        /// pending bytes. Idempotent: a second call is a no-op.
        /// @param socket The connection socket to watch.
        void StartReadableWatcher(ISocket* socket)
        {
            if (_watcherStarted)
                return;
            _watcherStarted = true;
            {
                std::scoped_lock const lock { _mu };
                // Retain the socket pointer so ShutdownWatcher can cancel a
                // parked initial WaitReadable. Without this, the watcher
                // parked on the very first WaitReadable (before any rearm
                // latch has been installed) is unreachable from
                // ShutdownWatcher — only socket->Close() can cancel the
                // pending I/O operation, and on epoll/kqueue that
                // cancellation may complete asynchronously after the
                // socket has already been destroyed by the connection.
                _socket = socket;
            }
            // The watcher takes a shared_ptr by value so its coroutine frame
            // keeps the Subscriber alive even after Run's frame has returned —
            // closing the second UAF where a parked WaitReadable resumed onto
            // a destroyed Subscriber's mutex.
            RunReadableWatcher(SharedFromThisAsSubscriber(), socket);
        }

        /// Wake any latch the watcher is parked on AND close the watched
        /// socket so a parked initial WaitReadable resumes with an error
        /// promptly. Pre-fix, this only woke `_rearm`; if the watcher had
        /// not yet reached its first rearm point (it was still parked on
        /// the initial WaitReadable from StartReadableWatcher), the wake
        /// was a no-op and the coroutine remained suspended until the
        /// connection's socket destruction eventually unwound the
        /// reactor's per-socket state — by which time the awaiter could
        /// resume against freed memory.
        void ShutdownWatcher() noexcept
        {
            std::shared_ptr<WakeLatch> latch;
            ISocket* socket = nullptr;
            {
                std::scoped_lock const lock { _mu };
                _shuttingDown = true;
                latch = std::exchange(_rearm, {});
                socket = std::exchange(_socket, nullptr);
            }
            if (latch)
                latch->WakeOnce(_reactor);
            // socket->Close() is idempotent (ISocket contract) and triggers
            // the reactor to cancel any pending WaitReadable with an error
            // completion. The connection's own Close on its way out is
            // redundant once we've done this, but harmless.
            if (socket != nullptr)
                socket->Close();
        }

        /// Called by the command loop once it has drained the readable bytes, so
        /// the watcher re-arms for the next readability edge.
        void RearmReadable()
        {
            std::shared_ptr<WakeLatch> resume;
            {
                std::scoped_lock const lock { _mu };
                _readablePending = false;
                resume = std::exchange(_rearm, {});
            }
            if (resume)
                resume->WakeOnce(_reactor);
        }

        /// @return True if the watcher signalled readable bytes are waiting.
        [[nodiscard]] bool ReadablePending() const
        {
            std::scoped_lock const lock { _mu };
            return _readablePending;
        }

        /// shared_from_this() on the ISubscriber base returns
        /// std::shared_ptr<ISubscriber>; we want a Subscriber pointer to
        /// access the private latch/reactor state. The static cast is safe:
        /// every Subscriber IS an ISubscriber and the enable_shared_from_this
        /// machinery is the same.
        [[nodiscard]] std::shared_ptr<Subscriber> SharedFromThisAsSubscriber()
        {
            return std::static_pointer_cast<Subscriber>(shared_from_this());
        }

      private:
        /// The single readable-watcher loop. Awaits readability, flags it + trips
        /// the wait latch, then parks on an internal latch until RearmReadable()
        /// lets it watch the next edge. Lives as long as the connection (the
        /// connection outlives it by construction: it only re-arms while the loop
        /// is in subscribe mode and is abandoned when the connection frame ends).
        /// @param self Owning reference to the subscriber (keeps it alive
        ///             across every co_await, even after Run's frame returns).
        static DetachedTask RunReadableWatcher(std::shared_ptr<Subscriber> self, ISocket* socket)
        {
            while (true)
            {
                {
                    std::scoped_lock const lock { self->_mu };
                    if (self->_shuttingDown)
                        co_return;
                }
                auto const readable = co_await socket->WaitReadable();
                if (!readable.has_value())
                    co_return; // socket closed/errored — the loop will observe it.

                auto rearmLatch = std::make_shared<WakeLatch>();
                std::shared_ptr<WakeLatch> waitLatch;
                {
                    std::scoped_lock const lock { self->_mu };
                    if (self->_shuttingDown)
                        co_return;
                    self->_readablePending = true;
                    self->_rearm = rearmLatch;
                    waitLatch = std::exchange(self->_latch, {});
                }
                if (waitLatch)
                    waitLatch->WakeOnce(self->_reactor);

                // Park until the command loop has consumed the bytes and re-armed.
                co_await SuspendOnLatch { rearmLatch };
                {
                    std::scoped_lock const lock { self->_mu };
                    if (self->_shuttingDown)
                        co_return;
                }
            }
        }

        /// Awaitable that parks the watcher on a one-shot latch until WakeOnce.
        struct SuspendOnLatch
        {
            std::shared_ptr<WakeLatch> latch;
            [[nodiscard]] bool await_ready() const noexcept
            {
                return false;
            }
            [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const
            {
                std::scoped_lock const lock { latch->mu };
                if (latch->resolved)
                    return false;
                latch->handle = handle;
                return true;
            }
            void await_resume() const noexcept {}
        };

        IReactor* _reactor;
        mutable std::mutex _mu;
        std::deque<PushMessage> _queue;
        std::shared_ptr<WakeLatch> _latch {}; ///< Latch the command loop parks on.
        std::shared_ptr<WakeLatch> _rearm {}; ///< Latch the watcher parks on.
        /// Borrowed (non-owning) pointer to the socket the watcher is
        /// parked on. Set by StartReadableWatcher; nulled by
        /// ShutdownWatcher after it triggers a Close to cancel the
        /// parked I/O. Lifetime: the connection owns the socket; the
        /// watcher is torn down via ShutdownWatcher BEFORE the
        /// connection destroys the socket.
        ISocket* _socket { nullptr };
        bool _readablePending { false }; ///< Watcher saw readable bytes.
        bool _watcherStarted { false };  ///< StartReadableWatcher ran.
        bool _shuttingDown { false };    ///< Connection torn down; watcher should exit.
    };

    /// Mutable per-connection protocol state, owned by RedisRespHandler::Run's
    /// coroutine frame and threaded by reference into Dispatch and the command
    /// handlers. It lives for the whole session, so a reference is stable across
    /// the suspensions inside the command loop.
    struct ConnectionState
    {
        /// Negotiated protocol version; flipped to Resp3 by a successful HELLO 3.
        RespVersion resp { RespVersion::Resp2 };

        /// True once the client has authenticated (or up-front when no
        /// credential is required). AUTH / HELLO ... AUTH flip this.
        bool authenticated { false };

        /// The connection's subscriber, created lazily on the first SUBSCRIBE.
        /// Null until then; non-null means pub/sub is in use on this connection.
        /// Held by shared_ptr so the registry's weak_ptr upgrade in Publish
        /// can safely pin the subscriber's lifetime past the Run frame's
        /// teardown (the classic UAF: snapshot → cleanup → Deliver on a freed
        /// subscriber).
        std::shared_ptr<Subscriber> subscriber {};

        /// Number of active channel + pattern subscriptions. >0 means the
        /// connection is in "subscribe mode" (restricted command set).
        std::size_t subscriptionCount { 0 };

        /// True between a successful `MULTI` and the matching `EXEC`/`DISCARD`.
        /// While set, every non-transaction-control command is queued instead
        /// of executed, and the dispatcher replies `+QUEUED` to the client.
        bool inMulti { false };

        /// Set when a queued command produced a parse-time error (unknown
        /// command, wrong arity by table lookup). Mirrors redis: a dirty
        /// transaction must abort on EXEC with `-EXECABORT`. Reset by DISCARD
        /// and after EXEC.
        bool multiDirty { false };

        /// One queued command: the full argv (verb + args, owned strings) and
        /// the CommandTable index resolved at queue time. EXEC dispatches
        /// directly via `CommandTable[commandTableIdx].handler` and skips
        /// the entire Dispatch prologue (Upper, auth lookup, command-table
        /// scan, arity re-check), all of which already ran when the command
        /// was queued.
        ///
        /// Invariant: `argv` MUST be non-empty (size >= 1). HandleExec's
        /// replay computes a `tail` span as `{ argv.data() + 1, argv.size()
        /// - 1 }`; an empty argv would underflow `size() - 1` to
        /// `size_t(-1)` and the replay would walk all addressable memory.
        /// Today the arity check in Dispatch guarantees the invariant
        /// before the push_back; this static_assert-style guard prevents a
        /// future bypass (savepoint replay, script verb, test code) from
        /// reintroducing the underflow silently.
        struct QueuedCommand
        {
            std::vector<std::string> argv;
            std::size_t commandTableIdx;
        };

        /// Commands enqueued under `MULTI`. Each entry carries its argv plus
        /// the cached command-table index. Played back in FIFO order on
        /// EXEC, with the responses framed as a single multi-bulk.
        std::vector<QueuedCommand> queue {};

        /// Total bytes of `queue` argv payload. Tracked separately from
        /// `queue.size()` so the cap check is O(1) without re-walking the
        /// queue. Reset alongside `queue.clear()` on DISCARD / EXEC / RESET.
        std::size_t queueBytes { 0 };

        /// Per-connection caps on the MULTI queue. Defaults to the
        /// module-level `MaxQueuedCommands` / `MaxQueuedBytes`. Tests inject
        /// smaller values via the test-only seam below so they can exercise
        /// the breach path without allocating a real 256 MiB.
        std::size_t maxQueuedCommands { MaxQueuedCommands };
        std::size_t maxQueuedBytes { MaxQueuedBytes };

        /// Per-connection WATCH state: snapshots taken by `WATCH` and a dirty
        /// flag that the WatchRegistry's mutation hook flips from any reactor
        /// thread. Created lazily on the first WATCH; null otherwise. Held by
        /// shared_ptr so the registry's weak_ptr upgrade in Touched can pin
        /// its lifetime past a concurrent connection teardown (mirrors the
        /// Subscriber pattern above).
        std::shared_ptr<WatchHandle> watch {};

        /// Cached pointer to the process-wide WATCH registry, copied from
        /// `SessionContext::watches` once at the start of `Run`. Null when
        /// transactions are not wired in. Caching here lets every write-verb
        /// handler call `NotifyWatchers(state, key)` without threading the
        /// SessionContext through their signatures.
        WatchRegistry* watchRegistry { nullptr };

        /// Cached pointer to the process-wide keyspace-notification helper,
        /// copied from `SessionContext::keyspaceNotifier` once at the start
        /// of `Run`. Null when no notifier is wired in (tests).
        KeyspaceNotifier* keyspaceNotifier { nullptr };

        /// `keyspaceNotifier != nullptr && keyspaceNotifier->IsEnabled()`,
        /// captured once at the start of `Run` and read on the hot path by
        /// `NotifyKeyspace` to skip the helper's indirect call entirely when
        /// keyspace notifications are off. Documented as fixed-for-life:
        /// flipping `notify-keyspace-events` mid-session does NOT affect
        /// connections already running — they keep their captured value
        /// until they exit. This is deliberate (same shape as `watchRegistry`
        /// and `keyspaceNotifier` caching) and lets the hottest write-path
        /// branch on a per-connection bool instead of chasing a notifier
        /// pointer through ConnectionState.
        bool keyspaceEnabled { false };
    };

    /// Mutation hook: every Redis write verb calls this after its engine
    /// mutation succeeds, so any `WATCH` snapshot on the touched key flips
    /// the watcher's dirty flag and a subsequent `EXEC` aborts. Centralised
    /// so adding a new write verb only has to call one helper.
    /// @param state Per-connection state (carries the cached registry pointer).
    /// @param key   Key just mutated.
    inline void NotifyWatchers(ConnectionState const* state, std::string_view key) noexcept
    {
        if (state != nullptr && state->watchRegistry != nullptr)
            (void) state->watchRegistry->Touched(key);
    }

    /// Keyspace-notification hook: same call sites as `NotifyWatchers`, fires
    /// `__keyspace@0__:<key> <event>` and/or `__keyevent@0__:<event> <key>`
    /// depending on the configured bitmask.
    /// @param state     Per-connection state (carries the cached notifier).
    /// @param classFlag One of `KeyspaceEvents::Generic` / `String` / `Expired`.
    /// @param event     The wire event name (e.g. "set", "del", "expire").
    /// @param key       The key the mutation applied to.
    inline void NotifyKeyspace(ConnectionState const* state,
                               std::uint32_t classFlag,
                               std::string_view event,
                               std::string_view key)
    {
        // Read the per-connection cached enable bit FIRST so the disabled
        // case (the daemon-wide default) costs one branch on a hot field,
        // no indirect call. Without this, every successful cache mutation
        // dereferenced `state->keyspaceNotifier`, called through the
        // notifier's vtable... ish path, and then bottomed out at
        // `_classes == 0` inside the helper.
        if (state == nullptr || !state->keyspaceEnabled)
            return;
        state->keyspaceNotifier->OnEvent(classFlag, event, key);
    }

    [[nodiscard]] std::string Upper(std::string_view sv)
    {
        std::string out { sv };
        for (auto& c: out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    template <typename T>
    [[nodiscard]] bool ParseUnsigned(std::string_view sv, T& out) noexcept
    {
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return ec == std::errc {} && ptr == sv.data() + sv.size();
    }

    template <typename T>
    [[nodiscard]] bool ParseSigned(std::string_view sv, T& out) noexcept
    {
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return ec == std::errc {} && ptr == sv.data() + sv.size();
    }

    /// Parse a finite double from `sv` in a locale-independent way, rejecting
    /// trailing garbage and non-finite inputs.
    ///
    /// Implementation: validate the byte sequence against the redis numeric
    /// grammar (`[-+]?[0-9]*(\.[0-9]+)?([eE][-+]?[0-9]+)?`) first, then parse
    /// the canonicalised text with `std::istringstream` pinned to the classic
    /// C locale via `imbue(std::locale::classic())`. That side-steps the
    /// global-LC_NUMERIC hazard that would otherwise let a non-`.`-LC_NUMERIC
    /// host (e.g. de_DE.UTF-8) reject the wire format the daemon's own
    /// `std::format` writes — and is portable across POSIX and Windows
    /// (unlike `uselocale`).
    ///
    /// We don't use `std::from_chars<double>` because the floating-point
    /// from_chars overload is unavailable on some libc++ versions (macOS
    /// before 26.0). Once the project floor catches up, this collapses
    /// to a single from_chars call.
    /// @param sv  Text to parse.
    /// @param out Receives the parsed value on success.
    /// @return True iff the whole string is a finite double.
    [[nodiscard]] bool ParseDouble(std::string_view sv, double& out) noexcept
    {
        if (sv.empty())
            return false;
        // Locale-neutral pre-validation: every byte must be from the redis
        // numeric grammar. Catches embedded NUL, whitespace, and any
        // locale-specific decimal separators (',') the daemon doesn't speak.
        bool sawDot = false;
        bool sawExp = false;
        bool sawDigit = false;
        for (auto const i: std::views::iota(std::size_t { 0 }, sv.size()))
        {
            auto const c = sv[i];
            if (c >= '0' && c <= '9')
                sawDigit = true;
            else if (c == '.')
            {
                if (sawDot || sawExp)
                    return false;
                sawDot = true;
            }
            else if (c == 'e' || c == 'E')
            {
                if (sawExp || !sawDigit)
                    return false;
                sawExp = true;
                sawDigit = false; // exponent must have its own digits
            }
            else if (c == '+' || c == '-')
            {
                if (i != 0 && !(sawExp && (sv[i - 1] == 'e' || sv[i - 1] == 'E')))
                    return false;
            }
            else
                return false;
        }
        if (!sawDigit)
            return false;

        // The validator guarantees ASCII; pin the stream to the classic
        // (C) locale so a non-`.`-LC_NUMERIC host (e.g. de_DE.UTF-8)
        // still parses the wire format the daemon emits. Portable across
        // POSIX and Windows. One istringstream construction per call —
        // not on any hot path (INCRBYFLOAT only).
        try
        {
            std::istringstream iss { std::string { sv } };
            iss.imbue(std::locale::classic());
            double value = 0;
            iss >> value;
            if (iss.fail() || !iss.eof() || !std::isfinite(value))
                return false;
            out = value;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    Task<bool> WriteAll(ISocket* socket, std::string_view payload)
    {
        if (payload.empty())
            co_return true;
        auto const r = co_await socket->Write(AsBytes(payload));
        co_return r.has_value();
    }

    /// Gather-write ordered segments as one reply, pinning the payload owner
    /// across a possibly-suspending write and verifying the full byte count.
    /// @param socket    Destination socket.
    /// @param segments  Ordered, non-owning views to gather.
    /// @param keepAlive Optional owner pinning the segments' backing storage.
    /// @return True if every byte was written.
    Task<bool> WriteAllVectored(ISocket* socket,
                                std::span<std::span<std::byte const> const> segments,
                                std::shared_ptr<void const> keepAlive = {})
    {
        std::size_t expected = 0;
        for (auto const seg: segments)
            expected += seg.size();
        if (expected == 0)
            co_return true;
        auto const r = co_await socket->WriteVectored(segments, std::move(keepAlive));
        co_return r.has_value() && *r == expected;
    }

    Task<bool> ReplyOk(ISocket* socket)
    {
        co_return co_await WriteAll(socket, "+OK\r\n");
    }
    Task<bool> ReplyPong(ISocket* socket)
    {
        co_return co_await WriteAll(socket, "+PONG\r\n");
    }

    /// Null reply. RESP3 has a dedicated null type (`_\r\n`); RESP2 spells it as
    /// a null bulk string (`$-1\r\n`). Callers that need a null *array* in RESP2
    /// (`*-1`) should special-case it; for fastcached's commands the bulk form
    /// is always the correct RESP2 spelling.
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyNull(ISocket* socket, RespVersion resp)
    {
        co_return co_await WriteAll(socket, resp == RespVersion::Resp3 ? "_\r\n" : "$-1\r\n");
    }

    Task<bool> ReplyInteger(ISocket* socket, std::int64_t value)
    {
        co_return co_await WriteAll(socket, std::format(":{}\r\n", value));
    }

    /// Boolean reply. RESP3 has a native boolean (`#t`/`#f`); RESP2 has no
    /// boolean type, so the redis convention is an integer 1/0.
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyBoolean(ISocket* socket, bool value, RespVersion resp)
    {
        if (resp == RespVersion::Resp3)
            co_return co_await WriteAll(socket, value ? "#t\r\n" : "#f\r\n");
        co_return co_await WriteAll(socket, value ? ":1\r\n" : ":0\r\n");
    }

    /// Double reply. RESP3 has a native double (`,<d>\r\n`, with `inf`/`-inf`/
    /// `nan` spellings); RESP2 returns the formatted number as a bulk string.
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyDouble(ISocket* socket, double value, RespVersion resp)
    {
        // RESP3 normalises the non-finite values to inf/-inf/nan (no sign on nan).
        std::string text;
        if (value != value) // NaN
            text = "nan";
        else if (value == std::numeric_limits<double>::infinity())
            text = "inf";
        else if (value == -std::numeric_limits<double>::infinity())
            text = "-inf";
        else
            text = std::format("{}", value);

        if (resp == RespVersion::Resp3)
            co_return co_await WriteAll(socket, std::format(",{}\r\n", text));
        co_return co_await WriteAll(socket, std::format("${}\r\n{}\r\n", text.size(), text));
    }

    /// Big-number reply (an integer outside the signed-64-bit range, given as a
    /// decimal string). RESP3 has a native big number (`(<n>\r\n`); RESP2 falls
    /// back to a bulk string. The caller is responsible for the digits.
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyBigNumber(ISocket* socket, std::string_view digits, RespVersion resp)
    {
        if (resp == RespVersion::Resp3)
            co_return co_await WriteAll(socket, std::format("({}\r\n", digits));
        co_return co_await WriteAll(socket, std::format("${}\r\n{}\r\n", digits.size(), digits));
    }

    /// Verbatim-string reply (human-readable text with a 3-char format hint).
    /// RESP3 prefixes the payload with `<fmt>:` inside a `=`-typed string; RESP2
    /// falls back to a plain bulk string (the format hint is dropped).
    /// @param fmt  Three-character format code, e.g. "txt" or "mkd".
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyVerbatim(ISocket* socket, std::string_view fmt, std::string_view text, RespVersion resp)
    {
        if (resp == RespVersion::Resp3)
            co_return co_await WriteAll(socket, std::format("={}\r\n{}:{}\r\n", fmt.size() + 1 + text.size(), fmt, text));
        co_return co_await WriteAll(socket, std::format("${}\r\n{}\r\n", text.size(), text));
    }

    Task<bool> ReplyError(ISocket* socket, std::string_view detail)
    {
        co_return co_await WriteAll(socket, std::format("-ERR {}\r\n", detail));
    }

    /// Aggregate-header writer for arrays/maps/sets/pushes. RESP3 distinguishes
    /// map (`%`), set (`~`) and push (`>`) from array (`*`); RESP2 flattens all
    /// of them to an array — a map of N pairs becomes a `*<2N>` array, every
    /// other aggregate a `*<count>` array.
    enum class Aggregate : std::uint8_t
    {
        Array,
        Map,
        Set,
        Push,
    };

    /// Emit the aggregate header line for `count` logical elements (for a Map,
    /// `count` is the number of key/value PAIRS). The element bodies are written
    /// by the caller afterwards.
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyAggregateHeader(ISocket* socket, Aggregate kind, std::size_t count, RespVersion resp)
    {
        if (resp == RespVersion::Resp3)
        {
            auto const prefix = [kind] {
                switch (kind)
                {
                    case Aggregate::Map:
                        return '%';
                    case Aggregate::Set:
                        return '~';
                    case Aggregate::Push:
                        return '>';
                    case Aggregate::Array:
                        break;
                }
                return '*';
            }();
            co_return co_await WriteAll(socket, std::format("{}{}\r\n", prefix, count));
        }
        // RESP2: arrays only. A map serialises as a flat array of 2N elements.
        auto const elements = kind == Aggregate::Map ? count * 2 : count;
        co_return co_await WriteAll(socket, std::format("*{}\r\n", elements));
    }

    /// Emit a RESP3 attribute header (`|<pairs>`) for `count` key/value pairs.
    /// An attribute is auxiliary metadata that PREFIXES the actual reply: the
    /// caller writes the pairs, then the real reply value. RESP2 has no attribute
    /// type, so under RESP2 this writes nothing and the caller's reply stands
    /// alone (attributes are purely advisory).
    /// @param resp The connection's negotiated protocol version.
    Task<bool> ReplyAttributeHeader(ISocket* socket, std::size_t count, RespVersion resp)
    {
        if (resp != RespVersion::Resp3)
            co_return true;
        co_return co_await WriteAll(socket, std::format("|{}\r\n", count));
    }

    Task<bool> ReplyBulkString(ISocket* socket, std::span<std::byte const> bytes, std::shared_ptr<void const> keepAlive = {})
    {
        // Gather `$<len>\r\n` + bytes + `\r\n` into one scattered write: the
        // value segment points directly at the cached payload (no copy), and
        // `keepAlive` keeps that payload alive across a write that may suspend.
        // `header` lives in this coroutine frame (suspended, not destroyed,
        // across the co_await), so its address is stable. The hot path (GET) is
        // syscall-bound, so one sendmsg per reply beats three send()s.
        auto const header = std::format("${}\r\n", bytes.size());
        std::array<std::span<std::byte const>, 3> const segments {
            AsBytes(header),
            bytes,
            AsBytes(Crlf),
        };
        co_return co_await WriteAllVectored(socket, segments, std::move(keepAlive));
    }

    Task<bool> ReplyBulkString(ISocket* socket, std::string_view text)
    {
        co_return co_await ReplyBulkString(socket, AsBytes(text));
    }

    /// Read one RESP value as a string. Supports the inline-command grammar
    /// (whitespace-separated tokens) when the first byte is not `*` — many
    /// Redis clients send the simpler form first.
    struct ParsedCommand
    {
        std::vector<std::string> args;
    };

    using ReadCommandResult = std::expected<ParsedCommand, ProtocolError>;

    /// Read a `$<len>\r\n<bytes>\r\n` bulk string argument from the reader->
    Task<std::expected<std::string, ProtocolError>> ReadBulkArg(ByteReader* reader)
    {
        auto const header = co_await reader->ReadLine();
        if (!header.has_value())
            co_return std::unexpected(header.error());
        auto const& line = *header;
        if (line.empty() || line[0] != '$')
            co_return std::unexpected(
                ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "expected $<len>" });
        std::int64_t len = 0;
        if (!ParseSigned(std::string_view { line }.substr(1), len) || len < 0)
            co_return std::unexpected(
                ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "bad bulk length" });

        auto const bytes = co_await reader->ReadExactly(static_cast<std::size_t>(len));
        if (!bytes.has_value())
            co_return std::unexpected(bytes.error());
        // Trailing CRLF.
        auto const crlf = co_await reader->ReadExactly(2);
        if (!crlf.has_value())
            co_return std::unexpected(crlf.error());
        co_return std::string { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
    }

    Task<ReadCommandResult> ReadOneCommand(ByteReader* reader)
    {
        auto const first = co_await reader->ReadLine();
        if (!first.has_value())
            co_return std::unexpected(first.error());
        auto const& line = *first;
        if (line.empty())
            co_return ParsedCommand {};

        if (line[0] != '*')
        {
            // Inline command: whitespace-split.
            ParsedCommand cmd;
            std::size_t i = 0;
            std::string_view const sv { line };
            while (i < sv.size())
            {
                while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t'))
                    ++i;
                if (i == sv.size())
                    break;
                auto const start = i;
                while (i < sv.size() && sv[i] != ' ' && sv[i] != '\t')
                    ++i;
                cmd.args.emplace_back(sv.substr(start, i - start));
            }
            co_return cmd;
        }

        std::int64_t count = 0;
        if (!ParseSigned(std::string_view { line }.substr(1), count) || count <= 0)
            co_return std::unexpected(
                ProtocolError { .code = ProtocolErrorCode::MalformedFrame, .context = "bad array length" });

        ParsedCommand cmd;
        cmd.args.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i)
        {
            auto arg = co_await ReadBulkArg(reader);
            if (!arg.has_value())
                co_return std::unexpected(arg.error());
            cmd.args.push_back(std::move(*arg));
        }
        co_return cmd;
    }

    /// Family-tagged option representation for SET. Existence (NX vs XX vs
    /// neither) and Expiry (EX vs PX vs neither) are independent families;
    /// each option from a family is mutually exclusive with the others
    /// in its family, so they're modelled as enums + optional rather
    /// than parallel booleans / integers. Adding KEEPTTL / GET / IDLE is
    /// a new field, not a new branch.
    enum class Existence : std::uint8_t
    {
        Any,           ///< No NX / XX clause.
        OnlyIfAbsent,  ///< NX
        OnlyIfPresent, ///< XX
    };

    struct SetOptions
    {
        Existence existence { Existence::Any };
        /// Absolute deadline when the SET carries an EX/PX clause;
        /// nullopt = no TTL. Sub-second precision preserved (the wire
        /// PX path goes through this end-to-end, not the lossy
        /// (raw+999)/1000 path the prior implementation forced).
        std::optional<TimePoint> deadline {};
    };

    /// Redis caps TTL inputs at INT32_MAX for the seconds form and the
    /// corresponding ms value for the millisecond form. Values outside
    /// that range — including the silent `0` and the silently-wrapping
    /// UINT64_MAX-998 — reply `-ERR invalid expire time`.
    constexpr std::uint64_t MaxTtlSeconds = std::numeric_limits<std::int32_t>::max();
    constexpr std::uint64_t MaxTtlMillis = static_cast<std::uint64_t>(MaxTtlSeconds) * 1000ULL;

    /// Parse the optional NX/XX/EX/PX clauses from a SET tail. Returns the
    /// human-readable Redis error string on the first violation: an unknown
    /// token, a missing argument, an EX/PX without a number, a TTL of 0,
    /// a TTL above the documented Redis cap, or two options from the same
    /// family (NX+XX, EX+PX).
    /// @param tail  Args after `SET key value` (so opts[0] is the first flag).
    /// @param clock Source of the current monotonic clock (to anchor PX/EX
    ///        into an absolute deadline at parse time — one clock read per
    ///        parse, deterministic under ManualClock).
    [[nodiscard]] std::expected<SetOptions, std::string> ParseSetOptions(std::span<std::string const> tail, IClock& clock)
    {
        SetOptions opts;
        for (std::size_t i = 0; i < tail.size(); ++i)
        {
            auto const tok = Upper(tail[i]);
            if (tok == "NX")
            {
                if (opts.existence != Existence::Any)
                    return std::unexpected(std::string { "syntax error" });
                opts.existence = Existence::OnlyIfAbsent;
            }
            else if (tok == "XX")
            {
                if (opts.existence != Existence::Any)
                    return std::unexpected(std::string { "syntax error" });
                opts.existence = Existence::OnlyIfPresent;
            }
            else if (tok == "EX" || tok == "PX")
            {
                if (opts.deadline.has_value())
                    return std::unexpected(std::string { "syntax error" });
                if (i + 1 >= tail.size())
                    return std::unexpected(std::string { "syntax error" });
                std::uint64_t raw = 0;
                if (!ParseUnsigned(std::string_view { tail[i + 1] }, raw))
                    return std::unexpected(std::string { "value is not an integer or out of range" });
                bool const millis = (tok == "PX");
                auto const cap = millis ? MaxTtlMillis : MaxTtlSeconds;
                if (raw == 0 || raw > cap)
                    return std::unexpected(std::string { "invalid expire time in 'set'" });
                opts.deadline =
                    millis ? clock.Now() + std::chrono::milliseconds { raw } : clock.Now() + std::chrono::seconds { raw };
                ++i;
            }
            else
                return std::unexpected("syntax error");
        }
        return opts;
    }

    /// Standard redis WRONGTYPE error reply, emitted when a string command lands
    /// on a key that holds a different type (e.g. a set) and vice versa.
    Task<bool> ReplyWrongType(ISocket* socket)
    {
        co_return co_await WriteAll(socket, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
    }

    Task<bool> HandleGet(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'get'");
        auto const result = engine->Get(args[0]);
        if (!result.has_value() || !result->found)
            co_return co_await ReplyNull(socket, resp);
        // A GET against a set (or any non-string type) is a WRONGTYPE error.
        if (SetCodec::IsSet(result->entry.flags))
            co_return co_await ReplyWrongType(socket);
        co_return co_await ReplyBulkString(socket, result->entry.ValueBytes(), result->entry.value.AsKeepAlive());
    }

    Task<bool> HandleSet(
        ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'set'");

        auto const& key = args[0];
        auto const& value = args[1];
        auto const opts = ParseSetOptions(args.subspan(2), engine->Clock());
        if (!opts.has_value())
            co_return co_await ReplyError(socket, opts.error());

        auto bytes = BytesFromString(value);
        // Deadline is either the absolute TimePoint the EX/PX clause
        // anchored at parse time, or TimePoint::max() (no TTL).
        auto const deadline = opts->deadline.value_or(TimePoint::max());

        std::expected<CasToken, StorageError> result { 0 };
        switch (opts->existence)
        {
            case Existence::OnlyIfAbsent:
                result = engine->AddWithDeadline(key, std::move(bytes), 0, deadline);
                break;
            case Existence::OnlyIfPresent:
                result = engine->ReplaceWithDeadline(key, std::move(bytes), 0, deadline);
                break;
            case Existence::Any:
                result = engine->SetWithDeadline(key, std::move(bytes), 0, deadline);
                break;
        }

        if (result.has_value())
        {
            NotifyWatchers(state, key);
            NotifyKeyspace(state, KeyspaceEvents::String, "set", key);
            co_return co_await ReplyOk(socket);
        }
        if (result.error().code == StorageErrorCode::KeyExists || result.error().code == StorageErrorCode::KeyNotFound)
            co_return co_await ReplyNull(socket, resp); // NX/XX precondition unmet.
        if (result.error().code == StorageErrorCode::ValueTooLarge)
            co_return co_await ReplyError(socket, "value too large");
        co_return co_await ReplyError(socket, "storage failure");
    }

    Task<bool> HandleSetEx(
        ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args, bool millis)
    {
        if (args.size() != 3)
            co_return co_await ReplyError(
                socket, millis ? "wrong number of arguments for 'psetex'" : "wrong number of arguments for 'setex'");
        std::uint64_t raw = 0;
        if (!ParseUnsigned(std::string_view { args[1] }, raw))
            co_return co_await ReplyError(socket, "value is not an integer or out of range");
        auto const cap = millis ? MaxTtlMillis : MaxTtlSeconds;
        if (raw == 0 || raw > cap)
            co_return co_await ReplyError(socket,
                                          millis ? "invalid expire time in 'psetex'" : "invalid expire time in 'setex'");

        auto bytes = BytesFromString(args[2]);
        // Route through the deadline-bearing seam so PSETEX keeps its
        // wire-supplied millisecond precision (the prior path forced
        // ceiling-division to whole seconds, turning `PSETEX k 50 v`
        // into a 1-second TTL).
        auto const deadline = millis ? engine->Clock().Now() + std::chrono::milliseconds { raw }
                                     : engine->Clock().Now() + std::chrono::seconds { raw };
        auto const result = engine->SetWithDeadline(args[0], std::move(bytes), 0, deadline);
        if (!result.has_value())
            co_return co_await ReplyError(socket, "storage failure");
        NotifyWatchers(state, args[0]);
        NotifyKeyspace(state, KeyspaceEvents::String, "set", args[0]);
        co_return co_await ReplyOk(socket);
    }

    /// Time unit a TTL command's wire word is denominated in. Drives the
    /// integer conversions for EXPIRE (seconds) vs PEXPIRE (milliseconds)
    /// and TTL/PTTL.
    enum class TtlUnit : std::uint8_t
    {
        Seconds,
        Milliseconds,
    };

    /// Saturating subtraction on signed-64-bit: returns the actual result
    /// when representable, otherwise INT64_MAX / INT64_MIN. Used to compute
    /// `absoluteTimestamp - now` for EXPIREAT/PEXPIREAT without UB on
    /// arbitrary client input (a wire-supplied INT64_MIN would otherwise
    /// trip signed-integer underflow).
    /// @param a Left operand.
    /// @param b Right operand.
    /// @return `a - b` clamped to the int64 range.
    [[nodiscard]] constexpr std::int64_t SaturatingSub(std::int64_t a, std::int64_t b) noexcept
    {
        constexpr auto Max = std::numeric_limits<std::int64_t>::max();
        constexpr auto Min = std::numeric_limits<std::int64_t>::min();
        // Portable overflow detection (MSVC lacks __builtin_sub_overflow): the
        // guard operands are themselves chosen to never overflow — `Max + b`
        // with b < 0 and `Min + b` with b > 0 both stay in range.
        if (b < 0 && a > Max + b)
            return Max; // a - b would exceed the positive range.
        if (b > 0 && a < Min + b)
            return Min; // a - b would underflow the negative range.
        return a - b;
    }

    /// Resolve a possibly-out-of-range relative delta into a steady-clock
    /// deadline, clamping to `TimePoint::max()` rather than allowing chrono
    /// duration overflow (a wire-supplied INT64_MAX in nanoseconds via
    /// `std::chrono::seconds{INT64_MAX}` would otherwise wrap into the
    /// distant past). Negative or zero deltas resolve to `now` (immediate
    /// expiry — matches the Redis "TTL in the past deletes" semantics).
    /// @param now   Current monotonic clock.
    /// @param delta Wire-unit count (seconds or milliseconds).
    /// @param unit  Which unit `delta` is denominated in.
    /// @return The clamped steady-clock deadline.
    [[nodiscard]] TimePoint DeadlineFromDelta(TimePoint now, std::int64_t delta, TtlUnit unit) noexcept
    {
        if (delta <= 0)
            return now;
        // Cap delta to a value that, when scaled into the steady_clock's
        // representation (nanoseconds on most hosts), cannot overflow the
        // duration's underlying int64. A 100-year offset is well past any
        // sane TTL and well inside the int64 range for nanoseconds.
        constexpr std::int64_t HundredYearsSeconds = 100LL * 365LL * 24LL * 60LL * 60LL;
        constexpr std::int64_t HundredYearsMillis = HundredYearsSeconds * 1000LL;
        auto const cap = unit == TtlUnit::Seconds ? HundredYearsSeconds : HundredYearsMillis;
        auto const clamped = delta > cap ? cap : delta;
        return unit == TtlUnit::Seconds ? now + std::chrono::seconds { clamped }
                                        : now + std::chrono::milliseconds { clamped };
    }

    /// EXPIRE / PEXPIRE / EXPIREAT / PEXPIREAT — relative or absolute TTL on
    /// an existing key. The four wire commands differ only by data (unit and
    /// absolute-vs-relative interpretation), so they share one handler.
    ///
    /// Redis EXPIREAT/PEXPIREAT take a UNIX timestamp; fastcached's
    /// monotonic clock has no notion of wall time, so the implementation
    /// approximates it by anchoring against std::chrono::system_clock once
    /// per call (mirrors the absolute-exptime path in CacheEngine).
    /// @param verb     One of "expire", "pexpire", "expireat", "pexpireat" — drives the
    ///                 wrong-arg-count error string.
    /// @param unit     Seconds vs milliseconds for the TTL word.
    /// @param absolute When true, the TTL word is an absolute UNIX timestamp;
    ///                 otherwise it's an offset from `now`.
    Task<bool> HandleExpire(ISocket* socket,
                            CacheEngine* engine,
                            ConnectionState* state,
                            std::span<std::string const> args,
                            std::string_view verb,
                            TtlUnit unit,
                            bool absolute)
    {
        if (args.size() != 2)
            co_return co_await ReplyError(socket, std::format("wrong number of arguments for '{}'", verb));
        std::int64_t raw = 0;
        if (!ParseSigned(std::string_view { args[1] }, raw))
            co_return co_await ReplyError(socket, "value is not an integer or out of range");

        // Resolve the absolute steady-clock deadline. Redis semantics:
        //   relative <= 0 -> key is deleted immediately (TTL already elapsed)
        //   absolute in the past -> same
        // We achieve this by passing `now` to Touch, which sets the entry to
        // expire as soon as it is observed (the next operation purges it).
        auto& clock = engine->Clock();
        auto const now = clock.Now();

        TimePoint deadline;
        if (absolute)
        {
            // Anchor against the injected wall clock once per call;
            // absolute-vs-relative arithmetic is then `delta = ts - sysNow`,
            // mirroring CacheEngine::ExpiryFromExptime's translation for
            // memcached absolute exptimes. SaturatingSub guards against UB
            // when `raw` is the wire-supplied INT64_MIN — a plain signed
            // subtraction would underflow. Going through the injected
            // IWallClock (not std::chrono::system_clock::now() directly)
            // lets tests drive the absolute branch deterministically via
            // ManualWallClock.
            auto const sysNow = engine->WallClock().Now().time_since_epoch();
            auto const sysWord = unit == TtlUnit::Seconds
                                     ? std::chrono::duration_cast<std::chrono::seconds>(sysNow).count()
                                     : std::chrono::duration_cast<std::chrono::milliseconds>(sysNow).count();
            auto const delta = SaturatingSub(raw, sysWord);
            deadline = DeadlineFromDelta(now, delta, unit);
        }
        else
        {
            deadline = DeadlineFromDelta(now, raw, unit);
        }

        auto const result = engine->TouchAt(args[0], deadline);
        if (result.has_value())
        {
            NotifyWatchers(state, args[0]);
            // Redis fires the verb-named event (one of expire / pexpire /
            // expireat / pexpireat) under the `g` (generic) class.
            NotifyKeyspace(state, KeyspaceEvents::Generic, verb, args[0]);
            co_return co_await ReplyInteger(socket, 1);
        }
        if (result.error().code == StorageErrorCode::KeyNotFound)
            co_return co_await ReplyInteger(socket, 0);
        co_return co_await ReplyError(socket, "storage failure");
    }

    /// TTL / PTTL — remaining time on the entry under `key`.
    /// Reply matches redis exactly: `-2` if the key is missing, `-1` if it
    /// exists with no TTL, otherwise the remaining time in the requested unit.
    /// Reads via `IStorage::PeekExpiry`, which does NOT bump LRU recency or
    /// `lastAccess` — a TTL probe must not be observable as a client access,
    /// or polling for expiry would keep the entry alive forever.
    Task<bool> HandleTtl(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, TtlUnit unit)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket,
                                          unit == TtlUnit::Seconds ? "wrong number of arguments for 'ttl'"
                                                                   : "wrong number of arguments for 'pttl'");
        auto const result = engine->Ttl(args[0]);
        if (!result.has_value())
            co_return co_await ReplyError(socket, "storage failure");
        if (!result->has_value())
            co_return co_await ReplyInteger(socket, -2); // missing
        if (!(*result)->hasExpiry)
            co_return co_await ReplyInteger(socket, -1); // present, no TTL
        auto const remaining = (*result)->remaining;
        auto const word = unit == TtlUnit::Seconds
                              ? std::chrono::duration_cast<std::chrono::seconds>(remaining).count()
                              : std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        co_return co_await ReplyInteger(socket, static_cast<std::int64_t>(word));
    }

    /// PERSIST key — remove any TTL on `key`, leaving the value in place.
    /// Reply: 1 if a TTL was actually cleared (the key existed and had a
    /// TTL), 0 otherwise (key absent OR key existed with no TTL — redis
    /// returns 0 in both no-cleared cases). Routes through
    /// `IStorage::ClearExpiry`, an atomic Peek+Touch primitive that
    /// closes the prior TOCTOU window — a concurrent SETEX between the
    /// separate Ttl read and TouchAt write could let PERSIST clear the
    /// new TTL while reporting :1, or report :0 against a stale view.
    Task<bool> HandlePersist(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'persist'");
        auto const result = engine->ClearExpiry(args[0]);
        if (result.has_value())
        {
            // Only an actual TTL-clearing transition counts as a mutation
            // for WATCH purposes: a no-op PERSIST on a TTL-less key does
            // not bump CAS in the storage layer either.
            if (*result)
            {
                NotifyWatchers(state, args[0]);
                NotifyKeyspace(state, KeyspaceEvents::Generic, "persist", args[0]);
            }
            co_return co_await ReplyInteger(socket, *result ? 1 : 0);
        }
        // KeyNotFound -> :0 (matches redis behaviour for absent keys).
        if (result.error().code == StorageErrorCode::KeyNotFound)
            co_return co_await ReplyInteger(socket, 0);
        co_return co_await ReplyError(socket, "storage failure");
    }

    Task<bool> HandleDel(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'del'");
        std::int64_t deleted = 0;
        // NotifyWatchers / NotifyKeyspace each have a lock-free fast path
        // (atomic _entryCount==0 / WouldPublish gate) so a per-key call
        // against an empty registry is a single atomic load. The previous
        // shape hoisted HasAnyWatchers() / WouldPublish() ONCE before the
        // loop — under multi-reactor, a WATCH issued on another reactor
        // between the hoist and a later key's mutation made that mutation
        // skip NotifyWatchers, letting a racing EXEC commit over a write
        // that should have aborted it. Per-key probes close the race at no
        // measurable steady-state cost.
        for (auto const& key: args)
        {
            auto const result = engine->Delete(key);
            if (result.has_value())
            {
                ++deleted;
                NotifyWatchers(state, key);
                NotifyKeyspace(state, KeyspaceEvents::Generic, "del", key);
            }
        }
        co_return co_await ReplyInteger(socket, deleted);
    }

    Task<bool> HandleExists(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'exists'");
        std::int64_t found = 0;
        for (auto const& key: args)
        {
            auto const result = engine->Get(key);
            if (result.has_value() && result->found)
                ++found;
        }
        co_return co_await ReplyInteger(socket, found);
    }

    Task<bool> HandlePing(ISocket* socket, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyPong(socket);
        co_return co_await ReplyBulkString(socket, args[0]);
    }

    Task<bool> HandleEcho(ISocket* socket, std::span<std::string const> args)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'echo'");
        co_return co_await ReplyBulkString(socket, args[0]);
    }

    Task<bool> HandleInfo(ISocket* socket, CacheEngine* engine, RespVersion resp)
    {
        auto const stats = engine->Snapshot();
        auto const body = std::format("# Server\r\nfastcached_version:{}\r\nredis_version:6.0.0-fastcached\r\n"
                                      "# Memory\r\nused_memory:{}\r\nmaxmemory:{}\r\n"
                                      "# Stats\r\ntotal_commands_processed:{}\r\nkeyspace_hits:{}\r\nkeyspace_misses:{}\r\n",
                                      RedisRespHandler::ServerVersion(),
                                      stats.bytesUsed,
                                      stats.bytesLimit,
                                      stats.cmdGet + stats.cmdSet,
                                      stats.getHits,
                                      stats.getMisses);
        // INFO is human-readable text: a verbatim string (txt) under RESP3, a
        // plain bulk string under RESP2.
        co_return co_await ReplyVerbatim(socket, "txt", body, resp);
    }

    /// One field of the HELLO handshake map. The value is either a string or an
    /// integer; `isInteger` selects which member is rendered.
    struct HelloField
    {
        std::string_view key;
        std::string_view stringValue;
        std::int64_t intValue;
        bool isInteger;
    };

    /// Write the HELLO handshake map (`server`, `version`, `proto`, `id`, `mode`,
    /// `role`) from a data-driven field table. Rendered as a RESP3 map (`%`) or a
    /// flat RESP2 array (`*`) per the negotiated version. Mirrors redis/valkey.
    /// @param resp The (already-negotiated) protocol version to render under.
    Task<bool> WriteHelloMap(ISocket* socket, RespVersion resp)
    {
        std::array<HelloField, 6> const fields { {
            { .key = "server", .stringValue = "fastcached", .intValue = 0, .isInteger = false },
            { .key = "version", .stringValue = ServerVersionBanner, .intValue = 0, .isInteger = false },
            { .key = "proto", .stringValue = {}, .intValue = resp == RespVersion::Resp3 ? 3 : 2, .isInteger = true },
            { .key = "id", .stringValue = {}, .intValue = 1, .isInteger = true },
            { .key = "mode", .stringValue = "standalone", .intValue = 0, .isInteger = false },
            { .key = "role", .stringValue = "master", .intValue = 0, .isInteger = false },
        } };
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Map, fields.size(), resp))
            co_return false;
        for (auto const& field: fields)
        {
            if (!co_await ReplyBulkString(socket, field.key))
                co_return false;
            auto const ok = field.isInteger ? co_await ReplyInteger(socket, field.intValue)
                                            : co_await ReplyBulkString(socket, field.stringValue);
            if (!ok)
                co_return false;
        }
        co_return true;
    }

    /// HELLO [<protover>] [AUTH <user> <pass>] [SETNAME <name>].
    ///
    /// Negotiates the RESP protocol version (2 or 3) and, on success, switches
    /// the connection via `state->resp`. An unsupported version replies
    /// `-NOPROTO`. The optional inline `AUTH <user> <pass>` clause authenticates
    /// the connection in the same round trip — this is the handshake modern redis
    /// clients (redis-py RESP3, go-redis, lettuce) send as their first command,
    /// so it must be honoured or those clients cannot connect to a
    /// password-protected server (mirrors valkey's helloCommand). When a
    /// credential is required and neither a prior AUTH nor a valid inline AUTH has
    /// authenticated the connection, HELLO replies `-NOAUTH` and does not switch
    /// protocol. `SETNAME` is accepted and ignored (no per-client name state).
    /// A bare `HELLO` (no args) keeps the current version.
    /// @param session Per-server collaborators (the auth policy).
    /// @param state   Per-connection state; `resp`/`authenticated` updated on
    ///        success (a pointer, not a reference, to satisfy the
    ///        coroutine-parameter lint; it points into Run's frame).
    Task<bool> HandleHello(ISocket* socket,
                           std::span<std::string const> args,
                           SessionContext session,
                           ConnectionState* state)
    {
        auto negotiated = state->resp;
        std::size_t next = 0;
        if (!args.empty() && !args[0].empty() && (std::isdigit(static_cast<unsigned char>(args[0][0])) != 0))
        {
            std::uint32_t ver = 0;
            if (!ParseUnsigned(std::string_view { args[0] }, ver) || ver < 2 || ver > 3)
                co_return co_await WriteAll(socket, "-NOPROTO unsupported protocol version\r\n");
            negotiated = ver == 3 ? RespVersion::Resp3 : RespVersion::Resp2;
            next = 1;
        }

        // Parse the optional AUTH / SETNAME option clauses.
        bool authAttempted = false;
        bool authOk = false;
        auto const auth = session.CurrentAuth();
        while (next < args.size())
        {
            auto const opt = Upper(args[next]);
            if (opt == "AUTH" && next + 2 < args.size())
            {
                // Only treat as an auth attempt when the server actually
                // requires a credential. Modern RESP3 clients (redis-py,
                // lettuce, go-redis) always send `HELLO 3 AUTH user pass`
                // as their handshake; rejecting it with -WRONGPASS against
                // a no-password server would lock them out of an otherwise
                // un-authed daemon.
                if (auth != nullptr && auth->Enabled())
                {
                    authAttempted = true;
                    authOk = auth->Verify(args[next + 1], args[next + 2]);
                }
                next += 3;
            }
            else if (opt == "SETNAME" && next + 1 < args.size())
            {
                next += 2; // accepted, ignored — no per-client name state.
            }
            else
            {
                co_return co_await ReplyError(socket, std::format("Syntax error in HELLO option '{}'", args[next]));
            }
        }

        bool const authEnabled = auth != nullptr && auth->Enabled();
        if (authAttempted)
        {
            if (!authOk)
                co_return co_await WriteAll(socket, "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
            state->authenticated = true;
        }

        // HELLO can be issued before authenticating, but the reply requires the
        // connection to be authenticated (matching password-protected redis).
        if (authEnabled && !state->authenticated)
            co_return co_await WriteAll(socket,
                                        "-NOAUTH HELLO must be called with the client already authenticated, "
                                        "otherwise the HELLO <proto> AUTH <user> <pass> option can be used to "
                                        "authenticate the client and select the RESP protocol version at the "
                                        "same time\r\n");

        state->resp = negotiated;
        co_return co_await WriteHelloMap(socket, negotiated);
    }

    /// Forward declarations so HandleCommand and its helpers can use the
    /// dispatch table for `COMMAND` introspection. Definitions are below
    /// (the table itself depends on the handler functions, so it can only
    /// be defined after them).
    extern std::size_t CommandTableSize() noexcept;
    extern std::string_view CommandTableName(std::size_t index) noexcept;
    extern std::int32_t CommandTableArity(std::size_t index) noexcept;
    extern std::int32_t CommandTableFirstKey(std::size_t index) noexcept;
    extern std::int32_t CommandTableLastKey(std::size_t index) noexcept;
    extern std::int32_t CommandTableKeyStep(std::size_t index) noexcept;

    /// Everything a command handler needs, bundled so the dispatch table can hold
    /// one uniform handler signature. Pointers borrow from Dispatch's frame /
    /// SessionContext and outlive the awaited handler. Defined here (not at the
    /// table's site below) so HandleExec can pass it by value into
    /// `CommandTableInvoke` for queue-replay.
    struct CommandContext
    {
        ISocket* socket;
        CacheEngine* engine;
        std::span<std::string const> tail; ///< Arguments after the command name.
        ConnectionState* state;
        SessionContext session; ///< Per-server collaborators (auth, pub/sub).
    };

    /// Invoke `CommandTable[index].handler(ctx)`. Used by HandleExec to
    /// bypass Dispatch's prologue when replaying queued commands — at
    /// queue time the table index, arity, and auth state were already
    /// validated, so EXEC just re-invokes the handler directly.
    [[nodiscard]] Task<bool> CommandTableInvoke(std::size_t index, CommandContext ctx);

    /// Find a command in the table by name (case-sensitive match against the
    /// canonical UPPER name in `CommandEntry`). Returns the index for
    /// `COMMAND INFO <name>` lookups, or std::nullopt if the caller passed
    /// an unknown name (which `COMMAND INFO` then renders as a RESP nil
    /// per redis convention).
    [[nodiscard]] std::optional<std::size_t> CommandTableFind(std::string_view upperName) noexcept;

    /// Emit one 6-element command descriptor from the table at `index`:
    /// `[name, arity, [flags], firstKey, lastKey, keyStep]`. Hoisted out
    /// of HandleCommand so the bare-COMMAND walk and the INFO-filtered
    /// walk emit identical wire shapes from one source.
    /// @param socket Destination socket.
    /// @param index  Row in the dispatch table to emit.
    /// @param resp   Negotiated RESP version.
    /// @return False on a socket write failure.
    Task<bool> WriteCommandDescriptor(ISocket* socket, std::size_t index, RespVersion resp)
    {
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, 6, resp))
            co_return false;
        if (!co_await ReplyBulkString(socket, CommandTableName(index)))
            co_return false;
        if (!co_await ReplyInteger(socket, CommandTableArity(index)))
            co_return false;
        // flags array — empty (no flag taxonomy modelled here).
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, 0, resp))
            co_return false;
        if (!co_await ReplyInteger(socket, CommandTableFirstKey(index)))
            co_return false;
        if (!co_await ReplyInteger(socket, CommandTableLastKey(index)))
            co_return false;
        co_return co_await ReplyInteger(socket, CommandTableKeyStep(index));
    }

    /// COMMAND [COUNT|DOCS|INFO|LIST|GETKEYS …]
    ///
    /// Real redis clients (jedis, lettuce, go-redis, redis-py) probe this
    /// at handshake to discover the server's command surface — a bare
    /// `COMMAND` returns an array of per-command descriptors, `COMMAND
    /// COUNT` returns the total. Each descriptor is a 6-element array:
    /// [name, arity, [flags], firstKey, lastKey, keyStep]. The flags
    /// array is empty here (we don't model flag categories yet); name
    /// comes from the table and the rest is read straight off the entry.
    ///
    /// Replying `*0` (the prior behaviour) made clients believe the
    /// daemon supported nothing and fall back to surprising defaults.
    Task<bool> HandleCommand(ISocket* socket, std::span<std::string const> args, RespVersion resp)
    {
        auto const sub = args.empty() ? std::string {} : Upper(args[0]);
        auto const total = CommandTableSize();

        if (sub == "COUNT")
            co_return co_await ReplyInteger(socket, static_cast<std::int64_t>(total));

        // `COMMAND DOCS` would emit a map per command; clients are happy
        // with an empty map here as long as it's well-formed.
        if (sub == "DOCS")
            co_return co_await ReplyAggregateHeader(socket, Aggregate::Map, 0, resp);

        // `COMMAND GETKEYS` reports the keys touched by an example command
        // line. We don't synthesise it here; the empty array is harmless.
        if (sub == "GETKEYS")
            co_return co_await ReplyAggregateHeader(socket, Aggregate::Array, 0, resp);

        // `COMMAND INFO name [name ...]` filters by the requested names,
        // emitting one descriptor per name (and RESP nil for unknown
        // names). Pre-fix this branch fell through to the bare-COMMAND
        // path and dumped descriptors for every command in the table,
        // so jedis/lettuce/redis-py probes that compare
        // `len(args[1:]) == len(reply)` saw a length mismatch.
        if (sub == "INFO" && args.size() > 1)
        {
            auto const requested = args.subspan(1);
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, requested.size(), resp))
                co_return false;
            for (auto const& name: requested)
            {
                auto const idx = CommandTableFind(Upper(name));
                if (!idx.has_value())
                {
                    if (!co_await ReplyNull(socket, resp))
                        co_return false;
                    continue;
                }
                if (!co_await WriteCommandDescriptor(socket, *idx, resp))
                    co_return false;
            }
            co_return true;
        }

        // Default — bare COMMAND or COMMAND INFO / LIST without specific
        // names: emit a flat array of per-command descriptors.
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, total, resp))
            co_return false;
        for (std::size_t i = 0; i < total; ++i)
            if (!co_await WriteCommandDescriptor(socket, i, resp))
                co_return false;
        co_return true;
    }

    Task<bool> HandleFlush(ISocket* socket, CacheEngine* engine, ConnectionState* state)
    {
        engine->FlushAll(0);
        // FLUSHDB / FLUSHALL invalidate every WATCH'd key on every
        // connection: a transaction that watched any key MUST abort the
        // matching EXEC after a flush, because the snapshot it captured
        // no longer exists. The per-key WatchRegistry::Touched hook
        // requires a key, so the flush path uses the database-wide
        // TouchedAll fan-out instead. Same lock-free fast path applies
        // (single atomic load when nothing is watched).
        if (state->watchRegistry != nullptr)
            (void) state->watchRegistry->TouchedAll();
        // Publish the canonical Redis FLUSHDB keyspace event. Empty key
        // matches Redis's own behaviour (the event channel
        // `__keyevent@0__:flushdb` is whole-database, not per-key).
        NotifyKeyspace(state, KeyspaceEvents::Generic, "flushdb", std::string_view {});
        co_return co_await ReplyOk(socket);
    }

    Task<bool> HandleSelect(ISocket* socket, std::span<std::string const> args)
    {
        // fastcached exposes a single logical keyspace. The redis client
        // crate issues `SELECT <index>` whenever the connection URL names a
        // database; accept any index rather than erroring (which aborts the
        // client's connection setup and surfaces as a startup timeout).
        static_cast<void>(args);
        co_return co_await ReplyOk(socket);
    }

    Task<bool> HandleClient(ISocket* socket, std::span<std::string const> args)
    {
        // Client libraries send CLIENT SETNAME / SETINFO / ID / GETNAME during
        // connection setup. We hold no per-client state, so acknowledge each
        // benignly instead of returning an error that would abort the
        // library's handshake.
        auto const sub = args.empty() ? std::string {} : Upper(args[0]);
        if (sub == "GETNAME")
            co_return co_await ReplyBulkString(socket, std::string_view {});
        if (sub == "ID")
            co_return co_await ReplyInteger(socket, 1);
        co_return co_await ReplyOk(socket);
    }

    Task<bool> HandleConfig(ISocket* socket, std::span<std::string const> args, RespVersion resp)
    {
        // CONFIG GET <param>... — redis clients probe parameters such as
        // `maxmemory` / `save` on connect. We expose no runtime tunables, so
        // every requested parameter reports "0". The reply is a key/value map:
        // a RESP3 map (`%`) or the equivalent flat RESP2 array (`*<2N>`).
        // CONFIG SET / RESETSTAT / REWRITE are accepted as no-ops.
        auto const sub = args.empty() ? std::string {} : Upper(args[0]);
        if (sub == "GET")
        {
            auto const params = std::span<std::string const> { args.data() + 1, args.size() - 1 };
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Map, params.size(), resp))
                co_return false;
            for (auto const& name: params)
            {
                if (!co_await ReplyBulkString(socket, name))
                    co_return false;
                if (!co_await ReplyBulkString(socket, std::string_view { "0" }))
                    co_return false;
            }
            co_return true;
        }
        co_return co_await ReplyOk(socket);
    }

    /// Write a pub/sub subscribe/unsubscribe confirmation frame:
    /// `[kind, channel, count]`. A Push (`>`) under RESP3, an array (`*`) under
    /// RESP2 — redis sends these as out-of-band pushes in RESP3.
    /// @param kind    "subscribe" / "unsubscribe" / "psubscribe" / "punsubscribe".
    /// @param channel The (un)subscribed channel or pattern.
    /// @param count   The connection's remaining subscription count.
    /// @param resp    The connection's negotiated protocol version.
    Task<bool> WriteSubscribeConfirm(
        ISocket* socket, std::string_view kind, std::string_view channel, std::int64_t count, RespVersion resp)
    {
        co_return (co_await ReplyAggregateHeader(socket, Aggregate::Push, 3, resp))
            && (co_await ReplyBulkString(socket, kind)) && (co_await ReplyBulkString(socket, channel))
            && (co_await ReplyInteger(socket, count));
    }

    /// Write one delivered pub/sub message as a push (RESP3) or array (RESP2):
    /// `[message, channel, payload]` or `[pmessage, pattern, channel, payload]`.
    /// @param message The delivery (by value — a coroutine parameter must not be
    ///        a reference).
    /// @param resp The connection's negotiated protocol version.
    Task<bool> WritePushMessage(ISocket* socket, PushMessage message, RespVersion resp)
    {
        if (message.kind == PushMessage::Kind::PMessage)
            co_return (co_await ReplyAggregateHeader(socket, Aggregate::Push, 4, resp))
                && (co_await ReplyBulkString(socket, std::string_view { "pmessage" }))
                && (co_await ReplyBulkString(socket, message.pattern)) && (co_await ReplyBulkString(socket, message.channel))
                && (co_await ReplyBulkString(socket, message.payload));
        co_return (co_await ReplyAggregateHeader(socket, Aggregate::Push, 3, resp))
            && (co_await ReplyBulkString(socket, std::string_view { "message" }))
            && (co_await ReplyBulkString(socket, message.channel)) && (co_await ReplyBulkString(socket, message.payload));
    }

    /// A pub/sub (un)subscribe verb described as data: the reply label plus the
    /// registry method to invoke. SUBSCRIBE/UNSUBSCRIBE/PSUBSCRIBE/PUNSUBSCRIBE
    /// differ only by these, so one handler drives all four from this table.
    struct SubscribeVerb
    {
        std::string_view label; ///< Reply kind label.
        std::size_t (*method)(IPubSubRegistry*,
                              std::shared_ptr<ISubscriber> const&,
                              std::string_view); ///< Adapter dispatching to the right registry call.
        bool subscribing;                        ///< True for (p)subscribe, false for (p)unsubscribe.
        bool pattern;                            ///< True for (p)subscribe/(p)unsubscribe pattern variant.
    };

    /// Adapter functions bridging the SubscribeVerb dispatch table to the
    /// registry's two distinct sub/unsub signatures (subscribing variants take
    /// a shared_ptr to extend the subscriber's lifetime into the registry's
    /// weak_ptr storage; unsubscribing variants only need the lookup key).
    /// The shared_ptr arg is ignored for unsubscribe paths.
    [[nodiscard]] inline std::size_t SubscribeAdapter(IPubSubRegistry* r,
                                                      std::shared_ptr<ISubscriber> const& sub,
                                                      std::string_view t)
    {
        return r->Subscribe(sub, t);
    }
    [[nodiscard]] inline std::size_t UnsubscribeAdapter(IPubSubRegistry* r,
                                                        std::shared_ptr<ISubscriber> const& sub,
                                                        std::string_view t)
    {
        return r->Unsubscribe(sub.get(), t);
    }
    [[nodiscard]] inline std::size_t PSubscribeAdapter(IPubSubRegistry* r,
                                                       std::shared_ptr<ISubscriber> const& sub,
                                                       std::string_view t)
    {
        return r->PSubscribe(sub, t);
    }
    [[nodiscard]] inline std::size_t PUnsubscribeAdapter(IPubSubRegistry* r,
                                                         std::shared_ptr<ISubscriber> const& sub,
                                                         std::string_view t)
    {
        return r->PUnsubscribe(sub.get(), t);
    }

    /// Drive a SUBSCRIBE-family command: for each channel/pattern argument, call
    /// the registry and write a confirmation carrying the running count. Creates
    /// the connection's Subscriber lazily on first use.
    /// @param verb  The data descriptor selecting label + registry method.
    /// @param state Connection state (subscriber + subscriptionCount updated).
    Task<bool> HandleSubscribeFamily(ISocket* socket,
                                     SessionContext session,
                                     std::span<std::string const> args,
                                     SubscribeVerb verb,
                                     ConnectionState* state)
    {
        if (session.pubsub == nullptr || !state->subscriber)
            co_return co_await ReplyError(socket, "pub/sub is not available");
        auto const sub = state->subscriber; // shared_ptr copy keeps the subscriber alive across awaits.
        // No-argument (P)UNSUBSCRIBE means "unsubscribe from every currently
        // subscribed channel/pattern" — the form clients (redis-py, lettuce,
        // ioredis) issue on graceful pool return. We snapshot the live set
        // first, then issue per-channel unsubscribes so every confirmation
        // carries the running count exactly as a multi-arg call would.
        // For (P)SUBSCRIBE empty args is still a usage error.
        if (args.empty())
        {
            if (verb.subscribing)
                co_return co_await ReplyError(socket, std::format("wrong number of arguments for '{}'", verb.label));
            auto const targets =
                verb.pattern ? session.pubsub->SnapshotPatterns(sub.get()) : session.pubsub->SnapshotChannels(sub.get());
            if (targets.empty())
            {
                // Redis: one confirmation with an empty channel field and count
                // 0 (clients use this to know "no subscriptions to drop").
                if (!co_await WriteSubscribeConfirm(socket, verb.label, std::string_view {}, 0, state->resp))
                    co_return false;
                co_return true;
            }
            for (auto const& target: targets)
            {
                auto const count = verb.method(session.pubsub, sub, target);
                state->subscriptionCount = count;
                if (!co_await WriteSubscribeConfirm(
                        socket, verb.label, target, static_cast<std::int64_t>(count), state->resp))
                    co_return false;
            }
            co_return true;
        }
        for (auto const& channel: args)
        {
            auto const count = verb.method(session.pubsub, sub, channel);
            state->subscriptionCount = count;
            if (!co_await WriteSubscribeConfirm(socket, verb.label, channel, static_cast<std::int64_t>(count), state->resp))
                co_return false;
        }
        co_return true;
    }

    /// PUBLISH channel message — fan out to subscribers; reply the receiver count.
    Task<bool> HandlePublish(ISocket* socket, SessionContext session, std::span<std::string const> args)
    {
        if (args.size() != 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'publish'");
        if (session.pubsub == nullptr)
            co_return co_await ReplyError(socket, "pub/sub is not available");
        auto const receivers = session.pubsub->Publish(args[0], args[1]);
        co_return co_await ReplyInteger(socket, static_cast<std::int64_t>(receivers));
    }

    /// `MULTI` — open a transaction queue. Subsequent non-transaction-control
    /// commands are queued (reply `+QUEUED`) until `EXEC` / `DISCARD`.
    /// Nested `MULTI` is rejected with the redis-standard error and does not
    /// affect the queue.
    Task<bool> HandleMulti(ISocket* socket, ConnectionState* state)
    {
        if (state->inMulti)
            co_return co_await ReplyError(socket, "MULTI calls can not be nested");
        state->inMulti = true;
        state->multiDirty = false;
        state->queue.clear();
        state->queueBytes = 0;
        // Drop any stale dirty bit that a racing Touched dropped on this
        // handle AFTER the previous EXEC's ClaimAndClearDirty. EXEC's
        // UnregisterAll erases the handle from the index BEFORE
        // ClaimAndClearDirty exchanges; a Touched that snapshotted the
        // strong_ptr before the erase can still fire MarkDirty afterward,
        // leaving _dirty=true on the reused handle. Without this claim,
        // the next EXEC on a fresh WATCH (against a key with no actual
        // racing write) would spuriously abort with *-1.
        if (state->watch)
            (void) state->watch->ClaimAndClearDirty();
        co_return co_await ReplyOk(socket);
    }

    /// `DISCARD` — abort the transaction: drop the queue and every WATCH.
    /// Outside `MULTI` redis replies `-ERR DISCARD without MULTI`.
    Task<bool> HandleDiscard(ISocket* socket, ConnectionState* state)
    {
        if (!state->inMulti)
            co_return co_await ReplyError(socket, "DISCARD without MULTI");
        state->inMulti = false;
        state->multiDirty = false;
        state->queue.clear();
        state->queueBytes = 0;
        if (state->watch && state->watchRegistry != nullptr)
            state->watchRegistry->UnregisterAll(state->watch.get());
        co_return co_await ReplyOk(socket);
    }

    /// `UNWATCH` — drop every WATCH snapshot on this connection. Always +OK
    /// (no-op if there were no watches). Valid both inside and outside MULTI.
    Task<bool> HandleUnwatch(ISocket* socket, ConnectionState* state)
    {
        if (state->watch && state->watchRegistry != nullptr)
            state->watchRegistry->UnregisterAll(state->watch.get());
        co_return co_await ReplyOk(socket);
    }

    /// `WATCH key [key ...]` — snapshot each key's current CAS. A later
    /// mutation on any of them flips the connection's dirty flag and
    /// aborts the matching `EXEC` with a nil multi-bulk. Disallowed inside
    /// `MULTI` (matches redis).
    Task<bool> HandleWatch(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'watch'");
        if (state->inMulti)
            co_return co_await ReplyError(socket, "WATCH inside MULTI is not allowed");
        if (state->watchRegistry == nullptr)
            co_return co_await ReplyError(socket, "transactions are not available");
        if (!state->watch)
            state->watch = std::make_shared<WatchHandle>();
        // Track FRESHLY-registered keys (Register returned `inserted=true`)
        // so a mid-call storage failure rolls back ONLY those keys, not
        // the connection's accumulated WATCH state. A previous shape
        // pushed every iterated key unconditionally, which silently wiped
        // re-registrations from earlier WATCH calls: `WATCH a` succeeded,
        // then `WATCH a b` with PeekCas(b) failing would Unregister `a`
        // (which was already registered by the first WATCH) and the
        // client's accumulated watch on `a` quietly evaporated.
        std::vector<std::string_view> rollbackKeys;
        rollbackKeys.reserve(args.size());
        for (auto const& key: args)
        {
            // Order matters under multi-reactor load:
            //   1. Register the index entry FIRST,
            //   2. Then PeekCas (non-bumping — a WATCH probe must not
            //      promote LRU recency the way GET would),
            //   3. Then store the snapshot on the handle.
            // Inserting the index before reading the CAS ensures any
            // concurrent `SET` that lands between PeekCas and Remember
            // calls `Touched` against an index that ALREADY contains this
            // handle — MarkDirty fires and EXEC aborts. The previous
            // "PeekCas-then-Register-with-cas" shape had a race window
            // where Touched found the index empty (handle not yet
            // registered), no handle dirtied, EXEC committed over the
            // racing write. Real Redis avoids this by being
            // single-threaded; fastcached cannot.
            //
            // A StorageError on PeekCas means the snapshot is unreliable —
            // treating an I/O failure as "key absent" would silently
            // commit a later EXEC against state we never read. Roll back
            // only the keys this call freshly registered.
            bool const inserted = state->watchRegistry->Register(state->watch, key);
            if (inserted)
                rollbackKeys.push_back(key);
            auto const cas = engine->PeekCas(key);
            if (!cas.has_value())
            {
                for (auto const rollbackKey: rollbackKeys)
                    state->watchRegistry->Unregister(state->watch.get(), rollbackKey);
                co_return co_await ReplyError(socket, "storage failure during WATCH");
            }
            state->watch->Remember(key, *cas);
        }
        co_return co_await ReplyOk(socket);
    }

    // (Forward declarations for CommandTable et al. live in the prior
    // declaration block above; HandleExec routes queued commands through
    // `CommandTableInvoke(idx, ctx)` instead of re-entering Dispatch.)

    /// `EXEC` — commit the queued transaction. If any watched key has been
    /// touched since `WATCH`, drop the queue and reply `*-1` (nil multi-bulk,
    /// redis's "aborted" sentinel). Otherwise frame the responses of every
    /// queued command as a single `*N` multi-bulk in FIFO order.
    Task<bool> HandleExec(ISocket* socket, CacheEngine* engine, SessionContext session, ConnectionState* state)
    {
        if (!state->inMulti)
            co_return co_await ReplyError(socket, "EXEC without MULTI");

        // Snapshot+clear local state up front so the EXEC reply is the only
        // thing that runs from here on, even if a replayed command throws.
        // Snapshot wasDirty FIRST, then zero multiDirty so the next MULTI
        // starts clean (defensive: every entry into MULTI also zeroes it,
        // but doubling up here means a future code path that fails to call
        // HandleMulti cannot leak a stale dirty bit into the next EXEC).
        auto queue = std::exchange(state->queue, {});
        auto const wasDirty = state->multiDirty;
        state->inMulti = false;
        state->multiDirty = false;
        state->queueBytes = 0;
        // Order is load-bearing for the EXEC race:
        //   1. UnregisterAll removes the handle from the registry index
        //      under WatchRegistry::_mu. Once we return, no new Touched
        //      call can find a strong_ptr to this handle. Snapshots are
        //      wiped outside _mu via Clear(); Clear no longer touches
        //      _dirty, so it cannot race with MarkDirty.
        //   2. ClaimAndClearDirty atomically exchanges (_dirty -> false)
        //      and returns the prior value. Any racing Touched that
        //      snapshotted the strong_ptr BEFORE the index erase but
        //      whose MarkDirty fires AFTER the exchange is collected by
        //      the NEXT MULTI's ClaimAndClearDirty (HandleMulti now
        //      drains it on entry, so a fresh transaction on the same
        //      connection starts clean).
        bool watchTripped = false;
        if (state->watch && state->watchRegistry != nullptr)
        {
            state->watchRegistry->UnregisterAll(state->watch.get());
            watchTripped = state->watch->ClaimAndClearDirty();
        }

        if (wasDirty)
            co_return co_await WriteAll(socket, "-EXECABORT Transaction discarded because of previous errors.\r\n");
        if (watchTripped)
        {
            // Redis: nil multi-bulk reply on a tripped WATCH. RESP2 spells
            // this as the null array `*-1\r\n` (NOT the null bulk `$-1\r\n`,
            // which is what `ReplyNull` writes); RESP3 collapses both to
            // the canonical null `_\r\n`.
            co_return co_await WriteAll(socket, state->resp == RespVersion::Resp3 ? "_\r\n" : "*-1\r\n");
        }

        // Write the multi-bulk header up front and then invoke each queued
        // command's handler DIRECTLY via its cached CommandTable index.
        // Bypassing Dispatch saves, per queued command: an Upper()
        // heap-allocation, an auth-source shared_ptr copy, a linear
        // CommandTable scan (the one that already ran at queue time), and
        // the per-arg byte-cap walk. For an EXEC of N commands this turns
        // 2N Dispatch prologues into N direct handler invocations.
        // EXEC's aggregate header is written before the queue is replayed.
        // If a queued handler returns false mid-replay (socket write
        // failure, e.g. EPIPE from a half-closed peer), the multi-bulk is
        // on-the-wire truncated and the outer co_return false triggers
        // connection teardown — the client sees `*N\r\n` + K<N elements
        // then EOF. A pooled-connection client that hands the closed
        // socket back to its pool for reuse would desync, but that's a
        // client-side pool bug (handing out a closed fd); the server-side
        // contract here is: a partial-EXEC reply is paired with an
        // immediate connection close, never with a `+OK` for a subsequent
        // command on the same fd. Buffering the entire EXEC reply before
        // flushing would close the desync window for misbehaving pools
        // but at the cost of holding every replay's bytes in memory until
        // the last handler completes — accept the simpler shape since
        // ISocket already guarantees post-failure unreachability.
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, queue.size(), state->resp))
            co_return false;
        for (auto& entry: queue)
        {
            // QueuedCommand invariant: argv.size() >= 1 (the verb name
            // occupies index 0). Today this is enforced by the arity
            // check upstream of state->queue.push_back; defending it here
            // means a future code path that bypasses the arity check (a
            // savepoint replay, a script verb, a test seam) cannot trip
            // the `size() - 1` underflow that would produce a span over
            // `size_t(-1)` bytes. Surface the breach as a per-element
            // -ERR rather than a SEGV — the outer aggregate header has
            // already been written with `queue.size()` elements promised.
            if (entry.argv.empty())
            {
                if (!co_await ReplyError(socket, "internal: queued command with empty argv"))
                    co_return false;
                continue;
            }
            auto const tail = std::span<std::string const> { entry.argv.data() + 1, entry.argv.size() - 1 };
            if (!co_await CommandTableInvoke(
                    entry.commandTableIdx,
                    CommandContext { .socket = socket, .engine = engine, .tail = tail, .state = state, .session = session }))
                co_return false;
        }
        co_return true;
    }

    /// Verbs that bypass the MULTI queue branch entirely. Two groups land here:
    ///   - The transaction-control verbs themselves (MULTI/EXEC/DISCARD/WATCH/
    ///     UNWATCH) — they steer the queue and therefore cannot be queued.
    ///   - The session-control verbs AUTH and RESET — every other write in
    ///     this Dispatch needs to see their effect IMMEDIATELY (AUTH unlocks
    ///     the connection, RESET clears every per-connection state, including
    ///     `inMulti`), so queuing them would either let unauthenticated
    ///     commands sneak in or fight RESET's clear-the-slate semantics.
    /// Note: QUIT is deliberately NOT in this list. Allowing QUIT to tear the
    /// session down mid-MULTI would leave EXEC writing aggregate elements to a
    /// closed fd — see `IsForbiddenInMulti`.
    [[nodiscard]] constexpr bool RunsEvenInsideMulti(std::string_view name) noexcept
    {
        return name == "MULTI" || name == "EXEC" || name == "DISCARD" || name == "WATCH" || name == "UNWATCH"
               || name == "AUTH" || name == "RESET";
    }

    /// Verbs that MUST NOT be queued inside MULTI. Three reasons land here:
    ///   1. The handler writes more than one RESP frame per reply, which would
    ///      desync EXEC's `*N` aggregate header from the actual element count
    ///      (every SUBSCRIBE channel produces its own confirmation frame).
    ///   2. The handler tears down the session mid-frame (QUIT closes the
    ///      socket; a subsequent queued command's reply would land on a closed
    ///      fd and the multi-bulk would never complete on the wire).
    ///   3. The handler mutates `state->resp` (HELLO ... <version>). EXEC has
    ///      ALREADY written the multi-bulk aggregate header using the
    ///      pre-replay resp version; flipping it mid-replay would mix RESP2
    ///      and RESP3 element encodings inside the same `*N` aggregate and
    ///      desync any strict client parser.
    /// Real redis simply queues these and replays them, accepting both
    /// hazards; we trade a smidge of compatibility for protocol robustness.
    /// A rejected command sets `multiDirty` so the matching EXEC aborts with
    /// `-EXECABORT` — the queue cannot silently lose the rejected verb.
    [[nodiscard]] constexpr bool IsForbiddenInMulti(std::string_view name) noexcept
    {
        return name == "SUBSCRIBE" || name == "UNSUBSCRIBE" || name == "PSUBSCRIBE" || name == "PUNSUBSCRIBE"
               || name == "QUIT" || name == "HELLO";
    }

    /// Map a StorageError from a set command to the appropriate RESP error,
    /// returning std::nullopt when there is no error to report (the caller then
    /// renders the success value). Centralises WRONGTYPE / generic-failure
    /// handling shared by every set command.
    /// @param err The storage error to translate (by value — a coroutine
    ///        parameter must not be a reference).
    Task<bool> ReplySetError(ISocket* socket, StorageError err)
    {
        if (err.code == StorageErrorCode::WrongType)
            co_return co_await ReplyWrongType(socket);
        co_return co_await ReplyError(socket, "storage failure");
    }

    Task<bool> HandleSAdd(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'sadd'");
        auto const added = engine->SetAdd(args[0], args.subspan(1));
        if (!added.has_value())
            co_return co_await ReplySetError(socket, added.error());
        if (*added > 0)
        {
            NotifyWatchers(state, args[0]);
            // Redis emits "sadd" under the generic class for set-mutating verbs.
            NotifyKeyspace(state, KeyspaceEvents::Generic, "sadd", args[0]);
        }
        co_return co_await ReplyInteger(socket, *added);
    }

    Task<bool> HandleSRem(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'srem'");
        auto const removed = engine->SetRemove(args[0], args.subspan(1));
        if (!removed.has_value())
            co_return co_await ReplySetError(socket, removed.error());
        if (*removed > 0)
        {
            NotifyWatchers(state, args[0]);
            NotifyKeyspace(state, KeyspaceEvents::Generic, "srem", args[0]);
        }
        co_return co_await ReplyInteger(socket, *removed);
    }

    Task<bool> HandleSMembers(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'smembers'");
        auto const members = engine->SetMembers(args[0]);
        if (!members.has_value())
            co_return co_await ReplySetError(socket, members.error());
        // SMEMBERS replies a set (~) under RESP3, an array (*) under RESP2.
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Set, members->size(), resp))
            co_return false;
        for (auto const& m: *members)
            if (!co_await ReplyBulkString(socket, m))
                co_return false;
        co_return true;
    }

    Task<bool> HandleSIsMember(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() != 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'sismember'");
        auto const present = engine->SetIsMember(args[0], args[1]);
        if (!present.has_value())
            co_return co_await ReplySetError(socket, present.error());
        // SISMEMBER replies a boolean (#) under RESP3, an integer 1/0 under RESP2.
        co_return co_await ReplyBoolean(socket, *present, resp);
    }

    Task<bool> HandleSMIsMember(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'smismember'");
        auto const flags = engine->SetMIsMember(args[0], args.subspan(1));
        if (!flags.has_value())
            co_return co_await ReplySetError(socket, flags.error());
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, flags->size(), resp))
            co_return false;
        for (bool const present: *flags)
            if (!co_await ReplyBoolean(socket, present, resp))
                co_return false;
        co_return true;
    }

    Task<bool> HandleSCard(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'scard'");
        auto const card = engine->SetCard(args[0]);
        if (!card.has_value())
            co_return co_await ReplySetError(socket, card.error());
        co_return co_await ReplyInteger(socket, *card);
    }

    Task<bool> HandleSPop(
        ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args, RespVersion resp)
    {
        if (args.empty() || args.size() > 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'spop'");
        // SPOP key            -> single member (bulk) or null.
        // SPOP key <count>    -> a set of up to <count> members.
        bool const hasCount = args.size() == 2;
        std::uint64_t count = 1;
        if (hasCount && !ParseUnsigned(std::string_view { args[1] }, count))
            co_return co_await ReplyError(socket, "value is out of range, must be positive");

        auto const popped = engine->SetPop(args[0], static_cast<std::size_t>(count));
        if (!popped.has_value())
            co_return co_await ReplySetError(socket, popped.error());
        if (!popped->empty())
        {
            NotifyWatchers(state, args[0]);
            NotifyKeyspace(state, KeyspaceEvents::Generic, "spop", args[0]);
        }

        if (!hasCount)
        {
            if (popped->empty())
                co_return co_await ReplyNull(socket, resp);
            co_return co_await ReplyBulkString(socket, (*popped)[0]);
        }
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Set, popped->size(), resp))
            co_return false;
        for (auto const& m: *popped)
            if (!co_await ReplyBulkString(socket, m))
                co_return false;
        co_return true;
    }

    /// LOLWUT — redis's whimsical version/art command. We reply a short banner
    /// as a verbatim string (txt) under RESP3, a bulk string under RESP2. Cheap,
    /// and it exercises the verbatim writer on a second command besides INFO.
    Task<bool> HandleLolwut(ISocket* socket, RespVersion resp)
    {
        auto const art = std::format("fastcached {}\n", RedisRespHandler::ServerVersion());
        co_return co_await ReplyVerbatim(socket, "txt", art, resp);
    }

    /// INCRBYFLOAT key increment — atomic floating-point read-modify-write over
    /// the value bytes. Per redis/valkey the reply is the new value as a bulk
    /// string in BOTH RESP2 and RESP3 (it does not use the RESP3 double type);
    /// the dedicated double type is exercised by `DEBUG PROTOCOL double` instead.
    Task<bool> HandleIncrByFloat(ISocket* socket,
                                 CacheEngine* engine,
                                 ConnectionState* state,
                                 std::span<std::string const> args)
    {
        if (args.size() != 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'incrbyfloat'");
        double delta = 0;
        if (!ParseDouble(std::string_view { args[1] }, delta))
            co_return co_await ReplyError(socket, "value is not a valid float");

        // The Update closure computes the new value bytes once; capture them
        // here so the reply can reuse the same bytes without a second Get on
        // the storage (which would also race a concurrent DEL/SET on the key).
        std::string encodedText;
        auto const result =
            engine->Update(args[0], [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
                double base = 0;
                if (current.found)
                {
                    // Redis: WRONGTYPE for INCRBYFLOAT on a non-string
                    // key. Mirrors HandleIncrDecrBy / HandleGet.
                    if (SetCodec::IsSet(current.entry.flags))
                        return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
                    auto const bytes = current.entry.ValueBytes();
                    std::string_view const text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
                    if (!ParseDouble(text, base))
                        return std::unexpected(MakeStorageError(StorageErrorCode::NotANumber));
                }
                auto const sum = base + delta;
                // Redis rejects overflow / NaN on the write itself rather than
                // storing 'inf' (which subsequent INCRBYFLOAT would refuse to
                // re-parse), so the prior value is left intact.
                if (!std::isfinite(sum))
                    return std::unexpected(MakeStorageError(StorageErrorCode::InfiniteOrNaN));
                encodedText = std::format("{}", sum);
                std::vector<std::byte> encoded(encodedText.size());
                std::memcpy(encoded.data(), encodedText.data(), encodedText.size());
                return IStorage::UpdateOutcome { .value = std::move(encoded),
                                                 .flags = 0,
                                                 .action = IStorage::UpdateAction::Store };
            });

        if (!result.has_value())
        {
            if (result.error().code == StorageErrorCode::WrongType)
                co_return co_await ReplyWrongType(socket);
            if (result.error().code == StorageErrorCode::NotANumber)
                co_return co_await ReplyError(socket, "value is not a valid float");
            if (result.error().code == StorageErrorCode::InfiniteOrNaN)
                co_return co_await ReplyError(socket, "increment would produce NaN or Infinity");
            co_return co_await ReplyError(socket, "storage failure");
        }
        NotifyWatchers(state, args[0]);
        // Redis emits "incrbyfloat" under the string class for INCRBYFLOAT
        // (matches the keyspace-events table in the redis docs).
        NotifyKeyspace(state, KeyspaceEvents::String, "incrbyfloat", args[0]);
        co_return co_await ReplyBulkString(
            socket,
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(encodedText.data()), encodedText.size() });
    }

    /// INCR / DECR / INCRBY / DECRBY — atomic signed-64-bit counter
    /// operations on the string value at `key`. Missing keys are
    /// initialised to `0` before the delta is applied (redis semantics);
    /// a non-integer existing value is `-ERR`. Goes through the storage's
    /// guarded `Update` primitive so the read-modify-write is atomic
    /// under concurrent writers.
    /// @param verb  Wire command name, drives the error string.
    /// @param delta Signed amount to add (negative for DEC*).
    Task<bool> HandleIncrDecrBy(ISocket* socket,
                                CacheEngine* engine,
                                ConnectionState* state,
                                std::string_view key,
                                std::string_view verb,
                                std::int64_t delta)
    {
        std::int64_t newValue = 0;
        bool overflow = false;
        std::string encodedText;
        auto const result =
            engine->Update(key, [&](GetResult const& current) -> std::expected<IStorage::UpdateOutcome, StorageError> {
                std::int64_t base = 0;
                if (current.found)
                {
                    // Redis returns WRONGTYPE (not the generic numeric
                    // error) for INCR on a non-string key. HandleGet
                    // line 815 has the same branch — repeat it here so
                    // the wire reply matches when a client INCRs into
                    // a set-typed key by mistake.
                    if (SetCodec::IsSet(current.entry.flags))
                        return std::unexpected(MakeStorageError(StorageErrorCode::WrongType));
                    auto const bytes = current.entry.ValueBytes();
                    std::string_view const text { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
                    if (text.empty())
                        return std::unexpected(MakeStorageError(StorageErrorCode::NotANumber));
                    if (!ParseSigned(text, base))
                        return std::unexpected(MakeStorageError(StorageErrorCode::NotANumber));
                }
                // Signed overflow detection — same contract as redis: refuse,
                // leave the previous value intact.
                if ((delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta)
                    || (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta))
                {
                    overflow = true;
                    return std::unexpected(MakeStorageError(StorageErrorCode::InfiniteOrNaN));
                }
                newValue = base + delta;
                encodedText = std::format("{}", newValue);
                std::vector<std::byte> encoded(encodedText.size());
                std::memcpy(encoded.data(), encodedText.data(), encodedText.size());
                return IStorage::UpdateOutcome { .value = std::move(encoded),
                                                 .flags = 0,
                                                 .action = IStorage::UpdateAction::Store };
            });

        if (!result.has_value())
        {
            if (result.error().code == StorageErrorCode::WrongType)
                co_return co_await ReplyWrongType(socket);
            if (result.error().code == StorageErrorCode::NotANumber)
                co_return co_await ReplyError(socket, "value is not an integer or out of range");
            if (overflow || result.error().code == StorageErrorCode::InfiniteOrNaN)
                co_return co_await ReplyError(socket, "increment or decrement would overflow");
            co_return co_await ReplyError(socket, std::format("storage failure for '{}'", verb));
        }
        NotifyWatchers(state, key);
        // Verb is "incr" / "decr" / "incrby" / "decrby"; the keyspace event
        // matches the wire verb (redis convention).
        NotifyKeyspace(state, KeyspaceEvents::String, verb, key);
        co_return co_await ReplyInteger(socket, newValue);
    }

    /// INCR / DECR — fixed-magnitude variants. Dispatched separately so the
    /// arg-count error string matches the wire verb exactly.
    Task<bool> HandleIncrDecr(ISocket* socket,
                              CacheEngine* engine,
                              ConnectionState* state,
                              std::span<std::string const> args,
                              std::string_view verb,
                              std::int64_t sign)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, std::format("wrong number of arguments for '{}'", verb));
        co_return co_await HandleIncrDecrBy(socket, engine, state, args[0], verb, sign);
    }

    /// INCRBY / DECRBY — variable-magnitude variants. The delta arg is
    /// signed; DECRBY negates it before delegating.
    Task<bool> HandleIncrDecrByVerb(ISocket* socket,
                                    CacheEngine* engine,
                                    ConnectionState* state,
                                    std::span<std::string const> args,
                                    std::string_view verb,
                                    bool negate)
    {
        if (args.size() != 2)
            co_return co_await ReplyError(socket, std::format("wrong number of arguments for '{}'", verb));
        std::int64_t delta = 0;
        if (!ParseSigned(std::string_view { args[1] }, delta))
            co_return co_await ReplyError(socket, "value is not an integer or out of range");
        if (negate)
        {
            // Guard against INT64_MIN negation (UB). Refuse with the same
            // overflow-style reply rather than wrapping silently.
            if (delta == std::numeric_limits<std::int64_t>::min())
                co_return co_await ReplyError(socket, "increment or decrement would overflow");
            delta = -delta;
        }
        co_return co_await HandleIncrDecrBy(socket, engine, state, args[0], verb, delta);
    }

    /// MGET key [key ...] — array reply, one bulk-or-nil per key in order.
    /// Uses non-mutating Peek so a probe via MGET does not bump LRU
    /// recency on every key (which would defeat eviction under MGET-heavy
    /// read workloads); compare GET which does promote.
    Task<bool> HandleMget(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'mget'");
        if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, args.size(), resp))
            co_return false;
        for (auto const& key: args)
        {
            // Peek (not Get) — MGET is a probe, not a client access. A Get
            // would bump LRU recency and stats on every key, defeating
            // eviction under MGET-heavy read workloads (a 1000-key cold
            // probe would re-promote all 1000 to MRU). Mirrors the no-bump
            // contract the new TTL command family already honours.
            auto const result = engine->Peek(key);
            if (!result.has_value() || !result->found || SetCodec::IsSet(result->entry.flags))
            {
                if (!co_await ReplyNull(socket, resp))
                    co_return false;
                continue;
            }
            if (!co_await ReplyBulkString(socket, result->entry.ValueBytes(), result->entry.value.AsKeepAlive()))
                co_return false;
        }
        co_return true;
    }

    /// MSET key value [key value ...] — unconditional batch set.
    /// Pair-count is enforced; the operation is NOT atomic across keys
    /// (matches redis cluster semantics where MSET on multiple slots is
    /// rejected, but on a single instance MSET is best-effort). Reply
    /// `+OK` once every key has been written.
    Task<bool> HandleMset(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.empty() || (args.size() % 2) != 0)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'mset'");
        // Per-key probes (no hoist): NotifyWatchers/NotifyKeyspace each
        // have a lock-free fast path — see HandleDel for the rationale.
        // The previous hoist allowed a concurrent WATCH on another reactor
        // mid-loop to be silently skipped for later keys, breaking the
        // WATCH guarantee for that racing transaction.
        for (std::size_t i = 0; i < args.size(); i += 2)
        {
            auto const r = engine->Set(args[i], BytesFromString(args[i + 1]), 0, 0);
            if (!r.has_value())
                co_return co_await ReplyError(socket, "storage failure");
            NotifyWatchers(state, args[i]);
            NotifyKeyspace(state, KeyspaceEvents::String, "set", args[i]);
        }
        co_return co_await ReplyOk(socket);
    }

    /// MSETNX key value [key value ...] — all-or-nothing set: every key
    /// must be absent at the moment of the check, otherwise no key is
    /// written. Reply `:1` on success, `:0` if any key already exists or
    /// if any Add fails mid-batch (in which case earlier writes are
    /// rolled back via Delete).
    ///
    /// Strict atomicity across multiple shards would require a global
    /// lock; this implementation is two-pass (probe → Add) with a
    /// best-effort rollback on partial failure. The wire contract now
    /// matches the redis single-instance promise ("nothing was
    /// committed" on `:0`); a concurrent SET racing into the gap can
    /// still let one Add fail, but the rollback erases earlier writes
    /// from the batch so the keyspace is not left half-written.
    Task<bool> HandleMsetNx(ISocket* socket, CacheEngine* engine, ConnectionState* state, std::span<std::string const> args)
    {
        if (args.empty() || (args.size() % 2) != 0)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'msetnx'");
        // First pass: any key already present aborts the whole batch.
        for (std::size_t i = 0; i < args.size(); i += 2)
        {
            auto const peek = engine->Peek(args[i]);
            if (peek.has_value() && peek->found)
                co_return co_await ReplyInteger(socket, 0);
        }
        // Per-key probes (no hoist) — see HandleDel for the rationale.
        // The previous hoist allowed a concurrent WATCH on another reactor
        // mid-loop to be silently skipped for later keys.
        //
        // Second pass: write each via Add (NX semantics per key). Track
        // committed keys so we can roll back if a later Add fails.
        std::vector<std::string_view> committed;
        committed.reserve(args.size() / 2);
        for (std::size_t i = 0; i < args.size(); i += 2)
        {
            auto const& key = args[i];
            auto const& value = args[i + 1];
            auto const r = engine->Add(key, BytesFromString(value), 0, 0);
            if (!r.has_value())
            {
                // Rollback any earlier writes from this batch. Best-effort:
                // a concurrent SET on a rolled-back key would still observe
                // an intermediate state, but the wire reply `:0` honestly
                // means "no key from this batch is intentionally retained".
                for (auto const& k: committed)
                {
                    (void) engine->Delete(k);
                    NotifyWatchers(state, k);
                    // The rollback Delete fires "del" — the keyspace
                    // would otherwise show the half-written batch.
                    NotifyKeyspace(state, KeyspaceEvents::Generic, "del", k);
                }
                co_return co_await ReplyInteger(socket, 0);
            }
            committed.push_back(key);
            NotifyWatchers(state, key);
            NotifyKeyspace(state, KeyspaceEvents::String, "set", key);
        }
        co_return co_await ReplyInteger(socket, 1);
    }

    /// DEBUG <subcommand> — only `DEBUG PROTOCOL <type>` is meaningful here. It
    /// emits exactly one value of the requested RESP3 type, mirroring redis's
    /// conformance command. This is the canonical, non-contrived way to exercise
    /// the writers that no cache command naturally produces (bignum, attribute,
    /// double, verbatim, true/false, null, set, map, push), so a RESP3 client or
    /// test can validate the full type system. Data-driven: one row per type.
    Task<bool> HandleDebug(ISocket* socket, std::span<std::string const> args, RespVersion resp)
    {
        auto const sub = args.empty() ? std::string {} : Upper(args[0]);
        if (sub != "PROTOCOL" || args.size() < 2)
            co_return co_await ReplyOk(socket); // accept other DEBUG subcommands as no-ops
        auto const type = Upper(args[1]);

        if (type == "STRING")
            co_return co_await ReplyBulkString(socket, std::string_view { "Simple status string" });
        if (type == "INTEGER")
            co_return co_await ReplyInteger(socket, 12345);
        if (type == "DOUBLE")
            co_return co_await ReplyDouble(socket, 1.5, resp);
        if (type == "BIGNUM")
            co_return co_await ReplyBigNumber(socket, "1234567999999999999999999999999999999", resp);
        if (type == "TRUE")
            co_return co_await ReplyBoolean(socket, true, resp);
        if (type == "FALSE")
            co_return co_await ReplyBoolean(socket, false, resp);
        if (type == "NULL")
            co_return co_await ReplyNull(socket, resp);
        if (type == "ARRAY")
        {
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Array, 3, resp))
                co_return false;
            if (!co_await ReplyInteger(socket, 1))
                co_return false;
            if (!co_await ReplyInteger(socket, 2))
                co_return false;
            co_return co_await ReplyInteger(socket, 3);
        }
        if (type == "VERBATIM")
            co_return co_await ReplyVerbatim(socket, "txt", "This is a verbatim\nstring", resp);
        if (type == "MAP")
        {
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Map, 1, resp))
                co_return false;
            if (!co_await ReplyInteger(socket, 1))
                co_return false;
            co_return co_await ReplyBoolean(socket, true, resp);
        }
        if (type == "SET")
        {
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Set, 2, resp))
                co_return false;
            if (!co_await ReplyInteger(socket, 1))
                co_return false;
            co_return co_await ReplyInteger(socket, 2);
        }
        if (type == "PUSH")
        {
            // Out-of-band push frame. Under RESP3 this is `>`-prefixed;
            // under RESP2 it flattens to a regular array so a RESP2 client
            // sees the same payload it would see for a pub/sub delivery
            // under that version.
            if (!co_await ReplyAggregateHeader(socket, Aggregate::Push, 3, resp))
                co_return false;
            if (!co_await ReplyBulkString(socket, std::string_view { "pubsub" }))
                co_return false;
            if (!co_await ReplyBulkString(socket, std::string_view { "channel" }))
                co_return false;
            co_return co_await ReplyBulkString(socket, std::string_view { "payload" });
        }
        if (type == "ATTRIB")
        {
            // Attributes are a RESP3-only frame; under RESP2 the writer drops
            // the attribute header entirely. Emitting the same attribute-payload
            // sequence then would desync the wire — each payload bulk would be
            // read by the client as a separate top-level reply. So on RESP2
            // reply with a single bulk-string ("none"), matching what an
            // attribute-aware client would see after stripping the (advisory)
            // attribute.
            if (resp != RespVersion::Resp3)
                co_return co_await ReplyBulkString(socket, std::string_view { "none" });
            // RESP3: attribute prefixes a real reply; here an empty map follows.
            if (!co_await ReplyAttributeHeader(socket, 1, resp))
                co_return false;
            if (!co_await ReplyBulkString(socket, std::string_view { "key-popularity" }))
                co_return false;
            if (!co_await ReplyBulkString(socket, std::string_view { "none" }))
                co_return false;
            co_return co_await ReplyAggregateHeader(socket, Aggregate::Array, 0, resp);
        }
        co_return co_await ReplyError(socket, std::format("Wrong protocol type name: {}", type));
    }

    /// Commands a client may issue before authenticating when a credential is
    /// required: AUTH itself, QUIT, and HELLO. HELLO is allowed pre-auth (like
    /// redis's CMD_NO_AUTH) because it carries the inline `AUTH <user> <pass>`
    /// clause modern clients use to authenticate and negotiate RESP3 in one round
    /// trip; HandleHello parses that clause and still requires the connection to
    /// be authenticated before it returns the server map (it replies `-NOAUTH`
    /// otherwise), so allowing it here does not leak anything to an unauthenticated
    /// client. Every other command replies `-NOAUTH` until AUTH succeeds.
    [[nodiscard]] bool IsPreAuthAllowed(std::string_view name) noexcept
    {
        return name == "AUTH" || name == "QUIT" || name == "HELLO";
    }

    // (CommandContext was moved to the forward-declare block above so
    // HandleExec can use it.)

    using CommandHandler = Task<bool> (*)(CommandContext);

    /// One row of the command dispatch table: a command name, its handler,
    /// and the metadata `COMMAND` and `COMMAND DOCS` introspection clients
    /// (redis-cli completion, lettuce / go-redis health probes) read out of
    /// the table. Aliases (DEL/UNLINK, FLUSHDB/FLUSHALL) are separate rows
    /// sharing a handler so each spelling appears in the introspection
    /// reply.
    ///
    /// `arity` matches redis exactly: positive N means *exactly* N args
    /// including the command name, negative -N means *at least* N. So
    /// `MGET key [key ...]` is `arity = -2`; `GET key` is `arity = 2`.
    /// `firstKey` / `lastKey` / `keyStep` describe where key arguments
    /// live; `lastKey = -1` denotes "to end of argv".
    struct CommandEntry
    {
        std::string_view name;
        CommandHandler handler;
        std::int32_t arity { 1 };
        std::int32_t firstKey { 0 };
        std::int32_t lastKey { 0 };
        std::int32_t keyStep { 0 };
    };

    /// Data-driven command dispatch table. Each handler is a captureless lambda
    /// (so it decays to a function pointer) that unpacks the CommandContext and
    /// forwards to the typed Handle* coroutine. Adding a command is one row here.
    /// AUTH and the QUIT side-effect are handled in Dispatch itself because they
    /// need the SessionContext / must end the session.
    constexpr auto CommandTable = std::array {
        CommandEntry { .name = "GET",
                       .handler = [](CommandContext c) { return HandleGet(c.socket, c.engine, c.tail, c.state->resp); },
                       .arity = 2,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry {
            .name = "SET",
            .handler = [](CommandContext c) { return HandleSet(c.socket, c.engine, c.state, c.tail, c.state->resp); },
            .arity = -3,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry {
            .name = "SETEX",
            .handler = [](CommandContext c) { return HandleSetEx(c.socket, c.engine, c.state, c.tail, /*millis*/ false); },
            .arity = 4,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry {
            .name = "PSETEX",
            .handler = [](CommandContext c) { return HandleSetEx(c.socket, c.engine, c.state, c.tail, /*millis*/ true); },
            .arity = 4,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry { .name = "DEL",
                       .handler = [](CommandContext c) { return HandleDel(c.socket, c.engine, c.state, c.tail); },
                       .arity = -2,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 1 },
        CommandEntry { .name = "UNLINK",
                       .handler = [](CommandContext c) { return HandleDel(c.socket, c.engine, c.state, c.tail); },
                       .arity = -2,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 1 },
        CommandEntry { .name = "EXISTS",
                       .handler = [](CommandContext c) { return HandleExists(c.socket, c.engine, c.tail); },
                       .arity = -2,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 1 },
        // TTL / EXPIRE family. EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT share one
        // parametric handler (data: time unit + absolute-vs-relative).
        CommandEntry { .name = "EXPIRE",
                       .handler =
                           [](CommandContext c) {
                               return HandleExpire(c.socket, c.engine, c.state, c.tail, "expire", TtlUnit::Seconds, false);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "PEXPIRE",
                       .handler =
                           [](CommandContext c) {
                               return HandleExpire(
                                   c.socket, c.engine, c.state, c.tail, "pexpire", TtlUnit::Milliseconds, false);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "EXPIREAT",
                       .handler =
                           [](CommandContext c) {
                               return HandleExpire(c.socket, c.engine, c.state, c.tail, "expireat", TtlUnit::Seconds, true);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "PEXPIREAT",
                       .handler =
                           [](CommandContext c) {
                               return HandleExpire(
                                   c.socket, c.engine, c.state, c.tail, "pexpireat", TtlUnit::Milliseconds, true);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "TTL",
                       .handler = [](CommandContext c) { return HandleTtl(c.socket, c.engine, c.tail, TtlUnit::Seconds); },
                       .arity = 2,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry {
            .name = "PTTL",
            .handler = [](CommandContext c) { return HandleTtl(c.socket, c.engine, c.tail, TtlUnit::Milliseconds); },
            .arity = 2,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry { .name = "PERSIST",
                       .handler = [](CommandContext c) { return HandlePersist(c.socket, c.engine, c.state, c.tail); },
                       .arity = 2,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        // Integer atomics. INCR/DECR are fixed-magnitude; INCRBY/DECRBY take a
        // signed delta as a second arg (DECRBY negates before applying).
        CommandEntry {
            .name = "INCR",
            .handler = [](CommandContext c) { return HandleIncrDecr(c.socket, c.engine, c.state, c.tail, "incr", +1); },
            .arity = 2,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry {
            .name = "DECR",
            .handler = [](CommandContext c) { return HandleIncrDecr(c.socket, c.engine, c.state, c.tail, "decr", -1); },
            .arity = 2,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry { .name = "INCRBY",
                       .handler =
                           [](CommandContext c) {
                               return HandleIncrDecrByVerb(c.socket, c.engine, c.state, c.tail, "incrby", /*negate*/ false);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "DECRBY",
                       .handler =
                           [](CommandContext c) {
                               return HandleIncrDecrByVerb(c.socket, c.engine, c.state, c.tail, "decrby", /*negate*/ true);
                           },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        // Batch GET/SET. MSET / MSETNX have a 2-step keyStep (key, value, key,
        // value, …); MGET is a contiguous key list.
        CommandEntry { .name = "MGET",
                       .handler = [](CommandContext c) { return HandleMget(c.socket, c.engine, c.tail, c.state->resp); },
                       .arity = -2,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 1 },
        CommandEntry { .name = "MSET",
                       .handler = [](CommandContext c) { return HandleMset(c.socket, c.engine, c.state, c.tail); },
                       .arity = -3,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 2 },
        CommandEntry { .name = "MSETNX",
                       .handler = [](CommandContext c) { return HandleMsetNx(c.socket, c.engine, c.state, c.tail); },
                       .arity = -3,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 2 },
        CommandEntry { .name = "PING",
                       .handler = [](CommandContext c) { return HandlePing(c.socket, c.tail); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "ECHO",
                       .handler = [](CommandContext c) { return HandleEcho(c.socket, c.tail); },
                       .arity = 2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "INFO",
                       .handler = [](CommandContext c) { return HandleInfo(c.socket, c.engine, c.state->resp); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "HELLO",
                       .handler = [](CommandContext c) { return HandleHello(c.socket, c.tail, c.session, c.state); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "COMMAND",
                       .handler = [](CommandContext c) { return HandleCommand(c.socket, c.tail, c.state->resp); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "FLUSHDB",
                       .handler = [](CommandContext c) { return HandleFlush(c.socket, c.engine, c.state); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "FLUSHALL",
                       .handler = [](CommandContext c) { return HandleFlush(c.socket, c.engine, c.state); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "SELECT",
                       .handler = [](CommandContext c) { return HandleSelect(c.socket, c.tail); },
                       .arity = 2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "CLIENT",
                       .handler = [](CommandContext c) { return HandleClient(c.socket, c.tail); },
                       .arity = -2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "CONFIG",
                       .handler = [](CommandContext c) { return HandleConfig(c.socket, c.tail, c.state->resp); },
                       .arity = -2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "SADD",
                       .handler = [](CommandContext c) { return HandleSAdd(c.socket, c.engine, c.state, c.tail); },
                       .arity = -3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "SREM",
                       .handler = [](CommandContext c) { return HandleSRem(c.socket, c.engine, c.state, c.tail); },
                       .arity = -3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "SMEMBERS",
                       .handler = [](CommandContext c) { return HandleSMembers(c.socket, c.engine, c.tail, c.state->resp); },
                       .arity = 2,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry {
            .name = "SISMEMBER",
            .handler = [](CommandContext c) { return HandleSIsMember(c.socket, c.engine, c.tail, c.state->resp); },
            .arity = 3,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry {
            .name = "SMISMEMBER",
            .handler = [](CommandContext c) { return HandleSMIsMember(c.socket, c.engine, c.tail, c.state->resp); },
            .arity = -3,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry { .name = "SCARD",
                       .handler = [](CommandContext c) { return HandleSCard(c.socket, c.engine, c.tail); },
                       .arity = 2,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry {
            .name = "SPOP",
            .handler = [](CommandContext c) { return HandleSPop(c.socket, c.engine, c.state, c.tail, c.state->resp); },
            .arity = -2,
            .firstKey = 1,
            .lastKey = 1,
            .keyStep = 1 },
        CommandEntry { .name = "LOLWUT",
                       .handler = [](CommandContext c) { return HandleLolwut(c.socket, c.state->resp); },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "INCRBYFLOAT",
                       .handler = [](CommandContext c) { return HandleIncrByFloat(c.socket, c.engine, c.state, c.tail); },
                       .arity = 3,
                       .firstKey = 1,
                       .lastKey = 1,
                       .keyStep = 1 },
        CommandEntry { .name = "DEBUG",
                       .handler = [](CommandContext c) { return HandleDebug(c.socket, c.tail, c.state->resp); },
                       .arity = -2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "PUBLISH",
                       .handler = [](CommandContext c) { return HandlePublish(c.socket, c.session, c.tail); },
                       .arity = 3,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "SUBSCRIBE",
                       .handler =
                           [](CommandContext c) {
                               return HandleSubscribeFamily(c.socket,
                                                            c.session,
                                                            c.tail,
                                                            SubscribeVerb { .label = "subscribe",
                                                                            .method = &SubscribeAdapter,
                                                                            .subscribing = true,
                                                                            .pattern = false },
                                                            c.state);
                           },
                       .arity = -2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "UNSUBSCRIBE",
                       .handler =
                           [](CommandContext c) {
                               return HandleSubscribeFamily(c.socket,
                                                            c.session,
                                                            c.tail,
                                                            SubscribeVerb { .label = "unsubscribe",
                                                                            .method = &UnsubscribeAdapter,
                                                                            .subscribing = false,
                                                                            .pattern = false },
                                                            c.state);
                           },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "PSUBSCRIBE",
                       .handler =
                           [](CommandContext c) {
                               return HandleSubscribeFamily(c.socket,
                                                            c.session,
                                                            c.tail,
                                                            SubscribeVerb { .label = "psubscribe",
                                                                            .method = &PSubscribeAdapter,
                                                                            .subscribing = true,
                                                                            .pattern = true },
                                                            c.state);
                           },
                       .arity = -2,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "MULTI",
                       .handler = [](CommandContext c) { return HandleMulti(c.socket, c.state); },
                       .arity = 1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "EXEC",
                       .handler = [](CommandContext c) { return HandleExec(c.socket, c.engine, c.session, c.state); },
                       .arity = 1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "DISCARD",
                       .handler = [](CommandContext c) { return HandleDiscard(c.socket, c.state); },
                       .arity = 1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "WATCH",
                       .handler = [](CommandContext c) { return HandleWatch(c.socket, c.engine, c.state, c.tail); },
                       .arity = -2,
                       .firstKey = 1,
                       .lastKey = -1,
                       .keyStep = 1 },
        CommandEntry { .name = "UNWATCH",
                       .handler = [](CommandContext c) { return HandleUnwatch(c.socket, c.state); },
                       .arity = 1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
        CommandEntry { .name = "PUNSUBSCRIBE",
                       .handler =
                           [](CommandContext c) {
                               return HandleSubscribeFamily(c.socket,
                                                            c.session,
                                                            c.tail,
                                                            SubscribeVerb { .label = "punsubscribe",
                                                                            .method = &PUnsubscribeAdapter,
                                                                            .subscribing = false,
                                                                            .pattern = true },
                                                            c.state);
                           },
                       .arity = -1,
                       .firstKey = 0,
                       .lastKey = 0,
                       .keyStep = 0 },
    };

    /// Definitions for the `COMMAND` introspection accessors. Kept here
    /// rather than as direct table iteration in HandleCommand because the
    /// table is defined below HandleCommand (it references the handler
    /// functions and would otherwise have to be split across two
    /// translation passes). One source of truth: every accessor reads the
    /// same row of CommandTable.
    std::size_t CommandTableSize() noexcept
    {
        return CommandTable.size();
    }
    std::string_view CommandTableName(std::size_t index) noexcept
    {
        return CommandTable[index].name;
    }
    std::int32_t CommandTableArity(std::size_t index) noexcept
    {
        return CommandTable[index].arity;
    }
    std::int32_t CommandTableFirstKey(std::size_t index) noexcept
    {
        return CommandTable[index].firstKey;
    }
    std::int32_t CommandTableLastKey(std::size_t index) noexcept
    {
        return CommandTable[index].lastKey;
    }
    std::int32_t CommandTableKeyStep(std::size_t index) noexcept
    {
        return CommandTable[index].keyStep;
    }

    std::optional<std::size_t> CommandTableFind(std::string_view upperName) noexcept
    {
        for (std::size_t i = 0; i < CommandTable.size(); ++i)
            if (CommandTable[i].name == upperName)
                return i;
        return std::nullopt;
    }

    Task<bool> CommandTableInvoke(std::size_t index, CommandContext ctx)
    {
        return CommandTable[index].handler(ctx);
    }

    /// RESET — redis's "reset this connection to a clean state". Clears
    /// subscriptions, resets the negotiated protocol to RESP2, and replies
    /// `+RESET\r\n`. Listed in IsAllowedInSubscribeMode but lacked a handler;
    /// without this, redis-cli and connection-pool clients that issue RESET
    /// to escape pub/sub mode would get `-ERR unknown command 'RESET'` and
    /// stay stuck in subscribe mode.
    /// @param socket  Current connection socket.
    /// @param session Per-server collaborators (the registry to unsubscribe from).
    /// @param state   Per-connection state to clear.
    /// @return Always true; RESET never ends the session.
    Task<bool> HandleReset(ISocket* socket, SessionContext session, ConnectionState* state)
    {
        if (session.pubsub != nullptr && state->subscriber)
            session.pubsub->UnsubscribeAll(state->subscriber.get());
        state->subscriptionCount = 0;
        state->resp = RespVersion::Resp2;
        // Redis RESET also clears any in-flight transaction and WATCH set.
        state->inMulti = false;
        state->multiDirty = false;
        state->queue.clear();
        state->queueBytes = 0;
        if (state->watch && state->watchRegistry != nullptr)
            state->watchRegistry->UnregisterAll(state->watch.get());
        co_return co_await WriteAll(socket, "+RESET\r\n");
    }

    /// Verify the AUTH credential against the policy and flip `state` on success.
    /// Factored out so Dispatch's pre-table prologue stays readable.
    Task<bool> HandleAuth(ISocket* socket, std::span<std::string const> tail, SessionContext session, ConnectionState* state)
    {
        auto const auth = session.CurrentAuth();
        if (auth == nullptr || !auth->Enabled())
            co_return co_await ReplyError(socket, "Client sent AUTH, but no password is set");
        bool ok = false;
        if (tail.size() == 1)
            ok = auth->Verify(tail[0]);
        else if (tail.size() == 2)
            ok = auth->Verify(tail[0], tail[1]);
        else
            co_return co_await ReplyError(socket, "wrong number of arguments for 'auth' command");
        if (!ok)
            co_return co_await WriteAll(socket, "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
        state->authenticated = true;
        co_return co_await ReplyOk(socket);
    }

    Task<bool> Dispatch(
        ISocket* socket, CacheEngine* engine, ParsedCommand cmd, SessionContext session, ConnectionState* state)
    {
        if (cmd.args.empty())
            co_return true;

        auto const name = Upper(cmd.args[0]);
        auto const tail = std::span<std::string const> { cmd.args.data() + 1, cmd.args.size() - 1 };

        auto const auth = session.CurrentAuth();
        bool const authEnabled = auth != nullptr && auth->Enabled();
        if (authEnabled && !state->authenticated && !IsPreAuthAllowed(name))
            co_return co_await WriteAll(socket, "-NOAUTH Authentication required.\r\n");

        // Inside `MULTI`, every non-transaction-control command is queued
        // instead of executed; the dispatcher then replies `+QUEUED`. We
        // still consult the table for an arity / unknown-command check at
        // queue time so the matching `EXEC` aborts (`-EXECABORT`) — matches
        // redis's `MULTI_DIRTY_EXEC` semantics. The queue branch runs BEFORE
        // the AUTH/QUIT/RESET fast-track below: a few side-effect verbs
        // (notably QUIT and the SUBSCRIBE family) would otherwise tear down
        // the session or split EXEC's `*N` aggregate header from the actual
        // element count — see `IsForbiddenInMulti`.
        if (state->inMulti && !RunsEvenInsideMulti(name))
        {
            // Once the transaction is dirty (cap breach, unknown verb,
            // wrong arity, forbidden verb), do NOT re-queue further
            // commands — the eventual EXEC will abort with -EXECABORT
            // regardless of what's in the queue. Pre-fix, the queue was
            // re-fillable up to the cap repeatedly, sustaining 256 MiB of
            // allocate/clear churn per misbehaving connection forever
            // without EXEC. Reply the same dirty-transaction error each
            // time so the client cannot infer whether its command was
            // accepted: ambiguity is the correct stance once the
            // transaction is already doomed.
            if (state->multiDirty)
                co_return co_await ReplyError(socket, "transaction discarded — send EXEC or DISCARD");
            if (IsForbiddenInMulti(name))
            {
                state->multiDirty = true;
                co_return co_await ReplyError(socket, std::format("{} is not allowed inside a transaction", name));
            }
            auto const idx = CommandTableFind(name);
            if (!idx.has_value())
            {
                state->multiDirty = true;
                co_return co_await ReplyError(socket, std::format("unknown command '{}'", name));
            }
            auto const arity = CommandTableArity(*idx);
            auto const actual = static_cast<std::int32_t>(cmd.args.size());
            bool const arityOk = (arity >= 0) ? (actual == arity) : (actual >= -arity);
            if (!arityOk)
            {
                state->multiDirty = true;
                co_return co_await ReplyError(socket, std::format("wrong number of arguments for '{}'", name));
            }
            // Enforce the MULTI queue caps before we copy the args into our
            // owned storage — a single client must not be able to OOM the
            // daemon by streaming SETs without ever sending EXEC. The
            // multiDirty bail above prevents the post-breach refill loop
            // (one breach = one drop, not endless allocate/clear churn).
            std::size_t argvBytes = 0;
            for (auto const& arg: cmd.args)
                argvBytes += arg.size();
            if (state->queue.size() >= state->maxQueuedCommands || state->queueBytes + argvBytes > state->maxQueuedBytes)
            {
                state->multiDirty = true;
                // Drop the existing queue immediately to reclaim memory; the
                // dirty flag ensures EXEC still aborts deterministically.
                state->queue.clear();
                state->queueBytes = 0;
                co_return co_await ReplyError(socket, "transaction queue exceeded per-connection limit");
            }
            state->queueBytes += argvBytes;
            // Stash the resolved CommandTable index alongside the argv —
            // EXEC will use it to bypass Dispatch's prologue.
            state->queue.push_back(ConnectionState::QueuedCommand { .argv = std::move(cmd.args), .commandTableIdx = *idx });
            co_return co_await WriteAll(socket, "+QUEUED\r\n");
        }

        // AUTH, QUIT and RESET carry side-effects beyond a plain reply, so they
        // sit outside the data table; every other command is a table lookup.
        if (name == "AUTH")
            co_return co_await HandleAuth(socket, tail, session, state);
        if (name == "QUIT")
        {
            (void) co_await ReplyOk(socket);
            socket->Close();
            co_return false; // signal session end
        }
        if (name == "RESET")
            co_return co_await HandleReset(socket, session, state);

        // Transaction-control verbs sit outside the data table because they
        // need ConnectionState (queue + watch handle) directly.
        if (name == "MULTI")
            co_return co_await HandleMulti(socket, state);
        if (name == "EXEC")
            co_return co_await HandleExec(socket, engine, session, state);
        if (name == "DISCARD")
            co_return co_await HandleDiscard(socket, state);
        if (name == "WATCH")
            co_return co_await HandleWatch(socket, engine, state, tail);
        if (name == "UNWATCH")
            co_return co_await HandleUnwatch(socket, state);

        // Table lookup by name. A range-based scan keeps this portable: MSVC's
        // std::array iterator is a class type (not a raw pointer like libstdc++),
        // so storing the std::ranges::find result in an `auto*` would not deduce.
        CommandHandler handler = nullptr;
        for (auto const& entry: CommandTable)
            if (entry.name == name)
            {
                handler = entry.handler;
                break;
            }
        if (handler != nullptr)
            co_return co_await handler(
                CommandContext { .socket = socket, .engine = engine, .tail = tail, .state = state, .session = session });

        co_return co_await ReplyError(socket, std::format("unknown command '{}'", name));
    }

    /// Commands a connection may run while in subscribe mode (after SUBSCRIBE).
    /// Mirrors redis: only the (un)subscribe verbs plus PING / QUIT / RESET.
    [[nodiscard]] bool IsAllowedInSubscribeMode(std::string_view name) noexcept
    {
        constexpr auto Allowed = std::array<std::string_view, 8> {
            "SUBSCRIBE", "UNSUBSCRIBE", "PSUBSCRIBE", "PUNSUBSCRIBE", "PING", "QUIT", "RESET", "HELLO",
        };
        return std::ranges::find(Allowed, name) != Allowed.end();
    }

    /// Flush any pub/sub messages queued for the subscriber to the socket as push
    /// frames. Runs on the connection's own reactor thread.
    /// @return False if a socket write failed (caller should end the session).
    Task<bool> DrainPushes(ISocket* socket, Subscriber* subscriber, RespVersion resp)
    {
        for (auto& message: subscriber->DrainQueue())
            if (!co_await WritePushMessage(socket, std::move(message), resp))
                co_return false;
        co_return true;
    }

} // namespace

std::string_view RedisRespHandler::ServerVersion() noexcept
{
    return ServerVersionBanner;
}

void RedisRespHandler::OverrideMultiQueueCapsForTests(std::size_t maxCommands, std::size_t maxBytes) noexcept
{
    _testMaxQueuedCommands = maxCommands;
    _testMaxQueuedBytes = maxBytes;
}

Task<void> RedisRespHandler::Run(ISocket* socket,
                                 CacheEngine* engine,
                                 std::vector<std::byte> primingBytes,
                                 SessionContext session)
{
    ByteReader reader { *socket, MaxLineBytes, MaxPayloadBytes };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

    // Per-connection protocol state. Authenticated up-front unless a credential
    // is required (AUTH / HELLO ... AUTH flips it); RESP2 until HELLO 3 upgrades.
    // Dispatch re-checks the live source on every command so a SIGHUP that
    // enables auth mid-session begins gating on the next request.
    ConnectionState state;
    auto const initialAuth = session.CurrentAuth();
    state.authenticated = !(initialAuth != nullptr && initialAuth->Enabled());
    // Cache the WATCH registry and keyspace-notifier pointers once so every
    // write-verb mutation hook can call them through `state` rather than
    // chasing them through SessionContext at every callsite.
    state.watchRegistry = session.watches;
    state.keyspaceNotifier = session.keyspaceNotifier;
    // Snapshot "is the notifier configured to publish anything at all?" once
    // per connection. Same shape as the watchRegistry pointer cache: a
    // mid-session reload of `notify-keyspace-events` does not affect
    // already-running connections, by design.
    state.keyspaceEnabled = state.keyspaceNotifier != nullptr && state.keyspaceNotifier->IsEnabled();
    // Honour test-only cap overrides set via OverrideMultiQueueCapsForTests.
    if (_testMaxQueuedCommands > 0)
        state.maxQueuedCommands = _testMaxQueuedCommands;
    if (_testMaxQueuedBytes > 0)
        state.maxQueuedBytes = _testMaxQueuedBytes;

    // The subscriber is heap-allocated and held via shared_ptr so the registry's
    // weak_ptr upgrade in Publish (and the long-lived readable-watcher detached
    // task) can pin its lifetime past this frame's teardown: the classic UAF was
    // a Publish snapshotting a raw pointer, the loop cleaning up + co_return-ing,
    // and Deliver landing on freed memory. The shared_ptr count drops to zero
    // only after every Publish-in-flight completes and the watcher exits.
    auto const subscriber = std::make_shared<Subscriber>(session.reactor);
    if (session.pubsub != nullptr)
        state.subscriber = subscriber;

    // RAII teardown: runs on every exit path, INCLUDING coroutine-frame
    // unwind from an exception. The original shape was a `cleanup` lambda
    // called manually at every `co_return`; an exception thrown inside
    // `Dispatch` (or any deeper co_await) bypassed the lambda and left
    // WATCH-index entries referencing this connection's now-dying handle.
    // The weak_ptr still expired cleanly, but the orphaned `_index` rows
    // accumulated under churn until some other touch happened on the same
    // key. The scope guard closes that gap.
    class Cleanup
    {
      public:
        Cleanup(SessionContext const& sess, std::shared_ptr<Subscriber> sub, ConnectionState& st) noexcept:
            _session { sess },
            _subscriber { std::move(sub) },
            _state { st }
        {
        }
        Cleanup(Cleanup const&) = delete;
        Cleanup(Cleanup&&) = delete;
        Cleanup& operator=(Cleanup const&) = delete;
        Cleanup& operator=(Cleanup&&) = delete;
        ~Cleanup()
        {
            // Run each cleanup step under SwallowDestructorException so a
            // throw from one (e.g. a std::scoped_lock raising
            // std::system_error under abnormal pthread state — mutex
            // resource exhaustion, PRIO_PI inheritance failure) does
            // NOT std::terminate the process before the remaining steps
            // run. The destructor is implicitly noexcept; a propagating
            // exception would otherwise leak the watcher coroutine frame
            // AND stale WATCH index entries until process exit.
            Detail::SwallowDestructorException([this] {
                if (_session.pubsub != nullptr)
                    _session.pubsub->UnsubscribeAll(_subscriber.get());
            });
            // Trip any latch the watcher is parked on so its coroutine frame is
            // freed promptly, instead of leaking until the daemon exits.
            Detail::SwallowDestructorException([this] { _subscriber->ShutdownWatcher(); });
            // Drop every WATCH index entry referencing this connection so a
            // stale weak_ptr cannot be upgraded by a later Touched call.
            Detail::SwallowDestructorException([this] {
                if (_state.watch && _state.watchRegistry != nullptr)
                    _state.watchRegistry->UnregisterAll(_state.watch.get());
            });
        }

      private:
        SessionContext const& _session;
        std::shared_ptr<Subscriber> _subscriber;
        ConnectionState& _state;
    };
    Cleanup const cleanup { session, subscriber, state };

    while (true)
    {
        // In subscribe mode the loop must wake on EITHER an incoming command OR a
        // delivered message. A single long-lived watcher owns the only
        // outstanding socket->WaitReadable() and flags `_readablePending`; the
        // push arm is the registry's Deliver. Park on the combined awaitable,
        // drain messages on wake, and only read a command when the watcher says
        // bytes are pending (or the reader already buffered a full command) —
        // never blocking in ReadOneCommand while messages could arrive.
        if (state.subscriptionCount > 0)
        {
            subscriber->StartReadableWatcher(socket);
            if (!co_await DrainPushes(socket, subscriber.get(), state.resp))
                co_return;
            if (reader.Buffered().empty() && !subscriber->ReadablePending())
            {
                co_await subscriber->WaitForPushOrReadable();
                if (!co_await DrainPushes(socket, subscriber.get(), state.resp))
                    co_return;
                // Woken by a push (no readable bytes): loop back to wait again.
                if (reader.Buffered().empty() && !subscriber->ReadablePending())
                    continue;
            }
        }

        auto cmd = co_await ReadOneCommand(&reader);
        // The watcher flagged readable bytes; we have now consumed a command from
        // them, so let it re-arm for the next readability edge.
        if (state.subscriptionCount > 0)
            subscriber->RearmReadable();
        if (!cmd.has_value())
            co_return; // truncated / malformed — drop connection (RAII cleans up)
        if (cmd->args.empty())
            continue;

        // Subscribe-mode command restriction: once subscribed, only the pub/sub
        // verbs plus PING/QUIT/RESET/HELLO are permitted.
        if (state.subscriptionCount > 0)
        {
            auto const name = Upper(cmd->args[0]);
            if (!IsAllowedInSubscribeMode(name))
            {
                (void) co_await ReplyError(socket,
                                           std::format("Can't execute '{}': only (P)SUBSCRIBE / (P)UNSUBSCRIBE / PING / "
                                                       "QUIT / RESET are allowed in this context",
                                                       name));
                continue;
            }
        }

        auto const keepGoing = co_await Dispatch(socket, engine, std::move(*cmd), session, &state);
        if (!keepGoing)
            co_return; // RAII cleanup fires on the way out

        // One command handled and replied — mark the request frame.
        FC_FRAME_MARK;
    }
}

} // namespace FastCache
