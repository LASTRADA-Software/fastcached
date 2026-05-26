// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/MemcachedBinary.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

namespace
{

    constexpr std::byte RequestMagic { 0x80 };
    constexpr std::byte ResponseMagic { 0x81 };
    constexpr std::size_t HeaderSize = 24;

    enum class Opcode : std::uint8_t
    {
        Get = 0x00,
        Set = 0x01,
        Add = 0x02,
        Replace = 0x03,
        Delete = 0x04,
        Increment = 0x05,
        Decrement = 0x06,
        Quit = 0x07,
        Flush = 0x08,
        GetQ = 0x09,
        NoOp = 0x0a,
        Version = 0x0b,
        GetK = 0x0c,
        GetKQ = 0x0d,
        Append = 0x0e,
        Prepend = 0x0f,
        Stat = 0x10,
        SetQ = 0x11,
        AddQ = 0x12,
        ReplaceQ = 0x13,
        DeleteQ = 0x14,
        IncrementQ = 0x15,
        DecrementQ = 0x16,
        QuitQ = 0x17,
        FlushQ = 0x18,
        AppendQ = 0x19,
        PrependQ = 0x1a,
        SaslList = 0x20,
        SaslAuth = 0x21,
        SaslStep = 0x22,
    };

    enum class Status : std::uint16_t
    {
        Ok = 0x00,
        KeyNotFound = 0x01,
        KeyExists = 0x02,
        ValueTooLarge = 0x03,
        InvalidArguments = 0x04,
        ItemNotStored = 0x05,
        IncrOnNonNumeric = 0x06,
        AuthError = 0x20,
        UnknownCommand = 0x81,
        OutOfMemory = 0x82,
    };

    struct RequestHeader
    {
        std::uint8_t magic;
        std::uint8_t opcode;
        std::uint16_t keyLen;
        std::uint8_t extrasLen;
        std::uint8_t dataType;
        std::uint16_t status; // unused in requests (vbucket id)
        std::uint32_t totalBodyLen;
        std::uint32_t opaque;
        std::uint64_t cas;
    };

    [[nodiscard]] bool ParseHeader(std::span<std::byte const> bytes, RequestHeader& out)
    {
        if (bytes.size() < HeaderSize)
            return false;
        out.magic = std::to_integer<std::uint8_t>(bytes[0]);
        out.opcode = std::to_integer<std::uint8_t>(bytes[1]);
        out.keyLen = ReadBigEndian<std::uint16_t>(bytes.subspan(2));
        out.extrasLen = std::to_integer<std::uint8_t>(bytes[4]);
        out.dataType = std::to_integer<std::uint8_t>(bytes[5]);
        out.status = ReadBigEndian<std::uint16_t>(bytes.subspan(6));
        out.totalBodyLen = ReadBigEndian<std::uint32_t>(bytes.subspan(8));
        out.opaque = ReadBigEndian<std::uint32_t>(bytes.subspan(12));
        out.cas = ReadBigEndian<std::uint64_t>(bytes.subspan(16));
        return true;
    }

    Task<bool> WriteResponse(ISocket& socket,
                             Opcode opcode,
                             Status status,
                             std::uint32_t opaque,
                             std::uint64_t cas,
                             std::span<std::byte const> extras,
                             std::span<std::byte const> key,
                             std::span<std::byte const> value)
    {
        std::array<std::byte, HeaderSize> hdr {};
        hdr[0] = ResponseMagic;
        hdr[1] = std::byte { static_cast<std::uint8_t>(opcode) };
        WriteBigEndian<std::uint16_t>(std::span<std::byte> { &hdr[2], 2 }, static_cast<std::uint16_t>(key.size()));
        hdr[4] = std::byte { static_cast<std::uint8_t>(extras.size()) };
        hdr[5] = std::byte { 0 };
        WriteBigEndian<std::uint16_t>(std::span<std::byte> { &hdr[6], 2 }, static_cast<std::uint16_t>(status));
        WriteBigEndian<std::uint32_t>(std::span<std::byte> { &hdr[8], 4 },
                                      static_cast<std::uint32_t>(extras.size() + key.size() + value.size()));
        WriteBigEndian<std::uint32_t>(std::span<std::byte> { &hdr[12], 4 }, opaque);
        WriteBigEndian<std::uint64_t>(std::span<std::byte> { &hdr[16], 8 }, cas);

        auto const r1 = co_await socket.Write(std::span<std::byte const> { hdr.data(), hdr.size() });
        if (!r1.has_value())
            co_return false;
        if (!extras.empty())
        {
            auto const r = co_await socket.Write(extras);
            if (!r.has_value())
                co_return false;
        }
        if (!key.empty())
        {
            auto const r = co_await socket.Write(key);
            if (!r.has_value())
                co_return false;
        }
        if (!value.empty())
        {
            auto const r = co_await socket.Write(value);
            if (!r.has_value())
                co_return false;
        }
        co_return true;
    }

    Task<bool> ReplyError(ISocket& socket, Opcode opcode, Status status, std::uint32_t opaque)
    {
        std::string_view const msg = status == Status::KeyNotFound        ? "Not found"
                                     : status == Status::KeyExists        ? "Data exists for key"
                                     : status == Status::ItemNotStored    ? "Not stored"
                                     : status == Status::IncrOnNonNumeric ? "Non-numeric server-side value"
                                     : status == Status::InvalidArguments ? "Invalid arguments"
                                     : status == Status::AuthError        ? "Auth failure"
                                                                          : "Internal error";
        co_return co_await WriteResponse(socket, opcode, status, opaque, 0, {}, {}, AsBytes(msg));
    }

    [[nodiscard]] Status MapStorageError(StorageErrorCode code) noexcept
    {
        switch (code)
        {
            case StorageErrorCode::KeyNotFound:
                return Status::KeyNotFound;
            case StorageErrorCode::KeyExists:
                return Status::KeyExists;
            case StorageErrorCode::CasMismatch:
                return Status::KeyExists;
            case StorageErrorCode::ValueTooLarge:
                return Status::ValueTooLarge;
            case StorageErrorCode::OutOfMemory:
                return Status::OutOfMemory;
            case StorageErrorCode::InvalidArgument:
                return Status::InvalidArguments;
            default:
                return Status::UnknownCommand;
        }
    }

    Task<bool> HandleStorage(ISocket& socket,
                             CacheEngine& engine,
                             Opcode opcode,
                             RequestHeader const& header,
                             std::span<std::byte const> extras,
                             std::span<std::byte const> key,
                             std::span<std::byte const> value)
    {
        if (extras.size() < 8)
            co_return co_await ReplyError(socket, opcode, Status::InvalidArguments, header.opaque);
        auto const flags = ReadBigEndian<std::uint32_t>(extras);
        auto const exptime = ReadBigEndian<std::uint32_t>(extras.subspan(4));

        std::vector<std::byte> valVec { value.begin(), value.end() };
        std::string keyStr;
        keyStr.reserve(key.size());
        for (auto const b: key)
            keyStr.push_back(static_cast<char>(b));

        std::expected<CasToken, StorageError> result { 0 };
        bool const isQuiet = opcode == Opcode::SetQ || opcode == Opcode::AddQ || opcode == Opcode::ReplaceQ
                             || opcode == Opcode::AppendQ || opcode == Opcode::PrependQ;
        auto const normalised =
            isQuiet ? static_cast<Opcode>(static_cast<std::uint8_t>(opcode) & ~std::uint8_t { 0x10 }) : opcode;
        switch (normalised)
        {
            case Opcode::Set:
                result = engine.Set(keyStr, std::move(valVec), flags, exptime);
                break;
            case Opcode::Add:
                result = engine.Add(keyStr, std::move(valVec), flags, exptime);
                break;
            case Opcode::Replace:
                result = engine.Replace(keyStr, std::move(valVec), flags, exptime);
                break;
            case Opcode::Append:
                result = engine.Append(keyStr, value);
                break;
            case Opcode::Prepend:
                result = engine.Prepend(keyStr, value);
                break;
            default:
                break;
        }

        if (!result.has_value())
        {
            if (isQuiet)
                co_return true;
            co_return co_await ReplyError(socket, opcode, MapStorageError(result.error().code), header.opaque);
        }
        if (isQuiet)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, *result, {}, {}, {});
    }

    Task<bool> HandleGet(
        ISocket& socket, CacheEngine& engine, Opcode opcode, RequestHeader const& header, std::span<std::byte const> key)
    {
        std::string keyStr;
        keyStr.reserve(key.size());
        for (auto const b: key)
            keyStr.push_back(static_cast<char>(b));

        auto const result = engine.Get(keyStr);
        bool const includeKey = opcode == Opcode::GetK || opcode == Opcode::GetKQ;
        bool const quiet = opcode == Opcode::GetQ || opcode == Opcode::GetKQ;

        if (!result.has_value() || !result->found)
        {
            if (quiet)
                co_return true;
            co_return co_await ReplyError(socket, opcode, Status::KeyNotFound, header.opaque);
        }

        auto const& entry = result->entry;
        std::array<std::byte, 4> extras {};
        WriteBigEndian<std::uint32_t>(extras, entry.flags);
        std::span<std::byte const> const keyOut = includeKey ? key : std::span<std::byte const> {};
        co_return co_await WriteResponse(socket,
                                         opcode,
                                         Status::Ok,
                                         header.opaque,
                                         entry.cas,
                                         std::span<std::byte const> { extras.data(), extras.size() },
                                         keyOut,
                                         std::span<std::byte const> { entry.value.data(), entry.value.size() });
    }

    Task<bool> HandleDelete(
        ISocket& socket, CacheEngine& engine, Opcode opcode, RequestHeader const& header, std::span<std::byte const> key)
    {
        std::string keyStr;
        keyStr.reserve(key.size());
        for (auto const b: key)
            keyStr.push_back(static_cast<char>(b));

        auto const result = engine.Delete(keyStr);
        bool const quiet = opcode == Opcode::DeleteQ;
        if (!result.has_value())
        {
            if (quiet)
                co_return true;
            co_return co_await ReplyError(socket, opcode, Status::KeyNotFound, header.opaque);
        }
        if (quiet)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    Task<bool> HandleVersion(ISocket& socket, RequestHeader const& header)
    {
        auto const ver = ServerVersionBanner;
        co_return co_await WriteResponse(socket, Opcode::Version, Status::Ok, header.opaque, 0, {}, {}, AsBytes(ver));
    }

    Task<bool> HandleNoOp(ISocket& socket, RequestHeader const& header)
    {
        co_return co_await WriteResponse(socket, Opcode::NoOp, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    Task<bool> HandleFlush(ISocket& socket, CacheEngine& engine, Opcode opcode, RequestHeader const& header)
    {
        engine.FlushAll(0);
        if (opcode == Opcode::FlushQ)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
    }

} // namespace

Task<void> MemcachedBinaryHandler::Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes)
{
    constexpr std::size_t MaxBodyBytes = 16 * 1024 * 1024;
    ByteReader reader { socket, /*maxLineBytes*/ 1, /*maxPayloadBytes*/ MaxBodyBytes + HeaderSize };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

    while (true)
    {
        auto const headerBytes = co_await reader.ReadExactly(HeaderSize);
        if (!headerBytes.has_value())
            co_return;

        RequestHeader header {};
        if (!ParseHeader(std::span<std::byte const> { headerBytes->data(), headerBytes->size() }, header))
            co_return;
        if (header.magic != static_cast<std::uint8_t>(RequestMagic))
            co_return; // protocol error — drop the connection
        if (header.totalBodyLen > MaxBodyBytes)
            co_return;

        auto const body = co_await reader.ReadExactly(header.totalBodyLen);
        if (!body.has_value())
            co_return;

        std::span<std::byte const> const bodySpan { body->data(), body->size() };
        if (bodySpan.size() < std::size_t { header.extrasLen } + header.keyLen)
            co_return;
        auto const extras = bodySpan.first(header.extrasLen);
        auto const key = bodySpan.subspan(header.extrasLen, header.keyLen);
        auto const value = bodySpan.subspan(static_cast<std::size_t>(header.extrasLen) + header.keyLen);

        auto const opcode = static_cast<Opcode>(header.opcode);
        bool keepGoing = true;
        switch (opcode)
        {
            case Opcode::Get:
            case Opcode::GetQ:
            case Opcode::GetK:
            case Opcode::GetKQ:
                keepGoing = co_await HandleGet(socket, engine, opcode, header, key);
                break;
            case Opcode::Set:
            case Opcode::SetQ:
            case Opcode::Add:
            case Opcode::AddQ:
            case Opcode::Replace:
            case Opcode::ReplaceQ:
            case Opcode::Append:
            case Opcode::AppendQ:
            case Opcode::Prepend:
            case Opcode::PrependQ:
                keepGoing = co_await HandleStorage(socket, engine, opcode, header, extras, key, value);
                break;
            case Opcode::Delete:
            case Opcode::DeleteQ:
                keepGoing = co_await HandleDelete(socket, engine, opcode, header, key);
                break;
            case Opcode::Quit:
                (void) co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
                socket.Close();
                co_return;
            case Opcode::QuitQ:
                socket.Close();
                co_return;
            case Opcode::NoOp:
                keepGoing = co_await HandleNoOp(socket, header);
                break;
            case Opcode::Version:
                keepGoing = co_await HandleVersion(socket, header);
                break;
            case Opcode::Flush:
            case Opcode::FlushQ:
                keepGoing = co_await HandleFlush(socket, engine, opcode, header);
                break;
            case Opcode::SaslList:
            case Opcode::SaslAuth:
            case Opcode::SaslStep:
                keepGoing = co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);
                break;
            default:
                keepGoing = co_await ReplyError(socket, opcode, Status::UnknownCommand, header.opaque);
                break;
        }
        if (!keepGoing)
            co_return;
    }
}

} // namespace FastCache
