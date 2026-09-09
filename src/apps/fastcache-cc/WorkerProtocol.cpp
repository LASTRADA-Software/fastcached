// SPDX-License-Identifier: Apache-2.0
#include "CacheProtocol.hpp"
#include "CodecEnvelope.hpp"
#include "Dispatch.hpp"
#include "WorkerProtocol.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What one refusal means on the wire and in the metrics.
    ///
    /// Both in one row, deliberately. They are answers to the same question asked
    /// by two audiences -- the client that has to decide whether to retry, and the
    /// operator watching a fleet -- and a refusal counted under one reason while
    /// being reported as another is worse than not counting it at all. Splitting
    /// them across a `switch` and a second `switch` is how that happens.
    ///
    /// No member carries a default, deliberately. A row answering only two of the
    /// three questions is not a row -- and `ErrorCode` has no zero enumerator to
    /// default to in the first place, so `{}` there names a value the enum does not
    /// have. Omitting the initializers makes the compiler ask for all three, which
    /// is the property the table wanted anyway.
    struct RefusalDescriptor
    {
        JobRefusal refusal;            ///< The reason this row describes.
        Wire::ErrorCode code;          ///< What the client is told.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
    };

    /// One row per `JobRefusal`, in enumerator order.
    ///
    /// The codes are separate rather than collapsed, because collapsing them was
    /// actively misleading: `ScratchUnavailable` and `SpawnFailed` both used to
    /// answer `StorageWriteFailed`, so a worker with no storage told the client
    /// "storage write failed" and two genuinely different operator problems -- an
    /// unwritable scratch disk, and a toolchain that is configured but cannot be
    /// executed -- were indistinguishable from either end. Found the hard way,
    /// diagnosing a CI failure that reported the one thing it could not possibly
    /// be.
    constexpr EnumTable<JobRefusal, RefusalDescriptor> RefusalTable { {
        { .refusal = JobRefusal::UnknownFingerprint,
          .code = Wire::ErrorCode::FingerprintMismatch,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint },
        { .refusal = JobRefusal::RejectedArgument,
          .code = Wire::ErrorCode::MalformedFrame,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedRejectedArgument },
        { .refusal = JobRefusal::ScratchUnavailable,
          .code = Wire::ErrorCode::WorkerScratchUnavailable,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedScratchUnavailable },
        { .refusal = JobRefusal::SpawnFailed,
          .code = Wire::ErrorCode::WorkerSpawnFailed,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedSpawnFailed },
        { .refusal = JobRefusal::ToolchainSurveyInFlight,
          .code = Wire::ErrorCode::WorkerToolchainSurveyInFlight,
          .counter = IMetricsSink::Counter::WorkerJobsRefusedSurveyInFlight },
    } };

    static_assert(RowsInEnumeratorOrder(RefusalTable, &RefusalDescriptor::refusal),
                  "RefusalTable must hold one row per JobRefusal, in enumerator order");

    /// The row describing `refusal`.
    /// @param refusal What the runner reported.
    /// @return Its descriptor.
    [[nodiscard]] constexpr RefusalDescriptor const& DescriptorFor(JobRefusal refusal) noexcept
    {
        return RefusalTable[static_cast<std::size_t>(refusal)];
    }

    /// This surface's rows. The shape, the lookup and why they exist are on
    /// `Wire::RefusedVerb`; what belongs here is only which verbs and what they say.
    ///
    /// `Auth`, because a `--requirepass` worker was refused at `REGISTER` and never
    /// joined the fleet at all -- absent rather than idle, which is harder to notice.
    constexpr SurfaceRefusal UnsupportedVersion {
        .code = Wire::ErrorCode::UnsupportedVersion,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnsupportedVersion,
    };
    constexpr SurfaceRefusal TruncatedFrame {
        .code = Wire::ErrorCode::MalformedFrame,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedTruncated,
    };
    constexpr SurfaceRefusal UnknownOpcode {
        .code = Wire::ErrorCode::UnknownOpcode,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnknownOpcode,
    };
    constexpr SurfaceRefusal UnimplementedVerb {
        .code = Wire::UnimplementedVerb,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedUnimplementedVerb,
    };
    constexpr SurfaceRefusal NotPermitted {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedNotPermitted,
    };
    constexpr SurfaceRefusal MalformedPayload {
        .code = Wire::ErrorCode::MalformedFrame,
        .counter = IMetricsSink::Counter::WorkerFramesRefusedMalformedPayload,
    };

    constexpr std::array RefusedVerbs {
        Wire::RefusedVerb { .op = Wire::Op::Auth,
                            .code = Wire::UnimplementedVerb,
                            .why = "this endpoint compiles and checks no credential" },
    };

    // The table is consulted only on the path a verb other than COMPILE takes, so a
    // row naming COMPILE would sit there looking like a decision and change nothing.
    // Refused at compile time rather than left to be noticed.
    static_assert(std::ranges::none_of(
                      RefusedVerbs, [](Wire::Op op) { return op == Wire::Op::Compile; }, &Wire::RefusedVerb::op),
                  "a refusal row for COMPILE is dead: the lookup never reaches it");
} // namespace

LeaseValidator SignedLeaseValidator(SecureByteBuffer signingKey,
                                    std::string advertisedEndpoint,
                                    WallClockRef clock,
                                    Distributed::WorkerLeaseState& lease,
                                    IMetricsSink& metrics,
                                    std::chrono::seconds slack)
{
    // `clock` by VALUE: a `WallClockRef` IS the borrow, so copying it into the closure
    // carries the guard rather than re-binding a reference. This capture was `&clock`,
    // and it is the retention a member scan cannot see -- the validator outlives this
    // call and nothing anywhere declares a wall-clock member for it (#1032).
    return [key = std::move(signingKey), endpoint = std::move(advertisedEndpoint), clock, &lease, &metrics, slack](
               std::string_view token, std::string_view fingerprint) -> LeaseDecision {
        // The fingerprint is the one the REQUEST names, and this runs BEFORE anything
        // has checked that this worker serves it -- `CompileJobRunner::Run` answers
        // that later, with `UnknownFingerprint`. So the two comparisons compose rather
        // than one presupposing the other: the grant must name what the request names,
        // and the request must name something served. Together that stops one
        // toolchain's lease from paying for another's compile; neither does it alone,
        // which is why the grant's fingerprint check is not redundant with the
        // worker's.
        // Read ONCE and used for both the verification and the spend, because those two
        // have to agree about when this is: a token that verified as unexpired against
        // one reading and was then filed against a later one would be remembered under a
        // deadline the check never saw. It is also one syscall rather than two on the
        // path of every dispatched compile.
        auto const now = clock.Now();

        // The fleet is READ per request rather than captured at construction, because
        // this validator is built at startup and the identity arrives later, in the
        // REGISTER reply (#401). Absent means this worker has not registered, and a
        // worker that does not know its fleet honours no grant -- which is the window
        // the ticket closes. An engaged but EMPTY identity is a scheduler that names
        // no cluster, is legal, and expects a grant that names none either.
        auto const cluster = lease.fleet.Pinned();
        //
        // Answered BEFORE the MAC, and that is not the oracle the MAC-first rule
        // guards against: this reports a fact about THIS WORKER, not about the token,
        // so a caller learns nothing about a grant it did not already hold. `detail`
        // is left empty for the same reason -- there is no authenticated fact to name.
        if (!cluster.has_value())
            return LeaseDecision { .refusal =
                                       Distributed::LeaseRefusal { .reason = Distributed::LeaseRefusalReason::Unregistered,
                                                                   .detail = {} },
                                   .remaining = std::nullopt };

        // ONE slack, reaching all THREE sites from this one parameter: the verification
        // below, the spend after it, and the budget this returns. The shared predicate
        // makes them agree on the COMPARISON; one value is what makes them agree on the
        // WINDOW, and it is the window a caller with a non-default slack would split.
        // That caller is no longer hypothetical: it is a parameter, defaulted to the
        // production value, so the window can be narrowed to something a test can
        // outrun. Without it nothing could ever observe a job outliving its grant -- a
        // verifying grant has at least `slack` of budget by construction, so the
        // refusal the caller draws from `remaining` would be unreachable and untested.
        auto verified = Distributed::VerifyLeaseToken(
            key,
            token,
            Distributed::LeaseExpectation { .endpoint = endpoint, .fingerprint = fingerprint, .clusterId = *cluster },
            now,
            slack);
        if (verified.has_value())
        {
            // **Spent here, after every reading of the token and before anything is
            // learned from it** (#614). A lease is single-use by construction -- the
            // scheduler mints one grant per lease, the client presents it in exactly one
            // COMPILE frame with no retry, and a RELEASE goes to the scheduler rather
            // than here -- so a second arrival is a replay and never an honest client.
            //
            // The order is the whole safety argument for the line below it. If the term
            // were adopted first, a captured grant naming an old term would walk this
            // worker's picture of the fleet backwards on demand; spending first means
            // only a grant that has never been used can move it, which is what makes a
            // lower term unambiguously a scheduler that was legitimately reset rather
            // than a replay.
            // **Every refusal a client would RETRY on is decided before this runs**, and
            // that is load-bearing rather than incidental. `CompileResponder` answers
            // `Stopping`, `NoCapacity` and `EndpointBusy` -- the three a client is told
            // to come back from -- before this validator is reached at all. Refusals
            // BELOW this point do burn the grant, which costs nothing today because
            // `Dispatch::CompileOnWorker` sends one frame and never re-presents a token;
            // the day a client retries one of them, or the slot and byte-budget checks
            // move inside this function, that retry becomes a permanent `Replayed` and
            // distribution stops with this counter blaming an attacker.
            if (!lease.spent.Spend(token, verified->expiresAt, now, slack))
                return LeaseDecision {
                    .refusal = Distributed::LeaseRefusal { .reason = Distributed::LeaseRefusalReason::Replayed,
                                                           .detail = "this lease has already been spent at this worker; a "
                                                                     "grant authorizes exactly one compile" },
                    .remaining = std::nullopt
                };

            // **The second learning channel, and it works when the first does not.**
            // A grant naming a later term than this worker knows is adopted -- and it
            // is safe to adopt precisely BECAUSE this line sits after the MAC
            // verified: `VerifyLeaseToken` authenticates before it reports on any
            // claim, so nothing an unauthenticated sender writes can move this
            // number. Reading the term off a refused token would let anybody holding
            // the wire push a worker's expectation up and make it refuse every honest
            // grant afterwards.
            //
            // It also makes the refusal in test terms observable at all: without
            // adoption, a validator that accepted everything and one that adopts
            // forward are indistinguishable from outside.
            // A term that went BACKWARDS is a scheduler somebody reset -- its Raft
            // directory wiped, the cluster re-bootstrapped, consensus turned off -- and
            // it is ADOPTED rather than refused since #614. Reported both ways round,
            // because the two audiences differ: the line is for whoever is reading this
            // worker's log after a fleet stopped behaving, the counter is for whoever is
            // looking at a dashboard and never reads a log.
            //
            // ONE predicate, and it belongs to the notice. Testing the transition here
            // and again inside `Observe` would be one decision made in two places, and
            // whichever copy is edited next is the one that stops agreeing.
            if (lease.notice.Observe(lease.term.Learn(verified->epoch)))
                metrics.Increment(IMetricsSink::Counter::WorkerSchedulerTermRegressions);
            // The bound travels out. It is the ONLY authenticated statement anybody has
            // about when the fleet stops wanting this answer, and until #522 it stopped
            // here: the surface below went on to spend minutes compiling against a
            // constant, so a cluster that had agreed on a different lifetime was obeyed
            // by the scheduler and the client and ignored by the machine doing the work.
            //
            // Derived HERE, from the `now` this validator already read, so the caller
            // needs no clock of its own.
            //
            // **With the skew slack, and dropping it would reintroduce the exact defect
            // the slack exists to prevent.** The obvious reading -- a budget should be
            // the grant's own expiry, since the slack is about a boundary -- is wrong
            // here, and a case in this file already says so: a worker whose clock is
            // minutes fast computes a NEGATIVE remaining for every job it was
            // legitimately granted, and would refuse them all, on exactly the machines
            // nobody is watching. Moving that refusal from before the compile to after
            // it makes it worse rather than better, since the work is done first.
            //
            // So this is the same window the verifier accepted on, and the cost is
            // stated rather than hidden: a compile is abandoned `slack` past the point
            // the scheduler reclaimed the key, not at it.
            return LeaseDecision { .refusal = std::nullopt,
                                   .remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       (verified->expiresAt + slack) - now) };
        }

        // The CLAIMS are dropped deliberately -- what a worker needs from a grant is
        // permission, and the object key inside it is the SCHEDULER's bookkeeping, so
        // a worker comparing it against what it was asked to compile would be
        // re-deriving a decision it is not the one making. The refusal travels whole,
        // diagnostic included: that string was formatted for this caller and used to
        // be allocated and dropped.
        return LeaseDecision { .refusal = std::move(verified.error()), .remaining = std::nullopt };
    };
}

LeaseValidator UncheckedLeaseValidator()
{
    return [](std::string_view, std::string_view) {
        // No refusal AND no bound, which are two separate facts rather than one
        // permissive answer. This worker holds no key, so it has authenticated nothing
        // and can state no bound -- and a caller that read a disengaged `remaining` as
        // "no time left" would refuse every compile on the single-machine install this
        // validator exists for. `LeaseDecision::remaining` carries that distinction.
        return LeaseDecision { .refusal = std::nullopt, .remaining = std::nullopt };
    };
}

WorkerProtocol::WorkerProtocol(ICompileJobRunner& jobs,
                               LeaseValidator validator,
                               Wire::CodecList acceptedCodecs,
                               IMetricsSink& metrics,
                               std::size_t maxDecompressedBytes):
    _jobs { jobs },
    _validator { std::move(validator) },
    _acceptedCodecs { std::move(acceptedCodecs) },
    _metrics { metrics },
    _maxDecompressedBytes { maxDecompressedBytes }
{
}

std::size_t DeclaredRequestFootprint(std::span<std::byte const> frame) noexcept
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        return 0;

    // What `Serve` already charged, and the floor for everything below: a frame this
    // function cannot look inside still costs its own length.
    auto const framed = static_cast<std::size_t>(header->payloadLength);

    // Every one of these is a request `Answer` refuses on its own terms -- a foreign
    // verb, a truncated frame, a payload that does not split. None of them reaches
    // `Unenvelope`, so none of them declares a second buffer, and answering `framed`
    // leaves the refusal where it belongs rather than turning a malformed frame into
    // a busy signal.
    //
    // The length test subtracts rather than adds: `RequestHeaderSize + framed` wraps
    // where `size_t` is 32 bits, and a wrapped sum turns "this frame is truncated"
    // into "this frame is long enough" and hands `subspan` a count past the end.
    // `DecodeRequestHeader` already refused a buffer shorter than the header, so the
    // subtraction cannot underflow.
    if (header->opRaw != static_cast<std::uint8_t>(Wire::Op::Compile) || frame.size() - Wire::RequestHeaderSize < framed)
        return framed;

    auto const fields = Wire::DecodeCompilePayload(frame.subspan(Wire::RequestHeaderSize, framed));
    if (!fields.has_value())
        return framed;

    auto const envelope = Wire::DecodeCodecEnvelope(fields->source);
    if (!envelope.has_value())
        return framed;

    // The declared expansion, believed only as a PRICE. Whether it is one this
    // endpoint will pay at all is `Unenvelope`'s ceiling, checked where the
    // allocation happens; this decides what it is charged for having asked.
    return std::max(framed, static_cast<std::size_t>(envelope->rawLength));
}

std::optional<std::vector<std::byte>> WorkerProtocol::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: the peer is not speaking this protocol at all, so there is no
        // framing in which a reply would be meaningful. The one case that closes.
        return std::nullopt;

    if (!Wire::IsSupported(header->version))
        return Refuse(_metrics, UnsupportedVersion, {});

    if (frame.size() < Wire::RequestHeaderSize + header->payloadLength)
        return Refuse(_metrics, TruncatedFrame, "frame shorter than its declared payload");
    auto const payload = frame.subspan(Wire::RequestHeaderSize, header->payloadLength);

    auto const* const descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        return Refuse(_metrics, UnknownOpcode, {});

    if (descriptor->code != Wire::Op::Compile)
    {
        // The wire row carries the code and the words; the counter is paired here,
        // because `RefusedVerb` cannot reach `IMetricsSink`. Asserted rather than
        // assumed: every row of `RefusedVerbs` answers `UnimplementedVerb` today, and
        // a row that answered something else would be counted under a name that no
        // longer described it.
        if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
            return Refuse(_metrics, SurfaceRefusal { .code = row->code, .counter = UnimplementedVerb.counter }, row->why);

        // A worker is not a scheduler and not a cache. Refused with a reply rather
        // than a close, so a client that sent the wrong verb to the wrong port
        // learns which -- a dropped connection is indistinguishable from a dead host.
        return Refuse(_metrics, NotPermitted, "this endpoint compiles; it does not schedule or cache");
    }

    return Compile(payload);
}

std::vector<std::byte> WorkerProtocol::Compile(std::span<std::byte const> payload)
{
    auto const fields = Wire::DecodeCompilePayload(payload);
    if (!fields.has_value())
        return Refuse(_metrics, MalformedPayload, {});

    auto const token = Wire::AsStringView(fields->leaseToken);
    auto const fingerprint = Wire::AsStringView(fields->fingerprint);

    // Checked BEFORE the payload is decompressed, let alone compiled.
    //
    // Stated precisely, because the obvious stronger claim is false: by the time this
    // runs, `WorkerServer` has already read the whole frame off the socket and
    // charged it against the in-flight byte budget, and the token is a field INSIDE
    // that payload -- so no earlier gate exists short of a protocol change, and an
    // unauthorized peer still costs a read and a slot. What the check saves is
    // decompression and the compiler spawn, which is the part that matters by three
    // or four orders of magnitude.
    //
    // The reason is carried out rather than collapsed to a bool, and its wire code
    // and its counter come from ONE row of `LeaseRefusalTable` -- one fact, two
    // audiences, which is what that table was built for in #281.
    //
    // NOT `UnknownLease`. That is the SCHEDULER's code, meaning "a lease I issued
    // and have since forgotten", and a worker answering with it sent an operator to
    // the scheduler to look for a fault that is local.
    auto const decision = _validator(token, fingerprint);

    // Stamped the instant the grant was read, because `decision.remaining` is measured
    // from exactly that moment. Everything between here and the compiler spawn --
    // decompressing a preprocessed translation unit is the large one -- is time the
    // grant is being spent on, so charging only the compile would let a job outlive its
    // lease and still be served, which is the one outcome this bound exists to stop.
    auto const acceptedAt = std::chrono::steady_clock::now();

    if (auto const& refusal = decision.refusal; refusal.has_value())
    {
        // The detail travels. It is empty for anything that failed the MAC -- a
        // caller that could not authenticate a token has established no fact about
        // it, so there is nothing truthful to say -- and populated for the two
        // refusals an operator actually has to act on.
        //
        // Through `Refuse` like every other refusal here, carrying another table's
        // row rather than its own: `LeaseRefusalTable` already pairs the code with
        // the counter, and this converts that pair rather than restating it.
        auto const& row = Distributed::DescribeLeaseRefusal(refusal->reason);
        return Refuse(_metrics, SurfaceRefusal { .code = row.code, .counter = row.workerCounter }, refusal->detail);
    }

    // Opened AFTER the lease check and BEFORE any expensive work, and refused on the
    // DECLARED decompressed length rather than on what it expands to -- see
    // `Unenvelope`, which carries the reasoning.
    //
    // A REPLY, never a close: the frame declared its own length, so the connection is
    // still synchronised and a peer that guessed wrong learns which.
    //
    // `UnenvelopeText`, not `Unenvelope`: the runner wants a `std::string`, and an
    // `Identity` source would otherwise copy a whole preprocessed translation unit
    // into a `std::vector<std::byte>` on the way. That used to be every source --
    // a node negotiated no codec but `Identity` (#265) -- and is now the
    // compression-less build's path, which can least afford the spare copy.
    auto source = UnenvelopeText(fields->source, _maxDecompressedBytes);
    if (!source.has_value())
    {
        // Code, text and counter come from ONE row rather than a ternary beside a
        // lookup: a malformed frame answered `UnsupportedCodec` while its message
        // said "malformed" would send an operator hunting a codec mismatch that
        // never happened.
        //
        // Counted here and not inside `Unenvelope`, because the launcher calls that
        // too and has no sink -- and a refusal answered on the wire while nothing
        // rises is how a port being probed with envelope bombs looked, on
        // `/metrics`, exactly like a port nobody was talking to.
        auto const reason = source.error();
        return Refuse(_metrics,
                      SurfaceRefusal { .code = WireCodeFor(reason), .counter = CounterFor(reason) },
                      DescribeEnvelopeError(reason));
    }

    // Counted around the runner rather than inside it: the runner is a seam with
    // its own fakes, and a fake that forgot to count would make every test agree
    // with a worker that does not.
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsStarted);
    auto const startedAt = std::chrono::steady_clock::now();

    auto const outcome =
        _jobs.Run(CompileJob { .fingerprint = std::string { fingerprint },
                               .args = DecodeArgs(fields->args),
                               // NOT `auto const source` above, for the reason
                               // `CodecEnvelope` records: `*std::move(x)` on a
                               // `const expected` is a `T const&&`, which binds
                               // to the COPY constructor with no diagnostic.
                               .preprocessed = *std::move(source),
                               // Sanitized where it becomes a path, not here: the
                               // runner is what creates the file, so the check
                               // belongs beside the creation rather than at each
                               // caller that might forget it.
                               .sourceName = std::string { Wire::AsStringView(fields->sourceName) },
                               // Validated where it becomes a command-line argument, for the
                               // same reason `sourceName` is sanitized where it becomes a path:
                               // the runner is what spells it, so the check belongs beside the
                               // spelling rather than at each caller that might forget it.
                               .compileDir = std::string { Wire::AsStringView(fields->compileDir) },
                               .compileDirReplacement = std::string { Wire::AsStringView(fields->compileDirReplacement) },
                               // The gcc half of the source-name repair (#883): validated
                               // where it becomes an argument, exactly as the pair above is.
                               .sourceRoot = std::string { Wire::AsStringView(fields->sourceRoot) },
                               .sourceRootReplacement = std::string { Wire::AsStringView(fields->sourceRootReplacement) } });
    if (!outcome.has_value())
    {
        // The refusal's detail rides the reply message, so a client's local fallback
        // can name the offending flag rather than only report that one existed. Empty
        // for every refusal that has nothing to add, which reproduces the previous
        // empty-message wire exactly.
        auto const& descriptor = DescriptorFor(outcome.error().reason);
        return Refuse(
            _metrics, SurfaceRefusal { .code = descriptor.code, .counter = descriptor.counter }, outcome.error().detail);
    }

    // A compiler that ran and rejected the code did its job — that is the client's
    // answer, not a worker failure — so this counts completions rather than
    // successes, and a non-zero exit is not a refusal.
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt);
    _metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted);
    _metrics.Increment(IMetricsSink::Counter::WorkerCompileMillisTotal, static_cast<std::uint64_t>(elapsed.count()));

    // **The job outlived its grant, so the object goes nowhere and the refusal says
    // WHICH bound ran out.** The compiler is not killed -- it has already finished, and
    // killing one mid-flight would not even unblock this thread, since the
    // grandchildren hold the pipe write ends (#239). What is declined is SERVING the
    // result: past the grant's expiry the scheduler has reclaimed the lease and may
    // have re-granted the key, so this object is work the fleet has already given away.
    //
    // **Which bound is the whole point of saying it.** This surface now has two, and
    // they are opposite diagnoses fixed by different people -- the same split
    // `TransportFailure` draws between `Expired` and `Silent` on the client, and it is
    // stated in the same words because an operator reading both ends must not have to
    // translate. Running out of GRANT means the cluster's `lease-lifetime` is shorter
    // than this site's slowest translation unit, and one number fixes it. Running out
    // of the endpoint's window instead means the compile exceeded the largest lease a
    // cluster may agree on at all, which no setting fixes. Reported as one number they
    // would send whoever read it to change the thing that was not the problem, which is
    // exactly what #245 records about reporting silence as an expiry.
    //
    // Compared against `remaining` as a STEADY-clock duration on both sides, so a wall
    // clock stepping during a long compile cannot decide this. Disengaged means this
    // worker authenticated nothing and has no bound to enforce, which is the keyless
    // single-machine install and not a job that has run out of time.
    //
    // Measured from `acceptedAt` rather than from `startedAt`: `remaining` is the budget
    // as it stood when the grant was read, so the unenveloping between the two is spent
    // out of it too. `elapsed` stays the COMPILE's own cost, because that is what the
    // counter beside it means.
    auto const held = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - acceptedAt);
    if (decision.remaining.has_value() && held > *decision.remaining)
    {
        auto const& row = Distributed::DescribeLeaseRefusal(Distributed::LeaseRefusalReason::Expired);
        return Refuse(_metrics,
                      SurfaceRefusal { .code = row.code, .counter = row.workerCounter },
                      std::format("this job held its grant for {} ms ({} ms of it compiling) and the grant allowed "
                                  "{} ms; the lease expired while it ran, so the object is not served. Raise the "
                                  "cluster's lease-lifetime setting if this site's translation units are longer "
                                  "than the fleet was told",
                                  held.count(),
                                  elapsed.count(),
                                  decision.remaining->count()));
    }

    // The object goes back in an envelope chosen from what the CLIENT said it
    // accepts -- carried in its own request, so no negotiation round trip. `Envelope`,
    // the same function the client wrapped its source with: the two directions are one
    // negotiation, and a second implementation of the choice is how they come to
    // disagree.
    // (This call site was that second implementation; #265 and `Envelope`'s own doc
    // carry the history.)
    auto const enveloped = Envelope(outcome->object, fields->acceptedCodecs, _acceptedCodecs);

    return Wire::EncodeReply(
        Wire::Status::Ok,
        Wire::EncodeCompileResult(Wire::CompileResult { .exitCode = static_cast<std::uint32_t>(outcome->exitCode),
                                                        .object = enveloped,
                                                        .stdoutText = Wire::AsBytes(outcome->stdoutText),
                                                        .stderrText = Wire::AsBytes(outcome->stderrText),
                                                        // Carried through from the runner, never recomputed from
                                                        // `fields` here -- see `ICompileJobRunner` (#280).
                                                        .correlation = Wire::AsBytes(outcome->correlation) }));
}

WorkerRegistrar::WorkerRegistrar(CredentialNotice& notice,
                                 std::string fingerprint,
                                 std::string endpoint,
                                 std::uint32_t slots,
                                 Wire::CodecList acceptedCodecs,
                                 Wire::CapacityFields capacity):
    _notice { notice },
    _fingerprint { std::move(fingerprint) },
    _endpoint { std::move(endpoint) },
    _slots { slots },
    _acceptedCodecs { std::move(acceptedCodecs) },
    // Moved rather than copied, like every other member here. It became worth
    // saying when `CapacityFields` grew the node's cache record: it was a handful
    // of scalars and is now a struct holding a vector, so a copy allocates.
    _capacity { std::move(capacity) }
{
}

std::expected<void, AnnounceRefusal> WorkerRegistrar::Register(ISocket& scheduler, Credential const& credential)
{
    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest { .fingerprint = _fingerprint,
                                                                    .endpoint = _endpoint,
                                                                    .slots = _slots,
                                                                    .acceptedCodecs = _acceptedCodecs,
                                                                    .capacity = _capacity });
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, &_notice, frame, credential));
    if (!outcome.IsHit())
        // The scheduler's own words, code and message both, which is the whole
        // reason this is not a bool: "not a member of this cluster" and "fingerprint
        // is not valid UTF-8" call for opposite actions from an operator, and this
        // node cannot tell them apart from its own side.
        //
        // `RedirectTarget` rather than a second reading of the same refusal: a
        // `NotLeader` whose message is prose, or names a bare port, is not a
        // redirect, and that judgement belongs in one place for the launcher's
        // lease chain and this alike.
        return std::unexpected { AnnounceRefusal { .reason = DescribeOutcome(outcome), .leader = RedirectTarget(outcome) } };

    // The reply is a record since wire version 4, not a bare id. A payload this
    // build cannot read is a refusal rather than a worker id of whatever the bytes
    // happened to spell -- which is what the previous shape would have done with it.
    auto reply = Wire::DecodeRegisterReply(outcome.value);
    if (!reply.has_value())
        return std::unexpected { AnnounceRefusal { .reason = "accepted, and answered with a malformed registration record",
                                                   .leader = std::nullopt } };

    _workerId = std::move(reply->workerId);
    _clusterId = std::move(reply->clusterId);
    _epoch = reply->epoch;
    // An EMPTY fleet identity is not refused, and that is deliberate. It is what a
    // scheduler with no `--cluster-id` sends, which is the one-machine deployment --
    // `SchedulerService`'s own contract says empty is legal and that a verifier
    // naming none expects none. Refusing it here would close #401's window by
    // breaking every single-machine install, which is the shape #303 is about.
    // "Registered" is what pins a worker; the identity is what it pins TO.
    if (_workerId.empty())
        // Accepted and unusable: every later heartbeat needs the id, so a worker
        // that kept going here would heartbeat nothing into a fleet that thinks it
        // is registered. Reported as a refusal because that is what it costs.
        // No `leader`: this scheduler accepted, so it is not telling this node to
        // go elsewhere. Following a redirect here would send a working registration
        // to a second scheduler over a fault that is this one's to fix.
        return std::unexpected { AnnounceRefusal { .reason = "accepted, and assigned no worker id",
                                                   .leader = std::nullopt } };

    return {};
}

std::expected<void, AnnounceRefusal> WorkerRegistrar::Heartbeat(ISocket& scheduler,
                                                                std::uint32_t inFlight,
                                                                Wire::LoadFields const& load,
                                                                Credential const& credential)
{
    if (_workerId.empty())
        // Never registered; nothing to refresh. Named rather than silent, because
        // the caller's next move -- register -- is the same either way, and a
        // diagnostic that cannot say which of the two happened is one an operator
        // cannot act on.
        return std::unexpected { AnnounceRefusal { .reason = "not registered", .leader = std::nullopt } };

    auto const frame = Wire::EncodeHeartbeat(_workerId, inFlight, load);
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, &_notice, frame, credential));
    if (outcome.IsHit())
        return {};

    // A scheduler that does not know this worker is telling it to register again --
    // it restarted, or expired this entry. Forgetting the id here is what makes the
    // caller's retry actually re-register instead of heartbeating into a void
    // forever while the fleet runs without it.
    if (outcome.kind == CacheOutcomeKind::Rejected && outcome.code == Wire::ErrorCode::UnknownLease)
        _workerId.clear();

    // The id is NOT cleared for a redirect. A `NotLeader` says this scheduler is
    // the wrong one to ask, not that the fleet has forgotten this worker -- and the
    // leader it names may well be holding the very registration this id belongs to,
    // since the registry is replicated. Clearing it here would turn every election
    // into a re-registration storm across the whole fleet.
    return std::unexpected { AnnounceRefusal { .reason = DescribeOutcome(outcome), .leader = RedirectTarget(outcome) } };
}

std::expected<void, AnnounceRefusal> WorkerRegistrar::Withdraw(ISocket& scheduler, Credential const& credential)
{
    if (_workerId.empty())
        // Never registered, so there is nothing on the other end to retire. Named
        // rather than silent for `Heartbeat`'s reason: a caller that cannot tell this
        // from a refused withdrawal cannot report either.
        return std::unexpected { AnnounceRefusal { .reason = "not registered", .leader = std::nullopt } };

    auto const frame = Wire::EncodeWithdraw(_workerId);
    auto const outcome = SyncRun(ExchangeFramed(&scheduler, &_notice, frame, credential));
    if (outcome.IsHit())
        return {};

    // No arm clears `_workerId` and no arm is fatal -- deliberately, and this is where
    // the two differ from `Heartbeat`. There, `UnknownLease` means *register again*
    // and clearing the id is what makes the retry work. Here every refusal, including
    // an `UnknownOpcode` from a scheduler too old to know the verb, leaves the same
    // fallback standing: the entry stops being heartbeated and expires on its own.
    // A withdrawal is an optimisation over that, never a replacement for it.
    return std::unexpected { AnnounceRefusal { .reason = DescribeOutcome(outcome), .leader = RedirectTarget(outcome) } };
}

} // namespace FastCache::Cc
