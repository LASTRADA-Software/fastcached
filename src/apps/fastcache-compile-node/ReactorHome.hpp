// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include <core/net/EventLoop.hpp>

namespace FastCache::Node
{

/// The reactor a component's sockets belong to, and where they go when the component does.
///
/// **The rule is `core::net::EventLoop::TeardownIsSerialisedWithDispatch()`**: an object a reactor
/// owns is destroyed on that reactor's thread, or with that reactor stopped. A component that keeps
/// reactor objects between operations -- a socket, the timer armed on it -- cannot know which thread
/// destroys it, so it does not free them itself: it hands them to `Retire`, which DEFERS rather than
/// posts (`NodeIoLoop::Retire` says why a post-and-wait hangs). Folded into the component's
/// destructor, so no owner has a call to remember.
///
/// An interface rather than `NodeIoLoop` itself, because `NodeIoLoop.hpp` is the one node header
/// that includes the platform reactor, and a case can then count what was handed over.
class IReactorHome
{
  public:
    IReactorHome() = default;
    IReactorHome(IReactorHome const&) = delete;
    IReactorHome(IReactorHome&&) = delete;
    IReactorHome& operator=(IReactorHome const&) = delete;
    IReactorHome& operator=(IReactorHome&&) = delete;
    virtual ~IReactorHome() = default;

    /// @return The reactor every socket and timer of the component belongs to.
    [[nodiscard]] virtual core::net::EventLoop& Loop() noexcept = 0;

    /// Take ownership of something `Loop()` owns, and free it once that reactor has stopped.
    ///
    /// Safe from any thread. @p owned may be null, which retires nothing.
    /// @param owned What to hold until the reactor has stopped.
    virtual void Retire(std::shared_ptr<void> owned) = 0;
};

} // namespace FastCache::Node
