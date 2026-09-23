// SPDX-License-Identifier: Apache-2.0
#include "MembershipGate.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/MembershipWire.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// A `NodeMetrics` asked of a node whose sources are detached: it is stopping.
    constexpr Cc::UncountedRefusal NodeIsStopping {
        .code = CompileCacheWire::ErrorCode::EndpointBusy,
        .rationale = "a node in its last moments before exit, once per request that reaches it then; a rise would say "
                     "only that somebody asked while it stopped, which its own log already says",
    };

    /// One row of `WireRoles`.
    struct WireRoleRow
    {
        Distributed::SchedulerRole role;         ///< What this node calls it.
        CompileCacheWire::WireSchedulerRole tag; ///< What the wire calls it.
    };

    /// Which wire tag each scheduler role travels as.
    ///
    /// A table rather than a switch for the reason the surface mapping above is one: the
    /// node's own enum is free to be reordered -- two `EnumTable`s already key on it --
    /// and transmitting it directly would silently make its declaration order a wire
    /// contract. Mapping here means a reorder is a compile error at this table rather
    /// than a client reading `Leader` where the node meant `Follower`.
    constexpr EnumTable<Distributed::SchedulerRole, WireRoleRow> WireRoles { {
        { .role = Distributed::SchedulerRole::Follower, .tag = CompileCacheWire::WireSchedulerRole::Follower },
        { .role = Distributed::SchedulerRole::Undecided, .tag = CompileCacheWire::WireSchedulerRole::Undecided },
        { .role = Distributed::SchedulerRole::Leader, .tag = CompileCacheWire::WireSchedulerRole::Leader },
    } };

    static_assert(RowsInEnumeratorOrder(WireRoles, &WireRoleRow::role),
                  "WireRoles must hold one row per SchedulerRole, in enumerator order");

    /// One row of `WireStandings`.
    struct WireStandingRow
    {
        Consensus::Standing standing;                ///< What consensus calls it.
        CompileCacheWire::WireConsensusStanding tag; ///< What the wire calls it.
    };

    /// Which wire tag each consensus standing travels as (#1449).
    ///
    /// A table for `WireRoles`' reason: `Consensus::Standing` is private and free to be
    /// reordered, so transmitting it directly would make its declaration order a wire
    /// contract nobody signed.
    constexpr EnumTable<Consensus::Standing, WireStandingRow> WireStandings { {
        { .standing = Consensus::Standing::NoCluster, .tag = CompileCacheWire::WireConsensusStanding::NoCluster },
        { .standing = Consensus::Standing::Voter, .tag = CompileCacheWire::WireConsensusStanding::Voter },
        { .standing = Consensus::Standing::Learner, .tag = CompileCacheWire::WireConsensusStanding::Learner },
        { .standing = Consensus::Standing::Outsider, .tag = CompileCacheWire::WireConsensusStanding::Outsider },
    } };

    static_assert(RowsInEnumeratorOrder(WireStandings, &WireStandingRow::standing),
                  "WireStandings must hold one row per Consensus::Standing, in enumerator order");

    /// What this surface does about one refusal it may be asked to answer.
    ///
    /// **Exactly one of the two is set**, checked per row rather than described: a
    /// counter says a rise is something an operator acts on, a rationale says a rise
    /// would mean nothing and carries the argument for that. Those are the two claims
    /// `Cc::Refuse` and `Cc::RefuseWithoutCounter` make, and pairing them here keeps the
    /// answer beside the arm rather than inside a `switch` where a missing case would
    /// only imply it.
    ///
    /// The wire code is deliberately NOT a column: it is a property of the refusal
    /// rather than of this surface, and `ErrorCodeFor` owns it for both enumerations.
    struct RefusalPolicy
    {
        /// What rises, or nothing where this surface deliberately counts none.
        std::optional<IMetricsSink::Counter> counter;

        /// Why nothing rises. Empty exactly when `counter` is set.
        ///
        /// Never sent and never read at run time. Spelled `rationale` and not `why`,
        /// because `CompileCacheWire::RefusedVerb::why` is text a CLIENT is sent and one
        /// word cannot carry both contracts.
        std::string_view rationale;
    };

    /// Answer a refusal the way its row decided.
    /// @param metrics Where a counted refusal is recorded.
    /// @param code What the client is told, from `ErrorCodeFor`.
    /// @param policy This surface's decision about the arm.
    /// @param detail Words for a person, or empty when there are none to add.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerRefusal(IMetricsSink& metrics,
                                                       CompileCacheWire::ErrorCode code,
                                                       RefusalPolicy const& policy,
                                                       std::string_view detail)
    {
        if (policy.counter.has_value())
            return Cc::Refuse(metrics, { .code = code, .counter = *policy.counter }, detail);
        return Cc::RefuseWithoutCounter({ .code = code, .rationale = policy.rationale }, detail);
    }

    /// What this surface does about each pre-payload decision.
    ///
    /// A total switch rather than an `EnumTable`, which is not a local preference:
    /// `PrePayloadDecision` is a wire enum both binaries compile in and states no
    /// `Last`, so giving it one to satisfy the table idiom would be a wire change bought
    /// for nothing. `-Werror=switch` is the guard instead -- and on MSVC it is NOT, C4062
    /// being off by default, which is why the list below is also asserted over.
    /// @param decision A decision other than `Serve`.
    /// @return What this surface does about it.
    [[nodiscard]] constexpr RefusalPolicy PrePayloadPolicy(CompileCacheWire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case CompileCacheWire::PrePayloadDecision::PayloadTooLarge:
                // Counted, and with no innocent reading available: both verbs here are
                // FIELDLESS and their `OpTable` rows bound them to `MaxControlPayload`
                // rather than to the session cap, so a header declaring more came from
                // no client of this tree at any version.
                return { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedPayloadTooLarge, .rationale = {} };
            case CompileCacheWire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder owns a verb only when FamilyOf names its family, and an opcode "
                                      "with no OpTable row is Unset -- so an unknown one is answered UnservedReply at "
                                      "the door and never reaches this surface" };
            case CompileCacheWire::PrePayloadDecision::Unauthenticated:
                return { .counter = std::nullopt,
                         .rationale = "AuthRequired() is false here by decision -- the credential on this listener is "
                                      "the scheduler's -- and DecidePrePayload yields this only for a surface that "
                                      "requires one" };
            case CompileCacheWire::PrePayloadDecision::Serve:
                break;
        }
        // Unreachable by contract, as `ErrorCodeFor`'s own `Serve` arm is: the endpoint
        // asks this only for a decision that refused. Closed rather than left to fall
        // off the end, and closed UNCOUNTED, because inventing an event for a request
        // that WAS served is the one wrong answer available here.
        return { .counter = std::nullopt, .rationale = "Serve is not a refusal and the endpoint never asks about it" };
    }

    // Not redundant with `-Werror=switch`, which catches a MISSING arm and not an EMPTY
    // one. An arm returning `{ nullopt, {} }` -- a rationale dropped in an edit -- would
    // reach `RefuseWithoutCounter` with nothing to say: a refusal answered correctly,
    // counted nowhere and asserting NOTHING, which is the one state no scan can see,
    // since an empty rationale joins no `RefuseUntriaged` backlog and is reported by
    // nobody. And it is the guard that works on MSVC, where C4062 is off by default and
    // a missing arm is silent -- which this branch has already paid for once.
    static_assert(std::ranges::all_of(std::array { CompileCacheWire::PrePayloadDecision::Serve,
                                                   CompileCacheWire::PrePayloadDecision::UnknownOpcode,
                                                   CompileCacheWire::PrePayloadDecision::PayloadTooLarge,
                                                   CompileCacheWire::PrePayloadDecision::Unauthenticated },
                                      [](CompileCacheWire::PrePayloadDecision decision) {
                                          auto const policy = PrePayloadPolicy(decision);
                                          return Cc::StatesOneRefusalClaim(policy.counter.has_value(), policy.rationale);
                                      }),
                  "every pre-payload arm must state either a counter or a rationale, and not both");

    /// One row of `EndpointRefusals`.
    struct EndpointRefusalRow
    {
        EndpointRefusal refusal; ///< Which endpoint decision this describes.
        RefusalPolicy policy;    ///< What this surface does about it.
    };

    /// Why neither credential arm counts, stated once for the two rows that share it.
    ///
    /// The enumerators are separate because a client is told different things about
    /// them; the ARGUMENT is one argument about one fact -- whose credential this
    /// listener carries -- so it is one sentence rather than two literals that can drift
    /// on any edit with nothing to catch it.
    constexpr std::string_view CredentialIsTheSchedulersRationale =
        "AUTH is the Session family, which MergedResponder routes to the scheduler; no credential outcome is ever "
        "decided against this surface";

    /// What this surface does about each endpoint-decided refusal.
    ///
    /// `.rationale` is spelled out as empty on the counted row rather than left to
    /// default: clang and gcc reject the omission under this project's pedantic flags
    /// (`-Wmissing-designated-field-initializers`) and MSVC does not say a word, which is
    /// a shape that builds clean on Windows and fails four CI legs.
    constexpr EnumTable<EndpointRefusal, EndpointRefusalRow> EndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          // Counted, and it names the REQUEST that was refused rather than the load that
          // exhausted the budget: on any node holding a tier the surface ceiling folds to
          // the cache's, so what a rise here says is that the DIAGNOSIS failed. That is
          // exactly when an operator is reaching for these verbs, so a flat graph would
          // be the worst possible moment to have one.
          .policy = { .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedEndpointBusy, .rationale = {} } },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .policy = { .counter = std::nullopt, .rationale = AnswerDeadlineIsTheEndpointsRationale } },
        { .refusal = EndpointRefusal::NodeProofUnchallenged,
          .policy = { .counter = std::nullopt, .rationale = NodeProofIsTheProversRationale } },
    } };

    // Positional rows alone would not catch an APPENDED enumerator: it leaves a
    // value-initialised row whose policy states NEITHER claim, and a guard that
    // short-circuits on the absent counter passes vacuously while the new refusal ships
    // uncounted and unexplained.
    static_assert(RowsInEnumeratorOrder(EndpointRefusals, &EndpointRefusalRow::refusal),
                  "EndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    static_assert(Cc::RowsStateOneRefusalClaim(EndpointRefusals,
                                               [](EndpointRefusalRow const& row) {
                                                   return Cc::RefusalClaim { .counted = row.policy.counter.has_value(),
                                                                             .rationale = row.policy.rationale };
                                               }),
                  "every endpoint refusal row must state either a counter or a rationale, and not both");
} // namespace

std::vector<std::byte> NodeStatusResponder::RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                         std::uint8_t /*opRaw*/,
                                                         std::string_view detail) const
{
    // Verb-blind for the reason every sibling answer is: `MergedResponder` routes by
    // verb FAMILY, so every verb arriving here is a node verb.
    return AnswerRefusal(_metrics, CompileCacheWire::ErrorCodeFor(decision), PrePayloadPolicy(decision), detail);
}

std::vector<std::byte> NodeStatusResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                 std::uint8_t /*opRaw*/,
                                                                 std::string_view detail) const
{
    auto const& row = EndpointRefusals[static_cast<std::size_t>(refusal)];
    return AnswerRefusal(_metrics, ErrorCodeFor(refusal), row.policy, detail);
}

std::optional<std::vector<std::byte>> NodeStatusResponder::RefusePeer(PeerIdentity const& peer, std::uint8_t /*opRaw*/) const
{
    return RefuseUnlessMember(_membership,
                              _metrics,
                              peer,
                              { .code = CompileCacheWire::ErrorCode::NotAMember,
                                .counter = IMetricsSink::Counter::NodeStatusRequestsRefusedNotAMember },
                              "this node reports its identity and counters to fleet members only");
}

Task<FrameReply> NodeStatusResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    // The verb is read back out of the frame this call was handed rather than taken on
    // the endpoint's word: `Answer` is reachable directly, which is why the gate exists
    // here as well as at the door. A frame too short to carry a header names no verb,
    // and `0xFF` is unassigned, so the refusal asks about a verb no policy admits rather
    // than about one it guessed.
    auto const header = CompileCacheWire::DecodeRequestHeader(frame);
    auto const opRaw = header.has_value() ? header->opRaw : std::uint8_t { 0xFF };

    // Before any suspension, and before the payload matters: a caller this node will not
    // answer must not be able to make it do work on the way to being refused.
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);

    if (!header.has_value())
        // Not this protocol at all. Empty is CLOSE, and it is the right answer to
        // exactly this: there is no frame to reply into.
        co_return std::vector<std::byte> {};

    switch (static_cast<CompileCacheWire::Op>(header->opRaw))
    {
        case CompileCacheWire::Op::NodeStatus:
            co_return CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok,
                                                    CompileCacheWire::EncodeNodeStatus(_identity.Describe()));
        case CompileCacheWire::Op::NodeMetrics: {
            // The reading `/metrics` renders -- every counter and every block of the snapshot beside
            // them -- in the encoding a live-stats cache subject streams (#1406). ONE capture, so the
            // verb, the page and the stream cannot be three readings of this node, and a client of
            // another layout is refused by name by the digest the encoding starts with.
            auto capture = _readings.Capture(CompileCacheWire::LiveSubject::Cache);
            if (!capture.has_value())
                co_return Cc::RefuseWithoutCounter(NodeIsStopping, "this node is stopping");
            co_return CompileCacheWire::EncodeReply(CompileCacheWire::Status::Ok, capture->body);
        }
        case CompileCacheWire::Op::ExplainAdmission: {
            // #1471. The fold the SURFACES enforce, through the same `_membership` oracle they
            // bind -- never a second walk over the same three participants, or the reported
            // answer and the enforced one can disagree, which is the defect this verb exists to
            // make visible rather than to add to.
            auto const payload = frame.subspan(CompileCacheWire::RequestHeaderSize);
            auto const host = payload.size() == header->payloadLength
                                  ? CompileCacheWire::DecodeExplainAdmissionPayload(payload)
                                  : std::nullopt;
            if (!host.has_value())
                co_return Cc::Refuse(_metrics,
                                     { .code = CompileCacheWire::ErrorCode::MalformedFrame,
                                       .counter = IMetricsSink::Counter::NodeAdmissionExplanationsRefusedMalformed },
                                     "explain-admission takes exactly one field, the host to ask about");

            co_return CompileCacheWire::EncodeReply(
                CompileCacheWire::Status::Ok,
                CompileCacheWire::EncodeAdmissionExplanation(Distributed::OnTheWire(_membership.Explain(*host))));
        }
        default:
            break;
    }

    // A verb routed here that this component does not serve. `UnimplementedVerb` and not
    // `DispatchNotPermitted`, which is the distinction #283/#340 cost this tree: a client
    // STEPS OVER the first and proceeds, and treats the second as fatal. Getting it
    // backwards gave a credentialled client a permanent 0% hit rate that read as a cold
    // cache.
    //
    // Uncounted, deliberately. It is what a HEALTHY build answers for a verb this node
    // has no component for -- once per exchange -- so a counter here would bury the
    // scan somebody would read it for.
    co_return Cc::RefuseWithoutCounter(
        Cc::UncountedRefusal { .code = CompileCacheWire::UnimplementedVerb,
                               .rationale = "a healthy answer, not an event: the router sent a verb here that "
                                            "this component does not own, which a client steps over" },
        "this node serves no component for that verb");
}

ConfiguredNodeStatus::ConfiguredNodeStatus(NodeConfig const& cfg,
                                           IClock const& clock,
                                           TimePoint startedAt,
                                           std::string version,
                                           std::string nodeId,
                                           NodeComponents components,
                                           NodeRuntimeSources sources) noexcept:
    _cfg { cfg },
    _clock { clock },
    _startedAt { startedAt },
    _version { std::move(version) },
    _nodeId { std::move(nodeId) },
    _components { components },
    _sources { sources }
{
}

CompileCacheWire::NodeStatusFields ConfiguredNodeStatus::Describe() const
{
    // Which `NodeSurface` a wire tag stands for. A table rather than a switch so the
    // mapping is one row per reported surface, and so the node's own enum can be
    // reordered freely: transmitting `NodeSurface` directly would make its declaration
    // order a wire contract, and this is the seam that stops it.
    //
    // The `0xFC` port is absent on purpose -- see `WireSurface`. A client learns it by
    // dialling it.
    struct Mapping
    {
        NodeSurface surface;
        CompileCacheWire::WireSurface tag;
    };
    static constexpr Mapping mappings[] = {
        { .surface = NodeSurface::Admin, .tag = CompileCacheWire::WireSurface::Admin },
        { .surface = NodeSurface::Raft, .tag = CompileCacheWire::WireSurface::Raft },
        { .surface = NodeSurface::Discovery, .tag = CompileCacheWire::WireSurface::Discovery },
    };

    // **There is deliberately no `--tls` boolean to read.** TLS is on by naming material
    // (`--tls-cert` with `--tls-key`) or by asking for material to be made
    // (`--tls-self-signed`), and a bare boolean beside them is what the config table
    // refuses to have. Derived here the same way, so this cannot report a scheme the
    // admin surface did not bind.
    auto const adminServesTls = (!_cfg.tlsCertFile.empty() && !_cfg.tlsKeyFile.empty()) || _cfg.tlsSelfSigned;

    CompileCacheWire::NodeStatusFields fields;
    fields.version = _version;
    fields.nodeId = _nodeId;

    // The key this node holds (#178), or nothing on a node that holds none: read from the
    // configuration the start applied it to, so it is the key the startup log named. The
    // wire spells the width itself, because its header may not include the crypto seam.
    static_assert(CompileCacheWire::IdentityPublicKeyBytes == Ed25519PublicKeyBytes,
                  "NodeStatus carries an Ed25519 public key, so the two widths are one fact");
    fields.runtime.identityPublicKey = _cfg.identityPublicKey;
    fields.uptimeSeconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(_clock.now() - _startedAt).count());

    namespace Bits = CompileCacheWire::NodeComponentBit;
    fields.components = (_components.cacheTier ? Bits::CacheTier : 0U) | (_components.worker ? Bits::Worker : 0U)
                        | (_components.scheduler ? Bits::Scheduler : 0U) | (_components.consensus ? Bits::Consensus : 0U);

    // **The `Worker` bit above and this reading answer DIFFERENT questions, and that is
    // the whole of #1295 rather than a nuance.** The bit says *this node has a worker
    // component*, which is false only on a node started with `--slots=0` (#206) -- and
    // there this source is not wired at all, so the reading below is absent rather than
    // a worker surveying nothing forever. This says *is that worker serving yet*, which is not a constant
    // at all: a node serves while it identifies its toolchains, so between start and the
    // heartbeat thread's first completed round it runs a worker that can honour nothing.
    // One `bool` reported both, so the state an operator most needs to see was the one
    // it could not express.
    //
    // Read through the seam, never from the served map itself: the map has exactly one
    // writer and `Describe()` runs on a reactor thread. See `NodeRuntimeState`.
    //
    // A null source leaves the field DISENGAGED rather than reporting `Surveying` with
    // zero of zero -- absent is not zero, and a caller that wired nothing has not
    // observed a node with nothing to serve.
    if (_sources.runtime != nullptr)
    {
        auto const reading = _sources.runtime->Toolchains();
        fields.runtime.toolchains = reading.state;
        fields.runtime.toolchainsServed = reading.served;
        fields.runtime.toolchainsDiscovered = reading.discovered;

        // **Three states, not two.** No source wired is *nothing publishes this*; a
        // source with no reading yet is *the first heartbeat round has not reported*;
        // and an engaged reading whose `registered` is zero is *this node has tried and
        // is registered nowhere*, which is the one an operator acts on.
        if (auto const registration = _sources.runtime->Registration(); registration.has_value())
        {
            fields.runtime.registrarsRegistered = registration->registered;
            fields.runtime.registrarsTotal = registration->total;

            // A DURATION on the wire, differenced here against the clock that stamped
            // it. An instant would be differenced by the receiver against its own clock,
            // which is a different clock -- the rule a heartbeat age already holds.
            // Absent when nothing has ever been accepted: *never* is not *a long time
            // ago*, and zero would report the healthy answer for both.
            if (registration->lastAccepted.has_value())
                fields.runtime.lastRegistrationSecondsAgo = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(_clock.now() - *registration->lastAccepted).count());
        }
    }

    // Read live rather than published: this accounting is already thread-safe and
    // already exact at the instant it is asked, so a publisher would only make it
    // staler. Both numbers or neither -- a slot count with no in-flight figure invites
    // the reading that the node is idle.
    if (_sources.capacity != nullptr)
    {
        fields.runtime.compileSlots = static_cast<std::uint32_t>(_sources.capacity->Slots());
        fields.runtime.compilesInFlight = static_cast<std::uint32_t>(_sources.capacity->InFlight());
        // The state an operator waiting to reboot polls (#1303). Read beside the in-flight
        // figure rather than derived from it by the client, because `Drained` is this
        // node's own claim that stopping abandons nothing -- and a client that inferred it
        // from a zero would claim it for a worker nobody cordoned.
        fields.runtime.cordon = _sources.capacity->CordonState();
    }

    // The question `components` cannot answer: a leading scheduler and a following one
    // both report `scheduler`, and a follower's registry is empty and reads exactly like
    // an idle fleet. Absent on a node running no scheduler, which is why `Undecided` is
    // safe to report as itself -- it means an election, not a missing component.
    //
    // Role and endpoint are read separately and deliberately are not atomic together:
    // `SchedulerService` says so itself, because a lock spanning both would buy an
    // atomicity the fleet does not have anyway.
    if (_sources.scheduler != nullptr)
    {
        fields.runtime.schedulerRole = WireRoles[static_cast<std::size_t>(_sources.scheduler->Role())].tag;
        fields.runtime.leaderEndpoint = _sources.scheduler->LeaderEndpoint();
    }

    // The one state in this record an operator may act on within seconds of reading it:
    // while it is `Open`, any machine that can route to this node's 0xFC port may ask to
    // join, and approving one hands it this cluster's key in cleartext. Reported here
    // rather than left to the repeating log line, because that line reaches only
    // whoever is reading THIS node's log -- and an operator who opened a window and
    // walked away has no other way to find out.
    //
    // Both halves together, and both absent on a node that runs no window: a count with
    // no state beside it cannot say whether a zero means nobody has found the window yet
    // or that there is no window, which are opposite facts.
    if (_sources.enrollment != nullptr)
    {
        auto const [state, pending] = _sources.enrollment->Summary();
        fields.runtime.enrollment = state;
        fields.runtime.enrollmentPending = pending;
    }

    // How many client tombstones this node is ENFORCING (#1471). Absent on a node with no
    // source, which is a node running no consensus: there is no committed set for a forget to
    // have reached, and a `0` there would answer a different question than "the cluster forgets
    // nobody". Read per request like the rest of this record, so a forget applied a moment ago
    // is visible on the next `--node-status` rather than at the next restart.
    if (_sources.membership != nullptr)
        fields.runtime.forgottenClients = static_cast<std::uint32_t>(_sources.membership->ForgottenClientCount());

    // Where peers DIAL this node's consensus port, beside the surfaces it BOUND below
    // (#1328). The operator bringing a machine in compares this against the endpoint
    // `--cluster-admit` echoed, and the bound raft port is routinely the wildcard -- so
    // this is the half of that comparison nothing else reports.
    //
    // Only an address travels. `NoConsensus` is ABSENT, not an empty string; `Unstated` is
    // refused by the startup table and by every reload, so a serving node cannot be in
    // it -- and were it, absent is the honest reading of an address nobody stated.
    if (auto dial = ConsensusDialAddressOf(_cfg); dial.has_value())
        fields.runtime.consensusEndpoint = std::move(*dial);

    // Where this node sits in the configuration consensus holds (#1449): a learner and a
    // following voter are one role and one component mask, and only one of them stands
    // when the leader goes. Absent on a node with no source, which is a node running no
    // consensus -- and on one whose tier is not attached yet or already gone, where
    // absent is the honest answer about a configuration nothing can be asked for.
    if (_sources.consensus != nullptr)
        if (auto const standing = _sources.consensus->CurrentStanding(); standing.has_value())
            fields.runtime.consensusStanding = WireStandings[static_cast<std::size_t>(*standing)].tag;

    // Every row of the node's condition table, whatever its state (#1364), read per request like
    // the rest of this record: a window closed a moment ago is `clear` on the next answer, which
    // is what an operator watching a live row is waiting to see.
    if (_sources.conditions != nullptr)
        fields.runtime.conditions = _sources.conditions->Snapshot();

    // The roster, when this node holds one (#178). Its lapse travels as an INSTANT rather than a
    // countdown, so a reader far from this node renders it against its own clock and a copy
    // taken a minute ago still says when it lapses rather than how long it had then.
    if (auto const summary = _sources.roster != nullptr ? _sources.roster->Summary() : std::nullopt)
        fields.runtime.roster = CompileCacheWire::NodeRosterFields {
            .version = summary->version,
            .voters = summary->voters,
            .principals = summary->principals,
            .revoked = summary->revoked,
            .certifiedUntilMillis = summary->certifiedUntil.transform([](std::chrono::system_clock::time_point until) {
                return static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(until.time_since_epoch()).count());
            }),
        };

    for (auto const& mapping: mappings)
    {
        // **A surface the configuration does not resolve is ABSENT, never a zero port.**
        // Absent is not zero: a client must be able to tell *this node runs no admin
        // surface* from *it runs one whose port I could not read*, and a 0 renders as a
        // dialable-looking number in every format that carries it.
        auto const endpoints = RowFor(mapping.surface).Resolve(_cfg);
        if (endpoints.empty())
            continue;
        // The row's FIRST endpoint. Discovery resolves two -- a shared listen socket and
        // a private reply socket -- and the one worth reporting is the one a peer sends
        // to, which is the row's own order.
        fields.surfaces.push_back(
            CompileCacheWire::SurfaceReport { .surface = mapping.tag,
                                              .port = static_cast<std::uint32_t>(endpoints.front().port),
                                              .tls = mapping.surface == NodeSurface::Admin && adminServesTls });
    }
    return fields;
}

} // namespace FastCache::Node
