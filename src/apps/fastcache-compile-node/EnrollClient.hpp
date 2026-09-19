// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// @file EnrollClient.hpp
/// `--enroll-from`: a fresh install asking a seed to admit it under the key it minted.
///
/// **A one-shot mode that opens NO surfaces at all.** It mints this node's identity and its
/// identity key into its state directory, dials the seed, polls until a person decides,
/// checks the roster it is handed, says what to do next, and exits -- the same idiom
/// `RunClusterAdmin` uses, and for the same reason: no reactor exists on this path, so a
/// blocking dial spends a thread this process owns outright.
///
/// **Nothing secret crosses** (#178). The request carries this node's PUBLIC key and the
/// approved reply carries the cluster's roster, which is every member's public key -- so a
/// captured exchange admits nobody, and there is nothing to spend. What the exchange cannot
/// defend against on its own is somebody between the two ends swapping one public key for
/// another, which is why this mode PRINTS both things an operator compares: this node's key
/// before it asks, against the key `--enroll-list` shows, and the roster's fingerprint once it
/// is admitted, against the fingerprint `--enroll-list` shows beside this node's row.
///
/// An admitted WORKER keeps the cluster's certified roster as its trust root, when the leader
/// has one to hand over: it then checks every grant against a roster its operator compared, and
/// needs no `--voter-key` (#178).

/// How long between two polls of a pending enrollment.
///
/// Seconds rather than milliseconds: the thing being waited for is a PERSON reading a
/// list and typing a command, so a tighter cadence buys nothing and spends the
/// leader's pre-auth surface on one machine that is already on its list.
inline constexpr std::chrono::milliseconds EnrollPollInterval { 2'000 };

/// How long `--enroll-from` waits in total before giving up.
///
/// Ten minutes, which is an operator walking to another terminal rather than an
/// operator going home. A joiner that gives up has changed nothing anywhere -- its
/// row stays on the leader's list until the window closes -- so the bound costs a
/// re-run and never a wrong state.
inline constexpr std::chrono::milliseconds EnrollTotalBound { std::chrono::minutes { 10 } };

/// What one exchange with the seed said, once the wire's vocabulary has been mapped
/// onto what this client does next.
///
/// A private enum: nothing transmits these ordinals. It exists so the DECISION is a
/// pure function over one reply -- the acquisition around it needs a socket and a
/// live leader, and the decision needs neither.
enum class EnrollProgress : std::uint8_t
{
    Waiting,  ///< Not admitted yet. Poll again.
    Admitted, ///< Admitted, and the roster is in hand.
    Refused,  ///< A person said no. Stop; asking again will not help.
    Redirect, ///< This node does not lead. `detail` names where to ask instead.
    Closed,   ///< No window is open, or it is full. Poll again and say why.
    Fatal,    ///< Anything else. Stop and report `detail`.
};

/// One reply, interpreted.
struct EnrollReading
{
    EnrollProgress progress { EnrollProgress::Fatal };

    /// What to tell the operator -- or, for `Redirect`, the endpoint to dial next.
    ///
    /// One field for both because a redirect IS what the operator is told when the
    /// hop budget runs out, and two fields would let those drift into disagreeing
    /// about the same reply.
    std::string detail;

    /// The roster's bytes, exactly as the seed sent them, non-empty exactly for `Admitted`.
    ///
    /// Kept as BYTES rather than decoded here, because the fingerprint an operator compares is
    /// taken over the bytes: a re-encoding of a decoded value would be a second input the two
    /// ends could disagree about. No secret, so a plain vector (#178).
    std::vector<std::byte> roster;

    /// The leader's CERTIFIED roster, exactly as sent: `Cluster::EncodeCertifiedRoster`'s bytes,
    /// or empty -- for every outcome but `Admitted`, and on an admission while the leader holds no
    /// certified roster yet (#178).
    std::vector<std::byte> certificate {};
};

/// Read one `Enroll` reply.
///
/// **Pure, and that is what makes the client's behaviour testable at all**: standing
/// a leader up to see what a joiner does about `EnrollmentFull` is a fleet fixture,
/// while this is a table of replies. The acquisition is left alone; only the decision
/// moves.
/// @param outcome What the exchange returned.
/// @return What this client should do next.
[[nodiscard]] EnrollReading ReadEnrollReply(Cc::CacheOutcome const& outcome);

/// Whether a state directory records that this node has already taken part in a
/// cluster.
///
/// A private enum: nothing transmits or persists these ordinals.
enum class ConsensusHistory : std::uint8_t
{
    /// Nothing has ever been recorded -- term zero, no vote, no log, no snapshot.
    ///
    /// A machine that has never run consensus, and also a machine started with
    /// `--raft-join` that has not been admitted yet: a node whose bootstrap set is
    /// EMPTY never stands for election, so it writes nothing. Those two are one state
    /// deliberately, because both may enrol.
    None,

    /// A term, a vote, a log entry or a snapshot is present.
    Recorded,
};

/// What `--cluster-dir` says about this node's consensus past.
///
/// **This is #1299, and its whole difficulty is WHERE it is asked rather than what it
/// asks.** A node started without `--raft-join` bootstraps a cluster of itself, elects
/// itself, and can never afterwards be admitted to anybody else's -- a one-way mistake
/// a forty-machine rollout offers thirty-nine times, and a silent one: the node comes
/// up, leads a cluster of one, and looks healthy on every surface.
///
/// **It is deliberately not a `StartupPolicyRejection` row**, and the reason is the
/// whole of why the obvious implementation would be worse than the bug. A one-machine
/// deployment with `--listen-raft` and no `--raft-join` bootstraps a cluster of itself
/// ON PURPOSE and correctly, and it is indistinguishable on disk from the trapped node
/// -- same self-election, same records. What separates them is not the state, it is
/// what the operator is trying to do RIGHT NOW, and only the enrol path knows that: a
/// node running `--enroll-from` is by definition asking to join somebody else's
/// cluster. In the startup table this predicate would refuse a legitimate single-node
/// install at every boot.
///
/// **It reports what is OBSERVED and asserts no cause**, for the reason a worker may
/// not call a lower scheduler term a reset: this directory holding a term and a vote is
/// a fact, and *it bootstrapped a cluster of itself* is one of two readings of it. The
/// other is a node that was already admitted to a cluster and is being pointed at
/// another. Both are refused and both have the same remedy, so the caller names both
/// rather than guessing between them.
///
/// Through `FileRaftStorage`, which is the one reader of this format in the tree: a
/// second one here would be a second thing to be wrong about a file whose misreading
/// refuses a machine that could have joined.
/// @param stateDirectory Where consensus keeps its durable state -- `--cluster-dir`.
/// @return What is recorded there, or why it could not be read.
[[nodiscard]] std::expected<ConsensusHistory, std::string> ReadConsensusHistory(std::filesystem::path const& stateDirectory);

/// What this node will claim about itself when it asks to join.
///
/// Derived from the resolved configuration rather than taken as two strings, so the
/// id and the endpoint are by construction the ones this node will run as: a joiner
/// admitted under an identity it does not then use is a member the cluster counts and
/// cannot reach, which is the failure `Cluster::ClusterMember` exists to prevent one
/// layer down.
/// @param cfg The resolved configuration, with its identity already applied.
/// @return The id and the consensus endpoint, or why neither could be derived.
[[nodiscard]] std::expected<std::pair<std::string, std::string>, std::string> EnrollClaim(NodeConfig const& cfg);

/// Who this node is asking to be admitted as: everything the request states about it.
struct JoinerIdentity
{
    std::string nodeId;       ///< The id it minted.
    std::string raftEndpoint; ///< Where its consensus port answers; empty for a worker.
    CompileCacheWire::EnrollRole role { CompileCacheWire::EnrollRole::Member }; ///< What it asks to be.
    Ed25519PublicKey publicKey {};                                              ///< The key it asks under.
};

/// What to tell the operator once a roster has been handed over, or why it cannot be believed.
///
/// **The roster must record THIS node under THIS key**, and that is asserted rather than
/// assumed: the leader answers `Approved` only once its own roster does, so a roster that
/// lacks this node was not produced by the cluster this node asked -- or an operator approved
/// another machine's key for this id. Either way nothing it names can be trusted, and saying
/// "admitted" over it would be a confident wrong signal.
///
/// Pure, so what an admitted machine is told -- the fingerprint to compare, and the
/// `--raft-peer` tokens its next start needs -- is pinned by a test rather than reachable only
/// through a dial loop.
/// @param self Who this node asked to be admitted as.
/// @param roster The roster's bytes, as received.
/// @return The text to print, or why the roster is refused.
[[nodiscard]] std::expected<std::string, std::string> DescribeAdmission(JoinerIdentity const& self,
                                                                        std::span<std::byte const> roster);

/// Keep the leader's certified roster as an admitted worker's trust root, and say what happened.
///
/// **Certified against the roster the operator compared**, never taken on the leader's word: a
/// strict majority of THAT roster's voters must endorse the certificate, unexpired, exactly as a
/// worker certifies any roster it adopts -- the enrollment roster stands in for the `--voter-key`
/// anchors, which is what makes those unnecessary. A certificate that does not certify is not
/// kept, and the worker then roots its trust in `--voter-key` as before.
///
/// Pure but for the store, so what an admitted worker keeps is pinned by a test.
/// @param roster The enrollment roster's bytes, which `DescribeAdmission` has already checked.
/// @param certificate The leader's certified roster, or empty.
/// @param now This machine's wall clock.
/// @param store Where a worker's roster is kept.
/// @return One sentence for the operator, ending in a newline.
[[nodiscard]] std::string KeepEnrolledRoster(std::span<std::byte const> roster,
                                             std::span<std::byte const> certificate,
                                             std::chrono::system_clock::time_point now,
                                             Distributed::IRosterStore& store);

/// Render an enrollment report the way an operator reads it before deciding.
///
/// **Both hosts on every row, and a disagreement MARKED rather than refused.** #242
/// settled that enforcing agreement between a claimed endpoint and an observed one
/// refuses the documented setup -- DNS names, a node dialling itself, NAT, VPN,
/// multi-homing -- and stops only a third host. Here the gate is a person's eyes, and
/// at forty rows nobody notices an unmarked mismatch, so the mark is the whole of what
/// this rendering adds over a list of ids.
///
/// **Each row shows the joiner's key WHOLE**, which is what an operator compares against the
/// key the joiner printed before approving it (#178), and the fingerprint of the roster the
/// leader last handed it, which is what the joiner's own print is compared against afterwards.
///
/// A free function so it is testable without a socket: what an operator is shown before
/// they admit a machine to the fleet is worth pinning, and a renderer inside the dial loop
/// would only be reachable through one.
/// @param report What the leader answered.
/// @return The text to print, ending in a newline.
[[nodiscard]] std::string RenderEnrollmentReport(CompileCacheWire::EnrollmentReport const& report);

/// Run one `--enroll-open`/`--enroll-close`/`--enroll-list`/`--enroll-approve`/
/// `--enroll-reject` and report what the seed said.
///
/// The OPERATOR's half of this pair. It asks `--scheduler`, like every other cluster
/// verb -- the first of them that connects, as `DialFirstReachable` decides -- follows
/// a `NotLeader` redirect the way `RunClusterAdmin` does, and exits.
/// @param cfg The resolved configuration; `schedulers` is the field read.
/// @param request What to do.
/// @param credential What to present, read where it is presented.
/// @param dialer How each endpoint is reached. Defaulted for `RunEnrollClient`'s
///        reason: production never varies it, and a test always does.
/// @return What to print on success, or what to print on failure.
[[nodiscard]] std::expected<std::string, std::string> RunEnrollAdmin(NodeConfig const& cfg,
                                                                     EnrollCommand const& request,
                                                                     ICredentialSource const& credential,
                                                                     IEndpointDialer& dialer = DefaultOneShotDialer());

/// Run `--enroll-from` to completion.
///
/// @param cfg The resolved configuration; `enrollFrom` and the state directory are the fields
///        read.
/// @param credential What to present to the seed, read where it is presented.
/// @param random Where a minted identity's bits come from, the id's and the key's alike.
/// @param wait How the poll loop spends the gap between two asks, and how it measures
///        the bound it is spending -- the seam `DrainWithin` takes, for the reason it
///        takes one: a loop that counts its requested sleeps states a bound and
///        enforces some multiple of it.
/// @param dialer How each poll reaches the seed. Defaulted for the reason
///        `DefaultDrainWait` is: production never varies it, and a test always does.
/// @param wallClock What an admitted worker certifies the leader's roster at.
/// @return What to print on success, or what to print on failure.
[[nodiscard]] std::expected<std::string, std::string> RunEnrollClient(
    NodeConfig const& cfg,
    ICredentialSource const& credential,
    ISecureRandom& random,
    IDrainWait& wait = DefaultDrainWait(),
    IEndpointDialer& dialer = DefaultOneShotDialer(),
    IWallClock const& wallClock = DefaultSystemWallClock());

} // namespace FastCache::Node
