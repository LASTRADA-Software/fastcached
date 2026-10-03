// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file TicketVerifier.hpp
/// Whether a machine ticket presented at AUTH speaks for the machine it names, to this node.
///
/// The verification order is the design, and each step's position is load-bearing:
///
/// 1. the bytes decode;
/// 2. this node holds a roster, and it is current -- a lapsed roster is not an unknown machine;
/// 3. the SIGNATURE, the first claim read: the claimed id only selects the key, and nothing else
///    about a ticket whose signature did not verify is ever reported -- not even whether its
///    claims are text, since those are an attacker's to choose;
/// 4. the two claims another machine will read are text, checked before either is compared,
///    logged or rendered -- the roster lookup above is a pure map lookup and does none of those;
/// 5. the audience names this node;
/// 6. the expiry is inside the one acceptance window (`TicketAcceptable`);
/// 7. the ticket has not been spent here -- LAST, so a ticket refused above never occupies room.
namespace FastCache::Distributed
{

/// Why a ticket was refused.
///
/// **PRIVATE: persisted and transmitted nowhere.** Every refusal travels as
/// `ErrorCode::TicketRefused`, with the message its row's `says` column chooses, so the enumerator
/// values mean nothing outside this process and carry no explicit numbering.
enum class TicketRefusal : std::uint8_t
{
    Malformed,      ///< The bytes are not a ticket.
    NotUtf8,        ///< Genuine, and its machine id or audience is not UTF-8 text.
    NoRoster,       ///< This node holds no roster, or holds one past its certification.
    UnknownMachine, ///< The roster admits no machine under that id, and no revoked key signed it.
    Forged,         ///< The machine is admitted, and its key did not sign this.
    Revoked,        ///< Signed by a key the cluster revoked: the forgotten machine itself.
    WrongAudience,  ///< Genuine, and minted for another endpoint.
    Expired,        ///< Genuine, and outside the acceptance window in either direction.
    Replayed,       ///< Genuine, and already spent at this node.
    SpentSetFull,   ///< Genuine, and this node cannot remember one more spend.
    Last,           ///< Enumerator count.
};

/// What a refusal may tell the caller.
///
/// **PRIVATE: persisted and transmitted nowhere**; it chooses a message, and the message is what
/// travels.
///
/// AUTH is reachable from any address, so a refusal's words are told to anyone who can reach the
/// port. A ticket is checked under the ROSTER's key for the id it claims, so the three refusals
/// that depend on what the roster holds under that id -- `forged` (the id has a live key),
/// `unknown-machine` (it has none) and `revoked` (the key that signed it is revoked) -- would
/// answer a stranger who signs a ticket for any id with its OWN key whether that id is admitted
/// here, and anyone holding a captured ticket whether its machine was forgotten. That is the
/// third-party roster fact `explain-admission`'s machine form keeps to members. So the three
/// travel as ONE message, and their counters stay apart for the operator.
///
/// This is the node proof's design reached from the other side (`NodeProofResponder`): a proof is
/// checked under the key the CALLER presented, so a caller who cannot sign learns nothing about
/// which ids and keys the cluster holds; a ticket cannot be checked that way, so its refusal is
/// what says nothing.
///
/// **What stays distinct, and why it is not the same leak.** `malformed` is about bytes the caller
/// sent. `no-roster` and `spent-set-full` are this node's own state, the same for every id.
/// `not-utf8`, `wrong-audience`, `expired` and `replayed` are reached only by a ticket whose
/// signature verified under the claimed id's LIVE key -- by the ticket's own machine, or by
/// somebody holding a ticket it signed. That holder does learn the machine is still admitted,
/// which is the residual this design accepts: it is told about the machine whose signature it
/// already holds, and about no id it merely names.
enum class TicketRefusalSays : std::uint8_t
{
    ItsName,     ///< Its row's name.
    NotAdmitted, ///< `TicketNotAdmittedMessage`, whichever of the roster's answers it was.
};

/// The one message the refusals that depend on the roster's entry for the claimed id travel as.
/// It names no reason, and says where the reason can be read: on the node, by its operator.
inline constexpr std::string_view TicketNotAdmittedMessage =
    "not admitted by this node: the machine this ticket names is not one it serves; this node's operator "
    "can see why on the node itself (--node-status, and its fastcache_node_tickets_refused counters)";

/// One refusal: what it may say on the wire and the counter an operator watches, as ONE row.
struct TicketRefusalRow
{
    TicketRefusal reason;          ///< Which refusal.
    std::string_view name;         ///< Its name: in logs, in tests, and on the wire when `says` allows.
    TicketRefusalSays says;        ///< What `ErrorCode::TicketRefused`'s message is for it.
    IMetricsSink::Counter counter; ///< What rises, always its own.
};

/// Every refusal, in enumerator order.
inline constexpr EnumTable<TicketRefusal, TicketRefusalRow> TicketRefusalTable { {
    { .reason = TicketRefusal::Malformed,
      .name = "malformed",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedMalformed },
    { .reason = TicketRefusal::NotUtf8,
      .name = "not-utf8",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedNotUtf8 },
    { .reason = TicketRefusal::NoRoster,
      .name = "no-roster",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedNoRoster },
    { .reason = TicketRefusal::UnknownMachine,
      .name = "unknown-machine",
      .says = TicketRefusalSays::NotAdmitted,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedUnknownMachine },
    { .reason = TicketRefusal::Forged,
      .name = "forged",
      .says = TicketRefusalSays::NotAdmitted,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedForged },
    { .reason = TicketRefusal::Revoked,
      .name = "revoked",
      .says = TicketRefusalSays::NotAdmitted,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedRevoked },
    { .reason = TicketRefusal::WrongAudience,
      .name = "wrong-audience",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedWrongAudience },
    { .reason = TicketRefusal::Expired,
      .name = "expired",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedExpired },
    { .reason = TicketRefusal::Replayed,
      .name = "replayed",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedReplayed },
    { .reason = TicketRefusal::SpentSetFull,
      .name = "spent-set-full",
      .says = TicketRefusalSays::ItsName,
      .counter = IMetricsSink::Counter::NodeTicketsRefusedSpentSetFull },
} };

static_assert(RowsInEnumeratorOrder(TicketRefusalTable, &TicketRefusalRow::reason),
              "TicketRefusalTable must hold one row per TicketRefusal, in enumerator order");

/// @param row A refusal.
/// @return The message it travels with: its name, or the one not-admitted message.
[[nodiscard]] constexpr std::string_view TicketRefusalMessage(TicketRefusalRow const& row) noexcept
{
    switch (row.says)
    {
        case TicketRefusalSays::ItsName:
            return row.name;
        case TicketRefusalSays::NotAdmitted:
            return TicketNotAdmittedMessage;
    }
    return TicketNotAdmittedMessage; // Unreachable; the one that says less, if it were not.
}

/// The row describing @p reason.
/// @param reason Why the ticket was refused.
/// @return Its row.
[[nodiscard]] constexpr TicketRefusalRow const& DescribeTicketRefusal(TicketRefusal reason) noexcept
{
    return TicketRefusalTable[static_cast<std::size_t>(reason)];
}

/// Why a ticket was refused, and -- for a revoked key, and only for one -- the evidence the refusal
/// carries.
///
/// **A revoked-key refusal cannot be spelled without its evidence.** Every other reason is built
/// from a CONSTANT (`consteval`), which refuses `Revoked` at compile time, and `Revoked` is built
/// only by `RevokedKey`, from the evidence -- so "refused as revoked, carrying nothing" is not a
/// value this type has. That pairing is what lets the endpoint refuse every later verb on the
/// connection as the forgotten machine's (`ConnectionFacts::revokedMachine`).
class TicketRejection
{
  public:
    /// A refusal that carries no evidence.
    /// @param reason Why; a constant, and never `Revoked`, which the build refuses.
    explicit consteval TicketRejection(TicketRefusal reason):
        _reason { reason }
    {
        if (reason == TicketRefusal::Revoked)
            throw std::logic_error { "a revoked-key refusal carries its evidence: TicketRejection::RevokedKey" };
    }

    /// A refusal because the ticket verified under a key this node's roster revoked.
    /// @param evidence The id the ticket named and the revoked key that signed it.
    /// @return The refusal, carrying the evidence.
    [[nodiscard]] static TicketRejection RevokedKey(RevokedKeyEvidence evidence)
    {
        return TicketRejection { std::move(evidence) };
    }

    /// @return Why the ticket was refused.
    [[nodiscard]] TicketRefusal Reason() const noexcept
    {
        return _reason;
    }

    /// @return The revoked key's evidence; engaged exactly when `Reason()` is `Revoked`.
    [[nodiscard]] std::optional<RevokedKeyEvidence> const& Revoked() const noexcept
    {
        return _revoked;
    }

  private:
    explicit TicketRejection(RevokedKeyEvidence evidence):
        _reason { TicketRefusal::Revoked },
        _revoked { std::move(evidence) }
    {
    }

    TicketRefusal _reason;
    std::optional<RevokedKeyEvidence> _revoked {};
};

/// Which endpoints a ticket may name to be spent at this node.
class IAudience
{
  public:
    IAudience() = default;
    IAudience(IAudience const&) = delete;
    IAudience(IAudience&&) = delete;
    IAudience& operator=(IAudience const&) = delete;
    IAudience& operator=(IAudience&&) = delete;
    virtual ~IAudience() = default;

    /// @param audience The endpoint a ticket names, as its presenter dialled it.
    /// @return True when that endpoint is this node.
    [[nodiscard]] virtual bool Matches(std::string_view audience) const = 0;
};

/// What this node answers to: the names a presenter may have dialled it by, and the ports it serves.
struct OwnAudience
{
    std::vector<std::string> names;   ///< This node's names, compared without regard to ASCII case.
    std::vector<std::uint16_t> ports; ///< The ports it serves a ticket on.
};

/// Whether @p audience names ONE machine a ticket could be spent at: a dial endpoint whose host is
/// neither a loopback host, the one loopback NAME, nor a wildcard. Every node is its own loopback
/// and no node is the wildcard, so a ticket naming one would be spendable at ANY node that heard it
/// presented.
///
/// **One predicate for both ends, parse included**: `AudienceMatches` asks it before it compares a
/// name, and a minter asks it before it signs, so a later change to how an audience is parsed --
/// unmapping, trimming -- cannot make the two disagree about which audiences are useless.
/// @param audience The endpoint a ticket names, or a caller asked a ticket for.
/// @return True when it names one machine.
[[nodiscard]] bool AudienceNamesOneMachine(std::string_view audience);

/// The first of a ticket's claims that is not UTF-8 text, by name.
///
/// The claims another machine reads -- the id an admission reports and the fleet renders, and the
/// audience -- must be text. **One check for both ends**: the verifier refuses such a ticket
/// `not-utf8`, and a minter refuses to sign one, which every verifier would refuse anyway.
/// @param claims A ticket's claims.
/// @return The offending field's name, or nullopt when every claim is text.
[[nodiscard]] std::optional<std::string_view> FirstClaimNotText(MachineTicketClaims const& claims);

/// Whether @p audience names this node.
///
/// A name of this node, or an address the locality oracle says is this machine's, at a port it
/// serves. Never a loopback host nor a wildcard, whoever answers: every node is its own loopback
/// and no node is the wildcard, so a ticket for either would be spendable at ANY node that heard
/// it presented.
/// @param audience The endpoint a ticket names.
/// @param own This node's names and ports.
/// @param locality Which addresses are this machine's.
/// @return True when the ticket was minted for this node.
[[nodiscard]] bool AudienceMatches(std::string_view audience, OwnAudience const& own, ILocalityOracle const& locality);

/// Whether a ticket expiring at @p expiresAt may be accepted at @p now: no more than
/// `LeaseTokenClockSkewSlack` past its expiry, and no further ahead than `MachineTicketLifetime`
/// plus that slack -- further than any minter issues.
///
/// **Every addition and subtraction is on @p now**, this machine's own clock. @p expiresAt is
/// the presenter's to choose, anywhere up to `MaxMachineTicketExpirySeconds`, whose value
/// depends on the standard library's clock resolution -- so it is only ever COMPARED. Writing
/// the slack as `expiresAt + slack` overflows a signed tick count at that ceiling, and a
/// `constexpr` evaluation over it fails the build, which is what the test pins.
///
/// **The ONE predicate**: `TicketVerifier::Verify` accepts by it and `SpentTickets` evicts by
/// it. Retention window and acceptance window are one window, so a spend can never be forgotten
/// while the ticket it spent is still acceptable.
/// @param expiresAt When the ticket stops being good, from its own claims.
/// @param now This machine's wall clock.
/// @return True while the ticket is inside the window.
[[nodiscard]] constexpr bool TicketAcceptable(std::chrono::system_clock::time_point expiresAt,
                                              std::chrono::system_clock::time_point now) noexcept
{
    return now - LeaseTokenClockSkewSlack <= expiresAt
           && expiresAt <= now + MachineTicketLifetime + LeaseTokenClockSkewSlack;
}

/// How many spent tickets one node remembers at most.
///
/// 2^18 entries of a 32-byte digest and an instant, about 20 MB with the ordered set's node
/// overhead. With a retention of at most the lifetime plus twice the slack (660 s), that is about
/// 400 accepted tickets a second sustained at one node before `spent-set-full` -- several times
/// what a leader serving a few dozen building PCs issues. A rise of that counter says resize
/// this, and nothing is admitted while it is full.
inline constexpr std::size_t MaxSpentTickets = std::size_t { 1 } << 18;

/// What spending a ticket did. PRIVATE: persisted and transmitted nowhere.
enum class SpendOutcome : std::uint8_t
{
    Spent,        ///< This call spent it.
    AlreadySpent, ///< It had been spent already, and still could be accepted.
    Full,         ///< Not spent: the set holds its capacity of tickets that are all still acceptable.
    WindowPassed, ///< Not spent: this set has already seen that expiry leave the window, so it may have
                  ///< forgotten a spend of this very ticket. The verifier answers `Expired`.
};

/// The tickets this node accepted, never more than a fixed COUNT of them, and never one forgotten
/// while it could be presented again.
///
/// Keyed by the ticket's expiry and the SHA-256 of the bytes its signature covers -- the CLAIMS,
/// nonce included, rather than the envelope, so one set of claims is one spend however its
/// signature happens to be encoded. The digest covers the expiry, so two presentations of one
/// ticket have one key, and ordering by expiry first puts the tickets the window has passed at the
/// FRONT: eviction pops from there rather than walking every entry on every spend.
///
/// **An entry leaves only once the window has passed it** -- behind this clock and outside
/// `TicketAcceptable`, the verifier's own predicate -- and **only from the front.** An entry that
/// is merely too far AHEAD of the clock is kept: that is a clock stepped back, and forgetting the
/// entry would make its ticket replayable the moment the clock stepped forward again. Keeping it
/// costs capacity instead, which fails closed and counts as `spent-set-full`.
///
/// **What leaves is remembered as a bound.** `_forgottenThrough` is the latest expiry ever evicted,
/// and a ticket expiring no later than it is refused (`WindowPassed`): this set has seen that expiry
/// leave the window, so it can no longer tell a fresh ticket from a spend it forgot. Without it, a
/// clock stepped forward and then corrected would find a spent ticket acceptable again and absent.
/// It fails closed, and the refusals clear by themselves within ten minutes, or, if this node
/// accepted tickets while its clock was ahead, once real time reaches that reading; restarting the
/// node clears them at once. An expiry is only ever spent within the lifetime plus the slack of
/// this clock's reading, which is what bounds the first case; a ticket accepted under a clock
/// running ahead carries that clock's future with it, which is the second.
///
/// **Staleness, both directions.** Remembering too much -- the bound above, and entries a clock
/// stepped back keeps -- fails CLOSED: a good ticket is refused as `expired`, or the set fills and
/// refuses as `spent-set-full`, and either heals as real time passes. Remembering too little fails
/// OPEN, and the one way it happens is a RESTART: the set lives in memory only, so a restarted node
/// has forgotten every spend, and a ticket spent in the last lifetime plus twice the slack (about
/// eleven minutes, the longest any ticket stays acceptable) can be presented once more. That is a
/// deliberate bound rather than an oversight: persisting the set would put a disk write on every
/// AUTH, and a replay needs a captured ticket AND a restart inside that window, after which the
/// replayed ticket is spent again like any other.
///
/// **Thread-safe**: one critical section covers evict, look up and insert, so exactly one of two
/// concurrent presenters of one ticket spends it. When full after eviction it REFUSES rather
/// than dropping a live entry, because dropping one makes that ticket replayable again.
class SpentTickets
{
  public:
    /// @param capacity The most tickets remembered at once.
    explicit SpentTickets(std::size_t capacity = MaxSpentTickets) noexcept;

    /// Spend a ticket, if it has not been spent already and there is room.
    /// @param claims The bytes the ticket's signature covers, exactly as they arrived.
    /// @param expiresAt Its OWN expiry, from those claims: part of the key, so another value for
    ///        the same bytes is another entry.
    /// @param now This machine's wall clock.
    /// @return What happened.
    [[nodiscard]] SpendOutcome Spend(std::span<std::byte const> claims,
                                     std::chrono::system_clock::time_point expiresAt,
                                     std::chrono::system_clock::time_point now);

    /// @return How many spent tickets are remembered now.
    [[nodiscard]] std::size_t Size() const;

  private:
    using Entry = std::pair<std::chrono::system_clock::time_point, Sha256::Digest>;

    std::size_t _capacity;
    mutable std::mutex _mutex;
    std::set<Entry> _spent; ///< By expiry, then digest.

    /// The latest expiry ever evicted: nothing expiring at or before it can be told apart from a
    /// spend this set forgot.
    std::chrono::system_clock::time_point _forgottenThrough = std::chrono::system_clock::time_point::min();
};

/// Verifies a machine ticket against the roster this node holds.
class TicketVerifier
{
  public:
    /// @param roster What keys are checked against; null on a node that holds none.
    /// @param audience Which endpoints are this node.
    /// @param spent Where accepted tickets are spent. Per node: a ticket spent at one node is not
    ///        spent at another, and need not be, since its audience names one.
    TicketVerifier(ILeaseRoster const* roster, IAudience const& audience, SpentTickets& spent) noexcept;

    /// Verify @p ticket, and spend it when it is good.
    /// @param ticket The ticket, as AUTH carried it.
    /// @param now This machine's wall clock.
    /// @return The machine it speaks for and the key that signed it, or the refusal -- carrying, for
    ///         a revoked key, the evidence that the presenter is the forgotten machine.
    [[nodiscard]] std::expected<ProvenIdentity, TicketRejection> Verify(std::span<std::byte const> ticket,
                                                                        std::chrono::system_clock::time_point now) const;

  private:
    ILeaseRoster const* _roster;
    IAudience const& _audience;
    SpentTickets& _spent;
};

/// The refusal as the wire carries it.
/// @param metrics Where the row's counter rises -- its own, whatever its message.
/// @param refusal Which refusal.
/// @return `ErrorCode::TicketRefused`, with `TicketRefusalMessage` of its row.
[[nodiscard]] std::vector<std::byte> AnswerTicketRefusal(IMetricsSink& metrics, TicketRefusal refusal);

/// What a verification established, answered and counted.
struct TicketAnswer
{
    std::optional<ProvenIdentity> machine {};     ///< The machine it speaks for; engaged only when accepted.
    std::optional<RevokedKeyEvidence> revoked {}; ///< Engaged only when refused for a revoked key.
    std::vector<std::byte> refusalReply {};       ///< The refusal, already counted; empty when accepted.
};

/// Answer a verification: the machine when it was accepted, the counted refusal when it was not.
///
/// **The one place a verification is counted, whichever way it went**: an accepted ticket moves
/// `NodeTicketsAccepted` exactly once and no refusal row, a refused one moves its own row exactly
/// once and never the accepted counter. Counting beside the verification rather than inside this
/// would be a second place to forget one half.
/// @param metrics Where the outcome is counted.
/// @param verified What `TicketVerifier::Verify` answered.
/// @return The machine, or the refusal reply and -- for a revoked key -- its evidence.
[[nodiscard]] TicketAnswer AnswerTicket(IMetricsSink& metrics, std::expected<ProvenIdentity, TicketRejection> verified);

} // namespace FastCache::Distributed
