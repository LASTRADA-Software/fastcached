// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/CacheEntry.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/MemcachedBinary.hpp>
#include <FastCache/Protocol/MemcachedShared.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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
        Verbosity = 0x1b,
        Touch = 0x1c,
        Gat = 0x1d,
        GatQ = 0x1e,
        SaslList = 0x20,
        SaslAuth = 0x21,
        SaslStep = 0x22,
        GatK = 0x23,
        GatKQ = 0x24,
    };

    enum class Status : std::uint8_t
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
        NotSupported = 0x83,
        InternalError = 0x84,
        Busy = 0x85,
        TemporaryFailure = 0x86,
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

    Task<bool> WriteResponse(ISocket* socket,
                             Opcode opcode,
                             Status status,
                             std::uint32_t opaque,
                             std::uint64_t cas,
                             std::span<std::byte const> extras,
                             std::span<std::byte const> key,
                             std::span<std::byte const> value,
                             std::shared_ptr<void const> keepAlive = {})
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

        // Gather header + extras + key + value into a single scattered write:
        // the value segment points directly at the cached, reference-counted
        // payload (no copy), and `keepAlive` keeps that payload alive across a
        // write that may suspend. `hdr` lives in this coroutine frame, which is
        // suspended (not destroyed) across the co_await, so its address is
        // stable for the duration of the write. Empty segments are skipped by
        // the socket layer. The hot path is syscall-bound, so one sendmsg per
        // reply beats up to four send()s — and now without the copy.
        std::array<std::span<std::byte const>, 4> const segments {
            std::span<std::byte const> { hdr.data(), hdr.size() },
            extras,
            key,
            value,
        };
        co_return co_await WriteAllVectored(socket, segments, std::move(keepAlive));
    }

    [[nodiscard]] constexpr std::string_view ErrorMessage(Status status) noexcept
    {
        switch (status)
        {
            case Status::KeyNotFound:
                return "Not found";
            case Status::KeyExists:
                return "Data exists for key";
            case Status::ItemNotStored:
                return "Not stored";
            case Status::IncrOnNonNumeric:
                return "Non-numeric server-side value";
            case Status::InvalidArguments:
                return "Invalid arguments";
            case Status::AuthError:
                return "Auth failure";
            case Status::NotSupported:
                return "Not supported";
            case Status::Busy:
                return "Busy";
            case Status::TemporaryFailure:
                return "Temporary failure";
            case Status::ValueTooLarge:
                return "Value too large";
            case Status::OutOfMemory:
                return "Out of memory";
            case Status::UnknownCommand:
                return "Unknown command";
            default:
                return "Internal error";
        }
    }

    Task<bool> ReplyError(ISocket* socket, Opcode opcode, Status status, std::uint32_t opaque)
    {
        co_return co_await WriteResponse(socket, opcode, status, opaque, 0, {}, {}, AsBytes(ErrorMessage(status)));
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
            case StorageErrorCode::WrongType:
                // append/prepend onto a typed value (a redis set/stream sharing
                // the keyspace) is refused, not corrupting it; the binary
                // protocol's closest "couldn't store this item" status.
                return Status::ItemNotStored;
            default:
                return Status::UnknownCommand;
        }
    }

    Task<bool> HandleStorage(ISocket* socket,
                             CacheEngine* engine,
                             Opcode opcode,
                             RequestHeader header,
                             std::span<std::byte const> extras,
                             std::span<std::byte const> key,
                             std::span<std::byte const> value)
    {
        if (extras.size() < 8)
            co_return co_await ReplyError(socket, opcode, Status::InvalidArguments, header.opaque);
        auto const flags = ReadBigEndian<std::uint32_t>(extras);
        auto const exptime = ReadBigEndian<std::uint32_t>(extras.subspan(4));

        std::vector<std::byte> valVec { value.begin(), value.end() };
        auto const keyView = AsStringView(key);

        std::expected<CasToken, StorageError> result { 0 };

        enum class StorageVerb : std::uint8_t
        {
            Set,
            Add,
            Replace,
            Append,
            Prepend
        };

        /// One row per storage opcode mapping it to its base verb and quiet
        /// bit. An explicit table (rather than the historical `opcode & ~0x10`
        /// normalisation) is required because Append/Prepend's quiet variants
        /// are 0x19/0x1a, not base (0x0e/0x0f) + 0x10 — the bit-clear produced
        /// GetQ/NoOp and the storage silently no-op'd.
        struct StorageOpcode
        {
            Opcode opcode;
            StorageVerb verb;
            bool quiet;
        };
        static constexpr std::array<StorageOpcode, 10> StorageOpcodes { {
            { .opcode = Opcode::Set, .verb = StorageVerb::Set, .quiet = false },
            { .opcode = Opcode::SetQ, .verb = StorageVerb::Set, .quiet = true },
            { .opcode = Opcode::Add, .verb = StorageVerb::Add, .quiet = false },
            { .opcode = Opcode::AddQ, .verb = StorageVerb::Add, .quiet = true },
            { .opcode = Opcode::Replace, .verb = StorageVerb::Replace, .quiet = false },
            { .opcode = Opcode::ReplaceQ, .verb = StorageVerb::Replace, .quiet = true },
            { .opcode = Opcode::Append, .verb = StorageVerb::Append, .quiet = false },
            { .opcode = Opcode::AppendQ, .verb = StorageVerb::Append, .quiet = true },
            { .opcode = Opcode::Prepend, .verb = StorageVerb::Prepend, .quiet = false },
            { .opcode = Opcode::PrependQ, .verb = StorageVerb::Prepend, .quiet = true },
        } };
        // Resolve to a raw pointer (or nullptr). std::ranges::find over a
        // std::array yields a wrapped iterator on MSVC but a raw pointer on
        // libc++; binding either to a named auto makes clang-tidy's
        // readability-qualified-auto and MSVC's type deduction disagree. A
        // range-based scan into an explicit pointer is portable across both.
        StorageOpcode const* descriptor = nullptr;
        for (StorageOpcode const& candidate: StorageOpcodes)
        {
            if (candidate.opcode == opcode)
            {
                descriptor = &candidate;
                break;
            }
        }
        StorageVerb const verb = descriptor != nullptr ? descriptor->verb : StorageVerb::Set;
        bool const isQuiet = descriptor != nullptr && descriptor->quiet;

        // A non-zero header CAS turns Set/Replace into a conditional store
        // (the spec's compare-and-swap): store only if the current CAS
        // matches, else KeyExists (mapped from CasMismatch). A zero CAS is
        // an unconditional store.
        bool const casConditional = header.cas != 0;
        switch (verb)
        {
            case StorageVerb::Set:
                result = casConditional ? engine->CompareAndSwap(keyView, header.cas, std::move(valVec), flags, exptime)
                                        : engine->Set(keyView, std::move(valVec), flags, exptime);
                break;
            case StorageVerb::Add:
                // Add requires the key to be absent; a CAS token is
                // meaningless here, so it is ignored per memcached.
                result = engine->Add(keyView, std::move(valVec), flags, exptime);
                break;
            case StorageVerb::Replace:
                result = casConditional ? engine->CompareAndSwap(keyView, header.cas, std::move(valVec), flags, exptime)
                                        : engine->Replace(keyView, std::move(valVec), flags, exptime);
                break;
            case StorageVerb::Append:
                result = engine->Append(keyView, value);
                break;
            case StorageVerb::Prepend:
                result = engine->Prepend(keyView, value);
                break;
        }

        // Per the memcached binary spec, quiet mutations suppress only the
        // success reply — "errors should not be allowed to go unnoticed", so
        // a failure still emits an error packet even for the quiet opcode.
        if (!result.has_value())
            co_return co_await ReplyError(socket, opcode, MapStorageError(result.error().code), header.opaque);
        if (isQuiet)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, *result, {}, {}, {});
    }

    Task<bool> HandleGet(
        ISocket* socket, CacheEngine* engine, Opcode opcode, RequestHeader header, std::span<std::byte const> key)
    {
        auto const result = engine->Get(AsStringView(key));
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
        // Hand the value's reference-counted buffer to the write as keep-alive
        // so the zero-copy value segment stays valid across a suspended write.
        co_return co_await WriteResponse(socket,
                                         opcode,
                                         Status::Ok,
                                         header.opaque,
                                         entry.cas,
                                         std::span<std::byte const> { extras.data(), extras.size() },
                                         keyOut,
                                         entry.ValueBytes(),
                                         entry.value.AsKeepAlive());
    }

    Task<bool> HandleDelete(
        ISocket* socket, CacheEngine* engine, Opcode opcode, RequestHeader header, std::span<std::byte const> key)
    {
        auto const result = engine->Delete(AsStringView(key));
        bool const quiet = opcode == Opcode::DeleteQ;
        // DeleteQ is a quiet mutation: it suppresses only the success reply.
        // A miss (KeyNotFound) is a failure and must still emit an error
        // packet per the memcached binary spec.
        if (!result.has_value())
            co_return co_await ReplyError(socket, opcode, Status::KeyNotFound, header.opaque);
        if (quiet)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    Task<bool> HandleVersion(ISocket* socket, RequestHeader header)
    {
        auto const ver = ServerVersionBanner;
        co_return co_await WriteResponse(socket, Opcode::Version, Status::Ok, header.opaque, 0, {}, {}, AsBytes(ver));
    }

    Task<bool> HandleNoOp(ISocket* socket, RequestHeader header)
    {
        co_return co_await WriteResponse(socket, Opcode::NoOp, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    Task<bool> HandleFlush(ISocket* socket, CacheEngine* engine, Opcode opcode, RequestHeader header)
    {
        engine->FlushAll(0);
        if (opcode == Opcode::FlushQ)
            co_return true;
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    /// memcached binary `touch` and `gat` / `gat*` family. The shared
    /// branch (touch vs gat) is captured by `withValue`.
    Task<bool> HandleTouchFamily(ISocket* socket,
                                 CacheEngine* engine,
                                 Opcode opcode,
                                 RequestHeader header,
                                 std::span<std::byte const> extras,
                                 std::span<std::byte const> key)
    {
        if (extras.size() < 4)
            co_return co_await ReplyError(socket, opcode, Status::InvalidArguments, header.opaque);
        auto const exptime = ReadBigEndian<std::uint32_t>(extras);
        bool const isTouch = opcode == Opcode::Touch;
        bool const includeKey = opcode == Opcode::GatK || opcode == Opcode::GatKQ;
        bool const isQuiet = opcode == Opcode::GatQ || opcode == Opcode::GatKQ;
        auto const keyView = AsStringView(key);

        if (isTouch)
        {
            auto const touched = engine->Touch(keyView, exptime);
            if (!touched.has_value())
                co_return co_await ReplyError(socket, opcode, MapStorageError(touched.error().code), header.opaque);
            co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, *touched, {}, {}, {});
        }

        // GAT family: refresh the expiry and read the value back in a
        // single atomic step so no concurrent writer can slip between the
        // touch and the read.
        auto const got = engine->GetAndTouch(keyView, exptime);
        // GAT is a quiet *read*: like GetQ it is "mum on cache miss", so a
        // miss is suppressed for the quiet variant. A miss surfaces here as
        // either KeyNotFound (the touch step found nothing) or a value with
        // found == false. Any *other* storage error is a genuine failure and
        // must always surface — quiet silences the miss, not real failures.
        bool const isMiss =
            (!got.has_value() && got.error().code == StorageErrorCode::KeyNotFound) || (got.has_value() && !got->found);
        if (isMiss)
        {
            if (isQuiet)
                co_return true;
            co_return co_await ReplyError(socket, opcode, Status::KeyNotFound, header.opaque);
        }
        if (!got.has_value())
            co_return co_await ReplyError(socket, opcode, MapStorageError(got.error().code), header.opaque);
        auto const& entry = got->entry;
        std::array<std::byte, 4> outExtras {};
        WriteBigEndian<std::uint32_t>(outExtras, entry.flags);
        std::span<std::byte const> const keyOut = includeKey ? key : std::span<std::byte const> {};
        co_return co_await WriteResponse(socket,
                                         opcode,
                                         Status::Ok,
                                         header.opaque,
                                         entry.cas,
                                         std::span<std::byte const> { outExtras.data(), outExtras.size() },
                                         keyOut,
                                         entry.ValueBytes(),
                                         entry.value.AsKeepAlive());
    }

    /// memcached binary increment / decrement, including auto-vivify on
    /// miss when expiration != 0xffffffff (the spec's sentinel for "fail
    /// rather than create").
    Task<bool> HandleArithmetic(ISocket* socket,
                                CacheEngine* engine,
                                Opcode opcode,
                                RequestHeader header,
                                std::span<std::byte const> extras,
                                std::span<std::byte const> key)
    {
        if (extras.size() < 20)
            co_return co_await ReplyError(socket, opcode, Status::InvalidArguments, header.opaque);
        auto const delta = ReadBigEndian<std::uint64_t>(extras);
        auto const initial = ReadBigEndian<std::uint64_t>(extras.subspan(8));
        auto const expiration = ReadBigEndian<std::uint32_t>(extras.subspan(16));

        bool const isQuiet = opcode == Opcode::IncrementQ || opcode == Opcode::DecrementQ;
        bool const isIncr = opcode == Opcode::Increment || opcode == Opcode::IncrementQ;
        auto const keyView = AsStringView(key);

        // Try the existing key first.
        auto result = isIncr ? engine->Increment(keyView, delta) : engine->Decrement(keyView, delta);

        // IncrementQ / DecrementQ are quiet mutations: per the memcached
        // binary spec they suppress only the success reply, so every failure
        // below still emits an error packet even for the quiet variant.
        if (!result.has_value() && result.error().code == StorageErrorCode::KeyNotFound)
        {
            // Spec: expiration of 0xffffffff means "do NOT create on miss".
            if (expiration == 0xFFFFFFFFU)
                co_return co_await ReplyError(socket, opcode, Status::KeyNotFound, header.opaque);
            // Auto-vivify with the initial value and the given expiration.
            auto const set = engine->Set(keyView, BytesFromString(std::to_string(initial)), 0, expiration);
            if (!set.has_value())
                co_return co_await ReplyError(socket, opcode, MapStorageError(set.error().code), header.opaque);
            result = IStorage::IncrResult { .value = initial, .cas = *set };
        }
        else if (!result.has_value())
        {
            // Non-numeric existing value or other error.
            auto const code = result.error().code;
            auto const status = code == StorageErrorCode::InvalidArgument ? Status::IncrOnNonNumeric : MapStorageError(code);
            co_return co_await ReplyError(socket, opcode, status, header.opaque);
        }

        if (isQuiet)
            co_return true;

        // Response: 8-byte big-endian new value.
        std::array<std::byte, 8> outValue {};
        WriteBigEndian<std::uint64_t>(outValue, result->value);
        co_return co_await WriteResponse(socket,
                                         opcode,
                                         Status::Ok,
                                         header.opaque,
                                         result->cas,
                                         {},
                                         {},
                                         std::span<std::byte const> { outValue.data(), outValue.size() });
    }

    /// Render one `STAT key value` packet for the binary Stat command.
    Task<bool> WriteStatLine(ISocket* socket, std::uint32_t opaque, std::string_view name, std::string value)
    {
        co_return co_await WriteResponse(socket, Opcode::Stat, Status::Ok, opaque, 0, {}, AsBytes(name), AsBytes(value));
    }

    Task<bool> HandleStat(ISocket* socket, CacheEngine* engine, RequestHeader header, std::span<std::byte const> key)
    {
        auto const stats = engine->Snapshot();
        auto const subkey = AsStringView(key);
        // Only the empty sub-key is exhaustive; other sub-keys (settings,
        // items, ...) get a minimal shape today and are filled out by the
        // text-protocol stats expansion. For binary clients that probe
        // capabilities, returning *some* data is better than UnknownCommand.
        // A plain forwarding lambda (not itself a coroutine): the `std::string`
        // value is moved into `WriteStatLine`'s frame, so nothing dangles and
        // the coroutine-capturing-lambda lint does not apply.
        auto const emit = [&](std::string_view name, std::uint64_t v) {
            return WriteStatLine(socket, header.opaque, name, std::to_string(v));
        };

        if (subkey.empty() || subkey == "default")
        {
            std::array<std::pair<std::string_view, std::uint64_t>, 23> const table { {
                { "curr_items", stats.itemCount },
                { "bytes", stats.bytesUsed },
                { "limit_maxbytes", stats.bytesLimit },
                { "evictions", stats.evictions },
                { "cmd_get", stats.cmdGet },
                { "cmd_set", stats.cmdSet },
                { "cmd_touch", stats.cmdTouch },
                { "cmd_flush", stats.cmdFlush },
                { "get_hits", stats.getHits },
                { "get_misses", stats.getMisses },
                { "delete_hits", stats.deleteHits },
                { "delete_misses", stats.deleteMisses },
                { "touch_hits", stats.touchHits },
                { "touch_misses", stats.touchMisses },
                { "cas_hits", stats.casHits },
                { "cas_misses", stats.casMisses },
                { "cas_badval", stats.casBadval },
                { "incr_hits", stats.incrHits },
                { "incr_misses", stats.incrMisses },
                { "decr_hits", stats.decrHits },
                { "decr_misses", stats.decrMisses },
                { "evicted_unfetched", stats.evictedUnfetched },
                { "expired_unfetched", stats.expiredUnfetched },
            } };
            for (auto const& [name, value]: table)
                if (!co_await emit(name, value))
                    co_return false;
        }
        // Empty-key terminator packet — spec-required end-of-stats marker.
        co_return co_await WriteResponse(socket, Opcode::Stat, Status::Ok, header.opaque, 0, {}, {}, {});
    }

    /// Opcodes a client may issue before authenticating when a credential is
    /// required: the SASL handshake plus the connection-control / no-op verbs.
    /// Every other opcode replies AuthError until SASL auth succeeds.
    [[nodiscard]] constexpr bool IsPreAuthAllowed(Opcode opcode) noexcept
    {
        switch (opcode)
        {
            case Opcode::SaslList:
            case Opcode::SaslAuth:
            case Opcode::SaslStep:
            case Opcode::Quit:
            case Opcode::QuitQ:
            case Opcode::Version:
            case Opcode::NoOp:
                return true;
            default:
                return false;
        }
    }

    /// Handle a SASL opcode. With no configured AuthPolicy (auth disabled) every
    /// SASL opcode replies AuthError, preserving the historical "no SASL"
    /// behaviour so non-authing clients fall back to the plain path. With a
    /// policy configured, SaslList advertises PLAIN, SaslAuth verifies a PLAIN
    /// `authzid\0authcid\0passwd` payload and flips `authenticated`, and SaslStep
    /// (PLAIN is single-step) replies AuthError.
    /// @param authenticated Per-connection flag, set true on a successful auth.
    Task<bool> HandleSasl(ISocket* socket,
                          Opcode opcode,
                          RequestHeader header,
                          std::span<std::byte const> key,
                          std::span<std::byte const> value,
                          SessionContext session,
                          bool* authenticated)
    {
        auto const auth = session.CurrentAuth();
        bool const authEnabled = auth != nullptr && auth->Enabled();
        if (!authEnabled)
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);

        if (opcode == Opcode::SaslList)
        {
            constexpr std::string_view Mechanisms = "PLAIN";
            co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, AsBytes(Mechanisms));
        }
        if (opcode == Opcode::SaslStep)
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);

        // SaslAuth: key = mechanism name, value = SASL initial response.
        std::string_view const mechanism { reinterpret_cast<char const*>(key.data()), key.size() };
        if (mechanism != "PLAIN")
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);

        // PLAIN payload is three NUL-separated fields: authzid, authcid, passwd.
        std::string_view const payload { reinterpret_cast<char const*>(value.data()), value.size() };
        auto const firstNul = payload.find('\0');
        if (firstNul == std::string_view::npos)
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);
        auto const secondNul = payload.find('\0', firstNul + 1);
        if (secondNul == std::string_view::npos)
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);
        auto const authcid = payload.substr(firstNul + 1, secondNul - firstNul - 1);
        auto const passwd = payload.substr(secondNul + 1);

        // An empty authcid means "the default user": validate the password alone
        // (the redis one-argument AUTH semantics), so a minimal PLAIN client that
        // sends `\0\0<pass>` is not locked out. A non-empty authcid must match the
        // configured username.
        bool const ok = authcid.empty() ? auth->Verify(passwd) : auth->Verify(authcid, passwd);
        if (!ok)
            co_return co_await ReplyError(socket, opcode, Status::AuthError, header.opaque);

        *authenticated = true;
        constexpr std::string_view Granted = "Authenticated";
        co_return co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, AsBytes(Granted));
    }

} // namespace

Task<void> MemcachedBinaryHandler::Run(ISocket* socket,
                                       CacheEngine* engine,
                                       std::vector<std::byte> primingBytes,
                                       SessionContext session)
{
    constexpr std::size_t MaxBodyBytes = 16 * 1024 * 1024;
    ByteReader reader { *socket, /*maxLineBytes*/ 1, /*maxPayloadBytes*/ MaxBodyBytes + HeaderSize };
    reader.PrimeWith(std::span<std::byte const> { primingBytes.data(), primingBytes.size() });

    // Authenticated up-front unless a credential is required at session start;
    // SASL flips this. Re-evaluated against the live auth source on every
    // command so SIGHUP reload of requirepass is honoured immediately.
    bool authenticated = !(session.CurrentAuth() != nullptr && session.CurrentAuth()->Enabled());

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

        auto const opcode = static_cast<Opcode>(header.opcode);
        auto const authNow = session.CurrentAuth();
        bool const authEnabled = authNow != nullptr && authNow->Enabled();
        if (!authEnabled)
            authenticated = true; // Reload turned auth off; no further gate.

        // Gate data commands until authenticated when a credential is required.
        // This MUST happen before the body is buffered: an unauthenticated
        // attacker can otherwise pipeline requests with totalBodyLen close to
        // the per-frame cap (16 MiB) and force the server to allocate that
        // much memory per request before being told to authenticate. We drain
        // the body without buffering it, then reply AuthError.
        if (authEnabled && !authenticated && !IsPreAuthAllowed(opcode))
        {
            if (auto const skipped = co_await reader.Skip(header.totalBodyLen); !skipped.has_value())
                co_return;
            if (!co_await ReplyError(socket, opcode, Status::AuthError, header.opaque))
                co_return;
            FC_FRAME_MARK;
            continue;
        }

        auto const body = co_await reader.ReadExactly(header.totalBodyLen);
        if (!body.has_value())
            co_return;

        std::span<std::byte const> const bodySpan { body->data(), body->size() };
        if (bodySpan.size() < std::size_t { header.extrasLen } + header.keyLen)
            co_return;
        auto const extras = bodySpan.first(header.extrasLen);
        auto const key = bodySpan.subspan(header.extrasLen, header.keyLen);
        auto const value = bodySpan.subspan(static_cast<std::size_t>(header.extrasLen) + header.keyLen);

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
                socket->Close();
                co_return;
            case Opcode::QuitQ:
                socket->Close();
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
            case Opcode::Touch:
            case Opcode::Gat:
            case Opcode::GatQ:
            case Opcode::GatK:
            case Opcode::GatKQ:
                keepGoing = co_await HandleTouchFamily(socket, engine, opcode, header, extras, key);
                break;
            case Opcode::Increment:
            case Opcode::IncrementQ:
            case Opcode::Decrement:
            case Opcode::DecrementQ:
                keepGoing = co_await HandleArithmetic(socket, engine, opcode, header, extras, key);
                break;
            case Opcode::Stat:
                keepGoing = co_await HandleStat(socket, engine, header, key);
                break;
            case Opcode::Verbosity:
                // Accepted as a no-op — fastcached logs via ILogger, not via
                // the verbosity dial. Reply Ok so capability-probing clients
                // continue rather than abort.
                keepGoing = co_await WriteResponse(socket, opcode, Status::Ok, header.opaque, 0, {}, {}, {});
                break;
            case Opcode::SaslList:
            case Opcode::SaslAuth:
            case Opcode::SaslStep:
                keepGoing = co_await HandleSasl(socket, opcode, header, key, value, session, &authenticated);
                break;
            default:
                keepGoing = co_await ReplyError(socket, opcode, Status::UnknownCommand, header.opaque);
                break;
        }
        if (!keepGoing)
            co_return;

        // One binary command handled and replied — mark the request frame.
        FC_FRAME_MARK;
    }
}

} // namespace FastCache
