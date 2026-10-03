// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "EnrollmentAbsence.hpp"
#include "EnrollmentWindow.hpp"

#include <FastCache/Cluster/EnrollRequestSignature.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/PeerText.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <CacheProtocol.hpp>
#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

namespace
{
    /// The most of a seed's own refusal words a joiner reports: room for the longest
    /// `EnrollmentAbsenceTable` sentence, and a bound on what a peer can make this node log.
    constexpr std::size_t MaxSeedRefusalText = 512;

    static_assert(std::ranges::all_of(EnrollmentAbsenceTable,
                                      [](EnrollmentAbsenceRow const& row) {
                                          return row.detail.size() <= MaxSeedRefusalText;
                                      }),
                  "a joiner reports every reason a seed of this build gives, whole");

    /// Which wire verb each operator action sends.
    struct EnrollVerbRow
    {
        EnrollAction action;          ///< What the operator typed.
        Wire::EnrollControlVerb verb; ///< What goes on the wire.
    };

    /// One row per `EnrollAction`, in enumerator order.
    ///
    /// A table rather than a `switch`, for the reason every table here is one: a sixth
    /// action is a row, and an action added without one would otherwise fall through to
    /// whichever arm an `if` ladder happened to end on -- which on this surface means
    /// sending `List` for something an operator spelled `Reject`.
    ///
    /// `None` maps to `List`, which is the harmless read: it is unreachable, because
    /// `main` dispatches on `action != None`, and a row that cannot be omitted is better
    /// than a hole shaped like one.
    constexpr EnumTable<EnrollAction, EnrollVerbRow> EnrollVerbs { {
        { .action = EnrollAction::None, .verb = Wire::EnrollControlVerb::List },
        { .action = EnrollAction::List, .verb = Wire::EnrollControlVerb::List },
        { .action = EnrollAction::Approve, .verb = Wire::EnrollControlVerb::Approve },
        { .action = EnrollAction::Reject, .verb = Wire::EnrollControlVerb::Reject },
        { .action = EnrollAction::AutoApprove, .verb = Wire::EnrollControlVerb::AutoApprove },
        { .action = EnrollAction::AutoApproveOff, .verb = Wire::EnrollControlVerb::AutoApproveOff },
        { .action = EnrollAction::Clear, .verb = Wire::EnrollControlVerb::Clear },
    } };

    static_assert(RowsInEnumeratorOrder(EnrollVerbs, &EnrollVerbRow::action),
                  "EnrollVerbs must hold one row per EnrollAction, in enumerator order");

    /// The wire verb @p action sends.
    /// @param action What the operator typed.
    /// @return The verb.
    [[nodiscard]] constexpr Wire::EnrollControlVerb WireVerbFor(EnrollAction action) noexcept
    {
        return EnrollVerbs[static_cast<std::size_t>(action)].verb;
    }

    /// The ENROLL-CONTROL frame @p request sends: its verb, and the operand that verb carries --
    /// the id and key for an approval, the id for a rejection, the duration for an arming, and
    /// nothing for the rest.
    /// @param request What the operator typed.
    /// @return The frame.
    [[nodiscard]] std::vector<std::byte> EnrollControlFrame(EnrollCommand const& request)
    {
        if (request.action == EnrollAction::AutoApprove)
            return Wire::EncodeEnrollAutoApprove(request.duration);
        if (request.action == EnrollAction::Approve && request.key.has_value())
            return Wire::EncodeEnrollApprove(request.subject, *request.key);
        return Wire::EncodeEnrollControl(WireVerbFor(request.action), request.subject);
    }

    /// How each decision is spelled in the list an operator reads.
    struct DecisionLabelRow
    {
        Wire::EnrollmentDecision decision; ///< The state.
        std::string_view label;            ///< What it reads as, padded to one width.
    };

    /// What each decision reads as in the list an operator sees.
    ///
    /// A plain array rather than an `EnumTable`, because `EnrollmentDecision` is a WIRE
    /// enum and so carries no trailing `Last` to take an extent from -- a sentinel there
    /// would permanently claim a byte, which is why `ErrorCode` has none either.
    /// Completeness is asserted below against `KnownEnrollmentDecisions` instead, which
    /// is the one list of these enumerators and is what the decoder reads too.
    ///
    /// Padded to a common width so the columns after it line up: this list is read by a
    /// person comparing two addresses on forty rows, and a ragged left edge makes the
    /// one comparison they are here to make harder.
    constexpr std::array DecisionLabels {
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Pending, .label = "pending " },
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Approved, .label = "approved" },
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Rejected, .label = "rejected" },
    };

    /// Whether every decision this build knows carries exactly one label.
    ///
    /// Derived from `KnownEnrollmentDecisions` rather than restated, so an enumerator
    /// ARRIVING is caught as well as one going away -- a restated list only ever notices
    /// the second, and the first is what leaves a row rendering as a fallback in the
    /// listing somebody reads before admitting a machine to the fleet.
    /// @return True when every known decision has one row.
    [[nodiscard]] consteval bool EveryDecisionIsLabelled() noexcept
    {
        return std::ranges::all_of(Wire::KnownEnrollmentDecisions, [](Wire::EnrollmentDecision decision) {
            return std::ranges::count(DecisionLabels, decision, &DecisionLabelRow::decision) == 1;
        });
    }

    static_assert(EveryDecisionIsLabelled(), "every enrollment decision must read as something in an operator's list");

    /// What @p decision reads as in a list.
    /// @param decision The state.
    /// @return Its label; never empty, by the assertion above.
    [[nodiscard]] constexpr std::string_view DescribeEnrollmentDecision(Wire::EnrollmentDecision decision) noexcept
    {
        // A range-based `for` rather than `std::ranges::find` with a named iterator, and
        // that is a PORTABILITY fix rather than a style one. `readability-qualified-auto`
        // asks for `auto *const found` here, which is right on libstdc++ and libc++ --
        // where a `std::array` iterator IS a pointer -- and does not COMPILE on MSVC,
        // where it is a class type. Taking the analyser's advice would turn a Linux-only
        // lint into a Windows-only build failure, which is the worse trade: the lint is
        // visible to whoever runs the sweep and the build failure lands on whoever next
        // builds on Windows. Restructuring satisfies both, and `NOLINT` is not an option
        // this repository allows.
        for (auto const& row: DecisionLabels)
            if (row.decision == decision)
                return row.label;
        return std::string_view {};
    }

} // namespace

EnrollReading ReadEnrollReply(Cc::CacheOutcome const& outcome)
{
    if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = std::string { Cc::DescribeTransportFailure(outcome.transportFailure) },
                               .roster = {} };

    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
    {
        // `NotLeader` is an INSTRUCTION rather than an answer about the cluster, and
        // it is judged by PARSING the message rather than by testing it for empty: an
        // empty one is replaced by the error table's default sentence, so "no leader
        // known" and "the leader is at h:p" arrive the same shape.
        if (auto const leader = Cc::RedirectTarget(outcome); leader.has_value())
            return EnrollReading { .progress = EnrollProgress::Redirect, .detail = *leader, .roster = {} };

        switch (outcome.code)
        {
            case Wire::ErrorCode::NotLeader:
                // Authentic, and naming nobody: an election is in progress. That is a
                // wait rather than a failure, and it is the same wait a full list is, so
                // the joiner keeps polling and the operator is told why.
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the cluster has no leader right now",
                                       .roster = {} };
            case Wire::ErrorCode::EnrollmentFull:
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the seed's enrollment list is full; it holds as many requests as "
                                                 "it will record at once",
                                       .roster = {} };
            case Wire::ErrorCode::EnrollmentHostFull:
                // The same wait, for a narrower reason: this machine's ADDRESS already has as
                // many requests waiting as one host may. One of them being decided frees a row.
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the seed already has as many requests waiting from this machine's "
                                                 "address as it records from one host; one being decided makes room",
                                       .roster = {} };
            case Wire::ErrorCode::NoCluster:
                // **Not a version problem.** A seed that records no joiner is the commonest
                // mistake, and it used to arrive as `UnknownOpcode` and be reported as *the seed
                // is running an older build*, sending somebody to upgrade a node that was already
                // current. Fatal, because the seed will not start recording joiners while a joiner
                // polls, and the remedy is a different ADDRESS -- or a change on the seed -- rather
                // than a different moment.
                //
                // **The SEED's words, never a reason guessed here.** It records nobody for a
                // reason only it knows -- no consensus, consensus confined to its own machine,
                // no scheduler, a pin that does not name it (`EnrollmentAbsenceTable`) -- and
                // this end saying *runs no consensus* to a node that runs it sends the
                // operator to the wrong fix. Bounded, since they are a peer's.
                return EnrollReading { .progress = EnrollProgress::Fatal,
                                       .detail = std::format("the seed records no joiner, and it is not a version "
                                                             "problem: {}",
                                                             BoundedPeerText(outcome.message, MaxSeedRefusalText)),
                                       .roster = {} };
            case Wire::ErrorCode::UnknownOpcode:
                // The one refusal that says the SEED is the problem rather than this
                // machine or this moment, and the one worth stopping on: a seed that
                // does not implement the verb will not start implementing it while
                // this loop polls.
                //
                // It now means what it says. While a node without consensus answered
                // this too, the sentence below was the likeliest thing a healthy fleet
                // would print -- a confident wrong signal, and worse than a vague right
                // one. Such a node answers `NoCluster` above; reaching HERE means the
                // verb really is unknown to the seed's build.
                return EnrollReading { .progress = EnrollProgress::Fatal,
                                       .detail = "the seed does not implement enrollment; it is running a build older "
                                                 "than this one",
                                       .roster = {} };
            default:
                break;
        }
        return EnrollReading { .progress = EnrollProgress::Fatal, .detail = Cc::DescribeOutcome(outcome), .roster = {} };
    }

    auto const reply = Wire::DecodeEnrollReply(outcome.value);
    if (!reply.has_value())
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = "the seed answered enroll with a body this build cannot read",
                               .roster = {} };

    switch (reply->outcome)
    {
        case Wire::EnrollOutcome::Pending:
            // Two readings, one answer: nobody has decided yet, or an approval has not reached
            // the leader's own roster -- which takes a moment and needs nothing from anybody.
            return EnrollReading { .progress = EnrollProgress::Waiting,
                                   .detail = "recorded; waiting to be admitted",
                                   .roster = {},
                                   .signature = reply->signature,
                                   .challenge = reply->challenge };
        case Wire::EnrollOutcome::Rejected:
            return EnrollReading { .progress = EnrollProgress::Refused,
                                   .detail = "an operator refused this machine",
                                   .roster = {},
                                   .signature = reply->signature,
                                   .challenge = reply->challenge };
        case Wire::EnrollOutcome::Approved:
            break;
    }

    // Asserted rather than assumed: an `Approved` carrying nothing would have this node
    // report an admission it can check nothing about, and a roster is what it checks.
    if (reply->roster.empty())
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = "the seed approved this machine and sent no roster",
                               .roster = {} };

    return EnrollReading { .progress = EnrollProgress::Admitted,
                           .detail = "approved",
                           .roster = std::vector<std::byte> { reply->roster.begin(), reply->roster.end() },
                           .signature = reply->signature,
                           .challenge = reply->challenge };
}

std::string RenderEnrollmentReport(Wire::EnrollmentReport const& report, std::string_view scheduler)
{
    auto out = std::string {};
    if (report.state == Wire::WireEnrollmentState::AutoApprove)
        out += std::format("enrollment: auto-approve ({} min left)\n", report.autoApproveSecondsLeft / 60);
    else
        out += "enrollment: manual\n";

    if (report.pending.empty())
    {
        out += "  (nothing waiting)\n";
        return out;
    }

    for (auto const& entry: report.pending)
    {
        // **The mark, which is the whole reason both hosts are shown.** They disagree
        // for entirely ordinary reasons -- a DNS name, a NAT, a node dialling itself --
        // so this is never a refusal; it is the one line an operator's eye stops on
        // before admitting a machine, and at forty rows an unmarked mismatch is one
        // nobody sees. A row that states no endpoint has nothing to compare.
        auto const claimedHost = HostOfEndpoint(entry.nodeEndpoint);
        auto const* const mismatch = !entry.nodeEndpoint.empty() && claimedHost != entry.peerId
                                         ? "  <-- claimed address does not match the host it came from"
                                         : "";

        // The SECOND mark, and it answers a question the first cannot: this row stopped
        // tracking the machine when somebody decided about it, or another KEY asked under
        // its id, and something has polled since claiming otherwise. Ordinary when a machine
        // moved after being decided about -- enrol it again -- and the signature of somebody
        // else answering to this id, which is the reading that wants an eye on it. Shown,
        // never acted on, for #242's reason.
        auto const drifted =
            entry.claimsChanged != 0
                ? std::format("  <-- {} later poll(s) claimed something other than this", entry.claimsChanged)
                : std::string {};

        // The THIRD mark: the host this row was first asked from, which is what the per-host bound
        // counts it by, and never moves. Shown on every row, so an `EnrollmentHostFull` naming a
        // host can be matched to that host's rows. Marked when the row has since been asked from
        // another address -- ordinary for a dual-stack or roaming machine, and the one pattern that
        // would otherwise hide a host's rows under another host's name. Compared folded, as the
        // bound compares, so an IPv4-mapped spelling of the same address is no move.
        auto const first = entry.firstPeerId.empty() ? std::string {} : std::format("  first from {}", entry.firstPeerId);
        auto const* const moved = !entry.firstPeerId.empty() && UnmappedHost(entry.peerId) != entry.firstPeerId
                                      ? "  <-- asked since from another address than it first asked from"
                                      : "";

        // The KEY, whole, and on a line of its own: it is 43 characters an operator compares
        // one by one against what the joiner printed, and an approval admits exactly this one.
        auto const where =
            entry.nodeEndpoint.empty() ? std::string { "no endpoint" } : std::format("claims {}", entry.nodeEndpoint);
        out += std::format("  {}  {}  {}  {}  from {}{}  {}s ago  {} attempt(s){}{}{}\n",
                           DescribeEnrollmentDecision(entry.decision),
                           entry.nodeId,
                           EnrollRoleRowFor(entry.role).name,
                           where,
                           entry.peerId,
                           first,
                           entry.firstSeenSecondsAgo,
                           entry.attempts,
                           mismatch,
                           drifted,
                           moved);
        out += std::format("      key     {}\n", FormatEd25519PublicKey(entry.publicKey));
        // The approval, ready to paste, for a row nobody has decided about: it names the key on
        // this row, so pasting it after comparing admits exactly what was compared -- and is
        // refused if the row has since been replaced by another machine under the same id.
        if (entry.decision == Wire::EnrollmentDecision::Pending)
            out += std::format("      approve fastcache-compile-node --scheduler={} --enroll-approve={}@{}\n",
                               scheduler,
                               entry.nodeId,
                               FormatEd25519PublicKey(entry.publicKey));
        // The audit line: admitted by a deadline, with nobody comparing this key, and when that
        // deadline was armed -- so a window can be matched to who got in during it.
        if (entry.autoApprovedArmedSecondsAgo.has_value())
            out += std::format("      auto-approved (window armed {}s ago)\n", *entry.autoApprovedArmedSecondsAgo);
        if (entry.rosterFingerprint.has_value())
            out += std::format("      roster  {}  (handed to it; compare with what it printed)\n",
                               Cluster::RenderRosterFingerprint(*entry.rosterFingerprint));
    }
    return out;
}

std::vector<std::byte> EncodeSignedEnroll(JoinerIdentity const& self,
                                          Ed25519KeyPair const& identity,
                                          std::span<std::byte const> nonce)
{
    auto const key = identity.PublicKey();
    auto const challenge = Wire::ChallengeBytes(self.challenge);
    auto const signature = Cluster::SignEnrollRequest(identity,
                                                      Cluster::EnrollRequestClaim { .nodeId = self.nodeId,
                                                                                    .nodeEndpoint = self.nodeEndpoint,
                                                                                    .role = self.role,
                                                                                    .publicKey = key,
                                                                                    .nonce = nonce,
                                                                                    .challenge = challenge });
    return Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = self.nodeId,
                                                    .nodeEndpoint = self.nodeEndpoint,
                                                    .role = self.role,
                                                    .publicKey = key,
                                                    .nonce = nonce,
                                                    .challenge = challenge,
                                                    .signature = signature });
}

std::expected<std::string, std::string> DescribeAdmission(JoinerIdentity const& self,
                                                          std::span<std::byte const> roster,
                                                          std::filesystem::path const& stateDirectory)
{
    auto const decoded = Cluster::DecodeRoster(roster);
    if (!decoded.has_value())
        return std::unexpected { std::format("the seed answered with a roster this build cannot read: {}",
                                             decoded.error().context) };

    auto const& row = EnrollRoleRowFor(self.role);
    if (!RosterRecordsJoiner(*decoded, self.nodeId, self.publicKey, self.role))
        return std::unexpected { std::format(
            "the seed said {} was admitted, and the roster it sent does not record {} as a {} under this machine's key "
            "{}. Either an operator approved a different key for this id, or something between this machine and the "
            "seed rewrote the reply; nothing it names can be trusted. Compare --enroll-list on the seed with this "
            "key, and ask again",
            self.nodeId,
            self.nodeId,
            row.name,
            FormatEd25519PublicKey(self.publicKey)) };

    auto out = std::format("admitted to the cluster as {}, a {}.\n"
                           "Roster fingerprint: {}\n"
                           "Compare it with the fingerprint --enroll-list shows for {} on the seed; if they differ, "
                           "something between the two rewrote the roster and this machine must not be started.\n",
                           self.nodeId,
                           row.name,
                           Cluster::RenderRosterFingerprint(Cluster::DigestOfRoster(roster)),
                           self.nodeId);

    // What approval DID, and where this machine wrote it -- and nothing it did not. The seed
    // recorded the key; this machine kept the identity it asked under, and nothing else: a
    // member keeps no record of the cluster it was admitted to.
    out += std::format("The seed recorded this machine's key in its cluster as a {}. On this machine, enrollment wrote "
                       "the identity it asked under -- its id and its identity key -- into {}, and nothing else: no "
                       "record of the cluster it was admitted to is kept here.\n",
                       row.name,
                       stateDirectory.string());
    return out;
}

std::expected<std::string, UnfinishedCommand> RunEnrollAdmin(NodeConfig const& cfg,
                                                             EnrollCommand const& request,
                                                             Cc::ICredentialFor& credentials,
                                                             IEndpointDialer& dialer)
{
    auto notice =
        Cc::CredentialNotice { [](std::string_view text) { std::cerr << "fastcache-compile-node: " << text << '\n'; } };

    // The first ask walks the configured list -- this machine's own node when `--scheduler` names
    // none (`AdminTargetsOf`) -- and takes whichever CONNECTS; a redirect names one
    // endpoint and is followed there, never back into the list (#1310) -- `AskTheLeader`, the one
    // loop the cluster verbs share. Each ask presents what @p credentials answers for the endpoint
    // it reached, so a redirect's leader is shown a ticket naming the leader.
    std::optional<Cc::MintFailure> missing;
    auto const configured = AdminTargetsOf(cfg);
    auto answered = AskTheLeader(dialer,
                                 configured,
                                 core::net::DialOptions { .connectTimeout = EnrollDialTimeout },
                                 "the cluster",
                                 [&](core::net::ISocket& socket, std::string_view endpoint) {
                                     auto const presented = credentials.Present(endpoint);
                                     missing = presented.missing;
                                     return core::async::syncRun(Cc::ExchangeFramed(
                                         &socket, &notice, EnrollControlFrame(request), presented.credential));
                                 });
    if (!answered.has_value())
        return std::unexpected { std::move(answered).error() };

    auto const& outcome = answered->outcome;
    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        return std::unexpected { Unanswered(outcome, Cc::RecordedReason(outcome, missing)) };

    auto const report = Wire::DecodeEnrollmentReport(outcome.value);
    if (!report.has_value())
        return std::unexpected { Unanswered(
            outcome, std::format("{} answered with a body this client cannot read", answered->endpoint)) };
    return RenderEnrollmentReport(*report, answered->endpoint);
}

} // namespace FastCache::Node
