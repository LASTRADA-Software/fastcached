// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace FastCache::Cluster
{

/// A member this node believes should be present, and how sure it is of each part.
///
/// Not a `ClusterMember`, and the difference is one field's type. A record here is
/// what somebody *knows*, assembled from a source that may not know all of it:
/// discovery proves a peer's consensus endpoint and learns nothing about the port
/// clients speak to, because nobody dials that port to find out.
///
/// **It carries no seat (#1535).** Which set a member is recorded in is decided against
/// the state on every pass (`MembershipProposals`), never by whoever made the desire. A
/// desire is made once and replayed on every pass for the life of the process, so a seat
/// written into it would undo the operator's next promotion or demotion one interval
/// after it committed -- the defect #1449 closed for this node's own record, reached
/// from discovery's. And the fact a source cannot know is the one that decides: a
/// bootstrap member is counted by the configuration and recorded nowhere in the
/// state, so a source that called every unrecorded peer a newcomer would demote it.
struct DesiredMember
{
    Consensus::NodeId id; ///< Stable identity; what consensus counts.

    /// Where its consensus port answers; empty for a member whose seat is not dialled.
    ///
    /// A learner dials in (`SeatNeedsEndpoint`), so an empty endpoint is its whole record.
    /// Empty for a seat that IS dialled is half a record, which `MembershipProposals` drops.
    std::string raftEndpoint;

    /// Where its `0xFC` port answers, as the record keeps it for every member (`ClusterMember::schedulerEndpoint`);
    /// absent when this node has no opinion.
    ///
    /// **Absent is not empty**, and that distinction is what keeps discovery from
    /// undoing a leader's own announcement. `AddMember` applies wholesale, so a
    /// proposal carrying an empty scheduler endpoint *clears* what was recorded --
    /// correct when the proposer knows the member has none, and destructive when it
    /// simply never knew. A node says `""` about itself and `nullopt` about a peer,
    /// and only the first of those is an assertion.
    std::optional<std::string> schedulerEndpoint;

    /// Its identity key; absent when this node has no opinion (#178).
    ///
    /// A node asserts ITS OWN, which it read out of its state directory, and has no opinion
    /// about a peer's: discovery proves where a peer answers, not which key it holds. Absent
    /// keeps whatever is recorded, which is `AddMember`'s own reading of a command with no
    /// key -- so discovery can never clear what a member announced.
    std::optional<Ed25519PublicKey> publicKey;
};

/// The seat a member is recorded in when nothing has placed it yet: a LEARNER (#1535).
///
/// Everything the reconciler is handed is an OBSERVATION -- a peer proved the cluster
/// key, this node knows its own record -- and an observation does not grant a vote. A
/// proof says a machine holds the key NOW; a vote is a claim that it will go on
/// answering, and a two-voter cluster needs both machines for every commit from the
/// moment the second is counted. Admitted straight as a voter, a discovered laptop on a
/// VPN made the always-on machine beside it unable to commit or re-elect alone, which is
/// #178's failure and the one learners exist to avoid. And the key is SHARED, so a proof
/// names no machine: votes handed out per proof are votes any key holder can multiply.
///
/// **Promotion is the operator's act, never automatic** -- `--cluster-admit` on the
/// learner. Catching up to the leader's commit index was the alternative, and it
/// answers the wrong question: it says the machine can answer now, which is what
/// discovery already knew, and nothing about whether it will stay. That is known only to
/// whoever knows the machine. The record is the operator's as well (`ClusterMember::seat`,
/// written only by the verb), so an automatic promotion would have to tell a learner the
/// operator chose -- the laptop -- from one discovery admitted, and nothing records which.
inline constexpr MemberSeat NewcomerSeat = MemberSeat::Learner;

/// What `MembershipProposals` decided: what to propose, and what it refused to.
struct MembershipPlan
{
    /// The commands to propose, in `desired` order; empty when nothing differs.
    std::vector<Command> proposals;

    /// Desires refused because the cluster FORGOT the id they name and revoked its key
    /// (#1528, #1555).
    ///
    /// Named rather than dropped, because a refusal nothing can see reads exactly like a
    /// desire that already matches: both propose nothing. The caller reports them, once
    /// per member -- the same desire comes back every pass for as long as whatever holds it
    /// runs, so it stays on this list for that long.
    std::vector<DesiredMember> forgotten;
};

/// What a leader should propose to make the cluster's state say what it knows.
///
/// The whole decision, as a pure function over three values: what the replicated state
/// currently holds, what consensus counts, and what this node believes the membership
/// ought to include. It is a function rather than a method for the reason
/// `WorkerRegistry` and `LeaseTable` are pure -- every rule below is a table-driven unit
/// test rather than a cluster and a sleep -- and it is *one* function rather than one per source
/// because this node has more than one: its own record, which only it can supply,
/// and the peers discovery has proved. Two callers each deciding "is this already
/// there?" would be two places for the answer to drift.
///
/// ## What it never proposes
///
/// **A removal.** A member vanishes from what this node can see for reasons that are
/// almost never "it left": a beacon lost on a broadcast, a switch rebooting, a laptop
/// closed for an hour. Removing on absence would take a node out of the quorum the
/// moment the network hiccupped, and a cluster that re-computes its own membership
/// from reachability is one that can shrink itself below a majority and never come
/// back. Raft already tolerates a member that does not answer; leaving is an operator
/// decision, and stays one.
///
/// **A record that already matches.** Proposing one costs a log entry, a replication
/// round and a snapshot's worth of growth per beacon interval, forever. So the
/// comparison is on the *whole* record rather than on the id -- a member whose
/// scheduler endpoint has just been announced differs from the one recorded, and must
/// be re-proposed, while one that agrees in every field must not.
///
/// **A seat something else placed.** A recorded member keeps the seat the operator's
/// verb wrote, so neither this node's own record nor a rediscovered peer promotes a
/// demotion back or demotes a promotion. One recorded nowhere but counted by @p active
/// -- a bootstrap member, which nothing puts in the state -- is recorded in the set
/// consensus already counts it in, so recording a bootstrap voter never demotes it. Only a
/// member neither places is a newcomer, and it joins as `NewcomerSeat`: a learner, which
/// an operator promotes (#1535).
///
/// ## What it refuses to propose
///
/// **A record for an id the cluster has FORGOTTEN (#1528, #1555).** Everything this
/// function is handed is an OBSERVATION -- a peer proved its key, this node knows its own
/// record -- and a forget is an operator's positive act, so an observation must not undo
/// one. It would, and silently: `--cluster-forget` leaves the machine running and a
/// machine always desires its own record, so the next pass would put the member back and
/// the quorum would flap -- removed on one pass, re-added on the next.
///
/// A forget revokes the id's key, and that revocation is what this asks: a desire for an
/// id `revokedKeys` names is refused, WHEREVER it now dials from -- an address is not an
/// identity, so no host enters into it. A forget always leaves one (`PrepareForget`
/// refuses one that would revoke nothing), and a member is never admitted without a key
/// (`ValidateAgainst`), so there is no forgotten member this cannot see.
///
/// Decided HERE, against the state, and not by forgetting the desire: whatever holds the
/// desire hands it back at the next pass, for as long as it runs. Asked only of a desire
/// that would propose something -- one the state already matches changes nothing. It
/// covers this node's OWN record too: a leader that was forgotten stops re-proposing
/// itself, and proposes its own removal instead (`NextQuorumChange`, #1539).
///
/// **Undoing a forget is the operator's, and it is deliberate**: `--cluster-admit`
/// commits `AddMember` directly -- never through this function -- under a NEW key, since
/// the revoked one is never admitted again (`KeyRevoked`). After it the id is recorded
/// again, and a desire for it passes once more -- unless it still carries the revoked key,
/// which is the forgotten machine whatever the record now says.
/// @param state The cluster's state as this node last applied it.
/// @param active The configuration consensus currently holds, both sets: where a member
///        the state does not record is already counted.
/// @param desired Records this node believes should be present.
/// @return What to propose, in `desired` order, and what was refused because the
///         cluster forgot its id.
[[nodiscard]] MembershipPlan MembershipProposals(ClusterState const& state,
                                                 Consensus::Configuration const& active,
                                                 std::span<DesiredMember const> desired);

/// @p desired, where each desire that states no key -- for an id the state records no key
/// for -- carries the key this node holds live for it.
///
/// **Why a leader fills it in**: a member is never admitted without a key (`ValidateAgainst`),
/// and discovery states none -- it proves only the key the roster already holds -- so a bootstrap
/// member the formation record names with its key, recorded nowhere yet, would be proposed
/// keyless and refused at every pass. The key this node holds live for the id IS that named key,
/// so the admission records it.
///
/// **Read per pass, never kept**: the result is not stored in the desires, so a key filled in
/// here cannot outlive an operator's re-admission under another one -- which is the reason
/// discovery states none. A recorded key always wins, because the roster reads the state first;
/// and a key this node holds live only by grace, for a forgotten member the configuration
/// still counts (`RosterKeys`), is a revoked key, which `MembershipProposals` refuses.
/// @param state The cluster's state as this node last applied it.
/// @param desired Records this node believes should be present.
/// @param keys What this node holds live for each id (`Consensus::IRaftPeerKeys::KeysOf`).
/// @return The desires, in order, each keyless one carrying the live key where there is one.
[[nodiscard]] std::vector<DesiredMember> WithLiveKeys(ClusterState const& state,
                                                      std::span<DesiredMember const> desired,
                                                      Consensus::IRaftPeerKeys const& keys);

/// What the leader knows about how far each member has replicated its log (#1537).
///
/// Read together with the configuration, from one `RaftDriver::Progress`, because the
/// two are compared: a member has CAUGHT UP when its match index reaches the commit
/// index, and the two read apart could straddle a commit.
struct Replication
{
    Consensus::LogIndex commitIndex; ///< The leader's commit index.

    /// Per member, the highest index known to match the leader's log. A member absent
    /// from it has acknowledged nothing to this leader -- including one it has never
    /// replicated to -- and has caught up with nothing.
    std::unordered_map<Consensus::NodeId, Consensus::LogIndex> matchIndex;

    /// Whether `id` holds every entry the cluster has committed.
    /// @param id The member.
    /// @return True when its match index reaches the commit index.
    [[nodiscard]] bool CaughtUp(Consensus::NodeId const& id) const;
};

/// What `NextQuorumChange` decided: the one change, and who a promotion waits for.
struct QuorumPlan
{
    /// The configuration to propose, or nullopt when nothing should change now.
    std::optional<Consensus::Configuration> change;

    /// Members recorded as voters that consensus holds as learners until they have
    /// caught up (#1537), in state order.
    ///
    /// Named rather than left for somebody to infer, because from the outside the wait
    /// is indistinguishable from a promotion nobody asked for: `--cluster-status` reads
    /// `seat=voter` and the member's own standing reads `learner`, for as long as it
    /// is away. The caller reports it.
    std::vector<Consensus::NodeId> catchingUp;
};

/// The one consensus membership change that moves the quorum towards the state.
///
/// The other half of `MembershipProposals`, and the two are deliberately separate
/// questions. That one decides who the cluster has agreed *exists*; this one
/// decides who it *counts*, which is a Raft configuration change with its own
/// safety rules. Until this existed the second answer never moved: a node
/// admitted at runtime was served by every surface and voted in none, so growing a
/// cluster's consensus meant restarting its members with a longer bootstrap list.
///
/// ## One change, and which one
///
/// A configuration change may add, remove, promote **or** demote exactly one member
/// (§4.3, and `Consensus::Membership::Classify`), so this returns one step and is
/// called again when it commits. Additions come first -- into whichever set the
/// member's record names -- then promotions, then demotions, then removals: growing
/// the voter set before shrinking it keeps the quorum reachable while a replacement is
/// in progress, where the other order passes through a configuration that is smaller
/// than either endpoint.
///
/// ## Learners (#1449)
///
/// A learner is proposed, moved and removed on the same terms as a voter, with one
/// difference that follows from what it is: adding one changes no quorum, so it is
/// always safe -- and it still waits for a dialable address, since a member nothing
/// can replicate to never catches up. A demotion or removal that would leave no voter
/// is never proposed: a configuration nobody is counted in can commit nothing,
/// including the change that would undo it.
///
/// ## A voter is counted only once it has caught up (#1537)
///
/// **Every member enters the configuration as a learner**, whatever its record names,
/// and one recorded as a voter is PROMOTED -- a second change -- once it is dialable
/// AND its match index has reached the commit index (`Replication::CaughtUp`). The
/// record still says voter throughout; only consensus is staged, which is Raft's own
/// answer (§4.2.1: catch a new server up before it is counted). Counted before it has
/// caught up, a voter that is away makes every commit wait for it: in a one-voter
/// cluster the promotion itself needs both machines, so nothing after it commits until
/// it returns -- reproduced, before this rule, by promoting an absent learner.
///
/// It covers an operator's promotion of a learner and an admission of a fresh voter
/// alike, since both are the member being COUNTED. It is not a decision about WHETHER
/// to promote -- that is the operator's (#1535) -- only about WHEN the promotion may
/// take effect. A member waiting for it is named in `QuorumPlan::catchingUp`.
///
/// **A learner is never removed for being absent**, which is the property bootstrap
/// members already have below, and for the same reason: nothing here asks whether a
/// member ANSWERS. Only a member the operator forgot -- gone from the state, admitted
/// at runtime -- is removed, whichever set it is in.
///
/// ## What it refuses to propose
///
/// **A member with no dialable address.** Counting a node the transport cannot
/// reach is precisely the failure this whole change exists to avoid, reached from
/// the other side: the cluster's quorum grows and the votes to satisfy it cannot
/// arrive. A member already *counted* is never dropped for this, though — an
/// endpoint that has become unreadable is a bad record, and shrinking the quorum
/// over one would turn a typo into a cluster that cannot elect. A dialable address
/// is necessary and not sufficient: the votes have to be able to arrive NOW, which is
/// the catch-up rule above.
///
/// **This node's own removal -- until the operator forgets it (#1539).** A leader
/// taking itself out of the quorum it leads is an operator's decision, and absent a
/// forget it would not stick anyway: a node always desires its own record, so the
/// next pass would propose putting it back, and a configuration flapping on a timer
/// is worse than one that is merely wrong.
///
/// A forget IS that decision, and it means the same thing whoever currently leads. A
/// leader whose own record is gone AND whose key the cluster has revoked -- what
/// `Forget` writes, and the one reading of *forgotten*
/// that the first pass of a fresh leader, which has recorded nothing yet, cannot
/// produce -- stops re-proposing its record (`MembershipProposals`, #1528) and proposes its
/// own removal, LAST: after every other change it can still make as the leader.
/// Once that commits it steps down (`RaftNode`, §4.2.2 -- #1449's demoted leader is
/// the same rule), and the remaining voters elect a leader that never admits it
/// again. Never the last voter: a configuration nobody is counted in can commit
/// nothing, and forgetting the only voter is refused BY NAME before anything is
/// proposed (`PrepareForget`), so the record never runs ahead of a quorum that
/// could not follow it.
///
/// Its own bootstrap entry does not protect it, for a reason that holds only for
/// this one entry: a node cannot start without naming itself, so that entry records
/// how it was started, not an operator's assertion about who belongs. Every OTHER
/// member this node's command line names is still such an assertion -- until the
/// cluster forgets it, which outranks it (below).
///
/// **A forgotten member leaves the quorum, whoever typed it (#1555).** A forget revokes
/// the key the member was admitted under, and on the consensus wire that takes effect when
/// the configuration stops counting the member: until then it keeps its key for itself
/// (`RosterKeys`), because cutting a counted voter off could leave a quorum nobody can
/// reach. So a forgotten member this policy KEPT would go on voting for as long as it runs
/// -- the forget failing open on the wire it most concerns. Whoever leads therefore
/// removes a counted member that is recorded nowhere and that `revokedKeys` names, with a
/// bootstrap set or without one. That is not absence read as removal: the
/// revocation is the forget's own record, which nothing else writes and a fresh leader's
/// empty state cannot contain. A forget that would leave no revocation is refused before
/// it is proposed (`PrepareForget`).
///
/// **The removal of a bootstrap member.** This is the one that is not obvious, and
/// getting it wrong shrinks a healthy cluster to one node: the bootstrap set puts a
/// member in the *configuration* and nothing puts it in the *state*, so on a cluster
/// whose members were bootstrapped rather than admitted, `state.members` holds the leader's
/// own record and nothing else. Read as "everybody else was forgotten", that
/// proposes removing every peer, one per commit, until the leader is alone and
/// refuses the others as strangers — which is what it did, exactly once, before
/// @p bootstrap existed.
///
/// So absence means removal only for a member that was **admitted at runtime**,
/// which is what tells "the operator forgot it" apart from "nobody ever wrote it
/// down". A bootstrap member is a member by the record it was started from, and
/// taking it out of the quorum is an operator's decision -- a forget, never absence.
///
/// The bootstrap set rather than a record of what this process has observed, and
/// the difference is a restart. An observation is rebuilt from live members only, so
/// a fleet restarted after a removal — a rolling restart, or one rebuilt from its
/// command lines — would count a forgotten member forever, with a correct-looking
/// member set and nothing logged. The bootstrap set comes off the command line and
/// says the same thing after every start.
///
/// **A node given no bootstrap set removes no member merely for being absent**, which
/// is the same rule read at its limit rather than an exception to it -- a FORGOTTEN one
/// it removes, above. A node that joined a fleet was
/// told nothing about the cluster's shape, so every member is equally unexplained to
/// it — and once such a node is elected it would otherwise remove all of them, one
/// per commit, which is the identical failure the parameter exists to prevent
/// reached through the one path that has nothing to compare against. What it costs
/// is that a fleet whose only bootstrapped node is gone can no longer shrink its
/// quorum until one leads again; it fails closed, counting a member too many rather
/// than too few.
/// @param state The cluster's state as this node last applied it.
/// @param active The configuration consensus currently holds, both sets.
/// @param self This node's own record as it announces it; its id is read, to ask whether
///        it was forgotten.
/// @param bootstrap The ids this node was started with, in either set; never removed
///        for being absent, and removed like any other member once forgotten.
/// @param replication What this leader knows of each member's log: who has caught up.
/// @return The change to propose, if any, and the members a promotion waits for.
[[nodiscard]] QuorumPlan NextQuorumChange(ClusterState const& state,
                                          Consensus::Configuration const& active,
                                          ClusterMember const& self,
                                          std::span<Consensus::NodeId const> bootstrap,
                                          Replication const& replication);

/// The `Forget` a leader proposes for `id`, or why it may not propose one (#1539, #1555).
///
/// Asked before a `Forget` is proposed, because afterwards is too late, and it answers
/// two questions only the proposing node can:
///
/// **Whether forgetting `id` leaves the quorum something it can follow.** The command
/// applies whatever it names, and the only voter's removal is one `NextQuorumChange`
/// can never propose -- a configuration with no voter commits nothing, including the
/// change that would undo it. So the record would say *forgotten* while consensus went
/// on counting the member, and its key would be refused by every surface while it led.
/// Refused by NAME instead, while the operator who typed `--cluster-forget` is
/// reading the answer. Against the configuration consensus holds rather than the state:
/// which members are COUNTED is the question, and a bootstrap voter is counted
/// while recorded nowhere.
///
/// **Which key the forget revokes beyond its record's.** `Apply` revokes whatever key the
/// record holds, and a member the state records without one -- or not at all, which is
/// every bootstrap member -- would keep a key its bootstrap roster states live on
/// every node started from it: the forgotten machine goes on proving itself there, which is
/// removal failing OPEN. The proposer states the key it holds live for the id, and the
/// replicated revocation then outranks every command line that types it
/// (`RosterKeys::KeysOf`).
///
/// **Whether the forget revokes anything at all.** A machine is forgotten by its key, so a
/// forget of an id the state records no member key for -- and
/// this node holds none live for would remove a record and nothing else, and the next
/// observation of the machine would admit it again. Refused by NAME instead.
/// @param state The replicated state as this node last applied it: what records a key for
///        @p id.
/// @param active The configuration consensus currently holds.
/// @param id The machine to be forgotten.
/// @param liveKey The key this node holds live for @p id (`Consensus::IRaftPeerKeys::KeysOf`),
///        or nullopt when it holds none.
/// @return The command to propose; `InvalidConfiguration`, naming the member, when it is
///         the only voter or when the forget would revoke nothing.
[[nodiscard]] std::expected<Command, ConsensusError> PrepareForget(ClusterState const& state,
                                                                   Consensus::Configuration const& active,
                                                                   Consensus::NodeId const& id,
                                                                   std::optional<Ed25519PublicKey> const& liveKey);

} // namespace FastCache::Cluster
