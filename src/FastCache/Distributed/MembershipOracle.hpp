// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

/// Answers whether the peer at an address has earned the fleet's capacity.
///
/// A seam rather than a call into `Cluster::PeerDirectory`, for two reasons. The
/// dependency would run the wrong way -- `Distributed` is the policy and `Cluster`
/// is one way of establishing the fact it needs -- and more importantly the answer
/// is *deployment-shaped*: a single-machine install has no cluster and every caller
/// is legitimately a member, while a shared fleet must refuse anyone who has not
/// proved the key. Those are two implementations of one question, which is what an
/// interface is for.
class IMembershipOracle
{
  public:
    virtual ~IMembershipOracle() = default;

    IMembershipOracle() = default;
    IMembershipOracle(IMembershipOracle const&) = default;
    IMembershipOracle& operator=(IMembershipOracle const&) = default;
    IMembershipOracle(IMembershipOracle&&) = default;
    IMembershipOracle& operator=(IMembershipOracle&&) = default;

    /// Classify one caller by where it is, and say which participant decided.
    ///
    /// **The one door for the address routes**, and there are exactly two of them: this machine
    /// (`Loopback`) and `--fleet-open` (`OpenPolicy`). No address admits a machine that is not this
    /// one -- that is what a key is for (`ExplainKey`). `Classify` below is derived from this rather
    /// than declared beside it, so an implementation cannot answer the enforced question and the
    /// reported one differently (#1471).
    /// @param peerAddress The kernel's peer host, as text.
    /// @return The verdict and the route that produced it.
    [[nodiscard]] virtual MembershipDecision Explain(std::string_view peerAddress) const = 0;

    /// Whether that peer may be scheduled onto the fleet, by its address alone.
    ///
    /// **Non-virtual, and that is load-bearing.** Left virtual this is a second door: an
    /// implementation could override it inconsistently with `Explain`, and the answer a
    /// `--node-status` reports would have a separate place to be right from the answer a surface
    /// enforces (#1471).
    /// @param peerAddress The kernel's peer host, as text.
    /// @return `Explain(peerAddress).verdict`.
    [[nodiscard]] Membership Classify(std::string_view peerAddress) const
    {
        return Explain(peerAddress).verdict;
    }

    /// Classify a key one connection showed it speaks for, and say which participant decided
    /// (#178).
    ///
    /// **A second question, not a second door onto the first.** `Explain` answers about an
    /// ADDRESS, which every connection from a host shares; this answers about a KEY one connection
    /// established -- by a session proof or a verified ticket -- which no other connection can
    /// borrow. `ExplainConnection` folds the two per connection on one `PrecedenceOf`.
    ///
    /// Pure virtual, because silence here is not safe in either direction: a composite that
    /// inherited *no opinion* would admit no machine and, worse, honour no revoked key. Every
    /// participant says what it knows -- an address route knows no keys and says so; the roster
    /// knows which keys are live and which are revoked.
    /// @param identity The id the connection named and the key that verified.
    /// @param evidence How the connection established it, which names the admission route.
    /// @return `Member` naming the evidence's route when the key is live for that id, `Forgotten`
    ///         naming `KeyTombstone` when it is revoked, and `Outsider` -- no opinion -- otherwise.
    [[nodiscard]] virtual MembershipDecision ExplainKey(ProvenIdentity const& identity, KeyEvidence evidence) const = 0;

    /// The key the cluster holds LIVE under @p id, whichever key is asking.
    ///
    /// `ExplainKey` answers no opinion both for an id nobody recorded and for an id recorded under
    /// ANOTHER key -- a machine whose `node-key` was replaced while the id it minted survived -- and
    /// the two have different remedies: waiting or admitting for the first, restoring the old key
    /// or forgetting the id for the second. This is what tells them apart. Pure virtual for
    /// `ExplainKey`'s reason: a participant that inherited silence would hide the second case.
    /// @param id The id to look up.
    /// @return The live key recorded under @p id, or nothing when this participant records none.
    [[nodiscard]] virtual std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view id) const = 0;
};

/// Fold one participant's answer into a running decision, on `PrecedenceOf`.
///
/// **REPLACE on a higher precedence, UNION on a tie**, and the tie arm is the load-bearing one: two
/// routes answering `Member` are BOTH right -- a connection can prove a key and present a ticket at
/// once, or sit on this machine under `--fleet-open` -- so keeping the first would report one true
/// route and hide another (#1471).
///
/// An `Outsider` answer carries an empty set (`DecidedBy` refuses to attribute one), so the tie arm
/// unions nothing and a refusal by ABSENCE stays distinguishable from one by row. The evidence a
/// revoked key was shown by folds the same way: two tombstones tie, and both kinds are kept.
/// @param decision The decision so far.
/// @param answer One more participant's answer.
constexpr void FoldMembership(MembershipDecision& decision, MembershipDecision const& answer) noexcept
{
    auto const rank = PrecedenceOf(answer.verdict);
    auto const winning = PrecedenceOf(decision.verdict);
    if (rank > winning)
        decision = answer;
    else if (rank == winning)
    {
        decision.decidedBy.Add(answer.decidedBy);
        decision.revokedBy.Add(answer.revokedBy);
    }
}

/// The tombstone for a revoked key shown by @p evidence: `Forgotten` by `KeyTombstone`, carrying
/// how it was shown.
/// @param evidence How the connection showed the key.
/// @return The decision.
[[nodiscard]] constexpr MembershipDecision TombstoneShownBy(KeyEvidence evidence) noexcept
{
    auto decision = DecidedBy(Membership::Forgotten, MembershipParticipant::KeyTombstone);
    decision.revokedBy.Add(evidence);
    return decision;
}

/// Whether a caller may be TOLD its key was revoked: a `Forgotten` whose revoked key this connection
/// PROVED it holds, by a session proof.
///
/// A revoked key shown only by a ticket is refused exactly as firmly, and counted as the removed
/// machine, but answered with the words a stranger gets: a ticket is bytes anybody may have
/// captured, and `revoked` would tell its holder that a third party was forgotten (#1555). A
/// `Forgotten` that names no evidence at all says nothing either -- the direction a mistake here
/// has to fail in.
/// @param decision What the fold concluded.
/// @return True when the verdict is `Forgotten` and a session proof showed the revoked key.
[[nodiscard]] constexpr bool RevocationIsProven(MembershipDecision const& decision) noexcept
{
    return decision.verdict == Membership::Forgotten && decision.revokedBy.Has(KeyEvidence::SessionProof);
}

/// Everyone is a member.
///
/// The right answer for a worker or scheduler that is not in a cluster at all --
/// one machine, or a fleet whose reachability is its boundary. It is **not** the
/// default anywhere: a node has to be configured into this, because "no policy" and
/// "a policy that admits everybody" must be a decision somebody made rather than
/// what happens when a field is left unset. `Membership::Outsider` being the zero
/// value is the other half of that rule.
class OpenMembership final: public IMembershipOracle
{
  public:
    [[nodiscard]] MembershipDecision Explain(std::string_view /*peerAddress*/) const override
    {
        return DecidedBy(Membership::Member, MembershipParticipant::OpenPolicy);
    }

    /// No opinion: an open policy admits by address, which `Explain` already answered for every
    /// caller. Revoking a key is the roster's to say, and a composite asks it beside this.
    [[nodiscard]] MembershipDecision ExplainKey(ProvenIdentity const& /*identity*/, KeyEvidence /*evidence*/) const override
    {
        return {};
    }
    /// No key: an open policy records none.
    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view /*id*/) const override
    {
        return std::nullopt;
    }
};

/// This machine is a member of its own node's fleet, and nothing else is -- by address.
///
/// A rule rather than a convenience. Anti-leeching exists to stop OTHER machines spending capacity
/// they do not contribute; a process on this host already has this host's CPU, and the
/// `fastcache-cc` a developer runs against their own node is the entire reason the node is there.
/// It is also what makes "off by default" safe: a node that holds no roster admits its own machine
/// and nothing else, so it is useful immediately and closed to the network until somebody says
/// otherwise.
///
/// Through `IsLoopbackHost`, which folds every spelling a kernel reports for a local peer -- both
/// IPv6 forms and the dual-stack mapped one -- and refuses an unnameable (empty) host.
class LoopbackMembership final: public IMembershipOracle
{
  public:
    /// @param peerAddress The kernel's peer host.
    /// @return `Member` by `Loopback` for this machine, and no opinion for anybody else.
    [[nodiscard]] MembershipDecision Explain(std::string_view peerAddress) const override
    {
        return DecidedBy(IsLoopbackHost(peerAddress) ? Membership::Member : Membership::Outsider,
                         MembershipParticipant::Loopback);
    }

    /// No opinion: where a caller is says nothing about which key it holds.
    [[nodiscard]] MembershipDecision ExplainKey(ProvenIdentity const& /*identity*/, KeyEvidence /*evidence*/) const override
    {
        return {};
    }

    /// No key: an address records none.
    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view /*id*/) const override
    {
        return std::nullopt;
    }
};

/// A member of any participant is a member.
///
/// Admission is a **union** of independent routes: this machine, `--fleet-open`, and the key
/// evidence a connection carries. Composed through `IMembershipOracle` rather than by teaching one
/// oracle several questions, because the routes are not alike -- one reads an address, one reads a
/// flag, one reads a roster of keys -- and a composite folds them on one `PrecedenceOf`, so a
/// revoked key outranks every admission route whichever participant admitted the caller.
///
/// Participants are borrowed and must outlive this object. That is safe by construction where it
/// is used: `Node::NodeMembership` owns both the participants and the composite, and is neither
/// copyable nor movable.
class AnyOfMembership final: public IMembershipOracle
{
  public:
    /// @param participants The routes to consult, in the order they are asked.
    ///        Each must outlive this object. An empty list refuses everybody,
    ///        which is the same direction every other default here fails in.
    explicit AnyOfMembership(std::vector<IMembershipOracle const*> participants):
        _participants { std::move(participants) }
    {
    }

    /// @param peerAddress The connecting peer's **host**, as the participants take it.
    /// @return The participants' answers folded on `PrecedenceOf`: forgotten, else member,
    ///         else outsider -- with every route that produced the winning verdict.
    ///
    /// **Not `any_of(... == Member)`**, which this was. That flattened every other answer to
    /// `Outsider`, so a participant's `Forgotten` reached no surface as itself and no counted
    /// refusal could fire (#1309). The initial value is `{Outsider, {}}` and stays so when every
    /// participant answers `Outsider`: nobody decided, a refusal by ABSENCE rather than by row.
    [[nodiscard]] MembershipDecision Explain(std::string_view peerAddress) const override
    {
        auto decision = MembershipDecision {};
        for (auto const* participant: _participants)
            FoldMembership(decision, participant->Explain(peerAddress));
        return decision;
    }

    /// @param identity The key one connection established.
    /// @param evidence How it established it.
    /// @return The participants' answers about it, folded exactly as `Explain` folds theirs --
    ///         so a revoked key, `Forgotten`, outranks whatever else admits the connection.
    [[nodiscard]] MembershipDecision ExplainKey(ProvenIdentity const& identity, KeyEvidence evidence) const override
    {
        auto decision = MembershipDecision {};
        for (auto const* participant: _participants)
            FoldMembership(decision, participant->ExplainKey(identity, evidence));
        return decision;
    }

    /// @param id The id to look up.
    /// @return The first participant's live key for @p id; one roster records keys, so there is
    ///         no second answer to reconcile.
    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view id) const override
    {
        for (auto const* participant: _participants)
            if (auto key = participant->LiveKeyOf(id); key.has_value())
                return key;
        return std::nullopt;
    }

  private:
    std::vector<IMembershipOracle const*> _participants;
};

/// What a revoked key a connection presented decides: `Forgotten`, by `KeyTombstone`, shown by a
/// TICKET, and nothing else.
///
/// **The only operation `RevokedKeyEvidence` has**, which is how a refused ticket's evidence cannot
/// become an admission: no oracle is asked, so a key roster that has not yet heard of the forget
/// cannot answer `Member` for it. Folded on `PrecedenceOf` like every other answer, so it outranks
/// this machine and `--fleet-open` exactly as a revoked PROVEN key does -- and it says it was shown
/// by a ticket, because it is a ticket's evidence by construction, so the caller is refused without
/// being told why (`RevocationIsProven`).
/// @param evidence The revoked key a connection presented.
/// @return `Forgotten`, decided by `KeyTombstone`, shown by `MachineTicket`.
[[nodiscard]] inline MembershipDecision DecisionOf(RevokedKeyEvidence const& evidence) noexcept
{
    (void) evidence;
    return TombstoneShownBy(KeyEvidence::MachineTicket);
}

/// The admission decision for one CONNECTION: the address routes, plus every key the connection
/// established.
///
/// ## Why the keys are asked here and not as an address
///
/// Every route answers `Explain(host)`, and one oracle serves every connection a surface accepts.
/// A key is a fact about ONE connection -- the key a proof or a ticket verified under -- so it is
/// asked separately, as `ExplainKey`, and folded here, where the connection's facts are.
///
/// ## Why it is the SAME fold
///
/// `FoldMembership`, on `PrecedenceOf`, unchanged, which settles both directions a key can move a
/// verdict (#178):
///
/// - a LIVE key admits a machine that is not this one, whatever address it dials from -- by
///   `ProvenIdentity` for a session proof and by `MachineTicket` for a verified ticket, and a
///   connection carrying both is admitted by both;
/// - a REVOKED key is `Forgotten`, which outranks every admission route, this machine and
///   `--fleet-open` included: the machine a cluster forgot is refused wherever it dials from --
///   whether it proved the key or presented a ticket this node refused for it (`revokedMachine`).
///
/// Asked on every call rather than cached at the handshake, so a key revoked while its connection
/// is open refuses the next verb.
///
/// @param oracle The admission routes, composed.
/// @param facts What the connection established: its host, and the proof and ticket it verified,
///        each disengaged when it established none -- the ordinary case, in which the fold returns
///        exactly what the address routes answered.
/// @return The folded decision.
[[nodiscard]] inline MembershipDecision ExplainConnection(IMembershipOracle const& oracle, ConnectionFacts const& facts)
{
    auto decision = oracle.Explain(facts.host);
    if (facts.proven.has_value())
        FoldMembership(decision, oracle.ExplainKey(*facts.proven, KeyEvidence::SessionProof));
    if (facts.authenticatedMachine.has_value())
        FoldMembership(decision, oracle.ExplainKey(*facts.authenticatedMachine, KeyEvidence::MachineTicket));
    if (facts.revokedMachine.has_value())
        FoldMembership(decision, DecisionOf(*facts.revokedMachine));
    return decision;
}

/// Whether @p decision rests on a LIVE proven identity: the one fact the verbs a joining machine
/// sends require (`CompileCacheWire::IdentityRequirement::ProvenNodeOnly`).
///
/// Read off the fold rather than asked again, so the verb gate and the admission answer cannot
/// disagree about one connection: the route is present exactly when the proven key is live and
/// nothing that outranks it -- a revoked key -- decided instead. A ticket never satisfies it.
/// @param decision What `ExplainConnection` concluded.
/// @return True when a live proven identity is among the routes that admitted the connection.
[[nodiscard]] constexpr bool RestsOnProvenIdentity(MembershipDecision const& decision) noexcept
{
    return decision.verdict == Membership::Member && decision.decidedBy.Has(MembershipParticipant::ProvenIdentity);
}

/// Whether @p decision admits by a MACHINE's key -- a proof or a verified ticket -- rather than by
/// loopback or `--fleet-open` alone.
///
/// For a surface that serves other machines by key only, read off the same fold every surface
/// enforces, never a second check: two checks of one question are two places to be right.
/// @param decision What `ExplainConnection` concluded.
/// @return True when a live key, of either evidence, is among the routes that admitted it.
[[nodiscard]] constexpr bool RestsOnMachineKey(MembershipDecision const& decision) noexcept
{
    return decision.verdict == Membership::Member
           && (decision.decidedBy.Has(MembershipParticipant::ProvenIdentity)
               || decision.decidedBy.Has(MembershipParticipant::MachineTicket));
}

/// Whether @p decision admits by a route that IDENTIFIES the caller -- this machine, a live proven
/// key or a verified ticket -- rather than by `--fleet-open` alone: what an operator's control verbs
/// require (`CompileCacheWire::IdentityRequirement::IdentifiedCaller`).
///
/// Read off the same fold that admitted the connection, through the route table's column
/// (`MembershipRoutes`), so which routes count is a row and the gate cannot come to disagree with it.
/// @param decision What `ExplainConnection` concluded.
/// @return True when an identifying route is among the routes that admitted the connection.
[[nodiscard]] constexpr bool RestsOnIdentifiedCaller(MembershipDecision const& decision) noexcept
{
    return decision.verdict == Membership::Member && AnyRouteIdentifies(decision.decidedBy);
}

/// Who is asking, as a scheduler-side verb sees one connection: its admission verdict, its host,
/// and the node id it proved when that identity is live.
///
/// **The one place a connection becomes a `CallerContext`**, so the door a surface asks before the
/// payload and the gate a verb asks after it read one fold, and the identity a joining verb
/// requires is read off the same decision that admitted the connection. A ticket admits the caller
/// and engages no `provenNodeId`: only a session proof does.
/// @param oracle The admission routes, composed.
/// @param facts What the connection established; its host is taken over by the returned context
///        after the fold has read it.
/// @return The context.
[[nodiscard]] inline CallerContext CallerContextOf(IMembershipOracle const& oracle, ConnectionFacts facts)
{
    auto const decision = ExplainConnection(oracle, facts);
    auto provenNodeId =
        RestsOnProvenIdentity(decision) && facts.proven.has_value() ? std::optional { facts.proven->id } : std::nullopt;
    return CallerContext { .membership = decision.verdict,
                           .peerId = std::move(facts.host),
                           .provenNodeId = std::move(provenNodeId),
                           .identified = RestsOnIdentifiedCaller(decision) };
}

/// Which admission route each kind of key evidence names: a table rather than a ternary, so a third
/// kind of evidence is a row, and `RowsInEnumeratorOrder` fails the build if it arrives without one.
struct KeyEvidenceRow
{
    KeyEvidence evidence;              ///< How a connection established a key.
    MembershipParticipant participant; ///< The route a live key established that way admits by.
};

/// One row per `KeyEvidence`, in enumerator order.
inline constexpr EnumTable<KeyEvidence, KeyEvidenceRow> KeyEvidenceRoutes { {
    { .evidence = KeyEvidence::SessionProof, .participant = MembershipParticipant::ProvenIdentity },
    { .evidence = KeyEvidence::MachineTicket, .participant = MembershipParticipant::MachineTicket },
} };

static_assert(RowsInEnumeratorOrder(KeyEvidenceRoutes, &KeyEvidenceRow::evidence),
              "KeyEvidenceRoutes must hold one row per KeyEvidence, in enumerator order");

/// Which identity keys the cluster holds LIVE, and which it has REVOKED -- the one participant with
/// an opinion about a key (#178).
///
/// Published from the applied `ClusterState` wholesale: members of either seat are live under their
/// ids, and every revoked key is revoked whatever id it was revoked under -- the key is the fact, and
/// the id a revocation carries is a label.
///
/// It admits no ADDRESS. Composed into `AnyOfMembership` it answers `Explain` with silence, so
/// adding it cannot widen who is admitted by where they dial from.
class KeyRosterMembership final: public IMembershipOracle
{
  public:
    /// Replace what is live and what is revoked.
    ///
    /// Built outside the lock and swapped in, so a reader never sees half of one roster and half of
    /// the next.
    /// @param live Every id's live key.
    /// @param revoked Every revoked key.
    void Publish(std::map<std::string, Ed25519PublicKey, std::less<>> live, std::vector<Ed25519PublicKey> revoked)
    {
        std::unique_lock const guard { _mutex };
        _live = std::move(live);
        _revoked = std::move(revoked);
    }

    /// No opinion: a key roster knows no addresses.
    [[nodiscard]] MembershipDecision Explain(std::string_view /*peerAddress*/) const override
    {
        return {};
    }

    /// @param identity The key a connection established, under the id it named.
    /// @param evidence How it established it, which names the route a live key admits by.
    /// @return `Forgotten` by `KeyTombstone`, shown by @p evidence, for a revoked key -- asked FIRST, so a key that is both
    ///         revoked and still recorded under some id is the removed machine -- `Member` by the
    ///         evidence's route for the live key of that id, and no opinion otherwise.
    [[nodiscard]] MembershipDecision ExplainKey(ProvenIdentity const& identity, KeyEvidence evidence) const override
    {
        std::shared_lock const guard { _mutex };
        if (std::ranges::contains(_revoked, identity.key))
            return TombstoneShownBy(evidence);
        if (auto const live = _live.find(identity.id); live != _live.end() && live->second == identity.key)
            return DecidedBy(Membership::Member, KeyEvidenceRoutes[static_cast<std::size_t>(evidence)].participant);
        return {};
    }

    /// @param id The id to look up.
    /// @return Its live key, or nothing when no member is recorded under it.
    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view id) const override
    {
        std::shared_lock const guard { _mutex };
        if (auto const live = _live.find(id); live != _live.end())
            return live->second;
        return std::nullopt;
    }

  private:
    /// Guards both sets. Mutable because `ExplainKey` is logically const and must still take it.
    mutable std::shared_mutex _mutex;
    std::map<std::string, Ed25519PublicKey, std::less<>> _live;
    std::vector<Ed25519PublicKey> _revoked;
};

} // namespace FastCache::Distributed
