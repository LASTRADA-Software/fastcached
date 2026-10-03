// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file ChallengeCookies.hpp
/// Discovery's challenges, held by NOTHING: each nonce is a MAC over who it was sent to, so a
/// proof carries back everything needed to judge it and no table waits on an answer.
///
/// A table of outstanding challenges is a table a beacon grows, and a beacon is unauthenticated:
/// anything on the segment can name any cluster and any node. Every bound on such a table is spent
/// by displacing somebody, and whoever is displaced is a real peer as soon as the flood is fast
/// enough -- four spoofed ids of another fleet displaced that fleet's challenge before it could
/// answer. So there is no table. The nonce a challenge carries is a COOKIE this node can recompute:
/// a serial, then a MAC under a key this node drew, over the serial, this node's cluster, the
/// cluster, node and endpoint the challenge was sent to, and the HOST it was sent to. The proof
/// echoes it; this node recomputes it from what the proof claims and the host the proof ARRIVED
/// from, and a proof that names anything other than what was challenged, or comes from anywhere
/// but where the challenge went, names a cookie this node never made. Checking that costs one
/// HMAC, BEFORE any signature and before any budget.
///
/// **The host binds a cookie to an address its answerer OWNS.** Without it, a flood collects real
/// cookies at its own address and answers them from as many source addresses as it can type, each
/// a fresh per-source budget -- so the per-source bound was a bound per TYPED address, and forged
/// proofs over real cookies drained the shared check budget before an honest proof arrived (the
/// reviewer's P9). Bound to the destination, a spoofed-source answer fails the HMAC; a forger has
/// to receive the challenge at every address it answers from. The assumption this rests on, and
/// states: **an honest peer answers from the address it was challenged at.** Discovery is
/// LAN-local and answers from the socket it beaconed from, so no NAT stands between the two.
///
/// A cookie never REPEATS, which is the property a nonce needs (`Core/Nonce.hpp`): the serial is
/// unique within this process, and the key is drawn afresh by every process and every epoch.
namespace FastCache::Cluster
{

/// Why a proof did not verify against a challenge this node issued.
///
/// **Private**: never transmitted or persisted.
enum class CookieRefusal : std::uint8_t
{
    NotIssued, ///< Its cookie is none this node made for what the proof names: another cluster, node or
               ///< endpoint than was challenged, a host other than the one it was sent to, a serial
               ///< never issued, or invented bytes.
    Expired,   ///< Made under a key this node has let go of, or past its window; the key that would judge
               ///< the oldest serials is gone, so those are Expired whether or not this node made them.
    Replayed,  ///< A cookie a VERIFIED proof already answered.
    Forged,    ///< A cookie this node made, but the signature does not verify under the key the proof carries.
    Unchecked, ///< A cookie this node made and has not spent, but the check budget said no: nothing was verified
               ///< and nothing spent, so the honest answer, if this is not it, still can be.
    Exhausted, ///< A cookie whose signature checks have already failed `MaxForgeriesPerChallenge` times: refused
               ///< without another, so one live cookie is not a key to unlimited checks.
};

/// Discovery's challenges as cookies: issued from a key drawn per EPOCH, and verified once each.
///
/// **Epochs.** A key is drawn from the injected `ISecureRandom` and used for half a challenge
/// lifetime; then the next challenge draws a new one, and the old becomes the PREVIOUS epoch. The
/// current and the previous are accepted, each until its own start plus a lifetime, so a cookie is
/// answerable for at least half a lifetime and never for more than one. A draw that fails is a
/// REFUSAL (#1527): no challenge is issued, and the epochs already held stay as they were.
///
/// **A state for EVERY issued cookie, so the record is never full.** Serials are issued in order, so
/// an epoch keeps two bits per serial it issued: unanswered, forged once, exhausted, or spent. An
/// epoch issues at most `MaxEpochChallenges` and then a new key is drawn at the next challenge, so
/// its record is a fixed bitmap every cookie it issued has a place in. A record with a capacity
/// below what was issued was a race again: a flood of VERIFIED proofs under a key of the attacker's
/// own, answering cookies it collected, filled it before an honest proof arrived, which was then
/// refused (the reviewer's P8). Filling a bitmap is not possible; forcing the two rotations that
/// would expire an honest cookie inside one round trip takes twice `MaxEpochChallenges` challenges.
///
/// **Spent once VERIFIED.** A cookie that answered a proof whose signature verified is recorded as
/// spent until its epoch is let go, so a recorded proof cannot be replayed inside its window. A
/// proof that did NOT verify does not spend it: it proves nothing about the cookie, and spending on
/// it would let a forger who saw a challenge go out destroy the honest answer at once. It is
/// COUNTED instead, and after `MaxForgeriesPerChallenge` failed checks the cookie is exhausted --
/// so a cookie buys a bounded number of signature checks however many sources name it, and what
/// a forger racing the honest answer buys is that one challenge, re-asked at the next beacon.
///
/// **Neither copyable nor movable**: a copy would carry a second record of what is spent, and a
/// proof spent in one would verify again in the other.
class ChallengeCookies
{
  public:
    /// The label every cookie's MAC is taken under, as its first field. `v2` because the MAC gained
    /// the destination host: a label names ONE construction.
    static constexpr std::string_view CookieLabel = "fastcache-discovery-cookie-v2";

    /// How many bytes of the nonce carry the serial: the rest carry the MAC.
    static constexpr std::size_t SerialBytes = sizeof(std::uint64_t);

    /// How many bytes an epoch key is.
    static constexpr std::size_t CookieKeyBytes = 32;

    /// @param random Where epoch keys come from; must outlive this.
    /// @param lifetime The longest a cookie stays answerable; an epoch is half of it.
    ChallengeCookies(ISecureRandom& random, std::chrono::seconds lifetime) noexcept;

    ChallengeCookies(ChallengeCookies const&) = delete;
    ChallengeCookies& operator=(ChallengeCookies const&) = delete;
    ChallengeCookies(ChallengeCookies&&) = delete;
    ChallengeCookies& operator=(ChallengeCookies&&) = delete;
    ~ChallengeCookies() = default;

    /// Issue a challenge to the node @p target describes, sent to @p destinationHost.
    /// @param now The time of issue, from the caller's clock.
    /// @param challengerCluster This node's cluster as it stands now, which the challenge names and
    ///        the cookie binds.
    /// @param target What the challenged node's beacon said: its cluster, node and endpoint are bound.
    /// @param destinationHost The host the challenge is sent to -- where the beacon came FROM, never
    ///        what it claimed. Bound, so only an answer from that host holds.
    /// @return The challenge to send, or why no epoch key could be drawn -- in which case none is
    ///         issued.
    [[nodiscard]] std::expected<DiscoveryWire::Challenge, SecureRandomError> Issue(
        core::platform::SteadyTimePoint now,
        std::string_view challengerCluster,
        CompileCacheWire::FleetSummary const& target,
        std::string_view destinationHost);

    /// Judge a proof against the challenge it claims to answer, spending that challenge only when
    /// the signature verifies.
    ///
    /// The ONE door to a proven discovery summary: the cookie first (one HMAC), then its epoch and
    /// whether it is spent, then the check budget, then the signature, then the spend.
    ///
    /// The budget is asked HERE, between the cookie and the signature, because nowhere else is
    /// right: before the cookie, a flood of garbage that names no challenge would spend the budget
    /// honest proofs need; after it, nothing bounds the checks a forger buys against one live
    /// cookie, which a forgery does not spend. A required parameter, so no caller can skip it.
    /// @param now The time of arrival, from the caller's clock.
    /// @param challengerCluster This node's cluster as it stands now: a cookie made under another
    ///        is `NotIssued`, so a node that changed cluster refuses the answers to its old challenges.
    /// @param proof What arrived.
    /// @param sourceHost The host the proof arrived FROM: a cookie holds only for the host its
    ///        challenge was sent to, so a proof from any other is `NotIssued`, before the budget.
    /// @param mayCheckSignature Asked once, only for a proof whose cookie holds, unexpired and
    ///        unspent: whether one more signature check may be spent now. Taking a token is its job.
    /// @return The proven summary, or which refusal.
    [[nodiscard]] std::expected<ProvenFleetSummary, CookieRefusal> Verify(core::platform::SteadyTimePoint now,
                                                                          std::string_view challengerCluster,
                                                                          DiscoveryWire::Proof const& proof,
                                                                          std::string_view sourceHost,
                                                                          std::function<bool()> const& mayCheckSignature);

    /// How many verified cookies are held as spent, across the epochs held.
    /// @return The count; never more than twice `MaxEpochChallenges`.
    [[nodiscard]] std::size_t SpentHeld() const noexcept;

  private:
    /// What one issued cookie has been through. Two bits in `Epoch::states`.
    ///
    /// **Private**: never transmitted or persisted. The values below `Spent` ARE the count of failed
    /// checks, which is what lets a forgery advance the state by one.
    enum class CookieState : std::uint8_t
    {
        Unanswered = 0, ///< No proof has been checked against it.
        ForgedOnce = 1, ///< One proof failed its signature check.
        Exhausted = 2,  ///< `MaxForgeriesPerChallenge` proofs failed: no more are checked.
        Spent = 3,      ///< A proof verified: it answers nothing again.
    };

    /// How many bits one cookie's state takes.
    static constexpr std::size_t StateBits = 2;

    /// The bits of one cookie's state, at the bottom of a word.
    static constexpr std::uint64_t StateMask = (std::uint64_t { 1 } << StateBits) - 1;

    /// How many bits one word of an epoch's state record holds.
    static constexpr std::size_t WordBits = 64;

    /// How many words one epoch's state record is: a place for every cookie it may issue.
    static constexpr std::size_t StateWords = MaxEpochChallenges * StateBits / WordBits;

    // The counted states run up to `Exhausted`, so the forgery cap IS that state's value; and every
    // state fits its bits, and the record is whole words.
    static_assert(std::to_underlying(CookieState::Exhausted) == MaxForgeriesPerChallenge,
                  "a cookie is exhausted after exactly MaxForgeriesPerChallenge failed checks");
    static_assert(std::to_underlying(CookieState::Spent) <= StateMask, "every cookie state fits its bits");
    static_assert(MaxEpochChallenges * StateBits % WordBits == 0, "an epoch's record is whole words");

    /// One key's worth of cookies.
    struct Epoch
    {
        SecureByteBuffer cookieKey;                ///< What its cookies are MACed under.
        std::uint64_t firstSerial { 0 };           ///< The first serial issued under it.
        core::platform::SteadyTimePoint startedAt; ///< When it was drawn; its cookies expire a lifetime after.
        std::vector<std::uint64_t> states;         ///< `StateBits` per serial it may issue.
    };

    /// The state of the @p offset-th cookie of @p epoch.
    /// @param epoch The epoch.
    /// @param offset Its serial minus the epoch's first.
    /// @return The state.
    [[nodiscard]] static CookieState StateOf(Epoch const& epoch, std::uint64_t offset) noexcept;

    /// Record @p state for the @p offset-th cookie of @p epoch.
    /// @param epoch The epoch.
    /// @param offset Its serial minus the epoch's first.
    /// @param state What it has been through now.
    static void SetState(Epoch& epoch, std::uint64_t offset, CookieState state) noexcept;

    /// The epoch a challenge issued at @p now is made under: the current one while it is young and
    /// has issued fewer than `MaxEpochChallenges`, else a newly drawn one, the current becoming the
    /// previous.
    /// @param now The time of issue.
    /// @return The epoch, or why no key could be drawn -- in which case the epochs are unchanged.
    [[nodiscard]] std::expected<Epoch*, SecureRandomError> IssuingEpoch(core::platform::SteadyTimePoint now);

    /// The epoch @p serial was issued under, when it is one still held.
    /// @param serial The cookie's serial.
    /// @return The epoch; `NotIssued` for a serial never issued, `Expired` for one older than any held.
    [[nodiscard]] std::expected<Epoch*, CookieRefusal> EpochOf(std::uint64_t serial) noexcept;

    ISecureRandom& _random;
    std::chrono::seconds _lifetime;
    std::uint64_t _nextSerial { 0 };
    std::optional<Epoch> _current;
    std::optional<Epoch> _previous;
};

} // namespace FastCache::Cluster
