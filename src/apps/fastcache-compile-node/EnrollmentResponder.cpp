// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentResponder.hpp"
#include "MembershipGate.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

// The wire's fingerprint width is spelled in the dependency-free header the launcher compiles
// in, and the digest's in `Core/Sha256`; this is the one file that sees both.
static_assert(Wire::RosterFingerprintBytes == Sha256::DigestSize, "a roster fingerprint on the wire is one SHA-256 digest");

namespace
{
    /// Why a size or opcode refusal on this surface moves nothing.
    ///
    /// Stated once because the pre-payload table below has two uncounted rows sharing
    /// the reason and `UncountedRefusal::rationale` is a forcing function rather than a
    /// field: what it forces is that somebody answered *would a rise here mean
    /// something happened*, and the answer is the same for both.
    constexpr std::string_view ShapeRefusalRationale =
        "a size or opcode refusal says the peer is confused about the framing, not about this cluster; summed into "
        "the enrollment series it would bury the two refusals that mean somebody is trying an open window";

    /// Why the endpoint's byte budget is not this surface's to count.
    constexpr std::string_view ByteBudgetRationale =
        "the byte budget says this surface is momentarily full, which the peer sees and retries; summed into a "
        "series read as somebody probing an open window it is what makes that series unreadable";

    /// Why a credential refusal here belongs to the scheduler.
    constexpr std::string_view CredentialIsTheSchedulersRationale =
        "the credential is the scheduler's -- AUTH is a Session verb and MergedResponder routes it there -- so the "
        "peer that presented it is counted against the component that checked it, once";

    /// One row per `EndpointRefusal`: what this surface does about it.
    struct EnrollmentEndpointRefusal
    {
        EndpointRefusal refusal;                  ///< Which endpoint decision this describes.
        std::optional<Cc::SurfaceRefusal> answer; ///< The row, or nothing where this surface counts none.
        std::string_view rationale;               ///< Why nothing is counted; read only when `answer` is absent.
    };

    /// This surface counts none of the endpoint's own refusals.
    ///
    /// **And that is four separate claims rather than one shrug.** The byte budget and
    /// the answer deadline are uncounted for reasons every surface here shares; the two
    /// credential rows are uncounted because the credential belongs to the scheduler,
    /// which checks it and counts it -- a second tally here would be one AUTH failure
    /// reported twice to whoever met both series.
    ///
    /// The counted refusals on this surface are all decided INSIDE `Answer`, where the
    /// verb and the window's state are both known, which is why this table has no
    /// counted row and that is not an omission.
    constexpr EnumTable<EndpointRefusal, EnrollmentEndpointRefusal> EnrollmentEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget, .answer = std::nullopt, .rationale = ByteBudgetRationale },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSchedulersRationale },
        { .refusal = EndpointRefusal::CredentialRejected,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSchedulersRationale },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .answer = std::nullopt,
          .rationale = AnswerDeadlineIsTheEndpointsRationale },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .answer = std::nullopt,
          .rationale = NodeProofIsTheProversRationale },
    } };

    static_assert(Cc::RowsStateOneRefusalClaim(EnrollmentEndpointRefusals,
                                               [](EnrollmentEndpointRefusal const& row) {
                                                   return Cc::RefusalClaim { .counted = row.answer.has_value(),
                                                                             .rationale = row.rationale };
                                               }),
                  "every enrollment endpoint refusal must state either a counted answer or a rationale, not both");

    // Positional rows alone would not catch an appended enumerator: it leaves a
    // value-initialised row whose `answer` is `nullopt`, which the guard above passes
    // vacuously. The row carries its enumerator so the position is checked whatever the
    // answer is.
    static_assert(RowsInEnumeratorOrder(EnrollmentEndpointRefusals, &EnrollmentEndpointRefusal::refusal),
                  "EnrollmentEndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    /// The refusal answered when this node is not the leader.
    ///
    /// Uncounted, exactly as the scheduler's is: a healthy cluster answers this every
    /// time a joiner or an operator reaches a follower, which on a three-node cluster is
    /// two thirds of the time, and it is an INSTRUCTION a client follows rather than an
    /// event. The message is the leader's endpoint and must stay parseable as one --
    /// `LeaderRedirectTarget` reads it -- so nothing is added to it.
    constexpr Cc::UncountedRefusal NotLeaderRefusal {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "a healthy cluster answers this whenever a joiner reaches a follower, which is most of the "
                     "time on any cluster above one node; it is an instruction the client follows, not an event",
    };
} // namespace

Task<FrameReply> EnrollmentResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Empty is CLOSE, and it is the right answer to exactly this: a frame whose
        // header will not decode is not this protocol, which is the one condition every
        // responder here closes on. Every other refusal is a reply.
        co_return std::vector<std::byte> {};

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        co_return Cc::Refuse(_metrics,
                             { .code = Wire::ErrorCode::MalformedFrame,
                               .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                             "the frame's declared length and its payload disagree");

    switch (static_cast<Wire::Op>(header->opRaw))
    {
        case Wire::Op::Enroll:
            // The HOST: what `Enroll` records is where a joiner dialled from, which is the fact
            // an operator compares against the endpoint it CLAIMS. A joiner is by construction
            // a machine this cluster has not admitted, so there is never a proof here to fold.
            co_return AnswerEnroll(payload, peer.host);
        case Wire::Op::EnrollControl:
            // The identity, whose proof `RefusePeer` has already folded into the membership
            // answer; `ClusterAdmit` gates on the host, which `Context` takes from it.
            co_return AnswerControl(payload, peer);
        default:
            break;
    }

    // Unreachable through `MergedResponder`, which routes by family and sends this
    // component nothing else -- and answered rather than asserted, for the reason every
    // pure virtual on `IFrameResponder` is pure: a surface that inherits an answer
    // inherits whatever the next verb added to this family happens to need.
    co_return Cc::RefuseWithoutCounter({ .code = Wire::UnimplementedVerb,
                                         .rationale = "unreachable: MergedResponder routes by family and this "
                                                      "component owns two verbs, both handled above" },
                                       "this node serves no component for that verb");
}

std::optional<std::vector<std::byte>> EnrollmentResponder::RefusePeer(PeerIdentity const& peer, std::uint8_t opRaw) const
{
    // `Enroll` admits everybody, which is this surface's one open door and is the whole
    // point of it. See the declaration.
    if (static_cast<Wire::Op>(opRaw) == Wire::Op::Enroll)
        return std::nullopt;

    return RefuseUnlessMember(
        _membership,
        _metrics,
        peer,
        { .code = Wire::ErrorCode::NotAMember, .counter = IMetricsSink::Counter::EnrollmentControlRefusedNotAMember },
        "deciding who joins this cluster is a member's verb");
}

std::vector<std::byte> EnrollmentResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                         std::uint8_t /*opRaw*/,
                                                         std::string_view detail) const
{
    if (decision == Wire::PrePayloadDecision::Unauthenticated)
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCodeFor(decision),
                            .counter = IMetricsSink::Counter::EnrollmentControlRefusedUnauthenticated },
                          detail);
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision), .rationale = ShapeRefusalRationale }, detail);
}

std::vector<std::byte> EnrollmentResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                 std::uint8_t /*opRaw*/,
                                                                 std::string_view detail) const
{
    auto const& row = EnrollmentEndpointRefusals[static_cast<std::size_t>(refusal)];
    return AnswerEndpointRefusal(_metrics, ErrorCodeFor(refusal), row.answer, row.rationale, detail);
}

std::vector<std::byte> EnrollmentResponder::AnswerEnroll(std::span<std::byte const> payload, std::string_view peer)
{
    // Leadership first, and before the payload is even split: a follower holds no
    // window anybody can act on, and telling a joiner where the leader is costs one
    // redirect where refusing it silently costs a rollout.
    if (_scheduler.Role() != Distributed::SchedulerRole::Leader)
        return Cc::RefuseWithoutCounter(NotLeaderRefusal, _scheduler.LeaderEndpoint());

    auto const fields = Wire::DecodeEnrollPayload(payload);
    if (!fields.has_value())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          "an enroll request names an id, a consensus endpoint, a role this build knows and a "
                          "32-byte identity key");

    auto const nodeId = Wire::AsStringView(fields->nodeId);
    auto const raftEndpoint = Wire::AsStringView(fields->raftEndpoint);
    auto const& role = EnrollRoleRowFor(fields->role);

    // Text, or the fleet refuses it: this id is copied into `ClusterState` on approval
    // and read back out of `/fleet.json` by everybody, so one byte that is not UTF-8
    // makes that document unparseable for the whole fleet. Refused where it ENTERS,
    // which is here -- a consensus entry is applied after it is committed, with nobody
    // left to refuse it. The endpoint travels the same way and is checked the same way.
    if (!IsValidUtf8(nodeId) || !IsValidUtf8(raftEndpoint))
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          "a joiner must name itself and its endpoint in UTF-8");

    // An empty id, or an endpoint that does not suit the role, is refused here rather than at
    // the approval, because the list is what a PERSON reads and a row they cannot act on is
    // one they should never be shown. A member with no endpoint is the member
    // `Cluster::ClusterMember` exists to make unrepresentable; a worker WITH one is claiming an
    // address the principal it becomes has nowhere to keep.
    if (nodeId.empty() || role.statesEndpoint == raftEndpoint.empty())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          std::format("an enroll request names an id, and a {} {} a consensus endpoint",
                                      role.name,
                                      role.statesEndpoint ? "must name" : "names no"));

    // A key the cluster has REVOKED is refused at the door (#1555), before the window records
    // anything. No approval could admit it -- `Cluster::ValidateAgainst` refuses it by name --
    // so a row for it is one the operator reading the list cannot act on, and `Pending` would
    // keep a forgotten machine polling for an answer that cannot come. Same code as that
    // refusal, so the joiner hears what an approval would have been told.
    if (auto const state = _scheduler.AdministeredState(); state.has_value() && state->IsRevoked(fields->publicKey))
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::InvalidClusterChange,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedRevokedKey },
                          std::format("{} was revoked when this cluster forgot the machine holding it, and a "
                                      "revoked key is never admitted again: move this machine's --cluster-dir "
                                      "aside so it mints a new identity, and enrol that",
                                      FormatEd25519PublicKey(fields->publicKey)));

    auto const claim =
        JoinerClaim { .nodeId = nodeId, .raftEndpoint = raftEndpoint, .role = fields->role, .publicKey = fields->publicKey };
    switch (_window.Offer(claim, peer))
    {
        case EnrollDecision::Closed:
            return Cc::Refuse(_metrics,
                              { .code = Wire::ErrorCode::EnrollmentClosed,
                                .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedClosed },
                              "no enrollment window is open on this node");
        case EnrollDecision::Full:
            return Cc::Refuse(
                _metrics,
                { .code = Wire::ErrorCode::EnrollmentFull, .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedFull },
                std::format("the enrollment window already holds {} request(s) and records no more", MaxPendingEnrollments));
        case EnrollDecision::Pending:
            // No roster, and the encoder is what makes that a length rather than a
            // convention -- see `EncodeEnrollReply`.
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Pending, {}));
        case EnrollDecision::Rejected:
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Rejected, {}));
        case EnrollDecision::Approved:
            break;
    }

    // **`Approved` means the cluster RECORDS this joiner, not that a person typed approve.**
    // `ClusterAdmit` returns once the command is appended, and the joiner polls every two
    // seconds; answering before the leader's own state holds the joiner would hand it a roster
    // that lacks its own entry, which is exactly what the joiner is told to refuse. So until
    // the change is applied here, an approved row answers `Pending` -- the one outcome that
    // says "not admitted yet" and carries nothing.
    auto const state = _scheduler.AdministeredState();
    if (!state.has_value())
        return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Pending, {}));
    auto const projected = Cluster::ProjectRoster(*state);
    if (!RosterRecordsJoiner(projected, nodeId, fields->publicKey, fields->role))
        return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Pending, {}));

    // The roster, and no secret: it is every member's PUBLIC key, and nothing in it lets its
    // holder prove anything (#178). The fingerprint is taken over these bytes and recorded
    // against the row, so the operator reads what was SENT beside what the joiner RECEIVED.
    auto const roster = Cluster::EncodeRoster(projected);
    _window.NoteServed(nodeId, Cluster::DigestOfRoster(roster));
    _metrics.Increment(IMetricsSink::Counter::EnrollmentRostersServed);
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Approved, roster));
}

std::vector<std::byte> EnrollmentResponder::AnswerControl(std::span<std::byte const> payload, PeerIdentity const& peer)
{
    auto const fields = Wire::DecodeEnrollControlPayload(payload);
    if (!fields.has_value())
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::MalformedFrame,
                                          .rationale = "a member sent a control frame this build cannot read, which "
                                                       "is a version mismatch between two machines one operator "
                                                       "installed; the enrollment series is read for strangers at an "
                                                       "open window and this is not one" },
                                        "an enroll-control request names a verb, and a subject exactly when the verb "
                                        "takes one");

    // Leadership, for every verb including the read: a follower's window is one nobody
    // can act on, so listing it would show an operator an empty list on the machine
    // that was never going to hold the rows.
    if (_scheduler.Role() != Distributed::SchedulerRole::Leader)
        return Cc::RefuseWithoutCounter(NotLeaderRefusal, _scheduler.LeaderEndpoint());

    switch (fields->verb)
    {
        case Wire::EnrollControlVerb::Open: {
            auto const outcome = _window.Open();
            if (outcome == EnrollControlOutcome::AlreadyInForce)
                return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::ClusterChangeNotNeeded,
                                                  .rationale = "an operator ran a verb twice; idempotence is not an "
                                                               "event and counting it would report one rollout as "
                                                               "several" },
                                                "the enrollment window is already open");
            _metrics.Increment(IMetricsSink::Counter::EnrollmentWindowsOpened);
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        }
        case Wire::EnrollControlVerb::Close: {
            auto const outcome = _window.Close();
            if (outcome == EnrollControlOutcome::AlreadyInForce)
                return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::ClusterChangeNotNeeded,
                                                  .rationale = "an operator ran a verb twice; idempotence is not an "
                                                               "event" },
                                                "the enrollment window is already closed");
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        }
        case Wire::EnrollControlVerb::List:
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        case Wire::EnrollControlVerb::Approve:
        case Wire::EnrollControlVerb::Reject:
            return AnswerDecision(fields->verb, Wire::AsStringView(fields->subject), peer);
    }

    // Unreachable: `DecodeEnrollControlPayload` refuses a verb byte this build cannot
    // name, so every value reaching here is one of the five above.
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::MalformedFrame,
                                      .rationale = "unreachable: the decoder refuses a verb this build cannot name" },
                                    "unknown enroll-control verb");
}

std::vector<std::byte> EnrollmentResponder::AnswerDecision(Wire::EnrollControlVerb verb,
                                                           std::string_view subject,
                                                           PeerIdentity const& peer)
{
    auto const entry = _window.Find(subject);
    if (!entry.has_value())
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::InvalidClusterChange,
                                          .rationale = "an operator named an id that is not waiting -- a typo, or a "
                                                       "row that went when somebody closed the window; both are read "
                                                       "off the reply and neither is a fleet event" },
                                        std::format("no machine named {} is waiting to enrol", subject));

    auto const decided =
        verb == Wire::EnrollControlVerb::Approve ? Wire::EnrollmentDecision::Approved : Wire::EnrollmentDecision::Rejected;

    // Already-settled is answered BEFORE the cluster is asked anything, because
    // `ClusterAdmit` is idempotent and would answer `ClusterChangeNotNeeded` with a
    // sentence about the cluster's member record. What an operator deciding twice needs
    // told is that this WINDOW has already decided. A joiner whose reply was lost needs no
    // second approval: it polls again and is answered the same roster (#178).
    if (entry->decision == decided)
        return Cc::RefuseWithoutCounter(
            { .code = Wire::ErrorCode::ClusterChangeNotNeeded,
              .rationale = "an operator decided the same way twice; idempotence is not an event" },
            std::format(
                "{} is already {}", subject, decided == Wire::EnrollmentDecision::Approved ? "approved" : "rejected"));

    if (verb == Wire::EnrollControlVerb::Approve)
    {
        // The cluster is asked FIRST and the window is marked only once it agreed.
        //
        // The other order is the tempting one and is worse: a window marked approved while
        // the change was refused would answer a joiner consensus has never heard of, which
        // `--cluster-status` cannot find and `--cluster-forget` cannot remove. This way round
        // the failure is a machine the cluster records that never polled again -- visible,
        // and removable by name.
        //
        // Through the one `SchedulerService` entry point each role has -- `ClusterAdmit` for a
        // member, which `--cluster-admit` reaches, and `AdmitPrincipal` for a worker: one gate,
        // one validation, one mapping from a consensus refusal onto a wire code.
        //
        // UNDER THE KEY THE ROW HOLDS (#178), which is the key the operator was shown: the
        // first key this id asked with, never refreshed. A member with no opinion about its
        // seat (#1449) keeps whatever it holds, so an approval cannot promote a machine the
        // operator demoted to a learner.
        auto const& role = EnrollRoleRowFor(entry->role);
        auto const reply =
            role.principal.has_value()
                ? _scheduler.AdmitPrincipal(Context(peer), subject, entry->publicKey, *role.principal)
                : _scheduler.ClusterAdmit(
                      Context(peer), subject, entry->raftEndpoint, FormatEd25519PublicKey(entry->publicKey), std::nullopt);

        // **Already recorded exactly this way is `Satisfied`, not a failure**: the machine was
        // admitted by another route -- an operator's `--cluster-admit` naming this key -- and
        // approving it here changes nothing but what this window answers. With no secret at
        // stake, answering it the roster on somebody else's decision hands out nothing that
        // decision did not already make public to every member.
        auto const satisfied = reply.error == Wire::ErrorCode::ClusterChangeNotNeeded;
        if (reply.status != Wire::Status::Ok && !satisfied)
            return Cc::RefuseWithoutCounter({ .code = reply.error,
                                              .rationale = "counted at the decision, in SchedulerService::Refuse, "
                                                           "which triages every code it can produce" },
                                            reply.message);
    }

    if (auto const outcome = _window.Decide(subject, decided); outcome != EnrollControlOutcome::Done)
        // The window moved under this decision: somebody closed it, or the row went.
        // Reported rather than retried, because a retry would race the same way and an
        // operator re-reading the list is the one action that settles it.
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::InvalidClusterChange,
                                          .rationale = "the window changed between the read and the decision, which "
                                                       "one operator re-reading the list settles; it says nothing "
                                                       "about the fleet" },
                                        std::format("the enrollment window changed while {} was being decided; "
                                                    "read the list again",
                                                    subject));

    // **Rejecting a row that was already ADMITTED does not un-admit it, and that has to
    // be said out loud.**
    //
    // `Reject` never reaches `ClusterAdmit` -- correctly, since rejecting a `Pending` row
    // commits nothing -- but `EnrollmentWindow::Decide` permits `Approved -> Rejected`,
    // which is the path an operator correcting a mis-approval takes. The membership change
    // was committed by the approval, so the reject stops the ROSTER being handed over (which
    // is worth having, and is why this is not refused outright) and leaves the machine in
    // `ClusterState` -- a member counted towards quorum, or a principal holding its key.
    // `--enroll-reject`'s own help text promised the opposite -- *"a machine refused here was
    // never a member and needs no --cluster-forget"* -- which is true only of a row that was
    // still pending; the flag's description now says so, and this is the record for the node
    // that actually did it.
    //
    // Warn rather than a counter: it is one machine, one operator, one remedy, and a
    // tally of it would answer a question nobody asks of a fleet. Named here rather than
    // in the reply because the success reply is a structured report with no sentence in
    // it, and widening the wire for one advisory line would be the expensive way to say
    // this.
    //
    // One remedy for every role, because one verb removes either (#1555): `--cluster-forget`
    // takes the id out of whichever list records it and revokes the key it was admitted under.
    if (decided == Wire::EnrollmentDecision::Rejected && entry->decision == Wire::EnrollmentDecision::Approved)
        _logger.Logf(LogLevel::Warn,
                     "enrollment: {} was rejected after it had already been approved, so it will not be handed the "
                     "roster -- but the approval had already committed it to the cluster as a {}, and this did NOT "
                     "remove it. It is still recorded, under the key it asked with: run --cluster-forget={} if that "
                     "is what you meant, which also revokes that key.",
                     subject,
                     EnrollRoleRowFor(entry->role).name,
                     subject);

    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
}

} // namespace FastCache::Node
