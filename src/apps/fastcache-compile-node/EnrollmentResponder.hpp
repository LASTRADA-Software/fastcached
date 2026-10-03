// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollmentWindow.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

/// @file EnrollmentResponder.hpp
/// The leader's enrollment list's front door.
///
/// ## Why this is a component of its own
///
/// `Op::Enroll` is `OpenBeforeAuth` and must admit a machine that is not a member --
/// a machine this cluster has never heard of is the entire population it serves.
/// The surface that could have hosted this pair answers *may this peer be here* through
/// the one membership check the scheduler's gate uses. So folding `Enroll` in would
/// either put the verb behind the answer that refuses it, or relax that answer for the
/// verbs beside it.
///
/// Separated, the change is purely ADDITIVE and that is checkable rather than argued:
/// no existing responder's `RefusePeer` or gate is touched, and the one door held open
/// is the one verb whose row in `OpTable` says so.
///
/// ## What it does not decide
///
/// Nothing durable, and nothing about the cluster. Approving a joiner goes through
/// `SchedulerService::ClusterAdmit` -- the same entry point `--cluster-admit` reaches,
/// with the same leadership-and-membership gate, the same `Cluster::Validate`, and the
/// same mapping from a consensus refusal onto a wire code. A copy of that mapping here
/// would be a second table to be wrong in, and the refusals an operator reads would
/// then depend on which door they came through.
///
/// What is here and nowhere else is WHICH refusal answers which condition on the
/// joiner's side, and which counter moves.
///
/// ## It is built only on a node that runs consensus
///
/// A node with no cluster has nothing to enrol anybody into, so `main` leaves the
/// component null and `MergedResponder` answers the whole family
/// `UnimplementedVerb` -- *this node serves no component for that verb family*. That
/// is the same shape every other optional component takes here, and it is better than
/// an enrollment surface that accepts requests and answers `NoCluster` to every
/// decision: a list that can never admit anybody should not record requests.

/// Serves `Enroll` and `EnrollControl`.
class EnrollmentResponder final: public IFrameResponder
{
  public:
    /// @param window The window this surface reports on and mutates; must outlive this.
    /// @param scheduler Whose leadership decides whether this node may answer, and
    ///        whose `ClusterAdmit` records an approval; must outlive this.
    /// @param membership Who may reach `EnrollControl`; must outlive this.
    /// @param self What this node says about itself: the cluster id an admission is signed for is
    ///        the one its FLEET-SUMMARY answer states, so the key a joiner proved and the cluster it
    ///        proved it for are one reading. Must outlive this.
    /// @param identity This node's identity key, which signs every admission; must outlive this.
    /// @param metrics Where refusals and admissions served are recorded; must outlive this.
    /// @param logger Where a reject of an already-admitted machine is said out loud.
    ///
    /// **It wires the window to leadership**, here rather than in `main`, so a test that
    /// builds this surface drives the wiring that ships: every role the scheduler is told
    /// reaches `EnrollmentWindow::OnRoleChanged`, which ends an armed auto-approve deadline at
    /// demotion. The observer captures the WINDOW, which must outlive the scheduler's last
    /// `SetRole` -- consensus, which calls it, is torn down first.
    EnrollmentResponder(EnrollmentWindow& window,
                        Distributed::SchedulerService& scheduler,
                        Distributed::IMembershipOracle const& membership,
                        Cluster::IFleetSummarySource const& self,
                        Ed25519KeyPair const& identity,
                        IMetricsSink& metrics,
                        ILogger& logger):
        _window { window },
        _scheduler { scheduler },
        _membership { membership },
        _self { self },
        _identity { identity },
        _metrics { metrics },
        _logger { logger }
    {
        _scheduler.ObserveRole([&window, &scheduler](Distributed::SchedulerRole role) {
            window.OnRoleChanged(role, scheduler.LeaderEndpoint());
        });
        // And the role it holds NOW, which a scheduler told before this surface was built never tells
        // again: the rows would otherwise wait for the next election to stop reading not-evaluated.
        window.OnRoleChanged(scheduler.Role(), scheduler.LeaderEndpoint());
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends: every decision is taken from this node's own memory -- the window, and
    /// the replicated state it hands the roster from.
    [[nodiscard]] core::async::Task<FrameReply> Answer(std::span<std::byte const> frame, PeerIdentity peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **`Enroll` is admitted whoever asks, and that is the one hole this surface
    /// opens.** The machine asking is on no list and holds no key this cluster knows --
    /// it is a fresh install -- so a membership test here would refuse exactly the
    /// population the verb exists for. What stands in place of the credential is a
    /// person: a request is recorded, bounded and forgotten when its machine stops asking,
    /// and admits nobody at all until an operator approves a named id under the key it
    /// listed.
    ///
    /// `EnrollControl` is refused to a non-member, before a payload is read, and
    /// counted. That refusal is the one carrying the security argument for the pair: a
    /// peer reaching it has asked to join and gone on to ask for the decision as well.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(PeerIdentity const& peer,
                                                                   std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// `NoPolicy`: AUTH is the Session family's; this surface is never routed one.
    [[nodiscard]] CredentialVerdict CheckCredential(std::span<std::byte const> /*payload*/) const override
    {
        return NotTheSessionSurface();
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The endpoint's own header window. Both verbs are answered from memory, so what
    /// this bounds is a peer sending a payload it already has in hand -- and this is the
    /// one surface on this node an unauthenticated peer can reach, so the short answer
    /// is the only defensible one.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// The control cap, which is what the larger of the two verbs declares.
    ///
    /// **`Op::Enroll`'s own `BoundedTo(MaxEnrollPayload)` is what bounds the pre-auth
    /// verb**, because `DecidePrePayload` takes the verb's cap when the verb declares
    /// one. So a stranger naming the pre-auth opcode cannot reach the number below, and
    /// reporting the tighter one here instead would refuse a legitimate `EnrollControl`
    /// the moment its payload grew. That is the load-bearing half and it holds
    /// regardless of anything on this page: the 1024-byte cap is enforced per VERB.
    ///
    /// It called this "the SURFACE's ceiling". It is not one:
    /// `MergedResponder::Largest` folds every owner but this one, so on the merged
    /// listener this number is not consulted. Saying *surface* of a value the surface
    /// never reads is the kind of claim that makes the pre-auth bound look like it rests
    /// here, when it rests on the verb's own row.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload;
    }

    /// A handful: one operator and however many machines are enrolling at once.
    ///
    /// **This number protects nothing, and must not be read as a narrowing.**
    /// `MergedResponder::Largest` folds every owner but this one, so on the merged
    /// listener nothing reads it. And `Largest` is a MAXIMUM: folded in, a small value
    /// here would still change nothing, while a larger one would widen the connection cap
    /// of the cache and compile surfaces too.
    /// Two edits that each read as reasonable compose into exactly that -- folding this
    /// member in (looks like the obvious repair, changes nothing) and then raising the
    /// number (looks like tuning, widens three surfaces).
    ///
    /// **A per-component narrowing is not expressible at this seam at all.**
    /// `RequestTimeout` and `HoldsOwnByteBudget` route to the owner because they are
    /// properties of the VERB. Connections and in-flight bytes are properties of one
    /// listener, one accept queue and one byte budget, so they cannot be per-component:
    /// the surface folds them, a maximum over the owners a node may or may not run and a
    /// sum only over the operator families every node runs side by side. The number below is this
    /// responder's own honest answer and nothing more; it is a pure virtual, so it
    /// cannot simply be dropped.
    ///
    /// **What actually bounds an unauthenticated peer's exposure here is the list's own
    /// bound** -- `MaxPendingEnrollments` rows, refused past it, held in memory, forgotten
    /// when unpolled and on restart -- and that nothing is admitted without a person, plus
    /// `Op::Enroll`'s own per-verb payload cap. No number in this class bounds it.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return 2 * MaxPendingEnrollments;
    }

    /// The connection cap times the request cap, so the byte budget never refuses
    /// anything the connection cap would have allowed. Derived from the two rows above
    /// and unread for the same reason they are: `MergedResponder::Largest` does not fold
    /// this responder.
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return 2ULL * MaxPendingEnrollments * CompileCacheWire::MaxControlPayload;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// No: both verbs are answered from memory in microseconds, so the endpoint's
    /// reservation is released almost as soon as it is taken and a second accounting
    /// would add nothing.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// None. Both verbs are answered from memory, so a peer cannot realistically vanish
    /// inside one, and the write that would discover it is the next statement anyway.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// None, and that is what makes `Enroll` a POLL rather than a park: a verb whose
    /// reply waited on a person would need a pulse, `Status::Progress` is asserted to be
    /// `Op::Compile`'s alone, and parking an unauthenticated peer inside this surface for
    /// as long as an operator takes to read a list is exactly what polling avoids.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    ///
    /// **Not a stream**: an enrollment verb is one request and one reply; a joiner polls for its decision.
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t /*opRaw*/) noexcept override
    {
        return nullptr;
    }

    /// @copydoc IFrameResponder::NodeProver
    ///
    /// **None, and the pairing is the opposite way round from what it looks like.** This surface
    /// exists for a machine the cluster has never admitted, so there is nothing it could prove;
    /// the prover exists for a machine the cluster already admitted. Two components, two
    /// populations.
    [[nodiscard]] INodeProver* NodeProver() noexcept override
    {
        return nullptr;
    }

  private:
    /// Admit @p entry as its role says, on @p caller's authority -- the one admission a manual
    /// approval and an armed deadline share, so the two cannot come to record a joiner
    /// differently.
    /// @param entry The row, holding the key the joiner asked with first.
    /// @param caller Who is admitting: the operator's context, or `Distributed::SelfCaller()`.
    /// @return The scheduler's reply.
    [[nodiscard]] Distributed::SchedulerReply AdmitRow(CompileCacheWire::EnrollmentPendingEntry const& entry,
                                                       Distributed::CallerContext const& caller);

    /// The seat the cluster records @p subject in, by its one spelling, or nothing when it seats
    /// no member by that id.
    /// @param subject The joiner's id.
    /// @return The seat's name from `Cluster::MemberSeatTable`, or nothing.
    [[nodiscard]] std::optional<std::string_view> RecordedSeatOf(std::string_view subject) const;

    /// Answer one `Enroll`.
    /// @param payload The request payload.
    /// @param peer The host the kernel reports.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerEnroll(std::span<std::byte const> payload, std::string_view peer);

    /// Refuse a verb whose identity column this ADMITTED caller does not meet, or nullopt
    /// (`IdentityRequirements`). A caller the surface does not admit is membership's refusal.
    ///
    /// `EnrollControl` is an operator's control verb: `--fleet-open` admits a caller to the
    /// window's open door (`Enroll`) and never to the decision behind it, or an anonymous caller on
    /// an open node approves itself. Asked at the door, after membership, and `AnswerControl` asks
    /// the door's whole question again, for `SchedulerProtocol`'s reason: a caller of `Answer` need
    /// not have asked `RefusePeer` first.
    /// @param peer The caller.
    /// @param opRaw The verb.
    /// @return The refusal, counted, or nullopt when the caller meets the verb's requirement.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefuseUnidentified(PeerIdentity const& peer,
                                                                           std::uint8_t opRaw) const;

    /// Answer one `EnrollControl`.
    /// @param payload The request payload.
    /// @param peer The host the kernel reports.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerControl(std::span<std::byte const> payload, PeerIdentity const& peer);

    /// Admit a joiner the armed auto-approve deadline answered for, and record that it did.
    /// @param nodeId The joiner's id.
    /// @param pending The signed `Pending` answer to this request.
    /// @return @p pending, admitted or not: the joiner's next poll is answered from the roster.
    [[nodiscard]] std::vector<std::byte> AnswerAutoApprove(std::string_view nodeId, std::vector<std::byte> const& pending);

    /// Drop every request nobody decided about, count and log what went, and answer the report.
    /// @param peer Who asked, named in the log line.
    /// @return The report as it stands afterwards.
    [[nodiscard]] std::vector<std::byte> AnswerClear(PeerIdentity const& peer);

    /// Apply one operator decision to one waiting id.
    /// @param verb `Approve` or `Reject`.
    /// @param subject Who it is about.
    /// @param key The key an `Approve` names, which must be the row's; nothing for a `Reject`.
    /// @param peer Who asked; `ClusterAdmit` gates on the membership `Context` folds from it.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerDecision(
        CompileCacheWire::EnrollControlVerb verb,
        std::string_view subject,
        std::optional<std::array<std::byte, CompileCacheWire::IdentityPublicKeyBytes>> const& key,
        PeerIdentity const& peer);

    /// Who is asking, as both the door and `ClusterAdmit` need it.
    ///
    /// One place the peer becomes a `CallerContext`, so an early refusal's
    /// classification is by construction the one the verb would have got --
    /// `SchedulerResponder::Context`'s argument, and the same shape.
    /// **The identity is folded through `Distributed::CallerContextOf`, since #1428 and #178**,
    /// here and not at the call site: this is the one place the peer becomes a `CallerContext`,
    /// and a fold at one call site would make the door and the authoritative gate answer
    /// differently about one connection.
    /// @param peer The caller, whose host is taken over by the returned context.
    /// @return The context.
    [[nodiscard]] Distributed::CallerContext Context(PeerIdentity peer) const
    {
        return Distributed::CallerContextOf(_membership, std::move(peer));
    }

    EnrollmentWindow& _window;
    Distributed::SchedulerService& _scheduler;
    Distributed::IMembershipOracle const& _membership;
    Cluster::IFleetSummarySource const& _self;
    Ed25519KeyPair const& _identity;
    IMetricsSink& _metrics;

    /// Where the one condition no counter can carry is said out loud: a machine rejected
    /// after its approval had already admitted it. It is per node and per machine, and an
    /// operator meets it in the log at the moment they go looking -- a counter would be a
    /// second tally with no second audience.
    ILogger& _logger;
};

} // namespace FastCache::Node
