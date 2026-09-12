// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "EnrollmentWindow.hpp"
#include "FrameEndpoint.hpp"

#include <FastCache/Auth/AuthPolicy.hpp>
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

/// @file EnrollmentResponder.hpp
/// The runtime enrollment window's front door.
///
/// ## Why this is a component of its own
///
/// `Op::Enroll` is `OpenBeforeAuth` and must admit a machine that is not a member --
/// that machine holding no secret of this cluster is the entire problem being solved.
/// Every other verb on this wire but `AUTH` requires a credential, and the two
/// surfaces that could have hosted this pair both answer *may this peer be here* once
/// for every verb they own: `SchedulerResponder::AuthRequired` ignores the opcode and
/// its `RefusePeer` delegates to the one membership check the scheduler's gate uses.
/// So folding `Enroll` in would either put the verb behind the answer that refuses it,
/// or relax that answer for the nine verbs beside it.
///
/// Separated, the change is purely ADDITIVE and that is checkable rather than argued:
/// no existing responder's `RefusePeer`, `AuthRequired` or gate is touched, and the one
/// door held open is the one verb whose row in `OpTable` says so.
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
/// decision: a window that can never admit anybody should not be openable.

/// Where the cluster key is read from when a joiner is approved.
///
/// **Read where it is PRESENTED, never captured at construction**, which is this
/// project's rule for an outbound credential and is the whole reason this is a seam
/// rather than a `SecureByteBuffer` member. The key is handed over a handful of times
/// in a fleet's life, so holding a second copy of the cluster's secret in this
/// process's memory for its whole uptime buys nothing and costs exactly what the rule
/// is about.
class IClusterKeySource
{
  public:
    IClusterKeySource() = default;
    IClusterKeySource(IClusterKeySource const&) = delete;
    IClusterKeySource(IClusterKeySource&&) = delete;
    IClusterKeySource& operator=(IClusterKeySource const&) = delete;
    IClusterKeySource& operator=(IClusterKeySource&&) = delete;
    virtual ~IClusterKeySource() = default;

    /// The key to hand an approved joiner.
    ///
    /// An `expected` rather than an empty buffer for a failure, because *this node
    /// holds no key* and *the key file has gone* are different facts and only the
    /// second is worth an operator's attention -- and a joiner handed zero bytes under
    /// a successful outcome would write an empty key file and fail much later, on a
    /// machine nobody is watching any more.
    /// @return The key, or why it could not be read.
    [[nodiscard]] virtual std::expected<SecureByteBuffer, std::string> ClusterKey() const = 0;
};

/// `IClusterKeySource` reading `--cluster-key-file` at each hand-over.
class FileClusterKeySource final: public IClusterKeySource
{
  public:
    /// @param path Where the key lives; empty means this node holds none.
    explicit FileClusterKeySource(std::filesystem::path path) noexcept:
        _path { std::move(path) }
    {
    }

    /// @copydoc IClusterKeySource::ClusterKey
    ///
    /// Through `ReadClusterKey`, which is the one reader in this binary: it holds the
    /// minimum-length rule and the trailing-newline trim, so a key handed to a joiner
    /// is byte-for-byte the key every other tier on this node already uses. A second
    /// reader would eventually differ by a newline, which is an HMAC that verifies
    /// nowhere and no diagnostic anywhere.
    [[nodiscard]] std::expected<SecureByteBuffer, std::string> ClusterKey() const override;

  private:
    std::filesystem::path _path;
};

/// Serves `Enroll` and `EnrollControl`.
class EnrollmentResponder final: public IFrameResponder
{
  public:
    /// @param window The window this surface reports on and mutates; must outlive this.
    /// @param scheduler Whose leadership decides whether this node may answer, and
    ///        whose `ClusterAdmit` records an approval; must outlive this.
    /// @param membership Who may reach `EnrollControl`; must outlive this.
    /// @param key Where an approved joiner's key comes from; must outlive this.
    /// @param metrics Where refusals and hand-overs are recorded; must outlive this.
    /// @param policy The credential this surface requires, or nullptr for none. Shared
    ///        rather than referenced because "there is no credential" has to be
    ///        representable, and a null reference is not.
    EnrollmentResponder(EnrollmentWindow& window,
                        Distributed::SchedulerService& scheduler,
                        Distributed::IMembershipOracle const& membership,
                        IClusterKeySource const& key,
                        IMetricsSink& metrics,
                        ILogger& logger,
                        std::shared_ptr<AuthPolicy const> policy = nullptr) noexcept:
        _window { window },
        _scheduler { scheduler },
        _membership { membership },
        _key { key },
        _metrics { metrics },
        _logger { logger },
        _policy { std::move(policy) }
    {
    }

    /// @copydoc IFrameResponder::Answer
    ///
    /// Never suspends: every decision is taken from this node's own memory, and the one
    /// thing that touches the filesystem -- reading the cluster key -- is a file a few
    /// dozen bytes long, read on the rare path where a person has just approved
    /// somebody.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    ///
    /// **`Enroll` is admitted whoever asks, and that is the one hole this surface
    /// opens.** The machine asking holds no secret of this cluster and is on no list --
    /// it is a fresh install -- so a membership test here would refuse exactly the
    /// population the verb exists for. What stands in place of the credential is a
    /// person: the window is closed by default, closes again on restart, and admits
    /// nobody at all until an operator approves a named id.
    ///
    /// `EnrollControl` is refused to a non-member, before a payload is read, and
    /// counted. That refusal is the one carrying the security argument for the pair: a
    /// peer reaching it has found an open window and gone on to ask for the decision.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// The surface-wide answer, and the opcode is deliberately ignored: which verb is
    /// reachable before a credential is `OpTable::preAuth`'s column and
    /// `DecidePrePayload` reads it, so answering per verb here would be a second
    /// spelling of the pre-auth set -- one a reviewer cannot see from the table, and
    /// one that can disagree with it.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return _policy != nullptr && _policy->Enabled();
    }

    /// @copydoc IFrameResponder::CheckCredential
    ///
    /// Delegates to this surface's own policy, which is the scheduler's object. It is
    /// unreachable through `MergedResponder` -- `AUTH` is a `Session` verb and routes to
    /// the scheduler -- and answered properly rather than stubbed, because a surface
    /// that inherits an answer inherits an open door by saying nothing.
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(_policy.get(), payload);
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
    /// `MergedResponder::Largest` folds `_cache`, `_scheduler` and `_compile`, and this
    /// responder is not among them, so on the merged listener this number is not
    /// consulted. Saying *surface* of a value the surface never reads is the kind of
    /// claim that makes the pre-auth bound look like it rests here, when it rests on the
    /// verb's own row.
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload;
    }

    /// A handful: one operator and however many machines are enrolling at once.
    ///
    /// Deliberately small. Every other ceiling on this node is sized for a fleet's
    /// worth of workers or a build's worth of launchers; this surface serves one person
    /// and at most `MaxPendingEnrollments` joiners that each dial, ask and hang up. A
    /// generous number here would be capacity handed to whoever can reach an open
    /// window, on the one surface where that is anybody.
    ///
    /// **That paragraph is inverted, and it is kept above so the inversion is legible.**
    /// `MergedResponder::Largest` folds `_cache`, `_scheduler` and `_compile`; this
    /// responder is in no fold, so nothing reads this number today. And `Largest` is a
    /// MAXIMUM, so the only effect this value could ever have if it were folded is to
    /// WIDEN: 128 is a no-op beside the incumbents, while raising it -- which the
    /// paragraph above invites, by arguing that a small number is protective -- would
    /// raise the cap for the cache and compile surfaces too. Generous here is the one
    /// direction that does anything, and it is the direction the warning warns against.
    ///
    /// Two edits that each read as reasonable compose into that: folding this member in
    /// (looks like the obvious repair, changes nothing) and then raising the number
    /// (looks like tuning, widens three surfaces).
    ///
    /// **A per-component narrowing is not expressible at this seam at all.**
    /// `RequestTimeout` and `HoldsOwnByteBudget` route to the owner because they are
    /// properties of the VERB. Connections and in-flight bytes are properties of one
    /// listener, one accept queue and one byte budget, so they cannot be per-component
    /// and a max is the only coherent fold over them. The number below is this
    /// responder's own honest answer and nothing more; it is a pure virtual, so it
    /// cannot simply be dropped.
    ///
    /// **What actually bounds an unauthenticated peer's exposure here is that the window
    /// is CLOSED by default**, opened only by a deliberate operator act, held in memory
    /// and forgotten on restart -- plus `Op::Enroll`'s own per-verb payload cap. No
    /// number in this class bounds it, and the accounting that would is tracked on
    /// #1338 rather than improvised here.
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

  private:
    /// Answer one `Enroll`.
    /// @param payload The request payload.
    /// @param peer The host the kernel reports.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerEnroll(std::span<std::byte const> payload, std::string_view peer);

    /// Answer one `EnrollControl`.
    /// @param payload The request payload.
    /// @param peer The host the kernel reports.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerControl(std::span<std::byte const> payload, std::string_view peer);

    /// Apply one operator decision to one waiting id.
    /// @param verb `Approve` or `Reject`.
    /// @param subject Who it is about.
    /// @param peer The host the kernel reports; `ClusterAdmit` gates on it.
    /// @return The encoded reply.
    [[nodiscard]] std::vector<std::byte> AnswerDecision(CompileCacheWire::EnrollControlVerb verb,
                                                        std::string_view subject,
                                                        std::string_view peer);

    /// Who is asking, as both the door and `ClusterAdmit` need it.
    ///
    /// One place the peer becomes a `CallerContext`, so an early refusal's
    /// classification is by construction the one the verb would have got --
    /// `SchedulerResponder::Context`'s argument, and the same shape.
    /// @param peer The caller's peer host, taken over by the returned context.
    /// @return The context.
    [[nodiscard]] Distributed::CallerContext Context(std::string peer) const
    {
        auto membership = _membership.Classify(peer);
        return Distributed::CallerContext { .membership = membership, .peerId = std::move(peer) };
    }

    EnrollmentWindow& _window;
    Distributed::SchedulerService& _scheduler;
    Distributed::IMembershipOracle const& _membership;
    IClusterKeySource const& _key;
    IMetricsSink& _metrics;

    /// Where the one condition no counter can carry is said out loud: a claim this
    /// surface took and could not give back. It is per node and per machine, and an
    /// operator meets it in the log at the moment they go looking for why a joiner is
    /// stuck -- a counter would be a second tally with no second audience.
    ILogger& _logger;

    std::shared_ptr<AuthPolicy const> _policy;
};

} // namespace FastCache::Node
