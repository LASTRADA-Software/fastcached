// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "NodeIdentity.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Platform/FileTrust.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include <CacheProtocol.hpp>
#include <EndpointDial.hpp>

namespace FastCache::Node
{

namespace Wire = CompileCacheWire;

namespace
{
    /// How long a single exchange with the seed may take.
    ///
    /// Generous for `ClusterAdminCli::DialTimeout`'s reason: an operator typed this
    /// and is watching, and the seed answers from memory, so anything slower than
    /// this is a network problem rather than a busy leader.
    constexpr std::chrono::milliseconds DialTimeout { 10'000 };

    /// How many `NotLeader` redirects one run will follow.
    ///
    /// Bounded because two nodes each holding a stale `_knownLeader` can name each
    /// other forever -- the same reason the worker's heartbeat bounds its own
    /// following. Three is a cluster in the middle of an election, which settles.
    constexpr int MaxRedirects = 3;

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
    /// sending `Open` for something an operator spelled `Reject`.
    ///
    /// `None` maps to `List`, which is the harmless read: it is unreachable, because
    /// `main` dispatches on `action != None`, and a row that cannot be omitted is better
    /// than a hole shaped like one.
    constexpr EnumTable<EnrollAction, EnrollVerbRow> EnrollVerbs { {
        { .action = EnrollAction::None, .verb = Wire::EnrollControlVerb::List },
        { .action = EnrollAction::Open, .verb = Wire::EnrollControlVerb::Open },
        { .action = EnrollAction::Close, .verb = Wire::EnrollControlVerb::Close },
        { .action = EnrollAction::List, .verb = Wire::EnrollControlVerb::List },
        { .action = EnrollAction::Approve, .verb = Wire::EnrollControlVerb::Approve },
        { .action = EnrollAction::Reject, .verb = Wire::EnrollControlVerb::Reject },
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
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Pending, .label = "pending  " },
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Approved, .label = "approved " },
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Collected, .label = "collected" },
        DecisionLabelRow { .decision = Wire::EnrollmentDecision::Rejected, .label = "rejected " },
    };

    /// Whether every decision this build knows carries exactly one label.
    ///
    /// Derived from `KnownEnrollmentDecisions` rather than restated, so an enumerator
    /// ARRIVING is caught as well as one going away -- a restated list only ever notices
    /// the second, and the first is what leaves a row rendering as a fallback in the
    /// listing somebody reads before handing over the fleet's key.
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
                               .clusterKey = {} };

    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
    {
        // `NotLeader` is an INSTRUCTION rather than an answer about the cluster, and
        // it is judged by PARSING the message rather than by testing it for empty: an
        // empty one is replaced by the error table's default sentence, so "no leader
        // known" and "the leader is at h:p" arrive the same shape.
        if (auto const leader = Cc::RedirectTarget(outcome); leader.has_value())
            return EnrollReading { .progress = EnrollProgress::Redirect, .detail = *leader, .clusterKey = {} };

        switch (outcome.code)
        {
            case Wire::ErrorCode::NotLeader:
                // Authentic, and naming nobody: an election is in progress. That is a
                // wait rather than a failure, and it is the same wait a closed window
                // is, so the joiner keeps polling and the operator is told why.
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the cluster has no leader right now",
                                       .clusterKey = {} };
            case Wire::ErrorCode::EnrollmentClosed:
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the seed is not accepting enrollments; ask an operator to open a "
                                                 "window with --enroll-open",
                                       .clusterKey = {} };
            case Wire::ErrorCode::EnrollmentFull:
                return EnrollReading { .progress = EnrollProgress::Closed,
                                       .detail = "the seed's enrollment window is full; it holds as many requests as "
                                                 "it will record at once",
                                       .clusterKey = {} };
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
                                       .clusterKey = {} };
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
                                       .clusterKey = {} };
            default:
                break;
        }
        return EnrollReading { .progress = EnrollProgress::Fatal, .detail = Cc::DescribeOutcome(outcome), .clusterKey = {} };
    }

    auto const reply = Wire::DecodeEnrollReply(outcome.value);
    if (!reply.has_value())
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = "the seed answered enroll with a body this build cannot read",
                               .clusterKey = {} };

    switch (reply->outcome)
    {
        case Wire::EnrollOutcome::Pending:
            return EnrollReading { .progress = EnrollProgress::Waiting,
                                   .detail = "recorded; waiting for an operator to approve it",
                                   .clusterKey = {} };
        case Wire::EnrollOutcome::Rejected:
            return EnrollReading { .progress = EnrollProgress::Refused,
                                   .detail = "an operator refused this machine",
                                   .clusterKey = {} };
        case Wire::EnrollOutcome::Approved:
            break;
    }

    // Asserted rather than assumed, and this is the assertion that matters: a server
    // bug that leaked the key on `Pending` would be invisible from here, because an
    // outcome byte is equally correct in the healthy and the broken build. The empty
    // case is the mirror -- an `Approved` carrying nothing would have this node write
    // a zero-length key file and fail much later, on a machine nobody is watching.
    if (reply->clusterKey.empty())
        return EnrollReading { .progress = EnrollProgress::Fatal,
                               .detail = "the seed approved this machine and sent no key",
                               .clusterKey = {} };

    return EnrollReading { .progress = EnrollProgress::Admitted,
                           .detail = "approved",
                           .clusterKey = SecureByteBuffer { reply->clusterKey.begin(), reply->clusterKey.end() } };
}

std::string RenderEnrollmentReport(Wire::EnrollmentReport const& report)
{
    auto out = std::string {};
    if (report.state == Wire::WireEnrollmentState::Open)
        out += std::format("enrollment window OPEN for {}s\n", report.openForSeconds);
    else
        out += "enrollment window closed\n";

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
        // before handing over a key, and at forty rows an unmarked mismatch is one
        // nobody sees.
        auto const claimedHost = HostOfEndpoint(entry.raftEndpoint);
        auto const* const mismatch =
            claimedHost != entry.peerId ? "  <-- claimed address does not match the host it came from" : "";

        // The SECOND mark, and it answers a question the first cannot: this row stopped
        // tracking the machine when somebody decided about it, and something has polled
        // since claiming otherwise. Ordinary when a machine moved after being decided
        // about -- enrol it again -- and the signature of somebody else answering to a
        // decided id, which is the reading that wants an eye on it. Shown, never acted
        // on, for #242's reason.
        auto const drifted =
            entry.claimsChanged != 0
                ? std::format("  <-- {} later poll(s) claimed something other than this", entry.claimsChanged)
                : std::string {};
        out += std::format("  {}  {}  claims {}  from {}  {}s ago  {} attempt(s){}{}\n",
                           DescribeEnrollmentDecision(entry.decision),
                           entry.nodeId,
                           entry.raftEndpoint,
                           entry.peerId,
                           entry.firstSeenSecondsAgo,
                           entry.attempts,
                           mismatch,
                           drifted);
    }
    return out;
}

std::expected<std::string, std::string> RunEnrollAdmin(NodeConfig const& cfg,
                                                       EnrollCommand const& request,
                                                       ICredentialSource const& credential)
{
    if (cfg.scheduler.empty())
        return std::unexpected { std::string { "--scheduler names where to ask; an enrollment command needs one" } };

    auto notice =
        Cc::CredentialNotice { [](std::string_view text) { std::cerr << "fastcache-compile-node: " << text << '\n'; } };

    auto endpoint = cfg.scheduler;
    for (auto hop = 0; hop <= MaxRedirects; ++hop)
    {
        // A one-shot CLI on the process main thread: no reactor exists here, so this
        // legitimately blocks. `RunClusterAdmin`'s idiom, verbatim, because it is the
        // same situation.
        BlockingConnector connector { DefaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = DialTimeout } };
        auto client = Cc::DialEndpointBlocking(connector, endpoint, DialOptions { .connectTimeout = DialTimeout });
        if (client == nullptr)
            return std::unexpected { std::format("cannot reach the cluster at {}", endpoint) };

        auto const outcome =
            SyncRun(Cc::ExchangeFramed(client.get(),
                                       &notice,
                                       Wire::EncodeEnrollControl(WireVerbFor(request.action), request.subject),
                                       credential.Current()));

        if (outcome.kind == Cc::CacheOutcomeKind::Transport)
            return std::unexpected { std::format("the cluster at {} did not answer", endpoint) };

        if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
        {
            // Followed rather than reported, and BOUNDED: two nodes each holding a
            // stale `_knownLeader` name each other forever.
            if (auto const leader = Cc::RedirectTarget(outcome); leader.has_value() && hop < MaxRedirects)
            {
                endpoint = *leader;
                continue;
            }
            return std::unexpected { Cc::DescribeOutcome(outcome) };
        }

        auto const report = Wire::DecodeEnrollmentReport(outcome.value);
        if (!report.has_value())
            return std::unexpected { std::format("{} answered with a body this client cannot read", endpoint) };
        return RenderEnrollmentReport(*report);
    }

    return std::unexpected { std::format("gave up after {} leader redirect(s)", MaxRedirects) };
}

std::expected<ConsensusHistory, std::string> ReadConsensusHistory(std::filesystem::path const& stateDirectory)
{
    auto storage = Consensus::FileRaftStorage::Open(stateDirectory);
    if (!storage.has_value())
        return std::unexpected { std::format("cannot read {}: {}", stateDirectory.string(), storage.error().context) };

    auto recovered = storage->Load();
    if (!recovered.has_value())
        return std::unexpected { std::format(
            "cannot read the consensus state in {}: {}", stateDirectory.string(), recovered.error().context) };

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
    // `ClusterSelfMember` is the one place this node's own `(id, endpoint)` pair is
    // derived. Deriving it again here would be a second spelling that can disagree
    // with the one consensus will actually run under, and the whole point of stating
    // both halves is that a member the cluster counts must be one it can dial.
    auto const* const self = ClusterSelfMember(cfg);
    if (self == nullptr)
        return std::unexpected { std::string {
            "this node names no consensus member of its own, so it has nothing to ask to be admitted as. "
            "--listen-raft says where its consensus port answers and --raft-self says at which address other "
            "nodes reach it" } };

    return std::pair { self->id, self->raftEndpoint };
}

std::expected<void, std::string> StoreClusterKey(std::filesystem::path const& path, SecureByteBuffer const& key)
{
    if (auto const parent = path.parent_path(); !parent.empty())
    {
        auto error = std::error_code {};
        std::filesystem::create_directories(parent, error);
        if (error)
            return std::unexpected { std::format("cannot create {}: {}", parent.string(), error.message()) };
    }

    {
        // **The refusal to overwrite IS this open, and there is no pre-flight
        // `exists()` above it.** `"wbx"` is C11 exclusive create -- `O_EXCL` -- so the
        // file is created or the open FAILS, in one step that no second process can
        // interleave with. Asking `exists()` first and then opening `"wb"` is the
        // available spelling and is wrong twice over: `"wb"` TRUNCATES, so both ways
        // of the pre-check being wrong destroy the key this machine already holds.
        // It is wrong when the answer is stale -- anything creating the file between
        // the two calls is overwritten -- and, the half that needs no race at all,
        // when `exists()` cannot answer: its `error_code` overload reports a failed
        // stat by returning FALSE and setting the code, so a file that is there but
        // could not be stat'd (a directory this process may not traverse, a
        // filesystem error) read as absent and was truncated. That code was
        // discarded, so the guard failed OPEN silently.
        //
        // A guard folded into the operation is self-enforcing; one called alongside
        // it needs somebody to remember the call and to read its error.
        //
        // If a platform's libc ignored `x`, this would silently go back to
        // truncating -- so the property is TESTED rather than assumed, and the test
        // is the only thing that can catch it: `A key file that already exists is
        // never written over` in `EnrollClient_test.cpp` reaches this open with
        // nothing in front of it, so a libc that ignores `x` turns that case RED on
        // the platform that ignores it. That is what answers macOS, rather than a
        // claim about its `fopen`. Measured here on the MSVC CRT: a fresh path opens,
        // an existing one returns `EEXIST` and is NOT truncated, and plain `"wb"` on
        // the same path truncates it.
        //
        // A `unique_ptr` over the handle for the reason `ReadClusterKey`'s has one:
        // there are failure paths below and a bare `fclose` at each is the one that
        // eventually gets forgotten.
        errno = 0;
        auto* const opened = std::fopen(path.string().c_str(), "wbx");
        auto const openErrno = errno;
        auto file = std::unique_ptr<std::FILE, int (*)(std::FILE*)> { opened, &std::fclose };
        if (file == nullptr)
        {
            // `EEXIST` is the ordinary answer and the one an operator acts on, so it
            // keeps its own sentence; everything else names what the C library said,
            // because "cannot create" alone sends somebody looking for a key file
            // that is not the problem.
            if (openErrno == EEXIST)
                return std::unexpected { std::format(
                    "{} already exists; this machine already holds a cluster key. Remove it "
                    "deliberately if it is to join a different cluster",
                    path.string()) };
            return std::unexpected { std::format(
                "cannot create {}: {}", path.string(), std::error_code { openErrno, std::generic_category() }.message()) };
        }
        if (!key.empty() && std::fwrite(key.data(), 1, key.size(), file.get()) != key.size())
            return std::unexpected { std::format("cannot write {} in full", path.string()) };
    }

    // The FILE's own list, protected against the directory's. On Windows a
    // machine-wide directory grants read to `BUILTIN\Users` inheritably, so a key
    // written without this is one every standard account can read -- which is #741
    // arriving through a new door, on the one file whose exposure admits a machine to
    // the fleet.
    //
    // Refused rather than warned about: a key this process just placed and cannot
    // protect is a key an operator would have to notice a log line to learn about.
    // The property is asked BACK rather than the call's own success being trusted,
    // which is what `SecureSecretFileForServices` returns.
    //
    // **The remedy names a RE-APPROVAL and not "enrol again", because the key is
    // spendable once.** This runs after the server has already marked the id collected,
    // so running `--enroll-from` a second time is refused
    // (`enrollment-already-collected`) and the operator would be following this
    // sentence into a dead end. A guard's remedy text is part of the guard and is the
    // only part most people read; a confident wrong one is worse than none.
    if (!SecureSecretFileForServices(path))
        return std::unexpected { std::format("{} was written and could not be made unreadable to other accounts on "
                                             "this machine. It holds this cluster's key: remove it and check who may "
                                             "write to {}. This node has already collected its key, so enrolling "
                                             "again is refused -- ask an operator to run --enroll-approve for this "
                                             "id again, which re-arms exactly one more collection",
                                             path.string(),
                                             path.parent_path().string()) };

    return {};
}

std::expected<std::string, std::string> RunEnrollClient(NodeConfig const& cfg,
                                                        ICredentialSource const& credential,
                                                        IRandomSource& random,
                                                        IDrainWait& wait)
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

    if (cfg.clusterKeyFile.empty())
        return std::unexpected { std::string {
            "--enroll-from needs --cluster-key-file: it names where the key this node is given gets written, and "
            "nothing else can be inferred from" } };

    // **A key already sitting there is refused BEFORE the exchange, because the grant is
    // spendable once.**
    //
    // `StoreClusterKey` refuses this too, and that refusal is the authoritative one --
    // but it runs after the seed has already taken `Approved -> Collected`, so a machine
    // with a pre-placed key file would dial, be approved, BURN the one collection its id
    // has, and only then be told locally that it had nowhere to put the key. Recovering
    // from that needs an operator to run `--enroll-approve` again for an id that looks
    // already handled. Asking here costs a `stat` and saves a machine the round trip and
    // the operator the re-approval.
    //
    // **Both checks stay, and each closes a window the other cannot.** This one is
    // ADVISORY: it fails fast on the ordinary case and is allowed to be wrong, because
    // anything can create that file during the ten minutes this mode then spends waiting
    // for a person. The one in `StoreClusterKey` is the exclusive CREATE itself, which is
    // what makes the guarantee atomic. Deleting this one costs a burned grant; deleting
    // that one costs the key the machine is holding, so they are not alternatives and the
    // redundancy is the point rather than an oversight.
    //
    // A failed `stat` deliberately PROCEEDS rather than refusing. "Cannot tell" is not
    // "already there", refusing on it would block a legitimate enrolment on a transient
    // filesystem error, and it is safe to be wrong in this direction precisely because
    // the create below cannot truncate.
    if (auto error = std::error_code {}; std::filesystem::exists(cfg.clusterKeyFile, error) && !error)
        return std::unexpected { std::format(
            "{} already exists; this machine already holds a cluster key. Remove it deliberately if it is to join a "
            "different cluster. Nothing has been asked of the seed, so no enrollment request was spent",
            cfg.clusterKeyFile.string()) };

    // **The one-way mistake, caught before anything is asked of anybody (#1299).**
    //
    // A node started without `--raft-join` bootstraps a cluster of itself, elects
    // itself, and can never afterwards be admitted to anybody else's. It is silent: the
    // node comes up, leads a cluster of one, and looks healthy on every surface. A
    // forty-machine rollout offers that mistake thirty-nine times, and enrollment makes
    // it sharper rather than softer -- an open window will be entered by machines that
    // are, some of the time, permanently unable to join.
    //
    // Refused BEFORE the identity is resolved, so a refusal writes nothing: a directory
    // that already holds consensus state also already holds an id, and a fresh one is
    // left untouched for whoever fixes the configuration and runs this again.
    auto const history = ReadConsensusHistory(NodeStateDirectory(cfg));
    if (!history.has_value())
        return std::unexpected { std::move(history).error() };
    if (*history == ConsensusHistory::Recorded)
        return std::unexpected { std::format(
            "{} already holds consensus state: this node has taken part in a cluster before. Either it was started "
            "without --raft-join, in which case it bootstrapped a cluster of ITSELF and can never be admitted to "
            "anybody else's; or it is already a member of one, in which case it does not need enrolling. Both are "
            "fixed the same way and only if you mean it: stop this node, delete {}, and run this again. A wiped "
            "state directory gets a NEW identity, which is what admission needs -- clearing only the log would "
            "leave this node's old identity in place holding a vote record for the cluster it led.",
            NodeStateDirectory(cfg).string(),
            NodeStateDirectory(cfg).string()) };

    // The identity is MINTED here, into `--cluster-dir`, before anything is asked of
    // anybody -- because it is what the seed is asked to admit. A joiner that asked
    // under one id and then started under another would be a member the cluster
    // counts and cannot reach.
    auto identity = ResolveNodeIdentity(NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!identity.has_value())
        return std::unexpected { std::move(identity).error() };

    auto resolved = cfg;
    ApplyNodeIdentity(resolved, *identity);

    auto claim = EnrollClaim(resolved);
    if (!claim.has_value())
        return std::unexpected { std::move(claim).error() };
    auto const& [nodeId, raftEndpoint] = *claim;

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
        // A one-shot CLI on the process main thread: no reactor exists here, so this
        // legitimately blocks. `DialEndpointBlocking` takes a `BlockingConnector` by
        // TYPE rather than an `IConnector`, which is what keeps that fact checkable
        // rather than a comment.
        BlockingConnector connector { DefaultAddressResolver(), BlockingConnectorOptions { .ioTimeout = DialTimeout } };
        auto client = Cc::DialEndpointBlocking(connector, seed, DialOptions { .connectTimeout = DialTimeout });
        if (client == nullptr)
            return std::unexpected { std::format("cannot reach the seed at {}", seed) };

        auto reading = ReadEnrollReply(SyncRun(
            Cc::ExchangeFramed(client.get(),
                               &notice,
                               Wire::EncodeEnroll(Wire::EnrollRequest { .nodeId = nodeId, .raftEndpoint = raftEndpoint }),
                               credential.Current())));

        switch (reading.progress)
        {
            case EnrollProgress::Admitted: {
                if (auto stored = StoreClusterKey(cfg.clusterKeyFile, reading.clusterKey); !stored.has_value())
                    return std::unexpected { std::move(stored).error() };
                return std::format("admitted to the cluster as {}.\n"
                                   "The cluster key is now at {}.\n"
                                   "Start this node with --raft-join, and with the same --cluster-dir and "
                                   "--cluster-key-file it was enrolled with.\n",
                                   nodeId,
                                   cfg.clusterKeyFile.string());
            }
            case EnrollProgress::Refused:
                return std::unexpected { std::format("{} refused this machine ({})", seed, reading.detail) };
            case EnrollProgress::Fatal:
                return std::unexpected { std::format("{} could not enrol this machine: {}", seed, reading.detail) };
            case EnrollProgress::Redirect:
                if (redirects >= MaxRedirects)
                    return std::unexpected { std::format(
                        "gave up after {} leader redirect(s); the last named {}", MaxRedirects, reading.detail) };
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
                // `MaxRedirects` exists for the loop two nodes with a stale
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
