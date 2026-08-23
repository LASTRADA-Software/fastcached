// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/SchedulerProtocol.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The verbs a scheduler answers.
    ///
    /// A table rather than three `case` labels so the membership test below is
    /// derived from it: a verb that reaches this class without a row is refused
    /// rather than served, which is the direction a mistake has to fail in.
    constexpr std::array SchedulerOps { Wire::Op::Register, Wire::Op::Heartbeat, Wire::Op::Lease };

    /// Whether this scheduler serves @p op at all.
    /// @param op The verb, already resolved against `OpTable`.
    /// @return True when it is one of `SchedulerOps`.
    [[nodiscard]] constexpr bool IsSchedulerVerb(Wire::Op op) noexcept
    {
        return std::ranges::find(SchedulerOps, op) != SchedulerOps.end();
    }
} // namespace

std::vector<std::byte> SchedulerProtocol::Answer(std::span<std::byte const> frame, CallerContext const& caller)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic or a short read. This is the one condition a caller cannot
        // answer *and* resynchronize from -- with no declared length there is no
        // way to find where the frame ended -- so the transport closes, and an
        // empty answer is how it is told to.
        return {};

    if (!Wire::IsSupported(header->version))
        // Naming the supported range, because a rejection that cannot say what
        // would have worked cannot be acted on.
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                      std::format("supported versions {}..{}",
                                                  static_cast<unsigned>(Wire::MinSupportedVersion),
                                                  static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        // Stepped over rather than fatal: the framing exists so a receiver can skip
        // a verb it does not know, which is what lets a newer client talk to an
        // older scheduler at all.
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode);

    if (!IsSchedulerVerb(descriptor->code))
        // A cache verb at the scheduler's port. Answered rather than dropped, so a
        // misconfigured client learns which port it got wrong instead of seeing
        // something indistinguishable from a dead host.
        return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

    auto const reply = Route(descriptor->code, payload, caller);
    if (reply.status == Wire::Status::Ok)
        return Wire::EncodeReply(Wire::Status::Ok, reply.payload);
    return Wire::EncodeErrorReply(reply.error, reply.message);
}

SchedulerReply SchedulerProtocol::Route(Wire::Op op, std::span<std::byte const> payload, CallerContext const& caller)
{
    switch (op)
    {
        case Wire::Op::Register: {
            auto const fields = Wire::DecodeRegisterPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Register(caller,
                                     WorkerRegistration { .fingerprint = Wire::AsStringView(fields->fingerprint),
                                                          .endpoint = Wire::AsStringView(fields->endpoint),
                                                          .slots = fields->slots,
                                                          .codecs = fields->acceptedCodecs });
        }
        case Wire::Op::Heartbeat: {
            auto const fields = Wire::DecodeHeartbeatPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Heartbeat(caller, Wire::AsStringView(fields->workerId), fields->inFlight);
        }
        case Wire::Op::Lease: {
            auto const fields = Wire::DecodeLeasePayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Lease(caller,
                                  Wire::LeaseRequest { .fingerprint = Wire::AsStringView(fields->fingerprint),
                                                       .key = Wire::AsStringView(fields->key),
                                                       .acceptedCodecs = fields->acceptedCodecs });
        }
        default:
            // Unreachable: `IsSchedulerVerb` has already refused everything else.
            // Kept as a refusal rather than an assertion because a verb added to
            // `SchedulerOps` without a case here must fail closed, which is the
            // whole reason the table drives the test.
            return SchedulerReply {
                .status = Wire::Status::Error, .error = Wire::ErrorCode::DispatchNotPermitted, .message = {}, .payload = {}
            };
    }
}

} // namespace FastCache::Distributed
