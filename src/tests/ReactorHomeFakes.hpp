// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include <apps/fastcache-compile-node/ReactorHome.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Testing
{

/// @file ReactorHomeFakes.hpp
/// A reactor home for a component whose sockets belong to NO reactor -- a case driving it through
/// `core::async::syncRun` over a blocking connector -- since a home is a required reference and
/// null is not a spelling.

/// A home over a `TestLoop` nobody turns: a timer armed on it never fires, and what is retired to it
/// is held until the home goes -- declared after the loop, so freed before it, which is the "with
/// that reactor stopped" arm of `TeardownIsSerialisedWithDispatch` by construction.
///
/// Right for a case whose sockets are blocking ones, where nothing is timed on a reactor and a kept
/// session ages out by the pool's own clock alone. A case that means the idle timer to FIRE runs on
/// a real loop (`NodeIoLoop`) instead.
class UnturnedLoopHome final: public Node::IReactorHome
{
  public:
    UnturnedLoopHome() = default;

    [[nodiscard]] core::net::EventLoop& Loop() noexcept override
    {
        return _loop;
    }

    void Retire(std::shared_ptr<void> owned) override
    {
        if (owned != nullptr)
            _retired.push_back(std::move(owned));
    }

  private:
    core::platform::ManualClock _clock;
    core::net::testing::TestLoop _loop { _clock };
    std::vector<std::shared_ptr<void>> _retired; ///< Declared after `_loop`: freed while it still exists.
};

} // namespace FastCache::Testing
