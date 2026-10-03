// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "EnrollmentWindow.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeStateFiles.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
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
                // **The one an operator actually gets, and it is NOT a version
                // problem.** The documented flow points `--enroll-from` at any member,
                // and most members run no consensus -- so this is the commonest
                // mistake, and it used to arrive as `UnknownOpcode` and be reported as
                // *the seed is running an older build*, sending somebody to upgrade a
                // node that was already current. Fatal, because a node that runs no
                // consensus will not start running it while this loop polls, and the
                // remedy is a different ADDRESS rather than a different moment.
                return EnrollReading { .progress = EnrollProgress::Fatal,
                                       .detail = "that node runs no consensus, so it belongs to no cluster and there "
                                                 "is nothing there to join. Point --enroll-from at a node that runs "
                                                 "consensus; --node-status names the components a node serves",
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
                                   .roster = {} };
        case Wire::EnrollOutcome::Rejected:
            return EnrollReading { .progress = EnrollProgress::Refused,
                                   .detail = "an operator refused this machine",
                                   .roster = {} };
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
                           .certificate = std::vector<std::byte> { reply->certificate.begin(), reply->certificate.end() } };
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
    // member keeps no record of the cluster it was admitted to, and a principal's roster is
    // `KeepEnrolledRoster`'s sentence.
    out += std::format("The seed recorded this machine's key in its cluster as a {}. On this machine, enrollment wrote "
                       "the identity it asked under -- its id and its identity key -- into {}",
                       row.name,
                       stateDirectory.string());
    if (!row.principal.has_value())
        out += ", and nothing else: no record of the cluster it was admitted to is kept here.\n";
    else
        out += ". Start this node with the same state directory; a worker principal joins no consensus.\n";
    return out;
}

std::string KeepEnrolledRoster(std::span<std::byte const> roster,
                               std::span<std::byte const> certificate,
                               std::chrono::system_clock::time_point now,
                               Distributed::IRosterStore& store)
{
    if (certificate.empty())
        return "The leader holds no certified roster yet, so none was kept: this worker adopts one when it first "
               "reaches a scheduler, and until then checks grants against --voter-key if it names any.\n";

    auto const compared = Cluster::DecodeRoster(roster);
    auto const offered = Cluster::DecodeCertifiedRoster(certificate);
    if (!compared.has_value() || !offered.has_value())
        return "The leader's certified roster could not be read, so none was kept: this worker adopts one when it "
               "first reaches a scheduler.\n";

    // The voters of the roster the OPERATOR compared stand in for the anchors a worker would
    // otherwise be given, so the certificate is judged exactly as any offered roster is.
    auto const voters = Cluster::VotersOf(*compared);
    auto adopted = Cluster::CertifyRoster(*offered,
                                          Cluster::CertificationInput { .clusterId = offered->clusterId,
                                                                        .voters = voters,
                                                                        .minimumVersion = 0,
                                                                        .held = std::nullopt,
                                                                        .now = now,
                                                                        .slack = Distributed::LeaseTokenClockSkewSlack });
    if (!adopted.has_value())
        return std::format("The leader's certified roster was not kept -- {} -- so this worker adopts one when it first "
                           "reaches a scheduler.\n",
                           Cluster::DescribeRosterRefusal(adopted.error()));

    if (auto saved = store.Save(
            Cluster::PersistedRoster { .certificate = adopted->certificate, .certifiedUntil = adopted->certifiedUntil });
        !saved.has_value())
        return std::format("The leader's certified roster could not be kept: {}.\n", saved.error());

    return std::format("Kept the cluster's certified roster, version {}, as {} in the same directory: it is this "
                       "worker's trust root, so it checks every grant against it and needs no --voter-key.\n",
                       adopted->certificate.version,
                       Distributed::RosterFileName);
}

std::expected<std::string, std::string> RunEnrollAdmin(NodeConfig const& cfg,
                                                       EnrollCommand const& request,
                                                       Cc::ICredentialFor& credentials,
                                                       IEndpointDialer& dialer)
{
    if (cfg.schedulers.empty())
        return std::unexpected { std::string { "--scheduler names where to ask; an enrollment command needs one" } };

    auto notice =
        Cc::CredentialNotice { [](std::string_view text) { std::cerr << "fastcache-compile-node: " << text << '\n'; } };

    // The first ask walks the configured list and takes whichever CONNECTS; a redirect names one
    // endpoint and is followed there, never back into the list (#1310) -- `AskTheLeader`, the one
    // loop the cluster verbs share. Each ask presents what @p credentials answers for the endpoint
    // it reached, so a redirect's leader is shown a ticket naming the leader.
    std::optional<Cc::MintFailure> missing;
    auto answered = AskTheLeader(dialer,
                                 cfg.schedulers,
                                 core::net::DialOptions { .connectTimeout = EnrollDialTimeout },
                                 "the cluster",
                                 [&](core::net::ISocket& socket, std::string_view endpoint) {
                                     auto const presented = credentials.Present(endpoint);
                                     missing = presented.missing;
                                     return core::async::syncRun(Cc::ExchangeFramed(
                                         &socket,
                                         &notice,
                                         EnrollControlFrame(request),
                                         presented.credential));
                                 });
    if (!answered.has_value())
        return std::unexpected { std::move(answered).error() };

    auto const& outcome = answered->outcome;
    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        return std::unexpected { Cc::RecordedReason(outcome, missing) };

    auto const report = Wire::DecodeEnrollmentReport(outcome.value);
    if (!report.has_value())
        return std::unexpected { std::format("{} answered with a body this client cannot read", answered->endpoint) };
    return RenderEnrollmentReport(*report, answered->endpoint);
}

std::expected<ConsensusHistory, std::string> ReadConsensusHistory(std::filesystem::path const& stateDirectory)
{
    auto storage = Consensus::FileRaftStorage::Open(stateDirectory);
    if (!storage.has_value())
        return std::unexpected { std::format("cannot read {}: {}{}",
                                             stateDirectory.string(),
                                             storage.error().context,
                                             StateFileUnreadableHint(stateDirectory)) };

    auto recovered = storage->Load();
    if (!recovered.has_value())
        return std::unexpected { std::format("cannot read the consensus state in {}: {}{}",
                                             stateDirectory.string(),
                                             recovered.error().context,
                                             StateFileUnreadableHint(stateDirectory)) };

    // Every durable trace, not one of them. A node that campaigned wrote a term AND a
    // self-vote AND a log entry, so any single field would do for the case this exists
    // to catch -- and asking all four is what makes the ANSWER right for the cases it
    // does not: a node admitted to somebody else's cluster carries a term it was told
    // and entries it was sent, and must be refused here too.
    //
    // The default-constructed value is documented as *a node that has never run*, which
    // is exactly the question, so this is that sentence rather than a reading of the
    // format.
    auto const& state = *recovered;
    auto const ran = state.state.currentTerm.value != 0 || state.state.votedFor.has_value() || !state.entries.empty()
                     || state.snapshot.has_value();
    return ran ? ConsensusHistory::Recorded : ConsensusHistory::None;
}

std::expected<std::pair<std::string, std::string>, std::string> EnrollClaim(NodeConfig const& cfg)
{
    // `ConsensusDialAddressOf` is the one place this node's consensus endpoint is derived --
    // the same one consensus runs under (`BootstrapMembersOf`). Deriving it again here would be
    // a second spelling that can disagree, and the whole point of stating both halves is that a
    // member the cluster counts must be one it can dial.
    auto dial = ConsensusDialAddressOf(cfg);
    if (cfg.nodeId.empty() || !dial.has_value())
        return std::unexpected { std::string {
            "this node names no consensus member of its own, so it has nothing to ask to be admitted as. "
            "--listen-raft says where its consensus port answers and --raft-self says at which address other "
            "nodes reach it" } };

    return std::pair { cfg.nodeId, *std::move(dial) };
}

std::expected<std::string, std::string> RunEnrollClient(NodeConfig const& cfg,
                                                        ISecureRandom& random,
                                                        INodeKeyFileGuard& keyGuard,
                                                        IDrainWait& wait,
                                                        IEndpointDialer& dialer,
                                                        core::platform::IWallClock const& wallClock)
{
    // **This mode's own preconditions are refused HERE and not as `StartupPolicyRejection`
    // rows, and that is a decision rather than a missed table row.** That table judges a
    // configuration this node will SERVE with -- it requires a `--scheduler`, and it
    // requires a toolchain -- and a node that is enrolling serves nothing and has
    // neither. A row there would refuse exactly the fresh install this mode exists for,
    // which is its entire population. `RunClusterAdmin` refuses its own empty
    // `--scheduler` inline for the same reason (`ClusterAdminCli.cpp`), and this is that
    // shape rather than a new one.
    //
    // Both refusals are taken before anything is asked of anybody, so a misconfigured
    // run costs no round trip and leaves no row on somebody else's list.
    //
    // **The seed's SHAPE is refused here too, and that is a reachability fix rather than
    // a second opinion.** `--enroll-from` has a `StartupPolicyRejection` row -- the same
    // idiom `--scheduler`, `--advertise` and `--upstream` use -- and on this path that
    // row cannot fire: `main` dispatches `--enroll-from` and RETURNS before the startup
    // table is consulted, so the row is reachable only through `--print-surfaces`. Left
    // to the dial, `--enroll-from=10.0.0.1` with no port was answered *cannot reach the
    // seed*, which names the wrong problem -- and named it AFTER this function had minted
    // this node's identity into `--cluster-dir`, so a typo wrote durable state. First of
    // this mode's preconditions for that reason: it is the cheapest, and it is the only
    // one that is purely about what the operator typed.
    //
    // `ParseDialEndpoint` and not a spelling of its own, so this and the table row cannot
    // come to different conclusions about the same string; a bare port names no machine,
    // which is the whole of why `HostOfEndpoint` is not the predicate here.
    if (!cfg.enrollFrom.empty() && !ParseDialEndpoint(cfg.enrollFrom).has_value())
        return std::unexpected { std::format(
            "--enroll-from={} is not an address to dial: it names a seed as <host>:<port>, and a bare port names no "
            "machine. Nothing has been changed on this machine",
            cfg.enrollFrom) };

    // **The one-way mistake, caught before anything is asked of anybody (#1299).**
    //
    // A node that runs consensus founds a cluster of one at its first start, elects itself,
    // and cannot afterwards be admitted to anybody else's as it stands: it would refuse every
    // leader but its own. It is silent: the node comes up, leads a cluster of one, and looks
    // healthy on every surface. So a directory holding that history is refused here, before
    // anything is asked of anybody.
    //
    // Refused BEFORE the identity is resolved, so a refusal writes nothing: a directory
    // that already holds consensus state also already holds an id, and a fresh one is
    // left untouched for whoever fixes the configuration and runs this again.
    //
    // After asking WHO wrote what the directory holds, which the key resolution below asks
    // again: a consensus store another account planted must be refused for that, never read
    // as this node's history.
    if (auto walked = JudgeStateDirectory(NodeStateDirectory(cfg), keyGuard); !walked.has_value())
        return std::unexpected { std::move(walked).error().message };
    auto const history = ReadConsensusHistory(NodeStateDirectory(cfg));
    if (!history.has_value())
        return std::unexpected { std::move(history).error() };
    if (*history == ConsensusHistory::Recorded)
        return std::unexpected { std::format(
            "{} already holds consensus state: this node has run a cluster before. Either it is its OWN -- every "
            "node that runs consensus founds a cluster of one at its first start -- and a node that led one cannot "
            "be admitted to anybody else's as it stands, since it would refuse every leader but its own; or it is "
            "one it was already admitted to, in which case it does not need enrolling. Both are fixed the same way "
            "and only if you mean it: stop this node, delete {}, and run this again. A wiped "
            "state directory gets a NEW identity, which is what admission needs -- clearing only the log would "
            "leave this node's old identity in place holding a vote record for the cluster it led.",
            NodeStateDirectory(cfg).string(),
            NodeStateDirectory(cfg).string()) };

    // The KEY first, as a start resolves it: it is what the seed records and what every later
    // proof is checked against, so a machine that asked under one key and started with another
    // would be a stranger to its own cluster -- and resolving it is what judges, and creates, the
    // state directory the id is minted into next, its owner's alone.
    auto key = ResolveNodeKeyFor(cfg, random, keyGuard);
    if (!key.has_value())
        return std::unexpected { std::move(key).error().message };

    // The identity is MINTED here, into `--cluster-dir`, before anything is asked of
    // anybody -- because it is what the seed is asked to admit. A joiner that asked
    // under one id and then started under another would be a member the cluster
    // counts and cannot reach.
    auto identity = ResolveNodeIdentity(NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!identity.has_value())
        return std::unexpected { std::move(identity).error() };

    auto resolved = cfg;
    ApplyNodeIdentity(resolved, *identity);

    // Always a worker principal. A machine that runs consensus joins a fleet as a LEARNER through
    // its formation record, which the node itself drives, not through this one-shot verb; what
    // `--enroll-from` admits is a worker by its key. It states no endpoint: a principal has none
    // anybody dials.
    auto const role = Wire::EnrollRole::Worker;
    auto self =
        JoinerIdentity { .nodeId = resolved.nodeId, .nodeEndpoint = {}, .role = role, .publicKey = key->pair.PublicKey() };
    auto const& nodeId = self.nodeId;

    // The key, whole, BEFORE the first ask: it is what the operator compares against the row
    // `--enroll-list` shows, and the comparison is the whole strength of an approval now that
    // no secret is exchanged (#178).
    std::cerr << std::format("fastcache-compile-node: asking {} to admit {} as a {} under the key\n"
                             "    {}\n"
                             "  Compare it with the key --enroll-list shows for {} before approving it.\n",
                             cfg.enrollFrom,
                             nodeId,
                             EnrollRoleRowFor(role).name,
                             FormatEd25519PublicKey(self.publicKey),
                             nodeId);

    auto notice =
        Cc::CredentialNotice { [](std::string_view text) { std::cerr << "fastcache-compile-node: " << text << '\n'; } };

    auto seed = cfg.enrollFrom;
    auto redirects = 0;
    auto said = std::string {};

    // The bound is MEASURED against the wait's own clock rather than accumulated from
    // the sleeps this loop asked for: a requested two seconds costs what the host's
    // timer granularity says, so counting them states a bound and enforces some
    // multiple of it.
    auto const startedAt = wait.Now();
    while (true)
    {
        // Through the seam, so a test can script what comes BACK. The blocking dial
        // and its `core::net::BlockingConnector` live in `BlockingEndpointDialer`, where
        // `DialEndpointBlocking` still sees the concrete type it requires.
        auto client = dialer.Dial(seed, core::net::DialOptions { .connectTimeout = EnrollDialTimeout });
        if (client == nullptr)
            return std::unexpected { std::format("cannot reach the seed at {}", seed) };

        auto reading = ReadEnrollReply(core::async::syncRun(
            Cc::ExchangeFramed(client.get(),
                               &notice,
                               // No credential: `ENROLL` is answered before authentication, and no roster holds this
                               // machine's key yet, so no ticket it could present would be admitted.
                               Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = self.nodeId,
                                                                        .nodeEndpoint = self.nodeEndpoint,
                                                                        .role = self.role,
                                                                        .publicKey = self.publicKey }))));

        switch (reading.progress)
        {
            case EnrollProgress::Admitted: {
                auto admitted = DescribeAdmission(self, reading.roster, NodeStateDirectory(cfg));
                // A worker keeps the leader's certified roster as its trust root; a member applies the
                // replicated state and needs none.
                if (!admitted.has_value() || self.role != Wire::EnrollRole::Worker)
                    return admitted;
                Distributed::FileRosterStore store { NodeStateDirectory(cfg) / Distributed::RosterFileName };
                return *admitted + KeepEnrolledRoster(reading.roster, reading.certificate, wallClock.now(), store);
            }
            case EnrollProgress::Refused:
                return std::unexpected { std::format("{} refused this machine ({})", seed, reading.detail) };
            case EnrollProgress::Fatal:
                return std::unexpected { std::format("{} could not enrol this machine: {}", seed, reading.detail) };
            case EnrollProgress::Redirect:
                if (redirects >= MaxEnrollRedirects)
                    return std::unexpected { std::format(
                        "gave up after {} leader redirect(s); the last named {}", MaxEnrollRedirects, reading.detail) };
                ++redirects;
                std::cerr << std::format(
                    "fastcache-compile-node: {} does not lead the cluster; asking {} instead\n", seed, reading.detail);
                seed = reading.detail;
                continue;
            case EnrollProgress::Waiting:
            case EnrollProgress::Closed:
                // **The redirect budget is a CHAIN bound, so reaching a node that
                // answered resets it.**
                //
                // `MaxEnrollRedirects` exists for the loop two nodes with a stale
                // `_knownLeader` can make by naming each other -- a property of
                // CONSECUTIVE redirects. Accumulated over the whole run it becomes a
                // total instead, and this mode waits up to ten minutes for a person:
                // that is ~300 polls, so three ordinary leadership changes anywhere in
                // the wait would abort a perfectly good enrolment with "gave up after 3
                // leader redirect(s)" -- a message naming a loop that never happened,
                // on a cluster that was about to admit this machine.
                //
                // A `Waiting` or `Closed` reading is a node that answered on its own
                // behalf, which is exactly what breaks a chain, so the count goes back
                // to zero here and the anti-loop property is untouched: a genuine
                // mutual-redirect loop produces redirects BACK TO BACK and never
                // reaches this arm.
                redirects = 0;
                break;
        }

        // Said once per DISTINCT reading rather than once per poll: a joiner waiting
        // ten minutes for a person produces three hundred polls, and a line each
        // would bury the one line that changes.
        if (reading.detail != said)
        {
            said = reading.detail;
            std::cerr << std::format("fastcache-compile-node: {} ({}); asking again every {}s\n",
                                     reading.detail,
                                     nodeId,
                                     EnrollPollInterval.count() / 1000);
        }

        if (wait.Now() - startedAt >= EnrollTotalBound)
            return std::unexpected { std::format(
                "gave up after {} minute(s) waiting to be approved by {}. Nothing has been changed on this "
                "machine, and this node's request stays on that seed's list until its window closes, so "
                "running this again after an operator approves it is enough",
                std::chrono::duration_cast<std::chrono::minutes>(EnrollTotalBound).count(),
                seed) };

        wait.Sleep(EnrollPollInterval);
    }
}

} // namespace FastCache::Node
