// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Ed25519.hpp>
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
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
/// `FastCache`. Being header-only over `Core/Ed25519`, `Core/Base64` and the
/// header-only `Core/WireFields` is what lets the launcher's build reach this
/// with dependency-free rows in `_fc_cc_core` -- and the vendored Monocypher, which
/// depends on nothing -- rather than a link against the library: the same rule
/// `Protocol/CompileCacheWire.hpp` states about itself, and for the same reason.
///
/// ## Signed by a MEMBER, not by the cluster (#178)
///
/// A grant was an HMAC under the cluster's pre-shared key until #178, so every holder of the
/// key could mint one, and a machine removed from the cluster went on minting grants every
/// worker honoured. It is now an Ed25519 signature by the issuing voter's OWN identity key,
/// and it names that voter: a worker verifies it against the key its roster holds for the
/// named signer, and refuses a signer that is not a voter there -- or whose key the cluster
/// revoked -- however well-formed the grant is.
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
    /// Inside the signature for the reason `endpoint` is: without it a grant is good on
    /// every fleet that trusts the same key. That was the pre-shared key until #178, and
    /// two fleets sharing a key file was the ORDINARY outcome of copying a working
    /// configuration to a second site or cloning staging from production -- the MAC
    /// verified, the endpoint matched, the fingerprint matched, the expiry was in the
    /// future, and cluster B's worker compiled work leased by a scheduler that was not its
    /// own ([#322](https://github.com/LASTRADA-Software/fastcached/issues/322)). A grant is
    /// signed by a voter's own identity key now, and the same outcome survives one layer
    /// down: a state directory copied to a second site copies the node, key included.
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

    /// The voter whose identity key signed this grant (#178).
    ///
    /// Read BEFORE the signature is checked, and that is not the oracle the
    /// signature-first rule guards against: the claimed signer only SELECTS the key to
    /// verify under, exactly as a Raft proof's claimed id does. A signer the verifier's
    /// roster names no key for is refused as unauthorized, with nothing reported about the
    /// rest of the grant.
    std::string signer;

    // Declared in wire order, matching `PackClaims`, so the struct reads in the order the
    // bytes do and a designated initializer lists them in the order it declares them.
};

/// Why a lease was refused. One enumerator per outcome that is not "compile it".
///
/// Ordered as the checks run, which is also the order of increasing information: the
/// first four are reachable by anybody, and everything below them is only ever reported
/// for a token whose signature has already verified. Private to this process -- a refusal
/// travels as its row's wire code, never as this ordinal -- so a row may be inserted where
/// its check runs.
enum class LeaseRefusalReason : std::uint8_t
{
    /// Not a lease token at all -- not base64, not this format, not this version.
    Malformed,
    /// This worker holds no roster it can verify a grant against: it has not yet been
    /// handed one it could certify, and holds none from an earlier run (#178).
    ///
    /// A fact about THIS WORKER, like `Unregistered`, so answering it before the signature
    /// is no oracle. Its own reason rather than a share of `RosterExpired`, because the
    /// operator actions are opposite: this one never reached a leader whose roster its
    /// anchors certify, while an expired one did and has since been cut off.
    NoRoster,
    /// This worker's roster has not been re-certified by a majority of its voters for longer
    /// than its lifetime and the skew slack (#178).
    ///
    /// The bound on how long a worker cut off from the cluster -- or kept talking to an
    /// ex-leader that withholds every newer roster -- goes on trusting the voters it last
    /// heard of. Past it, a grant signed by a machine the cluster has since revoked would
    /// verify, so none is honoured.
    RosterExpired,
    /// A grant no key this worker's roster holds for the named signer verifies: an unknown
    /// signer, a signer that is not a voter, or a forgery.
    Unauthorized,
    /// A grant whose signature verifies under a key the cluster has REVOKED (#178).
    ///
    /// The removed machine itself, still minting. Its own reason rather than a share of
    /// `Unauthorized` because the operator action is opposite: a rise here names a machine
    /// somebody removed and nobody stopped, while `Unauthorized` is junk, a forgery or a
    /// roster that has not caught up. The wire code is shared, because the client's answer
    /// is the same.
    SignerRevoked,
    /// An authentic lease reaching a worker that has not registered, so it has no
    /// fleet to compare against.
    ///
    /// Its own reason rather than a share of `ClusterMismatch`, because the operator
    /// actions are opposite: a mismatch means one identity key votes in two fleets, while this
    /// means a worker is serving before its first registration round completed --
    /// which is ordinary and transient at startup, and permanent if the scheduler is
    /// unreachable. Collapsing them would report a key-provisioning fault every time
    /// a node started (#401).
    Unregistered,
    /// An authentic lease, issued by a different fleet whose voter holds a key this roster
    /// trusts -- one identity key voting in two clusters.
    ClusterMismatch,
    /// An authentic lease, for a different worker.
    EndpointMismatch,
    /// An authentic lease, for a different toolchain.
    FingerprintMismatch,
    /// An authentic lease, past its expiry and the skew slack.
    Expired,
    /// An authentic, unexpired lease this worker has already spent.
    ///
    /// LAST because it is the last check, and it is last because spending is the one
    /// step with a side effect: a grant consumed on the way to being refused for some
    /// other reason would be a grant nobody got to use. Everything above it is a pure
    /// reading of the token.
    Replayed,
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
/// `NoRoster` and `RosterExpired` share a code of their own, `RosterExpired` (#178):
/// neither is a statement about the grant, both say this WORKER cannot verify anybody's
/// grant right now, and a client that read either as "your lease is bad" would stop asking
/// the right scheduler for a fresh one. `SignerRevoked` shares `LeaseUnauthorized` and keeps
/// a counter of its own, for `ClusterMismatch`'s reason below.
///
/// `ClusterMismatch` and `Replayed` share `LeaseUnauthorized` too, and each keeps a
/// counter of its own. The wire code is the same because the client's answer is the
/// same -- compile it locally -- and a code it does not know would be worse than one
/// it does. The counters are separate because the OPERATOR's answer is not: a rise in
/// the first says one identity key votes in two fleets, which is a provisioning mistake,
/// and a rise in the second says somebody is presenting a grant that has already been
/// spent, which is not. Summing them would send somebody to the wrong one.
///
/// There used to be an `EpochMismatch` row here, refusing an authentic grant that named
/// a superseded scheduler term, and it is gone rather than merely unused
/// ([#614](https://github.com/LASTRADA-Software/fastcached/issues/614)). `Replayed` is
/// what stops a captured grant being spent twice, and once that is enforced a lower term
/// is a scheduler that was legitimately reset -- see `KnownSchedulerTerm`, which now
/// adopts one instead of refusing it. Keeping the row would have left a refusal nothing
/// can reach and a series that reads zero because the event is impossible rather than
/// because it did not happen: two facts an operator cannot tell apart.
inline constexpr EnumTable<LeaseRefusalReason, LeaseRefusalDescriptor> LeaseRefusalTable { {
    { .reason = LeaseRefusalReason::Malformed,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::NoRoster,
      .code = CompileCacheWire::ErrorCode::RosterExpired,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseNoRoster },
    { .reason = LeaseRefusalReason::RosterExpired,
      .code = CompileCacheWire::ErrorCode::RosterExpired,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseRosterExpired },
    { .reason = LeaseRefusalReason::Unauthorized,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnauthorized },
    { .reason = LeaseRefusalReason::SignerRevoked,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseSignerRevoked },
    { .reason = LeaseRefusalReason::Unregistered,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseUnregistered },
    { .reason = LeaseRefusalReason::ClusterMismatch,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseWrongCluster },
    { .reason = LeaseRefusalReason::EndpointMismatch,
      .code = CompileCacheWire::ErrorCode::LeaseEndpointMismatch,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseEndpointMismatch },
    { .reason = LeaseRefusalReason::FingerprintMismatch,
      .code = CompileCacheWire::ErrorCode::FingerprintMismatch,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedUnknownFingerprint },
    { .reason = LeaseRefusalReason::Expired,
      .code = CompileCacheWire::ErrorCode::LeaseExpired,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseExpired },
    { .reason = LeaseRefusalReason::Replayed,
      .code = CompileCacheWire::ErrorCode::LeaseUnauthorized,
      .workerCounter = IMetricsSink::Counter::WorkerJobsRefusedLeaseReplayed },
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
///
/// **3 since #178**: the grant names its signer and carries an Ed25519 signature by that
/// voter's identity key where it carried an HMAC under the cluster key. The envelope's
/// second field changed width and meaning, so a version-2 grant is refused as malformed
/// rather than read.
inline constexpr std::uint8_t LeaseTokenVersion = 3;

/// The label a grant's signature is made under (#178).
///
/// Its own label, beside the grant it signs, as every construction signed by a member's own key
/// carries one: a label distinct from every other is what keeps a signature made for one from
/// verifying as another. Versioned
/// with the token, so a signature over a version-2 claim list could never verify as a
/// version-3 one even under the same key. `fastcache-lease-v1`, the HMAC label, is retired
/// and never reused.
inline constexpr std::string_view LeaseSignatureLabel = "fastcache-lease-v3";

/// Who signs a grant: the issuing scheduler's identity (#178).
///
/// A seam rather than a key pair, so the private key stays wherever the node keeps it --
/// `SchedulerService` holds no secret -- and a test signs through the same door production
/// does.
class ILeaseSigner
{
  public:
    ILeaseSigner() = default;
    ILeaseSigner(ILeaseSigner const&) = delete;
    ILeaseSigner(ILeaseSigner&&) = delete;
    ILeaseSigner& operator=(ILeaseSigner const&) = delete;
    ILeaseSigner& operator=(ILeaseSigner&&) = delete;
    virtual ~ILeaseSigner() = default;

    /// @return The id a grant names as its signer: this node's member id.
    [[nodiscard]] virtual std::string_view SignerId() const = 0;

    /// @return The public half of the key `Sign` signs with -- what a release handed back to
    ///         this scheduler is verified under.
    [[nodiscard]] virtual Ed25519PublicKey PublicKey() const = 0;

    /// Sign @p message with this node's identity key.
    /// @param message What is signed.
    /// @return The signature.
    [[nodiscard]] virtual Ed25519Signature Sign(std::span<std::byte const> message) const = 0;
};

/// What a verifier's roster says about the keys one signer id may sign a grant with.
struct LeaseSignerKeys
{
    /// The key the id signs with NOW -- present only for a VOTER the roster records a key
    /// for. A learner, a principal and a stranger have none: a grant is a scheduler's, and
    /// only a voter can lead.
    std::optional<Ed25519PublicKey> live;

    /// Every key the roster has revoked, whichever id it was revoked under: a signature that
    /// verifies under one is reported as the removed machine rather than as a forgery. The
    /// id a revocation carries is a label, so the SIGNATURE says who signed.
    std::vector<Ed25519PublicKey> revoked;
};

/// Where a verifier reads who may sign a grant.
class ILeaseSignerKeys
{
  public:
    ILeaseSignerKeys() = default;
    ILeaseSignerKeys(ILeaseSignerKeys const&) = delete;
    ILeaseSignerKeys(ILeaseSignerKeys&&) = delete;
    ILeaseSignerKeys& operator=(ILeaseSignerKeys const&) = delete;
    ILeaseSignerKeys& operator=(ILeaseSignerKeys&&) = delete;
    virtual ~ILeaseSignerKeys() = default;

    /// @param signer The id a grant names.
    /// @return What the roster says about it now.
    [[nodiscard]] virtual LeaseSignerKeys KeysOf(std::string_view signer) const = 0;
};

/// Whether a worker's roster may be trusted at an instant (#178).
///
/// **PRIVATE: persisted and transmitted nowhere.** Three answers rather than a `bool`,
/// because the two ways of being unusable are opposite operator actions: a worker with no
/// roster never reached a leader its anchors certify, while an expired one did and has since
/// been cut off.
enum class RosterStanding : std::uint8_t
{
    Current, ///< Trusted: certified, and not past its certification and the slack.
    Expired, ///< Held, and past its certification and the slack.
    Absent,  ///< Nothing a grant could be verified against.
};

/// What a worker's roster says about itself at an instant.
struct RosterReading
{
    RosterStanding standing { RosterStanding::Absent }; ///< Whether it may be trusted.

    /// When its certification lapses -- or lapsed. Absent for no roster at all, and for a
    /// roster that needs no certificate: a consensus member's own applied state.
    std::optional<std::chrono::system_clock::time_point> certifiedUntil;
};

/// A worker's roster: who may sign its grants, and whether it may be trusted now (#178).
///
/// Implemented twice, because a node answers the question two ways: a consensus member from
/// the state it applied, which needs no certificate, and a worker with no consensus from the
/// certified roster it adopted (`Distributed::RosterTrust`). Both are read per request, so an
/// applied revocation or an adopted roster reaches the next grant rather than the next restart.
class ILeaseRoster: public ILeaseSignerKeys
{
  public:
    /// @param now This machine's wall clock.
    /// @return Whether the roster may be trusted at @p now, and until when.
    [[nodiscard]] virtual RosterReading Read(std::chrono::system_clock::time_point now) const = 0;
};

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

    /// Whether a grant that expires at @p expiresAt is still good at @p now.
    ///
    /// **The acceptance window, in one expression, because two things depend on it and
    /// they must not be able to disagree** ([#614](https://github.com/LASTRADA-Software/fastcached/issues/614)).
    /// `VerifyLeaseToken` asks it to decide whether to accept a grant; `SpentLeases`
    /// asks it to decide how long to remember one. Written twice, the second copy is
    /// free to prune at `expiresAt` while the first goes on accepting for another five
    /// minutes -- and a spend that expires before the thing it is spending leaves a
    /// captured grant replayable again for the rest of that window. A shared CONSTANT
    /// would not have been enough: both sites would still spell the comparison, and the
    /// comparison is the part that is easy to get wrong.
    ///
    /// **The arithmetic, which is not obvious.** Not `now > expiresAt + slack`: that sum
    /// pushes an already-large instant past the tick count's ceiling, and `expiresAt` is
    /// attacker-chosen up to `MaxExpiryMillis`. The difference is not unconditionally
    /// safe either -- `max - min` overflows like any other, and while `expiresAt` is
    /// non-negative by construction (it decodes from an unsigned millisecond count),
    /// `now` is whatever this host's wall clock says, including a pre-epoch instant on a
    /// machine with a dead RTC. So the ordering is established with a COMPARISON first,
    /// which cannot overflow, and the subtraction only ever runs on
    /// `now > expiresAt >= epoch`, where the result is bounded by `now` and therefore
    /// representable.
    ///
    /// @param expiresAt When the grant stops being good, from its own claims.
    /// @param now This machine's wall clock.
    /// @param slack How far this clock may trail the minting scheduler's.
    /// @return True while the grant is still worth anything to anybody.
    [[nodiscard]] constexpr bool WithinAcceptanceWindow(std::chrono::system_clock::time_point expiresAt,
                                                        std::chrono::system_clock::time_point now,
                                                        std::chrono::seconds slack) noexcept
    {
        if (now <= expiresAt)
            return true;
        return now - expiresAt <= slack;
    }

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
            WireFields::AsBytes(claims.signer),
        });
    }

    /// What a grant's signature is over: its label, then the claims AS PACKED -- one field,
    /// so a verifier signs over the bytes a peer sent rather than a re-encoding of them.
    /// One function for the minter and the verifier, for `PackClaims`' reason.
    /// @param packed The packed claims.
    /// @return The signed message.
    [[nodiscard]] inline std::vector<std::byte> SignedLeaseMessage(std::span<std::byte const> packed)
    {
        return WireFields::Encode({ WireFields::AsBytes(LeaseSignatureLabel), packed });
    }

    /// How many fields a token's outer envelope holds: the claims, and the signature.
    inline constexpr std::size_t EnvelopeFieldCount = 2;

    /// How many fields the packed claims hold.
    inline constexpr std::size_t ClaimFieldCount = 9;

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
/// @param signer The issuing scheduler's identity. The grant names it as its signer
///        whatever @p claims says, so a grant can never name one member and carry
///        another's signature.
/// @param claims What the grant says.
/// @return The token, as base64 text.
[[nodiscard]] inline std::string MintLeaseToken(ILeaseSigner const& signer, LeaseClaims claims)
{
    claims.signer = std::string { signer.SignerId() };
    auto const packed = Detail::PackClaims(LeaseTokenVersion, claims);

    // The claims go into the message as ONE field rather than as nine, which is
    // what lets `AuthenticateLeaseToken` verify the bytes a peer actually sent
    // instead of a re-encoding of them.
    auto const signature = signer.Sign(Detail::SignedLeaseMessage(packed));
    auto const envelope =
        WireFields::Encode({ std::span<std::byte const> { packed }, std::span<std::byte const> { signature } });

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
/// @param signers Who may sign a grant, as the verifier's roster says.
/// @param token The token as presented.
/// @return The authentic claims, or why they are not: `Malformed`, `Unauthorized` or
///         `SignerRevoked`, and nothing else.
[[nodiscard]] inline std::expected<LeaseClaims, LeaseRefusalReason> AuthenticateLeaseToken(ILeaseSignerKeys const& signers,
                                                                                           std::string_view token)
{
    auto const decoded = Base64Decode(token);
    if (!decoded.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };

    auto const envelope = WireFields::AsBytes(*decoded);
    auto const outer = WireFields::SplitExactly(envelope, Detail::EnvelopeFieldCount);
    if (!outer.has_value())
        return std::unexpected { LeaseRefusalReason::Malformed };

    auto const packed = (*outer)[0];
    auto const signatureField = (*outer)[1];
    if (signatureField.size() != Ed25519SignatureBytes)
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

    // Verified over the bytes AS RECEIVED rather than over a re-encoding of the decoded
    // claims. Re-encoding would authenticate what this build would have written, not what
    // the peer sent, so any encoder asymmetry -- now or after a future field is added --
    // would quietly accept a token whose bytes say something else.
    //
    // The claimed signer SELECTS the key and is the only claim read before the signature
    // is, which is the Raft handshake's rule: nothing else is reported about a grant whose
    // signature has not verified.
    Ed25519Signature presented {};
    std::ranges::copy(signatureField, presented.begin());
    auto const message = Detail::SignedLeaseMessage(packed);
    auto const keys = signers.KeysOf(WireFields::AsStringView((*fields)[8]));
    if (!keys.live.has_value() || !Ed25519Verify(*keys.live, message, presented))
    {
        // Only a signature that VERIFIES under a revoked key is reported as one, so
        // `SignerRevoked` is a statement about who signed rather than about the claim.
        auto const byRevoked = std::ranges::any_of(
            keys.revoked, [&](Ed25519PublicKey const& revoked) { return Ed25519Verify(revoked, message, presented); });
        return std::unexpected { byRevoked ? LeaseRefusalReason::SignerRevoked : LeaseRefusalReason::Unauthorized };
    }

    return LeaseClaims { .serial = std::string { WireFields::AsStringView((*fields)[1]) },
                         .endpoint = std::string { WireFields::AsStringView((*fields)[2]) },
                         .fingerprint = std::string { WireFields::AsStringView((*fields)[3]) },
                         .key = std::string { WireFields::AsStringView((*fields)[4]) },
                         .expiresAt = std::chrono::system_clock::time_point { std::chrono::milliseconds {
                             static_cast<std::int64_t>(*expiryMillis) } },
                         .clusterId = std::string { WireFields::AsStringView((*fields)[6]) },
                         .epoch = *epoch,
                         .signer = std::string { WireFields::AsStringView((*fields)[8]) } };
}

/// What learning a scheduler term did to a worker's picture of the fleet.
///
/// Three outcomes and not a `bool`, because the one that matters is the one a boolean
/// cannot name: a term going BACKWARDS is a scheduler that was reset, and it is the
/// only condition here worth telling an operator about.
enum class TermTransition : std::uint8_t
{
    Unchanged, ///< The same term this worker already knew.
    Advanced,  ///< A newer term -- an election, or the first one ever learned.
    /// A term BELOW the one this worker knew.
    ///
    /// **Named for what was OBSERVED, never for what caused it**, and that is why this
    /// is not called `Reset`. A worker cannot tell a scheduler that was genuinely reset
    /// from a grant minted before a leadership change and delivered after one: both
    /// arrive as a lower term, both are authentic, and nothing in the token separates
    /// them. The second is ORDINARY -- a client holds its grant across a preprocess and
    /// a multi-megabyte upload, which is seconds, and an election inside that window
    /// puts a term-N grant behind a term-N+1 one at the same worker.
    Regressed,
};

/// What a `Learn` call did, and what it did it to.
///
/// The previous value travels because only the caller that performed the transition
/// can report it truthfully -- re-reading the member afterwards races another thread's
/// grant and would report a step that never happened.
struct TermChange
{
    TermTransition transition { TermTransition::Unchanged }; ///< Which way it moved.
    std::uint64_t previous {};                               ///< What this worker knew before.
    std::uint64_t current {};                                ///< What it knows now.
};

/// The most recent scheduler term a worker has been told about, if any.
///
/// **It is a diagnostic now, and it used to be a gate.** Until
/// [#614](https://github.com/LASTRADA-Software/fastcached/issues/614) this was a
/// monotonic maximum and a verifier refused any grant naming a lower term
/// (`LeaseEpochCheck`, deleted with that ticket). The monotonic rule was standing in
/// for replay protection, and `SpentLeases` below now provides that directly -- so the
/// term no longer decides anything, and a lower one is adopted rather than refused.
///
/// **Why the gate could not stay, measured rather than argued.** Three ordinary
/// operator actions drop a scheduler's term back to 0: wiping the Raft directory,
/// re-bootstrapping the cluster, turning consensus off. With a worker holding term 5
/// and a reset scheduler truthfully stating term 0, the old check was exactly
/// INVERTED -- the honest fresh grant was REFUSED and a term-5 token captured before
/// the reset was ACCEPTED, because the check asks `claimed >= expected` and 5 passes
/// where 0 does not. So the rule refused every legitimate grant, until every worker in
/// the fleet was restarted, while admitting the one thing it existed to stop.
///
/// **One channel writes it: an authentic grant that was also SPENT.** #421's first
/// shape also had the heartbeat reply state the term, which was deleted before it
/// shipped: that reply is unauthenticated, so anything able to answer a worker's
/// `--scheduler` dial could have written into this. The grant channel has no such
/// problem -- the MAC is verified first -- and since #614 the spend check runs before
/// this is written too, so a replayed grant cannot move the number either.
///
/// **Latest wins, not the maximum**, which is what makes a reset expressible at all.
/// Two threads writing concurrently may leave either value here, and that is harmless
/// in a way it would not have been before: nothing refuses on this number any more, so
/// the only consequence of landing on the older of two is one diagnostic line.
///
/// **Never-learned is a flag, not a term value.** `StandaloneSchedulerTerm` is
/// literally `0`, so a zero sentinel would make a single-node fleet's real term
/// indistinguishable from "nobody has told me anything" -- two states, one
/// representation, which is the collapse this project keeps paying for. It is also what
/// keeps the FIRST grant from reading as a reset: 0 arriving into an unlearned worker
/// is `Advanced`, and into a worker that knows 5 it is `Reset`.
class KnownSchedulerTerm
{
  public:
    /// Adopt the term an authentic, unspent grant named.
    ///
    /// @param term The scheduler term the grant carried.
    /// @return What this call changed, for the one caller that changed it.
    TermChange Learn(std::uint64_t term) noexcept
    {
        std::scoped_lock const guard { _mutex };
        auto const previous = _term;
        auto const known = _learned;
        _term = term;
        _learned = true;

        if (!known)
            return TermChange { .transition = TermTransition::Advanced, .previous = term, .current = term };
        if (term == previous)
            return TermChange { .transition = TermTransition::Unchanged, .previous = previous, .current = term };
        return TermChange { .transition = term > previous ? TermTransition::Advanced : TermTransition::Regressed,
                            .previous = previous,
                            .current = term };
    }

    /// @return The most recent term this worker has been told about, or nullopt when
    ///         it has been told none.
    [[nodiscard]] std::optional<std::uint64_t> Known() const
    {
        std::scoped_lock const guard { _mutex };
        return _learned ? std::optional { _term } : std::nullopt;
    }

  private:
    // A mutex rather than the pair of atomics this held before #614. The read-then-
    // decide-then-write is one transaction now -- a `TermChange` computed from a value
    // another thread has already overwritten describes a step nothing took -- and the
    // cost is a lock per dispatched compile, which is a lock per multi-second process
    // spawn. The atomics were justified by `Check()` running on every compile against a
    // shared cache line; nothing reads this on the hot path any more.
    mutable std::mutex _mutex;
    std::uint64_t _term { 0 };
    bool _learned { false };
};

/// The grants this worker has already spent, so no grant is spent twice.
///
/// **A lease is single-use by construction, and until
/// [#614](https://github.com/LASTRADA-Software/fastcached/issues/614) nothing enforced
/// it.** The scheduler mints one grant per lease and the client presents it exactly
/// once -- `Dispatch::CompileOnWorker` sends one COMPILE frame with no retry, and a
/// RELEASE goes to the scheduler rather than to a worker -- so a token arriving twice at
/// one worker is a replay and never an honest client. Measured before the fix: the same
/// captured token presented a second time was accepted, and would go on being accepted
/// until its expiry.
///
/// **This is what replaces the monotonic term**, and it is strictly finer than what it
/// replaces: the term bounded a captured grant to the leadership era it was minted in,
/// this bounds it to one use.
///
/// ## What it does NOT close, stated rather than discovered
///
/// A worker restart empties this set, so a token captured and withheld before the
/// restart is spendable once afterwards, for whatever is left of its `expiresAt` plus
/// `LeaseTokenClockSkewSlack`. That is not a regression: `KnownSchedulerTerm` starts
/// unlearned after a restart and the old check accepted every term, so the ratchet had
/// the identical hole for the identical duration. It is the residual the expiry is
/// documented to bound, and closing it needs durable state a worker does not have.
///
/// **A wall clock stepping BACKWARDS is the second, and it is not the restart case in
/// disguise.** Retention is re-evaluated only inside `Spend`, against the caller's
/// `now`, so "everything still acceptable is still remembered" holds exactly while the
/// clock is monotone. A worker whose clock is minutes AHEAD -- a resumed VM, a container
/// with no time source, the population `LeaseTokenClockSkewSlack` exists for -- prunes
/// entries whose window merely looks passed; when NTP steps it back, those tokens verify
/// again and are no longer remembered. One extra spend each. Bounded by the same window
/// and by how far the clock moved, and not worth durable state either, but it is a
/// second residual rather than a restatement of the first.
///
/// Two worker PROCESSES advertising one endpoint is a third, for the same reason: each
/// keeps its own set. That one is a deployment mistake rather than a property of this
/// type.
///
/// ## Bound
///
/// One entry per grant, dropped once `Detail::WithinAcceptanceWindow` -- the verifier's
/// own predicate -- says the grant could no longer be accepted at all. That is
/// `expiresAt` plus the clock-skew slack and never `expiresAt` alone, so the set holds
/// what the scheduler issued inside one lease lifetime and nothing older. Pruning is
/// amortized onto `Spend`, which runs once per dispatched compile: there is no timer,
/// because a worker with no traffic has nothing to prune and one with traffic prunes on
/// its own.
///
/// Entries are keyed by a SHA-256 of the token rather than by the token: it is 32 bytes
/// against a few hundred, it is fixed-width, and a credential that has been spent has no
/// business staying resident. The serial alone would not do -- `LeaseTable::_nextToken`
/// restarts at 1 with the scheduler process, so serials repeat across a restart while
/// tokens do not.
///
/// Header-only and beside `KnownSchedulerTerm` for that class's reason: both binaries
/// compile `WorkerProtocol.cpp` and only one links `FastCache`, so a seam with a `.cpp`
/// would need a new `_fc_cc_core` row and this needs none.
class SpentLeases
{
  public:
    /// Spend a grant, if it has not been spent already.
    ///
    /// **Thread-safe, and it has to be**: a worker answers compiles on a pool sized to
    /// its slot cap, so two threads can present one captured token at once. The whole
    /// read-and-insert is one critical section, which is what makes exactly one of them
    /// the spender -- a check followed by a separate insert would let both through.
    ///
    /// @param token The grant, exactly as it was presented.
    /// @param expiresAt When the grant stops being good, from its own claims. What
    ///        bounds how long this entry is kept.
    /// @param now This machine's wall clock.
    /// @param slack How far this clock may trail the minting scheduler's -- the same
    ///        value the verifier allows, through the same predicate. **Retention window
    ///        and acceptance window are ONE window**: `VerifyLeaseToken` accepts a grant
    ///        for `slack` past its own expiry, so an entry dropped at `expiresAt` would
    ///        leave the token acceptable AND unspent for the rest of that window --
    ///        replayable again, five minutes at a time. A spend that expires before the
    ///        thing it is spending is not a spend.
    /// @return True when this call spent it, false when it had already been spent.
    [[nodiscard]] bool Spend(std::string_view token,
                             std::chrono::system_clock::time_point expiresAt,
                             std::chrono::system_clock::time_point now,
                             std::chrono::seconds slack = LeaseTokenClockSkewSlack)
    {
        auto const fingerprint = Sha256::Hash(WireFields::AsBytes(token));

        std::scoped_lock const guard { _mutex };

        // Before the lookup, not after: an entry the verifier would no longer accept
        // describes a token nothing can spend, so keeping it would only grow the map.
        // The SAME predicate the verifier uses, so the two cannot disagree about the
        // window -- and a token reaching here is inside it by construction, so nothing
        // can be pruned by its own arrival.
        std::erase_if(_spent,
                      [now, slack](auto const& entry) { return !Detail::WithinAcceptanceWindow(entry.second, now, slack); });

        return _spent.emplace(fingerprint, expiresAt).second;
    }

    /// @return How many spent grants are currently remembered. For tests and for the
    ///         one question an operator asks about a set like this.
    [[nodiscard]] std::size_t Size() const
    {
        std::scoped_lock const guard { _mutex };
        return _spent.size();
    }

  private:
    /// Hashes a digest that is already a hash.
    ///
    /// The first eight bytes, read as an integer, rather than a second pass of anything:
    /// SHA-256's output is uniform, so any slice of it is as good a bucket index as a
    /// rehash and costs nothing.
    struct DigestHash
    {
        /// @param digest The token's digest.
        /// @return Its hash.
        [[nodiscard]] std::size_t operator()(Sha256::Digest const& digest) const noexcept
        {
            std::size_t value = 0;
            std::memcpy(&value, digest.data(), sizeof(value));
            return value;
        }
    };

    mutable std::mutex _mutex;
    std::unordered_map<Sha256::Digest, std::chrono::system_clock::time_point, DigestHash> _spent;
};

/// Says, once per regression, that this worker has adopted a scheduler term that went
/// BACKWARDS -- **without claiming to know why**.
///
/// Named for what it reports rather than for what it used to refuse: it was
/// `LeaseEpochNotice` when the condition was a refusal on the lease's epoch, and #614
/// deleted that refusal. A name describing what a thing no longer does is how the next
/// reader learns the wrong mechanism.
///
/// **The signal it replaces was not missing, it was WRONG**
/// ([#614](https://github.com/LASTRADA-Software/fastcached/issues/614)). Three ordinary
/// operator actions drop a scheduler's term back to 0 -- wiping the Raft directory,
/// re-bootstrapping the cluster, turning consensus off -- and before #614 every worker
/// that had learned a higher term then refused every grant until its process restarted.
/// On the worker that showed only as `WorkerLeaseStaleEpoch` climbing, which reads like
/// an election storm: an operator whose fleet stopped distributing was pointed at
/// consensus instability when the cause was a reset they performed themselves.
///
/// **What it says changed with what happens.** #768 shipped this as the observability
/// half of a refusal, latched and cleared by an accepted grant, because the downward
/// path was still open and the condition persisted until a restart. The condition is
/// gone: the worker adopts the lower term and goes on compiling. So the line reports a
/// TRANSITION rather than a state, and needs no latch -- a transition happens once by
/// construction, and a scheduler that flaps says it once per flap, which is the right
/// number.
///
/// It is still worth a line rather than only a counter. A term going backwards is either
/// something an operator did on purpose, in which case one line confirms it took effect,
/// or something nobody did, in which case it is the first news of a scheduler that lost
/// its Raft directory.
///
/// Header-only and beside `KnownSchedulerTerm` for that class's reason: both binaries
/// compile `WorkerProtocol.cpp` and only one links `FastCache`, so a seam with a `.cpp`
/// would need a new `_fc_cc_core` row and this needs none. It takes a SINK rather than
/// an `ILogger` for the same reason -- and it is the shape `Cc::CredentialNotice`
/// already uses to say a thing once.
class SchedulerTermRegressionNotice
{
  public:
    /// Where the line goes. Empty means say nothing.
    using Sink = std::function<void(std::string_view)>;

    /// @param sink Where to report; may be empty.
    explicit SchedulerTermRegressionNotice(Sink sink) noexcept:
        _sink { std::move(sink) }
    {
    }

    /// A notice that reports nowhere.
    ///
    /// Named rather than a default argument, for `CredentialNotice::Silent`'s reason: a
    /// defaulted parameter is how a diagnostic comes to be dropped at six call sites. A
    /// caller with nowhere to report has to say so.
    /// @return A notice with no sink.
    [[nodiscard]] static SchedulerTermRegressionNotice Silent()
    {
        return SchedulerTermRegressionNotice { Sink {} };
    }

    /// Report, if this change was a scheduler term going backwards.
    ///
    /// **The return value is the answer, and there is no tally beside it.** This owns
    /// the predicate "is this reportable"; a caller that tested `transition == Reset`
    /// itself and then called this would decide it twice, and a counter kept in here
    /// would be a second tally of what `WorkerSchedulerTermRegressions` already counts. So
    /// the one call site drives its metric off this, and a silent notice answers the
    /// same question a speaking one does.
    /// @param change What `KnownSchedulerTerm::Learn` reported, from the caller that
    ///        performed the transition.
    /// @return True when this change was a reset -- reported, if there is a sink.
    bool Observe(TermChange const& change)
    {
        if (change.transition != TermTransition::Regressed)
            return false;
        if (_sink)
            _sink(std::format(
                "adopted a scheduler term that went BACKWARDS, from {} to {}. TWO things look like this and this "
                "worker cannot tell them apart: a grant minted before a leadership change and delivered after one, "
                "which is ordinary and should appear once without repeating; or a scheduler that was reset -- its "
                "Raft directory wiped, the cluster re-bootstrapped, or consensus turned off -- which will repeat "
                "until somebody stops it. So the count is the diagnosis, not this line. Compiling either way, and a "
                "captured grant cannot be replayed through this, because a grant is spendable exactly once",
                change.previous,
                change.current));
        return true;
    }

  private:
    Sink _sink;
};

/// The fleet a worker was told it serves, learned once at registration.
///
/// **An `optional`, and that is the entire point of the type.** Three states have to
/// stay apart here and a bare string can only hold two: *never registered*, *registered
/// into a fleet that names itself*, and *registered into one that names none*. The last
/// is the one-machine deployment and is legal -- `SchedulerService`'s constructor says
/// so -- while the first must honour no grant at all. A bare string spells the first and
/// the third identically, which is how "harden the fleet check" becomes "every
/// single-machine install stops compiling" (#303's shape, #401's window).
///
/// Mutex-guarded rather than atomic for the reason `KnownSchedulerTerm` is: the value is
/// a string, it is written by the heartbeat thread on registration and read by every
/// compile thread, and it is borrowed for the life of the process.
class PinnedFleet
{
  public:
    /// Adopt the identity the scheduler named in its REGISTER reply.
    ///
    /// Idempotent and last-writer-wins: a worker that re-registers -- which it does
    /// after any refused heartbeat, not only after a restart -- adopts whatever the
    /// scheduler that accepted it says now.
    /// @param clusterId The fleet named, which may legally be empty.
    void Pin(std::string clusterId)
    {
        std::scoped_lock const guard { _mutex };
        _clusterId = std::move(clusterId);
    }

    /// @return The fleet this worker registered with, or nullopt when it has not
    ///         registered. An engaged but EMPTY string is a fleet that names none.
    [[nodiscard]] std::optional<std::string> Pinned() const
    {
        std::scoped_lock const guard { _mutex };
        return _clusterId;
    }

  private:
    mutable std::mutex _mutex;
    std::optional<std::string> _clusterId;
};

/// Everything a worker's lease check keeps between requests.
///
/// **One object rather than three references threaded through two factories.** All
/// three are per-worker mutable state, all three are borrowed for the life of the
/// process, and they are written in one order by one function -- spend, then learn,
/// then report -- so they are one thing. Passed separately they were three chances for
/// a caller to declare two and dangle on the third, which is not hypothetical: the test
/// that drives the production factory had already invented this struct for itself,
/// with that reason written in its comment, while production went on passing them
/// apart.
///
/// The clock and the metrics sink are deliberately NOT members. Those are process-wide
/// collaborators every layer already holds; these three exist only because a worker
/// checks leases.
struct WorkerLeaseState
{
    /// @param reported Where a scheduler term going backwards is reported. Taken as the
    ///        whole notice rather than as a sink, so a caller with nowhere to report
    ///        still has to say `SchedulerTermRegressionNotice::Silent()` by name -- which is
    ///        that type's own rule, and a defaulted sink here would have quietly
    ///        undone it.
    explicit WorkerLeaseState(SchedulerTermRegressionNotice reported) noexcept:
        notice { std::move(reported) }
    {
    }

    SpentLeases spent;                    ///< The grants already run here, so none runs twice.
    KnownSchedulerTerm term;              ///< The term the last authentic grant named.
    SchedulerTermRegressionNotice notice; ///< Where a term going backwards is said.
    PinnedFleet fleet;                    ///< The fleet this worker registered with.
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
};

/// Authenticate a grant and check it names this worker, this toolchain, and now.
///
/// **The signature is checked first, and the order is the whole design.** Every refusal
/// below it is only ever reported for a token that provably came from the
/// scheduler, so a forger learns exactly one thing -- that their forgery failed --
/// while an operator whose worker advertises `build-07:6675` where the scheduler
/// granted `10.0.0.7:6675` is told precisely that. Checking the plaintext endpoint
/// first would be cheaper and would turn a diagnostic into an oracle.
///
/// The cluster is checked FIRST, ahead of the endpoint and the fingerprint, because it
/// answers a different question: those two ask "is this grant for me", it asks "is it
/// from anybody I take orders from" (#322). It still sits below the MAC, so it is not
/// an oracle -- everything it reports is already in the token the caller is holding.
///
/// **`Replayed` is not decided here, and that is deliberate.** Spending a grant is the
/// one step with a side effect, and this function is a pure transform that both a
/// worker and a test call freely -- so the spend lives at the worker's seam
/// (`Cc::SignedLeaseValidator`), which owns the `SpentLeases` and calls this first.
/// Every refusal below is a reading of the token; a grant refused for one of them has
/// not been consumed, so nothing here can burn a lease on its way to declining it.
///
/// @param signers Who may sign a grant, as this worker's roster says.
/// @param token The token the client presented.
/// @param expected What this worker is, and what it knows.
/// @param now This machine's wall clock.
/// @param slack How far this clock may trail the scheduler's.
/// @return The authentic claims, or why the job is refused.
[[nodiscard]] inline std::expected<LeaseClaims, LeaseRefusal> VerifyLeaseToken(
    ILeaseSignerKeys const& signers,
    std::string_view token,
    LeaseExpectation const& expected,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds slack = LeaseTokenClockSkewSlack)
{
    auto authentic = AuthenticateLeaseToken(signers, token);
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

    if (authentic->endpoint != expected.endpoint)
        return std::unexpected { LeaseRefusal {
            .reason = LeaseRefusalReason::EndpointMismatch,
            .detail = std::format(
                "this lease was issued for {}; this worker answers on {}", authentic->endpoint, expected.endpoint) } };

    if (authentic->fingerprint != expected.fingerprint)
        return std::unexpected { LeaseRefusal { .reason = LeaseRefusalReason::FingerprintMismatch, .detail = {} } };

    // Through the SHARED predicate, which owns the overflow reasoning and is the same
    // window `SpentLeases` keeps an entry for. Two spellings of it would let a grant be
    // acceptable here and forgotten there -- replayable again for the difference.
    if (Detail::WithinAcceptanceWindow(authentic->expiresAt, now, slack))
        return std::move(*authentic);

    // Safe because the predicate above only answers false on `now > expiresAt`.
    auto const age = now - authentic->expiresAt;
    return std::unexpected { LeaseRefusal {
        .reason = LeaseRefusalReason::Expired,
        .detail = std::format("this lease expired {} seconds ago, allowing {} seconds of clock skew",
                              std::chrono::duration_cast<std::chrono::seconds>(age).count(),
                              slack.count()) } };
}

} // namespace FastCache::Distributed
