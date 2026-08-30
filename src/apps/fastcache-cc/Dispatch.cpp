// SPDX-License-Identifier: Apache-2.0
#include "Dispatch.hpp"

#include <format>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

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
        /// What this client can PRODUCE — what the source is compressed with.
        ///
        /// Deliberately not `accepted`, though the two hold the same value today.
        /// `accepted` is a decode capability and a caller may legitimately narrow it;
        /// narrowing what this client can *read* must not narrow what it can *write*,
        /// because the client never decodes its own source — the worker does. Folding
        /// the two together is the same conflation of two codec lists that #265 was.
        Wire::CodecList const& own;
        Credential const& credential;   ///< Presented to the worker.
        DispatchRequest const& request; ///< The job itself.
        ExchangeBudget budget;          ///< How long the compile may take.
        /// Ceiling on the object the worker may declare it is sending back.
        ///
        /// The launcher dialled a worker the SCHEDULER named, which is not the same
        /// as a worker this process trusts with its address space: a rogue or
        /// compromised fleet member answers this exchange too.
        std::size_t maxObjectBytes;
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
        auto const sourceField = Envelope(Wire::AsBytes(job.request.preprocessed), job.codecs, job.own);

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

        // The worker's declared decompressed size is checked before a byte of it is
        // expanded -- see `Unenvelope`. The reason travels, because "distribution
        // stopped helping" is otherwise a whole investigation.
        auto object = Unenvelope(result->object, job.maxObjectBytes);
        if (!object.has_value())
            return Refused(DispatchStatus::Unavailable,
                           std::format("compile result object from {} could not be decoded: {}",
                                       job.endpoint,
                                       DescribeEnvelopeError(object.error())));

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
    /// @param scheduler The scheduler that ISSUED this lease, which is not always the
    ///        one the client was configured with -- see `LeaseFromFleet`. A release
    ///        sent anywhere else resolves nothing: the key stays marked in flight on
    ///        the machine that actually holds it, which is the outcome this function
    ///        exists to prevent, reached by a different route (#237).
    /// @param request The job, for the key.
    /// @param leaseToken The token that was granted.
    /// @param credential Presented to the scheduler.
    /// @param budget The control budget; a release is a short request/reply and
    ///        must not inherit the compile's minutes.
    void ReleaseLease(IEndpointExchange& exchange,
                      std::string_view scheduler,
                      DispatchRequest const& request,
                      std::string_view leaseToken,
                      Credential const& credential,
                      ExchangeBudget budget)
    {
        // The key travels with the token: a token alone is a number a restarted
        // scheduler will have reissued, and resolving the wrong lease frees a key
        // somebody else is building.
        auto frame = Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = leaseToken, .key = request.objectKey });
        (void) exchange.Exchange(scheduler, std::move(frame), credential, budget);
    }

    /// How many `NotLeader` redirects one lease will follow.
    ///
    /// Two, which is one more than a correct fleet ever needs: a client points at a
    /// member, that member names the leader, and the second ask is answered. The
    /// spare hop covers the leader having moved again between the two, which an
    /// election in progress makes ordinary rather than exotic.
    ///
    /// Bounded at all because the chain is not this client's to trust. Two nodes that
    /// disagree about who leads -- a partition healing, a stale `_knownLeader` -- can
    /// name each other indefinitely, and a client without a ceiling would spend a
    /// build's worth of connects discovering that. The ceiling turns a cycle into an
    /// ordinary local compile, which is what every other lease refusal already means.
    constexpr int MaxLeaseRedirects = 2;

    /// One lease, following `NotLeader` to wherever it points.
    struct LeaseAttempt
    {
        CacheOutcome outcome;  ///< How the last exchange ended.
        std::string scheduler; ///< Who answered it, and who must resolve the lease.
    };

    /// Ask for a lease, following a redirect rather than reading it as a refusal.
    ///
    /// `SchedulerService` has always answered a non-leader with the leader's endpoint
    /// and this client used to discard it, so one election took every launcher out of
    /// distribution until somebody re-pointed them by hand (#237). The endpoint that
    /// finally answers travels back with the outcome because the RELEASE has to go to
    /// the same place -- see `ReleaseLease`.
    /// @param exchange How to reach a scheduler.
    /// @param start The configured endpoint to begin at.
    /// @param request The job, for the fingerprint and key.
    /// @param accepted What this client will read an object back in.
    /// @param credential Presented to each scheduler.
    /// @param budget The control budget, spent per attempt.
    /// @return The final outcome and the endpoint that produced it.
    [[nodiscard]] LeaseAttempt LeaseFromFleet(IEndpointExchange& exchange,
                                              std::string_view start,
                                              DispatchRequest const& request,
                                              Wire::CodecList const& accepted,
                                              Credential const& credential,
                                              ExchangeBudget budget)
    {
        LeaseAttempt attempt { .outcome = {}, .scheduler = std::string { start } };
        for (int hop = 0; hop <= MaxLeaseRedirects; ++hop)
        {
            // Rebuilt per attempt: `Exchange` takes the frame by value and moves it,
            // so the second ask cannot reuse the first one's bytes.
            auto frame = Wire::EncodeLease(Wire::LeaseRequest {
                .fingerprint = request.fingerprint, .key = request.objectKey, .acceptedCodecs = accepted });
            attempt.outcome = exchange.Exchange(attempt.scheduler, std::move(frame), credential, budget);

            auto redirect = RedirectTarget(attempt.outcome);
            if (!redirect.has_value())
                return attempt;
            attempt.scheduler = *std::move(redirect);
        }
        // Out of hops with the last answer still a redirect. Returned as it stands,
        // so the caller reports the scheduler's own words rather than inventing a
        // reason -- and `DispatchStatus::Declined` is exactly right: the fleet said
        // something ordinary and this compile happens locally.
        return attempt;
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
    // Derived ONCE. `available` is what this build can produce; `accepted` is what
    // this client will read back, which a caller may narrow and which therefore is
    // not the same question -- see `LeasedJob::own`.
    auto const available = AvailableCodecs();
    auto const& accepted = acceptedCodecs.empty() ? available : acceptedCodecs;

    // --- ask the scheduler where to compile ---------------------------------
    // Following `NotLeader` rather than reading it as a refusal, and remembering who
    // answered: the lease must be resolved against whoever issued it (#237).
    auto const lease = LeaseFromFleet(exchange, request.schedulerEndpoint, request, accepted, credential, budgets.control);
    auto const& leaseOutcome = lease.outcome;
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
                                              .own = available,
                                              .credential = credential,
                                              .request = request,
                                              .budget = budgets.compile,
                                              .maxObjectBytes = budgets.maxDecompressedBytes });

    // --- and hand the lease back, however that went -------------------------
    // On every path out of the compile, which is why the compile is a function
    // rather than a run of early returns: an unreachable worker, a refused job and
    // a finished one all mean the same thing to the scheduler -- this key is no
    // longer being built here.
    ReleaseLease(exchange, lease.scheduler, request, token, credential, budgets.control);
    return result;
}

} // namespace FastCache::Cc
