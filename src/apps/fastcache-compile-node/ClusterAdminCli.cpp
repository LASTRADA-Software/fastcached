// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "ClusterAdminCli.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <utility>

#include <core/async/SyncRun.hpp>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// How long to wait for the scheduler to answer.
    ///
    /// Generous, because an operator typed this and is watching: the alternative to
    /// waiting is telling them it failed when it had not, and a proposal that is in
    /// flight when the client gives up still commits.
    constexpr std::chrono::milliseconds DialTimeout { 10'000 };

    /// Column width for a member id in the rendered report.
    constexpr std::size_t IdColumn = 12;

    /// Column width for a receipt's labels, so the two values line up under each other.
    ///
    /// An operator compares these against a second screen, and two values that do not
    /// start at the same column are two values they have to find before they can compare
    /// them.
    constexpr std::size_t ReceiptLabelColumn = 20;

    /// A dash where a value is absent, so a column is never blank.
    ///
    /// An empty cell reads as "nothing was rendered here" while a dash reads as
    /// "this member has not said" -- which for a scheduler endpoint is the state of
    /// a member that has announced none, and not a fault.
    constexpr std::string_view Absent = "-";

    /// What a receipt says for an admission that stated no key (#178). Not `Absent`: the
    /// leader recorded no key BY THIS COMMAND, which keeps one already recorded, and a dash
    /// in a column of values reads as *this member holds none*.
    constexpr std::string_view NoKeyStated = "none stated (a key already recorded stays)";

    /// An admission request, with the key spelled back out as the text the wire carries.
    ///
    /// Through the one encoder, `FormatEd25519PublicKey`, rather than the text the operator
    /// typed: the leader parses whatever arrives, and what reached this process is the key
    /// `ParseMemberSpec` already read, so re-spelling it is the canonical form of the same
    /// 32 bytes. Absent stays disengaged all the way to the wire's zero-length field.
    /// @tparam op `Op::ClusterAdmit` or `Op::ClusterAdmitLearner`.
    /// @param request What the operator asked for.
    /// @return The framed request.
    template <Wire::Op op>
        requires(Wire::IsMemberAdmission(op))
    [[nodiscard]] std::vector<std::byte> EncodeAdmission(ClusterRequest const& request)
    {
        auto const keyText = request.publicKey.transform(FormatEd25519PublicKey);
        return Wire::EncodeClusterAdmit<op>(Wire::ClusterAdmitRequest {
            .memberId = request.key,
            .raftEndpoint = request.value,
            .publicKey = keyText.transform([](std::string const& text) { return std::string_view { text }; }) });
    }
} // namespace

std::vector<std::byte> EncodeClusterRequest(ClusterRequest const& request)
{
    switch (request.action)
    {
        case ClusterAction::None:
            return {};
        case ClusterAction::Status:
            return Wire::EncodeClusterStatus();
        case ClusterAction::Set:
            return Wire::EncodeClusterSet(Wire::ClusterSetRequest { .name = request.key, .value = request.value });
        case ClusterAction::Forget:
            return Wire::EncodeClusterForget(request.key);
        case ClusterAction::Admit:
            return EncodeAdmission<Wire::Op::ClusterAdmit>(request);
        case ClusterAction::AdmitLearner:
            return EncodeAdmission<Wire::Op::ClusterAdmitLearner>(request);
    }

    return {};
}

std::string RenderClusterState(Cluster::ClusterState const& state)
{
    auto out = std::string {};

    out += std::format("members ({}):\n", state.members.size());
    if (state.members.empty())
        // Said out loud rather than left as a blank section. An empty member set is
        // a real and alarming state -- a cluster that admits nobody -- and a report
        // that merely showed no rows would read as a rendering problem.
        out += "  (none)\n";
    for (auto const& member: state.members)
    {
        // An absent endpoint says WHY (#1340): a member that announced none and one a
        // record re-proposed with none cleared both carry none, and only the second had
        // one to lose. The
        // word comes from the table every member renderer spells it from.
        auto const scheduler = member.schedulerEndpoint.empty()
                                   ? std::format("{} ({})", Absent, Cluster::SchedulerEndpointStateName(member))
                                   : member.schedulerEndpoint;
        //
        // The seat is the RECORD (#1449) -- what the operator admitted the member as --
        // and consensus moves towards it one change at a time, so for a moment after an
        // admit it can lead what is counted. `--node-status` on the member says which
        // set it is counted in now.
        //
        // A learner dials in, so it may be recorded with no consensus endpoint: the dash,
        // never an empty `raft=`.
        //
        // The key WHOLE (#178), in the one spelling `--node-status` prints on the member, so
        // an operator compares two identical strings from two machines.
        out += std::format("  {:<{}} seat={} raft={} scheduler={} key={}\n",
                           member.id,
                           IdColumn,
                           Cluster::MemberSeatName(member.seat),
                           member.raftEndpoint.empty() ? std::string { Absent } : member.raftEndpoint,
                           scheduler,
                           FormatEd25519PublicKey(member.publicKey));
    }

    // Said out loud when empty, for the members' reason: an operator reading this after a
    // revocation needs "none" to be an answer rather than a section that failed to render. No
    // verb this build sends admits one; a state an older build wrote may still hold some, and
    // `--cluster-forget` removes each.
    out += std::format("principals ({}):\n", state.principals.size());
    if (state.principals.empty())
        out += "  (none)\n";
    for (auto const& principal: state.principals)
        out += std::format("  {:<{}} key={}\n", principal.id, IdColumn, FormatEd25519PublicKey(principal.publicKey));

    out += std::format("revoked keys ({}):\n", state.revokedKeys.size());
    if (state.revokedKeys.empty())
        out += "  (none)\n";
    for (auto const& revoked: state.revokedKeys)
        out += std::format("  {:<{}} key={}\n", revoked.id, IdColumn, FormatEd25519PublicKey(revoked.publicKey));

    out += std::format("settings ({}):\n", state.settings.size());
    if (state.settings.empty())
        out += "  (none)\n";
    for (auto const& setting: state.settings)
        out += std::format("  {:<{}} {}\n", setting.name, IdColumn, setting.value);

    // Every key this build knows, whether or not it is set, because the operator's
    // real question is usually "what CAN I set" -- and a report that listed only
    // what somebody had already set would answer it wrongly by omission.
    out += "known settings:\n";
    for (auto const& row: Cluster::SettingTable)
        out += std::format("  {:<{}} {}\n", row.name, IdColumn, row.summary);

    return out;
}

std::expected<std::string, std::string> InterpretClusterReply(ClusterAction action, std::span<std::byte const> reply)
{
    switch (action)
    {
        case ClusterAction::None:
            return std::unexpected { std::string { "no cluster request was made" } };

        case ClusterAction::Status: {
            auto const state = Cluster::DecodeState(reply);
            if (!state.has_value())
                // A leader running a build whose state format this one does not know.
                // Refused rather than rendered as an empty cluster, which is what a
                // partial read would look like and would be read as a fact -- and the
                // decoder's reason travels with it, so a version mismatch names both
                // versions rather than reading as damage.
                return std::unexpected { std::format("the leader's reply is in a format this build cannot read: {}",
                                                     state.error().context) };
            return RenderClusterState(*state);
        }

        case ClusterAction::Set:
        case ClusterAction::Forget:
            // Appended, not committed, and the wording says so: the leader cannot know
            // the difference until a majority answers, and claiming otherwise would be
            // the one thing a report like this must not do.
            return std::string { "accepted; the change is replicating\n" };

        case ClusterAction::Admit:
        case ClusterAction::AdmitLearner: {
            auto const receipt = Wire::DecodeClusterAdmitReceipt(reply);
            if (!receipt.has_value())
                // NOT *an older leader*, which is the tempting sentence and is wrong
                // here: `MinSupportedVersion` equals `CurrentVersion`, so a leader that
                // does not speak this version is refused by name at the header and
                // never reaches this arm. A body that arrives and will not read is a
                // damaged reply or a defect, and saying *upgrade something* would send
                // an operator to fix a machine that is fine.
                return std::unexpected { std::string {
                    "the leader took the request and answered with a receipt this build cannot read" } };

            // **Recorded, as received -- and about what happens next, nothing stronger
            // than APPENDED.** The distinction is the whole of #1296: what the leader
            // wrote into the command it knows instantly and alone, while whether a
            // majority has taken it it cannot know at all. An echo that reads as the
            // second when it is the first is a confident wrong signal, which is worse
            // than the silence it replaces.
            //
            // *Appended, not committed* is `SchedulerService::Offer`'s own phrase and is
            // deliberately not reworded here. Two spellings of one state is how a reader
            // ends up believing they are two states.
            //
            // The closing paragraph is the POINT of the receipt rather than a
            // pleasantry: these two lines are only worth printing because there is a
            // second screen to hold them against, and an operator who does not know
            // that compares them with their own memory, which is what nothing was
            // comparing in the first place.
            //
            // The seat is NOT in the receipt and is printed anyway, labelled as what it
            // is: the verb this request was sent as, which is the only one the leader
            // can have answered (#1449). It is not an echo, so it is not dressed as one.
            //
            // The key IS in it (#178), and an absent one is spelled as what it means rather
            // than as a dash: this admission stated no key, which KEEPS any key already
            // recorded -- a dash would read as *this member has no key*, which the leader
            // did not say.
            auto const seat =
                action == ClusterAction::AdmitLearner ? Cluster::MemberSeat::Learner : Cluster::MemberSeat::Voter;
            return std::format("recorded, as received:\n"
                               "  {:<{}}{}\n"
                               "  {:<{}}{}\n"
                               "  {:<{}}{}\n"
                               "  {:<{}}{} (the verb this request was sent as)\n"
                               "\n"
                               "Appended, not committed: a majority has to take it, and this leader cannot\n"
                               "see that yet. Ask for the cluster state again to see the result.\n"
                               "\n"
                               "Compare the first three lines against the machine itself -- the id it minted\n"
                               "into --cluster-dir, the consensus endpoint its own --print-surfaces prints,\n"
                               "and the identity key `fastcache-cli node` prints against it. Each is one\n"
                               "thing spelled on two machines, and nothing else\n"
                               "compares them.\n",
                               "member id",
                               ReceiptLabelColumn,
                               receipt->memberId,
                               Wire::ConsensusEndpointLabel,
                               ReceiptLabelColumn,
                               receipt->raftEndpoint,
                               "identity key",
                               ReceiptLabelColumn,
                               receipt->publicKey.value_or(std::string { NoKeyStated }),
                               "seat",
                               ReceiptLabelColumn,
                               Cluster::MemberSeatName(seat));
        }
    }

    return std::unexpected { std::string { "unknown cluster request" } };
}

ClusterExchange PutClusterRequest(core::net::ISocket& client,
                                  Cc::CredentialNotice& notice,
                                  ClusterRequest const& request,
                                  Cc::ICredentialFor& credentials,
                                  std::string_view scheduler)
{
    // Through the launcher's own exchange rather than a second copy of it. That
    // function exists precisely so the distributed verbs do not grow one: the
    // credential pipelining and the "a daemon that does not know AUTH still served
    // the command" fall-through are each subtle enough that two implementations
    // would differ, and the one that differed would be the untested one.
    //
    // The credential is asked for HERE, at the exchange and for the endpoint it goes to, rather
    // than folded into a value the caller assembled. There is nothing between the two today --
    // this verb dials and exchanges in one breath -- and that is precisely why it is worth
    // spelling: a site that reads the secret where it SENDS it cannot acquire a gap later without
    // somebody deliberately putting one there.
    auto const presented = credentials.Present(scheduler);
    return ClusterExchange { .outcome = core::async::syncRun(
                                 Cc::ExchangeFramed(&client, &notice, EncodeClusterRequest(request), presented.credential)),
                             .missing = presented.missing };
}

std::expected<std::string, UnfinishedCommand> InterpretClusterAnswer(ClusterAction action,
                                                                     ClusterExchange const& answer,
                                                                     std::string_view scheduler)
{
    auto const& outcome = answer.outcome;
    if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        return std::unexpected { Unanswered(outcome, std::format("the scheduler at {} did not answer", scheduler)) };

    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
    {
        // A redirect reaches here only from a caller that did not follow it: `AskTheLeader`
        // answers a chain that did not settle itself. `Pending` either way -- it decided nothing
        // about the request. WHETHER it carries an address is `Cc::RedirectTarget`'s question,
        // shared with the launcher (#237).
        if (auto const leader = Cc::RedirectTarget(outcome); leader.has_value())
            return std::unexpected { Unanswered(AnswerSource::Pending,
                                                std::format("gave up after {} leader redirect(s); the last, from {}, "
                                                            "named {}",
                                                            MaxLeaderRedirects,
                                                            scheduler,
                                                            *leader)) };
        if (outcome.code == Wire::ErrorCode::NotLeader)
            // An election in progress, which is a different fact from "somebody else
            // leads" and has no address to offer.
            return std::unexpected { Unanswered(outcome, "the cluster has no leader right now; try again shortly") };
        // A refusal THIS exchange's missing ticket explains is said as WHY no ticket was presented.
        return std::unexpected { Unanswered(outcome, Cc::RecordedReason(outcome, answer.missing)) };
    }

    // A reply that arrived: whatever the interpretation refuses, the peer decided it.
    return InterpretClusterReply(action, outcome.value).transform_error([&outcome](std::string reason) {
        return Unanswered(outcome, std::move(reason));
    });
}

std::expected<std::string, UnfinishedCommand> RunClusterAdmin(NodeConfig const& cfg,
                                                              ClusterRequest const& request,
                                                              Cc::ICredentialFor& credentials,
                                                              IEndpointDialer& dialer)
{
    // `--scheduler`'s values, or this machine's own node when it names none (`AdminTargetsOf`).
    auto const targets = AdminTargetsOf(cfg);

    // Owned here rather than threaded in: this is a one-shot CLI verb, so "once per
    // process" and "once per invocation" are the same thing, and the admin surface
    // has no long-lived object to hang it on. What matters is that the verb reports
    // at all -- before #363 the cluster verbs discarded this silently, so an operator
    // running `--cluster-status` with a token against an older scheduler was told
    // nothing.
    auto notice =
        Cc::CredentialNotice { [](std::string_view text) { std::cerr << "fastcache-compile-node: " << text << '\n'; } };

    // What the exchange that produced the final answer presented: asked per endpoint, so a
    // redirect's leader is shown a ticket naming it.
    std::optional<Cc::MintFailure> missing;
    auto answered = AskTheLeader(dialer,
                                 targets,
                                 core::net::DialOptions { .connectTimeout = DialTimeout },
                                 "the scheduler",
                                 [&](core::net::ISocket& socket, std::string_view endpoint) {
                                     auto exchanged = PutClusterRequest(socket, notice, request, credentials, endpoint);
                                     missing = exchanged.missing;
                                     return std::move(exchanged.outcome);
                                 });
    if (!answered.has_value())
        return std::unexpected { std::move(answered).error() };
    return InterpretClusterAnswer(
        request.action, ClusterExchange { .outcome = std::move(answered->outcome), .missing = missing }, answered->endpoint);
}

} // namespace FastCache::Node
