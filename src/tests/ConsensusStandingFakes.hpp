// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <mutex>
#include <optional>

#include <apps/fastcache-compile-node/ConsensusStanding.hpp>

/// @file ConsensusStandingFakes.hpp
/// A node's consensus, as a case scripts what it says about itself.
///
/// Shared rather than written per file, for `MembershipFakes.hpp`'s reason: a WRONG fake makes its
/// cases pass. It reaches an app header, so `src/FastCache` tests must not include it.
namespace FastCache::Testing
{

/// A consensus tier whose applied state is as caught up as a case says.
///
/// The reading is REQUIRED and undefaulted: which reading a node's proof surface sees is the case's
/// fact to state, and a fake defaulting to `CaughtUp` would let a case asserting `NodeKeyUnknown`
/// pass without ever saying the state had caught up -- the one condition under which that refusal
/// is the right one.
class ScriptedAppliedState final: public Node::IConsensusStandingSource
{
  public:
    /// @param reading What every question is answered with until the case changes it.
    explicit ScriptedAppliedState(Node::AppliedStateReading reading) noexcept:
        _reading { reading }
    {
    }

    /// No standing: the cases this fake serves are about the applied state, never the seat.
    [[nodiscard]] std::optional<Consensus::Standing> CurrentStanding() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] Node::AppliedStateReading CurrentAppliedState() const override
    {
        std::scoped_lock const lock { _lock };
        return _reading;
    }

    /// Answer @p reading from now on: a node whose consensus caught up, or restarted.
    /// @param reading The new reading.
    void Set(Node::AppliedStateReading reading)
    {
        std::scoped_lock const lock { _lock };
        _reading = reading;
    }

  private:
    mutable std::mutex _lock;
    Node::AppliedStateReading _reading;
};

} // namespace FastCache::Testing
