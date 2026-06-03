// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Protocol/MemcachedMeta.hpp>
#include <FastCache/Protocol/MemcachedShared.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    /// Parsed view of a single meta-protocol flag.
    ///
    /// Each flag token is a single letter optionally followed by a value
    /// (either bare digits/identifier or, for `M`, an `=`-prefixed mode
    /// like `M=S`). When the flag is bare (e.g. `c`, `k`, `q`), `token`
    /// is empty.
    struct MetaFlag
    {
        char letter { '\0' };
        std::string_view token;
    };

    [[nodiscard]] MetaFlag ParseFlag(std::string_view raw) noexcept
    {
        MetaFlag out;
        if (raw.empty())
            return out;
        out.letter = raw[0];
        out.token = raw.substr(1);
        // `M=S` style — strip the leading `=` for the mode flag only, so the
        // mode reads as a bare letter. Every other flag keeps its token
        // verbatim; in particular an opaque token (`O=abc`) must round-trip
        // unchanged.
        if (out.letter == 'M' && !out.token.empty() && out.token.front() == '=')
            out.token.remove_prefix(1);
        return out;
    }

    /// Parse `token` into `value` and, on success, set the companion
    /// presence flag. Used by the flag-parsing switches for the many
    /// `letter(value)` flags that pair a `hasX` bool with an `x` field.
    /// @param token Flag payload (the text after the flag letter).
    /// @param has   Presence flag, set to `true` only on a successful parse.
    /// @param value Destination, written only on a successful parse.
    template <typename T>
    void SetIfParsed(std::string_view token, bool& has, T& value) noexcept
    {
        if (T v = 0; ParseUnsigned(token, v))
        {
            has = true;
            value = v;
        }
    }

    /// Map a storage error code to its meta-protocol response token:
    /// `NS` (not stored), `NF` (not found), `EX` (cas mismatch). Any other
    /// code falls back to a generic server-error line. Mirrors the binary
    /// handler's `MapStorageError`.
    /// @param code Storage error code to translate.
    /// @return The meta response token for @p code.
    [[nodiscard]] std::string_view MetaErrorToken(StorageErrorCode code) noexcept
    {
        switch (code)
        {
            case StorageErrorCode::KeyExists:
                return "NS";
            case StorageErrorCode::KeyNotFound:
                return "NF";
            case StorageErrorCode::CasMismatch:
                return "EX";
            default:
                return "SERVER_ERROR storage failure";
        }
    }

    /// Append the optional response flags (k/O/c/s/t/f/h/l) to a meta
    /// response line. Each flag is rendered in the same form the request
    /// used (bare letter, or letter+token).
    // Fields are ordered widest-first (8-byte scalars, then the 16-byte
    // string_views, then the 4-byte flags word, then the bool predicates) so
    // the struct carries no avoidable padding.
    struct MetaResponseFlags
    {
        std::uint64_t cas { 0 };
        std::size_t size { 0 };
        std::int64_t ttlSeconds { 0 };
        std::int64_t lastAccessSeconds { 0 };
        std::string_view key;
        std::string_view opaque;
        std::uint32_t flags { 0 };
        bool echoKey { false };
        bool echoOpaque { false };
        bool emitCas { false };
        bool emitSize { false };
        bool emitFlags { false };
        bool emitHit { false };
        bool hit { false };
        bool emitTtl { false };
        bool emitLastAccess { false };
        bool stale { false };
    };

    void AppendResponseFlags(std::string& out, MetaResponseFlags const& f)
    {
        if (f.echoKey)
        {
            out.push_back(' ');
            out.push_back('k');
            out.append(f.key);
        }
        if (f.echoOpaque)
        {
            out.push_back(' ');
            out.push_back('O');
            out.append(f.opaque);
        }
        if (f.emitCas)
        {
            out.append(" c");
            out.append(std::to_string(f.cas));
        }
        if (f.emitFlags)
        {
            out.append(" f");
            out.append(std::to_string(f.flags));
        }
        if (f.emitSize)
        {
            out.append(" s");
            out.append(std::to_string(f.size));
        }
        if (f.emitTtl)
        {
            out.append(" t");
            out.append(std::to_string(f.ttlSeconds));
        }
        if (f.emitLastAccess)
        {
            out.append(" l");
            out.append(std::to_string(f.lastAccessSeconds));
        }
        if (f.emitHit)
        {
            out.append(" h");
            out.push_back(f.hit ? '1' : '0');
        }
        if (f.stale)
            out.append(" X");
    }

    /// Seed a `MetaResponseFlags` with the flags every meta response echoes
    /// back verbatim — the opaque token (`O`) and the key (`k`). All four
    /// request-flag bags expose the same `hasOpaque`/`opaque`/`wantKey`
    /// members, so this is templated over the bag type.
    /// @param f   Parsed request flags.
    /// @param key The request key (echoed when `k` was requested).
    /// @return A response-flag bag with the echo fields pre-filled.
    template <typename Flags>
    [[nodiscard]] MetaResponseFlags BaseResponseFlags(Flags const& f, std::string_view key)
    {
        MetaResponseFlags rf;
        rf.echoOpaque = f.hasOpaque;
        rf.opaque = f.opaque;
        rf.echoKey = f.wantKey;
        rf.key = key;
        return rf;
    }

    /// Walk the flag tokens and populate the strongly-typed flag bag.
    /// Unknown letters are ignored — the spec says "all flags are
    /// optional and a server should ignore unknown ones".
    struct MetaGetFlags
    {
        bool wantCas { false };
        bool wantFlags { false };
        bool wantHit { false };
        bool wantKey { false };
        bool wantLastAccess { false };
        bool quietOnMiss { false };
        bool wantSize { false };
        bool wantTtl { false };
        bool noBump { false };
        bool wantValue { false };
        bool hasOpaque { false };
        std::string_view opaque;
        bool hasNewTtl { false };
        std::uint32_t newTtl { 0 };
        bool hasVivify { false };
        std::uint32_t vivifyTtl { 0 };
    };

    [[nodiscard]] MetaGetFlags ParseGetFlags(std::span<std::string_view const> tokens)
    {
        MetaGetFlags out;
        for (auto const& raw: tokens)
        {
            auto const f = ParseFlag(raw);
            switch (f.letter)
            {
                case 'c':
                    out.wantCas = true;
                    break;
                case 'f':
                    out.wantFlags = true;
                    break;
                case 'h':
                    out.wantHit = true;
                    break;
                case 'k':
                    out.wantKey = true;
                    break;
                case 'l':
                    out.wantLastAccess = true;
                    break;
                case 'q':
                    out.quietOnMiss = true;
                    break;
                case 's':
                    out.wantSize = true;
                    break;
                case 't':
                    out.wantTtl = true;
                    break;
                case 'u':
                    out.noBump = true;
                    break;
                case 'v':
                    out.wantValue = true;
                    break;
                case 'O':
                    out.hasOpaque = true;
                    out.opaque = f.token;
                    break;
                case 'T':
                    SetIfParsed(f.token, out.hasNewTtl, out.newTtl);
                    break;
                case 'N':
                    SetIfParsed(f.token, out.hasVivify, out.vivifyTtl);
                    break;
                default:
                    break;
            }
        }
        return out;
    }

    struct MetaSetFlags
    {
        bool wantCas { false };
        bool wantKey { false };
        bool wantSize { false };
        bool quiet { false };
        bool hasTtl { false };
        std::uint32_t ttl { 0 };
        bool hasFlags { false };
        std::uint32_t flags { 0 };
        bool hasMode { false };
        char mode { 'S' }; // S=set, E=add, A=append, P=prepend, R=replace
        bool hasCompareCas { false };
        std::uint64_t compareCas { 0 };
        bool hasOpaque { false };
        std::string_view opaque;
        bool markStale { false };
        bool hasVivify { false };
        std::uint32_t vivifyTtl { 0 };
    };

    [[nodiscard]] MetaSetFlags ParseSetFlags(std::span<std::string_view const> tokens)
    {
        MetaSetFlags out;
        for (auto const& raw: tokens)
        {
            auto const f = ParseFlag(raw);
            switch (f.letter)
            {
                case 'c':
                    out.wantCas = true;
                    break;
                case 'k':
                    out.wantKey = true;
                    break;
                case 's':
                    out.wantSize = true;
                    break;
                case 'q':
                    out.quiet = true;
                    break;
                case 'I':
                    out.markStale = true;
                    break;
                case 'O':
                    out.hasOpaque = true;
                    out.opaque = f.token;
                    break;
                case 'T':
                    SetIfParsed(f.token, out.hasTtl, out.ttl);
                    break;
                case 'F':
                    SetIfParsed(f.token, out.hasFlags, out.flags);
                    break;
                case 'M':
                    if (!f.token.empty())
                    {
                        out.hasMode = true;
                        out.mode = f.token.front();
                    }
                    break;
                case 'C':
                    SetIfParsed(f.token, out.hasCompareCas, out.compareCas);
                    break;
                case 'N':
                    SetIfParsed(f.token, out.hasVivify, out.vivifyTtl);
                    break;
                default:
                    break;
            }
        }
        return out;
    }

    struct MetaDeleteFlags
    {
        bool wantKey { false };
        bool quiet { false };
        bool markStale { false };
        bool hasCompareCas { false };
        std::uint64_t compareCas { 0 };
        bool hasOpaque { false };
        std::string_view opaque;
        bool hasTtl { false };
        std::uint32_t ttl { 0 };
    };

    [[nodiscard]] MetaDeleteFlags ParseDeleteFlags(std::span<std::string_view const> tokens)
    {
        MetaDeleteFlags out;
        for (auto const& raw: tokens)
        {
            auto const f = ParseFlag(raw);
            switch (f.letter)
            {
                case 'k':
                    out.wantKey = true;
                    break;
                case 'q':
                    out.quiet = true;
                    break;
                case 'I':
                    out.markStale = true;
                    break;
                case 'O':
                    out.hasOpaque = true;
                    out.opaque = f.token;
                    break;
                case 'C':
                    SetIfParsed(f.token, out.hasCompareCas, out.compareCas);
                    break;
                case 'T':
                    SetIfParsed(f.token, out.hasTtl, out.ttl);
                    break;
                default:
                    break;
            }
        }
        return out;
    }

    struct MetaArithFlags
    {
        bool quiet { false };
        bool wantCas { false };
        bool wantKey { false };
        bool wantTtl { false };
        bool wantValue { false };
        char mode { 'I' }; // I/+=incr, D/-=decr
        std::uint64_t delta { 1 };
        bool hasInitial { false };
        std::uint64_t initial { 0 };
        bool hasVivifyTtl { false };
        std::uint32_t vivifyTtl { 0 };
        bool hasNewTtl { false };
        std::uint32_t newTtl { 0 };
        bool hasOpaque { false };
        std::string_view opaque;
        bool hasCompareCas { false };
        std::uint64_t compareCas { 0 };
    };

    [[nodiscard]] MetaArithFlags ParseArithFlags(std::span<std::string_view const> tokens)
    {
        MetaArithFlags out;
        for (auto const& raw: tokens)
        {
            auto const f = ParseFlag(raw);
            switch (f.letter)
            {
                case 'q':
                    out.quiet = true;
                    break;
                case 'c':
                    out.wantCas = true;
                    break;
                case 'k':
                    out.wantKey = true;
                    break;
                case 't':
                    out.wantTtl = true;
                    break;
                case 'v':
                    out.wantValue = true;
                    break;
                case 'M':
                    if (!f.token.empty())
                    {
                        char const m = f.token.front();
                        if (m == 'I' || m == '+')
                            out.mode = 'I';
                        else if (m == 'D' || m == '-')
                            out.mode = 'D';
                    }
                    break;
                case 'D':
                    if (std::uint64_t v = 0; ParseUnsigned(f.token, v))
                        out.delta = v;
                    break;
                case 'J':
                    SetIfParsed(f.token, out.hasInitial, out.initial);
                    break;
                case 'N':
                    SetIfParsed(f.token, out.hasVivifyTtl, out.vivifyTtl);
                    break;
                case 'T':
                    SetIfParsed(f.token, out.hasNewTtl, out.newTtl);
                    break;
                case 'O':
                    out.hasOpaque = true;
                    out.opaque = f.token;
                    break;
                case 'C':
                    SetIfParsed(f.token, out.hasCompareCas, out.compareCas);
                    break;
                default:
                    break;
            }
        }
        return out;
    }

    [[nodiscard]] std::int64_t TtlSecondsFromExpiry(TimePoint expiry, TimePoint now)
    {
        if (expiry == TimePoint::max())
            return -1;
        auto const diff = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
        return diff;
    }

    /// Seconds since the entry was last read (meta `l` flag / `ME la=`).
    /// A never-accessed entry (sentinel `lastAccess`) reports 0.
    [[nodiscard]] std::int64_t LastAccessSeconds(TimePoint lastAccess, TimePoint now)
    {
        if (lastAccess == TimePoint::min())
            return 0;
        return std::chrono::duration_cast<std::chrono::seconds>(now - lastAccess).count();
    }

    Task<bool> HandleMg(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        // mg <key> <flags...>
        if (args.empty())
        {
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing key\r\n");
        }
        auto const key = args[0];
        auto const flagTokens = args.subspan(1);
        auto const f = ParseGetFlags(flagTokens);
        auto const now = engine->Clock().Now();

        // Pick the read primitive once: get-and-touch when refreshing the TTL
        // (`T`) so the touch and read form a single atomic step — rather than a
        // Get followed by a second GetAndTouch, which would do two lookups,
        // copy the value twice, and double-count get_hits; a non-mutating Peek
        // when the client asked not to bump the LRU (`u`); else a plain Get.
        auto readKey = [&](std::string_view k) -> std::expected<GetResult, StorageError> {
            if (f.hasNewTtl)
                return engine->GetAndTouch(k, f.newTtl);
            if (f.noBump)
                return engine->Peek(k);
            return engine->Get(k);
        };

        auto got = readKey(key);
        bool found = got.has_value() && got->found;

        // Auto-vivify on miss when N is given.
        if (!found && f.hasVivify)
        {
            auto const set = engine->Add(key, std::vector<std::byte> {}, 0, f.vivifyTtl);
            if (set.has_value())
            {
                got = readKey(key);
                found = got.has_value() && got->found;
            }
        }

        if (!found)
        {
            if (f.quietOnMiss)
                co_return true;
            std::string line { "EN" };
            auto rf = BaseResponseFlags(f, key);
            AppendResponseFlags(line, rf);
            line.append(Crlf);
            co_return co_await WriteAll(socket, line);
        }

        // When `T` was requested, `got` is already the post-touch entry, so the
        // `c` (CAS) and `t` (TTL) flags reflect the state this command just
        // installed — a follow-up `cas`/`ms C` sees the CAS the response
        // advertised rather than spuriously failing.
        auto const& entry = got->entry;

        std::string line;
        line.reserve(64);
        if (f.wantValue)
        {
            line.append("VA ");
            line.append(std::to_string(entry.value.size()));
        }
        else
        {
            line.append("HD");
        }

        auto rf = BaseResponseFlags(f, key);
        rf.emitCas = f.wantCas;
        rf.cas = entry.cas;
        rf.emitFlags = f.wantFlags;
        rf.flags = entry.flags;
        rf.emitSize = f.wantSize;
        rf.size = entry.value.size();
        rf.emitTtl = f.wantTtl;
        rf.ttlSeconds = TtlSecondsFromExpiry(entry.expiry, now);
        rf.emitHit = f.wantHit;
        rf.hit = true;
        rf.emitLastAccess = f.wantLastAccess;
        rf.lastAccessSeconds = LastAccessSeconds(entry.lastAccess, now);
        rf.stale = entry.stale;
        AppendResponseFlags(line, rf);
        line.append(Crlf);
        if (f.wantValue)
        {
            line.append(AsStringView(entry.value));
            line.append(Crlf);
        }
        co_return co_await WriteAll(socket, line);
    }

    Task<bool> HandleMs(ISocket* socket, CacheEngine* engine, ByteReader* reader, std::span<std::string_view const> args)
    {
        // ms <key> <datalen> <flags...>
        if (args.size() < 2)
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing args\r\n");
        auto const key = args[0];
        std::uint32_t datalen = 0;
        if (!ParseUnsigned(args[1], datalen))
            co_return co_await WriteAll(socket, "CLIENT_ERROR bad datalen\r\n");
        auto const flagTokens = args.subspan(2);
        auto const f = ParseSetFlags(flagTokens);

        // Read the payload + trailing CRLF.
        auto payload = co_await reader->ReadExactly(datalen);
        if (!payload.has_value())
            co_return co_await WriteAll(socket, "CLIENT_ERROR bad payload\r\n");
        auto trailing = co_await reader->ReadExactly(2);
        if (!trailing.has_value())
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing CRLF\r\n");

        auto const exptime = f.hasTtl ? f.ttl : 0U;
        auto const flagsValue = f.hasFlags ? f.flags : 0U;

        std::expected<CasToken, StorageError> result { 0 };
        if (f.markStale)
        {
            // `I` — mark the existing entry stale instead of storing. The
            // payload was still consumed above (to keep the stream framed)
            // but is intentionally ignored; the `T` flag, if present,
            // refreshes the TTL on the now-stale entry.
            result = engine->MarkStale(key, f.hasTtl ? std::optional<std::uint32_t> { f.ttl } : std::nullopt);
        }
        else
        {
            // A `C` (compare-cas) flag is a per-mode precondition, not a
            // standalone replace: a CAS-guarded append/prepend must still
            // append/prepend (the CAS check is pushed into the storage
            // primitive), and only set/replace map to CompareAndSwap.
            switch (f.mode)
            {
                case 'E':
                    result = engine->Add(key, std::move(*payload), flagsValue, exptime);
                    break;
                case 'A':
                    result = engine->Append(key,
                                            std::span<std::byte const> { payload->data(), payload->size() },
                                            f.hasCompareCas ? f.compareCas : 0);
                    break;
                case 'P':
                    result = engine->Prepend(key,
                                             std::span<std::byte const> { payload->data(), payload->size() },
                                             f.hasCompareCas ? f.compareCas : 0);
                    break;
                case 'R':
                    result = f.hasCompareCas
                                 ? engine->CompareAndSwap(key, f.compareCas, std::move(*payload), flagsValue, exptime)
                                 : engine->Replace(key, std::move(*payload), flagsValue, exptime);
                    break;
                case 'S':
                default:
                    result = f.hasCompareCas
                                 ? engine->CompareAndSwap(key, f.compareCas, std::move(*payload), flagsValue, exptime)
                                 : engine->Set(key, std::move(*payload), flagsValue, exptime);
                    break;
            }

            // `N` — auto-vivify on an append/prepend miss: create the key
            // with the supplied payload and vivify TTL. The append/prepend
            // path passes the payload by span (never moved), so it is still
            // intact here.
            if (!result.has_value() && result.error().code == StorageErrorCode::KeyNotFound && f.hasVivify
                && (f.mode == 'A' || f.mode == 'P'))
                result = engine->Add(key, std::move(*payload), flagsValue, f.vivifyTtl);
        }

        // Quiet (`q`) suppresses only the success line; error tokens
        // (NS/EX/NF) must still be sent so a client can detect a lost
        // conditional store.
        if (f.quiet && result.has_value())
            co_return true;

        std::string line { result.has_value() ? std::string_view { "HD" } : MetaErrorToken(result.error().code) };

        auto rf = BaseResponseFlags(f, key);
        if (result.has_value())
        {
            rf.emitCas = f.wantCas;
            rf.cas = *result;
            rf.emitSize = f.wantSize;
            if (f.wantSize)
            {
                // Report the resulting item size, not the request datalen:
                // they differ for append/prepend (concatenation) and
                // mark-stale (the payload is discarded). Peek is non-mutating.
                auto const stored = engine->Peek(key);
                rf.size = stored.has_value() && stored->found ? stored->entry.value.size() : datalen;
            }
        }
        AppendResponseFlags(line, rf);
        line.append(Crlf);
        co_return co_await WriteAll(socket, line);
    }

    Task<bool> HandleMd(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        // md <key> <flags...>
        if (args.empty())
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing key\r\n");
        auto const key = args[0];
        auto const f = ParseDeleteFlags(args.subspan(1));

        // Resolve the outcome token. `I` marks the entry stale (optionally
        // refreshing TTL via `T`) instead of removing it; `C` performs an
        // atomic compare-and-delete; otherwise a plain delete. The CAS
        // check and the delete happen in a single critical section
        // (CompareAndDelete) so a concurrent writer cannot replace the
        // value between the compare and the erase.
        std::string line;
        if (f.markStale)
        {
            auto const r = engine->MarkStale(key, f.hasTtl ? std::optional<std::uint32_t> { f.ttl } : std::nullopt);
            line.assign(r.has_value() ? std::string_view { "HD" } : MetaErrorToken(r.error().code));
        }
        else
        {
            auto const r = f.hasCompareCas ? engine->CompareAndDelete(key, f.compareCas) : engine->Delete(key);
            line.assign(r.has_value() ? std::string_view { "HD" } : MetaErrorToken(r.error().code));
        }

        // Quiet (`q`) suppresses the benign outcomes (deleted / not-found);
        // an EX (cas mismatch) is still reported so a conditional delete that
        // lost a race remains observable.
        if (f.quiet && (line == "HD" || line == "NF"))
            co_return true;
        auto rf = BaseResponseFlags(f, key);
        AppendResponseFlags(line, rf);
        line.append(Crlf);
        co_return co_await WriteAll(socket, line);
    }

    Task<bool> HandleMa(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        // ma <key> <flags...>
        if (args.empty())
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing key\r\n");
        auto const key = args[0];
        auto const f = ParseArithFlags(args.subspan(1));
        auto const now = engine->Clock().Now();

        auto result = f.mode == 'I' ? engine->Increment(key, f.delta) : engine->Decrement(key, f.delta);

        if (!result.has_value() && result.error().code == StorageErrorCode::KeyNotFound && f.hasVivifyTtl)
        {
            // Auto-vivify with the initial value.
            std::uint64_t const initial = f.hasInitial ? f.initial : 0U;
            auto const set = engine->Set(key, BytesFromString(std::to_string(initial)), 0, f.vivifyTtl);
            if (set.has_value())
                result = IStorage::IncrResult { .value = initial, .cas = *set };
        }

        if (f.hasNewTtl && result.has_value())
        {
            // Touch bumps the CAS; rebind so the `c` response flag reports the
            // post-touch CAS the client must use next (as HandleMg does) —
            // otherwise a follow-up `C(token)` spuriously fails with EX.
            auto const touched = engine->Touch(key, f.newTtl);
            if (touched.has_value())
                result->cas = *touched;
        }

        // Quiet (`q`) suppresses only the success response; NF / CLIENT_ERROR
        // must still be sent.
        if (f.quiet && result.has_value())
            co_return true;

        std::string line;
        if (!result.has_value())
        {
            // `ma` adds one arith-specific token (non-numeric value) on top of
            // the shared NF/server-error mapping.
            if (result.error().code == StorageErrorCode::InvalidArgument)
                line = "CLIENT_ERROR non-numeric value";
            else
                line.assign(MetaErrorToken(result.error().code));
        }
        else if (f.wantValue)
        {
            auto const text = std::to_string(result->value);
            line.append("VA ");
            line.append(std::to_string(text.size()));
            auto rf = BaseResponseFlags(f, key);
            rf.emitCas = f.wantCas;
            rf.cas = result->cas;
            rf.emitTtl = f.wantTtl;
            // Re-read (non-mutating) to report the post-touch TTL without
            // counting a get_hit or stamping lastAccess on the counter.
            if (f.wantTtl)
            {
                auto const re = engine->Peek(key);
                if (re.has_value() && re->found)
                    rf.ttlSeconds = TtlSecondsFromExpiry(re->entry.expiry, now);
            }
            AppendResponseFlags(line, rf);
            line.append(Crlf);
            line.append(text);
            line.append(Crlf);
            co_return co_await WriteAll(socket, line);
        }
        else
        {
            line = "HD";
        }

        auto rf = BaseResponseFlags(f, key);
        if (result.has_value())
        {
            rf.emitCas = f.wantCas;
            rf.cas = result->cas;
        }
        AppendResponseFlags(line, rf);
        line.append(Crlf);
        co_return co_await WriteAll(socket, line);
    }

    Task<bool> HandleMe(ISocket* socket, CacheEngine* engine, std::span<std::string_view const> args)
    {
        // me <key> [b]
        if (args.empty())
            co_return co_await WriteAll(socket, "CLIENT_ERROR missing key\r\n");
        auto const key = args[0];
        auto const got = engine->Get(key);
        if (!got.has_value() || !got->found)
            co_return co_await WriteAll(socket, "EN\r\n");
        auto const& entry = got->entry;
        auto const now = engine->Clock().Now();
        auto line = std::format("ME {} exp={} la={} cas={} fetch=1 cls=1 size={}\r\n",
                                key,
                                TtlSecondsFromExpiry(entry.expiry, now),
                                LastAccessSeconds(entry.lastAccess, now),
                                entry.cas,
                                entry.value.size());
        co_return co_await WriteAll(socket, line);
    }

} // namespace

Task<bool> MemcachedMeta::Dispatch(ISocket* socket,
                                   CacheEngine* engine,
                                   ByteReader* reader,
                                   std::string_view command,
                                   std::span<std::string_view const> args)
{
    if (command == "mg")
        co_return co_await HandleMg(socket, engine, args);
    if (command == "ms")
        co_return co_await HandleMs(socket, engine, reader, args);
    if (command == "md")
        co_return co_await HandleMd(socket, engine, args);
    if (command == "ma")
        co_return co_await HandleMa(socket, engine, args);
    if (command == "me")
        co_return co_await HandleMe(socket, engine, args);
    if (command == "mn")
        co_return co_await WriteAll(socket, "MN\r\n");
    co_return co_await WriteAll(socket, "ERROR\r\n");
}

} // namespace FastCache
