// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentResponder.hpp"
#include "MembershipGate.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

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
        "the enrollment series it would bury the two refusals that mean somebody is trying to join uninvited";

    /// Why the endpoint's byte budget is not this surface's to count.
    constexpr std::string_view ByteBudgetRationale =
        "the byte budget says this surface is momentarily full, which the peer sees and retries; summed into a "
        "series read as somebody probing the enrollment list it is what makes that series unreadable";

    /// One row per `EndpointRefusal`: what this surface does about it.
    struct EnrollmentEndpointRefusal
    {
        EndpointRefusal refusal;                  ///< Which endpoint decision this describes.
        std::optional<Cc::SurfaceRefusal> answer; ///< The row, or nothing where this surface counts none.
        std::string_view rationale;               ///< Why nothing is counted; read only when `answer` is absent.
    };

    /// This surface counts none of the endpoint's own refusals.
    ///
    /// **And that is five separate claims rather than one shrug.** The byte budget and
    /// the answer deadline are uncounted for reasons every surface here shares; the two
    /// credential rows are uncounted because `AUTH` is the session component's, which
    /// checks it and counts it -- a second tally here would be one AUTH failure reported
    /// twice to whoever met both series.
    ///
    /// The counted refusals on this surface are all decided INSIDE `Answer`, where the
    /// verb and the window's state are both known, which is why this table has no
    /// counted row and that is not an omission.
    constexpr EnumTable<EndpointRefusal, EnrollmentEndpointRefusal> EnrollmentEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget, .answer = std::nullopt, .rationale = ByteBudgetRationale },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSessionsRationale },
        { .refusal = EndpointRefusal::CredentialRejected,
          .answer = std::nullopt,
          .rationale = CredentialIsTheSessionsRationale },
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

    /// An operator's control verb from a caller only `--fleet-open` admitted: the enrollment surface's
    /// own row for `Distributed::IdentityRequirements`' refusal, so the rule is the table's and the
    /// counter this surface's.
    constexpr Cc::SurfaceRefusal ControlUnidentified {
        .code = Wire::ErrorCode::IdentifiedCallerRequired,
        .counter = IMetricsSink::Counter::EnrollmentControlRefusedIdentifiedCallerRequired,
    };

    static_assert(Distributed::RequirementRowOf(CompileCacheWire::IdentityRequirement::IdentifiedCaller).refusal
                      == ControlUnidentified.code,
                  "the enrollment surface refuses an unidentified caller under the code the requirement names");

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

core::async::Task<FrameReply> EnrollmentResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
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

    if (auto refusal = RefuseUnlessMember(
            _membership,
            _metrics,
            peer,
            { .code = Wire::ErrorCode::NotAMember, .counter = IMetricsSink::Counter::EnrollmentControlRefusedNotAMember },
            "deciding who joins this cluster is a member's verb");
        refusal.has_value())
        return refusal;
    return RefuseUnidentified(peer, opRaw);
}

std::optional<std::vector<std::byte>> EnrollmentResponder::RefuseUnidentified(PeerIdentity const& peer,
                                                                              std::uint8_t opRaw) const
{
    // Asked of a caller the surface ADMITS, as the scheduler's `RefuseUnlessIdentified` is: one it
    // does not is membership's refusal, and a stranger must be told what the door tells it.
    auto const* const descriptor = Wire::FindOp(opRaw);
    auto const caller = Context(peer);
    if (descriptor == nullptr || caller.membership != Distributed::Membership::Member)
        return std::nullopt;
    auto const& row = Distributed::RequirementRowOf(descriptor->identity);
    if (row.satisfiedBy(caller))
        return std::nullopt;
    return Cc::Refuse(_metrics, ControlUnidentified, std::format("{} {}", descriptor->name, row.remedy));
}

std::vector<std::byte> EnrollmentResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                         std::uint8_t /*opRaw*/,
                                                         std::string_view detail) const
{
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision),
                                      .rationale = decision == Wire::PrePayloadDecision::Unauthenticated
                                                       ? NodeChecksNoPasswordRationale
                                                       : ShapeRefusalRationale },
                                    detail);
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
                          "an enroll request names an id, an endpoint, a role this build knows, a 32-byte "
                          "identity key and a 32-byte nonce");

    // A machine joins ONE way, as a learner. A role the wire still decodes and no row names is
    // refused here, before anything is recorded, so no later step reads a row for it.
    if (!ServesEnrollRole(fields->role))
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          "a machine enrolls as a learner; no other role is admitted");

    auto const nodeId = Wire::AsStringView(fields->nodeId);
    auto const nodeEndpoint = Wire::AsStringView(fields->nodeEndpoint);
    auto const& role = EnrollRoleRowFor(fields->role);

    // Text, or the fleet refuses it: this id is copied into `ClusterState` on approval
    // and read back out of `/fleet.json` by everybody, so one byte that is not UTF-8
    // makes that document unparseable for the whole fleet. Refused where it ENTERS,
    // which is here -- a consensus entry is applied after it is committed, with nobody
    // left to refuse it. The endpoint travels the same way and is checked the same way.
    if (!IsValidUtf8(nodeId) || !IsValidUtf8(nodeEndpoint))
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          "a joiner must name itself and its endpoint in UTF-8");

    // Held to the one id bound where it ENTERS, as every id this fleet carries is. Past it, the
    // row would be one `--enroll-reject` refuses to name -- it takes an id through that same
    // bound -- so only `--enroll-clear`, which drops every honest row with it, could remove it.
    if (nodeId.size() > Wire::MaxIdBytes)
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedIdTooLong },
                          std::format("a joiner's id is at most {} bytes, as every id this fleet carries is; this one "
                                      "is {}",
                                      Wire::MaxIdBytes,
                                      nodeId.size()));

    // An empty id, or an endpoint that does not suit the role, is refused here rather than at
    // the approval, because the list is what a PERSON reads and a row they cannot act on is
    // one they should never be shown. Neither live role states one, so an endpoint here is a
    // claim the record it becomes has nowhere to keep.
    if (nodeId.empty() || role.statesEndpoint == nodeEndpoint.empty())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          std::format("an enroll request names an id, and a {} {} an endpoint",
                                      role.name,
                                      role.statesEndpoint ? "must name" : "names no"));

    // A key no signature can PROVE anything under -- a small-order point, or a non-canonical
    // spelling of one -- is refused at the door too, before the window records a row an operator
    // might approve: approving it would admit a key anybody can forge for. No build of this
    // software mints one (`Ed25519KeyPair::FromSeed`), so this is the malformed row: the request
    // came from no version of this client.
    if (auto const fault = Ed25519PublicKeyFaultOf(fields->publicKey); fault.has_value())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          std::format("{} cannot be enrolled: {}",
                                      FormatEd25519PublicKey(fields->publicKey),
                                      DescribePublicKeyFault(*fault)));

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

    // **A machine the cluster already records under exactly this key and role, and this list holds
    // no row for, is answered the roster.** The admission was decided and replicated; the list is
    // one leader's memory, forgotten at a restart or a change of leader. A row this list DOES hold
    // decides as it always has, so a rejection still stops the roster being handed over. Without this, a joiner
    // admitted just before the leader changed would be recorded afresh here, and its approval
    // refused as *already a member* -- a machine the cluster admitted, polling forever. Nothing
    // is handed out that the approval did not already make every member's: a roster is public
    // keys (#178).
    auto const recorded = [&] {
        if (_window.Find(nodeId).has_value())
            return false;
        auto const state = _scheduler.AdministeredState();
        return state.has_value()
               && RosterRecordsJoiner(Cluster::ProjectRoster(*state), nodeId, fields->publicKey, fields->role);
    }();

    auto const claim =
        JoinerClaim { .nodeId = nodeId, .nodeEndpoint = nodeEndpoint, .role = fields->role, .publicKey = fields->publicKey };
    // **Every answer is signed, over the joiner's own nonce, by this node's identity key** -- a
    // refusal and a "not yet" as well as an approval. The roster is public, so nothing in an answer's
    // content can tell it from a copy anybody at the polled endpoint could make; and a refusal sends
    // a joiner away for an hour while a "not yet" keeps it waiting on a join that may be dead, so an
    // unsigned one of either is a lever for whoever answers there. The joiner holds each to a key it
    // proved for this cluster. The cluster id is the one this node's own summary states, the summary
    // a joiner's probe proves the key with.
    //
    // **So every `Enroll` answered past the list's bounds costs one Ed25519 signature, to anybody**:
    // the verb is pre-auth, and nothing but its payload cap and the header window bounds who asks.
    // That is the work a stranger buys per request here -- small, and the reason a full list and a
    // capped host are refused BEFORE this lambda runs and sign nothing.
    auto const answer = [&](Wire::EnrollOutcome outcome,
                            std::span<std::byte const> roster = {},
                            std::span<std::byte const> certificate = {}) {
        auto const signature = Cluster::SignAdmission(_identity,
                                                      Cluster::AdmissionClaim { .nonce = fields->nonce,
                                                                                .joinerId = nodeId,
                                                                                .joinerKey = fields->publicKey,
                                                                                .clusterId = _self.Current().clusterId,
                                                                                .outcome = outcome,
                                                                                .roster = roster });
        return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(outcome, roster, certificate, signature));
    };
    switch (recorded ? EnrollDecision::Approved : _window.Offer(claim, peer))
    {
        case EnrollDecision::Full:
            return Cc::Refuse(
                _metrics,
                { .code = Wire::ErrorCode::EnrollmentFull, .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedFull },
                std::format("the enrollment list already holds {} request(s) and records no more", MaxPendingEnrollments));
        case EnrollDecision::HostFull:
            // The host in the spelling the bound counts and the list shows (`first from`), which is
            // not necessarily where those rows' machines ask from NOW: a refusal naming rows the
            // operator cannot find on the list is a claim nobody can check.
            return Cc::Refuse(_metrics,
                              { .code = Wire::ErrorCode::EnrollmentHostFull,
                                .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedHostCap },
                              std::format("{} already has {} undecided request(s) on the enrollment list, as many as "
                                          "one host may; they are counted by the address each FIRST asked from, which "
                                          "--enroll-list shows as 'first from {}'. This one is recorded once one of "
                                          "those is decided",
                                          UnmappedHost(peer),
                                          MaxPendingEnrollmentsPerHost,
                                          UnmappedHost(peer)));
        case EnrollDecision::Pending:
            // No roster, and the encoder is what makes that a length rather than a
            // convention -- see `EncodeEnrollReply`.
            return answer(Wire::EnrollOutcome::Pending);
        case EnrollDecision::Rejected:
            return answer(Wire::EnrollOutcome::Rejected);
        case EnrollDecision::AutoApprove:
            return AnswerAutoApprove(nodeId, answer(Wire::EnrollOutcome::Pending));
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
        return answer(Wire::EnrollOutcome::Pending);
    auto const projected = Cluster::ProjectRoster(*state);
    if (!RosterRecordsJoiner(projected, nodeId, fields->publicKey, fields->role))
        return answer(Wire::EnrollOutcome::Pending);

    // The roster, and no secret: it is every member's PUBLIC key, and nothing in it lets its
    // holder prove anything (#178). The fingerprint is taken over these bytes and recorded
    // against the row, so the operator reads what was SENT beside what the joiner RECEIVED.
    auto const roster = Cluster::EncodeRoster(projected);
    _window.NoteServed(nodeId, Cluster::DigestOfRoster(roster));
    _metrics.Increment(IMetricsSink::Counter::EnrollmentRostersServed);

    // And the roster a majority of the voters has CERTIFIED, when there is one, because the
    // approved reply still has the field. No joiner keeps it any more: a learner applies its
    // fleet's state, which needs no certificate. Empty until the voters have endorsed one.
    auto const certificate =
        _scheduler.CurrentCertifiedRoster()
            .transform([](Cluster::CertifiedRoster const& certified) { return Cluster::EncodeCertifiedRoster(certified); })
            .value_or(std::vector<std::byte> {});

    return answer(Wire::EnrollOutcome::Approved, roster, certificate);
}

std::vector<std::byte> EnrollmentResponder::AnswerClear(PeerIdentity const& peer)
{
    // Counted and SAID, because it forgets machines nobody decided about on one operator's word:
    // the log is where the ids are, and the counter is how a dashboard sees it happened.
    auto const cleared = _window.ClearPending();
    _metrics.Increment(IMetricsSink::Counter::EnrollmentRequestsCleared, cleared.size());
    auto ids = std::string {};
    for (auto const& id: cleared)
        ids += std::format("{}{}", ids.empty() ? "" : ", ", id);
    _logger.Logf(LogLevel::Warn,
                 "enrollment: an operator at {} cleared {} request(s) nobody had decided about{}{}; approved and "
                 "rejected rows are kept, and a machine still asking is recorded again at its next poll",
                 peer.host,
                 cleared.size(),
                 cleared.empty() ? "" : ": ",
                 ids);
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
}

std::vector<std::byte> EnrollmentResponder::AnswerAutoApprove(std::string_view nodeId, std::vector<std::byte> const& pending)
{
    // Pending, whatever happens here: the roster is not applied yet, so the joiner's next poll
    // is the one answered `Approved` -- the ordering a manual approval already has.
    auto const entry = _window.Find(nodeId);
    if (!entry.has_value())
        return pending;

    // An id the cluster already seats is not a newcomer, and a deadline admits newcomers: the
    // same rule as a person's approval, and it stays waiting for a person to see it.
    if (auto const seat = RecordedSeatOf(nodeId); seat.has_value())
    {
        _logger.Logf(LogLevel::Warn,
                     "enrollment: not auto-approving {}: it is already a member as {}, and an approval does not "
                     "change a member's seat",
                     nodeId,
                     *seat);
        return pending;
    }

    // On the LEADER's own authority: nobody asked on this connection but the joiner, and the
    // decision was the operator's when they armed the deadline.
    auto const reply = AdmitRow(*entry, Distributed::SelfCaller());
    auto const satisfied = reply.error == Wire::ErrorCode::ClusterChangeNotNeeded;
    if (reply.status != Wire::Status::Ok && !satisfied)
    {
        // Not decided, so it stays waiting and the next poll asks the deadline again: a
        // change the cluster could not take right now is taken on a later ask, or by a person.
        _logger.Logf(LogLevel::Warn, "enrollment: auto-approving {} was refused: {}", nodeId, reply.message);
        return pending;
    }

    if (_window.Decide(nodeId, Wire::EnrollmentDecision::Approved) == EnrollControlOutcome::Done)
    {
        _window.MarkAutoApproved(nodeId);
        _metrics.Increment(IMetricsSink::Counter::EnrollmentApprovalsAuto);
    }
    return pending;
}

std::vector<std::byte> EnrollmentResponder::AnswerControl(std::span<std::byte const> payload, PeerIdentity const& peer)
{
    // The door's whole question -- membership, then the verb's identity column -- asked again, for a
    // caller of `Answer` that never asked the door: a stranger is told `NotAMember` here as there.
    if (auto refusal = RefusePeer(peer, static_cast<std::uint8_t>(Wire::Op::EnrollControl)); refusal.has_value())
        return *std::move(refusal);

    auto const fields = Wire::DecodeEnrollControlPayload(payload);
    if (!fields.has_value())
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::MalformedFrame,
                                          .rationale = "a member sent a control frame this build cannot read, which "
                                                       "is a version mismatch between two machines one operator "
                                                       "installed; the enrollment series is read for strangers "
                                                       "asking to join and this is not one" },
                                        "an enroll-control request names a verb, and a subject exactly when the verb "
                                        "takes one");

    // Leadership, for every verb including the read: a follower's window is one nobody
    // can act on, so listing it would show an operator an empty list on the machine
    // that was never going to hold the rows.
    if (_scheduler.Role() != Distributed::SchedulerRole::Leader)
        return Cc::RefuseWithoutCounter(NotLeaderRefusal, _scheduler.LeaderEndpoint());

    switch (fields->verb)
    {
        case Wire::EnrollControlVerb::List:
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        case Wire::EnrollControlVerb::Approve:
        case Wire::EnrollControlVerb::Reject:
            return AnswerDecision(fields->verb, Wire::AsStringView(fields->subject), fields->key, peer);
        case Wire::EnrollControlVerb::AutoApprove: {
            // The decoder guarantees a duration on this verb; the RULES are judged here, from the
            // table the CLI judged it by, because a peer may send anything.
            auto const armed = _window.ArmAutoApprove(fields->duration.value_or(std::chrono::seconds::zero()));
            if (!armed.has_value())
                return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::InvalidClusterChange,
                                                  .rationale = "an operator's duration typo is read off the reply and "
                                                               "says nothing about the fleet" },
                                                AutoApproveSentence(armed.error()));
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        }
        case Wire::EnrollControlVerb::AutoApproveOff:
            // Answered with the report either way: ending a window that is already shut leaves
            // exactly what the operator wanted, and the report says so.
            (void) _window.DisarmAutoApprove();
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
        case Wire::EnrollControlVerb::Clear:
            return AnswerClear(peer);
    }

    // Unreachable: `DecodeEnrollControlPayload` refuses a verb byte this build cannot
    // name, so every value reaching here is one of the six above.
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::MalformedFrame,
                                      .rationale = "unreachable: the decoder refuses a verb this build cannot name" },
                                    "unknown enroll-control verb");
}

Distributed::SchedulerReply EnrollmentResponder::AdmitRow(Wire::EnrollmentPendingEntry const& entry,
                                                          Distributed::CallerContext const& caller)
{
    auto const& role = EnrollRoleRowFor(entry.role);
    return _scheduler.ClusterAdmit(
        caller, entry.nodeId, entry.nodeEndpoint, FormatEd25519PublicKey(entry.publicKey), role.seat);
}

std::optional<std::string_view> EnrollmentResponder::RecordedSeatOf(std::string_view subject) const
{
    auto const state = _scheduler.AdministeredState();
    if (!state.has_value())
        return std::nullopt;
    auto const* const member = core::findOrNull(state->members, subject, &Cluster::ClusterMember::id);
    if (member == nullptr)
        return std::nullopt;
    return Cluster::MemberSeatTable[static_cast<std::size_t>(member->seat)].name;
}

std::vector<std::byte> EnrollmentResponder::AnswerDecision(
    Wire::EnrollControlVerb verb,
    std::string_view subject,
    std::optional<std::array<std::byte, Wire::IdentityPublicKeyBytes>> const& key,
    PeerIdentity const& peer)
{
    auto const entry = _window.Find(subject);
    if (!entry.has_value())
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::InvalidClusterChange,
                                          .rationale = "an operator named an id that is not waiting -- a typo, or a "
                                                       "row forgotten after its machine stopped asking; both are read "
                                                       "off the reply and neither is a fleet event" },
                                        std::format("no machine named {} is waiting to enrol", subject));

    // **The key the operator named is the key the row holds, or nothing is admitted.** A row is
    // replaced when its machine stops asking and another asks under the same id, so an id alone
    // would admit a machine nobody compared. Counted, because the likeliest cause is exactly
    // that substitution; asked before anything else, so no answer about the row's state is
    // given for a key that is not the one it holds. The decoder guarantees a key on `Approve`.
    if (verb == Wire::EnrollControlVerb::Approve && key != std::optional { entry->publicKey })
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::InvalidClusterChange,
                            .counter = IMetricsSink::Counter::EnrollmentApprovalsRefusedKeyMismatch },
                          std::format("{} is waiting under key {}, not {}: the machine asking under that id now is "
                                      "not the one whose key was compared, so nothing was admitted; compare again "
                                      "with --enroll-list",
                                      subject,
                                      FormatEd25519PublicKey(entry->publicKey),
                                      key.has_value() ? FormatEd25519PublicKey(*key) : std::string { "no key" }));

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
        // **An approval admits a newcomer; it never changes a member's seat** (#1449). A
        // member that asks again under its own key is answered the roster without anybody
        // approving it, so the row an operator is approving here is one for an id the cluster
        // already seats -- and approving it must not read as demoting a voter to a learner, nor
        // as succeeding while changing nothing. Named, with the seat it holds.
        if (auto const seat = RecordedSeatOf(subject); seat.has_value())
            return Cc::RefuseWithoutCounter(
                { .code = Wire::ErrorCode::InvalidClusterChange,
                  .rationale = "an operator approved an id the cluster already seats; read off the reply, and "
                               "nothing about the fleet changed" },
                std::format("{} is already a member as {}; an approval does not change a member's seat", subject, *seat));

        // The cluster is asked FIRST and the window is marked only once it agreed.
        //
        // The other order is the tempting one and is worse: a window marked approved while
        // the change was refused would answer a joiner consensus has never heard of, which
        // `--cluster-status` cannot find and `--cluster-forget` cannot remove. This way round
        // the failure is a machine the cluster records that never polled again -- visible,
        // and removable by name.
        //
        // Through the one `SchedulerService` entry point a learner has -- `ClusterAdmit`, which
        // `--cluster-admit-learner` reaches: one gate, one validation, one mapping from a
        // consensus refusal onto a wire code.
        //
        // UNDER THE KEY THE ROW HOLDS (#178), which is the key the operator was shown: the
        // first key this id asked with, never refreshed.
        //
        // A learner is admitted in the LEARNER seat with no consensus endpoint -- it dials in,
        // and holds no vote until an operator promotes it.
        auto const reply = AdmitRow(*entry, Context(peer));

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
        // Once the cluster agreed, and not before: a refused approval admitted nobody.
        _metrics.Increment(IMetricsSink::Counter::EnrollmentApprovalsManual);
    }

    if (auto const outcome = _window.Decide(subject, decided); outcome != EnrollControlOutcome::Done)
        // The list moved under this decision: the row was forgotten between the read and now.
        // Reported rather than retried, because a retry would race the same way and an
        // operator re-reading the list is the one action that settles it.
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::InvalidClusterChange,
                                          .rationale = "the list changed between the read and the decision, which "
                                                       "one operator re-reading the list settles; it says nothing "
                                                       "about the fleet" },
                                        std::format("the enrollment list changed while {} was being decided; "
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
    // `ClusterState`, holding its key.
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
    // One remedy (#1555): `--cluster-forget` takes the id out of the cluster and revokes the key
    // it was admitted under.
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
