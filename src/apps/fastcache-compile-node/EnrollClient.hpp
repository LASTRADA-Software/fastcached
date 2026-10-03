// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EndpointDialer.hpp"
#include "NodeConfig.hpp"
#include "OneShotAnswer.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <TicketCredentials.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/ISocket.hpp>

namespace FastCache::Node
{

/// @file EnrollClient.hpp
/// The enrollment exchange as both ends of a join read it: what one `Enroll` reply means to the
/// joiner -- a formation controller polling the fleet it asked -- and the operator's verbs that
/// decide who may join.
///
/// **Nothing secret crosses** (#178). The request carries the joiner's PUBLIC key and the
/// approved reply carries the cluster's roster, which is every member's public key -- so a
/// captured exchange admits nobody, and there is nothing to spend. What the exchange cannot
/// defend against on its own is somebody between the two ends swapping one public key for
/// another, which is why an operator compares the joiner's key with the one `--enroll-list`
/// shows before approving it.

/// How long a single enrollment exchange may take to connect.
///
/// Generous for `ClusterAdminCli::DialTimeout`'s reason: the seed answers from memory, so anything
/// slower than this is a network problem rather than a busy leader. One constant for the operator's
/// enrollment verbs and a formation controller's poll.
inline constexpr std::chrono::milliseconds EnrollDialTimeout { 10'000 };

/// How many CONSECUTIVE `NotLeader` redirects an enrollment follows before it goes back to where it
/// started.
///
/// Bounded because two nodes each holding a stale `_knownLeader` can name each other forever -- the
/// same reason the worker's heartbeat bounds its own following. Three is a cluster in the middle of
/// an election, which settles. A chain bound, never a total: a node that answered on its own behalf
/// resets it. One constant for `RunEnrollAdmin` and a formation controller, and the operator verbs'
/// own bound: `RunEnrollAdmin` asks through `AskTheLeader`, so two numbers here would be two
/// answers to one question.
inline constexpr int MaxEnrollRedirects = MaxLeaderRedirects;

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

    /// The answering node's signature over its answer -- an approval, a refusal or a "not yet" alike
    /// (`SignedOutcomeTable`) -- as sent, and DISENGAGED when it carried none. Read here and judged by
    /// the caller, which alone knows the nonce it sent and the key it proved for the fleet it asked
    /// (`Cluster::VerifyAdmission`).
    std::optional<CompileCacheWire::EnrollReplySignature> signature {};
};

/// Which reading an answer's OUTCOME became, for the outcomes the answering node signs.
struct SignedOutcomeRow
{
    EnrollProgress progress;                 ///< The reading.
    CompileCacheWire::EnrollOutcome outcome; ///< The outcome the answer stated, which its signature covers.
};

/// The readings a fleet's own answer produces, each beside the outcome it signed: every one of them
/// can move a joiner -- into the fleet, back to solitary, or into waiting instead of giving up -- so
/// every one is held to the key the joiner proved. A redirect, a closed window and a failure are
/// wire REFUSALS, which nobody signs, so a formation joiner lets none of them move what a forger
/// could want moved: each counts towards giving the join up as silence does, and a redirect's
/// endpoint is proved before it is asked.
inline constexpr std::array SignedOutcomeTable {
    SignedOutcomeRow { .progress = EnrollProgress::Waiting, .outcome = CompileCacheWire::EnrollOutcome::Pending },
    SignedOutcomeRow { .progress = EnrollProgress::Admitted, .outcome = CompileCacheWire::EnrollOutcome::Approved },
    SignedOutcomeRow { .progress = EnrollProgress::Refused, .outcome = CompileCacheWire::EnrollOutcome::Rejected },
};

/// The outcome @p progress was read from, when it is one a fleet signs.
/// @param progress A reading.
/// @return Its signed outcome, or nothing for a reading no answer signs.
[[nodiscard]] constexpr std::optional<CompileCacheWire::EnrollOutcome> SignedOutcomeOf(EnrollProgress progress) noexcept
{
    for (auto const& row: SignedOutcomeTable)
        if (row.progress == progress)
            return row.outcome;
    return std::nullopt;
}

/// Read one `Enroll` reply.
///
/// **Pure, and that is what makes the client's behaviour testable at all**: standing
/// a leader up to see what a joiner does about `EnrollmentFull` is a fleet fixture,
/// while this is a table of replies. The acquisition is left alone; only the decision
/// moves.
/// @param outcome What the exchange returned.
/// @return What this client should do next.
[[nodiscard]] EnrollReading ReadEnrollReply(Cc::CacheOutcome const& outcome);

/// Who this node is asking to be admitted as: everything the request states about it.
struct JoinerIdentity
{
    std::string nodeId;       ///< The id it minted.
    std::string nodeEndpoint; ///< The `0xFC` endpoint it states; empty while no live role states one.
    CompileCacheWire::EnrollRole role { CompileCacheWire::EnrollRole::Learner }; ///< What it asks to be.
    Ed25519PublicKey publicKey {};                                               ///< The key it asks under.
};

/// What to tell the operator once a roster has been handed over, or why it cannot be believed.
///
/// **The roster must record THIS node under THIS key**, and that is asserted rather than
/// assumed: the leader answers `Approved` only once its own roster does, so a roster that
/// lacks this node was not produced by the cluster this node asked -- or an operator approved
/// another machine's key for this id. Either way nothing it names can be trusted, and saying
/// "admitted" over it would be a confident wrong signal.
///
/// Pure, so what an admitted machine is told -- the fingerprint to compare, what approval
/// recorded, and where this machine wrote what it kept -- is pinned by a test rather than
/// reachable only through a dial loop. It names no step this build does not have.
/// @param self Who this node asked to be admitted as.
/// @param roster The roster's bytes, as received.
/// @param stateDirectory Where this node minted the identity it asked under.
/// @return The text to print, or why the roster is refused.
[[nodiscard]] std::expected<std::string, std::string> DescribeAdmission(JoinerIdentity const& self,
                                                                        std::span<std::byte const> roster,
                                                                        std::filesystem::path const& stateDirectory);

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
[[nodiscard]] std::string RenderEnrollmentReport(CompileCacheWire::EnrollmentReport const& report,
                                                 std::string_view scheduler);

/// Run one `--enroll-list`/`--enroll-approve`/`--enroll-reject` and report what the seed said.
///
/// The OPERATOR's half of this pair. It asks `--scheduler`, like every other cluster
/// verb -- the first of them that connects, as `DialFirstReachable` decides, and this
/// machine's own node when it names none (`AdminTargetsOf`) -- follows a `NotLeader`
/// redirect to the leader it names through `AskTheLeader`, the one bounded loop
/// `RunClusterAdmin` follows too, and exits. Safe for `--enroll-approve`: a `NotLeader`
/// is a refusal, so nothing was applied where it landed.
///
/// Each ask presents what @p credentials answers for the endpoint it went to, so the
/// leader a redirect names is shown a ticket naming IT.
/// @param cfg The resolved configuration; `schedulers` is the field read.
/// @param request What to do.
/// @param credentials What each endpoint is shown, asked where it is presented.
/// @param dialer How each endpoint is reached. Defaulted because production never varies
///        it, and a test always does.
/// @return What to print on success, or what to print on failure and where the answer came from.
[[nodiscard]] std::expected<std::string, UnfinishedCommand> RunEnrollAdmin(NodeConfig const& cfg,
                                                                           EnrollCommand const& request,
                                                                           Cc::ICredentialFor& credentials,
                                                                           IEndpointDialer& dialer = DefaultOneShotDialer());

} // namespace FastCache::Node
