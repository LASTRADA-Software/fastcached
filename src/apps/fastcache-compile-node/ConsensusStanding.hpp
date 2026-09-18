// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>

#include <mutex>
#include <optional>
#include <shared_mutex>

namespace FastCache::Node
{

/// Where this node sits in its own consensus configuration, asked when somebody asks (#1449).
///
/// A seam rather than a pointer to the tier, for the reason `LiveStatsSourceSlot` is one: the
/// `NodeStatus` surface that reports this is built before consensus, and a narrow question keeps
/// that surface from depending on everything a consensus tier is.
class IConsensusStandingSource
{
  public:
    IConsensusStandingSource() = default;
    IConsensusStandingSource(IConsensusStandingSource const&) = delete;
    IConsensusStandingSource(IConsensusStandingSource&&) = delete;
    IConsensusStandingSource& operator=(IConsensusStandingSource const&) = delete;
    IConsensusStandingSource& operator=(IConsensusStandingSource&&) = delete;
    virtual ~IConsensusStandingSource() = default;

    /// This node's standing in the configuration it operates under right now.
    ///
    /// The configuration CONSENSUS holds, never the replicated member record: the record says
    /// what an operator admitted a member as, and consensus moves towards it one change at a
    /// time, so the two can differ for as long as a change is replicating.
    /// @return The standing, or nullopt when there is nothing to ask -- nothing attached yet,
    ///         or this process is stopping.
    [[nodiscard]] virtual std::optional<Consensus::Standing> CurrentStanding() const = 0;
};

/// The consensus tier's standing, attached once the tier exists and detached before it goes.
///
/// **Why a slot rather than a constructor argument**, which is `LiveStatsSourceSlot`'s argument
/// one surface along: a node's `NodeStatus` answer is built before consensus, because consensus
/// is told the `0xFC` surface's bound endpoint, so the tier is attached after it exists and
/// **detached before it is destroyed**. The detach is the attachment's destructor, which makes
/// it a fact about declaration order rather than a line at one return path.
///
/// A detached slot answers nothing, so a `NodeStatus` asked while the process stops reports the
/// field absent rather than reading freed memory. The lock is shared by every reader and taken
/// exclusively only to attach and to detach.
class ConsensusStandingSlot final: public IConsensusStandingSource
{
  public:
    /// Holds a source attached; detaches it when destroyed.
    class Attachment
    {
      public:
        /// @param slot The slot to detach from.
        explicit Attachment(ConsensusStandingSlot& slot) noexcept:
            _slot { &slot }
        {
        }

        ~Attachment()
        {
            _slot->Detach();
        }

        Attachment(Attachment const&) = delete;
        Attachment(Attachment&&) = delete;
        Attachment& operator=(Attachment const&) = delete;
        Attachment& operator=(Attachment&&) = delete;

      private:
        ConsensusStandingSlot* _slot;
    };

    ConsensusStandingSlot() = default;

    /// Start answering from @p source, until the returned attachment is destroyed.
    ///
    /// Null is legal and attaches nothing, so a node that runs no consensus -- whose tier
    /// pointer is null -- is spelled the same way as one that does, with no `if` at the site.
    /// @param source What to ask; must outlive the attachment when non-null.
    /// @return The attachment. Declare it after @p source.
    [[nodiscard]] Attachment Attach(IConsensusStandingSource const* source)
    {
        {
            std::unique_lock const guard { _mutex };
            _source = source;
        }
        return Attachment { *this };
    }

    /// @return The attached source's answer, or nullopt when none is attached.
    [[nodiscard]] std::optional<Consensus::Standing> CurrentStanding() const override
    {
        std::shared_lock const guard { _mutex };
        return _source != nullptr ? _source->CurrentStanding() : std::nullopt;
    }

  private:
    /// Stop answering; returns only once no reader is inside the source.
    void Detach()
    {
        std::unique_lock const guard { _mutex };
        _source = nullptr;
    }

    mutable std::shared_mutex _mutex;
    IConsensusStandingSource const* _source { nullptr };
};

} // namespace FastCache::Node
