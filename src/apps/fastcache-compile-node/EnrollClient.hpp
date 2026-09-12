// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// @file EnrollClient.hpp
/// `--enroll-from`: a keyless fresh install asking a seed to let it in.
///
/// **A one-shot mode that opens NO surfaces at all.** It mints this node's identity
/// into `--cluster-dir`, dials the seed, polls until a person decides, writes the
/// cluster key with a protected access list, says what to do next, and exits -- the
/// same idiom `RunClusterAdmin` uses, and for the same reason: no reactor exists on
/// this path, so a blocking dial spends a thread this process owns outright.
///
/// Because it opens nothing there is no widening question and no reload to refuse,
/// and by the time the node starts normally its `clusterKeyFile` is non-empty, so
/// the startup guard that refuses a keyless network-facing node is satisfied forever
/// after. That guard must keep refusing; this mode is what makes satisfying it
/// possible without placing the key by hand on every machine first.
///
/// **The key crosses in cleartext.** The `0xFC` wire has no TLS -- `--tls-*` cover
/// the admin surface only -- and what a captured cluster key buys is admission to the
/// fleet, which is object injection into everybody's build. That is accepted
/// deliberately: one round trip, at a moment an operator chose, to a machine they
/// approved by name, in place of thirty-nine manual secret placements that each have
/// an exposure of their own. Sealing the reply to an ephemeral joiner key is the
/// upgrade and needs a curve this tree does not carry, so it is a follow-up rather
/// than a blocker. An out-of-band passphrase is NOT the answer: it reintroduces the
/// distribution problem being solved.

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
    Waiting,  ///< Recorded and undecided. Poll again.
    Admitted, ///< Approved, and the key is in hand.
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

    /// The cluster key, non-empty exactly for `Admitted`.
    ///
    /// `SecureByteBuffer`, so **this holder's** storage is wiped by the allocator on
    /// release rather than by somebody remembering to -- including the copies THIS value
    /// makes, since the allocator travels with the type.
    ///
    /// It used to claim "every copy this value makes on its way to disk", and that
    /// reached further than the type can. Two plain `std::vector<std::byte>` copies sit
    /// on the journey and are not covered: `EncodeEnrollReply`'s return
    /// (`CompileCacheWire.hpp`, server side) and `CacheOutcome::value`
    /// (`CacheProtocol.hpp:119`, client side). Others are plausible and were NOT read --
    /// the frame copy, the socket buffers, stdio's buffer behind `fwrite` -- and are
    /// named as unverified rather than folded into the count.
    ///
    /// That residue is an ACCEPTED cost, not an open defect: this exchange carries the
    /// key in cleartext by design, which is why the window is closed by default and why
    /// opening it is an operator's deliberate act. A sentence promising more than the
    /// mechanism delivers is what stops the next reader asking.
    ///
    /// **Named `clusterKey` and not `key` so the guard can find it.**
    /// `scripts/check-credential-containers.sh` locates credential holders BY NAME, and
    /// a bare `key` cannot be one of its terms -- this is a cache, where `key` is the
    /// thing being cached in most of the tree. An unfindable holder is the mirror of the
    /// defect that scan exists to prevent: not a term that stopped matching, but a path
    /// that was never added.
    SecureByteBuffer clusterKey;
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

/// Write an approved joiner's key where this node will read it from.
///
/// Creates the parent directory, writes the bytes, and then narrows the FILE's own
/// access list -- `SecureSecretFileForServices`, the same call `--seed-config` makes
/// and for the same reason: on Windows a machine-wide directory grants read broadly
/// and everything created inside inherits it, so a key written without this step is
/// one every standard account can read.
///
/// Refuses to overwrite an existing file. A machine that already holds a cluster key
/// is either already a member or is being pointed at a second cluster, and both are
/// decisions an operator makes by removing the file deliberately.
///
/// That refusal is the CREATE itself (`fopen` with `"wbx"`, i.e. `O_EXCL`) and not a
/// check in front of it, so there is no window in which a file that appeared a moment
/// ago is truncated, and no way for a stat this process cannot perform to be read as
/// "absent". The distinction is load-bearing rather than stylistic: the mode that a
/// pre-check would guard is `"wb"`, which TRUNCATES, so a guard that fails open
/// destroys the key the machine is holding.
/// @param path Where `--cluster-key-file` says the key lives.
/// @param key The bytes the leader handed over.
/// @return Nothing, or why the key could not be stored.
[[nodiscard]] std::expected<void, std::string> StoreClusterKey(std::filesystem::path const& path,
                                                               SecureByteBuffer const& key);

/// Render an enrollment report the way an operator reads it before deciding.
///
/// **Both hosts on every row, and a disagreement MARKED rather than refused.** #242
/// settled that enforcing agreement between a claimed endpoint and an observed one
/// refuses the documented setup -- DNS names, a node dialling itself, NAT, VPN,
/// multi-homing -- and stops only a third host. Here the gate is a person's eyes, and
/// at forty rows nobody notices an unmarked mismatch, so the mark is the whole of what
/// this rendering adds over a list of ids.
///
/// A free function so it is testable without a socket: what an operator is shown before
/// they hand over the fleet's key is worth pinning, and a renderer inside the dial loop
/// would only be reachable through one.
/// @param report What the leader answered.
/// @return The text to print, ending in a newline.
[[nodiscard]] std::string RenderEnrollmentReport(CompileCacheWire::EnrollmentReport const& report);

/// Run one `--enroll-open`/`--enroll-close`/`--enroll-list`/`--enroll-approve`/
/// `--enroll-reject` and report what the seed said.
///
/// The OPERATOR's half of this pair. It asks `--scheduler`, like every other cluster
/// verb, follows a `NotLeader` redirect the way `RunClusterAdmin` does, and exits.
/// @param cfg The resolved configuration; `scheduler` is the field read.
/// @param request What to do.
/// @param credential What to present, read where it is presented.
/// @return What to print on success, or what to print on failure.
[[nodiscard]] std::expected<std::string, std::string> RunEnrollAdmin(NodeConfig const& cfg,
                                                                     EnrollCommand const& request,
                                                                     ICredentialSource const& credential);

/// Run `--enroll-from` to completion.
///
/// @param cfg The resolved configuration; `enrollFrom`, `clusterDir` and
///        `clusterKeyFile` are the fields read.
/// @param credential What to present to the seed, read where it is presented.
/// @param random Where a minted identity's bits come from.
/// @param wait How the poll loop spends the gap between two asks, and how it measures
///        the bound it is spending -- the seam `DrainWithin` takes, for the reason it
///        takes one: a loop that counts its requested sleeps states a bound and
///        enforces some multiple of it.
/// @return What to print on success, or what to print on failure.
[[nodiscard]] std::expected<std::string, std::string> RunEnrollClient(NodeConfig const& cfg,
                                                                      ICredentialSource const& credential,
                                                                      IRandomSource& random,
                                                                      IDrainWait& wait = DefaultDrainWait());

} // namespace FastCache::Node
