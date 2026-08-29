// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"

#include <FastCache/Core/Compression.hpp>

#include <format>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The codec ids this build can actually produce and consume.
    ///
    /// Derived from `Core/Compression`, so a build configured without compression
    /// offers only `Identity` and still interoperates -- the negotiation falls back
    /// to it rather than refusing, because a build must never lose distribution
    /// because two machines were configured differently.
    [[nodiscard]] Wire::CodecList AvailableCodecs()
    {
        Wire::CodecList out;
        for (auto const codec: { CompressionCodec::Zstd, CompressionCodec::Lz4 })
            if (Compression::IsAvailable(codec))
                out.push_back(static_cast<std::uint8_t>(codec));
        out.push_back(Wire::IdentityCodec); // always, and always last
        return out;
    }

    /// The final component of a path, in either separator style.
    ///
    /// The worker is told what to CALL its scratch file, not where the client keeps
    /// its sources: a compiler records the name it was handed (clang-cl and gcc in
    /// the `.file` symbol, MSVC in its compiland record), so matching the base name
    /// is what makes a dispatched object byte-identical to a locally compiled one --
    /// measured at seven bytes' difference on clang-cl before this, and none after.
    /// The directory buys none of that and would tell a worker where a client's
    /// checkout lives.
    /// @param path The source path as the build system spelled it.
    /// @return Its final component.
    [[nodiscard]] std::string_view BaseName(std::string_view path)
    {
        auto const slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    /// Wrap `payload` in a codec envelope, compressing when it is worth it.
    ///
    /// Falls back to `Identity` whenever compression did not actually shrink the
    /// payload, which is the same shrink-check `Core/Compression` applies to stored
    /// values: an incompressible object should not pay a decompress on the way out.
    /// @param payload The bytes to send.
    /// @param peerCodecs What the receiving end can decode.
    /// @return The framed envelope.
    [[nodiscard]] std::vector<std::byte> Envelope(std::string_view payload, Wire::CodecList const& peerCodecs)
    {
        auto const raw = Wire::AsBytes(payload);
        auto const chosen = Wire::ChooseCodec(peerCodecs, AvailableCodecs());
        if (chosen != Wire::IdentityCodec)
        {
            auto compressed = Compression::Compress(static_cast<CompressionCodec>(chosen), raw, /*level=*/1);
            if (compressed.size() < raw.size())
                return Wire::EncodeCodecEnvelope(chosen, static_cast<std::uint32_t>(raw.size()), compressed);
        }
        return Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(raw.size()), raw);
    }

    /// Undo a codec envelope.
    /// @param field The enveloped field.
    /// @return The original bytes, or nullopt when the envelope is malformed or its
    ///         codec is one this build cannot decode.
    [[nodiscard]] std::optional<std::vector<std::byte>> Unenvelope(std::span<std::byte const> field)
    {
        auto const envelope = Wire::DecodeCodecEnvelope(field);
        if (!envelope.has_value())
            return std::nullopt;
        if (envelope->codec == Wire::IdentityCodec)
            return std::vector<std::byte> { envelope->bytes.begin(), envelope->bytes.end() };

        auto const codec = static_cast<CompressionCodec>(envelope->codec);
        if (!Compression::IsAvailable(codec))
            return std::nullopt;
        auto decoded = Compression::Decompress(codec, envelope->bytes, envelope->rawLength);
        if (!decoded.has_value())
            return std::nullopt;
        return std::move(*decoded);
    }

    /// Build a `DispatchResult` that carries only a reason.
    [[nodiscard]] DispatchResult Refused(DispatchStatus status, std::string detail)
    {
        return DispatchResult { .status = status,
                                .exitCode = 0,
                                .object = {},
                                .stdoutText = {},
                                .stderrText = {},
                                .detail = std::move(detail),
                                .workerEndpoint = {} };
    }

    /// Encode the argument list as one length-prefixed field per argument.
    [[nodiscard]] std::vector<std::byte> EncodeArgs(std::span<std::string const> args)
    {
        // One field per argument, not a joined string: an argument may contain a
        // space, and a receiver splitting on whitespace would turn `-DMSG=hello world`
        // into two flags. The wire already has a framing for lists; use it.
        std::vector<std::byte> out;
        for (auto const& arg: args)
        {
            auto const bytes = Wire::AsBytes(arg);
            auto header = std::vector<std::byte>(sizeof(std::uint32_t));
            WriteBigEndian<std::uint32_t>(header, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), header.begin(), header.end());
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        return out;
    }

    /// Everything the compile half of a dispatch needs, once a lease exists.
    ///
    /// A struct rather than six parameters, three of which are string-ish and would
    /// be transposable at the one call site there is.
    struct LeasedJob
    {
        std::string_view endpoint;       ///< The worker the scheduler named.
        std::string_view leaseToken;     ///< What authorizes the job there.
        Wire::CodecList const& codecs;   ///< What that worker can decode.
        Wire::CodecList const& accepted; ///< What this client can decode.
        Credential const& credential;    ///< Presented to the worker.
        DispatchRequest const& request;  ///< The job itself.
        ExchangeBudget budget;           ///< How long the compile may take.
    };

    /// Send one preprocessed translation unit to the worker a lease named.
    ///
    /// Split out of `Dispatch` so the lease has exactly ONE place to be resolved.
    /// Every branch below is a way a job can end, and a release written per branch
    /// is a release somebody forgets on the branch added next -- which is how the
    /// key this lease pins comes to be pinned for the full lease timeout.
    /// @param exchange How to reach the worker.
    /// @param job The lease and what to compile under it.
    /// @return What happened, as `Dispatch` will return it.
    [[nodiscard]] DispatchResult CompileOnWorker(IEndpointExchange& exchange, LeasedJob const& job)
    {
        auto const argsField = EncodeArgs(job.request.args);
        // Compressed against the WORKER's codecs, which the grant relayed -- not
        // against this client's. The two need not agree, and guessing wrong would
        // only be discovered after the whole preprocessed payload had crossed the
        // network.
        auto const sourceField = Envelope(job.request.preprocessed, job.codecs);

        // Not `const`: the frame carries a whole preprocessed translation unit, so it
        // is MOVED into the exchange rather than copied on the hot path of a build.
        auto compileFrame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = job.leaseToken,
                                                                       .fingerprint = job.request.fingerprint,
                                                                       .args = argsField,
                                                                       .source = sourceField,
                                                                       .acceptedCodecs = job.accepted,
                                                                       .sourceName = BaseName(job.request.sourceName) });
        auto const compileOutcome = exchange.Exchange(job.endpoint, std::move(compileFrame), job.credential, job.budget);
        if (compileOutcome.kind == CacheOutcomeKind::Transport)
            // Unreachable, broken mid-reply, or out of budget. The three are one
            // answer here -- compile it locally -- and the endpoint is named because
            // that is the part an operator can act on.
            return Refused(DispatchStatus::Unavailable, std::format("compile exchange with {} failed", job.endpoint));
        if (!compileOutcome.IsHit())
            // A refusal here is the worker declining the JOB -- an unknown lease, a
            // fingerprint it does not have, an argument it will not accept. Distinct
            // from the compiler running and rejecting the code, which arrives as a
            // successful exchange carrying a non-zero exit code.
            return Refused(DispatchStatus::Declined,
                           std::format("{} refused the job: {}", job.endpoint, DescribeOutcome(compileOutcome)));

        auto const result = Wire::DecodeCompileResult(compileOutcome.value);
        if (!result.has_value())
            return Refused(DispatchStatus::Unavailable, "malformed compile result");

        auto object = Unenvelope(result->object);
        if (!object.has_value())
            return Refused(DispatchStatus::Unavailable, "compile result object could not be decoded");

        return DispatchResult { .status = DispatchStatus::Compiled,
                                .exitCode = static_cast<int>(result->exitCode),
                                .object = *std::move(object),
                                .stdoutText = std::string { Wire::AsStringView(result->stdoutText) },
                                .stderrText = std::string { Wire::AsStringView(result->stderrText) },
                                .detail = {},
                                .workerEndpoint = std::string { job.endpoint } };
    }

    /// Tell the scheduler this lease is done with, however the job ended.
    ///
    /// Without it the key stays marked in-flight for the scheduler's whole lease
    /// timeout -- ten minutes by default -- so recompiling the same translation unit
    /// inside that window is refused `already-in-flight` and falls back to a local
    /// compile (#212). Expiry is the safety net for a client that DIED; this is the
    /// ordinary path.
    ///
    /// A fresh connection rather than the one the lease came back on, and that is
    /// not tidiness: the scheduler sweeps a connection that has been idle for five
    /// seconds (`FrameServer::RequestTimeout`), and a compile takes longer than
    /// that far more often than not. Holding it would make the release fail exactly
    /// when there was something to release.
    ///
    /// **Best effort and deliberately silent.** Every reason it can fail --
    /// scheduler restarted, leadership moved, the lease already expired -- is one
    /// the client can do nothing about and the fleet recovers from on its own, and
    /// there is no caller decision it could change.
    /// @param exchange How to reach the scheduler.
    /// @param request The job, for the scheduler endpoint and the key.
    /// @param leaseToken The token that was granted.
    /// @param credential Presented to the scheduler.
    /// @param budget The control budget; a release is a short request/reply and
    ///        must not inherit the compile's minutes.
    void ReleaseLease(IEndpointExchange& exchange,
                      DispatchRequest const& request,
                      std::string_view leaseToken,
                      Credential const& credential,
                      ExchangeBudget budget)
    {
        // The key travels with the token: a token alone is a number a restarted
        // scheduler will have reissued, and resolving the wrong lease frees a key
        // somebody else is building.
        auto frame = Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = leaseToken, .key = request.objectKey });
        (void) exchange.Exchange(request.schedulerEndpoint, std::move(frame), credential, budget);
    }

} // namespace

std::vector<std::string> DecodeArgs(std::span<std::byte const> field)
{
    std::vector<std::string> out;
    std::size_t offset = 0;
    while (offset + sizeof(std::uint32_t) <= field.size())
    {
        auto const length = ReadBigEndian<std::uint32_t>(field.subspan(offset, sizeof(std::uint32_t)));
        offset += sizeof(std::uint32_t);
        if (field.size() - offset < length)
            return {}; // truncated: refuse the whole list rather than a prefix of it
        out.emplace_back(Wire::AsStringView(field.subspan(offset, length)));
        offset += length;
    }
    return offset == field.size() ? out : std::vector<std::string> {};
}

DispatchResult Dispatch(IEndpointExchange& exchange,
                        DispatchRequest const& request,
                        DispatchBudgets const& budgets,
                        Credential const& credential,
                        Wire::CodecList const& acceptedCodecs)
{
    auto const accepted = acceptedCodecs.empty() ? AvailableCodecs() : acceptedCodecs;

    // --- ask the scheduler where to compile ---------------------------------
    auto leaseFrame = Wire::EncodeLease(
        Wire::LeaseRequest { .fingerprint = request.fingerprint, .key = request.objectKey, .acceptedCodecs = accepted });
    auto const leaseOutcome =
        exchange.Exchange(request.schedulerEndpoint, std::move(leaseFrame), credential, budgets.control);
    if (leaseOutcome.kind == CacheOutcomeKind::Transport)
        // Unreachable or broken mid-reply. Nothing was leased, so there is nothing
        // to release and the worker is never asked -- the client does not guess at
        // an endpoint the scheduler did not name.
        return Refused(DispatchStatus::Unavailable, "scheduler exchange failed");
    if (!leaseOutcome.IsHit())
        // NoWorker, NoCapacity, AlreadyInFlight, DispatchNotPermitted -- every one
        // of them ordinary, and every one answered by compiling locally. The
        // scheduler's own words travel so the caller can say which it was.
        return Refused(DispatchStatus::Declined, DescribeOutcome(leaseOutcome));

    auto const grant = Wire::DecodeLeaseGrant(leaseOutcome.value);
    if (!grant.has_value())
        return Refused(DispatchStatus::Unavailable, "malformed lease grant");

    auto const endpoint = std::string { Wire::AsStringView(grant->endpoint) };
    auto const token = std::string { Wire::AsStringView(grant->leaseToken) };

    // The lease conversation is over and its connection is already gone: an exchange
    // owns its socket for exactly one request/reply. Holding one for the length of a
    // compile would spend a slot of the scheduler's connection budget per parallel
    // launcher on nothing, and the scheduler would sweep it anyway.

    // --- have the worker compile it -----------------------------------------
    auto result = CompileOnWorker(exchange,
                                  LeasedJob { .endpoint = endpoint,
                                              .leaseToken = token,
                                              .codecs = grant->workerCodecs,
                                              .accepted = accepted,
                                              .credential = credential,
                                              .request = request,
                                              .budget = budgets.compile });

    // --- and hand the lease back, however that went -------------------------
    // On every path out of the compile, which is why the compile is a function
    // rather than a run of early returns: an unreachable worker, a refused job and
    // a finished one all mean the same thing to the scheduler -- this key is no
    // longer being built here.
    ReleaseLease(exchange, request, token, credential, budgets.control);
    return result;
}

} // namespace FastCache::Cc
