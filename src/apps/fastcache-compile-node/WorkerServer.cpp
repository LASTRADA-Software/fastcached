// SPDX-License-Identifier: Apache-2.0
#include "WorkerServer.hpp"

#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/Framing/LineReader.hpp>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Largest request a worker will buffer; declared in the header because
    /// `WorkerProtocol` is handed the same figure. See `WorkerMaxRequestBytes`.
    constexpr std::size_t MaxRequestBytes = WorkerMaxRequestBytes;

    /// How many payload bytes all the jobs on this worker may be reading at once.
    ///
    /// **This had to land with the detach, not after it.** Serving one compile at a
    /// time bounded peak memory to a single `MaxRequestBytes` by accident; serving
    /// `slots` of them makes it `slots` times that -- 8 GiB on a 32-slot node, asked
    /// for by any cluster member, which is a memory-exhaustion hole opened by a fix.
    /// `FrameEndpoint` records the same lesson for the node's other framed surfaces.
    ///
    /// Deliberately ONE request's worth, which is the same figure the cache
    /// responder chose and for the same reason: ordinary translation units are a few
    /// megabytes, so dozens run side by side, while a single 256 MiB monster cannot
    /// be joined by a second one.
    constexpr std::size_t MaxInFlightBytes = MaxRequestBytes;

    /// Take `want` bytes from `counter` without exceeding `budget`.
    /// @param counter The shared in-flight total.
    /// @param want How many bytes this job declared.
    /// @param budget The ceiling.
    /// @return Whether the reservation was made.
    [[nodiscard]] bool TryReserve(std::atomic<std::size_t>& counter, std::size_t want, std::size_t budget)
    {
        auto current = counter.load(std::memory_order_acquire);
        while (current + want <= budget)
            if (counter.compare_exchange_weak(current, current + want, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        return false;
    }

    /// Gives a reservation back however its scope ends.
    class ReservedBytes
    {
      public:
        /// @param counter The shared in-flight total; must outlive this.
        /// @param bytes How many were reserved.
        ReservedBytes(std::atomic<std::size_t>& counter, std::size_t bytes) noexcept:
            _counter { &counter },
            _bytes { bytes }
        {
        }
        ~ReservedBytes()
        {
            _counter->fetch_sub(_bytes, std::memory_order_acq_rel);
        }
        ReservedBytes(ReservedBytes const&) = delete;
        ReservedBytes& operator=(ReservedBytes const&) = delete;
        ReservedBytes(ReservedBytes&&) = delete;
        ReservedBytes& operator=(ReservedBytes&&) = delete;

      private:
        std::atomic<std::size_t>* _counter;
        std::size_t _bytes;
    };

    /// Write every byte, or report failure.
    [[nodiscard]] Task<bool> WriteAll(ISocket* socket, std::span<std::byte const> bytes)
    {
        auto const written = co_await socket->Write(bytes);
        // The byte count is checked, not merely the call: a short write leaves the
        // client blocked on a length the reply promised and never delivered.
        co_return written.has_value() && *written == bytes.size();
    }
} // namespace

WorkerServer::WorkerServer(IListener& listener,
                           Cc::WorkerProtocol& protocol,
                           std::size_t slots,
                           Distributed::IMembershipOracle const& membership,
                           IMetricsSink& metrics,
                           ILogger& logger,
                           IExecutor& jobs) noexcept:
    _listener { listener },
    _jobs { jobs },
    _protocol { protocol },
    _slots { slots },
    _membership { membership },
    _metrics { metrics },
    _logger { logger }
{
}

WorkerServer::~WorkerServer()
{
    // Close the door before counting who is still inside. Draining without this
    // races the accept loop: it can admit one more job just as the count reaches
    // zero, and the wait then returns while that job is starting.
    Shutdown();

    // Every job holds a slot until it ends, so zero means nothing is still running
    // on the executor with a pointer into this object.
    auto guard = std::unique_lock { _drainMutex };
    _drained.wait(guard, [this] { return _inFlight.load(std::memory_order_acquire) == 0; });
}

void WorkerServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

std::size_t WorkerServer::InFlight() const noexcept
{
    return _inFlight.load(std::memory_order_acquire);
}

Task<void> WorkerServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll timeout is how the loop wakes to observe Shutdown() on POSIX,
            // where Close() does not unblock a parked accept(). Not a failure.
            auto const code = accepted.error().code;
            if (code == NetErrorCode::WouldBlock || code == NetErrorCode::Timeout)
                continue;
            _logger.Logf(LogLevel::Debug, "worker: accept loop ended ({})", accepted.error().ToString());
            co_return;
        }

        auto socket = *std::move(accepted);

        // A connection accepted just as shutdown began is dropped rather than
        // admitted. The loop condition is checked before `Accept()` parks, so
        // without this the last admission can land after the drain started.
        if (_shuttingDown.load(std::memory_order_acquire))
        {
            socket->Close();
            co_return;
        }

        // Anti-leeching, and it comes FIRST -- before the slot cap, before the
        // payload -- for the reason the cap itself comes before the payload: a
        // caller with no claim on this machine must not be able to make it buffer a
        // multi-megabyte preprocessed translation unit, which would be a
        // memory-exhaustion hole reachable by exactly the peers this check exists to
        // keep out. The same ordering `CompileCacheHandler`'s auth gate documents.
        //
        // This machine and this cluster's members. Everyone else is refused as a
        // *reply* rather than by closing, so a misconfigured peer learns which of
        // the two it is instead of seeing a connection it cannot tell from a dead
        // host. Without this the port accepted anybody who could route to it and ran
        // their compiler for them: `--bind` defaults to 0.0.0.0, so that was the
        // network.
        if (_membership.Classify(socket->PeerAddress()) != Distributed::Membership::Member)
        {
            _metrics.Increment(IMetricsSink::Counter::WorkerJobsRefusedNotAMember);
            (void) co_await WriteAll(socket.get(),
                                     Wire::EncodeErrorReply(Wire::ErrorCode::NotAMember,
                                                            "this worker compiles for its own machine and its cluster"));
            socket->Close();
            continue;
        }

        // The cap is checked before the request is read, so an over-capacity client
        // is refused without this worker buffering its multi-megabyte payload
        // first. Refused, never queued: queueing hides the overload from the
        // scheduler that is trying to route around it, and the client has a local
        // compile waiting either way.
        auto const before = _inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (before >= _slots)
        {
            _inFlight.fetch_sub(1, std::memory_order_acq_rel);
            _metrics.Increment(IMetricsSink::Counter::WorkerJobsRefusedNoSlot);
            (void) co_await WriteAll(socket.get(), Wire::EncodeErrorReply(Wire::ErrorCode::NoCapacity, {}));
            socket->Close();
            continue;
        }

        // Detached, and the loop goes straight back to accepting. Served inline --
        // as this was until #213 -- a worker advertising thirty slots ran exactly
        // one compile at a time: `_inFlight` never exceeded 1, the cap above was
        // unreachable, and a saturated fleet reported `1 / 30 compiling`.
        //
        // The cap is what keeps the cores from being oversubscribed, which is the
        // job the inline version was doing by accident. It is checked ABOVE, before
        // the payload is read, so nothing about that ordering changes.
        //
        // Guarded because the slot is taken before the job exists: a coroutine frame
        // this process cannot allocate would leave the count raised with nothing
        // that will ever lower it, and the destructor's wait is deliberately
        // unbounded. The one path that could make that wait never return.
        try
        {
            ServeDetached(std::move(socket));
        }
        catch (...)
        {
            ReleaseSlot();
            _logger.Logf(LogLevel::Error, "worker: could not start a job; the connection is dropped");
        }
    }
    co_return;
}

void WorkerServer::ReleaseSlot() noexcept
{
    // Under the lock, notify included. The destructor can only wake by taking this
    // same mutex, so it cannot run ahead and free the members while this thread is
    // still inside them -- which is the whole reason this is a condition variable
    // rather than `_inFlight.wait()`, and the reason it is one function rather than
    // the two call sites each spelling it out.
    auto const guard = std::scoped_lock { _drainMutex };
    _inFlight.fetch_sub(1, std::memory_order_acq_rel);
    _drained.notify_all();
}

DetachedTask WorkerServer::ServeDetached(std::unique_ptr<ISocket> socket)
{
    try
    {
        co_await Serve(std::move(socket));
    }
    catch (...)
    {
        // A `DetachedTask`'s `unhandled_exception` calls `std::terminate`, so an
        // exception escaping one client's compile would take the worker and every
        // other compile on it. It costs this client instead.
        _logger.Logf(LogLevel::Error, "worker: dropping a connection after an exception while serving it");
    }

    // On every path out, including the firewall above: a slot leaked once is a
    // worker that reports itself permanently busier than it is, and the scheduler
    // takes it out of rotation silently.
    ReleaseSlot();
    co_return;
}

Task<void> WorkerServer::Serve(std::unique_ptr<ISocket> socket)
{
    // Onto the executor before anything is read, so the accept loop is free the
    // moment a job is admitted. A compile spawns a process and blocks for seconds;
    // it cannot run here and it cannot run on a reactor, which is what an executor
    // sized to the slot cap is for.
    //
    // Everything after this line stays on that thread, and that depends on the
    // worker's listener being a BLOCKING one: `BlockingSocket::Read` does its
    // `recv` eagerly and hands back an already-ready awaitable, so no `co_await`
    // below suspends. Put this port on a reactor and each read would resume the
    // coroutine on the reactor thread instead -- and `_protocol.Answer`, the actual
    // compile, would run there. That is #213 again, one layer over and much harder
    // to see, because the hop back is invisible at every call site.
    co_await ResumeOn { _jobs };

    ByteReader reader { *socket, /*maxLineBytes*/ 1, MaxRequestBytes };
    auto const header = co_await reader.ReadExactly(Wire::RequestHeaderSize);
    if (header.has_value())
    {
        auto const decoded = Wire::DecodeRequestHeader(*header);
        if (decoded.has_value() && decoded->payloadLength <= MaxRequestBytes)
        {
            // Checked on the DECLARED length, before a payload byte is read, so an
            // over-budget request costs no allocation at all. The slot cap alone
            // does not bound memory: `slots` jobs each declaring the per-request
            // maximum is still `slots` times it.
            if (!TryReserve(_bytesInFlight, decoded->payloadLength, MaxInFlightBytes))
            {
                _metrics.Increment(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy);
                // Its own code, not NoCapacity: this says "come back shortly", while
                // NoCapacity says "the fleet is full". An operator sent to buy
                // machines over a transient byte budget is being sent to fix
                // something that was never wrong.
                (void) co_await WriteAll(socket.get(), Wire::EncodeErrorReply(Wire::ErrorCode::EndpointBusy, {}));
                socket->Close();
                co_return;
            }
            ReservedBytes const reserved { _bytesInFlight, decoded->payloadLength };

            auto const payload = co_await reader.ReadExactly(decoded->payloadLength);
            if (payload.has_value())
            {
                std::vector<std::byte> frame { header->begin(), header->end() };
                frame.insert(frame.end(), payload->begin(), payload->end());

                // Counted at the socket, which is what "bytes received" means
                // to an operator sizing a link: the payload as it arrived, not
                // what it decompressed to.
                _metrics.Increment(IMetricsSink::Counter::WorkerBytesReceived, static_cast<std::uint64_t>(frame.size()));

                if (auto const reply = _protocol.Answer(frame); reply.has_value())
                {
                    _metrics.Increment(IMetricsSink::Counter::WorkerBytesReturned,
                                       static_cast<std::uint64_t>(reply->size()));
                    (void) co_await WriteAll(socket.get(), *reply);
                }
                // No reply means a foreign magic: the peer is not speaking this
                // protocol, and there is no framing in which an answer would be
                // meaningful. Closing is the only thing left.
            }
        }
        else if (decoded.has_value())
            (void) co_await WriteAll(socket.get(), Wire::EncodeErrorReply(Wire::ErrorCode::PayloadTooLarge, {}));
    }
    socket->Close();
    co_return;
}

} // namespace FastCache::Node
