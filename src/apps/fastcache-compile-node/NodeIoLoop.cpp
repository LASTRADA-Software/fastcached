// SPDX-License-Identifier: Apache-2.0
#include "FrameEndpoint.hpp"
#include "NodeIoLoop.hpp"

#include <FastCache/Async/Task.hpp>

#include <utility>

namespace FastCache::Node
{

namespace
{

    /// Run one adopted server's accept loop, then tell the owner it has ended.
    ///
    /// A free function taking raw pointers rather than a capturing lambda: a
    /// coroutine's closure outlives the expression that created it, so captures are
    /// a use-after-free waiting to happen -- the shape `RaftPeerServer::ServePeer`
    /// already uses for the same reason.
    ///
    /// The `try` is a firewall. This is a `DetachedTask`, whose unhandled_exception
    /// terminates the process, and one surface throwing must not take the node with
    /// it -- the same reasoning `ReactorServerLoop` applies per connection.
    /// @param server The accept loop to run.
    /// @param owner Told when it ends, so the last loop can stop the reactor.
    DetachedTask RunAdopted(FrameServer* server, NodeIoLoop* owner)
    {
        try
        {
            co_await server->Run();
        }
        catch (...)
        {
            server->NoteLoopThrew();
        }
        owner->NoteLoopFinished();
        co_return;
    }

} // namespace

NodeIoLoop::NodeIoLoop() = default;

NodeIoLoop::~NodeIoLoop() = default;

void NodeIoLoop::Adopt(FrameServer& server)
{
    _loops.push_back(&server);
}

void NodeIoLoop::Start()
{
    // Spawned on the CALLING thread, before the reactor thread exists. Each is a
    // DetachedTask, so it begins eagerly and arms its first `Accept()` here -- which
    // is the point: a client that dials the moment this returns must not find a
    // listener nobody is accepting on.
    _loopsRunning.store(_loops.size(), std::memory_order_release);
    for (auto* server: _loops)
        RunAdopted(server, this);

    // Nothing adopted means nothing to run and nothing that could stop the reactor,
    // so `Run()` would never return. Starting no thread at all is the only correct
    // answer, and it is what a node with neither framed surface enabled gets.
    if (_loops.empty())
        return;

    _thread = std::jthread { [this] { _reactor.Run(); } };
}

void NodeIoLoop::NoteLoopStarted() noexcept
{
    _loopsRunning.fetch_add(1, std::memory_order_acq_rel);
}

void NodeIoLoop::NoteLoopFinished() noexcept
{
    if (_loopsRunning.fetch_sub(1, std::memory_order_acq_rel) == 1)
        _reactor.Stop();
}

} // namespace FastCache::Node
