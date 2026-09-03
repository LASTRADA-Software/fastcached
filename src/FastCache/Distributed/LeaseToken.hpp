// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

    /// Which cluster issued it.
    ///
    /// Inside the MAC for the reason `endpoint` is: without it a grant is good on
    /// every fleet that trusts the same key, and two fleets sharing a key file is
    /// the ORDINARY outcome of copying a working configuration to a second site or
    /// cloning staging from production. The MAC verifies, the endpoint matches, the
    /// fingerprint matches, the expiry is in the future -- and cluster B's worker
    /// compiles work leased by a scheduler that is not its own
    /// ([#322](https://github.com/LASTRADA-Software/fastcached/issues/322)).
    ///
    /// Empty is legal and means a node with no `--cluster-id`, which is the
    /// one-machine deployment. A verifier that has none expects none: two nodes that
    /// have both declined to name a cluster are not thereby in different ones.
    std::string clusterId;

    /// The scheduler term this grant was issued under.
    ///
    /// The cluster id closes the door between two FLEETS; this one closes it between
    /// two leaders of the same fleet. Without it a grant minted under an old term
    /// stays good after a new leader takes over, so a token captured before an
    /// election is replayable after it.
    ///
    /// The Raft term rather than a counter the scheduler keeps: a per-process counter
    /// restarts at zero with the process, and a restarted leader would then re-mint
    /// epochs it had already used. Zero is the term of a node leading alone with no
    /// consensus, which is a real deployment and not a missing answer.
    std::uint64_t epoch {};

    // Declared LAST, matching the wire order in `PackClaims`: these two are appended
    // after the fields version 1 carried, so the struct reads in the order the bytes
    // do and a designated initializer lists them in the order it declares them.
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
    /// An authentic lease, issued by a different fleet that shares this key.
    ClusterMismatch,
    /// An authentic lease, issued under a scheduler term that is no longer current.
    EpochMismatch,
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
///
/// `ClusterMismatch` and `EpochMismatch` share `LeaseUnauthorized` too, and each
/// keeps a counter of its own. The wire code is the same because the client's answer
/// is the same -- compile it locally -- and a code it does not know would be worse
/// than one it does. The counters are separate because the OPERATOR's answer is not:
/// a rise in the first says two fleets are sharing a key file, which is a
/// provisioning mistake, and a rise in the second says grants are outliving
/// elections, which is not. Summing them would send somebody to the wrong one.
inline constexpr EnumTable<LeaseRefusalReason, LeaseRefusalDescriptor> LeaseRefusalTable { {
    { .reason = LeaseRefusalReason::Malformed,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::Unauthorized,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::ClusterMismatch,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseWrongCluster },
    { .reason = LeaseRefusalReason::EpochMismatch,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseStaleEpoch },
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

/// The token layout this build emits.
///
/// Carried as a field of its own rather than inferred, so a future layout is a
/// refusal by name in an old build rather than a mis-parse: the fields are
/// length-prefixed, so a different arity would otherwise decode as *something*.
///
/// **2 since #322**, which added the cluster id and the epoch to what the MAC
/// covers. Every outstanding version-1 grant stops authenticating, which is exactly
/// why that ticket argued for doing it while the format is new: a covered field
/// added after a fleet is deployed is a flag day, and nothing is deployed.
inline constexpr std::uint8_t LeaseTokenVersion = 2;

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
        auto const epoch = WireFields::ToBigEndian<std::uint64_t>(claims.epoch);
        return WireFields::Encode({
            std::span<std::byte const> { versionField },
            WireFields::AsBytes(claims.serial),
            WireFields::AsBytes(claims.endpoint),
            WireFields::AsBytes(claims.fingerprint),
            WireFields::AsBytes(claims.key),
            std::span<std::byte const> { expiry },
            WireFields::AsBytes(claims.clusterId),
            std::span<std::byte const> { epoch },
        });
    }

    /// How many fields a token's outer envelope holds: the claims, and the tag.
    inline constexpr std::size_t EnvelopeFieldCount = 2;

    /// How many fields the packed claims hold.
    inline constexpr std::size_t ClaimFieldCount = 8;

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

    // The claims go into the message as ONE field rather than as eight, which is
    // what lets `AuthenticateLeaseToken` authenticate the bytes a peer actually
    // sent instead of a re-encoding of them. Signed through the seam directly, so
    // the mint and the verify below reach one construction through one door --
    // there was briefly a `Detail::ExpectedTag` wrapper here, and once verify moved
    // onto `VerifyFields` it covered only half the pair it existed to keep together.
    auto const tag =
        Cluster::SignFields(signingKey, Cluster::SigningDomain::LeaseToken, { std::span<std::byte const> { packed } });
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
    auto const epoch = WireFields::FromBigEndian<std::uint64_t>((*fields)[7]);
    if (!epoch.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };

    // The tag is recomputed over the bytes AS RECEIVED rather than over a re-encoding
    // of the decoded claims. Re-encoding would authenticate what this build would have
    // written, not what the peer sent, so any encoder asymmetry -- now or after a
    // future field is added -- would quietly accept a token whose bytes say something
    // else. `Cluster::VerifyFields` compares in constant time because a caller can
    // retry, and a comparison that stops at the first difference lets them recover
    // the tag one byte at a time.
    Sha256::Digest presented {};
    std::ranges::copy(tag, presented.begin());
    if (!Cluster::VerifyFields(signingKey, Cluster::SigningDomain::LeaseToken, { packed }, presented))
        return std::unexpected { LeaseRefusalReason::Unauthorized };

    return LeaseClaims { .serial = std::string { WireFields::AsStringView((*fields)[1]) },
                         .endpoint = std::string { WireFields::AsStringView((*fields)[2]) },
                         .fingerprint = std::string { WireFields::AsStringView((*fields)[3]) },
                         .key = std::string { WireFields::AsStringView((*fields)[4]) },
                         .expiresAt = std::chrono::system_clock::time_point { std::chrono::milliseconds {
                             static_cast<std::int64_t>(*expiryMillis) } },
                         .clusterId = std::string { WireFields::AsStringView((*fields)[6]) },
                         .epoch = *epoch };
}

/// Whether this verifier knows which scheduler term is current.
///
/// A type rather than an `std::optional<std::uint64_t>`, and the difference is that
/// this one cannot be defaulted. An omitted optional value-initializes to "no
/// expectation", so a verifier added later would silently accept a grant from any
/// term -- which is the hole this class exists to close, reopened by saying nothing.
/// `PreAuth` and `PayloadCap` in the wire table are spelled the same way for the same
/// reason.
///
/// **`NotKnownHere` is a real answer and not a placeholder**, and it stayed the only
/// answer until #421 gave a worker somewhere to learn the term FROM -- the heartbeat
/// reply, plus any authentic grant naming a newer one. A verifier that has never
/// learned one still says so here, because a worker that refused everything before
/// its first heartbeat could not cold-start and every fleet would deadlock on
/// restart. So the states are three, not two: never learned, learned, and a refusal
/// reachable only from the second.
///
/// **`NotOlderThan`, not `MustEqual`, and the asymmetry is the design.** A grant
/// naming a term BELOW the one this worker knows was issued by a scheduler that has
/// since been deposed: that is what the epoch is for, and it is refused. A grant
/// naming a term ABOVE it is accepted, because the worker is the one that is behind
/// -- the scheduler is authoritative about its own term and the MAC has already
/// proved the grant authentic. Refusing that direction would make a worker which
/// missed one heartbeat reject the new leader, which is the fleet quietly ceasing to
/// distribute right after an election: the exact failure #421 exists to prevent,
/// produced by #421.
///
/// The consequence worth stating plainly is that **a worker's own staleness can never
/// cause a refusal.** That is what removes the availability trade the ticket expected
/// to have to make, and it is this project's caching rule in another setting:
/// staleness is safe to hold when it fails closed and self-heals, and adopting
/// forward is the self-healing half.
class LeaseEpochCheck
{
  public:
    /// Deleted on purpose: a verifier states what it knows. See the class comment.
    LeaseEpochCheck() = delete;

    /// This verifier cannot know the current term, so it does not check one.
    /// @return The check.
    [[nodiscard]] static constexpr LeaseEpochCheck NotKnownHere() noexcept
    {
        return LeaseEpochCheck { false, 0 };
    }

    /// This verifier knows a term, and refuses any grant issued STRICTLY BEFORE it.
    ///
    /// Named for what it does rather than for the field it holds: `MustEqual` was the
    /// obvious spelling and would have been a lie, since a grant from a later term is
    /// accepted. See the class comment for why that direction has to be accepted.
    /// @param epoch The most recent term this verifier has learned.
    /// @return The check.
    [[nodiscard]] static constexpr LeaseEpochCheck NotOlderThan(std::uint64_t epoch) noexcept
    {
        return LeaseEpochCheck { true, epoch };
    }

    /// @param claimed The term the grant names.
    /// @return True when this verifier accepts it.
    [[nodiscard]] constexpr bool Accepts(std::uint64_t claimed) const noexcept
    {
        return !_checked || claimed >= _epoch;
    }

    /// @return The most recent term this verifier has learned; meaningless when it
    ///         has learned none.
    [[nodiscard]] constexpr std::uint64_t Expected() const noexcept
    {
        return _epoch;
    }

    /// @return Whether this verifier checks the term at all.
    [[nodiscard]] constexpr bool Checked() const noexcept
    {
        return _checked;
    }

  private:
    constexpr LeaseEpochCheck(bool checked, std::uint64_t epoch) noexcept:
        _checked { checked },
        _epoch { epoch }
    {
    }

    bool _checked;
    std::uint64_t _epoch;
};

/// The most recent scheduler term a worker has been told about, if any.
///
/// The mutable companion of `LeaseEpochCheck`, which is a value: this is the thing a
/// worker LEARNS into and the check is the immutable reading it hands the verifier.
/// It lives beside that type rather than beside the worker because both binaries
/// compile `WorkerProtocol.cpp` and only one of them links `FastCache` -- a seam with
/// a `.cpp` would need a new `_fc_cc_core` row, and this needs none.
///
/// **One channel writes it: an authentic grant.** #421's first shape also had the
/// HEARTBEAT reply state the term, which was faster and scheduler-driven, and it was
/// deleted before it shipped. That reply is unauthenticated -- `Credential` is
/// client-to-server and the frame surface is plaintext -- so anything able to answer a
/// worker's `--scheduler` dial could write `UINT64_MAX` here once and the worker would
/// refuse every authentic grant until it restarted. A value that only ever rises is a
/// ratchet, and a ratchet an unauthenticated peer can turn is a permanent denial of
/// service rather than a transient one. The grant channel has no such problem: the MAC
/// is verified first, so only the scheduler can move this number.
///
/// **Monotonic, so a late writer cannot walk it backwards.** Raft terms only ever
/// increase within a cluster, and the MAC binds the cluster -- so a lower number
/// reaching `Learn` is a stale message overtaking a fresh one, not a demotion, and
/// taking the maximum is what makes the order two threads happen to write in
/// irrelevant.
///
/// **Never-learned is a flag, not a term value.** `StandaloneSchedulerTerm` is
/// literally `0`, so a zero sentinel would make a single-node fleet's real term
/// indistinguishable from "nobody has told me anything" -- two states, one
/// representation, which is the collapse this project keeps paying for. The cost is a
/// second atomic and an ordering that has to be stated: `_term` is published first
/// and `_learned` released after it, so a reader that acquires `_learned == true`
/// sees a `_term` at least as new. A reader that sees `false` accepts everything,
/// which is the safe direction.
///
/// A term learned too LATE costs nothing at all: the check accepts anything not older,
/// so a worker that is behind refuses nothing and catches up from the next grant.
///
/// The sentence that used to sit here claimed a term could never be learned too EARLY,
/// "because there is no channel that reports a term before a scheduler has entered
/// it". It was written beside a channel for which it was false, and is recorded as
/// deleted rather than quietly dropped: what makes the remaining channel safe is the
/// MAC, not the absence of a way to lie.
class KnownSchedulerTerm
{
  public:
    /// Record a term this worker has been told about, keeping the highest seen.
    /// @param term The scheduler term.
    void Learn(std::uint64_t term) noexcept
    {
        auto seen = _term.load(std::memory_order_relaxed);
        while (term > seen && !_term.compare_exchange_weak(seen, term, std::memory_order_relaxed))
        {
        }

        // Guarded, because this runs on every dispatched compile and publishes a
        // transition that happens once per process. The two members share a cache
        // line, so an unconditional release store took it exclusive per compile and
        // forced every other worker thread's next `Check()` to re-acquire it -- a
        // coherence round trip to republish a flag that has been true since the first
        // heartbeat. The relaxed load reads the line without invalidating it.
        //
        // What the guard gives up, stated because it is the half that is not obvious:
        // a LATER `Learn` that raises `_term` no longer carries a release edge. Nothing
        // races -- `_term` is atomic — and the only reachable effect is a reader seeing
        // an older term, which accepts more and never refuses. That is the direction
        // this class is safe in by design.
        if (!_learned.load(std::memory_order_relaxed))
            _learned.store(true, std::memory_order_release);
    }

    /// @return What a verifier should expect of a grant's term right now.
    [[nodiscard]] LeaseEpochCheck Check() const noexcept
    {
        if (!_learned.load(std::memory_order_acquire))
            return LeaseEpochCheck::NotKnownHere();
        return LeaseEpochCheck::NotOlderThan(_term.load(std::memory_order_relaxed));
    }

  private:
    std::atomic<std::uint64_t> _term { 0 };
    std::atomic<bool> _learned { false };
};

/// What a verifier expects a grant to say about itself.
struct LeaseExpectation
{
    std::string_view endpoint;    ///< The endpoint this worker registered under.
    std::string_view fingerprint; ///< The toolchain this worker is about to run.

    /// The cluster this verifier belongs to; empty when it names none.
    ///
    /// Compared for EQUALITY rather than for presence, so two nodes that have both
    /// declined to name a cluster still agree -- that is the one-machine deployment
    /// and it must keep working -- while a node that names one refuses a grant from a
    /// fleet that names another, or none.
    std::string_view clusterId;

    /// Whether this verifier knows the current term. No default: see
    /// `LeaseEpochCheck`.
    LeaseEpochCheck epoch;
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
/// The cluster and the epoch are checked FIRST, ahead of the endpoint and the
/// fingerprint, because they answer a different question: those two ask "is this
/// grant for me", these ask "is it from anybody I take orders from" (#322). Both
/// still sit below the MAC, so neither is an oracle -- everything they report is
/// already in the token the caller is holding.
///
/// @param signingKey The cluster's pre-shared key.
/// @param token The token the client presented.
/// @param expected What this worker is, and what it knows.
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

    // Before the endpoint and the fingerprint, because those answer "is this grant
    // for me" while these answer "is this grant from anyone I take orders from". A
    // token from another fleet that happens to name this endpoint would otherwise be
    // reported as a match on the way to being refused for something else.
    if (authentic->clusterId != expected.clusterId)
        return std::unexpected { LeaseRefusal {
            .reason = LeaseRefusalReason::ClusterMismatch,
            .detail = std::format("this lease was issued by cluster '{}'; this worker belongs to '{}'",
                                  authentic->clusterId,
                                  expected.clusterId) } };

    if (!expected.epoch.Accepts(authentic->epoch))
        return std::unexpected { LeaseRefusal {
            .reason = LeaseRefusalReason::EpochMismatch,
            .detail = std::format("this lease was issued under scheduler term {}; this worker has seen term {}",
                                  authentic->epoch,
                                  expected.epoch.Expected()) } };

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
