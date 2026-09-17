// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CacheProtocol.hpp>
#include <Dispatch.hpp>
#include <tests/ScriptedSocket.hpp>

/// A notice these cases do not inspect.
///
/// Shared on purpose: every case here asserts the OUTCOME's `credentialIgnored`
/// flag, not the diagnostic, and a fresh object per call would imply they cared. The
/// cases that do care build their own recording notice, because a shared one reports
/// once and would let whichever case ran first silence the rest -- a coupling to
/// Catch2's ordering that is invisible until it fails.
/// @return A notice with no sink.
///
/// **`inline`, and it has to be.** This is a non-member function DEFINED in a header, so two
/// translation units including `FleetHarness.hpp` are two definitions and the link fails --
/// `multiple definition of Unwatched()`. It went unnoticed because the harness had exactly ONE
/// includer until #1471 added a second, which is the shape of every latent ODR violation in a
/// header: correct-looking, and only discoverable by using the header twice.
///
/// `inline` rather than `static`: the comment above says the notice is SHARED on purpose, and
/// internal linkage would give each translation unit its own. Harmless for a sink-less notice
/// today, and a silent divergence from the stated intent the moment one records anything.
[[nodiscard]] inline FastCache::Cc::CredentialNotice& Unwatched()
{
    static FastCache::Cc::CredentialNotice notice = FastCache::Cc::CredentialNotice::Silent();
    return notice;
}

namespace FastCache::Testing
{

/// A compile fleet running deterministically in one process.
///
/// Test infrastructure, header-only and never linked into the library — the same
/// shape as `Consensus/RaftClusterHarness.hpp`, which is the model, and the same
/// argument for existing.
///
/// ## Why this exists rather than more unit tests
///
/// `SchedulerService`, `WorkerRegistry` and `LeaseTable` each have cases pinning
/// their rules in isolation, and that is not the same as the fleet being right.
/// Several rules in `.agent/rules/distributed-compilation.md` are about a
/// **sequence** across two machines — a lease granted by one scheduler and
/// resolved against that same one after leadership has moved — and no
/// single-transition test reaches an interleaving. The only other way to arrange
/// one today is a script that spawns processes, binds ports and waits on a
/// wall clock, which cannot schedule the interleaving at all: it can only hope
/// for it.
///
/// ## Why it lives in `src/tests/`
///
/// By shape it belongs beside `RaftClusterHarness`. It does not go there.
/// `RaftClusterHarness` sits in `Consensus/` because everything it touches sits
/// in `Consensus/`; a fleet spans `Distributed/`, the node and the launcher's
/// client sources, so putting it under `src/FastCache/` would make a library
/// directory include an app header. This tree already enforces that `Net/` must
/// not depend on `Core/` with a ctest; library-depending-on-app is the worse
/// version of the same thing and has no gate to catch it.
///
/// ## What is real here and what is not
///
/// Real: `SchedulerService` and `SchedulerProtocol` decide and frame every reply;
/// the launcher's own `Cc::ExchangeFramed` writes and reads the client side;
/// `Cc::Dispatch` runs unmodified, so the lease-redirect and release-routing
/// logic under test is the shipped logic.
///
/// Not real: there is no socket and no worker process. A request is handed to the
/// addressed scheduler's protocol object, and its answer is replayed to the
/// client through a `ScriptedSocket`. The worker's endpoint answers whatever
/// `SetWorkerReply` was given.
///
/// Time moves only in `Step`.
class FleetHarness final: public Cc::IEndpointExchange
{
  public:
    /// What one exchange did, in the order it happened.
    struct Call
    {
        std::string endpoint; ///< Who was asked. Usually the fact under test.
        std::uint8_t opRaw;   ///< Which verb, as the byte on the wire.
        /// How it ended. Recorded because "the client asked the right machine" and
        /// "the right machine did the thing" are separate facts, and a case that
        /// checks only the first cannot tell a resolved lease from a refused one.
        Cc::CacheOutcomeKind kind;
        /// The refusal, meaningful only when @ref kind is `Rejected`.
        CompileCacheWire::ErrorCode code;
    };

    /// The fleet every node in this harness belongs to.
    ///
    /// One id across all of them, because a grant minted by any of these schedulers
    /// must verify on any of these workers -- that is what the harness exists to
    /// drive. Named rather than empty so the comparison is a real one (#322).
    static constexpr std::string_view ClusterId = "fleet-harness";

    /// @param signingKey The cluster key every scheduler here signs with. Empty —
    ///        the default — means unsigned grants, which is what a fleet with no
    ///        `--cluster-key-file` runs and is the simpler thing to assert against.
    explicit FleetHarness(SecureByteBuffer signingKey = {}):
        _signingKey { std::move(signingKey) }
    {
        // A worker that refuses is the default because the release is the subject
        // here, and the rule under test is that it happens on EVERY path out of the
        // compile -- a refused job included. A test wanting a successful compile
        // says so with `SetWorkerReply`.
        _workerReply = CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::NoCapacity, "harness worker");
    }

    /// Add a scheduler at @p endpoint.
    ///
    /// It starts as a follower knowing no leader, because that is what a node that
    /// has not yet heard from consensus is. `ElectLeader` is what makes a fleet.
    /// @param endpoint How clients address it, e.g. `"sched-a:6676"`.
    void AddScheduler(std::string endpoint)
    {
        auto node = std::make_unique<Node>(*this, std::move(endpoint));
        _nodes.push_back(std::move(node));
    }

    /// Make @p endpoint the leader and every other scheduler its follower.
    ///
    /// One call, both halves: a test that set only the new leader would leave the
    /// old one still answering as leader, which is a fleet no election produces
    /// and would let a wrong client pass.
    /// @param endpoint The scheduler that now leads; must have been added.
    void ElectLeader(std::string_view endpoint)
    {
        (void) NodeAt(endpoint); // refuse an endpoint nobody added, loudly
        for (auto const& node: _nodes)
            if (node->endpoint == endpoint)
                node->service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
            else
                node->service.SetRole(Distributed::SchedulerRole::Follower, endpoint, Distributed::StandaloneSchedulerTerm);
    }

    /// Where every subsequent request appears to come from.
    ///
    /// **A constant here cannot carry a membership property.** The default is `127.0.0.1`, and
    /// loopback is never forgotten -- deliberately, so a node always admits its own machine --
    /// so a forget published against the default caller applies to nobody and a case asserting
    /// a refusal goes green having exercised nothing. Any case about admission sets this first.
    /// @param host The caller's host, as `ISocket::PeerAddress()` would report it.
    void SetCallerHost(std::string host)
    {
        _callerHost = std::move(host);
    }

    /// Let one node's admission oracle decide what its callers are.
    ///
    /// **Per NODE, and that is the point rather than generality.** The state #1471 is about is
    /// two machines disagreeing about one host -- a forget committed on the leader and not yet
    /// applied on the second -- and one oracle shared across the fleet cannot express it at all.
    ///
    /// A node given no oracle keeps answering `Member`, so every case written before this still
    /// asserts what it always did. The oracle is borrowed, not owned: the production objects
    /// belong to the case, because a `NodeMembership` needs a config and a logger this harness
    /// has no business inventing.
    /// @param endpoint Which node; must have been added.
    /// @param oracle Who decides its callers, or nullptr to go back to admitting everybody.
    void SetMembershipAt(std::string_view endpoint, Distributed::IMembershipOracle const* oracle)
    {
        NodeAt(endpoint).membership = oracle;
    }

    /// Register a worker with one scheduler.
    /// @param scheduler Which scheduler's registry to put it in.
    /// @param workerEndpoint Where the worker answers compiles.
    /// @param fingerprint The toolchain it serves.
    /// @param slots How many jobs it will take at once.
    void RegisterWorker(std::string_view scheduler,
                        std::string_view workerEndpoint,
                        std::string_view fingerprint,
                        std::uint32_t slots = 1)
    {
        auto const reply = NodeAt(scheduler).service.Register(
            // `SetupCaller`, never the case's caller: this ARRANGES the fleet and throws when
            // refused, so a case that set a forgotten caller host would fail here during setup
            // rather than at the assertion it was written for -- which reads as the harness
            // being broken. Registration IS membership-gated in production (`Gate` refuses
            // `Register` too); what this says is that the worker doing the registering is not
            // the client under test. Do not unify these two.
            SetupCaller(),
            Distributed::WorkerRegistration {
                .fingerprint = fingerprint, .endpoint = workerEndpoint, .slots = slots, .codecs = {} });
        if (reply.status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a worker registration" };
        _workerEndpoints.emplace_back(workerEndpoint);
    }

    /// Announce a MACHINE to one scheduler, as `NodeAnnounce` does.
    ///
    /// **The verb a node sends whatever components it runs**, which is why this is beside
    /// `RegisterWorker` rather than folded into it: a machine with `--slots=0` sends this and
    /// never registers anything, and a harness that could only express a registration could
    /// not build the fleet
    /// [#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440) is about -- a
    /// scheduler-only leader whose own row and own history the page has to carry.
    /// @param scheduler Which scheduler to tell.
    /// @param machineEndpoint The machine announcing itself.
    /// @param history Closed buckets it is handing over; empty is the ordinary case.
    void AnnounceMachine(std::string_view scheduler,
                         std::string_view machineEndpoint,
                         std::span<Distributed::FleetBucket const> history = {})
    {
        auto const reply = NodeAt(scheduler).service.AnnounceNode(
            // `SetupCaller` for `RegisterWorker`'s reason: this ARRANGES the fleet, so a case
            // that set a refused caller host must fail at its own assertion rather than here.
            SetupCaller(),
            Distributed::NodePresence { .endpoint = machineEndpoint,
                                        .version = "harness",
                                        .capacity = Distributed::NodeCapacity { .logicalCores = 8 },
                                        .load = Distributed::NodeLoad {} },
            history);
        if (reply.status != CompileCacheWire::Status::Ok)
            throw std::runtime_error { "FleetHarness: the scheduler refused a machine announcement" };
    }

    /// The Machines rows one scheduler would draw.
    ///
    /// `NodeReports()` and never the worker entries, because that is what the page walks: a
    /// machine serving two toolchains is two registry entries and ONE row, and a machine
    /// running no worker is no entry at all and still a row.
    /// @param scheduler Which scheduler to ask.
    /// @return One report per machine it knows about.
    [[nodiscard]] std::vector<Distributed::NodeReport> MachinesAt(std::string_view scheduler)
    {
        return NodeAt(scheduler).service.Workers().NodeReports();
    }

    /// Where @p scheduler files what other machines hand it.
    ///
    /// Borrowed, and the case owns it: a history store needs paths and a logger this harness
    /// has no business inventing -- `SetMembershipAt`'s arrangement, for its reason.
    /// @param scheduler Which scheduler.
    /// @param sink Where handed-over buckets go, or null to discard them.
    void SetHistorySinkAt(std::string_view scheduler, Distributed::IFleetHistorySink* sink)
    {
        NodeAt(scheduler).service.SetHistorySink(sink);
    }

    /// Advance every clock in the fleet.
    ///
    /// The only way time moves. A lease expiry, a heartbeat age and a grant's
    /// absolute deadline are all read from these, so a test states the passage of
    /// time rather than sleeping for it.
    /// @param by How far.
    void Step(std::chrono::milliseconds by)
    {
        _clock.Advance(by);
        _wallClock.Advance(by);
    }

    /// Run @p hook the next time a compile is sent to a worker.
    ///
    /// **This is what makes an interleaving arrangeable.** `Cc::Dispatch` leases,
    /// compiles and releases in one call, so a test cannot get between the grant
    /// and the release from outside — and the moment between them is exactly where
    /// leadership moving is interesting. The hook fires inside the compile
    /// exchange, on the caller's thread, deterministically.
    /// @param hook What to do; cleared after it fires.
    void OnCompile(std::function<void()> hook)
    {
        _onCompile = std::move(hook);
    }

    /// What a worker endpoint answers a COMPILE with.
    /// @param reply A complete reply frame.
    void SetWorkerReply(std::vector<std::byte> reply)
    {
        _workerReply = std::move(reply);
    }

    /// Every exchange the fleet has served, in order.
    /// @return The log.
    [[nodiscard]] std::vector<Call> const& Calls() const noexcept
    {
        return _calls;
    }

    /// Whether @p scheduler still has @p key marked as being built.
    /// @param scheduler Which scheduler to ask.
    /// @param key The object key.
    /// @return True while a live lease suppresses it there.
    [[nodiscard]] bool IsInFlight(std::string_view scheduler, std::string_view key)
    {
        return NodeAt(scheduler).service.Leases().IsInFlight(key);
    }

    /// The fleet's counters, shared by every scheduler in it.
    /// @return The sink.
    [[nodiscard]] AtomicMetricsSink& Metrics() noexcept
    {
        return _metrics;
    }

    /// Answer one exchange, from whichever endpoint it was addressed to.
    /// @param hostPort The endpoint the client chose. **This is the fact most of
    ///        these tests are about**: which machine the client decided to ask.
    /// @param frame The complete request.
    /// @param credential Ignored; see the class comment.
    /// @param budget Ignored; nothing here blocks.
    /// @return The outcome, decoded by the launcher's own client code.
    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& credential,
                                            Cc::ExchangeBudget budget) override
    {
        (void) budget;
        // Logged BEFORE the answer, so the log is in the order requests were SENT --
        // an exchange nested inside this one's compile hook would otherwise appear
        // to have happened first. Its outcome is filled in below, by index rather
        // than by reference, because that nested call can reallocate the vector.
        auto const slot = _calls.size();
        _calls.push_back(Call { .endpoint = std::string { hostPort },
                                .opRaw = OpOf(frame),
                                .kind = Cc::CacheOutcomeKind::Transport,
                                .code = CompileCacheWire::ErrorCode::MalformedFrame });

        auto reply = Answer(hostPort, frame);
        // The launcher's own framing, over a socket that replays what the addressed
        // scheduler actually produced. Asserting against hand-written reply bytes
        // would let the two ends drift apart independently, which is the failure
        // #340 was.
        ScriptedSocket socket { std::move(reply) };
        auto outcome = SyncRun(Cc::ExchangeFramed(&socket, &Unwatched(), std::move(frame), credential));

        _calls[slot].kind = outcome.kind;
        _calls[slot].code = outcome.code;
        return outcome;
    }

  private:
    /// One scheduler and everything it owns.
    struct Node
    {
        /// @param harness The fleet it belongs to, for the shared clocks and sink.
        /// @param at How clients address it.
        Node(FleetHarness& harness, std::string at):
            endpoint { std::move(at) },
            service { harness._clock,
                      harness._wallClock,
                      harness._metrics,
                      harness._logger,
                      harness._signingKey,
                      // One fleet, so one cluster id across every node the harness
                      // builds: a grant minted by any of them must verify on any
                      // other, which is what the harness exists to exercise (#322).
                      FleetHarness::ClusterId },
            protocol { service, harness._metrics }
        {
        }

        std::string endpoint;
        Distributed::SchedulerService service;
        Distributed::SchedulerProtocol protocol;
        /// Who decides this node's callers, or null to admit everybody (the default, and what
        /// every case predating #1471 relies on). Borrowed -- see `SetMembershipAt`.
        Distributed::IMembershipOracle const* membership { nullptr };
    };

    /// The scheduler at @p endpoint.
    /// @param endpoint Who to find.
    /// @return The node.
    /// @throws std::runtime_error when nothing was added at that endpoint — a
    ///         silent miss would make a routing test pass for the wrong reason.
    [[nodiscard]] Node& NodeAt(std::string_view endpoint)
    {
        for (auto const& node: _nodes)
            if (node->endpoint == endpoint)
                return *node;
        throw std::runtime_error { "FleetHarness: no scheduler at " + std::string { endpoint } };
    }

    /// The reply bytes for one request, from whoever it was addressed to.
    /// @param hostPort The addressed endpoint.
    /// @param frame The request.
    /// @return What that endpoint answers.
    [[nodiscard]] std::vector<std::byte> Answer(std::string_view hostPort, std::span<std::byte const> frame)
    {
        auto const worker = std::ranges::find(_workerEndpoints, hostPort);
        if (worker == _workerEndpoints.end())
        {
            auto& node = NodeAt(hostPort);
            return node.protocol.Answer(frame, Caller(node));
        }

        // A worker endpoint. The hook fires here rather than around the whole
        // Dispatch call because this is the only instant that is *between* the
        // grant and the release.
        if (_onCompile)
        {
            auto const hook = std::exchange(_onCompile, {});
            hook();
        }
        return _workerReply;
    }

    /// The context the harness ARRANGES the world with: a member on loopback, always.
    ///
    /// Separate from `Caller` deliberately. Setup helpers that throw on refusal -- registering a
    /// worker, for one -- must not be subject to the membership a case is testing, or arranging
    /// the fleet fails before the assertion runs.
    /// @return A member calling from loopback.
    [[nodiscard]] static Distributed::CallerContext SetupCaller() noexcept
    {
        return Distributed::CallerContext { .membership = Distributed::Membership::Member, .peerId = "127.0.0.1" };
    }

    /// The context handed to @p node's service for the current caller.
    ///
    /// **Decided by that node's oracle when it has one**, so a membership property is
    /// falsifiable here at all: before #1471 this returned a hardcoded `Member`, which made any
    /// case about who is admitted pass against an oracle that refused nobody.
    ///
    /// No oracle means `Member`, which is what every case about dispatch, leases and release
    /// routing needs -- those are not about admission and would be testing nothing else if this
    /// started refusing them.
    /// @param node Whose oracle to ask.
    /// @return The context, carrying that node's verdict about `_callerHost`.
    [[nodiscard]] Distributed::CallerContext Caller(Node const& node) const
    {
        auto const verdict =
            node.membership != nullptr ? node.membership->Classify(_callerHost) : Distributed::Membership::Member;
        return Distributed::CallerContext { .membership = verdict, .peerId = _callerHost };
    }

    /// The verb byte a framed request carries, for the call log.
    ///
    /// Raw rather than an `Op`, because a frame this build does not know is
    /// exactly the thing worth seeing in a log of what a client sent. A test
    /// compares against `static_cast<std::uint8_t>(Wire::Op::Lease)`.
    /// @param frame The request.
    /// @return Its opcode byte, or `0xFF` when the frame carries no header.
    [[nodiscard]] static std::uint8_t OpOf(std::span<std::byte const> frame)
    {
        auto const header = CompileCacheWire::DecodeRequestHeader(frame);
        return header.has_value() ? header->opRaw : std::uint8_t { 0xFF };
    }

    ManualClock _clock;
    ManualWallClock _wallClock;
    AtomicMetricsSink _metrics;
    NullLogger _logger;
    /// Where requests appear to come from. Loopback by default, which is what every case
    /// predating #1471 assumed and what a node always admits.
    std::string _callerHost { "127.0.0.1" };
    SecureByteBuffer _signingKey;
    std::vector<std::unique_ptr<Node>> _nodes;
    std::vector<std::string> _workerEndpoints;
    std::vector<std::byte> _workerReply;
    std::vector<Call> _calls;
    std::function<void()> _onCompile;
};

} // namespace FastCache::Testing
