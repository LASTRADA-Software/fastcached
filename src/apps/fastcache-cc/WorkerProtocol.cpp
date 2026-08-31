// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"
#include "Dispatch.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <ranges>
#include <string>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What one refusal means on the wire and in the metrics.
    ///
    /// Both in one row, deliberately. They are answers to the same question asked
    /// by two audiences -- the client that has to decide whether to retry, and the
    /// operator watching a fleet -- and a refusal counted under one reason while
    /// being reported as another is worse than not counting it at all. Splitting
    /// them across a `switch` and a second `switch` is how that happens.
    ///
    /// No member carries a default, deliberately. A row answering only two of the
    /// three questions is not a row -- and `ErrorCode` has no zero enumerator to
    /// default to in the first place, so `{}` there names a value the enum does not
    /// have. Omitting the initializers makes the compiler ask for all three, which
    /// is the property the table wanted anyway.
    struct RefusalDescriptor
    {
        JobRefusal refusal;            ///< The reason this row describes.
        Wire::ErrorCode code;          ///< What the client is told.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
    };

    /// One row per `JobRefusal`, in enumerator order.
    ///
    /// The codes are separate rather than collapsed, because collapsing them was
    /// actively misleading: `ScratchUnavailable` and `SpawnFailed` both used to
    /// answer `StorageWriteFailed`, so a worker with no storage told the client
    /// "storage write failed" and two genuinely different operator problems -- an
    /// unwritable scratch disk, and a toolchain that is configured but cannot be
    /// executed -- were indistinguishable from either end. Found the hard way,
    /// diagnosing a CI failure that reported the one thing it could not possibly
    /// be.
    constexpr EnumTable<JobRefusal, RefusalDescriptor> RefusalTable { {
        { .refusal = JobRefusal::UnknownFingerprint,
          .code = Wire::ErrorCode::FingerprintMismatch,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint },
        { .refusal = JobRefusal::RejectedArgument,
          .code = Wire::ErrorCode::MalformedFrame,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedRejectedArgument },
        { .refusal = JobRefusal::ScratchUnavailable,
          .code = Wire::ErrorCode::WorkerScratchUnavailable,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedScratchUnavailable },
        { .refusal = JobRefusal::SpawnFailed,
          .code = Wire::ErrorCode::WorkerSpawnFailed,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedSpawnFailed },
        { .refusal = JobRefusal::ToolchainSurveyInFlight,
          .code = Wire::ErrorCode::WorkerToolchainSurveyInFlight,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedSurveyInFlight },
    } };

    static_assert(RowsInEnumeratorOrder(RefusalTable, &RefusalDescriptor::refusal),
                  "RefusalTable must hold one row per JobRefusal, in enumerator order");

    /// The row describing `refusal`.
    /// @param refusal What the runner reported.
    /// @return Its descriptor.
    [[nodiscard]] constexpr RefusalDescriptor const& DescriptorFor(JobRefusal refusal) noexcept
    {
        return RefusalTable[static_cast<std::size_t>(refusal)];
    }

    /// This surface's rows. The shape, the lookup and why they exist are on
    /// `Wire::RefusedVerb`; what belongs here is only which verbs and what they say.
    ///
    /// `Auth`, because a `--requirepass` worker was refused at `REGISTER` and never
    /// joined the fleet at all -- absent rather than idle, which is harder to notice.
    constexpr std::array RefusedVerbs {
        Wire::RefusedVerb { .op = Wire::Op::Auth,
                            .code = Wire::UnimplementedVerb,
                            .why = "this endpoint compiles and checks no credential" },
    };

    // The table is consulted only on the path a verb other than COMPILE takes, so a
    // row naming COMPILE would sit there looking like a decision and change nothing.
    // Refused at compile time rather than left to be noticed.
    static_assert(std::ranges::none_of(
                      RefusedVerbs, [](Wire::Op op) { return op == Wire::Op::Compile; }, &Wire::RefusedVerb::op),
                  "a refusal row for COMPILE is dead: the lookup never reaches it");
} // namespace

LeaseValidator SignedLeaseValidator(std::vector<std::byte> signingKey,
                                    std::string advertisedEndpoint,
                                    std::string clusterId,
                                    IWallClock const& clock)
{
    return [key = std::move(signingKey), endpoint = std::move(advertisedEndpoint), cluster = std::move(clusterId), &clock](
               std::string_view token, std::string_view fingerprint) -> std::optional<Distributed::LeaseRefusal> {
        // The fingerprint is the one the REQUEST names, and this runs BEFORE anything
        // has checked that this worker serves it -- `CompileJobRunner::Run` answers
        // that later, with `UnknownFingerprint`. So the two comparisons compose rather
        // than one presupposing the other: the grant must name what the request names,
        // and the request must name something served. Together that stops one
        // toolchain's lease from paying for another's compile; neither does it alone,
        // which is why the grant's fingerprint check is not redundant with the
        // worker's.
        auto verified = Distributed::VerifyLeaseToken(
            key,
            token,
            Distributed::LeaseExpectation { .endpoint = endpoint,
                                            .fingerprint = fingerprint,
                                            .clusterId = cluster,
                                            // Stated, not omitted -- see the header.
                                            .epoch = Distributed::LeaseEpochCheck::NotKnownHere() },
            clock.Now());
        if (verified.has_value())
            return std::nullopt;

        // The CLAIMS are dropped deliberately -- what a worker needs from a grant is
        // permission, and the object key inside it is the SCHEDULER's bookkeeping, so
        // a worker comparing it against what it was asked to compile would be
        // re-deriving a decision it is not the one making. The refusal travels whole,
        // diagnostic included: that string was formatted for this caller and used to
        // be allocated and dropped.
        return std::move(verified.error());
    };
}

LeaseValidator UncheckedLeaseValidator()
{
    return [](std::string_view, std::string_view) {
        return std::optional<Distributed::LeaseRefusal> {};
    };
}

WorkerProtocol::WorkerProtocol(ICompileJobRunner& jobs,
                               LeaseValidator validator,
                               Wire::CodecList acceptedCodecs,
                               IMetricsSink& metrics,
                               std::size_t maxDecompressedBytes):
    _jobs { jobs },
    _validator { std::move(validator) },
    _acceptedCodecs { std::move(acceptedCodecs) },
    _metrics { metrics },
    _maxDecompressedBytes { maxDecompressedBytes }
{
}

std::size_t DeclaredRequestFootprint(std::span<std::byte const> frame) noexcept
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        return 0;

    // What `Serve` already charged, and the floor for everything below: a frame this
    // function cannot look inside still costs its own length.
    auto const framed = static_cast<std::size_t>(header->payloadLength);

    // Every one of these is a request `Answer` refuses on its own terms -- a foreign
    // verb, a truncated frame, a payload that does not split. None of them reaches
    // `Unenvelope`, so none of them declares a second buffer, and answering `framed`
    // leaves the refusal where it belongs rather than turning a malformed frame into
    // a busy signal.
    //
    // The length test subtracts rather than adds: `RequestHeaderSize + framed` wraps
    // where `size_t` is 32 bits, and a wrapped sum turns "this frame is truncated"
    // into "this frame is long enough" and hands `subspan` a count past the end.
    // `DecodeRequestHeader` already refused a buffer shorter than the header, so the
    // subtraction cannot underflow.
    if (header->opRaw != static_cast<std::uint8_t>(Wire::Op::Compile) || frame.size() - Wire::RequestHeaderSize < framed)
        return framed;

    auto const fields = Wire::DecodeCompilePayload(frame.subspan(Wire::RequestHeaderSize, framed));
    if (!fields.has_value())
        return framed;

    auto const envelope = Wire::DecodeCodecEnvelope(fields->source);
    if (!envelope.has_value())
        return framed;

    // The declared expansion, believed only as a PRICE. Whether it is one this
    // endpoint will pay at all is `Unenvelope`'s ceiling, checked where the
    // allocation happens; this decides what it is charged for having asked.
    return std::max(framed, static_cast<std::size_t>(envelope->rawLength));
}

std::optional<std::vector<std::byte>> WorkerProtocol::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: the peer is not speaking this protocol at all, so there is no
        // framing in which a reply would be meaningful. The one case that closes.
        return std::nullopt;

    if (!Wire::IsSupported(header->version))
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion, {});

    if (frame.size() < Wire::RequestHeaderSize + header->payloadLength)
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame, "frame shorter than its declared payload");
    auto const payload = frame.subspan(Wire::RequestHeaderSize, header->payloadLength);

    auto const* const descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode, {});

    if (descriptor->code != Wire::Op::Compile)
    {
        if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
            return Wire::EncodeErrorReply(row->code, row->why);

        // A worker is not a scheduler and not a cache. Refused with a reply rather
        // than a close, so a client that sent the wrong verb to the wrong port
        // learns which -- a dropped connection is indistinguishable from a dead host.
        return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted,
                                      "this endpoint compiles; it does not schedule or cache");
    }

    return Compile(payload);
}

std::vector<std::byte> WorkerProtocol::Compile(std::span<std::byte const> payload)
{
    auto const fields = Wire::DecodeCompilePayload(payload);
    if (!fields.has_value())
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame, {});

    auto const token = Wire::AsStringView(fields->leaseToken);
    auto const fingerprint = Wire::AsStringView(fields->fingerprint);

    // Checked BEFORE the payload is decompressed, let alone compiled.
    //
    // Stated precisely, because the obvious stronger claim is false: by the time this
    // runs, `WorkerServer` has already read the whole frame off the socket and
    // charged it against the in-flight byte budget, and the token is a field INSIDE
    // that payload -- so no earlier gate exists short of a protocol change, and an
    // unauthorized peer still costs a read and a slot. What the check saves is
    // decompression and the compiler spawn, which is the part that matters by three
    // or four orders of magnitude.
    //
    // The reason is carried out rather than collapsed to a bool, and its wire code
    // and its counter come from ONE row of `LeaseRefusalTable` -- one fact, two
    // audiences, which is what that table was built for in #281.
    //
    // NOT `UnknownLease`. That is the SCHEDULER's code, meaning "a lease I issued
    // and have since forgotten", and a worker answering with it sent an operator to
    // the scheduler to look for a fault that is local.
    if (auto const refusal = _validator(token, fingerprint); refusal.has_value())
    {
        auto const& row = Distributed::DescribeLeaseRefusal(refusal->reason);
        _metrics.Increment(row.workerCounter);

        // The detail travels. It is empty for anything that failed the MAC -- a
        // caller that could not authenticate a token has established no fact about
        // it, so there is nothing truthful to say -- and populated for the two
        // refusals an operator actually has to act on.
        return Wire::EncodeErrorReply(row.code, refusal->detail);
    }

    // Opened AFTER the lease check and BEFORE any expensive work, and refused on the
    // DECLARED decompressed length rather than on what it expands to -- see
    // `Unenvelope`, which carries the reasoning.
    //
    // A REPLY, never a close: the frame declared its own length, so the connection is
    // still synchronised and a peer that guessed wrong learns which.
    //
    // `UnenvelopeText`, not `Unenvelope`: the runner wants a `std::string`, and an
    // `Identity` source would otherwise copy a whole preprocessed translation unit
    // into a `std::vector<std::byte>` on the way. That used to be every source --
    // a node negotiated no codec but `Identity` (#265) -- and is now the
    // compression-less build's path, which can least afford the spare copy.
    auto source = UnenvelopeText(fields->source, _maxDecompressedBytes);
    if (!source.has_value())
    {
        // Code, text and counter come from ONE row rather than a ternary beside a
        // lookup: a malformed frame answered `UnsupportedCodec` while its message
        // said "malformed" would send an operator hunting a codec mismatch that
        // never happened.
        //
        // Counted here and not inside `Unenvelope`, because the launcher calls that
        // too and has no sink -- and a refusal answered on the wire while nothing
        // rises is how a port being probed with envelope bombs looked, on
        // `/metrics`, exactly like a port nobody was talking to.
        auto const reason = source.error();
        _metrics.Increment(CounterFor(reason));
        return Wire::EncodeErrorReply(WireCodeFor(reason), DescribeEnvelopeError(reason));
    }

    // Counted around the runner rather than inside it: the runner is a seam with
    // its own fakes, and a fake that forgot to count would make every test agree
    // with a worker that does not.
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsStarted);
    auto const startedAt = std::chrono::steady_clock::now();

    auto const outcome = _jobs.Run(CompileJob { .fingerprint = std::string { fingerprint },
                                                .args = DecodeArgs(fields->args),
                                                // NOT `auto const source` above, for the reason
                                                // `CodecEnvelope` records: `*std::move(x)` on a
                                                // `const expected` is a `T const&&`, which binds
                                                // to the COPY constructor with no diagnostic.
                                                .preprocessed = *std::move(source),
                                                // Sanitized where it becomes a path, not here: the
                                                // runner is what creates the file, so the check
                                                // belongs beside the creation rather than at each
                                                // caller that might forget it.
                                                .sourceName = std::string { Wire::AsStringView(fields->sourceName) } });
    if (!outcome.has_value())
    {
        auto const& descriptor = DescriptorFor(outcome.error().reason);
        _metrics.Increment(descriptor.counter);
        // The refusal's detail rides the reply message, so a client's local fallback
        // can name the offending flag rather than only report that one existed. Empty
        // for every refusal that has nothing to add, which reproduces the previous
        // empty-message wire exactly.
        return Wire::EncodeErrorReply(descriptor.code, outcome.error().detail);
    }

    // A compiler that ran and rejected the code did its job — that is the client's
    // answer, not a worker failure — so this counts completions rather than
    // successes, and a non-zero exit is not a refusal.
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted);
    _metrics.Increment(IMetricsSink::Counter::WorkerCompileMillisTotal, static_cast<std::uint64_t>(elapsed.count()));

    // The object goes back in an envelope chosen from what the CLIENT said it
    // accepts -- carried in its own request, so no negotiation round trip. `Envelope`,
    // the same function the client wrapped its source with: the two directions are one
    // negotiation, and a second implementation of the choice is how they come to
    // disagree.
    // (This call site was that second implementation; #265 and `Envelope`'s own doc
    // carry the history.)
    auto const enveloped = Envelope(outcome->object, fields->acceptedCodecs, _acceptedCodecs);

    return Wire::EncodeReply(
        Wire::Status::Ok,
        Wire::EncodeCompileResult(Wire::CompileResult { .exitCode = static_cast<std::uint32_t>(outcome->exitCode),
                                                        .object = enveloped,
                                                        .stdoutText = Wire::AsBytes(outcome->stdoutText),
                                                        .stderrText = Wire::AsBytes(outcome->stderrText),
                                                        // Carried through from the runner, never recomputed from
                                                        // `fields` here -- see `ICompileJobRunner` (#280).
                                                        .correlation = Wire::AsBytes(outcome->correlation) }));
}

WorkerRegistrar::WorkerRegistrar(CredentialNotice& notice,
                                 std::string fingerprint,
                                 std::string endpoint,
                                 std::uint32_t slots,
                                 Wire::CodecList acceptedCodecs,
                                 Wire::CapacityFields capacity):
    _notice { notice },
    _fingerprint { std::move(fingerprint) },
    _endpoint { std::move(endpoint) },
    _slots { slots },
    _acceptedCodecs { std::move(acceptedCodecs) },
    // Moved rather than copied, like every other member here. It became worth
    // saying when `CapacityFields` grew the node's cache record: it was a handful
    // of scalars and is now a struct holding a vector, so a copy allocates.
    _capacity { std::move(capacity) }
{
}

std::expected<void, AnnounceRefusal> WorkerRegistrar::Register(ISocket& scheduler, Credential const& credential)
{
    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest { .fingerprint = _fingerprint,
                                                                    .endpoint = _endpoint,
                                                                    .slots = _slots,
                                                                    .acceptedCodecs = _acceptedCodecs,
                                                                    .capacity = _capacity });
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, &_notice, frame, credential));
    if (!outcome.IsHit())
        // The scheduler's own words, code and message both, which is the whole
        // reason this is not a bool: "not a member of this cluster" and "fingerprint
        // is not valid UTF-8" call for opposite actions from an operator, and this
        // node cannot tell them apart from its own side.
        //
        // `RedirectTarget` rather than a second reading of the same refusal: a
        // `NotLeader` whose message is prose, or names a bare port, is not a
        // redirect, and that judgement belongs in one place for the launcher's
        // lease chain and this alike.
        return std::unexpected { AnnounceRefusal { .reason = DescribeOutcome(outcome), .leader = RedirectTarget(outcome) } };

    _workerId = std::string { Wire::AsStringView(outcome.value) };
    if (_workerId.empty())
        // Accepted and unusable: every later heartbeat needs the id, so a worker
        // that kept going here would heartbeat nothing into a fleet that thinks it
        // is registered. Reported as a refusal because that is what it costs.
        // No `leader`: this scheduler accepted, so it is not telling this node to
        // go elsewhere. Following a redirect here would send a working registration
        // to a second scheduler over a fault that is this one's to fix.
        return std::unexpected { AnnounceRefusal { .reason = "accepted, and assigned no worker id",
                                                   .leader = std::nullopt } };

    return {};
}

std::expected<void, AnnounceRefusal> WorkerRegistrar::Heartbeat(ISocket& scheduler,
                                                                std::uint32_t inFlight,
                                                                Wire::LoadFields const& load,
                                                                Credential const& credential)
{
    if (_workerId.empty())
        // Never registered; nothing to refresh. Named rather than silent, because
        // the caller's next move -- register -- is the same either way, and a
        // diagnostic that cannot say which of the two happened is one an operator
        // cannot act on.
        return std::unexpected { AnnounceRefusal { .reason = "not registered", .leader = std::nullopt } };

    auto const frame = Wire::EncodeHeartbeat(_workerId, inFlight, load);
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, &_notice, frame, credential));
    if (outcome.IsHit())
        return {};

    // A scheduler that does not know this worker is telling it to register again --
    // it restarted, or expired this entry. Forgetting the id here is what makes the
    // caller's retry actually re-register instead of heartbeating into a void
    // forever while the fleet runs without it.
    if (outcome.kind == CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::UnknownLease)
        _workerId.clear();

    // The id is NOT cleared for a redirect. A `NotLeader` says this scheduler is
    // the wrong one to ask, not that the fleet has forgotten this worker -- and the
    // leader it names may well be holding the very registration this id belongs to,
    // since the registry is replicated. Clearing it here would turn every election
    // into a re-registration storm across the whole fleet.
    return std::unexpected { AnnounceRefusal { .reason = DescribeOutcome(outcome), .leader = RedirectTarget(outcome) } };
}

} // namespace FastCache::Cc
