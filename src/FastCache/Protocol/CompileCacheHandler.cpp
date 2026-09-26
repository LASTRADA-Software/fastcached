// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PrefetchGroupManifest.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheAuth.hpp>
#include <FastCache/Protocol/CompileCacheHandler.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
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

#include <core/net/DeadlineTimer.hpp>

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

    /// Every verb that lives somewhere else now.
    ///
    /// A **table** rather than a conditional chain, and it earns that as soon as
    /// there are three answers: the scheduler moved to one binary, compiles to
    /// another, and a cluster exists in neither. A chain of ternaries is how the
    /// third answer comes to be the second one's, and a refusal that names the wrong
    /// destination is worse than one that names none.
    ///
    /// A cluster verb is refused with `NoCluster` rather than `DispatchNotPermitted`
    /// because those are different facts: one says this endpoint does not hand out
    /// capacity, the other that there is no replicated state here to read or change.
    /// A client told the first would go looking for a scheduler; told the second, it
    /// knows the question does not apply.
    constexpr std::array RelocatedVerbs {
        Wire::RefusedVerb { .op = Wire::Op::Register,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --serve-scheduler and point clients at it" },
        Wire::RefusedVerb { .op = Wire::Op::Heartbeat,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --serve-scheduler and point clients at it" },
        // `DispatchNotPermitted` and never `UnknownOpcode`, even though this verb is
        // newer than most clients: *unimplemented is not served elsewhere*. A worker
        // told `UnknownOpcode` here would conclude this daemon is too OLD to know the
        // verb, when the truth is that this endpoint is a cache and the scheduler is
        // somewhere else -- and it would then step over a refusal it should follow.
        Wire::RefusedVerb { .op = Wire::Op::Withdraw,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --serve-scheduler and point clients at it" },
        Wire::RefusedVerb { .op = Wire::Op::Lease,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --serve-scheduler and point clients at it" },
        // With the cordon row below, `Wire::NoCompileWorker`'s fact: a node started with
        // `--slots=0` refuses both verbs for the same reason and with the same code (#206).
        Wire::RefusedVerb { .op = Wire::Op::Compile,
                            .code = Wire::NoCompileWorker::Code,
                            .why = "this endpoint runs no compile worker: it is a cache and does not execute compiles, "
                                   "so send the job to the worker endpoint the lease named" },
        Wire::RefusedVerb { .op = Wire::Op::Release,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache and no longer schedules; run the fleet's scheduler with "
                                   "fastcache-compile-node --serve-scheduler and point clients at it" },
        Wire::RefusedVerb { .op = Wire::Op::ClusterStatus,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster; ask a "
                                   "fastcache-compile-node --serve-scheduler instead" },
        Wire::RefusedVerb { .op = Wire::Op::ClusterSet,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster; ask a "
                                   "fastcache-compile-node --serve-scheduler instead" },
        Wire::RefusedVerb { .op = Wire::Op::ClusterForget,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster; ask a "
                                   "fastcache-compile-node --serve-scheduler instead" },
        Wire::RefusedVerb { .op = Wire::Op::ClusterAdmit,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster; ask a "
                                   "fastcache-compile-node --serve-scheduler instead" },
        // With `ClusterAdmit` and for its reason: the same replicated change, recording
        // the other seat (#1449), so one endpoint must not refuse the two spellings with
        // two codes.
        Wire::RefusedVerb { .op = Wire::Op::ClusterAdmitLearner,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster; ask a "
                                   "fastcache-compile-node --serve-scheduler instead" },
        // **`DispatchNotPermitted` and never `UnknownOpcode`**, for the reason `Withdraw`
        // above states and which applies here with more force, these verbs being newer
        // than every deployed client: *unimplemented is not served elsewhere*. A client
        // told `UnknownOpcode` concludes this daemon is too OLD to know the verb -- when
        // the truth is that this endpoint is a cache and a compile node answers it -- and
        // it then STEPS OVER a refusal it should follow.
        //
        // Not `NoCluster`, which the four cluster rows take: that says *there is no
        // replicated state here*, and these verbs ask about a PROCESS rather than about a
        // cluster. A client told `NoCluster` would go looking for consensus it does not
        // need.
        Wire::RefusedVerb { .op = Wire::Op::NodeStatus,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache, not a compile node; ask a fastcache-compile-node "
                                   "what it is, or read this daemon's own /metrics" },
        Wire::RefusedVerb { .op = Wire::Op::NodeMetrics,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache, not a compile node; its counters are on the admin "
                                   "surface's /metrics, which needs no credential" },
        // With the node rows above and for their reason: admission is a property of the
        // PROCESS being asked -- which routes IT would admit a host through -- and a cache
        // folds no such routes. Not `NoCluster`: a client told that goes looking for
        // consensus, when the answer is that it asked the wrong binary.
        Wire::RefusedVerb { .op = Wire::Op::ExplainAdmission,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache, not a compile node: it folds no admission routes, so "
                                   "there is nothing here to explain; ask a fastcache-compile-node which routes "
                                   "admit a host" },
        // With the node rows and for their reason: a cordon asks about a worker
        // PROCESS, and this endpoint runs none. A client told this goes to the compile
        // node on its machine, which is where a cordon is answered.
        Wire::RefusedVerb { .op = Wire::Op::Cordon,
                            .code = Wire::NoCompileWorker::Code,
                            .why = "this endpoint runs no compile worker: it is a cache, so there is nothing here to "
                                   "cordon; ask the fastcache-compile-node on this machine" },
        // **`NoCluster`, with the cluster rows and NOT with the node rows
        // above.** `Enroll` is a self-service `ClusterAdmit` -- it asks to be written
        // into the replicated membership configuration, and `EnrollControl`'s `Approve`
        // performs exactly the `ClusterAdmit` an operator would have typed. Two verbs
        // that change the same replicated state must not be refused with two different
        // codes by one endpoint, or a client's remedy depends on which spelling it
        // happened to use. A joiner told `NoCluster` knows the question does not apply
        // here and goes looking for a node that runs consensus; told
        // `DispatchNotPermitted` it would conclude this endpoint merely declines to
        // schedule and keep asking.
        //
        // `Enroll` is PRE-AUTH, so this refusal is reachable by anybody who can route to
        // the port. That is deliberate and costs nothing: it names a role this process
        // already advertises by answering `0xFC` at all, and says no more than the
        // `NoCluster` a `ClusterStatus` has always drawn from the same door.
        Wire::RefusedVerb { .op = Wire::Op::Enroll,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster, so there is nothing here to "
                                   "join; ask a fastcache-compile-node that runs consensus instead" },
        Wire::RefusedVerb { .op = Wire::Op::EnrollControl,
                            .code = Wire::ErrorCode::NoCluster,
                            .why = "this endpoint is a cache and belongs to no cluster, so it opens no enrollment "
                                   "window; ask a fastcache-compile-node that runs consensus instead" },
        // `DispatchNotPermitted` with the node rows, for their reason: the fleet is served by a
        // compile node that schedules, not unimplemented, and a client told `UnknownOpcode` would
        // conclude this daemon is too old and step over a refusal it should act on.
        Wire::RefusedVerb { .op = Wire::Op::FleetText,
                            .code = Wire::ErrorCode::DispatchNotPermitted,
                            .why = "this endpoint is a cache, not a compile node, and serves no fleet; read it from "
                                   "the fleet's scheduler, a fastcache-compile-node --serve-scheduler" },
    };

    /// Whether every compile-family verb has a row here saying `Wire::NoCompileWorker`'s fact.
    ///
    /// A node running no worker answers the whole family with that fact, so neither
    /// endpoint can reword it or move its code without the other (#206). Walked over
    /// `OpTable`'s family column rather than naming the verbs, so a compile verb added
    /// there without a row here fails the build instead of reaching the generic refusal.
    /// @return True when every such verb has its row.
    [[nodiscard]] constexpr bool EveryCompileVerbSaysNoCompileWorker() noexcept
    {
        return std::ranges::all_of(Wire::OpTable, [](Wire::OpDescriptor const& verb) {
            if (verb.family != Wire::VerbFamily::Compile)
                return true;
            auto const* const row = Wire::FindRefusal(RelocatedVerbs, verb.code);
            return row != nullptr && Wire::SaysNoCompileWorker(*row);
        });
    }

    static_assert(EveryCompileVerbSaysNoCompileWorker());

    /// What to answer `op` with.
    ///
    /// A verb reaching here without a row is answered with the generic refusal
    /// rather than served, which is the direction a mistake has to fail in -- the
    /// same reason the command loop's `switch` carries no `default:`.
    /// @param op The verb.
    /// @return The refusal.
    [[nodiscard]] constexpr Wire::RefusedVerb RefusalFor(Wire::Op op) noexcept
    {
        if (auto const* const row = Wire::FindRefusal(RelocatedVerbs, op); row != nullptr)
            return *row;
        return Wire::RefusedVerb { .op = op, .code = Wire::ErrorCode::DispatchNotPermitted, .why = {} };
    }

    /// Interpret a byte span as a UTF-8/ASCII string (copying).
    [[nodiscard]] std::string BytesToString(std::span<std::byte const> bytes)
    {
        return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    }

    /// Write all bytes of `payload` to the socket.
    /// @return true on success, false on socket error.
    [[nodiscard]] core::async::Task<bool> WriteAll(core::net::ISocket* socket, std::span<std::byte const> payload)
    {
        auto const r = co_await socket->write(payload);
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
    [[nodiscard]] core::async::Task<bool> Reply(core::net::ISocket* socket,
                                                Wire::Status status,
                                                std::vector<std::byte> payload)
    {
        auto const frame = Wire::EncodeReply(status, payload);
        co_return co_await WriteAll(socket, frame);
    }

    /// Send one refusal, and record it.
    ///
    /// **Takes a ROW rather than a code, which is the whole of #494's daemon half.**
    /// This was `ReplyError(socket, code, message)` -- one private funnel that all
    /// twelve of this file's refusals passed through, so a counter here would have
    /// collapsed twelve refusals into one series. Three of the twelve answer
    /// `MalformedFrame`, which is exactly the case a table keyed on the CODE cannot
    /// hold. Pushing the row to the call site is the same argument that made `Refuse`
    /// take one, applied a level in.
    ///
    /// The sink is a pointer because `SessionContext::metrics` is one; the guard lives
    /// in `Cc::Refuse`'s own overload rather than here, so no call site in this file
    /// says anything about nullability.
    /// The row travels BY VALUE, not by reference. A coroutine copies its parameters
    /// into the frame and a reference parameter does not survive the first suspend
    /// point -- and every caller here passes a braced temporary, which is the shape
    /// that dangles. `SurfaceRefusal` is two enumerators, so there is nothing to save
    /// by borrowing. `cppcoreguidelines-avoid-reference-coroutine-parameters` says so
    /// too, and only under clang-tidy: gcc built this without a word.
    /// @param socket Client socket.
    /// @param metrics Where the refusal is recorded, or null when nothing collects.
    /// @param refusal Which refusal, as one row.
    /// @param message Detail; the code's default message is used when empty.
    /// @return true when the whole reply reached the socket.
    [[nodiscard]] core::async::Task<bool> ReplyRefused(core::net::ISocket* socket,
                                                       IMetricsSink* metrics,
                                                       Cc::SurfaceRefusal refusal,
                                                       std::string message)
    {
        auto const frame = Cc::Refuse(metrics, refusal, message);
        co_return co_await WriteAll(socket, frame);
    }

    /// What a pre-payload refusal answers with, and what it moves.
    ///
    /// A row per decision rather than one counter for whatever `ErrorCodeFor` returns:
    /// the two reachable outcomes are opposite operator problems -- a client sending
    /// more than it may, and a client that never learned it needs a credential -- and
    /// one series carrying both could answer neither.
    ///
    /// A switch rather than a table indexed by the enumerator, which is the shape
    /// `CompileResponder::RefusalFor` already chose for this same enum and for the
    /// reason recorded there: `PrePayloadDecision` states no `Last`, it is a wire enum
    /// both binaries compile in, and adding a count to a shared header to satisfy a
    /// local table idiom would be a wire change bought for nothing. `ErrorCodeFor`
    /// answers it the same way. No `default`, so a fifth outcome is a build failure --
    /// where an indexed array's size assertion would still read `4 == 4` and index one
    /// past the end.
    ///
    /// The CODE is not restated here. It comes from `ErrorCodeFor`, which already owns
    /// that mapping, so this answers only the half that is this surface's own.
    /// @param decision What the wire's own gate concluded.
    /// @return The counter a refusal for @p decision moves.
    [[nodiscard]] constexpr IMetricsSink::Counter CounterForDecision(Wire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case Wire::PrePayloadDecision::PayloadTooLarge:
                return IMetricsSink::Counter::CacheFramesRefusedPayloadTooLarge;
            case Wire::PrePayloadDecision::UnknownOpcode:
                // Refused earlier, where the opcode is first resolved, so the payload
                // can be skipped and the connection kept usable. Named anyway, so a
                // caller arriving here with it cannot invent a second series for one
                // event.
                return IMetricsSink::Counter::CacheFramesRefusedUnknownOpcode;
            case Wire::PrePayloadDecision::Unauthenticated:
            case Wire::PrePayloadDecision::Serve:
                // `Serve` is not a refusal, and a total function still has to answer
                // it. It follows `ErrorCodeFor`'s own choice rather than inventing a
                // second one.
                break;
        }
        return IMetricsSink::Counter::CacheFramesRefusedUnauthenticated;
    }

    /// Send one refusal that is deliberately not counted.
    /// @param socket Client socket.
    /// @param refusal Which refusal, and why nothing rises for it.
    /// @param message Detail; the code's default message is used when empty.
    /// @return true when the whole reply reached the socket.
    [[nodiscard]] core::async::Task<bool> ReplyUncounted(core::net::ISocket* socket,
                                                         Cc::UncountedRefusal refusal,
                                                         std::string message)
    {
        auto const frame = Cc::RefuseWithoutCounter(refusal, message);
        co_return co_await WriteAll(socket, frame);
    }

    /// What the caller's command loop should do after one command is handled.
    enum class Next : std::uint8_t
    {
        Continue, ///< Command complete; read the next one.
        Abort,    ///< Connection is finished (EOF, framing error, or write failure).
    };

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
    [[nodiscard]] core::async::Task<Next> HandleStore(core::net::ISocket* socket,
                                                      IMetricsSink* metrics,
                                                      CacheEngine* engine,
                                                      PrefetchGroupManifest* manifest,
                                                      std::vector<std::byte> payload)
    {
        auto const fields = Wire::DecodeStorePayload(payload);
        if (!fields.has_value())
            co_return co_await ReplyRefused(socket,
                                            metrics,
                                            { .code = Wire::ErrorCode::MalformedFrame,
                                              .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedPayload },
                                            {})
                ? Next::Continue
                : Next::Abort;

        // `CompileCache/CompileValue`'s recipe, not a copy here. Decoding, building
        // the layout from the two root fields, rewriting the regions and re-encoding
        // were spelled out in this function and nowhere else -- so when a compile node
        // became a second server for this wire it had none of them (#319). Sharing
        // only the rewrite would have left the ORDER here to be remembered twice.
        auto canonical =
            CanonicalStoredValue(fields->value, BytesToString(fields->srcRoot), BytesToString(fields->buildTree));
        switch (canonical.outcome)
        {
            case CanonicalizationOutcome::Canonicalized:
                break;
            case CanonicalizationOutcome::NotACompileValue:
                // This server speaks the whole protocol and says so; a node's cache
                // tier stores an opaque value verbatim instead. One policy each,
                // above the one they now share.
                co_return co_await ReplyRefused(socket,
                                                metrics,
                                                { .code = Wire::ErrorCode::MalformedValue,
                                                  .counter = IMetricsSink::Counter::CacheStoresRefusedNotACompileValue },
                                                {})
                    ? Next::Continue
                    : Next::Abort;
            case CanonicalizationOutcome::ForeignGeneration:
                // A value written under a canonicalization spec this build does not
                // implement. Refused rather than stored, because storing it would
                // mean storing text this server cannot rewrite -- the producing
                // checkout's absolute paths under a key every machine computes, which
                // is what `CanonicalStoredValue` exists to prevent (#483). Neither
                // server has a choice about this one.
                //
                // The MESSAGE names both generations, and it is the whole of the
                // operator's diagnostic here: a rolling upgrade otherwise presents as
                // stores that fail for no stated reason, and this reply is the only
                // place a client of the DAEMON can see that the fleet is merely mixed.
                //
                // The CODE is its own (#544). It was `MalformedValue`, which was wrong
                // in the way `.agent/rules/storage.md` records for `Corrupt` against
                // `UnsupportedFormatVersion` -- the obvious remedy for a cache
                // reported damaged is to wipe it, and a fleet mid-rollout is the one
                // state where that reading is both most likely and most expensive.
                //
                // COUNTED, and its own counter as well as its own code. The two are
                // separate facts and were separated in separate changes: #483 split
                // the counter while both arms still answered one code, because two
                // opposite events summed into one series are unreadable whatever the
                // wire says. This arm now differs from the one above in BOTH, which is
                // the shape `SurfaceRefusal` is for -- a row is the refusal, not the
                // code, so sharing one never implied sharing the other.
                //
                // #483 predicted this change would "turn these two arms into a table",
                // and it did not, so the prediction is answered rather than deleted.
                // Nothing is left to tabulate: the arms now share no field at all, the
                // code is already the single named enumerator both servers spell, and
                // the counter must stay per-surface. A table of two rows differing in
                // every column is the switch that is already here, spelled less
                // directly.
                co_return co_await ReplyRefused(socket,
                                                metrics,
                                                { .code = Wire::ErrorCode::ForeignValueGeneration,
                                                  .counter = IMetricsSink::Counter::CacheStoresRefusedForeignGeneration },
                                                ForeignGenerationMessage(canonical.generation))
                    ? Next::Continue
                    : Next::Abort;
        }
        auto const keyStr = BytesToString(fields->key);
        auto const groupStr = BytesToString(fields->prefetchGroup);
        // Moved, not copied: `CacheEngine::Set` takes the value BY VALUE and
        // `canonical` is dead after this call, so passing it by name spent a full
        // object-blob memcpy plus an allocation on every STORE.
        auto const stored = engine->Set(keyStr, std::move(canonical.bytes), /*flags=*/0, /*exptime=*/0);
        if (!stored.has_value())
            // The one arm of this surface that is about THIS MACHINE rather than
            // whoever is talking to it, and nothing counted it: the
            // `fastcached_write_errors_total` named in `CacheMalformedValues`'s own
            // history does not exist any more.
            co_return co_await ReplyRefused(
                socket,
                metrics,
                { .code = Wire::ErrorCode::StorageWriteFailed, .counter = IMetricsSink::Counter::CacheStoresFailed },
                {})
                ? Next::Continue
                : Next::Abort;

        // Record prefetch group membership (best-effort: a manifest failure must not fail
        // the STORE — the value is already safely stored).
        if (!groupStr.empty())
            (void) manifest->AddKey(groupStr, keyStr, engine->Clock().now());

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
    [[nodiscard]] core::async::Task<Next> HandleAuth(core::net::ISocket* socket,
                                                     IMetricsSink* metrics,
                                                     std::shared_ptr<AuthPolicy const> policy,
                                                     std::vector<std::byte> payload,
                                                     bool* credentialAccepted)
    {
        // The decision is shared with the compile node's frame server, which
        // terminates this verb too (#289); what stays here is how to ANSWER it.
        switch (CheckCredential(policy.get(), payload))
        {
            case CredentialOutcome::Malformed:
                // Its own counter, not the ordinary malformed-payload one: garbage
                // aimed at the credential verb is what a scanner produces, and summing
                // it with client defects buries the series somebody is watching.
                co_return co_await ReplyRefused(socket,
                                                metrics,
                                                { .code = Wire::ErrorCode::MalformedFrame,
                                                  .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedCredential },
                                                {})
                    ? Next::Continue
                    : Next::Abort;

            case CredentialOutcome::NoPolicy:
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

            case CredentialOutcome::Rejected:
                co_return co_await ReplyRefused(
                    socket,
                    metrics,
                    { .code = Wire::ErrorCode::Unauthenticated, .counter = IMetricsSink::Counter::CacheCredentialsRejected },
                    "authentication failed")
                    ? Next::Continue
                    : Next::Abort;

            case CredentialOutcome::Accepted:
                break;
        }

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
        auto const now = engine->Clock().now();
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
    [[nodiscard]] core::async::Task<Next> HandleFetch(core::net::ISocket* socket,
                                                      IMetricsSink* metrics,
                                                      CacheEngine* engine,
                                                      PrefetchGroupManifest* manifest,
                                                      std::set<std::string, std::less<>>* primedGroups,
                                                      std::vector<std::byte> payload)
    {
        auto const key = Wire::DecodeFetchPayload(payload);
        if (!key.has_value())
            co_return co_await ReplyRefused(socket,
                                            metrics,
                                            { .code = Wire::ErrorCode::MalformedFrame,
                                              .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedPayload },
                                            {})
                ? Next::Continue
                : Next::Abort;

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

    /// Handle one CACHE-DROP command: remove one key from this daemon's store.
    ///
    /// **Served, not refused.** `UnimplementedVerb` would tell a client this daemon is too
    /// old to know the verb, and `DispatchNotPermitted` that another endpoint answers it;
    /// both are false of a daemon that holds the key. The credential gate has already run:
    /// `DecidePrePayload` asked for authentication exactly as it does before a `STORE`,
    /// which is the gate a write gets here.
    ///
    /// Through the ENGINE, and so through `NotifyingStorage`: a Redis `WATCH` on the key is
    /// dirtied exactly as a memcached `delete` dirties it. No keyspace event is published,
    /// for the same reason a memcached `delete` and a `STORE` publish none -- the event
    /// names belong to the Redis verbs that own them
    /// (`docs/operations/known-limitations.md`).
    ///
    /// The prefetch-group manifest is left naming the key. Warming a group calls
    /// `Prefetch`, which pulls a key into L1 only while L2 still holds it, so a dropped
    /// member is a miss rather than something a warm can bring back.
    /// @param socket   Client socket.
    /// @param metrics  Where a malformed payload is counted.
    /// @param engine   Cache engine.
    /// @param payload  The request payload, by value (see HandleStore).
    /// @return Whether the command loop should continue or abort.
    [[nodiscard]] core::async::Task<Next> HandleCacheDrop(core::net::ISocket* socket,
                                                          IMetricsSink* metrics,
                                                          CacheEngine* engine,
                                                          std::vector<std::byte> payload)
    {
        auto const key = Wire::DecodeCacheDropPayload(payload);
        if (!key.has_value())
            co_return co_await ReplyRefused(socket,
                                            metrics,
                                            { .code = Wire::ErrorCode::MalformedFrame,
                                              .counter = IMetricsSink::Counter::CacheFramesRefusedMalformedPayload },
                                            {})
                ? Next::Continue
                : Next::Abort;

        auto const removed = engine->Delete(BytesToString(*key));
        if (removed.has_value())
            co_return co_await Reply(socket, Wire::Status::Ok, {}) ? Next::Continue : Next::Abort;
        if (removed.error().code == StorageErrorCode::KeyNotFound)
            // `Miss`, never `Error`: see `Op::CacheDrop`.
            co_return co_await Reply(socket, Wire::Status::Miss, {}) ? Next::Continue : Next::Abort;

        co_return co_await ReplyUncounted(
            socket,
            { .code = Wire::ErrorCode::StorageWriteFailed,
              .rationale = "WriteErrorReportingStorage reports a removal it could not persist as "
                           "fastcached_write_errors_total, at the removal, where every protocol's delete is visible" },
            {})
            ? Next::Continue
            : Next::Abort;
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
    [[nodiscard]] core::async::Task<Next> HandleDistributed(core::net::ISocket* socket, Wire::Op op)
    {
        // Answered, never served. `fastcached` is a cache and nothing else: the fleet's
        // scheduler moved to `fastcache-compile-node --serve-scheduler`, because
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
        // **Uncounted, and for the reason the scheduler's equivalent arm is:** the
        // code comes from the matched row, so a counter beside it would have to be
        // built from a table that has no counter column. Unlike the scheduler's, this
        // table is NOT empty -- every scheduling verb lands here -- so the second
        // clause that arm records does not apply and is deliberately not claimed.
        auto const refusal = RefusalFor(op);
        co_return co_await ReplyUncounted(
            socket,
            { .code = refusal.code,
              .rationale = "the code comes from the matched RefusalFor row, which carries no counter column; a rise "
                           "would name the verb rather than the surface, and the verb is already in the message" },
            std::string { refusal.why })
            ? Next::Continue
            : Next::Abort;
    }

    /// What a subscriber's read watch learned while its stream ran.
    ///
    /// Plain members: the watcher and the handler share the connection's one reactor thread. Shared
    /// for LIFETIME, because the watcher can still be parked when the handler moves on.
    struct SubscriberWatch
    {
        bool finished { false }; ///< The wait has resolved and given the read slot back.
        bool gone { false };     ///< EOF or a reset.
        bool abortive { false }; ///< That departure was a reset.

        /// The handler retired this watch; what the retirement delivers is not the peer's doing.
        bool retired { false };
    };

    /// Watch a subscriber's read side, once.
    ///
    /// **It reads and never writes**, so the handler stays the only writer on the connection.
    /// One wake, then done: a subscriber that sent bytes has asked for something else, and one
    /// whose wait answered EOF or an error has left.
    /// @param socket The connection; the handler retires this wait with `CancelRead` before it
    ///        reads the socket again or returns. The retired wait resumes in the loop's next
    ///        drain (core-cpp 0.2.1, G2), possibly after the socket is gone, which is why
    ///        nothing below the `co_await` touches `socket`.
    /// @param watch Where the answer is left.
    core::async::DetachedTask WatchSubscriber(core::net::ISocket* socket, std::shared_ptr<SubscriberWatch> watch)
    {
        auto const readable = co_await socket->waitReadable();
        if (!watch->retired)
        {
            watch->gone = !readable.has_value() || *readable == 0;
            watch->abortive = !readable.has_value();
        }
        watch->finished = true;
    }

    /// What a push's hold closed, if it expired.
    struct PushHold
    {
        core::net::ISocket* socket { nullptr }; ///< The connection to close.
        bool expired { false };                 ///< Whether the hold ran out and closed it.
    };

    /// End a connection whose push stayed parked past its hold.
    ///
    /// The close is the only thing that retrieves a parked write, which resumes with a failure;
    /// the flag is what tells that failure apart from the peer resetting.
    /// @param state The push's `PushHold`.
    void ExpirePushHold(void* state)
    {
        auto* const hold = static_cast<PushHold*>(state);
        hold->expired = true;
        hold->socket->close();
    }

    /// The daemon's half of a stream: every push written here, bounded by a timer of its own.
    ///
    /// A timer per push rather than a sweeper, because this handler has none: the daemon's
    /// connections are bounded by the peer and by `--max-connections`, not by a deadline table.
    class DaemonPushSink final: public IPushSink
    {
      public:
        /// @param socket The connection.
        /// @param reactor The connection's reactor, which runs the hold.
        /// @param watch The read watch armed for the stream.
        DaemonPushSink(core::net::ISocket* socket,
                       core::net::EventLoop* reactor,
                       std::shared_ptr<SubscriberWatch const> watch) noexcept:
            _socket { socket },
            _reactor { reactor },
            _watch { std::move(watch) }
        {
        }

        /// @copydoc IPushSink::Push
        [[nodiscard]] core::async::Task<PushOutcome> Push(std::vector<std::byte> frame,
                                                          std::chrono::milliseconds hold) override
        {
            PushHold state { .socket = _socket, .expired = false };
            core::net::DeadlineTimer const timer { *_reactor, _reactor->clock().now() + hold, &ExpirePushHold, &state };
            if (co_await WriteAll(_socket, frame))
                co_return PushOutcome::Delivered;
            co_return state.expired ? PushOutcome::Stalled : PushOutcome::Lost;
        }

        /// @copydoc IPushSink::Activity
        [[nodiscard]] PeerActivity Activity() const noexcept override
        {
            if (!_watch->finished)
                return PeerActivity::Quiet;
            if (!_watch->gone)
                return PeerActivity::Sent;
            return _watch->abortive ? PeerActivity::Reset : PeerActivity::Departed;
        }

        /// @copydoc IPushSink::Stopping
        ///
        /// **Never**, and that is not an omission: the daemon stops by stopping its reactors,
        /// which free a stream parked on its timer without resuming it. Nothing is counted and
        /// nothing is written, which is the right answer for a process going away.
        [[nodiscard]] bool Stopping() const noexcept override
        {
            return false;
        }

      private:
        core::net::ISocket* _socket;
        core::net::EventLoop* _reactor;
        std::shared_ptr<SubscriberWatch const> _watch;
    };

    /// A subject this daemon does not stream: the node and the fleet are a compile node's.
    constexpr Cc::UncountedRefusal SubjectServedElsewhere {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a misdirection a healthy fleet produces whenever a dashboard is pointed at the cache daemon, "
                     "answered with where to go instead; DispatchNotPermitted and not UnimplementedVerb because a "
                     "compile node serves the subject, so it is served elsewhere rather than unimplemented",
    };

    /// A stream whose connection no longer passes the credential the live policy now requires.
    constexpr Cc::SurfaceRefusal Revoked { .code = Wire::ErrorCode::Unauthenticated,
                                           .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked };

    /// Who the daemon streams to.
    ///
    /// **The credential is decided before the payload is read** -- `SUBSCRIBE` is `RequiresAuth`
    /// in `OpTable`, so a policy that asks for one has already been answered by
    /// `DecidePrePayload` -- and **re-asked every tick**, on the rotation exception's terms: a
    /// connection that proved a secret keeps its stream when the secret rotates, and one that never
    /// presented any loses it the tick the policy starts requiring one. The same two directions
    /// the command loop's own gate takes, per command.
    class DaemonLiveGate final: public ILiveGate
    {
      public:
        /// @param session The connection's session: the live policy and the sink.
        /// @param credentialAccepted Whether this connection proved a credential.
        DaemonLiveGate(SessionContext const* session, bool const* credentialAccepted) noexcept:
            _session { session },
            _credentialAccepted { credentialAccepted }
        {
        }

        /// @copydoc ILiveGate::RefuseWatcher
        [[nodiscard]] std::optional<std::vector<std::byte>> RefuseWatcher(LiveWatcher const& /*watcher*/) const override
        {
            return std::nullopt;
        }

        /// @copydoc ILiveGate::Admit
        [[nodiscard]] std::optional<std::vector<std::byte>> Admit(Wire::SubscribeRequest const& request,
                                                                  LiveWatcher const& /*watcher*/) const override
        {
            if (request.subject == Wire::LiveSubject::Cache)
                return std::nullopt;
            return Cc::RefuseWithoutCounter(
                SubjectServedElsewhere,
                "this daemon streams the cache subject only; subscribe to a node or the fleet at a fastcache-compile-node");
        }

        /// @copydoc ILiveGate::Recheck
        // The watcher is unnamed because this daemon decides on its own credential and never
        // on who is asking: it runs no node handshake, so `proven` is honestly disengaged on
        // every connection it will ever see, and reading it would be reading a constant.
        [[nodiscard]] std::optional<std::vector<std::byte>> Recheck(Wire::LiveSubject /*subject*/,
                                                                    LiveWatcher const& /*watcher*/) const override
        {
            auto const policy = _session->CurrentAuth();
            if (policy == nullptr || !policy->Enabled() || *_credentialAccepted)
                return std::nullopt;
            return Cc::Refuse(
                _session->metrics, Revoked, "this daemon now requires a credential this connection never presented");
        }

      private:
        SessionContext const* _session;
        bool const* _credentialAccepted;
    };

    /// The connection state a subscription borrows from the command loop.
    struct SubscribeContext
    {
        core::net::ISocket* socket { nullptr };     ///< The connection.
        ByteReader const* reader { nullptr };       ///< Its reader, for what it has already buffered.
        SessionContext const* session { nullptr };  ///< Its session.
        bool const* credentialAccepted { nullptr }; ///< Whether it proved a credential.
    };

    /// A daemon that streams no live stats, which a `SUBSCRIBE` is told by name.
    constexpr Cc::UncountedRefusal NoLiveStats {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a process built or run without live stats answers every subscription this way, which is its "
                     "configuration rather than an event; the daemon body always wires one, so this is a harness's answer",
    };

    /// Serve one `SUBSCRIBE` on this connection until the stream ends.
    ///
    /// **The handler stays the one writer**: the stream pushes through `DaemonPushSink`, and the
    /// terminal reply is written here once it returns. The read watch is retired with
    /// `CancelRead` before this returns, which is what lets the command loop read the socket
    /// again -- or return, and have `Connection` close it -- with nothing parked on its read slot.
    /// @param context The connection state borrowed from the loop.
    /// @param frame The whole request, header included.
    /// @return Whether the command loop continues.
    [[nodiscard]] core::async::Task<Next> HandleSubscribe(SubscribeContext context, std::vector<std::byte> frame)
    {
        auto* const socket = context.socket;
        auto const* const session = context.session;
        if (session->liveStats == nullptr || session->reactor == nullptr)
            co_return co_await ReplyUncounted(
                socket, NoLiveStats, "this daemon streams no live stats; read /metrics on its admin surface")
                ? Next::Continue
                : Next::Abort;

        // A peer that already pipelined bytes has asked for something else before the stream
        // began, so there is nothing to watch for: the stream ends at once, in order.
        auto watch = std::make_shared<SubscriberWatch>();
        if (!context.reader->Buffered().empty())
            watch->finished = true;
        else
            WatchSubscriber(socket, watch);

        DaemonPushSink sink { socket, session->reactor, watch };
        DaemonLiveGate const gate { session, context.credentialAccepted };
        // `proven` is left disengaged and that is the truth rather than a default: this daemon
        // runs no node handshake, so no connection it serves can ever have proved an identity.
        auto const terminal = co_await session->liveStats->Serve(
            frame, LiveWatcher { .host = socket->peerAddress() }, &sink, &gate, session->reactor);

        if (!watch->finished)
        {
            watch->retired = true;
            socket->cancelRead();
        }
        if (terminal.empty())
            co_return Next::Abort;
        co_return co_await WriteAll(socket, terminal) ? Next::Continue : Next::Abort;
    }

} // namespace

core::async::Task<void> CompileCacheHandler::Run(core::net::ISocket* socket,
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
        // Between commands, not after one: `Compact()` releases nothing, so the buffer
        // a 256 MiB STORE grew into would be held for the rest of a connection that no
        // longer ends after one operation. `--max-connections` times that is a ceiling
        // nobody chose. Whatever a pipelined peer has already sent is kept.
        reader.ReleaseSpareCapacity();

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
            (void) co_await ReplyRefused(socket,
                                         session.metrics,
                                         { .code = Wire::ErrorCode::UnsupportedVersion,
                                           .counter = IMetricsSink::Counter::CacheFramesRefusedUnsupportedVersion },
                                         std::move(message));
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
            auto const drainable = static_cast<std::uint64_t>(session.maxPayloadBytes) * Wire::OversizeDrainFactor;
            bool drained = false;
            if (header->payloadLength <= drainable)
                drained = (co_await reader.Skip(header->payloadLength)).has_value();

            // Answer either way: even a frame too big to drain gets its reason,
            // on the chance the sender is not still writing and can read it.
            if (!co_await ReplyRefused(socket,
                                       session.metrics,
                                       { .code = Wire::ErrorCode::PayloadTooLarge,
                                         .counter = IMetricsSink::Counter::CacheFramesRefusedPayloadTooLarge },
                                       std::move(message)))
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
            if (!co_await ReplyRefused(socket,
                                       session.metrics,
                                       { .code = Wire::ErrorCode::UnknownOpcode,
                                         .counter = IMetricsSink::Counter::CacheFramesRefusedUnknownOpcode },
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
        // Both pre-payload refusals, from one predicate. The ceiling and the
        // credential were two hand-written checks here and nowhere else; the compile
        // node's frame server has the same loop and needed the same rule, so the rule
        // moved into the header both surfaces already share rather than being written
        // a second time (#289). `DecidePrePayload` also fixes the ORDER -- bound
        // first, then gate -- which is what stops a peer declaring the session cap on
        // the one verb the gate deliberately holds open.
        auto const decision = Wire::DecidePrePayload({ .opRaw = header->opRaw,
                                                       .declaredLength = header->payloadLength,
                                                       .sessionCap = session.maxPayloadBytes,
                                                       .authRequired = authRequired,
                                                       .credentialAccepted = credentialAccepted });
        if (decision != Wire::PrePayloadDecision::Serve)
        {
            // Only the size refusal says more than its code does. The credential
            // refusal deliberately carries no detail: what it would have to say is
            // which verb the peer failed to reach, and an unauthenticated caller
            // learns nothing here it did not already know.
            auto message = decision == Wire::PrePayloadDecision::PayloadTooLarge
                               ? std::format("declared payload {} bytes exceeds the {} cap of {}",
                                             header->payloadLength,
                                             descriptor->name,
                                             Wire::OpPayloadCap(header->opRaw, session.maxPayloadBytes))
                               : std::string {};
            if (decision == Wire::PrePayloadDecision::PayloadTooLarge)
                session.LogFrameDrop(ProtocolLabel,
                                     ProtocolError { .code = ProtocolErrorCode::PayloadTooLarge, .context = message });

            // Drained before answering, which is this surface's convention for every
            // refusal in this loop. Unchanged from the two branches this replaced.
            if (!(co_await reader.Skip(header->payloadLength)).has_value())
                co_return;
            if (!co_await ReplyRefused(socket,
                                       session.metrics,
                                       { .code = Wire::ErrorCodeFor(decision), .counter = CounterForDecision(decision) },
                                       std::move(message)))
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
                next = co_await HandleStore(socket, session.metrics, engine, &manifest, std::move(*payload));
                break;
            case Wire::Op::Fetch:
                next = co_await HandleFetch(socket, session.metrics, engine, &manifest, &primedGroups, std::move(*payload));
                break;
            case Wire::Op::CacheDrop:
                next = co_await HandleCacheDrop(socket, session.metrics, engine, std::move(*payload));
                break;
            case Wire::Op::Auth:
                next = co_await HandleAuth(socket, session.metrics, policy, std::move(*payload), &credentialAccepted);
                break;

            // Distributed execution and cluster administration, neither of which
            // this daemon does. The payload is read and discarded rather than left
            // on the socket: the refusal is a reply, so the connection has to stay
            // in sync for whatever the client pipelined behind it.
            case Wire::Op::Register:
            case Wire::Op::NodeAnnounce:
            case Wire::Op::Heartbeat:
            case Wire::Op::Withdraw:
            case Wire::Op::Lease:
            case Wire::Op::Compile:
            case Wire::Op::Release:
            case Wire::Op::ClusterStatus:
            case Wire::Op::ClusterSet:
            case Wire::Op::ClusterForget:
            case Wire::Op::ClusterAdmitClient:
            case Wire::Op::ClusterForgetClient:
            case Wire::Op::ClusterAdmit:
            case Wire::Op::ClusterAdmitLearner:
            case Wire::Op::ClusterAdmitWorker:
            // The operator verbs, answered by a compile node and refused HERE by name.
            // Sharing the arm above is right rather than convenient: `HandleDistributed`
            // is the one door to `RefusalFor`, which is the table that says which code
            // and which sentence -- a second arm would be a second place to decide it.
            //
            // A missing arm here does NOT fail the build on MSVC (C4062 is off by
            // default), and what it produces is a DROPPED FRAME rather than a refusal:
            // the client waits, times out, and reports a dead endpoint for a daemon that
            // is working perfectly. `Every op in the table is dispatched` is what catches
            // it, and it caught exactly this.
            case Wire::Op::NodeStatus:
            case Wire::Op::NodeMetrics:
            case Wire::Op::ExplainAdmission:
            case Wire::Op::Cordon:
            // The enrollment pair, answered by a compile node that runs consensus. Same
            // arm for the same reason: `RefusalFor` is the one place the code and the
            // sentence are decided.
            case Wire::Op::Enroll:
            case Wire::Op::EnrollControl:
            // The fleet document, answered by the fleet's scheduler; same arm, same reason.
            case Wire::Op::FleetText:
            // The node proof (#1428, #178), answered by a compile node that runs consensus and so
            // holds the roster a proven identity is judged against. This daemon holds none, so
            // both verbs take the same arm for the same reason as the enrollment pair:
            // `RefusalFor` is the one place the code and the sentence are decided.
            case Wire::Op::NodeChallenge:
            case Wire::Op::ProveNode:
                next = co_await HandleDistributed(socket, descriptor->code);
                break;

            // Live stats: the cache subject, streamed. The payload is the request, so the whole
            // frame is handed on rather than re-framed.
            case Wire::Op::Subscribe: {
                std::vector<std::byte> frame { headerBytes->begin(), headerBytes->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());
                next = co_await HandleSubscribe(SubscribeContext { .socket = socket,
                                                                   .reader = &reader,
                                                                   .session = &session,
                                                                   .credentialAccepted = &credentialAccepted },
                                                std::move(frame));
                break;
            }
        }

        if (next == Next::Abort)
            co_return;
    }
}

} // namespace FastCache
