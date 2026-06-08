// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/MemcachedMeta.hpp>
#include <FastCache/Protocol/MemcachedShared.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
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

    constexpr std::size_t MaxLineBytes = 4096;
    constexpr std::size_t MaxPayloadBytes = 16 * 1024 * 1024; // 16 MiB

    /// Split a line on whitespace into `out` (cleared first). `maxParts == 0`
    /// (the default) imposes no limit: the line length is already bounded by
    /// ByteReader's MaxLineBytes, so a multi-key `get`/`gets`/`gat`/`gats`
    /// keeps every key instead of silently dropping those past a fixed cap.
    /// Filling a caller-owned vector (cleared, capacity reused) avoids a heap
    /// allocation on every command — the per-request tokenization was one of
    /// the last per-GET allocations on the text hot path.
    /// @param line Source line.
    /// @param out  Destination token list (cleared, then appended to).
    /// @param maxParts Cap on token count (0 = unlimited).
    void Tokenize(std::string_view line, std::vector<std::string_view>& out, std::size_t maxParts = 0)
    {
        out.clear();
        std::size_t i = 0;
        while (i < line.size() && (maxParts == 0 || out.size() < maxParts))
        {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            if (i == line.size())
                break;
            auto const start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t')
                ++i;
            out.emplace_back(line.data() + start, i - start);
        }
    }

    [[nodiscard]] bool ParseCasToken(std::string_view sv, CasToken& out) noexcept
    {
        return ParseUnsigned(sv, out);
    }

    /// True if `tail` is "noreply" — a single-token suffix on storage commands.
    [[nodiscard]] bool IsNoReply(std::string_view token) noexcept
    {
        return token == "noreply";
    }

    Task<bool> WriteError(ISocket* socket, std::string_view code, std::string_view detail = {})
    {
        std::string line { code };
        if (!detail.empty())
        {
            line.push_back(' ');
            line.append(detail);
        }
        line.append(Crlf);
        co_return co_await WriteAll(socket, line);
    }

    Task<bool> WriteLine(ISocket* socket, std::string_view text)
    {
        std::string line { text };
        line.append(Crlf);
        co_return co_await WriteAll(socket, line);
    }

    /// Parse the trailing args of a storage command (after the command name).
    /// Returns false on shape error.
    struct StorageArgs
    {
        std::string_view key;
        std::uint32_t flags;
        std::uint32_t exptime;
        std::uint32_t bytes;
        CasToken cas; // 0 except for the `cas` command
        bool noreply;
    };

    [[nodiscard]] bool ParseStorageArgs(std::span<std::string_view const> parts, bool isCas, StorageArgs& out)
    {
        // `set/add/replace/append/prepend <key> <flags> <exptime> <bytes> [noreply]`  → 4 required
        // `cas <key> <flags> <exptime> <bytes> <cas-token> [noreply]`               → 5 required
        auto const required = isCas ? std::size_t { 5 } : std::size_t { 4 };
        if (parts.size() < required)
            return false;

        out.key = parts[0];
        if (!ParseUnsigned(parts[1], out.flags))
            return false;
        if (!ParseUnsigned(parts[2], out.exptime))
            return false;
        if (!ParseUnsigned(parts[3], out.bytes))
            return false;
        out.cas = 0;
        std::size_t consumed = 4;
        if (isCas)
        {
            if (!ParseCasToken(parts[4], out.cas))
                return false;
            consumed = 5;
        }
        out.noreply = parts.size() > consumed && IsNoReply(parts[consumed]);
        return true;
    }

    /// Upper bound on a `VALUE` header line: "VALUE " + key (memcached caps
    /// keys at 250) + three space-separated unsigned decimals (flags up to
    /// 10 digits, size up to 20, cas up to 20) + spaces + CRLF, with slack.
    constexpr std::size_t ValueHeaderCap = 250 + 96;

    /// Format the `VALUE <key> <flags> <bytes>[ <cas>]\r\n` *header line* into a
    /// caller-owned fixed buffer (no heap allocation), returning a view of the
    /// written bytes. The value bytes themselves are a separate zero-copy
    /// gather segment, so this emits only the small line. The buffer must
    /// outlive the returned view (e.g. it lives in the coroutine frame across
    /// the write).
    /// @param out Destination buffer, at least ValueHeaderCap bytes.
    /// @return View over the formatted header within @p out.
    [[nodiscard]] std::string_view FormatValueHeader(std::span<char> out,
                                                     std::string_view key,
                                                     CacheEntry const& entry,
                                                     bool includeCas)
    {
        auto* p = out.data();
        auto const append = [&p](std::string_view sv) {
            p = std::ranges::copy(sv, p).out;
        };
        auto const appendUint = [&p](std::uint64_t v) {
            auto const r = std::to_chars(p, p + 20, v);
            p = r.ptr;
        };
        append("VALUE ");
        append(key);
        *p++ = ' ';
        appendUint(entry.flags);
        *p++ = ' ';
        appendUint(entry.ValueSize());
        if (includeCas)
        {
            *p++ = ' ';
            appendUint(entry.cas);
        }
        append(Crlf);
        return std::string_view { out.data(), static_cast<std::size_t>(p - out.data()) };
    }

    /// Retained backing for a gathered get/gets/gat reply: the per-hit header
    /// lines and the GetResults that own the values' reference-counted buffers.
    /// Held alive (via a shared_ptr keep-alive) for the whole write so the
    /// zero-copy value segments stay valid, even across a suspension.
    struct GatherState
    {
        std::vector<GetResult> hits;
        std::vector<std::string> headers;
    };

    /// Send a gathered `VALUE` reply: for each hit, three segments
    /// [header][value][CRLF] with the value pointing directly at the cached
    /// payload (no copy), then a trailing `END\r\n`.
    /// @param socket Destination socket.
    /// @param state  Collected hits + formatted headers (moved in; kept alive).
    /// @return True if the full reply was written.
    Task<bool> WriteGatheredValues(ISocket* socket, std::shared_ptr<GatherState> state)
    {
        std::vector<std::span<std::byte const>> segments;
        segments.reserve((state->hits.size() * 3) + 1);
        for (std::size_t i = 0; i < state->hits.size(); ++i)
        {
            segments.push_back(AsBytes(state->headers[i]));
            segments.push_back(state->hits[i].entry.ValueBytes());
            segments.push_back(AsBytes(Crlf));
        }
        segments.push_back(AsBytes(std::string_view { "END\r\n" }));
        co_return co_await WriteAllVectored(socket, segments, std::move(state));
    }

    Task<bool> HandleGet(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> keys, bool includeCas)
    {
        // Fast path for the overwhelmingly common single-key `get`: assemble
        // the reply with frame-local state and no heap allocation beyond the
        // one header string — no GatherState shared_ptr, no hits/segments
        // vectors. The header string and the GetResult live in this coroutine
        // frame (alive across the suspended write); the value's reference-
        // counted handle is the write's keep-alive.
        if (keys.size() == 1)
        {
            auto result = engine->Get(keys[0]);
            if (!result.has_value() || !result->found)
                co_return co_await WriteAll(socket, "END\r\n");
            // Header formatted into a frame-local buffer (no heap allocation);
            // it lives across the suspended write since it's in this frame.
            std::array<char, ValueHeaderCap> headerBuf {};
            auto const header = FormatValueHeader(headerBuf, keys[0], result->entry, includeCas);
            std::array<std::span<std::byte const>, 4> const segments {
                AsBytes(header),
                result->entry.ValueBytes(),
                AsBytes(Crlf),
                AsBytes(std::string_view { "END\r\n" }),
            };
            co_return co_await WriteAllVectored(socket, segments, result->entry.value.AsKeepAlive());
        }

        auto state = std::make_shared<GatherState>();
        {
            // Synchronous lookup loop — scoped so the zone ends before the
            // co_await below (a zone must never straddle a suspension point).
            FC_ZONE_SCOPED_N("memcached.HandleGet.lookup");
            state->hits.reserve(keys.size());
            state->headers.reserve(keys.size());
            for (auto const key: keys)
            {
                auto result = engine->Get(key);
                if (!result.has_value() || !result->found)
                    continue;
                std::array<char, ValueHeaderCap> headerBuf {};
                state->headers.emplace_back(FormatValueHeader(headerBuf, key, result->entry, includeCas));
                state->hits.push_back(std::move(*result));
            }
        }
        co_return co_await WriteGatheredValues(socket, std::move(state));
    }

    Task<bool> HandleStorage(ISocket* socket,
                             CacheEngine* engine,
                             ByteReader* reader,
                             std::string_view commandName,
                             std::span<std::string_view const> args)
    {
        auto const isCas = commandName == "cas";
        StorageArgs parsed {};
        if (!ParseStorageArgs(args, isCas, parsed))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad command line format");

        // Read the payload (bytes + trailing CRLF).
        auto payload = co_await reader->ReadExactly(parsed.bytes);
        if (!payload.has_value())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad payload");

        // Consume the CRLF that follows the payload — memcached sends it.
        auto const trailing = co_await reader->ReadExactly(2);
        if (!trailing.has_value())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing CRLF after payload");

        std::expected<CasToken, StorageError> result { 0 };
        bool unknownCommand = false;
        {
            // Synchronous storage dispatch — scoped so the zone closes before
            // any co_await below (a zone must not straddle a suspension point).
            FC_ZONE_SCOPED_N("memcached.HandleStorage.dispatch");
            if (commandName == "set")
                result = engine->Set(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
            else if (commandName == "add")
                result = engine->Add(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
            else if (commandName == "replace")
                result = engine->Replace(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
            else if (commandName == "append")
                result = engine->Append(parsed.key, std::span<std::byte const> { payload->data(), payload->size() });
            else if (commandName == "prepend")
                result = engine->Prepend(parsed.key, std::span<std::byte const> { payload->data(), payload->size() });
            else if (commandName == "cas")
                result = engine->CompareAndSwap(parsed.key, parsed.cas, std::move(*payload), parsed.flags, parsed.exptime);
            else
                unknownCommand = true;
        }
        if (unknownCommand)
            co_return co_await WriteError(socket, "ERROR");

        if (parsed.noreply)
            co_return true;

        if (result.has_value())
            co_return co_await WriteLine(socket, "STORED");

        switch (result.error().code)
        {
            case StorageErrorCode::KeyExists:
                co_return co_await WriteLine(socket, "NOT_STORED");
            case StorageErrorCode::KeyNotFound:
                if (commandName == "cas")
                    co_return co_await WriteLine(socket, "NOT_FOUND");
                co_return co_await WriteLine(socket, "NOT_STORED");
            case StorageErrorCode::CasMismatch:
                co_return co_await WriteLine(socket, "EXISTS");
            case StorageErrorCode::ValueTooLarge:
                co_return co_await WriteError(socket, "SERVER_ERROR", "object too large for cache");
            default:
                co_return co_await WriteError(socket, "SERVER_ERROR", "storage failure");
        }
    }

    Task<bool> HandleDelete(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        if (args.empty())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing key");

        bool const noreply = args.size() > 1 && IsNoReply(args.back());
        auto const result = [&] {
            FC_ZONE_SCOPED_N("memcached.HandleDelete.dispatch");
            return engine->Delete(args[0]);
        }();
        if (noreply)
            co_return true;
        if (result.has_value())
            co_return co_await WriteLine(socket, "DELETED");
        co_return co_await WriteLine(socket, "NOT_FOUND");
    }

    Task<bool> HandleIncrDecr(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args, bool isIncr)
    {
        if (args.size() < 2)
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing args");

        std::uint64_t delta = 0;
        if (!ParseUnsigned(args[1], delta))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "delta not numeric");

        bool const noreply = args.size() > 2 && IsNoReply(args.back());
        auto const result = [&] {
            FC_ZONE_SCOPED_N("memcached.HandleIncrDecr.dispatch");
            return isIncr ? engine->Increment(args[0], delta) : engine->Decrement(args[0], delta);
        }();
        if (noreply)
            co_return true;
        if (result.has_value())
            co_return co_await WriteLine(socket, std::to_string(result->value));
        switch (result.error().code)
        {
            case StorageErrorCode::KeyNotFound:
                co_return co_await WriteLine(socket, "NOT_FOUND");
            case StorageErrorCode::InvalidArgument:
                co_return co_await WriteError(socket, "CLIENT_ERROR", "cannot increment or decrement non-numeric value");
            default:
                co_return co_await WriteError(socket, "SERVER_ERROR", "storage failure");
        }
    }

    Task<bool> HandleFlushAll(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        std::uint32_t delay = 0;
        bool noreply = false;
        for (auto const& token: args)
        {
            if (IsNoReply(token))
                noreply = true;
            else if (!ParseUnsigned(token, delay))
                co_return co_await WriteError(socket, "CLIENT_ERROR", "bad flush_all arg");
        }
        engine->FlushAll(delay);
        if (noreply)
            co_return true;
        co_return co_await WriteLine(socket, "OK");
    }

    /// Append a single `<prefix><name> <value>\r\n` stat line.
    /// @param out    Destination buffer.
    /// @param prefix Line prefix, e.g. "STAT " or "STAT items:1:".
    /// @param name   Stat name appended directly after the prefix.
    /// @param value  Stat value, rendered verbatim.
    void AppendStatLine(std::string& out, std::string_view prefix, std::string_view name, std::string_view value)
    {
        out.append(prefix);
        out.append(name);
        out.push_back(' ');
        out.append(value);
        out.append(Crlf);
    }

    /// Append a stat line whose value is an unsigned integer.
    /// @param out    Destination buffer.
    /// @param prefix Line prefix, e.g. "STAT " or "STAT items:1:".
    /// @param name   Stat name appended directly after the prefix.
    /// @param value  Stat value, rendered as decimal text.
    void AppendStatLine(std::string& out, std::string_view prefix, std::string_view name, std::uint64_t value)
    {
        AppendStatLine(out, prefix, name, std::to_string(value));
    }

    /// Render the default `stats` snapshot. Includes the full counter set
    /// added in the storage-stats expansion.
    void AppendDefaultStats(std::string& out, StorageStats const& stats)
    {
        auto const append = [&](std::string_view name, std::uint64_t value) {
            AppendStatLine(out, "STAT ", name, value);
        };
        out.append("STAT version ");
        out.append(MemcachedTextHandler::ServerVersion());
        out.append(Crlf);
        append("curr_items", stats.itemCount);
        append("bytes", stats.bytesUsed);
        append("limit_maxbytes", stats.bytesLimit);
        append("evictions", stats.evictions);
        append("cmd_get", stats.cmdGet);
        append("cmd_set", stats.cmdSet);
        append("cmd_touch", stats.cmdTouch);
        append("cmd_flush", stats.cmdFlush);
        append("get_hits", stats.getHits);
        append("get_misses", stats.getMisses);
        append("delete_hits", stats.deleteHits);
        append("delete_misses", stats.deleteMisses);
        append("incr_hits", stats.incrHits);
        append("incr_misses", stats.incrMisses);
        append("decr_hits", stats.decrHits);
        append("decr_misses", stats.decrMisses);
        append("touch_hits", stats.touchHits);
        append("touch_misses", stats.touchMisses);
        append("cas_hits", stats.casHits);
        append("cas_misses", stats.casMisses);
        append("cas_badval", stats.casBadval);
        append("evicted_unfetched", stats.evictedUnfetched);
        append("expired_unfetched", stats.expiredUnfetched);
    }

    /// Synthetic `stats settings` — render the engine's effective
    /// configuration. fastcached does not yet thread a `Config` object
    /// into the text handler, so this returns only the settings the
    /// engine itself can derive. A future pass will inject Config and
    /// add port / bind address / max_item_size / etc.
    void AppendSettings(std::string& out, StorageStats const& stats)
    {
        auto const append = [&](std::string_view name, std::string_view value) {
            AppendStatLine(out, "STAT ", name, value);
        };
        append("maxbytes", std::to_string(stats.bytesLimit));
        append("evictions", "on");
        append("cas_enabled", "yes");
        append("flush_enabled", "yes");
        append("verbosity", "0");
    }

    /// Synthetic `stats items` — fastcached does not have slab classes,
    /// so one synthetic class (id 1) represents the entire LRU.
    void AppendItems(std::string& out, StorageStats const& stats)
    {
        auto const append = [&](std::string_view name, std::uint64_t value) {
            AppendStatLine(out, "STAT items:1:", name, value);
        };
        append("number", stats.itemCount);
        append("evicted", stats.evictions);
        append("evicted_unfetched", stats.evictedUnfetched);
        append("expired_unfetched", stats.expiredUnfetched);
    }

    /// Synthetic `stats slabs` — one virtual slab class covers everything.
    void AppendSlabs(std::string& out, StorageStats const& stats)
    {
        auto const append = [&](std::string_view name, std::uint64_t value) {
            AppendStatLine(out, "STAT 1:", name, value);
        };
        append("chunk_size", 1024);
        append("used_chunks", stats.itemCount);
        append("free_chunks", 0);
        append("get_hits", stats.getHits);
        append("cmd_set", stats.cmdSet);
        out.append("STAT active_slabs 1\r\n");
        out.append("STAT total_malloced ");
        out.append(std::to_string(stats.bytesUsed));
        out.append(Crlf);
    }

    Task<bool> HandleStats(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        auto const stats = engine->Snapshot();
        std::string out;
        out.reserve(512);

        std::string_view const sub = args.empty() ? std::string_view {} : args[0];
        if (sub.empty())
            AppendDefaultStats(out, stats);
        else if (sub == "settings")
            AppendSettings(out, stats);
        else if (sub == "items")
            AppendItems(out, stats);
        else if (sub == "slabs")
            AppendSlabs(out, stats);
        else if (sub == "sizes")
        {
            // Without per-entry size tracking we can only report a single
            // bucket. memcached uses 32-byte powers-of-two; we approximate
            // with one row at the median bucket. Clients that depend on
            // this for capacity planning should switch to mgdump.
            auto const avg = stats.itemCount == 0 ? std::size_t { 0 } : stats.bytesUsed / stats.itemCount;
            out.append("STAT ");
            out.append(std::to_string(avg));
            out.push_back(' ');
            out.append(std::to_string(stats.itemCount));
            out.append(Crlf);
        }
        else if (sub == "conns")
        {
            // Without a connection registry we cannot enumerate active
            // connections. Return an empty result rather than ERROR so
            // capability probes succeed.
        }
        else if (sub == "reset")
        {
            // Counter reset is not yet exposed by IStorage — return RESET
            // to acknowledge the command without lying about state.
            co_return co_await WriteLine(socket, "RESET");
        }
        else
        {
            co_return co_await WriteError(socket, "CLIENT_ERROR", "unsupported stats sub-command");
        }
        out.append("END\r\n");
        co_return co_await WriteAll(socket, out);
    }

    /// `touch <key> <exptime> [noreply]`.
    Task<bool> HandleTouch(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        if (args.size() < 2)
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing args");
        std::uint32_t exptime = 0;
        if (!ParseUnsigned(args[1], exptime))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad exptime");
        bool const noreply = args.size() > 2 && IsNoReply(args.back());

        auto const r = engine->Touch(args[0], exptime);
        if (noreply)
            co_return true;
        if (r.has_value())
            co_return co_await WriteLine(socket, "TOUCHED");
        co_return co_await WriteLine(socket, "NOT_FOUND");
    }

    /// `gat <exptime> <key>...` and `gats <exptime> <key>...`.
    Task<bool> HandleGat(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args, bool includeCas)
    {
        if (args.size() < 2)
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing args");
        std::uint32_t exptime = 0;
        if (!ParseUnsigned(args[0], exptime))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad exptime");

        auto state = std::make_shared<GatherState>();
        state->hits.reserve(args.size());
        state->headers.reserve(args.size());
        for (std::size_t i = 1; i < args.size(); ++i)
        {
            auto const& key = args[i];
            // Refresh expiry and read the value back atomically. Composing
            // a separate Touch + Get would let a concurrent writer delete
            // or mutate the entry in between, crediting touch_hits for a
            // value the response never returns. On a miss, skip the key.
            auto r = engine->GetAndTouch(key, exptime);
            if (!r.has_value() || !r->found)
                continue;
            std::array<char, ValueHeaderCap> headerBuf {};
            state->headers.emplace_back(FormatValueHeader(headerBuf, key, r->entry, includeCas));
            state->hits.push_back(std::move(*r));
        }
        co_return co_await WriteGatheredValues(socket, std::move(state));
    }

    /// `cache_memlimit <megabytes> [noreply]`.
    Task<bool> HandleCacheMemlimit(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        if (args.empty())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing megabytes argument");
        std::uint64_t megabytes = 0;
        if (!ParseUnsigned(args[0], megabytes))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad megabytes argument");
        bool const noreply = args.size() > 1 && IsNoReply(args.back());
        // Guard the MB->bytes conversion: an absurd argument must not wrap
        // size_t to a tiny budget (mass eviction) or zero (= unlimited).
        constexpr std::uint64_t BytesPerMegabyte = 1024U * 1024U;
        if (megabytes > SIZE_MAX / BytesPerMegabyte)
            co_return co_await WriteError(socket, "CLIENT_ERROR", "megabytes argument too large");
        engine->Resize(static_cast<std::size_t>(megabytes * BytesPerMegabyte));
        if (noreply)
            co_return true;
        co_return co_await WriteLine(socket, "OK");
    }

} // namespace

std::string_view MemcachedTextHandler::ServerVersion() noexcept
{
    return ServerVersionBanner;
}

Task<void> MemcachedTextHandler::Run(ISocket* socket,
                                     CacheEngine* engine,
                                     std::vector<std::byte> primingBytes,
                                     SessionContext session)
{
    ByteReader reader { *socket, MaxLineBytes, MaxPayloadBytes };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

    // The memcached text protocol has no authentication handshake, so when a
    // credential is required there is no way to satisfy it over this protocol:
    // every command except `version`/`quit` is refused. Clients that need auth
    // must use the binary (SASL) or RESP (AUTH) protocols.
    // Reused across commands: cleared each iteration, so its capacity persists
    // and the common path tokenizes without a per-command heap allocation.
    std::vector<std::string_view> parts;

    while (true)
    {
        auto const lineResult = co_await reader.ReadLine();
        if (!lineResult.has_value())
        {
            // Truncated or LineTooLong — drop the connection.
            co_return;
        }

        auto const& line = *lineResult;
        if (line.empty())
            continue;

        Tokenize(line, parts);
        if (parts.empty())
        {
            (void) co_await WriteError(socket, "ERROR");
            continue;
        }

        auto const command = parts[0];
        auto const tail = std::span<std::string_view const> { parts.data() + 1, parts.size() - 1 };

        // Resolve auth per-command so SIGHUP reload of requirepass is honoured
        // immediately on the next request; the captured shared_ptr keeps the
        // policy alive for the duration of this command's evaluation.
        auto const auth = session.CurrentAuth();
        bool const authRequired = auth != nullptr && auth->Enabled();
        if (authRequired && command != "version" && command != "quit")
        {
            // The text protocol has no auth handshake, so this command can never
            // be satisfied. Reply once, then end the session rather than
            // `continue`: a refused storage command (`set <k> <f> <e> <bytes>`)
            // still has its data block sitting unread in the stream, and
            // continuing would parse those value bytes as the next command —
            // desyncing the parser and emitting a spurious second error. Clients
            // that need auth must use the binary (SASL) or RESP (AUTH) protocol.
            (void) co_await WriteError(socket, "CLIENT_ERROR", "authentication required");
            co_return;
        }

        bool ok = true;
        if (command == "get")
            ok = co_await HandleGet(socket, engine, tail, /*includeCas*/ false);
        else if (command == "gets")
            ok = co_await HandleGet(socket, engine, tail, /*includeCas*/ true);
        else if (command == "set" || command == "add" || command == "replace" || command == "append" || command == "prepend"
                 || command == "cas")
            ok = co_await HandleStorage(socket, engine, &reader, command, tail);
        else if (command == "delete")
            ok = co_await HandleDelete(socket, engine, tail);
        else if (command == "incr")
            ok = co_await HandleIncrDecr(socket, engine, tail, /*isIncr*/ true);
        else if (command == "decr")
            ok = co_await HandleIncrDecr(socket, engine, tail, /*isIncr*/ false);
        else if (command == "flush_all")
            ok = co_await HandleFlushAll(socket, engine, tail);
        else if (command == "stats")
            ok = co_await HandleStats(socket, engine, tail);
        else if (command == "version")
            ok = co_await WriteLine(socket, std::format("VERSION {}", ServerVersion()));
        else if (command == "touch")
            ok = co_await HandleTouch(socket, engine, tail);
        else if (command == "gat")
            ok = co_await HandleGat(socket, engine, tail, /*includeCas*/ false);
        else if (command == "gats")
            ok = co_await HandleGat(socket, engine, tail, /*includeCas*/ true);
        else if (command == "cache_memlimit")
            ok = co_await HandleCacheMemlimit(socket, engine, tail);
        else if (command == "verbosity")
            // Accepted as a no-op — fastcached logs via ILogger.
            ok = co_await WriteLine(socket, "OK");
        else if (command == "slabs" || command == "lru" || command == "lru_crawler")
            // Synthetic stub — fastcached does not implement slab allocation
            // or a separate LRU crawler; capability probes get OK rather
            // than ERROR. See docs for the non-applicable rationale.
            ok = co_await WriteLine(socket, "OK");
        else if (command == "mg" || command == "ms" || command == "md" || command == "ma" || command == "me"
                 || command == "mn")
            ok = co_await MemcachedMeta::Dispatch(socket, engine, &reader, command, tail);
        else if (command == "quit")
        {
            socket->Close();
            co_return;
        }
        else
            ok = co_await WriteError(socket, "ERROR");

        if (!ok)
            co_return;

        // One command fully handled and its response written — mark the frame
        // so the Tracy viewer renders per-request timing. FrameMark is a
        // stackless timeline event, so it is safe inside this coroutine.
        FC_FRAME_MARK;
    }
}

} // namespace FastCache
