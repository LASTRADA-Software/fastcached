// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/MemcachedText.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
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
    constexpr std::string_view Crlf = "\r\n";

    /// Split a line on whitespace into at most maxParts tokens.
    [[nodiscard]] std::vector<std::string_view> Tokenize(std::string_view line, std::size_t maxParts = 16)
    {
        std::vector<std::string_view> out;
        std::size_t i = 0;
        while (i < line.size() && out.size() < maxParts)
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
        return out;
    }

    template <typename T>
    [[nodiscard]] bool ParseUnsigned(std::string_view sv, T& out) noexcept
    {
        auto const [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return ec == std::errc {} && ptr == sv.data() + sv.size();
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

    Task<bool> WriteAll(ISocket& socket, std::string_view payload)
    {
        if (payload.empty())
            co_return true;
        auto const result = co_await socket.Write(AsBytes(payload));
        co_return result.has_value();
    }

    Task<bool> WriteAll(ISocket& socket, std::span<std::byte const> payload)
    {
        if (payload.empty())
            co_return true;
        auto const result = co_await socket.Write(payload);
        co_return result.has_value();
    }

    Task<bool> WriteError(ISocket& socket, std::string_view code, std::string_view detail = {})
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

    Task<bool> WriteLine(ISocket& socket, std::string_view text)
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

    /// Encode a single `VALUE` block for a get/gets response.
    void AppendValueBlock(std::string& out, std::string_view key, CacheEntry const& entry, bool includeCas)
    {
        out.append("VALUE ");
        out.append(key);
        out.push_back(' ');
        out.append(std::to_string(entry.flags));
        out.push_back(' ');
        out.append(std::to_string(entry.value.size()));
        if (includeCas)
        {
            out.push_back(' ');
            out.append(std::to_string(entry.cas));
        }
        out.append(Crlf);
        for (auto const b: entry.value)
            out.push_back(static_cast<char>(b));
        out.append(Crlf);
    }

    Task<bool> HandleGet(ISocket& socket, CacheEngine& engine, std::span<std::string_view const> keys, bool includeCas)
    {
        std::string response;
        response.reserve(64 * keys.size());
        for (auto const key: keys)
        {
            auto const result = engine.Get(key);
            if (!result.has_value() || !result->found)
                continue;
            AppendValueBlock(response, key, result->entry, includeCas);
        }
        response.append("END\r\n");
        co_return co_await WriteAll(socket, response);
    }

    Task<bool> HandleStorage(ISocket& socket,
                             CacheEngine& engine,
                             ByteReader& reader,
                             std::string_view commandName,
                             std::span<std::string_view const> args)
    {
        auto const isCas = commandName == "cas";
        StorageArgs parsed {};
        if (!ParseStorageArgs(args, isCas, parsed))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad command line format");

        // Read the payload (bytes + trailing CRLF).
        auto payload = co_await reader.ReadExactly(parsed.bytes);
        if (!payload.has_value())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "bad payload");

        // Consume the CRLF that follows the payload — memcached sends it.
        auto const trailing = co_await reader.ReadExactly(2);
        if (!trailing.has_value())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing CRLF after payload");

        std::expected<CasToken, StorageError> result { 0 };
        if (commandName == "set")
            result = engine.Set(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
        else if (commandName == "add")
            result = engine.Add(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
        else if (commandName == "replace")
            result = engine.Replace(parsed.key, std::move(*payload), parsed.flags, parsed.exptime);
        else if (commandName == "append")
            result = engine.Append(parsed.key, std::span<std::byte const> { payload->data(), payload->size() });
        else if (commandName == "prepend")
            result = engine.Prepend(parsed.key, std::span<std::byte const> { payload->data(), payload->size() });
        else if (commandName == "cas")
            result = engine.CompareAndSwap(parsed.key, parsed.cas, std::move(*payload), parsed.flags, parsed.exptime);
        else
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
            default:
                co_return co_await WriteError(socket, "SERVER_ERROR", "storage failure");
        }
    }

    Task<bool> HandleDelete(ISocket& socket, CacheEngine& engine, std::span<std::string_view const> args)
    {
        if (args.empty())
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing key");

        bool const noreply = args.size() > 1 && IsNoReply(args.back());
        auto const result = engine.Delete(args[0]);
        if (noreply)
            co_return true;
        if (result.has_value())
            co_return co_await WriteLine(socket, "DELETED");
        co_return co_await WriteLine(socket, "NOT_FOUND");
    }

    Task<bool> HandleIncrDecr(ISocket& socket, CacheEngine& engine, std::span<std::string_view const> args, bool isIncr)
    {
        if (args.size() < 2)
            co_return co_await WriteError(socket, "CLIENT_ERROR", "missing args");

        std::uint64_t delta = 0;
        if (!ParseUnsigned(args[1], delta))
            co_return co_await WriteError(socket, "CLIENT_ERROR", "delta not numeric");

        bool const noreply = args.size() > 2 && IsNoReply(args.back());
        auto const result = isIncr ? engine.Increment(args[0], delta) : engine.Decrement(args[0], delta);
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

    Task<bool> HandleFlushAll(ISocket& socket, CacheEngine& engine, std::span<std::string_view const> args)
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
        engine.FlushAll(delay);
        if (noreply)
            co_return true;
        co_return co_await WriteLine(socket, "OK");
    }

    Task<bool> HandleStats(ISocket& socket, CacheEngine& engine)
    {
        auto const stats = engine.Snapshot();
        auto const lines = std::format("STAT version {}\r\n"
                                       "STAT curr_items {}\r\n"
                                       "STAT bytes {}\r\n"
                                       "STAT limit_maxbytes {}\r\n"
                                       "STAT evictions {}\r\n"
                                       "STAT cmd_get {}\r\n"
                                       "STAT cmd_set {}\r\n"
                                       "STAT get_hits {}\r\n"
                                       "STAT get_misses {}\r\n"
                                       "END\r\n",
                                       MemcachedTextHandler::ServerVersion(),
                                       stats.itemCount,
                                       stats.bytesUsed,
                                       stats.bytesLimit,
                                       stats.evictions,
                                       stats.cmdGet,
                                       stats.cmdSet,
                                       stats.getHits,
                                       stats.getMisses);
        co_return co_await WriteAll(socket, lines);
    }

} // namespace

std::string_view MemcachedTextHandler::ServerVersion() noexcept
{
    return ServerVersionBanner;
}

Task<void> MemcachedTextHandler::Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes)
{
    ByteReader reader { socket, MaxLineBytes, MaxPayloadBytes };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

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

        auto const parts = Tokenize(line);
        if (parts.empty())
        {
            (void) co_await WriteError(socket, "ERROR");
            continue;
        }

        auto const command = parts[0];
        auto const tail = std::span<std::string_view const> { parts.data() + 1, parts.size() - 1 };

        bool ok = true;
        if (command == "get")
            ok = co_await HandleGet(socket, engine, tail, /*includeCas*/ false);
        else if (command == "gets")
            ok = co_await HandleGet(socket, engine, tail, /*includeCas*/ true);
        else if (command == "set" || command == "add" || command == "replace" || command == "append" || command == "prepend"
                 || command == "cas")
            ok = co_await HandleStorage(socket, engine, reader, command, tail);
        else if (command == "delete")
            ok = co_await HandleDelete(socket, engine, tail);
        else if (command == "incr")
            ok = co_await HandleIncrDecr(socket, engine, tail, /*isIncr*/ true);
        else if (command == "decr")
            ok = co_await HandleIncrDecr(socket, engine, tail, /*isIncr*/ false);
        else if (command == "flush_all")
            ok = co_await HandleFlushAll(socket, engine, tail);
        else if (command == "stats")
            ok = co_await HandleStats(socket, engine);
        else if (command == "version")
            ok = co_await WriteLine(socket, std::format("VERSION {}", ServerVersion()));
        else if (command == "quit")
        {
            socket.Close();
            co_return;
        }
        else
            ok = co_await WriteError(socket, "ERROR");

        if (!ok)
            co_return;
    }
}

} // namespace FastCache
