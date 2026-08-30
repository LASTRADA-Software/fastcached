// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// The lease token: what the scheduler signs, and what a worker will check before it
/// spawns anything (#282 -- see "What this does NOT close" below).
///
/// ## What this closes
///
/// A lease used to be a small decimal serial the scheduler minted for its own
/// bookkeeping, and the worker's validator was `[](...){ return true; }`. So a
/// worker compiled for anyone who could reach its port and present any string:
/// the lease was capacity accounting the scheduler kept, never a credential the
/// worker could check. Membership decided *who may connect*; nothing decided *who
/// the scheduler actually sent*.
///
/// ## What this does NOT close
///
/// Only the scheduler half. It **mints** a signed grant and authenticates one handed
/// back to `Release`; no worker checks a token before it spawns a compiler yet, so a
/// compile port's boundary today is still reachability plus membership. That half is
/// [#282](https://github.com/LASTRADA-Software/fastcached/issues/282), and until it
/// lands `VerifyLeaseToken` and `LeaseRefusalTable` below have no production caller.
/// A signed grant nobody verifies buys nothing on its own; what it buys is that the
/// format, the wire codes and the counters exist for #282 to be written against,
/// which is also why signing must ship first -- a worker that refused unsigned leases
/// before every scheduler in a fleet could mint them would stop distributing
/// mid-upgrade.
///
/// ## Why it is header-only
///
/// `apps/fastcache-cc/WorkerProtocol.cpp` is where a lease will be verified, and that
/// file is compiled into `fastcache-cc`, which deliberately does **not link**
/// `FastCache`. Being header-only over `Core/Sha256`, `Core/Base64` and the
/// header-only `Core/WireFields` is what lets the launcher's build reach this
/// with two dependency-free rows in `_fc_cc_core` rather than a link against the
/// library -- the same rule `Protocol/CompileCacheWire.hpp` states about itself,
/// and for the same reason.
///
/// It performs no I/O and holds no state: every function here is a pure transform,
/// and `now` is a parameter rather than an injected clock precisely so that both
/// callers keep their own time seam. That is the documented exception to this
/// project's inject-everything rule -- a genuinely pure leaf computation -- and
/// not an oversight.
namespace FastCache::Distributed
{

/// The claims a grant authenticates, decoded and **owned**.
///
/// Owning rather than a `*View` of the token's bytes, and that is a rule this tree
/// has already paid for: a struct returned by value must not borrow from what it
/// decoded, because `Verify(Mint(x))` is the obvious spelling and would be a
/// use-after-free the moment one member became a view. The token is base64 text
/// that has to be decoded into a temporary buffer anyway, so there is nothing
/// stable to borrow from in the first place.
struct LeaseClaims
{
    /// The scheduler's own `LeaseTable` handle, carried inside the token.
    ///
    /// Which is what keeps `LeaseTable` pure: it mints and resolves a serial and
    /// knows nothing of keys, wall clocks or MACs. `SchedulerService::Release`
    /// authenticates the token and hands this back down.
    std::string serial;

    /// The ONE worker this grant is good for, as the scheduler advertised it.
    ///
    /// Inside the MAC, not merely beside it. Without it a lease minted for worker A
    /// is a lease on every worker in the fleet -- the same failure `DiscoveryWire`
    /// closes by putting the `(node, endpoint)` pair inside the proof tag.
    std::string endpoint;

    /// The toolchain the grant was issued against.
    ///
    /// Which makes `CompileCacheWire::ErrorCode::FingerprintMismatch` reachable for
    /// the first time. It is documented as "this worker's toolchain is not the one
    /// the lease named" and *nothing could perform that check*, because until now
    /// the lease named no toolchain: the only fingerprint a worker saw was the one
    /// the client stated about itself in the same frame.
    std::string fingerprint;

    /// The object key this grant covers.
    std::string key;

    /// When the grant stops being good, as an absolute WALL-CLOCK instant.
    ///
    /// Wall-clock and not `TimePoint`, which is this tree's steady clock: a steady
    /// instant means nothing on another machine, and a lease is checked on a
    /// machine other than the one that minted it by definition.
    std::chrono::system_clock::time_point expiresAt {};
};

/// Why a lease was refused. One enumerator per outcome that is not "compile it".
///
/// Ordered as the checks run, which is also the order of increasing information:
/// the first two are reachable by anybody, and everything below them is only ever
/// reported for a token whose MAC has already verified.
enum class LeaseRefusalReason : std::uint8_t
{
    /// Not a lease token at all -- not base64, not this format, not this version.
    Malformed = 0,
    /// A lease token this cluster's key does not authenticate.
    Unauthorized,
    /// An authentic lease, for a different worker.
    EndpointMismatch,
    /// An authentic lease, for a different toolchain.
    FingerprintMismatch,
    /// An authentic lease, past its expiry and the skew slack.
    Expired,
    Last, ///< Not a reason, and has no row: the length of a table keyed by one.
};

/// What a refusal is on the wire and in the metrics.
///
/// Both in one row. They are the same fact asked by two audiences -- the client
/// deciding whether to retry, and the operator watching a fleet -- and a refusal
/// counted under one reason while being reported as another is worse than not
/// counting it. It is the shape `Cc::RefusalDescriptor` already uses at the
/// worker, deliberately, so the verification half of this is a table lookup rather
/// than a second `switch`.
///
/// No member carries a default: a row answering two of the three questions is not
/// a row, and `CompileCacheWire::ErrorCode` has no zero enumerator for `{}` to name.
struct LeaseRefusalDescriptor
{
    LeaseRefusalReason reason;           ///< The outcome this row describes.
    CompileCacheWire::ErrorCode code;    ///< What the caller is told.
    IMetricsSink::Counter workerCounter; ///< What an operator sees rise at a worker.
};

/// One row per `LeaseRefusalReason`, in enumerator order.
///
/// `Malformed` and `Unauthorized` share `LeaseUnauthorized` on the wire, and that
/// is the security decision rather than a shortcut: a receiver cannot tell a
/// forgery from a random string, so reporting them apart would only ever tell an
/// attacker how close they got. They stay separate *here* because a caller logging
/// locally can usefully distinguish "somebody sent us junk" from "somebody sent us
/// a well-formed token signed with the wrong key", which is what a botched key
/// rollout looks like.
///
/// `FingerprintMismatch` reuses the existing wire code rather than minting a new
/// one: a client already answers it correctly, and a second spelling of one fact
/// is how two peers come to disagree about what happened.
inline constexpr EnumTable<LeaseRefusalReason, LeaseRefusalDescriptor> LeaseRefusalTable { {
    { .reason = LeaseRefusalReason::Malformed,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::Unauthorized,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::EndpointMismatch,
      .code = CompileCacheWire::ErrorCode::LeaseEndpointMismatch,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch },
    { .reason = LeaseRefusalReason::FingerprintMismatch,
      .code = CompileCacheWire::ErrorCode::FingerprintMismatch,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint },
    { .reason = LeaseRefusalReason::Expired,
      .code = CompileCacheWire::ErrorCode::LeaseExpired,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired },
} };

static_assert(RowsInEnumeratorOrder(LeaseRefusalTable, &LeaseRefusalDescriptor::reason),
              "LeaseRefusalTable must hold one row per LeaseRefusalReason, in enumerator order");

/// The row describing @p reason.
/// @param reason Why the lease was refused.
/// @return Its descriptor.
[[nodiscard]] constexpr LeaseRefusalDescriptor const& DescribeLeaseRefusal(LeaseRefusalReason reason) noexcept
{
    return LeaseRefusalTable[static_cast<std::size_t>(reason)];
}

/// A refusal, and what to tell the caller about it.
struct LeaseRefusal
{
    LeaseRefusalReason reason { LeaseRefusalReason::Malformed };

    /// Human-readable detail, or empty to use the error table's default message.
    ///
    /// Only ever populated for a refusal reached AFTER the MAC verified, which is
    /// what keeps it from being an oracle: everything it names is already in the
    /// token the caller is holding, in the clear. A caller that could not
    /// authenticate a token has established no fact about it, so there is nothing
    /// truthful to say beyond the default.
    std::string detail;
};

/// Domain separation label, folded in ahead of every field.
///
/// The cluster's pre-shared key already MACs discovery proofs, and one key serving
/// two constructions is how a tag produced for one purpose comes to be accepted for
/// the other. The differing field arity makes that implausible on its own; this
/// makes it impossible, for eighteen bytes on a message that is being hashed anyway.
inline constexpr std::string_view LeaseTokenDomain = "fastcache-lease-v1";

/// The token layout this build emits.
///
/// Carried as a field of its own rather than inferred, so a future layout is a
/// refusal by name in an old build rather than a mis-parse: the fields are
/// length-prefixed, so a different arity would otherwise decode as *something*.
inline constexpr std::uint8_t LeaseTokenVersion = 1;

/// How far a verifier's wall clock may trail the minting scheduler's.
///
/// Five minutes, and the figure is a judgement about which way to be wrong. A
/// fleet's machines are not all NTP-managed -- a workstation that has been asleep,
/// a container with no time source -- and an unsynchronised clock is minutes out,
/// not seconds. Too little slack refuses legitimate compiles on exactly the
/// machines nobody is watching; too much extends how long a *captured* token stays
/// useful, and the difference between a ten- and a fifteen-minute capture window is
/// nothing next to that.
///
/// **What the expiry does and does not enforce.** It bounds how long a token that
/// leaked is worth replaying. It is emphatically NOT a capacity bound: a worker
/// will accept an authentic lease for up to this long after the scheduler stopped
/// suppressing its key, and that is fine, because what stops a worker running more
/// than it should is its own slot accounting and its in-flight byte budget -- never
/// the lease. Anything built later on "an unexpired lease implies the scheduler
/// still holds capacity for it" is building on a bound that does not exist.
inline constexpr std::chrono::seconds LeaseTokenClockSkewSlack { 300 };

namespace Detail
{

    /// The claim fields, in wire order, ready to be packed or MACed.
    ///
    /// One function rather than one per direction, because a minter and a verifier
    /// that each spell this list are a minter and a verifier that will one day spell
    /// it differently -- which presents as every lease in the fleet failing to
    /// authenticate after a change nobody connected to it.
    ///
    /// The fields are length-prefixed rather than joined by a separator, which is
    /// `DiscoveryWire`'s scar: a separator that can occur inside a value is not a
    /// framing, so `{endpoint="a", key="b:1"}` and `{endpoint="a:b", key="1"}` would
    /// otherwise authenticate identically -- and an endpoint is `host:port`, so the
    /// separator is *always* inside a value here.
    ///
    /// @param version The layout tag.
    /// @param claims What is being claimed.
    /// @return The packed claim fields.
    [[nodiscard]] inline std::vector<std::byte> PackClaims(std::uint8_t version, LeaseClaims const& claims)
    {
        auto const versionField = std::array { static_cast<std::byte>(version) };
        auto const expiry = WireFields::ToBigEndian<std::uint64_t>(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(claims.expiresAt.time_since_epoch()).count()));
        return WireFields::Encode({
            std::span<std::byte const> { versionField },
            WireFields::AsBytes(claims.serial),
            WireFields::AsBytes(claims.endpoint),
            WireFields::AsBytes(claims.fingerprint),
            WireFields::AsBytes(claims.key),
            std::span<std::byte const> { expiry },
        });
    }

    /// The tag a holder of @p signingKey must produce for @p claims.
    /// @param signingKey The cluster's pre-shared key.
    /// @param packedClaims The output of `PackClaims`.
    /// @return The expected tag.
    [[nodiscard]] inline Sha256::Digest ExpectedTag(std::span<std::byte const> signingKey,
                                                    std::span<std::byte const> packedClaims)
    {
        auto const message = WireFields::Encode({ WireFields::AsBytes(LeaseTokenDomain), packedClaims });
        return HmacSha256(signingKey, message);
    }

    /// How many fields a token's outer envelope holds: the claims, and the tag.
    inline constexpr std::size_t EnvelopeFieldCount = 2;

    /// How many fields the packed claims hold.
    inline constexpr std::size_t ClaimFieldCount = 6;

    /// The largest expiry this host's wall clock can represent, in milliseconds.
    ///
    /// Refused at decode, before anything converts the number -- because the
    /// conversion is where it would go wrong. `system_clock::duration` is
    /// nanoseconds on libstdc++ and libc++, so a millisecond count anywhere near
    /// `uint64`'s ceiling overflows a signed 64-bit tick count on the way in, which
    /// is undefined behaviour rather than a large date. Derived from the clock
    /// rather than written as a year, so a standard library with a coarser tick
    /// gets the wider range it can actually hold instead of this file's guess.
    inline constexpr std::int64_t MaxExpiryMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::duration::max()).count();

} // namespace Detail

/// Mint a signed grant.
///
/// @param signingKey The cluster's pre-shared key; must not be empty. A caller with
///        no key does not call this -- it has nothing to sign with. Nothing here
///        enforces that, and nothing needs to: `AuthenticateLeaseToken` refuses an
///        empty key outright, so a token minted with one authenticates nowhere.
/// @param claims What the grant says.
/// @return The token, as base64 text.
[[nodiscard]] inline std::string MintLeaseToken(std::span<std::byte const> signingKey, LeaseClaims const& claims)
{
    auto const packed = Detail::PackClaims(LeaseTokenVersion, claims);
    auto const tag = Detail::ExpectedTag(signingKey, packed);
    auto const envelope = WireFields::Encode({ std::span<std::byte const> { packed }, std::span<std::byte const> { tag } });

    // Base64 rather than the raw bytes, which would travel perfectly well: the token
    // is a `string_view` on three wire fields, lands in the launcher's verbose output
    // and in whatever an operator pastes into a bug report, and this fleet's own rule
    // is that text a peer sends is text or `/fleet.json` stops parsing for everybody.
    // A raw-byte token is one report field away from breaking that; base64 makes it
    // structurally impossible.
    return Base64Encode(envelope);
}

/// Authenticate a token without checking who it names.
///
/// The half `SchedulerService::Release` needs: the scheduler is not a worker, so the
/// endpoint and fingerprint a grant names are not facts about *it*, but the serial
/// inside is what resolves the lease.
/// @param signingKey The cluster's pre-shared key.
/// @param token The token as presented.
/// @return The authentic claims, or why they are not.
[[nodiscard]] inline std::expected<LeaseClaims, LeaseRefusalReason> AuthenticateLeaseToken(
    std::span<std::byte const> signingKey, std::string_view token)
{
    // An empty key authenticates NOTHING, and that has to be said here rather than
    // left to each caller. An empty HMAC key is a perfectly valid HMAC key, so
    // without this a node whose key file failed to load would happily verify every
    // token minted by another node that also had none -- two machines agreeing on
    // "no secret" and calling it authentication. A caller that legitimately runs
    // without a key does not reach this function at all; it decides, in the open,
    // that it is not checking.
    if (signingKey.empty())
        return std::unexpected { LeaseRefusalReason::Unauthorized };

    auto const decoded = Base64Decode(token);
    if (!decoded.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };

    auto const envelope = WireFields::AsBytes(*decoded);
    auto const outer = WireFields::SplitExactly(envelope, Detail::EnvelopeFieldCount);
    if (!outer.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };

    auto const packed = (*outer)[0];
    auto const tag = (*outer)[1];
    if (tag.size() != Sha256::DigestSize)
        return std::unexpected { LeaseRefusalReason::Malformed };

    auto const fields = WireFields::SplitExactly(packed, Detail::ClaimFieldCount);
    if (!fields.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };
    if ((*fields)[0].size() != 1 || std::to_integer<std::uint8_t>((*fields)[0][0]) != LeaseTokenVersion)
        return std::unexpected { LeaseRefusalReason::Malformed };
    auto const expiryMillis = WireFields::FromBigEndian<std::uint64_t>((*fields)[5]);
    if (!expiryMillis.has_value() || *expiryMillis > static_cast<std::uint64_t>(Detail::MaxExpiryMillis))
        return std::unexpected { LeaseRefusalReason::Malformed };

    // The tag is recomputed over the bytes AS RECEIVED rather than over a re-encoding
    // of the decoded claims. Re-encoding would authenticate what this build would have
    // written, not what the peer sent, so any encoder asymmetry -- now or after a
    // future field is added -- would quietly accept a token whose bytes say something
    // else. `ConstantTimeEquals` because a caller can retry, and a comparison that
    // stops at the first difference lets them recover the tag one byte at a time.
    Sha256::Digest presented {};
    std::ranges::copy(tag, presented.begin());
    if (!ConstantTimeEquals(Detail::ExpectedTag(signingKey, packed), presented))
        return std::unexpected { LeaseRefusalReason::Unauthorized };

    return LeaseClaims { .serial = std::string { WireFields::AsStringView((*fields)[1]) },
                         .endpoint = std::string { WireFields::AsStringView((*fields)[2]) },
                         .fingerprint = std::string { WireFields::AsStringView((*fields)[3]) },
                         .key = std::string { WireFields::AsStringView((*fields)[4]) },
                         .expiresAt = std::chrono::system_clock::time_point {
                             std::chrono::milliseconds { static_cast<std::int64_t>(*expiryMillis) } } };
}

/// What a verifier expects a grant to say about itself.
struct LeaseExpectation
{
    std::string_view endpoint;    ///< The endpoint this worker registered under.
    std::string_view fingerprint; ///< The toolchain this worker is about to run.
};

/// Authenticate a grant and check it names this worker, this toolchain, and now.
///
/// **The MAC is checked first, and the order is the whole design.** Every refusal
/// below it is only ever reported for a token that provably came from the
/// scheduler, so a forger learns exactly one thing -- that their forgery failed --
/// while an operator whose worker advertises `build-07:6675` where the scheduler
/// granted `10.0.0.7:6675` is told precisely that. Checking the plaintext endpoint
/// first would be cheaper and would turn a diagnostic into an oracle.
///
/// @param signingKey The cluster's pre-shared key.
/// @param token The token the client presented.
/// @param expected What this worker is.
/// @param now This machine's wall clock.
/// @param slack How far this clock may trail the scheduler's.
/// @return The authentic claims, or why the job is refused.
[[nodiscard]] inline std::expected<LeaseClaims, LeaseRefusal> VerifyLeaseToken(
    std::span<std::byte const> signingKey,
    std::string_view token,
    LeaseExpectation const& expected,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds slack = LeaseTokenClockSkewSlack)
{
    auto authentic = AuthenticateLeaseToken(signingKey, token);
    if (!authentic.has_value())
        return std::unexpected { LeaseRefusal { .reason = authentic.error(), .detail = {} } };

    if (authentic->endpoint != expected.endpoint)
        return std::unexpected { LeaseRefusal {
            .reason = LeaseRefusalReason::EndpointMismatch,
            .detail = std::format(
                "this lease was issued for {}; this worker answers on {}", authentic->endpoint, expected.endpoint) } };

    if (authentic->fingerprint != expected.fingerprint)
        return std::unexpected { LeaseRefusal { .reason = LeaseRefusalReason::FingerprintMismatch, .detail = {} } };

    // Not `now > expiresAt + slack`: that sum pushes an already-large instant past
    // the tick count's ceiling, and `expiresAt` is attacker-chosen up to
    // `MaxExpiryMillis`.
    //
    // The difference is not unconditionally safe either, which is what the earlier
    // spelling of this comment claimed. `max - min` overflows like any other, and
    // `expiresAt` is non-negative by construction (it decodes from an unsigned
    // millisecond count) while `now` is whatever this host's wall clock says --
    // including a pre-epoch instant on a machine with a dead RTC. So the ordering is
    // established with a COMPARISON first, which cannot overflow, and the subtraction
    // only ever runs on `now > expiresAt >= epoch`, where the result is bounded by
    // `now` and therefore representable.
    if (now <= authentic->expiresAt)
        return std::move(*authentic);

    auto const age = now - authentic->expiresAt;
    if (age > slack)
        return std::unexpected { LeaseRefusal {
            .reason = LeaseRefusalReason::Expired,
            .detail = std::format("this lease expired {} seconds ago, allowing {} seconds of clock skew",
                                  std::chrono::duration_cast<std::chrono::seconds>(age).count(),
                                  slack.count()) } };

    return std::move(*authentic);
}

} // namespace FastCache::Distributed
