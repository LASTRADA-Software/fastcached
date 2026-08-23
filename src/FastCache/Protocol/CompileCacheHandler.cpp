// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/CompileCache/PrefetchGroupManifest.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
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

    /// Handle one AUTH command: verify the presented credential.
    ///
    /// Verification goes through `AuthPolicy`, whose comparison is constant-time,
    /// so the secret cannot be recovered a byte at a time from reply timing.
    ///
    /// A failed AUTH is answered and the connection **kept**, matching every other
    /// handler here: a refusal is a reply, not a close. That does leave a peer free
    /// to guess repeatedly on one connection, which is a rate-limiting concern
    /// rather than a framing one, and closing would not fix it — reconnecting costs
    /// an attacker nothing while costing every honest launcher its pipelining.
    ///
    /// @param socket   Client socket.
    /// @param policy   The policy resolved for this command, or null when auth is
    ///                 off. Passed in rather than re-resolved so the gate that let
    ///                 this frame through and the verify that answers it are the
    ///                 same policy even across a concurrent rotation.
    /// @param payload  The request payload, by value (see HandleStore).
    /// @param credentialAccepted [out] Set true only when a credential was actually
    ///                      VERIFIED. Never cleared: a later failed attempt must not
    ///                      revoke something the peer already proved.
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleAuth(ISocket* socket,
                                        std::shared_ptr<AuthPolicy const> policy,
                                        std::vector<std::byte> payload,
                                        bool* credentialAccepted)
    {
        auto const fields = Wire::DecodeAuthPayload(payload);
        if (!fields.has_value())
            co_return co_await ReplyError(socket, Wire::ErrorCode::MalformedFrame, {}) ? Next::Continue : Next::Abort;

        if (policy == nullptr || !policy->Enabled())
        {
            // Auth is off, so there is no credential to check and nothing to
            // refuse. Answering Ok rather than an error keeps a token-configured
            // launcher working against a server that does not require one — the
            // alternative would make enabling a token on the client a breaking
            // change against every unauthenticated daemon.
            //
            // `credentialAccepted` is deliberately NOT set: nothing was verified.
            // Setting it would mean a SIGHUP that later enables auth blesses this
            // connection on the strength of a check that never ran — the same hole
            // as seeding the flag from the policy, reached from the other side.
            // Nothing is lost, because while auth is off the gate never reads it.
            co_return co_await Reply(socket, Wire::Status::Ok, {}) ? Next::Continue : Next::Abort;
        }

        auto const username = Wire::AsStringView(fields->username);
        auto const secret = Wire::AsStringView(fields->secret);
        // An empty username asks to be checked against the secret alone (the redis
        // `requirepass` form), so a client configured with only a token is not
        // locked out of a server that also names a user.
        bool const ok = username.empty() ? policy->Verify(secret) : policy->Verify(username, secret);
        if (!ok)
            co_return co_await ReplyError(socket, Wire::ErrorCode::Unauthenticated, "authentication failed") ? Next::Continue
                                                                                                             : Next::Abort;

        *credentialAccepted = true;
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

    /// Answer one distributed-execution verb, or refuse it when this endpoint does
    /// not serve them.
    ///
    /// One function rather than four arms in the command loop, and not only for
    /// tidiness: the gate is the security-relevant decision here, and having it in
    /// exactly one place is what makes "can an unauthorized endpoint reach this?" a
    /// question with a single answer. A verb added to the table without a case here
    /// is refused rather than served, which is the direction a mistake has to fail
    /// in.
    /// @param socket Client socket.
    /// @param session The connection's session context.
    /// @param op The verb, already resolved against the table.
    /// @param payload The request payload, by value (see HandleStore).
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] Task<Next> HandleDistributed(ISocket* socket, Wire::Op op)
    {
        // Answered, never served. `fastcached` is a cache and nothing else: the fleet's
        // scheduler moved to `fastcache-compile-node --listen-scheduler`, because
        // handing out capacity is a decision only one node may make at a time and
        // nothing here can establish which node that is.
        //
        // These verbs keep their place in `OpTable` and keep getting a typed refusal
        // rather than being dropped, and both halves matter. A client built against an
        // older daemon must learn WHY its scheduling stopped working -- a closed
        // connection is indistinguishable from a dead host, and an unknown opcode
        // would say the daemon is too old when it is in fact too new. The message
        // names where the scheduler went, because a refusal that cannot say what
        // would have worked cannot be acted on.
        std::string const message = op == Wire::Op::Compile
                                 ? "this endpoint is a cache and does not execute compiles; send the job to the "
                                   "worker endpoint the lease named"
                                 : "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --listen-scheduler and point clients at it";
        co_return co_await ReplyError(socket, Wire::ErrorCode::DispatchNotPermitted, message) ? Next::Continue
                                                                                             : Next::Abort;
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

    // Whether this connection has presented a credential that was actually
    // VERIFIED. Deliberately not "is this connection allowed through": that is
    // derived per command from this plus the live policy, so the two questions
    // cannot drift.
    //
    // Seeding it from the policy instead — `authenticated = !authRequired` — is
    // the obvious spelling and it is wrong in both directions. A connection
    // opened while auth was off would stay exempt for its whole life across a
    // SIGHUP that turned auth ON, which is the hole a reload is meant to close;
    // and nothing would distinguish "auth is off" from "this peer proved
    // something", so enabling auth later would silently bless every open
    // connection. Recording only what was *proved* keeps the derivation honest.
    //
    // Rotation is the deliberate exception in the other direction: a peer that
    // authenticated stays authenticated when the secret changes under it, as
    // redis does. Re-gating on rotation would fail every in-flight build at the
    // moment an operator rotates a secret, and the peer did prove the credential
    // that was current when it connected.
    bool credentialAccepted = false;

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

        // Resolved once per command, and from the LIVE policy: a SIGHUP that turns
        // auth off releases open connections immediately, and one that turns it on
        // gates them. Held in a local for the duration of this command so a
        // concurrent rotation cannot make the gate and the verify disagree.
        auto const policy = session.CurrentAuth();
        bool const authRequired = policy != nullptr && policy->Enabled();

        // The gate runs BEFORE the payload is buffered, and drains rather than
        // reads. Gating after the read would let an unauthenticated peer pipeline
        // frames declaring `maxPayloadBytes` each (256 MiB by default) and make the
        // server allocate all of it per frame before being told to authenticate —
        // a memory-exhaustion hole opened by the very check meant to close a hole.
        // `Skip` discards in chunks and never materialises the frame, so refusing
        // costs bandwidth the peer was going to spend anyway, not footprint.
        // A verb the table bounds more tightly than the session does is checked
        // here, after the opcode is known. This is what keeps the gate below
        // meaningful: AUTH is deliberately reachable before authentication, so
        // without its own ceiling an unauthenticated peer could declare
        // `maxPayloadBytes` on opcode 0x03 and get exactly the allocation the gate
        // exists to deny — defeating it through the one door it holds open.
        //
        // Drained and answered rather than closed, like every other refusal here.
        if (auto const opCap = Wire::OpPayloadCap(header->opRaw, session.maxPayloadBytes); header->payloadLength > opCap)
        {
            auto message = std::format(
                "declared payload {} bytes exceeds the {} cap of {}", header->payloadLength, descriptor->name, opCap);
            session.LogFrameDrop(
                ProtocolLabel,
                ProtocolError { .code = ProtocolErrorCode::PayloadTooLarge, .context = std::string { message } });
            if (!(co_await reader.Skip(header->payloadLength)).has_value())
                co_return;
            if (!co_await ReplyError(socket, Wire::ErrorCode::PayloadTooLarge, std::move(message)))
                co_return;
            continue;
        }

        if (authRequired && !credentialAccepted && !Wire::IsPreAuthAllowed(header->opRaw))
        {
            if (!(co_await reader.Skip(header->payloadLength)).has_value())
                co_return;
            if (!co_await ReplyError(socket, Wire::ErrorCode::Unauthenticated, {}))
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
            case Wire::Op::Auth:
                next = co_await HandleAuth(socket, policy, std::move(*payload), &credentialAccepted);
                break;

            // Distributed execution, which this daemon does not do. The payload is
            // read and discarded rather than left on the socket: the refusal is a
            // reply, so the connection has to stay in sync for whatever the client
            // pipelined behind it.
            case Wire::Op::Register:
            case Wire::Op::Heartbeat:
            case Wire::Op::Lease:
            case Wire::Op::Compile:
                next = co_await HandleDistributed(socket, descriptor->code);
                break;
        }

        if (next == Next::Abort)
            co_return;
    }
}

} // namespace FastCache
