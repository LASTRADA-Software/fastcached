// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/EndpointDialer.hpp>
#include <apps/fastcache-compile-node/EnrollChannel.hpp>
#include <apps/fastcache-compile-node/EnrollClient.hpp>
#include <apps/fastcache-compile-node/FleetProbe.hpp>
#include <apps/fastcache-compile-node/FormationController.hpp>
#include <apps/fastcache-compile-node/FormationLoop.hpp>
#include <apps/fastcache-compile-node/NodeConfig.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

/// @file NodeFormationControllerFakes.hpp
/// The fakes a formation controller is driven through: the fleet it asks, the seeds it probes, the
/// store it archives and the reform it asks for.
///
/// App-level, beside `NodeFormationFakes.hpp`, because every seam here is a node header, and
/// `FormationFakes.hpp` is included by library tests that must not reach one. Shared for that
/// header's reason: a WRONG fake makes its cases pass.
namespace FastCache::Testing
{

/// One poll, as an `IEnrollChannel` was asked it.
struct AskedPoll
{
    std::string endpoint;         ///< Where it was sent.
    Node::JoinerIdentity self;    ///< Who asked.
    std::vector<std::byte> nonce; ///< What the asker drew for it.
};

/// How a scripted answer is signed when its poll is answered: by whose key, for which cluster, and
/// over which poll's nonce -- a value no script can know beforehand, so it is signed at the poll. The
/// outcome signed is the one the reading was read from (`Node::SignedOutcomeOf`).
struct AdmissionSigning
{
    std::string signer;    ///< Whose `TestKeyPair` signs it.
    std::string clusterId; ///< The cluster it admits into.

    /// Sign over this EARLIER poll's nonce rather than the one answered -- a recorded admission
    /// replayed to a later ask. Empty signs over the poll answered.
    std::optional<std::size_t> overPoll {};
};

/// The outcome a fleet signs for each reading it answers with, written HERE rather than read from
/// `Node::SignedOutcomeOf`: a fake signing over the very mapping the controller verifies with would
/// agree with any swap of that mapping's rows, so no controller case could see one.
/// @param progress The reading answered.
/// @return The outcome the fleet's signature covers, or nothing for a reading nobody signs.
[[nodiscard]] constexpr std::optional<CompileCacheWire::EnrollOutcome> FleetSignedOutcomeOf(
    Node::EnrollProgress progress) noexcept
{
    constexpr auto signs = std::array {
        std::pair { Node::EnrollProgress::Waiting, CompileCacheWire::EnrollOutcome::Pending },
        std::pair { Node::EnrollProgress::Admitted, CompileCacheWire::EnrollOutcome::Approved },
        std::pair { Node::EnrollProgress::Refused, CompileCacheWire::EnrollOutcome::Rejected },
    };
    for (auto const& [reading, outcome]: signs)
        if (reading == progress)
            return outcome;
    return std::nullopt;
}

/// One scripted answer to a poll: a reading, signed at the poll when the script says how.
///
/// Data rather than a callable, so a script states WHAT answers and the fake alone decides how.
class ScriptedAnswer
{
  public:
    /// Answer with @p reading as it is. Implicit, so a script lists readings as readings.
    /// @param reading The reading.
    ScriptedAnswer(Node::EnrollReading reading):
        _reading { std::move(reading) }
    {
    }

    /// Answer with @p reading, carrying a signature made as @p signing says.
    /// @param reading The reading; its outcome and roster are what is signed, so it is one a fleet signs.
    /// @param signing How.
    ScriptedAnswer(Node::EnrollReading reading, AdmissionSigning signing):
        _reading { std::move(reading) },
        _signing { std::move(signing) }
    {
    }

    /// @param asked Every poll so far, the one answered last.
    /// @return The answer.
    [[nodiscard]] Node::EnrollReading Answer(std::span<AskedPoll const> asked) const
    {
        auto reading = _reading;
        auto const outcome = FleetSignedOutcomeOf(reading.progress);
        if (!_signing.has_value() || !outcome.has_value())
            return reading;
        auto const& poll = asked.back();
        auto const& signedOver = asked[_signing->overPoll.value_or(asked.size() - 1)];
        reading.signature = Cluster::SignAdmission(TestKeyPair(_signing->signer),
                                                   Cluster::AdmissionClaim { .nonce = signedOver.nonce,
                                                                             .joinerId = poll.self.nodeId,
                                                                             .joinerKey = poll.self.publicKey,
                                                                             .clusterId = _signing->clusterId,
                                                                             .outcome = *outcome,
                                                                             .roster = reading.roster });
        return reading;
    }

  private:
    Node::EnrollReading _reading;
    std::optional<AdmissionSigning> _signing;
};

/// Answers each `Enroll` poll from a script, and records every poll it was asked.
class ScriptedEnrollChannel final: public Node::IEnrollChannel
{
  public:
    /// Answer the next polls with @p answers, in order.
    /// @param answers One answer per poll.
    void Script(std::vector<ScriptedAnswer> answers)
    {
        _script.insert(_script.end(), std::make_move_iterator(answers.begin()), std::make_move_iterator(answers.end()));
    }

    /// Answer every poll past the script with @p answer: a fleet that keeps saying the same thing.
    /// @param answer The answer.
    void ScriptForever(ScriptedAnswer answer)
    {
        _forever = std::move(answer);
    }

    /// @copydoc Node::IEnrollChannel::Poll
    /// A poll past the script, with nothing scripted forever, is WAITING: a fleet that has not
    /// decided, which moves nothing -- never an answer a case did not ask for.
    [[nodiscard]] Node::EnrollReading Poll(std::string_view endpoint,
                                           Node::JoinerIdentity const& self,
                                           std::span<std::byte const> nonce) override
    {
        _asked.push_back(
            AskedPoll { .endpoint = std::string { endpoint }, .self = self, .nonce = { nonce.begin(), nonce.end() } });
        if (!_script.empty())
        {
            auto next = std::move(_script.front());
            _script.pop_front();
            return next.Answer(_asked);
        }
        if (_forever.has_value())
            return _forever->Answer(_asked);
        return Node::EnrollReading {
            .progress = Node::EnrollProgress::Waiting, .detail = "nothing scripted", .roster = {}, .certificate = {}
        };
    }

    /// @return Where every poll went, in order.
    [[nodiscard]] std::vector<std::string> AskedEndpoints() const
    {
        auto endpoints = std::vector<std::string> {};
        for (auto const& poll: _asked)
            endpoints.push_back(poll.endpoint);
        return endpoints;
    }

    /// @return Every poll, in order.
    [[nodiscard]] std::vector<AskedPoll> const& Asked() const noexcept
    {
        return _asked;
    }

  private:
    std::vector<AskedPoll> _asked;
    std::deque<ScriptedAnswer> _script;
    std::optional<ScriptedAnswer> _forever;
};

/// Records every cluster whose store it was asked to archive, and how many records the store had
/// kept by then -- which is what shows the record was written BEFORE the archive ran.
class RecordingArchiver final: public Node::IStoreArchiver
{
  public:
    /// @copydoc Node::IStoreArchiver::Archive
    [[nodiscard]] std::expected<void, std::string> Archive(std::string_view clusterId) override
    {
        if (_failure.has_value())
            return std::unexpected { *_failure };
        _archived.emplace_back(clusterId);
        return {};
    }

    /// Make every later archive fail with @p reason.
    /// @param reason What the refusal says.
    void FailArchives(std::string reason)
    {
        _failure = std::move(reason);
    }

    /// @return Every cluster archived, in order.
    [[nodiscard]] std::vector<std::string> const& Archived() const noexcept
    {
        return _archived;
    }

  private:
    std::vector<std::string> _archived;
    std::optional<std::string> _failure;
};

/// A dialer that reaches nobody, and counts how often it was asked.
class UnreachableDialer final: public Node::IEndpointDialer
{
  public:
    /// @return Nothing: no endpoint answers.
    [[nodiscard]] std::unique_ptr<core::net::ISocket> Dial(std::string_view /*endpoint*/,
                                                           core::net::DialOptions /*options*/) override
    {
        _dials.fetch_add(1);
        return nullptr;
    }

    /// @return How many dials were asked.
    [[nodiscard]] int Dials() const noexcept
    {
        return _dials.load();
    }

  private:
    std::atomic<int> _dials { 0 }; ///< Every dial asked.
};

/// Judges every record a move is about to write by a script: nothing refused until a case says so.
class ScriptedShapeJudge final: public Node::IShapeJudge
{
  public:
    /// @copydoc Node::IShapeJudge::RefusalOf
    [[nodiscard]] std::optional<std::string> RefusalOf(Cluster::FormationRecord const& next) const override
    {
        auto const lock = std::scoped_lock { _lock };
        _asked.push_back(next.mode);
        if (_delegate != nullptr)
            return _delegate->RefusalOf(next);
        if (_refuseMode.has_value() && *_refuseMode == next.mode)
            return _refusal;
        return std::nullopt;
    }

    /// Answer every later question as @p judge does -- a production judge put where a rig's fake sits.
    /// @param judge The judge asked; must outlive every question.
    void DelegateTo(Node::IShapeJudge const& judge)
    {
        auto const lock = std::scoped_lock { _lock };
        _delegate = &judge;
    }

    /// Refuse every record that moves the node into @p mode, with @p refusal.
    /// @param mode The mode refused.
    /// @param refusal What the refusal says.
    void Refuse(Cluster::NodeMode mode, std::string refusal)
    {
        auto const lock = std::scoped_lock { _lock };
        _refuseMode = mode;
        _refusal = std::move(refusal);
    }

    /// Refuse nothing again.
    void AcceptAll()
    {
        auto const lock = std::scoped_lock { _lock };
        _refuseMode.reset();
    }

    /// @return The mode of every record it was asked about, in order.
    [[nodiscard]] std::vector<Cluster::NodeMode> Asked() const
    {
        auto const lock = std::scoped_lock { _lock };
        return _asked;
    }

  private:
    mutable std::mutex _lock;                       ///< Guards the script and the record of questions.
    mutable std::vector<Cluster::NodeMode> _asked;  ///< See `Asked`.
    std::optional<Cluster::NodeMode> _refuseMode;   ///< The mode refused, if any.
    std::string _refusal;                           ///< What a refusal says.
    Node::IShapeJudge const* _delegate { nullptr }; ///< Asked instead of the script, once set.
};

/// Records every configuration a reform published, in order.
struct RecordingPublisher final: Node::IConfigPublisher
{
    /// @copydoc Node::IConfigPublisher::Publish
    void Publish(Node::NodeConfig const& cfg) override
    {
        published.push_back(cfg);
    }

    std::vector<Node::NodeConfig> published; ///< What was published, in order.
};

/// Answers a seed probe from a table of endpoints, and records every seed it was asked.
class ScriptedFleetProbe final: public Node::IFleetProbe
{
  public:
    /// Answer every probe of @p endpoint with @p fleet.
    /// @param endpoint The seed.
    /// @param fleet What it proves.
    void Answer(std::string endpoint, Cluster::ProvenFleet fleet)
    {
        _answers.emplace_back(std::move(endpoint), std::move(fleet));
    }

    /// @copydoc Node::IFleetProbe::Ask
    [[nodiscard]] std::expected<Cluster::ProvenFleet, std::string> Ask(Cluster::SeedCandidate const& seed) override
    {
        _asked.push_back(seed);
        for (auto const& [endpoint, fleet]: _answers)
            if (endpoint == seed.endpoint)
                return fleet;
        return std::unexpected { "nothing answers at " + seed.endpoint };
    }

    /// @return Every seed asked, in order.
    [[nodiscard]] std::vector<Cluster::SeedCandidate> const& Asked() const noexcept
    {
        return _asked;
    }

    /// Answer every summary question of @p endpoint with @p summary.
    /// @param endpoint Whom it asks.
    /// @param summary What it proves.
    void AnswerSummary(std::string endpoint, Cluster::ProvenFleetSummary summary)
    {
        _summaries.emplace_back(std::move(endpoint), std::move(summary));
    }

    /// @copydoc Node::IFleetProbe::AskSummary
    [[nodiscard]] std::expected<Cluster::ProvenFleetSummary, std::string> AskSummary(std::string_view endpoint) override
    {
        _askedSummaries.emplace_back(endpoint);
        for (auto const& [answering, summary]: _summaries)
            if (answering == endpoint)
                return summary;
        return std::unexpected { "nothing answers at " + std::string { endpoint } };
    }

    /// @return Every endpoint asked for a summary, in order.
    [[nodiscard]] std::vector<std::string> const& AskedSummaries() const noexcept
    {
        return _askedSummaries;
    }

  private:
    std::vector<Cluster::SeedCandidate> _asked;
    std::vector<std::pair<std::string, Cluster::ProvenFleet>> _answers;
    std::vector<std::string> _askedSummaries;
    std::vector<std::pair<std::string, Cluster::ProvenFleetSummary>> _summaries;
};

/// Records every command a controller proposes to its cluster, and answers each as a case scripts.
class RecordingClusterAdmin final: public Distributed::IClusterAdmin
{
  public:
    /// @copydoc Distributed::IClusterAdmin::ClusterState
    [[nodiscard]] Cluster::ClusterState ClusterState() const override
    {
        std::scoped_lock const lock { _lock };
        return _state;
    }

    /// @copydoc Distributed::IClusterAdmin::ProposeToCluster
    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override
    {
        std::scoped_lock const lock { _lock };
        _proposed.push_back(command);
        if (_refusal.has_value())
            return std::unexpected { *_refusal };
        return {};
    }

    /// Refuse every later proposal with @p refusal: a node that stopped leading, say.
    /// @param refusal What each proposal answers.
    void Refuse(ConsensusError refusal)
    {
        std::scoped_lock const lock { _lock };
        _refusal = std::move(refusal);
    }

    /// @return Every command proposed, in order, refused ones included.
    [[nodiscard]] std::vector<Cluster::Command> Proposed() const
    {
        std::scoped_lock const lock { _lock };
        return _proposed;
    }

  private:
    mutable std::mutex _lock;
    Cluster::ClusterState _state;
    std::vector<Cluster::Command> _proposed;
    std::optional<ConsensusError> _refusal;
};

/// Counts the reforms it was asked for.
class CountingReformSignal final: public Node::IReformSignal
{
  public:
    /// @copydoc Node::IReformSignal::RequestReform
    void RequestReform() override
    {
        ++_requests;
    }

    /// @return How many were asked for.
    [[nodiscard]] int Requests() const noexcept
    {
        return _requests;
    }

  private:
    int _requests { 0 };
};

} // namespace FastCache::Testing
