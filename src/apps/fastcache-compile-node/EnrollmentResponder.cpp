// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"
#include "EnrollmentResponder.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

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

std::expected<SecureByteBuffer, std::string> FileClusterKeySource::ClusterKey() const
{
    if (_path.empty())
        return std::unexpected { std::string { "this node holds no --cluster-key-file, so it has no key to hand over" } };
    return ReadClusterKey(_path);
}

Task<std::vector<std::byte>> EnrollmentResponder::Answer(std::span<std::byte const> frame, std::string peer)
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
            co_return AnswerEnroll(payload, peer);
        case Wire::Op::EnrollControl:
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

std::optional<std::vector<std::byte>> EnrollmentResponder::RefusePeer(std::string_view peer, std::uint8_t opRaw) const
{
    // `Enroll` admits everybody, which is this surface's one open door and is the whole
    // point of it. See the declaration.
    if (static_cast<Wire::Op>(opRaw) == Wire::Op::Enroll)
        return std::nullopt;

    if (_membership.Classify(peer) == Distributed::Membership::Member)
        return std::nullopt;

    return Cc::Refuse(
        _metrics,
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
                          "an enroll request names an id and a consensus endpoint");

    auto const nodeId = Wire::AsStringView(fields->nodeId);
    auto const raftEndpoint = Wire::AsStringView(fields->raftEndpoint);

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

    // An empty id or endpoint is refused here rather than at the approval, because the
    // list is what a PERSON reads and a blank row is one they cannot act on. It is the
    // same rule `Cluster::ClusterMember` exists to make unrepresentable, asked one layer
    // earlier so the row never reaches the operator at all.
    if (nodeId.empty() || raftEndpoint.empty())
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedMalformed },
                          "an enroll request names both an id and a consensus endpoint, and neither may be empty");

    auto const decision = _window.Offer(nodeId, raftEndpoint, peer);
    switch (decision)
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
            // No key bytes, and the encoder is what makes that a length rather than a
            // convention -- see `EncodeEnrollReply`.
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Pending, {}));
        case EnrollDecision::Rejected:
            return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Rejected, {}));
        case EnrollDecision::Collected:
            // **The spend.** This returns before `_key.ClusterKey()` is ever reached, so
            // there are no key bytes in this frame to withhold -- which is the property
            // worth having rather than a decision not to serve them, and the reason the
            // arm is here and not a `firstHandOver`-style flag further down. While the
            // window answered `Approved` on every poll and gated only the counter, a
            // second hand-over was served and was invisible: the tally stayed at one and
            // the real joiner still got its key on its next poll.
            return Cc::Refuse(_metrics,
                              { .code = Wire::ErrorCode::EnrollmentAlreadyCollected,
                                .counter = IMetricsSink::Counter::EnrollmentRequestsRefusedAlreadyCollected },
                              "the cluster key for this id has already been collected; if that was not this machine, "
                              "ask an operator to approve it again");
        case EnrollDecision::Approved:
            break;
    }

    auto key = _key.ClusterKey();
    if (!key.has_value())
    {
        // **The claim goes back.** `Offer` took the one collection this id has before
        // this read could be attempted, and it had to -- taking it afterwards would let
        // two polls both be served. So a fault here must not be paid for by the joiner:
        // a filesystem permission error lasting one second would otherwise leave a
        // machine that can never collect, with nothing in its refusal saying why.
        //
        // **And the answer is READ.** This used to be `(void)`, which made it a claim
        // with no reader -- and the value nobody looked at is the one that matters: any
        // outcome but `Returned` means this repair failed, so the machine is holding a
        // consumed collection after all. That is the exact state the repair exists to
        // prevent, reached through the repair itself, and silent. It is per node and
        // per machine, so the log is where it belongs rather than a counter.
        switch (_window.ReturnClaim(nodeId))
        {
            case ClaimReturn::Returned:
                break;
            case ClaimReturn::NoSuchRow:
                _logger.Logf(LogLevel::Warn,
                             "enrollment: could not read this node's key file for {}, and its row is gone -- the "
                             "window was closed in between. That machine's collection was consumed: open the window "
                             "and approve it again once the key file is readable.",
                             nodeId);
                break;
            case ClaimReturn::AlreadyMoved:
                _logger.Logf(LogLevel::Warn,
                             "enrollment: could not read this node's key file for {}, and its row was decided about "
                             "in between, so the collection it had was consumed and NOT given back. Approve {} again "
                             "once the key file is readable, which re-arms exactly one more collection.",
                             nodeId,
                             nodeId);
                break;
        }
        // `StorageWriteFailed` rather than a refusal about enrollment, because that is
        // what happened: the decision was taken and this node could not read its own
        // key file. Telling the joiner the window was closed would send an operator to
        // re-open a window that is open.
        return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCode::StorageWriteFailed,
                                          .rationale = "a node that cannot read its own key file has one fault with "
                                                       "one remedy, and it is already in this node's log; a counter "
                                                       "here would be a second tally of a condition an operator "
                                                       "meets the moment they look" },
                                        key.error());
    }

    // Unconditional, because `EnrollDecision::Approved` is reachable exactly once per
    // approval: the window takes the `Approved` -> `Collected` transition under its own
    // lock on the call that produces it, and a failed key read gives that claim back
    // above rather than spending it. The flag this used to be gated on is gone, and its
    // absence is the fix -- a bool whose only job was to keep a counter honest about a
    // hand-over that was happening anyway described the problem instead of preventing it.
    //
    // It rises again when an operator RE-APPROVES a collected row, and that is the
    // honest reading: the key genuinely goes out a second time, so a second event is
    // what the tally should show.
    _metrics.Increment(IMetricsSink::Counter::EnrollmentKeysHandedOver);

    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollReply(Wire::EnrollOutcome::Approved, *key));
}

std::vector<std::byte> EnrollmentResponder::AnswerControl(std::span<std::byte const> payload, std::string_view peer)
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
                                                           std::string_view peer)
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
    // told is that this WINDOW has already decided.
    //
    // **`Collected` is NOT folded into `Approved` here**, and that is the recovery path
    // rather than a loosening. Approving a collected row re-arms exactly one more
    // collection, which is how a joiner whose reply was lost gets un-stranded -- one
    // operator command, on the id they already approved. While the two were folded, this
    // arm answered *already in force* and the only instruction it offered was
    // `--cluster-forget`: a quorum change to recover from a dropped packet, followed by
    // a re-approve that consensus then refuses because the member is already in
    // `ClusterState`. Four commands, two of them counter-intuitive, none of them
    // documented. A remedy that expensive is one an operator works around.
    if (entry->decision == decided)
        return Cc::RefuseWithoutCounter(
            { .code = Wire::ErrorCode::ClusterChangeNotNeeded,
              .rationale = "an operator decided the same way twice; idempotence is not an event" },
            std::format(
                "{} is already {}", subject, decided == Wire::EnrollmentDecision::Approved ? "approved" : "rejected"));

    if (verb == Wire::EnrollControlVerb::Approve)
    {
        // **Can this node actually hand a key over? Asked BEFORE the cluster is told
        // anything, because `ClusterAdmit` is the irreversible half.**
        //
        // This does NOT reorder the window-against-cluster argument below -- it is a
        // pure read that marks nothing, and it sits above both. What it closes is a
        // third order nobody had considered: the key was consulted only on the joiner's
        // next poll, in `AnswerEnroll`, so a node whose key file was named but
        // unreadable committed the membership change, answered the operator `Ok`, and
        // then served that joiner `StorageWriteFailed` forever. The cluster had
        // permanently gained a member that could never collect -- a phantom counted
        // towards every future election, from a command that reported success.
        //
        // Refused at the DECISION, so the operator who typed `--enroll-approve` is the
        // one who learns the key file is broken, which is the only moment anybody is
        // watching. `main` also refuses to serve this family at all when
        // `--cluster-key-file` is unset; that answers the structural case, and this
        // answers the one a configuration cannot see, which is a file that is named and
        // cannot be read.
        //
        // The bytes are DROPPED immediately -- this asks a question and does not carry
        // an answer -- because an outbound credential is read where it is presented and
        // this is not that place. The hand-over re-reads it in `AnswerEnroll`.
        if (auto const readable = _key.ClusterKey(); !readable.has_value())
            return Cc::RefuseWithoutCounter(
                { .code = Wire::ErrorCode::StorageWriteFailed,
                  .rationale = "one fault with one remedy, already in this node's log and in the operator's own "
                               "reply; the counter for it would be a second tally of a condition the person who "
                               "typed the command is looking straight at" },
                std::format("this node cannot read the cluster key it would have to hand over, so {} was NOT admitted "
                            "and nothing was committed to the cluster: {}",
                            subject,
                            readable.error()));

        // The cluster is asked FIRST and the window is marked only once it agreed.
        //
        // The other order is the tempting one and is worse: a window marked approved
        // while the membership change was refused hands this cluster's key to a machine
        // consensus has never heard of, which cannot then be found by
        // `--cluster-status` and cannot be removed by `--cluster-forget`. This way
        // round the failure is a member the cluster knows about that never collected a
        // key -- visible in `--cluster-status`, removable by name, and unable to serve
        // anything in the meantime.
        //
        // Through `SchedulerService::ClusterAdmit`, which is the same entry point
        // `--cluster-admit` reaches: one gate, one validation, one mapping from a
        // consensus refusal onto a wire code.
        auto const reply = _scheduler.ClusterAdmit(Context(std::string { peer }), subject, entry->raftEndpoint);

        // **A RE-APPROVAL reaches a cluster that already holds this member, and that is
        // a `Satisfied` refusal rather than a failure.** The consensus rulebook draws
        // exactly this distinction -- *already in force* is a record to go and correct,
        // not a command that can never be recorded -- and this is the shape it was drawn
        // for: the operator is not adding a member, they are re-arming one collection
        // for a machine consensus already knows about. Reading it as a refusal is what
        // would leave the joiner polling `Pending` forever after a recovery that looked
        // like it worked.
        //
        // Only for a row that HAS collected: an `Approved`-but-uncollected row meeting
        // this answer means the member was admitted by another route, and letting that
        // through silently would hand the key out on the strength of somebody else's
        // decision.
        //
        // **A re-approval also CLEARS this member's announced scheduler endpoint**, and
        // that is left alone here deliberately. `ClusterAdmit` sends an empty one on
        // purpose -- a node that MOVED has moved both ports -- and `AddMember` applies
        // wholesale, so recovery, which is not a move, clears a live member's endpoint
        // until it next wins an election and re-announces. Preserving it at this call
        // site would break the move case the verb exists for, so the residual is the
        // REPORTING one: neither renderer can say whether an absent endpoint was never
        // announced or was cleared. That is #1340, and this path is what took it from
        // latent to reachable by an ordinary operator action.
        auto const reArming = entry->decision == Wire::EnrollmentDecision::Collected;
        auto const satisfied = reply.error == Wire::ErrorCode::ClusterChangeNotNeeded;
        if (reply.status != Wire::Status::Ok && !(reArming && satisfied))
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
    // commits nothing -- but `EnrollmentWindow::Decide` permits `Approved -> Rejected`
    // and `Collected -> Rejected`, which is the path an operator correcting a
    // mis-approval takes. The membership change was committed by the approval, so the
    // reject stops the key HAND-OVER (which is worth having, and is why this is not
    // refused outright) and leaves the machine in `ClusterState`, counted towards
    // quorum. `--enroll-reject`'s own help text promised the opposite -- *"a machine
    // refused here was never a member and needs no --cluster-forget"* -- which is true
    // only of a row that was still pending; the flag's description now says so, and this
    // is the record for the node that actually did it.
    //
    // Warn rather than a counter: it is one machine, one operator, one remedy, and a
    // tally of it would answer a question nobody asks of a fleet. Named here rather than
    // in the reply because the success reply is a structured report with no sentence in
    // it, and widening the wire for one advisory line would be the expensive way to say
    // this.
    if (decided == Wire::EnrollmentDecision::Rejected
        && (entry->decision == Wire::EnrollmentDecision::Approved || entry->decision == Wire::EnrollmentDecision::Collected))
        _logger.Logf(LogLevel::Warn,
                     "enrollment: {} was rejected after it had already been {}, so it will not be handed the cluster "
                     "key -- but the approval had already committed it to the cluster, and this did NOT remove it. It "
                     "is still a member counted towards quorum: run --cluster-forget={} if that is what you meant.",
                     subject,
                     entry->decision == Wire::EnrollmentDecision::Collected ? "given the key" : "approved",
                     subject);

    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeEnrollmentReport(_window.Report()));
}

} // namespace FastCache::Node
