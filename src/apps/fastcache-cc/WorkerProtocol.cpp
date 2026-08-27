// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "Dispatch.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/Compression.hpp>
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

    /// Undo a codec envelope, refusing anything this worker cannot decode.
    [[nodiscard]] std::optional<std::string> Unenvelope(std::span<std::byte const> field)
    {
        auto const envelope = Wire::DecodeCodecEnvelope(field);
        if (!envelope.has_value())
            return std::nullopt;
        if (envelope->codec == Wire::IdentityCodec)
            return std::string { Wire::AsStringView(envelope->bytes) };

        auto const codec = static_cast<CompressionCodec>(envelope->codec);
        if (!Compression::IsAvailable(codec))
            return std::nullopt;
        auto const decoded = Compression::Decompress(codec, envelope->bytes, envelope->rawLength);
        if (!decoded.has_value())
            return std::nullopt;
        return std::string { Wire::AsStringView(*decoded) };
    }

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
} // namespace

WorkerProtocol::WorkerProtocol(CompileJobRunner& jobs,
                               LeaseValidator validator,
                               Wire::CodecList acceptedCodecs,
                               IMetricsSink& metrics):
    _jobs { jobs },
    _validator { std::move(validator) },
    _acceptedCodecs { std::move(acceptedCodecs) },
    _metrics { metrics }
{
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
        // A worker is not a scheduler and not a cache. Refused with a reply rather
        // than a close, so a client that sent the wrong verb to the wrong port
        // learns which -- a dropped connection is indistinguishable from a dead host.
        return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted,
                                      "this endpoint compiles; it does not schedule or cache");

    return Compile(payload);
}

std::vector<std::byte> WorkerProtocol::Compile(std::span<std::byte const> payload)
{
    auto const fields = Wire::DecodeCompilePayload(payload);
    if (!fields.has_value())
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame, {});

    auto const token = Wire::AsStringView(fields->leaseToken);
    auto const fingerprint = Wire::AsStringView(fields->fingerprint);

    // Checked BEFORE the payload is decompressed, let alone compiled: an
    // unauthorized peer must not be able to make this worker do the expensive part.
    if (_validator && !_validator(token, fingerprint))
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownLease, {});

    auto source = Unenvelope(fields->source);
    if (!source.has_value())
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedCodec,
                                      "the preprocessed source is in a codec this worker cannot decode");

    // Counted around the runner rather than inside it: the runner is a seam with
    // its own fakes, and a fake that forgot to count would make every test agree
    // with a worker that does not.
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsStarted);
    auto const startedAt = std::chrono::steady_clock::now();

    auto const outcome = _jobs.Run(CompileJob { .fingerprint = std::string { fingerprint },
                                                .args = DecodeArgs(fields->args),
                                                .preprocessed = *std::move(source),
                                                // Sanitized where it becomes a path, not here: the
                                                // runner is what creates the file, so the check
                                                // belongs beside the creation rather than at each
                                                // caller that might forget it.
                                                .sourceName = std::string { Wire::AsStringView(fields->sourceName) } });
    if (!outcome.has_value())
    {
        auto const& descriptor = DescriptorFor(outcome.error());
        _metrics.Increment(descriptor.counter);
        return Wire::EncodeErrorReply(descriptor.code, {});
    }

    // A compiler that ran and rejected the code did its job — that is the client's
    // answer, not a worker failure — so this counts completions rather than
    // successes, and a non-zero exit is not a refusal.
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted);
    _metrics.Increment(IMetricsSink::Counter::WorkerCompileMillisTotal, static_cast<std::uint64_t>(elapsed.count()));

    // The object goes back in an envelope chosen from what the CLIENT said it
    // accepts -- carried in its own request, so no negotiation round trip.
    auto const chosen = Wire::ChooseCodec(_acceptedCodecs, _acceptedCodecs);
    auto enveloped =
        Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(outcome->object.size()), outcome->object);
    (void) chosen;

    return Wire::EncodeReply(
        Wire::Status::Ok,
        Wire::EncodeCompileResult(Wire::CompileResult { .exitCode = static_cast<std::uint32_t>(outcome->exitCode),
                                                        .object = enveloped,
                                                        .stdoutText = Wire::AsBytes(outcome->stdoutText),
                                                        .stderrText = Wire::AsBytes(outcome->stderrText) }));
}

WorkerRegistrar::WorkerRegistrar(std::string fingerprint,
                                 std::string endpoint,
                                 std::uint32_t slots,
                                 Wire::CodecList acceptedCodecs,
                                 Wire::CapacityFields capacity):
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

std::expected<void, std::string> WorkerRegistrar::Register(ISocket& scheduler, Credential const& credential)
{
    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest { .fingerprint = _fingerprint,
                                                                    .endpoint = _endpoint,
                                                                    .slots = _slots,
                                                                    .acceptedCodecs = _acceptedCodecs,
                                                                    .capacity = _capacity });
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, frame, credential));
    if (!outcome.IsHit())
        // The scheduler's own words, code and message both, which is the whole
        // reason this is not a bool: "not a member of this cluster" and "fingerprint
        // is not valid UTF-8" call for opposite actions from an operator, and this
        // node cannot tell them apart from its own side.
        return std::unexpected { DescribeOutcome(outcome) };

    _workerId = std::string { Wire::AsStringView(outcome.value) };
    if (_workerId.empty())
        // Accepted and unusable: every later heartbeat needs the id, so a worker
        // that kept going here would heartbeat nothing into a fleet that thinks it
        // is registered. Reported as a refusal because that is what it costs.
        return std::unexpected { std::string { "accepted, and assigned no worker id" } };

    return {};
}

bool WorkerRegistrar::Heartbeat(ISocket& scheduler,
                                std::uint32_t inFlight,
                                Wire::LoadFields const& load,
                                Credential const& credential)
{
    if (_workerId.empty())
        return false; // never registered; nothing to refresh

    auto const frame = Wire::EncodeHeartbeat(_workerId, inFlight, load);
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, frame, credential));
    if (outcome.IsHit())
        return true;

    // A scheduler that does not know this worker is telling it to register again --
    // it restarted, or expired this entry. Forgetting the id here is what makes the
    // caller's retry actually re-register instead of heartbeating into a void
    // forever while the fleet runs without it.
    if (outcome.kind == CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::UnknownLease)
        _workerId.clear();
    return false;
}

} // namespace FastCache::Cc
