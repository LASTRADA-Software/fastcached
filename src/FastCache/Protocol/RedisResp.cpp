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
#include <FastCache/Protocol/RedisResp.hpp>

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
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
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
            // The watcher takes a shared_ptr by value so its coroutine frame
            // keeps the Subscriber alive even after Run's frame has returned —
            // closing the second UAF where a parked WaitReadable resumed onto
            // a destroyed Subscriber's mutex.
            RunReadableWatcher(SharedFromThisAsSubscriber(), socket);
        }

        /// Wake any latch the watcher is parked on so its coroutine frame
        /// completes promptly when the owning connection ends; otherwise a
        /// watcher parked on the rearm latch (after the loop consumed bytes
        /// but the connection exited before re-entering subscribe mode) would
        /// never resume and the coroutine frame would leak.
        void ShutdownWatcher() noexcept
        {
            std::shared_ptr<WakeLatch> latch;
            {
                std::scoped_lock const lock { _mu };
                _shuttingDown = true;
                latch = std::exchange(_rearm, {});
            }
            if (latch)
                latch->WakeOnce(_reactor);
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
        bool _readablePending { false };      ///< Watcher saw readable bytes.
        bool _watcherStarted { false };       ///< StartReadableWatcher ran.
        bool _shuttingDown { false };         ///< Connection torn down; watcher should exit.
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
    };

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
    /// We can't use `std::from_chars<double>` because the floating-point
    /// from_chars overload is unavailable on some libc++ versions (e.g. macOS
    /// before 26.0). Plain `std::strtod` honours LC_NUMERIC, so on a
    /// non-C-locale host (e.g. de_DE.UTF-8 with `,` as the decimal separator)
    /// it would refuse to parse the same wire-format the daemon's own
    /// `std::format` writes ('.'-separated). Fix: validate that the input
    /// contains only locale-neutral characters from the redis grammar
    /// (`[-+]?[0-9]*(\.[0-9]+)?([eE][-+]?[0-9]+)?`) and only then hand the
    /// canonicalised text to strtod. Under a non-C LC_NUMERIC strtod will
    /// reject the '.' as junk and our trailing-garbage check fires — but
    /// because we have already validated the input is well-formed under the
    /// C locale, the only way that can happen is the locale itself, and we
    /// can correct it by retrying with a manual decimal-point swap if needed.
    /// In practice, validating + using strtod is sufficient on every platform
    /// we target; the validator below catches truly invalid inputs and the
    /// strtod call is gated to a permitted character set.
    /// @param sv  Text to parse.
    /// @param out Receives the parsed value on success.
    /// @return True iff the whole string is a finite double.
    [[nodiscard]] bool ParseDouble(std::string_view sv, double& out) noexcept
    {
        if (sv.empty())
            return false;
        // Locale-neutral pre-validation: every byte must be from the redis
        // numeric grammar. This both rejects garbage early (e.g. embedded NUL,
        // whitespace, locale-specific decimal separators like ',') and makes
        // the subsequent strtod call safe under any LC_NUMERIC: strtod can
        // only encounter ASCII digits, '.', '+', '-', 'e', 'E'.
        bool sawDot = false;
        bool sawExp = false;
        bool sawDigit = false;
        for (auto const i: std::views::iota(std::size_t { 0 }, sv.size()))
        {
            auto const c = sv[i];
            if (c >= '0' && c <= '9')
            {
                sawDigit = true;
            }
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
            {
                return false;
            }
        }
        if (!sawDigit)
            return false;
        // The validator above guarantees ASCII; strtod under any locale will
        // either parse the whole string or stop at '.' (under a non-'.'-locale
        // locale). For maximum portability and to side-step the LC_NUMERIC
        // hazard entirely on hosts whose default locale is not C, build a
        // manual sign+digits+exponent reading via `from_chars` for the integer
        // pieces and recombine.
        std::string const text { sv }; // strtod needs a NUL-terminated buffer.
        char* end = nullptr;
        errno = 0;
        auto const value = std::strtod(text.c_str(), &end);
        if (errno == 0 && end == text.c_str() + text.size() && std::isfinite(value))
        {
            out = value;
            return true;
        }
        // strtod refused (likely a non-'.'-LC_NUMERIC); reconstruct manually.
        // Split at the 'e'/'E', integer/fractional at '.'. All pieces are pure
        // digit runs (plus an optional leading sign on the mantissa and
        // exponent), validated above.
        std::size_t expPos = sv.size();
        for (auto const i: std::views::iota(std::size_t { 0 }, sv.size()))
            if (sv[i] == 'e' || sv[i] == 'E')
            {
                expPos = i;
                break;
            }
        auto const mantissaSv = sv.substr(0, expPos);
        long exp = 0;
        if (expPos < sv.size())
        {
            auto const expSv = sv.substr(expPos + 1);
            auto const [ptr, ec] = std::from_chars(expSv.data(), expSv.data() + expSv.size(), exp);
            if (ec != std::errc {} || ptr != expSv.data() + expSv.size())
                return false;
        }
        int sign = 1;
        std::size_t mPos = 0;
        if (mPos < mantissaSv.size() && (mantissaSv[mPos] == '+' || mantissaSv[mPos] == '-'))
        {
            if (mantissaSv[mPos] == '-')
                sign = -1;
            ++mPos;
        }
        auto const intStart = mPos;
        while (mPos < mantissaSv.size() && mantissaSv[mPos] != '.')
            ++mPos;
        auto const intPart = mantissaSv.substr(intStart, mPos - intStart);
        std::string_view fracPart;
        if (mPos < mantissaSv.size())
            fracPart = mantissaSv.substr(mPos + 1);
        // Reassemble as integer-only mantissa with adjusted exponent so we can
        // call from_chars<long long> once and then scale by 10^exp.
        std::string concat;
        concat.reserve(intPart.size() + fracPart.size());
        concat.append(intPart);
        concat.append(fracPart);
        auto const adjustedExp = exp - static_cast<long>(fracPart.size());
        long long mantissa = 0;
        if (!concat.empty())
        {
            auto const [ptr, ec] = std::from_chars(concat.data(), concat.data() + concat.size(), mantissa);
            if (ec != std::errc {} || ptr != concat.data() + concat.size())
                return false;
        }
        auto const result =
            static_cast<double>(sign) * static_cast<double>(mantissa) * std::pow(10.0, static_cast<double>(adjustedExp));
        if (!std::isfinite(result))
            return false;
        out = result;
        return true;
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

    /// Optional argument parsing for SET command flags (EX/PX/NX/XX).
    struct SetOptions
    {
        bool nx { false };
        bool xx { false };
        std::uint32_t exptime { 0 };
    };

    [[nodiscard]] std::expected<SetOptions, std::string> ParseSetOptions(std::span<std::string const> tail)
    {
        SetOptions opts;
        for (std::size_t i = 0; i < tail.size(); ++i)
        {
            auto const tok = Upper(tail[i]);
            if (tok == "NX")
                opts.nx = true;
            else if (tok == "XX")
                opts.xx = true;
            else if (tok == "EX" || tok == "PX")
            {
                if (i + 1 >= tail.size())
                    return std::unexpected(std::string { "missing argument for " } + tok);
                std::uint64_t raw = 0;
                if (!ParseUnsigned(std::string_view { tail[i + 1] }, raw))
                    return std::unexpected(std::string { "bad number for " } + tok);
                opts.exptime =
                    tok == "EX" ? static_cast<std::uint32_t>(raw) : static_cast<std::uint32_t>((raw + 999) / 1000);
                ++i;
            }
            else
                return std::unexpected("unknown SET option: " + tok);
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

    Task<bool> HandleSet(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'set'");

        auto const& key = args[0];
        auto const& value = args[1];
        auto const opts = ParseSetOptions(args.subspan(2));
        if (!opts.has_value())
            co_return co_await ReplyError(socket, opts.error());

        std::vector<std::byte> bytes;
        bytes.reserve(value.size());
        for (auto const c: value)
            bytes.push_back(static_cast<std::byte>(c));

        std::expected<CasToken, StorageError> result { 0 };
        if (opts->nx)
            result = engine->Add(key, std::move(bytes), 0, opts->exptime);
        else if (opts->xx)
            result = engine->Replace(key, std::move(bytes), 0, opts->exptime);
        else
            result = engine->Set(key, std::move(bytes), 0, opts->exptime);

        if (result.has_value())
            co_return co_await ReplyOk(socket);
        if (result.error().code == StorageErrorCode::KeyExists || result.error().code == StorageErrorCode::KeyNotFound)
            co_return co_await ReplyNull(socket, resp); // NX/XX precondition unmet.
        if (result.error().code == StorageErrorCode::ValueTooLarge)
            co_return co_await ReplyError(socket, "value too large");
        co_return co_await ReplyError(socket, "storage failure");
    }

    Task<bool> HandleSetEx(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, bool millis)
    {
        if (args.size() != 3)
            co_return co_await ReplyError(
                socket, millis ? "wrong number of arguments for 'psetex'" : "wrong number of arguments for 'setex'");
        std::uint64_t raw = 0;
        if (!ParseUnsigned(std::string_view { args[1] }, raw))
            co_return co_await ReplyError(socket, "ttl must be a number");
        auto const exptime = millis ? static_cast<std::uint32_t>((raw + 999) / 1000) : static_cast<std::uint32_t>(raw);

        std::vector<std::byte> bytes;
        bytes.reserve(args[2].size());
        for (auto const c: args[2])
            bytes.push_back(static_cast<std::byte>(c));
        auto const result = engine->Set(args[0], std::move(bytes), 0, exptime);
        if (!result.has_value())
            co_return co_await ReplyError(socket, "storage failure");
        co_return co_await ReplyOk(socket);
    }

    Task<bool> HandleDel(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'del'");
        std::int64_t deleted = 0;
        for (auto const& key: args)
        {
            auto const result = engine->Delete(key);
            if (result.has_value())
                ++deleted;
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

    Task<bool> HandleCommand(ISocket* socket, std::span<std::string const> args)
    {
        // Always reply with an empty array; sccache only uses COMMAND
        // DOCS/COUNT for sanity checks.
        static_cast<void>(args);
        co_return co_await WriteAll(socket, "*0\r\n");
    }

    Task<bool> HandleFlush(ISocket* socket, CacheEngine* engine)
    {
        engine->FlushAll(0);
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

    Task<bool> HandleSAdd(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'sadd'");
        auto const added = engine->SetAdd(args[0], args.subspan(1));
        if (!added.has_value())
            co_return co_await ReplySetError(socket, added.error());
        co_return co_await ReplyInteger(socket, *added);
    }

    Task<bool> HandleSRem(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
    {
        if (args.size() < 2)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'srem'");
        auto const removed = engine->SetRemove(args[0], args.subspan(1));
        if (!removed.has_value())
            co_return co_await ReplySetError(socket, removed.error());
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

    Task<bool> HandleSPop(ISocket* socket, CacheEngine* engine, std::span<std::string const> args, RespVersion resp)
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
    Task<bool> HandleIncrByFloat(ISocket* socket, CacheEngine* engine, std::span<std::string const> args)
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
            if (result.error().code == StorageErrorCode::NotANumber)
                co_return co_await ReplyError(socket, "value is not a valid float");
            if (result.error().code == StorageErrorCode::InfiniteOrNaN)
                co_return co_await ReplyError(socket, "increment would produce NaN or Infinity");
            co_return co_await ReplyError(socket, "storage failure");
        }
        co_return co_await ReplyBulkString(
            socket,
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(encodedText.data()), encodedText.size() });
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

    /// Everything a command handler needs, bundled so the dispatch table can hold
    /// one uniform handler signature. Pointers borrow from Dispatch's frame /
    /// SessionContext and outlive the awaited handler.
    struct CommandContext
    {
        ISocket* socket;
        CacheEngine* engine;
        std::span<std::string const> tail; ///< Arguments after the command name.
        ConnectionState* state;
        SessionContext session; ///< Per-server collaborators (auth, pub/sub).
    };

    using CommandHandler = Task<bool> (*)(CommandContext);

    /// One row of the command dispatch table: a command name and its handler.
    /// Aliases (DEL/UNLINK, FLUSHDB/FLUSHALL) are separate rows sharing a handler.
    struct CommandEntry
    {
        std::string_view name;
        CommandHandler handler;
    };

    /// Data-driven command dispatch table. Each handler is a captureless lambda
    /// (so it decays to a function pointer) that unpacks the CommandContext and
    /// forwards to the typed Handle* coroutine. Adding a command is one row here.
    /// AUTH and the QUIT side-effect are handled in Dispatch itself because they
    /// need the SessionContext / must end the session.
    constexpr auto CommandTable = std::array {
        CommandEntry { .name = "GET",
                       .handler = [](CommandContext c) { return HandleGet(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry { .name = "SET",
                       .handler = [](CommandContext c) { return HandleSet(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry {
            .name = "SETEX",
            .handler = [](CommandContext c) { return HandleSetEx(c.socket, c.engine, c.tail, /*millis*/ false); } },
        CommandEntry {
            .name = "PSETEX",
            .handler = [](CommandContext c) { return HandleSetEx(c.socket, c.engine, c.tail, /*millis*/ true); } },
        CommandEntry { .name = "DEL", .handler = [](CommandContext c) { return HandleDel(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "UNLINK", .handler = [](CommandContext c) { return HandleDel(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "EXISTS",
                       .handler = [](CommandContext c) { return HandleExists(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "PING", .handler = [](CommandContext c) { return HandlePing(c.socket, c.tail); } },
        CommandEntry { .name = "ECHO", .handler = [](CommandContext c) { return HandleEcho(c.socket, c.tail); } },
        CommandEntry { .name = "INFO",
                       .handler = [](CommandContext c) { return HandleInfo(c.socket, c.engine, c.state->resp); } },
        CommandEntry { .name = "HELLO",
                       .handler = [](CommandContext c) { return HandleHello(c.socket, c.tail, c.session, c.state); } },
        CommandEntry { .name = "COMMAND", .handler = [](CommandContext c) { return HandleCommand(c.socket, c.tail); } },
        CommandEntry { .name = "FLUSHDB", .handler = [](CommandContext c) { return HandleFlush(c.socket, c.engine); } },
        CommandEntry { .name = "FLUSHALL", .handler = [](CommandContext c) { return HandleFlush(c.socket, c.engine); } },
        CommandEntry { .name = "SELECT", .handler = [](CommandContext c) { return HandleSelect(c.socket, c.tail); } },
        CommandEntry { .name = "CLIENT", .handler = [](CommandContext c) { return HandleClient(c.socket, c.tail); } },
        CommandEntry { .name = "CONFIG",
                       .handler = [](CommandContext c) { return HandleConfig(c.socket, c.tail, c.state->resp); } },
        CommandEntry { .name = "SADD", .handler = [](CommandContext c) { return HandleSAdd(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "SREM", .handler = [](CommandContext c) { return HandleSRem(c.socket, c.engine, c.tail); } },
        CommandEntry {
            .name = "SMEMBERS",
            .handler = [](CommandContext c) { return HandleSMembers(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry {
            .name = "SISMEMBER",
            .handler = [](CommandContext c) { return HandleSIsMember(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry {
            .name = "SMISMEMBER",
            .handler = [](CommandContext c) { return HandleSMIsMember(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry { .name = "SCARD",
                       .handler = [](CommandContext c) { return HandleSCard(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "SPOP",
                       .handler = [](CommandContext c) { return HandleSPop(c.socket, c.engine, c.tail, c.state->resp); } },
        CommandEntry { .name = "LOLWUT", .handler = [](CommandContext c) { return HandleLolwut(c.socket, c.state->resp); } },
        CommandEntry { .name = "INCRBYFLOAT",
                       .handler = [](CommandContext c) { return HandleIncrByFloat(c.socket, c.engine, c.tail); } },
        CommandEntry { .name = "DEBUG",
                       .handler = [](CommandContext c) { return HandleDebug(c.socket, c.tail, c.state->resp); } },
        CommandEntry { .name = "PUBLISH",
                       .handler = [](CommandContext c) { return HandlePublish(c.socket, c.session, c.tail); } },
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
                           } },
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
                           } },
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
                           } },
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
                           } },
    };

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

    // The subscriber is heap-allocated and held via shared_ptr so the registry's
    // weak_ptr upgrade in Publish (and the long-lived readable-watcher detached
    // task) can pin its lifetime past this frame's teardown: the classic UAF was
    // a Publish snapshotting a raw pointer, the loop cleaning up + co_return-ing,
    // and Deliver landing on freed memory. The shared_ptr count drops to zero
    // only after every Publish-in-flight completes and the watcher exits.
    auto const subscriber = std::make_shared<Subscriber>(session.reactor);
    if (session.pubsub != nullptr)
        state.subscriber = subscriber;

    auto const cleanup = [&] {
        if (session.pubsub != nullptr)
            session.pubsub->UnsubscribeAll(subscriber.get());
        // Trip any latch the watcher is parked on so its coroutine frame is
        // freed promptly, instead of leaking until the daemon exits.
        subscriber->ShutdownWatcher();
    };

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
            {
                cleanup();
                co_return;
            }
            if (reader.Buffered().empty() && !subscriber->ReadablePending())
            {
                co_await subscriber->WaitForPushOrReadable();
                if (!co_await DrainPushes(socket, subscriber.get(), state.resp))
                {
                    cleanup();
                    co_return;
                }
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
        {
            cleanup();
            co_return; // truncated / malformed — drop connection
        }
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
        {
            cleanup();
            co_return;
        }

        // One command handled and replied — mark the request frame.
        FC_FRAME_MARK;
    }
}

} // namespace FastCache
