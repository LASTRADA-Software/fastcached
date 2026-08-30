// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
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

    /// @return True when the daemon served a value.
    [[nodiscard]] bool IsHit() const noexcept
    {
        return kind == CacheOutcomeKind::Hit;
    }
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

/// The two deadlines one exchange runs under.
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
/// @return The outcome.
[[nodiscard]] Task<CacheOutcome> ExchangeFramed(ISocket* client, std::vector<std::byte> frame, Credential credential = {});

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
[[nodiscard]] Task<CacheOutcome> CacheFetch(ISocket* client, std::string_view key, Credential credential = {});

/// STORE one entry over an already-connected client.
///
/// Pipelines AUTH the same way `CacheFetch` does, for the same reason.
/// @param client Connected transport; not owned.
/// @param request The fields to send.
/// @param credential Credential to present; default-constructed sends none.
/// @return The outcome; `kind == Hit` means the daemon acknowledged the write.
[[nodiscard]] Task<CacheOutcome> CacheStore(ISocket* client,
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
