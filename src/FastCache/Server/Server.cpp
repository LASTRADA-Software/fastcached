// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/TlsWrap.hpp>
#include <FastCache/Server/Connection.hpp>
#include <FastCache/Server/Server.hpp>

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
    ///
    /// The body is wrapped in a catch-all firewall: this coroutine is a
    /// DetachedTask, whose `unhandled_exception` calls std::terminate, so an
    /// exception escaping a handler (e.g. std::bad_alloc while serving a large
    /// value) would take down the entire daemon and every other connection.
    /// We instead log it and drop only this connection.
    DetachedTask RunConnectionDetached(std::unique_ptr<Connection> connection, ILogger* logger, IAdmissionControl* admission)
    {
        try
        {
            co_await connection->Run();
        }
        catch (...)
        {
            LogConnectionFirewallException(*logger);
        }
        if (admission)
            admission->OnConnectionEnded();
        co_return;
    }

    /// Holds an `accepting` flag true for as long as an accept loop is running.
    ///
    /// RAII rather than a store beside each `co_return`, because the loop can also
    /// end by its coroutine frame being destroyed while suspended -- at which point
    /// a flag set by hand would go on claiming the acceptor is armed, and the
    /// readiness line it feeds would be a claim about a loop that no longer exists.
    class AcceptingScope
    {
      public:
        /// @param flag Set true now and false when this object dies.
        explicit AcceptingScope(std::atomic<bool>& flag) noexcept:
            _flag { flag }
        {
            _flag.store(true, std::memory_order_release);
        }
        AcceptingScope(AcceptingScope const&) = delete;
        AcceptingScope(AcceptingScope&&) = delete;
        AcceptingScope& operator=(AcceptingScope const&) = delete;
        AcceptingScope& operator=(AcceptingScope&&) = delete;
        ~AcceptingScope()
        {
            _flag.store(false, std::memory_order_release);
        }

      private:
        std::atomic<bool>& _flag;
    };

} // namespace

Server::Server(IListener& listener,
               CacheEngine& engine,
               ILogger& logger,
               IAdmissionControl* admission,
               IMetricsSink* metrics,
               SessionContext session,
               TlsContext* tls,
               LogSource logSource) noexcept:
    _listener { listener },
    _engine { engine },
    _logger { logger },
    _admission { admission },
    _metrics { metrics },
    _session { session },
    _tls { tls },
    _logSource { logSource }
{
}

Task<void> Server::Run()
{
    // Declared before the loop so it is already true at the first suspend: the
    // caller that started this coroutine resumes the moment `Accept()` parks, and
    // what it needs to know then is that the accept is registered rather than that
    // `Run()` was called. See `IsAccepting()`.
    AcceptingScope const accepting { _accepting };
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
        // count the rejection. We also bump a TLS-specific counter when
        // this Server is wrapping accepted sockets in TLS, so operators
        // can attribute traffic to plaintext vs TLS without a per-bind
        // metrics label dimension.
        if (_admission && !_admission->AllowAccept())
        {
            if (_metrics)
            {
                _metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected);
                if (_tls != nullptr)
                    _metrics->Increment(IMetricsSink::Counter::ConnectionsAdmissionRejectedTls);
            }
            (*accepted)->Close();
            continue;
        }

        if (_admission)
            _admission->OnConnectionStarted();
        _accepted.fetch_add(1, std::memory_order_relaxed);
        if (_metrics)
        {
            _metrics->Increment(IMetricsSink::Counter::ConnectionsTotal);
            if (_tls != nullptr)
                _metrics->Increment(IMetricsSink::Counter::ConnectionsTotalTls);
        }

        auto connection =
            std::make_unique<Connection>(WrapTls(std::move(*accepted), _tls), _engine, _logger, _session, _logSource);
        RunConnectionDetached(std::move(connection), &_logger, _admission);
    }
    co_return;
}

void Server::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
}

} // namespace FastCache
