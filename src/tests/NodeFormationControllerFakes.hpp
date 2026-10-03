// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/EnrollChannel.hpp>
#include <apps/fastcache-compile-node/EnrollClient.hpp>
#include <apps/fastcache-compile-node/FleetProbe.hpp>
#include <apps/fastcache-compile-node/FormationController.hpp>
#include <tests/FormationFakes.hpp>

/// @file NodeFormationControllerFakes.hpp
/// The fakes a formation controller is driven through: the fleet it asks, the seeds it probes, the
/// store it archives and the reform it asks for.
///
/// App-level, beside `NodeFormationFakes.hpp`, because every seam here is a node header, and
/// `FormationFakes.hpp` is included by library tests that must not reach one. Shared for that
/// header's reason: a WRONG fake makes its cases pass.
namespace FastCache::Testing
{

/// Answers each `Enroll` poll from a script, and records every poll it was asked.
class ScriptedEnrollChannel final: public Node::IEnrollChannel
{
  public:
    /// One poll, as it was asked.
    struct AskedPoll
    {
        std::string endpoint;      ///< Where it was sent.
        Node::JoinerIdentity self; ///< Who asked.
    };

    /// Answer the next polls with @p readings, in order.
    /// @param readings One reading per poll.
    void Script(std::vector<Node::EnrollReading> readings)
    {
        _script.insert(_script.end(), std::make_move_iterator(readings.begin()), std::make_move_iterator(readings.end()));
    }

    /// Answer every poll past the script with @p reading: a fleet that keeps saying the same thing.
    /// @param reading The reading.
    void ScriptForever(Node::EnrollReading reading)
    {
        _forever = std::move(reading);
    }

    /// @copydoc Node::IEnrollChannel::Poll
    /// A poll past the script, with nothing scripted forever, is WAITING: a fleet that has not
    /// decided, which moves nothing -- never an answer a case did not ask for.
    [[nodiscard]] Node::EnrollReading Poll(std::string_view endpoint, Node::JoinerIdentity const& self) override
    {
        _asked.push_back(AskedPoll { .endpoint = std::string { endpoint }, .self = self });
        if (!_script.empty())
        {
            auto next = std::move(_script.front());
            _script.pop_front();
            return next;
        }
        if (_forever.has_value())
            return *_forever;
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
    std::deque<Node::EnrollReading> _script;
    std::optional<Node::EnrollReading> _forever;
};

/// Records every cluster whose store it was asked to archive, and how many records the store had
/// kept by then -- which is what shows the record was written BEFORE the archive ran.
class RecordingArchiver final: public Node::IStoreArchiver
{
  public:
    /// @param store The store whose saves an archive is counted against; must outlive this.
    explicit RecordingArchiver(InMemoryFormationStore const& store):
        _store { store }
    {
    }

    /// @copydoc Node::IStoreArchiver::Archive
    /// While archives are HELD, it says it has been entered and waits for `ReleaseArchives` -- a disk
    /// that is slow, as long as a case wants -- bounded so a case that never releases fails rather
    /// than hangs.
    [[nodiscard]] std::expected<void, std::string> Archive(std::string_view clusterId) override
    {
        {
            std::unique_lock gate { _gate };
            _entered = true;
            _changed.notify_all();
            (void) _changed.wait_for(gate, HeldArchiveBound, [this] { return !_held; });
        }
        _archivedAfterSave = _store.Saves().size();
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

    /// Hold every later archive until `ReleaseArchives`.
    void HoldArchives()
    {
        std::scoped_lock const gate { _gate };
        _held = true;
    }

    /// Let held archives, and every later one, run.
    void ReleaseArchives()
    {
        std::scoped_lock const gate { _gate };
        _held = false;
        _changed.notify_all();
    }

    /// @return Whether an archive has been entered, held or not.
    [[nodiscard]] bool Entered() const
    {
        std::scoped_lock const gate { _gate };
        return _entered;
    }

    /// @return Every cluster archived, in order.
    [[nodiscard]] std::vector<std::string> const& Archived() const noexcept
    {
        return _archived;
    }

    /// @return How many records the store held when the last archive ran.
    [[nodiscard]] std::size_t ArchivedAfterSave() const noexcept
    {
        return _archivedAfterSave;
    }

  private:
    /// How long a held archive waits for its release before it runs anyway.
    static constexpr std::chrono::seconds HeldArchiveBound { 10 };

    InMemoryFormationStore const& _store;
    mutable std::mutex _gate;
    std::condition_variable _changed;
    bool _held { false };
    bool _entered { false };
    std::vector<std::string> _archived;
    std::size_t _archivedAfterSave { 0 };
    std::optional<std::string> _failure;
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

  private:
    std::vector<Cluster::SeedCandidate> _asked;
    std::vector<std::pair<std::string, Cluster::ProvenFleet>> _answers;
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
