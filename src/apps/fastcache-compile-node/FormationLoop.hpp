// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FormationController.hpp"
#include "LiveNodeConfig.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeReload.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/InheritedListener.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

/// @file FormationLoop.hpp
/// **A restart and a transition are one code path.** A move that changes a node's shape -- which
/// consensus it runs, whether the Raft port is bound, whether it serves the scheduler -- saves the
/// record and asks for a reform; the serving body then ends exactly as it ends for a stop, and the
/// loop adopts the record again and starts a fresh body from it. So what a start does with a record
/// -- finish an interrupted archive, apply the mode -- is what every reform does, and nothing a
/// previous body derived survives into the next.
namespace FastCache::Node
{

/// How a serving body ended. **Private**: never transmitted or persisted.
enum class BodyEnd : std::uint8_t
{
    Stopped, ///< It served until it was asked to stop.
    Refused, ///< It refused to start, or ended in a refusal.
    Reform,  ///< A move changed the node's shape; the loop adopts the record and starts a body again.
};

/// What a serving body ended with.
struct BodyOutcome
{
    BodyEnd end { BodyEnd::Stopped }; ///< Why it ended.
    int exitCode { 0 };               ///< What the process returns, when this is the last body.
};

/// One serving body, built from the configuration the record produced.
using ServingBody = std::function<BodyOutcome(NodeConfig const& cfg)>;

/// Adopt the formation record into a copy of the configuration, or say why the node must not start.
using FormationAdopter = std::function<std::expected<NodeConfig, std::string>(NodeConfig cfg)>;

/// How long the loop waits before each reform of a RUN of them: the n-th waits the n-th row, and every
/// later one the last. A record that flaps between two shapes is paced the way a supervisor's start
/// limit paces a sequence of restarts, while the move a zero-config node makes once -- solitary to a
/// member -- waits nothing.
inline constexpr auto ReformBackoff = std::to_array<std::chrono::milliseconds>({
    std::chrono::milliseconds { 0 },
    std::chrono::milliseconds { 1'000 },
    std::chrono::milliseconds { 5'000 },
    std::chrono::milliseconds { 15'000 },
    std::chrono::milliseconds { 30'000 },
});

/// A body that served at least this long ends a run of reforms: the reform after it waits the first row
/// of `ReformBackoff` again. Long enough that a shape a fleet keeps moving a node out of is a run.
inline constexpr std::chrono::minutes ReformRunEnds { 5 };

/// Where a reform stands in a run of them, and how long it waits before its body.
struct ReformPace
{
    std::size_t run { 0 };                                                ///< How many reforms in a row, this one included.
    std::chrono::milliseconds wait { std::chrono::milliseconds::zero() }; ///< Its `ReformBackoff` row.
};

/// Pace the reform that follows a body: a body that served `ReformRunEnds` or longer begins a new
/// run, any other continues it, and the n-th reform of a run waits the n-th `ReformBackoff` row and
/// every later one the last. One function, so whatever runs bodies paces them the way `main` does.
/// @param run How many reforms in a row preceded this one; 0 before the first.
/// @param served How long the body that just ended served.
/// @return The run this reform is in, and how long it waits.
[[nodiscard]] ReformPace NextReformPace(std::size_t run, core::platform::SteadyTimePoint::duration served) noexcept;

/// How the loop learns of a stop and paces itself between bodies.
struct LoopControls
{
    /// Whether a stop was asked: the daemon's stop flag. A stop that arrives between two bodies
    /// starts no further one.
    std::function<bool()> stopRequested;

    /// Wait as long as asked, returning early once a stop is asked.
    std::function<void(std::chrono::milliseconds)> pause;

    /// What how long a body served is read from.
    core::platform::IClock const& clock;
};

/// The reform flag a formation controller raises and a serving body's stop loop reads.
///
/// A flag rather than a call into the body: the move that asks is taken on the controller's
/// thread, and what ends a body is its own loop noticing -- the same shape as a stop, so a reform
/// drains exactly what a stop drains.
class ReformRequest final: public IReformSignal
{
  public:
    /// @copydoc IReformSignal::RequestReform
    ///
    /// Safe from any thread.
    void RequestReform() override;

    /// @return Whether a reform was asked for and not taken yet. Leaves it pending.
    [[nodiscard]] bool Pending() const noexcept;

    /// Read and clear.
    /// @return Whether a reform was asked for since the last `Take`.
    [[nodiscard]] bool Take() noexcept;

  private:
    std::atomic<bool> _requested { false };
};

/// How a body that returned @p exitCode ended, given the reform flag and whether a stop was asked.
///
/// A body ends for a reform only when it served and ended cleanly (`ExitCodeOf(ProcessExit::Served)`) with a reform pending
/// and no stop asked: a body that refused is a refusal whatever was pending, and a stop asked during
/// a reform is a stop -- an operator stopping a node mid-move must not be answered by a restart.
/// Takes the flag, so the next body starts with none pending.
/// @param exitCode What the body returned.
/// @param reform The reform flag.
/// @param stopRequested Whether a stop was asked.
/// @return The outcome.
[[nodiscard]] BodyOutcome ClassifyBodyEnd(int exitCode, ReformRequest& reform, bool stopRequested) noexcept;

/// What a body says as it stops accepting work, which is why it is draining.
///
/// The stop's sentence is unchanged byte for byte: the e2e fixtures wait on it (`dist-compile-e2e.sh`).
/// @param reforming Whether a reform, rather than a stop, ended the body.
/// @return The sentence.
[[nodiscard]] std::string_view DrainSentence(bool reforming) noexcept;

/// What adopting the formation record found.
struct AdoptedFormation
{
    Cluster::FormationRecord record;    ///< The record the body runs by, its pending archive finished.
    Cluster::FleetEndpoints remembered; ///< The fleet endpoints last known; empty when none are.
};

/// Read (or mint) the record, finish an interrupted archive, and apply it to @p cfg: the START's adoption.
///
/// In order, stopping at the first failure: `ReadKeptFormation` -- a record another build wrote, or
/// a damaged one, is refused by name and never minted over; `KeepFormation` -- only an ABSENT record
/// mints, and the mint is saved before anything reads it; `ResumeFormation` -- the store a move left
/// in the root is archived before any consensus tier opens the directory, so a learner never starts
/// over the solitary log it was leaving; then `ApplyFormation`. A reform adopts through
/// `ReadoptFormation`, which never mints.
/// @param cfg The configuration to shape.
/// @param store Where the record is kept.
/// @param archiver What moves a left cluster's store out of the root.
/// @param endpoints Where the fleet endpoints are remembered.
/// @param random Where a minted cluster id comes from.
/// @param wall What a minted cluster's creation time is read from.
/// @return The record and the remembered endpoints, or why the node must not start.
[[nodiscard]] std::expected<AdoptedFormation, std::string> AdoptFormation(NodeConfig& cfg,
                                                                          Cluster::IFormationStore& store,
                                                                          IStoreArchiver& archiver,
                                                                          Cluster::FleetEndpointsFile& endpoints,
                                                                          ISecureRandom& random,
                                                                          core::platform::IWallClock const& wall);

/// Adopt the record again at a REFORM: `AdoptFormation` without the mint.
///
/// Minting belongs to the first start alone. The body that just ended was adopted from a record, so one
/// that is gone now is lost state, never a first start: minting would make the node solitary in a new
/// cluster, silently leaving its fleet, over the fleet's Raft store still in the root. Refused by name
/// instead (`FormationRecordGone`), as a reload refuses the same state.
/// @param cfg The configuration to shape.
/// @param store Where the record is kept.
/// @param archiver What moves a left cluster's store out of the root.
/// @param endpoints Where the fleet endpoints are remembered.
/// @return The record and the remembered endpoints, or why the node must not go on.
[[nodiscard]] std::expected<AdoptedFormation, std::string> ReadoptFormation(NodeConfig& cfg,
                                                                            Cluster::IFormationStore& store,
                                                                            IStoreArchiver& archiver,
                                                                            Cluster::FleetEndpointsFile& endpoints);

/// The listening socket a supervisor handed this process, kept for the PROCESS rather than for a body.
///
/// A body's listener owns the descriptor it adopts and closes it as the body ends -- and the handoff
/// happens ONCE: `AdoptInheritedDescriptors` clears the environment it read, so it cannot be asked
/// again. Handed the original, a reformed body would serve a descriptor the last body closed; handed
/// nothing, it would bind `--listen-node` for itself, on a port the supervisor holds. So `main` keeps
/// the original here for as long as the process runs, and hands every body a COPY its listener may own.
///
/// **POSIX only, and the copy must never be extended to Windows** -- nor to a port this process binds
/// itself. Every body has its own reactor, and on Windows a completion-port association belongs to the
/// SOCKET, not to the handle: once one body's copy was associated with its IOCP, associating a NEW copy
/// with the next body's IOCP is refused (`CreateIoCompletionPort` error 87, ERROR_INVALID_PARAMETER),
/// measured on Windows 11 build 26200 -- so the second body could never accept on it. A port the node
/// binds itself is instead bound AGAIN by every body; an exclusive rebind at once succeeds whether the
/// last body's client hung up first or was still connected (measured likewise, and held by
/// `FrameEndpoint_test`'s reform case). Windows hands no socket over, so it never reaches this class.
class ActivationHold
{
  public:
    /// @param inherited What the supervisor handed over, or nothing. Owned from here on.
    /// @param descriptors How a copy is made and the original closed. Must outlive this.
    ActivationHold(std::optional<int> inherited, IInheritedDescriptors const& descriptors) noexcept;
    ActivationHold(ActivationHold const&) = delete;
    ActivationHold(ActivationHold&&) = delete;
    ActivationHold& operator=(ActivationHold const&) = delete;
    ActivationHold& operator=(ActivationHold&&) = delete;

    /// Closes the original, which no body ever owned.
    ~ActivationHold();

    /// What one body serves.
    /// @return A copy of the inherited socket the body's listener owns; nothing when nothing was
    ///         handed over, so the body binds `--listen-node` itself; or why no copy could be made.
    [[nodiscard]] std::expected<std::optional<int>, std::string> ForBody() const;

  private:
    std::optional<int> _inherited;             ///< The supervisor's socket; closed by this, never by a body.
    IInheritedDescriptors const& _descriptors; ///< How a copy is made and the original closed.
};

/// Run bodies until one ends other than `Reform`, adopting the record again before each.
///
/// Every body gets a FRESH copy of the configuration IN FORCE with the record adopted into it, so a
/// reform starts from what the operator has configured now and the record alone -- never from what a
/// previous body derived, and never from the START configuration: that one no reload changes, and a
/// body shaped from it, published as the configuration in force, silently reverted every accepted
/// reload -- a `--fleet-member` entry removed came back, a rotated `--requirepass` went back to the old
/// one. Between two bodies the loop waits its `ReformBackoff` row, and a stop asked by then starts no
/// further body.
/// @param config The configuration in force, read before each body: the reloader's live snapshot when
///        this node has a file, the start's when it has none (`LiveNodeConfig`).
/// @param body What serves.
/// @param adopt How the record is adopted before a body; a refusal ends the loop with the `Formation` stage's code.
/// @param controls Whether a stop arrived, how the loop waits, and what a body's serving time is read from.
/// @param logger Where a refusal and each reform are said.
/// @return The last body's exit code, or `ExitCodeFor(StartStage::Formation)` for a refused adoption.
[[nodiscard]] int RunFormationLoop(INodeConfigSource const& config,
                                   ServingBody const& body,
                                   FormationAdopter const& adopt,
                                   LoopControls const& controls,
                                   ILogger& logger);

/// Where the configuration a reformed body runs by is published: what a reload compares against, and
/// what the worker reads where it registers from.
class IConfigPublisher
{
  public:
    IConfigPublisher() = default;
    IConfigPublisher(IConfigPublisher const&) = delete;
    IConfigPublisher(IConfigPublisher&&) = delete;
    IConfigPublisher& operator=(IConfigPublisher const&) = delete;
    IConfigPublisher& operator=(IConfigPublisher&&) = delete;
    virtual ~IConfigPublisher() = default;

    /// @param cfg The configuration the next body runs by.
    virtual void Publish(NodeConfig const& cfg) = 0;
};

/// The node's reloader, as where a reformed body's configuration is published.
class ReloaderPublisher final: public IConfigPublisher
{
  public:
    /// @param reloader What publishes; must outlive this.
    explicit ReloaderPublisher(NodeReloader& reloader) noexcept:
        _reloader { reloader }
    {
    }

    /// @copydoc IConfigPublisher::Publish
    void Publish(NodeConfig const& cfg) override;

  private:
    NodeReloader& _reloader; ///< What publishes.
};

/// One serving body as `main` runs it.
///
/// Handed the record the body runs by -- the one it was adopted from -- and the listening socket it
/// serves: its OWN copy of what the supervisor handed over, or nothing, so it binds `--listen-node`.
/// Returns what the process exits with, should this be the last body.
using NodeBody =
    std::function<int(NodeConfig const& cfg, Cluster::FormationRecord const& record, std::optional<int> served)>;

/// What a reform adopts the record from, and where it publishes what it adopted.
struct ReformAdoption
{
    Cluster::IFormationStore& store;         ///< Where the record is kept.
    IStoreArchiver& archiver;                ///< What finishes a left cluster's store at every reform.
    Cluster::FleetEndpointsFile& endpoints;  ///< Where the fleet endpoints are remembered.
    IConfigPublisher* publisher { nullptr }; ///< Told what every reformed body runs by; null with no file.
};

/// What a reform does before its body runs, and all of it: the record adopted again into @p next
/// (`ReadoptFormation` -- never a mint), the reshaped configuration judged by the startup rules
/// (`StartupPolicyRejection`) BEFORE anything serves it, @p running rewritten to what was adopted, and
/// the configuration published.
///
/// **One function for `RunNodeBodies` and for anything else that reforms a body**, the formation
/// harness among them: a reform that skips the judge serves a shape the next restart refuses.
/// @param next A fresh copy of the configuration in force.
/// @param running Rewritten to what the reformed body is adopted from, when it is accepted.
/// @param parts Where the record is and where the result is published.
/// @return The configuration the reformed body runs by, or why the node must not serve it.
[[nodiscard]] std::expected<NodeConfig, std::string> AdoptForReform(NodeConfig next,
                                                                    AdoptedFormation& running,
                                                                    ReformAdoption const& parts);

/// What every serving body is run with, across reforms.
struct NodeBodies
{
    ReformAdoption adoption;          ///< What every reform adopts from and publishes to.
    ReformRequest& reform;            ///< Raised by a move; taken as each body ends.
    ActivationHold const& activation; ///< What each body serves a copy of.
    LoopControls controls;            ///< Whether a stop arrived, and how the loop paces itself.
};

/// Run serving bodies until one ends other than for a reform: what `main` serves by.
///
/// **A restart and a transition are one code path**, so every reform does what a start does: the
/// record is adopted again (`ReadoptFormation` -- never a mint), the reshaped configuration is judged
/// by the startup rules (`StartupPolicyRejection`) BEFORE the body runs, and is published. The FIRST
/// body runs by what the start adopted and judged, where a refusal could still be reported, and adopts
/// nothing. Every body serves its own copy of the inherited socket, and how it ended is
/// `ClassifyBodyEnd`'s.
/// @param config The configuration in force; every body is adopted into a copy of what it says when
///        that body starts, so the move judge, the reform and a restart read one configuration.
/// @param running What the start adopted, on entry; rewritten at every reform to what the running body
///        was adopted from -- which is what a reload shapes its candidate by (`RunningFormationOf`).
/// @param parts What every body is run with.
/// @param body What serves.
/// @param logger Where a refusal and each reform are said.
/// @return The last body's exit code; `ExitCodeFor(StartStage::Formation)` for a refused adoption.
[[nodiscard]] int RunNodeBodies(INodeConfigSource const& config,
                                AdoptedFormation& running,
                                NodeBodies const& parts,
                                NodeBody const& body,
                                ILogger& logger);

/// What a reload shapes its candidate by: the record the RUNNING body was adopted from, never the file.
///
/// A move saves its record on the formation's beat thread before the body it ends has drained, so the
/// file can be AHEAD of the body; a candidate shaped by it would describe a mode the node has not
/// entered, and be published as the configuration in force. A reload runs on the body's stop loop, the
/// thread `RunNodeBodies` adopts on between bodies, so @p running needs no lock.
/// @param running The record the running body was adopted from; must outlive the returned function.
/// @return The basis.
[[nodiscard]] KeptFormationReader RunningFormationOf(AdoptedFormation const& running);

} // namespace FastCache::Node
