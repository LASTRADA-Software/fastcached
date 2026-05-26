// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/Server.hpp>

#include <FastCache/Server/Connection.hpp>

#include <memory>
#include <utility>

namespace FastCache
{

namespace
{

    /// Per-connection coroutine. Self-owning via DetachedTask; the unique_ptr
    /// ensures the Connection object is freed when the coroutine ends.
    /// Admission and metrics observers are passed by raw pointer (may be
    /// null) so Connection cleanup decrements the admission gate.
    DetachedTask RunConnectionDetached(std::unique_ptr<Connection> connection,
                                       IAdmissionControl* admission)
    {
        co_await connection->Run();
        if (admission)
            admission->OnConnectionEnded();
        co_return;
    }

} // namespace

Server::Server(IListener& listener,
               CacheEngine& engine,
               ILogger& logger,
               IAdmissionControl* admission,
               IMetricsSink* metrics) noexcept:
    _listener { listener },
    _engine { engine },
    _logger { logger },
    _admission { admission },
    _metrics { metrics }
{
}

Task<void> Server::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            _logger.Logf(LogLevel::Debug, "Server: accept ended ({})", accepted.error().ToString());
            co_return;
        }

        // Admission control: refuse if the cap is full. We still accepted
        // the socket (the OS already did the SYN-ACK), so close it and
        // count the rejection.
        if (_admission && !_admission->AllowAccept())
        {
            if (_metrics)
                _metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected);
            (*accepted)->Close();
            continue;
        }

        if (_admission)
            _admission->OnConnectionStarted();
        _accepted.fetch_add(1, std::memory_order_relaxed);
        if (_metrics)
            _metrics->Increment(IMetricsSink::Counter::ConnectionsTotal);

        auto connection = std::make_unique<Connection>(std::move(*accepted), _engine, _logger);
        RunConnectionDetached(std::move(connection), _admission);
    }
    co_return;
}

void Server::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

} // namespace FastCache
