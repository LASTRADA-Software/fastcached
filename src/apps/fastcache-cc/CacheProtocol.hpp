// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/KeepAlive.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// How a cache round trip ended.
///
/// `Miss` and `Rejected` are deliberately separate. They were the same byte on
/// the pre-version wire, which meant a launcher talking to a daemon that could
/// not serve it saw an endlessly cold cache and reported nothing — the build
/// merely got slower, forever, with no clue why. Telling them apart is the whole
/// reason the wire carries a status space and an error code.
enum class CacheOutcomeKind : std::uint8_t
{
    Hit,       ///< The value was served. `value` holds it.
    Miss,      ///< The daemon has no such entry. Not an error.
    Rejected,  ///< The daemon refused the command. `code` and `message` say why.
    Transport, ///< The exchange never completed (socket error, short read).
};

/// Which way a `Transport` outcome went wrong.
///
/// **Three states, not one, and the collapse was the bug**
/// ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)). Every one
/// of these is answered the same way -- compile locally, because the client is
/// holding the source -- and that sameness is exactly why they were folded into one
/// sentence. But the same ACTION is not the same DIAGNOSIS: "that machine is off"
/// and "that compile took longer than the budget" are fixed in different places, by
/// different people, and an operator handed one string for both has nothing to act
/// on.
///
/// It matters more since #223 made the compile budget minutes long, and more again
/// now that keepalive answers a dead host in seconds: without a name for what
/// happened, the improvement is invisible -- the same non-answer, sooner.
enum class TransportFailure : std::uint8_t
{
    /// No transport failure; the exchange completed. The default, because a
    /// `Hit`/`Miss`/`Rejected` outcome must not carry a cause it does not have.
    None,

    /// No connection was ever made: the host refused, was unroutable, or the name
    /// did not resolve inside the dial budget. The peer may never have existed.
    Unreached,

    /// Connected, then the connection broke on its own.
    ///
    /// **This is what a vanished host looks like**, and the state keepalive exists
    /// to reach quickly: without probes the same host produces `Expired` minutes
    /// later, because nothing else notices. See `Net/KeepAlive.hpp`.
    PeerLost,

    /// The exchange was still running when the total budget ran out, and THIS side
    /// closed the socket. The peer may be entirely healthy and merely slow -- which
    /// is the reading a `PeerLost` must never be given.
    Expired,

    /// The peer went QUIET: connected, its host still answering at the TCP level, and
    /// nothing said for the idle budget.
    ///
    /// **Its own name rather than an `Expired`, because it is the one diagnosis the
    /// other three cannot express** and the reason #245 was built. `Unreached` is a
    /// machine that is not there, `PeerLost` is a connection that broke, `Expired` is
    /// a compile that took longer than we were prepared to wait — and this is a worker
    /// process that stopped making progress while its kernel went on acknowledging
    /// everything, which is the exact state keepalive is blind to. Reported as
    /// `Expired` it would send an operator to raise a timeout that was never the
    /// problem.
    ///
    /// It can only ever be reported on an exchange whose peer had agreed to pulse, so
    /// its appearance is itself information: this worker said it would keep talking and
    /// then did not.
    Silent,
};

/// A phrase naming @p failure, for the sentence a fall-back is recorded under.
///
/// A table rather than a `switch` at the one call site that needs it today: the
/// next consumer -- `--show-stats`, a log line -- must say the same words, and two
/// places spelling one taxonomy is how they drift.
/// @param failure What went wrong.
/// @return A phrase, always non-empty, so a caller never has to handle a gap.
[[nodiscard]] constexpr std::string_view DescribeTransportFailure(TransportFailure failure) noexcept
{
    switch (failure)
    {
        case TransportFailure::None:
            return "completed";
        case TransportFailure::Unreached:
            return "could not be reached";
        case TransportFailure::PeerLost:
            return "went away mid-exchange";
        case TransportFailure::Expired:
            return "ran out of budget";
        case TransportFailure::Silent:
            return "stopped reporting progress";
    }
    return "could not be reached";
}

/// The result of one FETCH or STORE.
struct CacheOutcome
{
    CacheOutcomeKind kind { CacheOutcomeKind::Transport };
    std::vector<std::byte> value;                                                     ///< Payload on a hit.
    CompileCacheWire::ErrorCode code { CompileCacheWire::ErrorCode::MalformedFrame }; ///< Valid when `kind == Rejected`.
    std::string message; ///< The daemon's own words, when it refused.

    /// True when a credential was presented and the daemon did not understand the
    /// AUTH verb at all — i.e. it predates authentication on this protocol.
    ///
    /// The exchange still succeeds (such a daemon steps over the unknown verb and
    /// serves the command behind it), so this is not an error and must not be
    /// treated as one. It is reported because the operator asked for something
    /// that did not happen: they set a token, and this traffic went unauthenticated.
    /// Discovering that from a security review rather than from the tool is the
    /// silent no-op this codebase keeps a list about.
    bool credentialIgnored { false };

    /// Which way a `Transport` outcome went wrong; `None` for every other kind.
    ///
    /// Defaulted to `Unreached` alongside the `Transport` default above, because the
    /// two defaults describe one seeded answer: an exchange that never ran must read
    /// as "nothing was reached", never as a peer that was contacted and lost.
    TransportFailure transportFailure { TransportFailure::Unreached };

    /// @return True when the daemon served a value.
    [[nodiscard]] bool IsHit() const noexcept
    {
        return kind == CacheOutcomeKind::Hit;
    }
};

/// Says once, per process, that a configured credential went unchecked.
///
/// Passed to the exchanges as a POINTER rather than a reference, and that is not a
/// style choice: they are coroutines, and a reference parameter to a coroutine
/// dangles the moment the frame outlives the caller's full expression
/// (`cppcoreguidelines-avoid-reference-coroutine-parameters`). `RunExchange` in this
/// same tree already takes its reactor and connector as pointers for exactly that
/// reason. Non-coroutine holders -- `WorkerRegistrar`, `ReactorExchange` -- keep a
/// reference, because they are not frames that can outlive their caller.
///
/// **The once-guard is a member and the output is a sink**, which is the whole point
/// ([#363](https://github.com/LASTRADA-Software/fastcached/issues/363)). It was a
/// function-local `static bool` inside the launcher's `main.cpp`, so exactly one of
/// the seven consumers of an outcome could report: the cache path. Every dispatch
/// path -- COMPILE, LEASE, RELEASE, REGISTER, HEARTBEAT and the cluster admin verbs,
/// three of them in other translation units and one in another executable entirely --
/// received `credentialIgnored` and had nowhere to say it.
///
/// That mattered more after #340 than before. Making a scheduler answer AUTH with a
/// code the launcher steps over is correct -- a permanent 0% hit rate presenting as a
/// cold cache was the bug -- but it converts a loud failure into a silent success on
/// the dispatch paths, and the diagnostic meant to compensate was wired to the one
/// path that did not need it.
///
/// Injected rather than reached for, per the DI rule: a second hidden global with a
/// nicer name would be the same defect with better spelling.
class CredentialNotice
{
  public:
    /// Where the line goes. Empty means say nothing.
    using Sink = std::function<void(std::string_view)>;

    /// @param sink Where to report; may be empty.
    explicit CredentialNotice(Sink sink) noexcept:
        _sink { std::move(sink) }
    {
    }

    /// A notice that reports nowhere.
    ///
    /// Named rather than a default argument, and that is deliberate: a defaulted
    /// parameter is how six call sites came to drop this in the first place. A caller
    /// that genuinely has nowhere to report has to say so.
    /// @return A notice with no sink.
    [[nodiscard]] static CredentialNotice Silent()
    {
        return CredentialNotice { Sink {} };
    }

    /// Report, if this outcome says a credential went unchecked and nothing has said
    /// so yet.
    ///
    /// Guarded so a build of thousands of translation units says it once rather than
    /// thousands of times, which is the difference between a diagnostic and noise.
    /// @param outcome The completed exchange.
    /// @return True when this call was the one that reported.
    bool Observe(CacheOutcome const& outcome)
    {
        if (!outcome.credentialIgnored || _said)
            return false;
        _said = true;
        if (_sink)
            _sink("the peer does not support authentication; the configured credential was ignored");
        return true;
    }

    /// @return Whether anything has been reported yet.
    [[nodiscard]] bool Reported() const noexcept
    {
        return _said;
    }

  private:
    Sink _sink;
    bool _said { false };
};

/// Describe an outcome for a diagnostic line — the daemon's own code and words
/// when it refused, so a launcher can say *why* a build is not caching rather
/// than only that it is not.
/// @param outcome The outcome to describe.
/// @return A short human-readable phrase; empty for a hit.
[[nodiscard]] std::string DescribeOutcome(CacheOutcome const& outcome);

/// Whether the daemon is serving this launcher, judged by one completed exchange.
///
/// `Hit` and `Miss` are both a working daemon answering about a key. `Rejected`
/// and `Transport` are not answers about the key at all — one is a daemon that
/// declined the command (a version mismatch, a credential it will not take), the
/// other a daemon that was never reached — and neither is going to answer the
/// *next* command in this invocation either.
///
/// The launcher acts on that in one place: a `STORE` after a fetch that was
/// refused or that never arrived spends the whole encoded value — an object file,
/// so megabytes, on the hot path of a parallel build — to be told the same thing
/// again. It is the argument `IsStorableSize` makes below, reached from the other
/// side: do not pay a transfer to be refused.
///
/// **It deliberately says nothing about whether the invocation continues.** A
/// cache and a compile fleet are two services, usually on two machines, and an
/// answer about one is not an answer about the other — `RunCached` used to return
/// on a fetch that failed at the transport, so an unreachable `FASTCACHE_ADDR`
/// took a perfectly healthy fleet down with it and every build went local while
/// staying green (issue #236).
/// @param kind How the exchange ended.
/// @return True when a further command to the same daemon is worth sending.
[[nodiscard]] constexpr bool CacheIsServing(CacheOutcomeKind kind) noexcept
{
    return kind == CacheOutcomeKind::Hit || kind == CacheOutcomeKind::Miss;
}

/// The credential this launcher presents, if any.
///
/// An empty `secret` means "no credential configured", and every exchange below
/// then sends exactly the bytes it always did. That is what keeps a launcher that
/// has never heard of authentication byte-compatible on the wire with one that
/// has: the AUTH frame exists only when there is something to put in it.
struct Credential
{
    std::string username; ///< Empty selects the default user (the `requirepass` form).
    std::string secret;   ///< Empty means no credential is configured.

    /// @return True when a credential should be presented.
    [[nodiscard]] bool Configured() const noexcept
    {
        return !secret.empty();
    }
};

/// Told, on the exchange's own thread, each time the exchange demonstrably moved
/// forward.
///
/// **The seam a bound on SILENCE needs, and the reason it is a seam at all.** A total
/// deadline is armed once and can be enforced from outside the protocol; an idle
/// deadline has to be pushed out by something only the protocol can see — a request
/// fully written, a `Status::Progress` frame read — and the thing that owns the timer
/// is `ReactorExchange`, which knows nothing about statuses. So the protocol reports
/// the events and the exchange decides what they are worth, which is the same split
/// `IFrameResponder` makes on the serving side: the endpoint owns *when*, the surface
/// owns *what*.
///
/// **Not a clock, and deliberately no arithmetic here.** An implementation is free to
/// re-arm a timer, count, or do nothing; this reports facts. That is what keeps
/// `CacheProtocol` free of both a reactor and a clock, and what lets a test assert the
/// pulses were seen without a timer anywhere in the case.
///
/// Reached through a POINTER for the reason `CredentialNotice` is: the exchange is a
/// coroutine, whose frame outlives the expression that created it, so a reference
/// parameter would bind to storage the caller may already have destroyed
/// (`cppcoreguidelines-avoid-reference-coroutine-parameters`). Null means nobody is
/// listening, which is every exchange but the compile.
class IExchangeLiveness
{
  public:
    IExchangeLiveness() = default;
    virtual ~IExchangeLiveness() = default;
    IExchangeLiveness(IExchangeLiveness const&) = delete;
    IExchangeLiveness& operator=(IExchangeLiveness const&) = delete;
    IExchangeLiveness(IExchangeLiveness&&) = delete;
    IExchangeLiveness& operator=(IExchangeLiveness&&) = delete;

    /// The exchange moved forward: a request went out in full, or the peer said it is
    /// still working.
    ///
    /// **It does not say WHICH**, and that is the contract rather than a shortcut. An
    /// idle bound asks one question — has anything happened — and an implementation
    /// handed the distinction would eventually treat two kinds of forward motion
    /// differently, which is a second policy nobody asked for in the place least able
    /// to explain itself.
    virtual void MovedForward() noexcept = 0;
};

/// What one exchange runs under: its two deadlines, and how a dead peer is noticed.
///
/// Two rather than one, because they bound different things and neither implies the
/// other -- the collapse `DialEndpoint` used to make. They are also a named struct
/// rather than two adjacent `milliseconds` parameters, for the reason
/// `PeerTransportOptions` gives: a reader at the call site cannot transpose
/// `.connect` with `.total`, which two bare durations invite.
///
/// It lives beside `CacheOutcome` rather than beside the reactor that enforces it,
/// because a budget is a property of the exchange and not of the machinery: the
/// dispatch verbs run under one too, and `Dispatch.hpp` must not have to include a
/// reactor to say so.
struct ExchangeBudget
{
    /// Ceiling on opening the connection, name resolution included.
    std::chrono::milliseconds connect { 1'000 };

    /// Ceiling on the whole exchange after the dial; non-positive means unbounded.
    ///
    /// The launcher's first real end-to-end bound. `SO_RCVTIMEO` bounded a single
    /// call, so a daemon dribbling a byte at a time could hold a compile forever
    /// while never once exceeding it.
    std::chrono::milliseconds total { 10'000 };

    /// @return True when `total` bounds this exchange.
    ///
    /// The rule lives on the type rather than at each consumer, because the
    /// arithmetic alone says the OPPOSITE of what the value means: a zero total put
    /// the deadline at `Now()`, so every exchange died on the reactor's next turn --
    /// a knob documented as "turn the ceiling off" that turned the cache off
    /// instead, silently, since every caller answers a transport failure by
    /// compiling. A consumer that re-derives the comparison is a consumer that can
    /// omit it.
    [[nodiscard]] bool BoundsTotal() const noexcept
    {
        return total > std::chrono::milliseconds::zero();
    }

    /// Ceiling on SILENCE, once the connection is open; non-positive means unbounded.
    ///
    /// **The third deadline, and the one that finally splits the question `total`
    /// could never answer alone** ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)).
    /// `total` asks *how slow may this exchange legitimately be*, and on a dispatched
    /// compile the honest answer is "as slow as the slowest translation unit anybody
    /// compiles" — minutes, which is then also how long a worker making no progress
    /// goes unnoticed. This asks *how long since anything happened*, which has a much
    /// tighter honest answer, and it is only askable because the worker now says
    /// something: `CompileCacheWire::Status::Progress`.
    ///
    /// It is reset by forward motion (`IExchangeLiveness::MovedForward`) and by nothing
    /// else, so a peer cannot buy time by being slow — only by being alive.
    ///
    /// Left off for every exchange that is bounded by a round trip anyway: a cache
    /// FETCH answered from memory has nothing to pulse, and arming an idle bound there
    /// would be a second name for the total.
    std::chrono::milliseconds idle { 0 };

    /// @return True when `idle` bounds this exchange's silence.
    ///
    /// The rule lives on the type for the reason `BoundsTotal` gives, and the value is
    /// spelled the same way `ArmSocketDeadline` spells it: non-positive is *no bound*,
    /// never *a bound of zero*, which would expire on the reactor's next turn and turn
    /// the cache off while reading as a knob that turns a ceiling off.
    [[nodiscard]] bool BoundsIdle() const noexcept
    {
        return idle > std::chrono::milliseconds::zero();
    }

    /// Whether the connection probes a peer that has stopped answering.
    ///
    /// **The third thing, and it is here because `total` alone cannot answer both
    /// questions** ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)).
    /// *How slow may this exchange legitimately be* and *how fast is a dead peer
    /// noticed* are separate, and #223 already established that collapsing them
    /// costs the wrong one: shortening `total` to notice a dead worker abandons
    /// every translation unit worth distributing while the worker finishes the job
    /// anyway.
    ///
    /// So a cache round trip -- bounded by a round trip, `total` measured in seconds
    /// -- leaves this `No` and loses nothing. A dispatched compile, bounded by how
    /// long a COMPILER runs and therefore minutes long by design, sets it: without
    /// it, a client whose worker's host is powered off holds a build slot for the
    /// whole compile budget, and on a `-j16` build a handful of those is a stalled
    /// build.
    ///
    /// It detects a dead connection or host, never a peer that is alive and merely
    /// silent -- that is what `idle` below now measures. See `Net/KeepAlive.hpp`.
    KeepAlive keepAlive { KeepAlive::No };

    /// Field-by-field equality, so a `static_assert` can hold two independently
    /// written budgets against each other.
    ///
    /// Defaulted rather than spelled out, and that is the whole point: a field added
    /// to this struct joins the comparison by itself. A hand-written one would keep
    /// agreeing after the field that made two budgets differ stopped being covered,
    /// which is exactly the shape of #247's defect.
    [[nodiscard]] friend constexpr bool operator==(ExchangeBudget const&, ExchangeBudget const&) = default;
};

/// Send one framed request and read its reply, presenting `credential` if
/// configured.
///
/// The shared exchange every command goes through, exposed because distributed
/// execution has its own verbs and must not grow a second copy of this: the
/// credential pipelining, the drain-both-replies rule, and the "a daemon that does
/// not know AUTH still served the command" fall-through are each subtle enough that
/// two implementations would differ, and the one that differed would be the one
/// nobody tested against an old daemon.
/// @param client Connected transport; not owned.
/// @param frame A complete framed request.
/// @param credential Credential to present; default-constructed sends none.
/// @param liveness Told each time the exchange moves forward; null for none.
/// @return The outcome.
[[nodiscard]] Task<CacheOutcome> ExchangeFramed(ISocket* client,
                                                CredentialNotice* notice,
                                                std::vector<std::byte> frame,
                                                Credential credential = {},
                                                IExchangeLiveness* liveness = nullptr);

/// Where a refusal says to ask instead, when it says so.
///
/// `NotLeader` is not a refusal in the sense the others are: `NoWorker` and
/// `NoCapacity` are answers about the fleet, while this one is an instruction about
/// WHOM to ask, and a client that reads it as the former takes itself out of
/// distribution over a leader election it could simply have followed
/// ([#237](https://github.com/LASTRADA-Software/fastcached/issues/237)).
///
/// **Judged by PARSING the message, never by asking whether it is empty.** An empty
/// message is replaced with the error table's default sentence before it reaches the
/// wire, so "no leader is known" and "the leader is at h:p" arrive as the same shape
/// -- a non-empty string either way. Only "does this parse as an address" separates
/// them, and a client that dialled a sentence would report a scheduler endpoint no
/// operator ever typed. `ClusterAdminCli` learned this the hard way; that call site
/// now asks here, so the two cannot come to disagree.
///
/// Parsing means a host, a colon and a port that is a number, which is
/// `Core/HostPort.hpp`'s `ParseDialEndpoint` and not a test spelled again here --
/// `DialEndpoint` asks that helper the same question about the same string a moment
/// later, and a second author would eventually answer differently. `ClusterAdminCli`
/// only ever PRINTED such a message; a launcher dials it, and every hop spent on
/// prose is one the real leader never hears.
///
/// A redirect naming an address is therefore distinguishable from an election in
/// progress, which names none and has nothing to offer but "try again shortly".
///
/// @param outcome A completed exchange's outcome.
/// @return The endpoint to retry against, or `std::nullopt` when this outcome is not
///         a redirect -- a `NotLeader` raised while no leader is known included, and
///         any message that does not parse as `host:port`.
[[nodiscard]] std::optional<std::string> RedirectTarget(CacheOutcome const& outcome);

/// FETCH one key over an already-connected client.
///
/// When `credential` is configured, an AUTH frame is **pipelined** ahead of the
/// FETCH — both are written before either reply is read — so authenticating costs
/// bytes but no extra round trip. This matters because the launcher opens a fresh
/// connection per operation, so a wait-for-AUTH-then-send spelling would double
/// the round trips of every translation unit in a build. See the note in
/// `CompileCacheWire.hpp`.
///
/// A rejected credential surfaces as the *fetch's* outcome (`Rejected` /
/// `Unauthenticated`), because that is the answer the caller acts on; the AUTH
/// reply is consumed first so the two never desynchronise.
///
/// @param client Connected transport; not owned.
/// @param key The key to look up.
/// @param credential Credential to present; default-constructed sends none.
/// @return The outcome; `value` holds the stored bytes on a hit.
[[nodiscard]] Task<CacheOutcome> CacheFetch(ISocket* client,
                                            CredentialNotice* notice,
                                            std::string_view key,
                                            Credential credential = {});

/// STORE one entry over an already-connected client.
///
/// Pipelines AUTH the same way `CacheFetch` does, for the same reason.
/// @param client Connected transport; not owned.
/// @param request The fields to send.
/// @param credential Credential to present; default-constructed sends none.
/// @return The outcome; `kind == Hit` means the daemon acknowledged the write.
[[nodiscard]] Task<CacheOutcome> CacheStore(ISocket* client,
                                            CredentialNotice* notice,
                                            CompileCacheWire::StoreRequest request,
                                            Credential credential = {});

/// Default ceiling on a value the launcher will offer to the daemon.
///
/// 256 MiB, matching the daemon's own `--storage-max-value` default, so that out
/// of the box the launcher does not spend a build's time pushing something the
/// other side is certain to refuse. The two are separate settings on separate
/// processes that never negotiate -- the wire has no handshake by design -- so
/// this agreement is a chosen default, not a derived one, and either side can be
/// retuned without the other noticing.
// `ULL`, not `UL`: `unsigned long` is 32 bits on Win64 (LLP64), so a future
// edit raising this past 4 GiB would silently wrap there and nowhere else.
inline constexpr std::size_t DefaultMaxStoreBytes = 256ULL * 1024ULL * 1024ULL;

/// Whether a value of `valueBytes` is worth offering to the daemon at all.
///
/// A single object file can be enormous -- a C++23 translation unit built with
/// `-g` reached 356 MB in the report that prompted this -- and one entry that
/// size would dominate a cache sized for thousands of ordinary objects. Past the
/// limit the launcher stores nothing: the compile has already succeeded, so the
/// build is unaffected and only this one result stays uncached.
///
/// This is a *client* policy, deliberately checked before connecting rather than
/// left for the daemon to refuse. Sending it anyway costs the transfer on every
/// rebuild of that translation unit, and pays it to be told no.
/// @param valueBytes Size of the encoded value.
/// @param limitBytes The ceiling; **0 means no limit**, not "store nothing".
/// @return True when the value may be sent.
[[nodiscard]] constexpr bool IsStorableSize(std::size_t valueBytes, std::size_t limitBytes) noexcept
{
    return limitBytes == 0 || valueBytes <= limitBytes;
}

} // namespace FastCache::Cc
