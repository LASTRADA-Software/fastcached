// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/CompileCache/PrefetchGroupManifest.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache
{
namespace
{

    namespace Wire = CompileCacheWire;

    /// Cap on a single framed line/field's length. The compile-cache protocol
    /// never uses line reads, but ByteReader requires a line cap; set it to the
    /// same generous bound the other handlers use.
    constexpr std::size_t MaxLineBytes = 65536;

    /// Protocol label for this handler's `LogFrameDrop` lines. Matches the name
    /// `ProtocolFlavor::CompileCache` renders to, so a connection log and a frame
    /// drop name the same thing.
    constexpr std::string_view ProtocolLabel = "compile-cache";

    /// How far past `maxPayloadBytes` an over-cap frame may still be drained so
    /// that its refusal can be a *reply* rather than a dropped connection.
    ///
    /// Refusing without draining is what made an over-cap STORE break builds: the
    /// client is mid-send when the server stops reading and closes, so it never
    /// sees the typed `payload-too-large` it was answered with — it sees its own
    /// write fail (and, before issue #68, died of SIGPIPE doing so). "Declared
    /// payload N exceeds cap M" is the one message that makes an operator raise
    /// `--storage-max-value`, and it is worth reading and discarding the frame to
    /// deliver it.
    ///
    /// Bounded, and expressed as a multiple of the operator's own cap rather than
    /// a byte count of its own: the cap is already their statement of the largest
    /// thing this server will handle on a connection, so being willing to discard
    /// a few times that much needs no second knob and scales when they retune the
    /// first one. Past the bound the server closes as before — a peer declaring
    /// gigabytes it was never going to be allowed to store has stopped being a
    /// client worth resynchronizing with.
    constexpr std::uint64_t OversizeDrainFactor = 4;

    /// Interpret a byte span as a UTF-8/ASCII string (copying).
    [[nodiscard]] std::string BytesToString(std::span<std::byte const> bytes)
    {
        return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    }

    /// Write all bytes of `payload` to the socket.
    /// @return true on success, false on socket error.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> payload)
    {
        auto const r = co_await socket->Write(payload);
        // Verify the byte count, not merely that the call succeeded: ISocket::Write
        // is a write-all contract, so a short count is a backend bug that must
        // surface as a failed reply rather than a silently truncated one. A
        // truncated reply is especially bad here — the frame declares its length
        // up front, so the client blocks waiting for bytes that never come.
        co_return r.has_value() && *r == payload.size();
    }

    /// Send one reply frame.
    /// @param socket Client socket.
    /// @param status The outcome.
    /// @param payload Reply body, taken by value so it survives the suspend point.
    /// @return true when the whole reply reached the socket.
    [[nodiscard]] Task<bool> Reply(ISocket* socket, Wire::Status status, std::vector<std::byte> payload)
    {
        auto const frame = Wire::EncodeReply(status, payload);
        co_return co_await WriteAll(socket, frame);
    }

    /// Send one typed error reply.
    /// @param socket Client socket.
    /// @param code The refusal reason.
    /// @param message Detail; the code's default message is used when empty.
    /// @return true when the whole reply reached the socket.
    [[nodiscard]] Task<bool> ReplyError(ISocket* socket, Wire::ErrorCode code, std::string message)
    {
        auto const frame = Wire::EncodeErrorReply(code, message);
        co_return co_await WriteAll(socket, frame);
    }

    /// What the caller's command loop should do after one command is handled.
    enum class Next : std::uint8_t
    {
        Continue, ///< Command complete; read the next one.
        Abort,    ///< Connection is finished (EOF, framing error, or write failure).
    };

    /// Canonicalize every text region of `value` in place using the producer's
    /// layout. The object blob is never touched.
    /// @param value    The decoded compile-value to rewrite.
    /// @param producer The producing machine's roots.
    /// @return True when every region canonicalized; false leaves `value` partial.
    [[nodiscard]] bool CanonicalizeRegions(CompileValue& value, PathCanon::Layout const& producer)
    {
        for (auto& region: value.textRegions)
        {
            auto canon = PathCanon::CanonicalizeRegion(region.bytes, region.grammar, producer);
            if (!canon.has_value())
                return false;
            region.bytes = std::move(*canon);
        }
        return true;
    }

    /// Handle one STORE command: canonicalize with the producer's layout, store
    /// the canonical value, and record prefetch group membership.
    ///
    /// The payload arrives already read and is split synchronously, so this
    /// coroutine has exactly one suspend point (the reply) rather than the five
    /// the field-at-a-time reader used to need.
    /// @param socket   Client socket.
    /// @param engine   Cache engine.
    /// @param manifest Prefetch group manifest.
    /// @param payload  The request payload, by value: a coroutine must not hold a
    ///                 reference parameter across a suspend point, and the field
    ///                 views below point into it.
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleStore(ISocket* socket,
                                         CacheEngine* engine,
                                         PrefetchGroupManifest* manifest,
                                         std::vector<std::byte> payload)
    {
        auto const fields = Wire::DecodeStorePayload(payload);
        if (!fields.has_value())
            co_return co_await ReplyError(socket, Wire::ErrorCode::MalformedFrame, {}) ? Next::Continue : Next::Abort;

        auto decoded = DecodeCompileValue(fields->value);
        if (!decoded.has_value())
            co_return co_await ReplyError(socket, Wire::ErrorCode::MalformedValue, {}) ? Next::Continue : Next::Abort;

        PathCanon::Layout const producer { .sourceRoot = BytesToString(fields->srcRoot),
                                           .buildTree = BytesToString(fields->buildTree) };
        if (!CanonicalizeRegions(*decoded, producer))
            co_return co_await ReplyError(socket, Wire::ErrorCode::CanonicalizationFailed, {}) ? Next::Continue
                                                                                               : Next::Abort;

        auto const canonicalBytes = EncodeCompileValue(*decoded);
        auto const keyStr = BytesToString(fields->key);
        auto const groupStr = BytesToString(fields->prefetchGroup);
        auto const stored = engine->Set(keyStr, canonicalBytes, /*flags=*/0, /*exptime=*/0);
        if (!stored.has_value())
            co_return co_await ReplyError(socket, Wire::ErrorCode::StorageWriteFailed, {}) ? Next::Continue : Next::Abort;

        // Record prefetch group membership (best-effort: a manifest failure must not fail
        // the STORE — the value is already safely stored).
        if (!groupStr.empty())
            (void) manifest->AddKey(groupStr, keyStr, engine->Clock().Now());

        co_return co_await Reply(socket, Wire::Status::Ok, {}) ? Next::Continue : Next::Abort;
    }

    /// Claim the right to warm `prefetch group`, once per cache engine.
    ///
    /// A per-connection set cannot bound this work: a compiler launcher opens a fresh
    /// connection per translation unit, so every one of a build's thousands of fetches
    /// arrived with an empty set and re-warmed the whole prefetch group — a measured 60-hit
    /// build issued 27022 prefetches and 13969 disk reads that way.
    ///
    /// The claim is therefore shared across connections, but keyed on the ENGINE
    /// rather than held in a plain function-local static: a static would outlive the
    /// engine it describes, so a second engine in the same process (a restart, or the
    /// next test case) would inherit "already warmed" for a cache that is in fact
    /// cold and would never prefetch. Entries are keyed by engine address and dropped
    /// when that engine is gone.
    /// @param engine        The engine the prefetch group belongs to.
    /// @param prefetchGroup The group id to claim.
    /// @return True when this call claimed it (the caller should warm it).
    [[nodiscard]] bool ClaimGroupForWarming(CacheEngine const* engine, std::string const& prefetchGroup)
    {
        // Bounded so a long-lived process cannot accumulate state for engines that no
        // longer exist. Group ids are few (one per build ref), so the cap is generous;
        // overflowing it merely re-warms a group, never returns a wrong value.
        constexpr std::size_t MaxTrackedEngines = 8;
        constexpr std::size_t MaxGroupsPerEngine = 256;

        static std::mutex warmedMutex;
        static std::map<CacheEngine const*, std::set<std::string, std::less<>>> warmedByEngine;

        std::scoped_lock const guard { warmedMutex };

        // A new engine means any previously tracked one is at best stale; keeping only
        // a bounded set of the most recent avoids unbounded growth without needing an
        // engine-destruction hook.
        if (!warmedByEngine.contains(engine) && warmedByEngine.size() >= MaxTrackedEngines)
            warmedByEngine.clear();

        auto& prefetchGroups = warmedByEngine[engine];
        if (prefetchGroups.size() >= MaxGroupsPerEngine)
            prefetchGroups.clear();
        return prefetchGroups.insert(prefetchGroup).second;
    }

    /// Warm the rest of `keyStr`'s prefetch group into L1. Called *after* the reply is
    /// sent, so the current fetch is never slowed. Warming is idempotent and cheap
    /// for already-warm keys; `primedGroups` avoids re-warming within a session.
    /// @param engine        Cache engine.
    /// @param manifest      Prefetch group manifest.
    /// @param keyStr        The key just served (the demand signal).
    /// @param primedGroups [in,out] Prefetch groups already warmed on this connection.
    void PrefetchGroup(CacheEngine* engine,
                       PrefetchGroupManifest* manifest,
                       std::string const& keyStr,
                       std::set<std::string, std::less<>>& primedGroups)
    {
        auto const now = engine->Clock().Now();
        auto const prefetchGroup = manifest->GroupOf(keyStr, now);
        if (!prefetchGroup.has_value() || !prefetchGroup->has_value() || primedGroups.contains(**prefetchGroup))
            return;

        primedGroups.insert(**prefetchGroup);
        // Cheap connection-local check first; the per-engine claim is what actually
        // bounds the work to one warm per prefetch group.
        if (!ClaimGroupForWarming(engine, **prefetchGroup))
            return;

        auto const members = manifest->Keys(**prefetchGroup, now);
        if (!members.has_value())
            return;
        for (auto const& member: *members)
            if (member != keyStr)
                (void) engine->Prefetch(member);
    }

    /// Handle one FETCH command: serve the canonical value verbatim, then warm the
    /// rest of its prefetch group.
    /// @param socket        Client socket.
    /// @param engine        Cache engine.
    /// @param manifest      Prefetch group manifest.
    /// @param primedGroups [in,out] Prefetch groups already warmed on this connection
    ///                      (pointer: a coroutine must not hold reference params).
    /// @param payload       The request payload, by value (see HandleStore).
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleFetch(ISocket* socket,
                                         CacheEngine* engine,
                                         PrefetchGroupManifest* manifest,
                                         std::set<std::string, std::less<>>* primedGroups,
                                         std::vector<std::byte> payload)
    {
        auto const key = Wire::DecodeFetchPayload(payload);
        if (!key.has_value())
            co_return co_await ReplyError(socket, Wire::ErrorCode::MalformedFrame, {}) ? Next::Continue : Next::Abort;

        auto const keyStr = BytesToString(*key);
        auto const got = engine->Get(keyStr);
        if (!got.has_value() || !got->found)
        {
            // A miss is a legitimate negative, distinct from a refusal: it carries
            // Status::Miss and an empty payload, so a client can tell "not cached"
            // from "your request was rejected" — which the pre-version format,
            // where both were the byte 0x00, could not express.
            co_return co_await Reply(socket, Wire::Status::Miss, {}) ? Next::Continue : Next::Abort;
        }

        // Serve the canonical value verbatim.
        auto const value = got->entry.ValueBytes();
        if (!co_await Reply(socket, Wire::Status::Ok, std::vector<std::byte> { value.begin(), value.end() }))
            co_return Next::Abort;

        // Leading-key group prefetch: this fetch is the demand signal that the
        // rest of the build group is about to be requested.
        PrefetchGroup(engine, manifest, keyStr, *primedGroups);
        co_return Next::Continue;
    }

} // namespace

Task<void> CompileCacheHandler::Run(ISocket* socket,
                                    CacheEngine* engine,
                                    std::vector<std::byte> primingBytes,
                                    SessionContext session)
{
    ByteReader reader { *socket, MaxLineBytes, session.maxPayloadBytes };
    reader.PrimeWith(primingBytes);

    PrefetchGroupManifest manifest { engine->Storage() };

    // Prefetch groups already prefetched on this connection, so a second fetch from the
    // same prefetch group does not re-warm the whole set.
    std::set<std::string, std::less<>> primedGroups;

    // The version the first command declared. Every later command on this
    // connection must match: a stream that changes version mid-flight is
    // nonsensical rather than merely unsupported, and rejecting it is cheaper
    // than carrying two decoders.
    std::optional<Wire::WireVersion> pinnedVersion;

    while (true)
    {
        auto const headerBytes = co_await reader.ReadExactly(Wire::RequestHeaderSize);
        if (!headerBytes.has_value())
        {
            // Routine at a command boundary — the launcher opens a fresh
            // connection per operation — so FrameDropSeverity logs a clean
            // disconnect at Debug and a genuine framing fault at Warn.
            session.LogFrameDrop(ProtocolLabel, headerBytes.error());
            co_return;
        }

        auto const header = Wire::DecodeRequestHeader(*headerBytes);
        if (!header.has_value())
        {
            // Wrong magic: the peer is not speaking this protocol at all, so
            // there is no framing in which a reply would be meaningful.
            session.LogFrameDrop(
                ProtocolLabel,
                ProtocolError { .code = ProtocolErrorCode::MalformedFrame,
                                .context = std::format("bad magic 0x{:02x}", static_cast<unsigned>((*headerBytes)[0])) });
            co_return;
        }

        if (!Wire::IsSupported(header->version) || (pinnedVersion.has_value() && *pinnedVersion != header->version))
        {
            // Name the range as well as the offence: a rejection that does not
            // say what would have worked cannot be acted on, and this is the one
            // message an operator with a mismatched install will ever see.
            auto message = std::format("unsupported wire version {}; this server speaks {}..{}",
                                       static_cast<unsigned>(header->version),
                                       static_cast<unsigned>(Wire::MinSupportedVersion),
                                       static_cast<unsigned>(Wire::CurrentVersion));
            session.LogFrameDrop(
                ProtocolLabel,
                ProtocolError { .code = ProtocolErrorCode::UnsupportedFeature, .context = std::string { message } });
            (void) co_await ReplyError(socket, Wire::ErrorCode::UnsupportedVersion, std::move(message));
            co_return;
        }
        pinnedVersion = header->version;

        if (header->payloadLength > session.maxPayloadBytes)
        {
            // Rejected on the declared length, before a single payload byte is
            // buffered. The pre-version format could only discover this
            // field-by-field, after the reader had already taken the memory.
            auto message =
                std::format("declared payload {} bytes exceeds cap {}", header->payloadLength, session.maxPayloadBytes);
            session.LogFrameDrop(
                ProtocolLabel,
                ProtocolError { .code = ProtocolErrorCode::PayloadTooLarge, .context = std::string { message } });

            // Step over the body the same way an unknown opcode's is stepped
            // over, so the sender's write completes and it can read the answer.
            // `Skip` discards in chunks and never materialises the frame, so the
            // memory the cap protects is still never taken -- draining costs
            // bandwidth the peer was going to spend anyway, not footprint.
            auto const drainable = static_cast<std::uint64_t>(session.maxPayloadBytes) * OversizeDrainFactor;
            bool drained = false;
            if (header->payloadLength <= drainable)
                drained = (co_await reader.Skip(header->payloadLength)).has_value();

            // Answer either way: even a frame too big to drain gets its reason,
            // on the chance the sender is not still writing and can read it.
            if (!co_await ReplyError(socket, Wire::ErrorCode::PayloadTooLarge, std::move(message)))
                co_return;
            if (!drained)
                co_return;
            continue;
        }

        auto const* descriptor = Wire::FindOp(header->opRaw);
        if (descriptor == nullptr)
        {
            // Recoverable, and deliberately so: the declared payload length lets
            // us step over a verb we do not know and keep the connection usable,
            // which is what allows a later version to add one without a flag day.
            if (!(co_await reader.Skip(header->payloadLength)).has_value())
                co_return;
            if (!co_await ReplyError(socket,
                                     Wire::ErrorCode::UnknownOpcode,
                                     std::format("unknown opcode 0x{:02x}", static_cast<unsigned>(header->opRaw))))
                co_return;
            continue;
        }

        auto payload = co_await reader.ReadExactly(header->payloadLength);
        if (!payload.has_value())
        {
            session.LogFrameDrop(ProtocolLabel, payload.error());
            co_return;
        }

        Next next = Next::Abort;
        switch (descriptor->code)
        {
            case Wire::Op::Store:
                next = co_await HandleStore(socket, engine, &manifest, std::move(*payload));
                break;
            case Wire::Op::Fetch:
                next = co_await HandleFetch(socket, engine, &manifest, &primedGroups, std::move(*payload));
                break;
        }

        if (next == Next::Abort)
            co_return;
    }
}

} // namespace FastCache
