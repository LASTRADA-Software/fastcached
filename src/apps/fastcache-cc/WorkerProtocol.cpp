// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "Dispatch.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/Compression.hpp>

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

    /// The wire error a refusal maps to.
    ///
    /// A table read rather than a switch with a default, so a refusal added to
    /// JobRefusal without a code here is a compile error rather than a silent
    /// `malformed-frame` that tells an operator nothing.
    [[nodiscard]] Wire::ErrorCode WireCodeFor(JobRefusal refusal)
    {
        switch (refusal)
        {
            case JobRefusal::UnknownFingerprint:
                return Wire::ErrorCode::FingerprintMismatch;
            case JobRefusal::RejectedArgument:
                return Wire::ErrorCode::MalformedFrame;
            case JobRefusal::ScratchUnavailable:
            case JobRefusal::SpawnFailed:
                // Both mean "this worker is broken", not "your code is wrong", and
                // the client must be able to tell them apart from a compile that
                // ran. StorageWriteFailed is the nearest existing code for "I could
                // not do my job", and it is deliberately NOT an exit code.
                return Wire::ErrorCode::StorageWriteFailed;
        }
        return Wire::ErrorCode::MalformedFrame;
    }
} // namespace

WorkerProtocol::WorkerProtocol(CompileJobRunner& jobs, LeaseValidator validator, Wire::CodecList acceptedCodecs):
    _jobs { jobs },
    _validator { std::move(validator) },
    _acceptedCodecs { std::move(acceptedCodecs) }
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

    auto const outcome = _jobs.Run(CompileJob { .fingerprint = std::string { fingerprint },
                                                .args = DecodeArgs(fields->args),
                                                .preprocessed = *std::move(source),
                                                .sourceName = {} });
    if (!outcome.has_value())
        return Wire::EncodeErrorReply(WireCodeFor(outcome.error()), {});

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
                                 Wire::CodecList acceptedCodecs):
    _fingerprint { std::move(fingerprint) },
    _endpoint { std::move(endpoint) },
    _slots { slots },
    _acceptedCodecs { std::move(acceptedCodecs) }
{
}

bool WorkerRegistrar::Register(ITcpClient& scheduler, Credential const& credential)
{
    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest {
        .fingerprint = _fingerprint, .endpoint = _endpoint, .slots = _slots, .acceptedCodecs = _acceptedCodecs });
    auto const outcome = ExchangeFramed(scheduler, frame, credential);
    if (!outcome.IsHit())
        return false;

    _workerId = std::string { Wire::AsStringView(outcome.value) };
    return !_workerId.empty();
}

bool WorkerRegistrar::Heartbeat(ITcpClient& scheduler, std::uint32_t inFlight, Credential const& credential)
{
    if (_workerId.empty())
        return false; // never registered; nothing to refresh

    auto const frame = Wire::EncodeHeartbeat(_workerId, inFlight);
    auto const outcome = ExchangeFramed(scheduler, frame, credential);
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
