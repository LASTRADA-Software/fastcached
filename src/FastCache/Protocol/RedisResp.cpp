// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisResp.hpp>

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    constexpr std::size_t kMaxLineBytes = 65536;
    constexpr std::size_t kMaxPayloadBytes = 64 * 1024 * 1024;
    constexpr std::string_view kCrlf = "\r\n";

    [[nodiscard]] std::string Upper(std::string_view sv)
    {
        std::string out { sv };
        for (auto& c : out)
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

    Task<bool> WriteAll(ISocket& socket, std::string_view payload)
    {
        if (payload.empty())
            co_return true;
        auto const r = co_await socket.Write(AsBytes(payload));
        co_return r.has_value();
    }

    Task<bool> WriteAll(ISocket& socket, std::span<std::byte const> payload)
    {
        if (payload.empty())
            co_return true;
        auto const r = co_await socket.Write(payload);
        co_return r.has_value();
    }

    Task<bool> ReplyOk(ISocket& socket) { co_return co_await WriteAll(socket, "+OK\r\n"); }
    Task<bool> ReplyPong(ISocket& socket) { co_return co_await WriteAll(socket, "+PONG\r\n"); }
    Task<bool> ReplyNil(ISocket& socket) { co_return co_await WriteAll(socket, "$-1\r\n"); }

    Task<bool> ReplyInteger(ISocket& socket, std::int64_t value)
    {
        co_return co_await WriteAll(socket, std::format(":{}\r\n", value));
    }

    Task<bool> ReplySimpleString(ISocket& socket, std::string_view text)
    {
        co_return co_await WriteAll(socket, std::format("+{}\r\n", text));
    }

    Task<bool> ReplyError(ISocket& socket, std::string_view detail)
    {
        co_return co_await WriteAll(socket, std::format("-ERR {}\r\n", detail));
    }

    Task<bool> ReplyBulkString(ISocket& socket, std::span<std::byte const> bytes)
    {
        auto const header = std::format("${}\r\n", bytes.size());
        if (!co_await WriteAll(socket, header))
            co_return false;
        if (!co_await WriteAll(socket, bytes))
            co_return false;
        co_return co_await WriteAll(socket, kCrlf);
    }

    Task<bool> ReplyBulkString(ISocket& socket, std::string_view text)
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

    /// Read a `$<len>\r\n<bytes>\r\n` bulk string argument from the reader.
    Task<std::expected<std::string, ProtocolError>> ReadBulkArg(ByteReader& reader)
    {
        auto const header = co_await reader.ReadLine();
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

        auto const bytes = co_await reader.ReadExactly(static_cast<std::size_t>(len));
        if (!bytes.has_value())
            co_return std::unexpected(bytes.error());
        // Trailing CRLF.
        auto const crlf = co_await reader.ReadExactly(2);
        if (!crlf.has_value())
            co_return std::unexpected(crlf.error());
        co_return std::string { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
    }

    Task<ReadCommandResult> ReadOneCommand(ByteReader& reader)
    {
        auto const first = co_await reader.ReadLine();
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
                opts.exptime = tok == "EX" ? static_cast<std::uint32_t>(raw)
                                           : static_cast<std::uint32_t>((raw + 999) / 1000);
                ++i;
            }
            else
                return std::unexpected("unknown SET option: " + tok);
        }
        return opts;
    }

    Task<bool> HandleGet(ISocket& socket, CacheEngine& engine, std::span<std::string const> args)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'get'");
        auto const result = engine.Get(args[0]);
        if (!result.has_value() || !result->found)
            co_return co_await ReplyNil(socket);
        co_return co_await ReplyBulkString(socket,
                                           std::span<std::byte const> { result->entry.value.data(),
                                                                        result->entry.value.size() });
    }

    Task<bool> HandleSet(ISocket& socket, CacheEngine& engine, std::span<std::string const> args)
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
        for (auto const c : value)
            bytes.push_back(static_cast<std::byte>(c));

        std::expected<CasToken, StorageError> result { 0 };
        if (opts->nx)
            result = engine.Add(key, std::move(bytes), 0, opts->exptime);
        else if (opts->xx)
            result = engine.Replace(key, std::move(bytes), 0, opts->exptime);
        else
            result = engine.Set(key, std::move(bytes), 0, opts->exptime);

        if (result.has_value())
            co_return co_await ReplyOk(socket);
        if (result.error().code == StorageErrorCode::KeyExists
            || result.error().code == StorageErrorCode::KeyNotFound)
            co_return co_await ReplyNil(socket);
        co_return co_await ReplyError(socket, "storage failure");
    }

    Task<bool> HandleSetEx(ISocket& socket, CacheEngine& engine, std::span<std::string const> args, bool millis)
    {
        if (args.size() != 3)
            co_return co_await ReplyError(socket,
                                          millis ? "wrong number of arguments for 'psetex'"
                                                 : "wrong number of arguments for 'setex'");
        std::uint64_t raw = 0;
        if (!ParseUnsigned(std::string_view { args[1] }, raw))
            co_return co_await ReplyError(socket, "ttl must be a number");
        auto const exptime = millis ? static_cast<std::uint32_t>((raw + 999) / 1000)
                                    : static_cast<std::uint32_t>(raw);

        std::vector<std::byte> bytes;
        bytes.reserve(args[2].size());
        for (auto const c : args[2])
            bytes.push_back(static_cast<std::byte>(c));
        auto const result = engine.Set(args[0], std::move(bytes), 0, exptime);
        if (!result.has_value())
            co_return co_await ReplyError(socket, "storage failure");
        co_return co_await ReplyOk(socket);
    }

    Task<bool> HandleDel(ISocket& socket, CacheEngine& engine, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'del'");
        std::int64_t deleted = 0;
        for (auto const& key : args)
        {
            auto const result = engine.Delete(key);
            if (result.has_value())
                ++deleted;
        }
        co_return co_await ReplyInteger(socket, deleted);
    }

    Task<bool> HandleExists(ISocket& socket, CacheEngine& engine, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyError(socket, "wrong number of arguments for 'exists'");
        std::int64_t found = 0;
        for (auto const& key : args)
        {
            auto const result = engine.Get(key);
            if (result.has_value() && result->found)
                ++found;
        }
        co_return co_await ReplyInteger(socket, found);
    }

    Task<bool> HandlePing(ISocket& socket, std::span<std::string const> args)
    {
        if (args.empty())
            co_return co_await ReplyPong(socket);
        co_return co_await ReplyBulkString(socket, args[0]);
    }

    Task<bool> HandleEcho(ISocket& socket, std::span<std::string const> args)
    {
        if (args.size() != 1)
            co_return co_await ReplyError(socket, "wrong number of arguments for 'echo'");
        co_return co_await ReplyBulkString(socket, args[0]);
    }

    Task<bool> HandleInfo(ISocket& socket, CacheEngine& engine)
    {
        auto const stats = engine.Snapshot();
        auto const body = std::format(
            "# Server\r\nfastcached_version:{}\r\nredis_version:6.0.0-fastcached\r\n"
            "# Memory\r\nused_memory:{}\r\nmaxmemory:{}\r\n"
            "# Stats\r\ntotal_commands_processed:{}\r\nkeyspace_hits:{}\r\nkeyspace_misses:{}\r\n",
            RedisRespHandler::ServerVersion(),
            stats.bytesUsed,
            stats.bytesLimit,
            stats.cmdGet + stats.cmdSet,
            stats.getHits,
            stats.getMisses);
        co_return co_await ReplyBulkString(socket, body);
    }

    Task<bool> HandleHello(ISocket& socket, std::span<std::string const> args)
    {
        // HELLO [<protover>] — we only support RESP2.
        if (!args.empty())
        {
            std::uint32_t ver = 0;
            if (ParseUnsigned(std::string_view { args[0] }, ver) && ver != 2)
                co_return co_await WriteAll(socket, "-NOPROTO sorry, RESP3 not supported\r\n");
        }
        // Minimal RESP2 HELLO reply — an array of key/value pairs.
        auto const body = std::format(
            "*6\r\n$6\r\nserver\r\n$13\r\n{}\r\n"
            "$5\r\nproto\r\n:2\r\n"
            "$2\r\nid\r\n:1\r\n",
            "fastcached-0");
        co_return co_await WriteAll(socket, body);
    }

    Task<bool> HandleCommand(ISocket& socket, std::span<std::string const> args)
    {
        // Always reply with an empty array; sccache only uses COMMAND
        // DOCS/COUNT for sanity checks.
        static_cast<void>(args);
        co_return co_await WriteAll(socket, "*0\r\n");
    }

    Task<bool> HandleFlush(ISocket& socket, CacheEngine& engine)
    {
        engine.FlushAll(0);
        co_return co_await ReplyOk(socket);
    }

    Task<bool> Dispatch(ISocket& socket, CacheEngine& engine, ParsedCommand const& cmd)
    {
        if (cmd.args.empty())
            co_return true;

        auto const name = Upper(cmd.args[0]);
        auto const tail = std::span<std::string const> { cmd.args.data() + 1, cmd.args.size() - 1 };

        if (name == "GET") co_return co_await HandleGet(socket, engine, tail);
        if (name == "SET") co_return co_await HandleSet(socket, engine, tail);
        if (name == "SETEX") co_return co_await HandleSetEx(socket, engine, tail, /*millis*/ false);
        if (name == "PSETEX") co_return co_await HandleSetEx(socket, engine, tail, /*millis*/ true);
        if (name == "DEL" || name == "UNLINK") co_return co_await HandleDel(socket, engine, tail);
        if (name == "EXISTS") co_return co_await HandleExists(socket, engine, tail);
        if (name == "PING") co_return co_await HandlePing(socket, tail);
        if (name == "ECHO") co_return co_await HandleEcho(socket, tail);
        if (name == "INFO") co_return co_await HandleInfo(socket, engine);
        if (name == "HELLO") co_return co_await HandleHello(socket, tail);
        if (name == "COMMAND") co_return co_await HandleCommand(socket, tail);
        if (name == "FLUSHDB" || name == "FLUSHALL") co_return co_await HandleFlush(socket, engine);
        if (name == "QUIT")
        {
            (void) co_await ReplyOk(socket);
            socket.Close();
            co_return false; // signal session end
        }
        if (name == "AUTH")
            co_return co_await ReplyError(socket, "Client sent AUTH, but no password is set");
        co_return co_await ReplyError(socket, std::format("unknown command '{}'", name));
    }

} // namespace

std::string_view RedisRespHandler::ServerVersion() noexcept
{
    return "fastcached-0.0.1";
}

Task<void> RedisRespHandler::Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes)
{
    ByteReader reader { socket, kMaxLineBytes, kMaxPayloadBytes };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

    while (true)
    {
        auto cmd = co_await ReadOneCommand(reader);
        if (!cmd.has_value())
            co_return; // truncated / malformed — drop connection
        if (cmd->args.empty())
            continue;
        auto const keepGoing = co_await Dispatch(socket, engine, *cmd);
        if (!keepGoing)
            co_return;
    }
}

} // namespace FastCache
