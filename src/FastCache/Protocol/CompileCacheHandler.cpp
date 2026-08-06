// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CohortManifest.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>

#include <array>
#include <cstddef>
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

    /// Cap on a single framed line/field's length. The compile-cache protocol
    /// never uses line reads, but ByteReader requires a line cap; set it to the
    /// same generous bound the other handlers use.
    constexpr std::size_t MaxLineBytes = 65536;

    /// Read a big-endian u32 length prefix, then that many bytes, from the
    /// reader. Returns nullopt on any framing/read error (the caller aborts the
    /// connection — a compile-cache client is trusted infrastructure, not an
    /// arbitrary peer, so a malformed frame just ends the session).
    /// @param reader Source reader (pointer, not a reference: a coroutine must not
    ///               hold reference parameters across a suspend point).
    /// @return The bytes, or nullopt on error.
    [[nodiscard]] Task<std::optional<std::vector<std::byte>>> ReadLengthPrefixed(ByteReader* reader)
    {
        auto const lenBytes = co_await reader->ReadExactly(sizeof(std::uint32_t));
        if (!lenBytes.has_value())
            co_return std::nullopt;
        auto const len = ReadBigEndian<std::uint32_t>(*lenBytes);
        if (len == 0)
            co_return std::vector<std::byte> {};
        auto body = co_await reader->ReadExactly(len);
        if (!body.has_value())
            co_return std::nullopt;
        co_return std::move(*body);
    }

    /// Interpret a byte vector as a UTF-8/ASCII string_view (no copy).
    [[nodiscard]] std::string BytesToString(std::vector<std::byte> const& bytes)
    {
        return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    }

    /// Append a big-endian u32 to a byte buffer.
    void AppendU32(std::vector<std::byte>& out, std::uint32_t n)
    {
        std::array<std::byte, sizeof(std::uint32_t)> buf {};
        WriteBigEndian<std::uint32_t>(buf, n);
        out.insert(out.end(), buf.begin(), buf.end());
    }

    /// Write all bytes of `payload` to the socket.
    /// @return true on success, false on socket error.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> payload)
    {
        auto const r = co_await socket->Write(payload);
        co_return r.has_value();
    }

    /// Build the STORE error reply `[0x00][u32 msgLen][msg]`.
    [[nodiscard]] std::vector<std::byte> StoreError(std::string_view message)
    {
        std::vector<std::byte> out;
        out.push_back(static_cast<std::byte>(CompileCacheHandler::Status::Err));
        AppendU32(out, static_cast<std::uint32_t>(message.size()));
        auto const* p = reinterpret_cast<std::byte const*>(message.data());
        out.insert(out.end(), p, p + message.size());
        return out;
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

    /// Handle one STORE command: read its frames, canonicalize with the producer's
    /// layout, store the canonical value, and record cohort membership.
    /// @param socket   Client socket.
    /// @param engine   Cache engine.
    /// @param reader   Source reader.
    /// @param manifest Cohort manifest.
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleStore(ISocket* socket, CacheEngine* engine, ByteReader* reader, CohortManifest* manifest)
    {
        auto const key = co_await ReadLengthPrefixed(reader);
        auto const cohort = co_await ReadLengthPrefixed(reader);
        auto const srcRoot = co_await ReadLengthPrefixed(reader);
        auto const buildTree = co_await ReadLengthPrefixed(reader);
        auto const valueBytes = co_await ReadLengthPrefixed(reader);
        if (!key || !cohort || !srcRoot || !buildTree || !valueBytes)
            co_return Next::Abort;

        auto decoded = DecodeCompileValue(*valueBytes);
        if (!decoded.has_value())
            co_return co_await WriteAll(socket, StoreError("malformed compile-value frame")) ? Next::Continue : Next::Abort;

        PathCanon::Layout const producer { .sourceRoot = BytesToString(*srcRoot), .buildTree = BytesToString(*buildTree) };
        if (!CanonicalizeRegions(*decoded, producer))
            co_return co_await WriteAll(socket, StoreError("path canonicalization failed")) ? Next::Continue : Next::Abort;

        auto const canonicalBytes = EncodeCompileValue(*decoded);
        auto const keyStr = BytesToString(*key);
        auto const stored = engine->Set(keyStr, canonicalBytes, /*flags=*/0, /*exptime=*/0);
        if (!stored.has_value())
            co_return co_await WriteAll(socket, StoreError("storage write failed")) ? Next::Continue : Next::Abort;

        // Record cohort membership (best-effort: a manifest failure must not fail
        // the STORE — the value is already safely stored).
        if (!cohort->empty())
            (void) manifest->AddKey(BytesToString(*cohort), keyStr, engine->Clock().Now());

        std::array<std::byte, 1> const okReply { static_cast<std::byte>(CompileCacheHandler::Status::Ok) };
        co_return co_await WriteAll(socket, okReply) ? Next::Continue : Next::Abort;
    }

    /// Claim the right to warm `cohort`, once per cache engine.
    ///
    /// A per-connection set cannot bound this work: a compiler launcher opens a fresh
    /// connection per translation unit, so every one of a build's thousands of fetches
    /// arrived with an empty set and re-warmed the whole cohort — a measured 60-hit
    /// build issued 27022 prefetches and 13969 disk reads that way.
    ///
    /// The claim is therefore shared across connections, but keyed on the ENGINE
    /// rather than held in a plain function-local static: a static would outlive the
    /// engine it describes, so a second engine in the same process (a restart, or the
    /// next test case) would inherit "already warmed" for a cache that is in fact
    /// cold and would never prefetch. Entries are keyed by engine address and dropped
    /// when that engine is gone.
    /// @param engine The engine the cohort belongs to.
    /// @param cohort The cohort id to claim.
    /// @return True when this call claimed it (the caller should warm it).
    [[nodiscard]] bool ClaimCohortForWarming(CacheEngine const* engine, std::string const& cohort)
    {
        // Bounded so a long-lived process cannot accumulate state for engines that no
        // longer exist. Cohort ids are few (one per build ref), so the cap is generous;
        // overflowing it merely re-warms a cohort, never returns a wrong value.
        constexpr std::size_t MaxTrackedEngines = 8;
        constexpr std::size_t MaxCohortsPerEngine = 256;

        static std::mutex warmedMutex;
        static std::map<CacheEngine const*, std::set<std::string, std::less<>>> warmedByEngine;

        std::scoped_lock const guard { warmedMutex };

        // A new engine means any previously tracked one is at best stale; keeping only
        // a bounded set of the most recent avoids unbounded growth without needing an
        // engine-destruction hook.
        if (!warmedByEngine.contains(engine) && warmedByEngine.size() >= MaxTrackedEngines)
            warmedByEngine.clear();

        auto& cohorts = warmedByEngine[engine];
        if (cohorts.size() >= MaxCohortsPerEngine)
            cohorts.clear();
        return cohorts.insert(cohort).second;
    }

    /// Warm the rest of `keyStr`'s cohort into L1. Called *after* the reply is
    /// sent, so the current fetch is never slowed. Warming is idempotent and cheap
    /// for already-warm keys; `primedCohorts` avoids re-warming within a session.
    /// @param engine        Cache engine.
    /// @param manifest      Cohort manifest.
    /// @param keyStr        The key just served (the demand signal).
    /// @param primedCohorts [in,out] Cohorts already warmed on this connection.
    void PrefetchCohort(CacheEngine* engine,
                        CohortManifest* manifest,
                        std::string const& keyStr,
                        std::set<std::string, std::less<>>& primedCohorts)
    {
        auto const now = engine->Clock().Now();
        auto const cohort = manifest->CohortOf(keyStr, now);
        if (!cohort.has_value() || !cohort->has_value() || primedCohorts.contains(**cohort))
            return;

        primedCohorts.insert(**cohort);
        // Cheap connection-local check first; the per-engine claim is what actually
        // bounds the work to one warm per cohort.
        if (!ClaimCohortForWarming(engine, **cohort))
            return;

        auto const members = manifest->Keys(**cohort, now);
        if (!members.has_value())
            return;
        for (auto const& member: *members)
            if (member != keyStr)
                (void) engine->Prefetch(member);
    }

    /// Handle one FETCH command: serve the canonical value verbatim, then warm the
    /// rest of its cohort.
    /// @param socket        Client socket.
    /// @param engine        Cache engine.
    /// @param reader        Source reader.
    /// @param manifest      Cohort manifest.
    /// @param primedCohorts [in,out] Cohorts already warmed on this connection
    ///                      (pointer: a coroutine must not hold reference params).
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleFetch(ISocket* socket,
                                         CacheEngine* engine,
                                         ByteReader* reader,
                                         CohortManifest* manifest,
                                         std::set<std::string, std::less<>>* primedCohorts)
    {
        auto const key = co_await ReadLengthPrefixed(reader);
        if (!key)
            co_return Next::Abort;

        auto const keyStr = BytesToString(*key);
        auto const got = engine->Get(keyStr);
        if (!got.has_value() || !got->found)
        {
            std::array<std::byte, 1> const missReply { static_cast<std::byte>(CompileCacheHandler::Status::Err) };
            co_return co_await WriteAll(socket, missReply) ? Next::Continue : Next::Abort;
        }

        // Serve the canonical value verbatim: [0x01][u32 len][bytes].
        auto const value = got->entry.ValueBytes();
        std::vector<std::byte> reply;
        reply.push_back(static_cast<std::byte>(CompileCacheHandler::Status::Ok));
        AppendU32(reply, static_cast<std::uint32_t>(value.size()));
        reply.insert(reply.end(), value.begin(), value.end());
        if (!co_await WriteAll(socket, reply))
            co_return Next::Abort;

        // Leading-key cohort prefetch: this fetch is the demand signal that the
        // rest of the build cohort is about to be requested.
        PrefetchCohort(engine, manifest, keyStr, *primedCohorts);
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

    CohortManifest manifest { engine->Storage() };

    // Cohorts already prefetched on this connection, so a second fetch from the
    // same cohort does not re-warm the whole set.
    std::set<std::string, std::less<>> primedCohorts;

    while (true)
    {
        // Each command begins with [magic 0xFC][op]. Read the two header bytes.
        auto const header = co_await reader.ReadExactly(2);
        if (!header.has_value())
            co_return; // EOF or framing error — client disconnected.
        auto const magic = (*header)[0];
        auto const op = static_cast<Op>((*header)[1]);
        if (magic != CompileCacheMagic)
            co_return; // Not our protocol mid-stream — abort.

        Next next = Next::Abort;
        switch (op)
        {
            case Op::Store:
                next = co_await HandleStore(socket, engine, &reader, &manifest);
                break;
            case Op::Fetch:
                next = co_await HandleFetch(socket, engine, &reader, &manifest, &primedCohorts);
                break;
            default:
                co_return; // Unknown opcode — abort.
        }

        if (next == Next::Abort)
            co_return;
    }
}

} // namespace FastCache
