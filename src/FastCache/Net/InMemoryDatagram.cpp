// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InMemoryDatagram.hpp>

#include <algorithm>
#include <utility>

namespace FastCache
{

/// One endpoint on a `DatagramBus`.
class InMemoryDatagramSocket final: public IDatagramSocket
{
  public:
    /// Attach to @p bus at @p endpoint.
    /// @param bus The segment.
    /// @param endpoint This socket's address.
    InMemoryDatagramSocket(DatagramBus& bus, std::string endpoint):
        _bus { bus },
        _endpoint { std::move(endpoint) }
    {
        _bus.Attach(_endpoint);
    }

    ~InMemoryDatagramSocket() override
    {
        _bus.Detach(_endpoint);
    }

    InMemoryDatagramSocket(InMemoryDatagramSocket const&) = delete;
    InMemoryDatagramSocket(InMemoryDatagramSocket&&) = delete;
    InMemoryDatagramSocket& operator=(InMemoryDatagramSocket const&) = delete;
    InMemoryDatagramSocket& operator=(InMemoryDatagramSocket&&) = delete;

    std::expected<void, NetError> Send(std::span<std::byte const> payload, std::string_view to) override
    {
        _bus.Deliver(payload, to, _endpoint);
        return {};
    }

    std::expected<ReceivedDatagram, DatagramWait> Receive(std::chrono::milliseconds timeout) override
    {
        std::unique_lock lock { _bus._mutex };

        auto& inbox = _bus._inboxes[_endpoint];
        auto const ready = [&inbox] {
            return inbox.closed || !inbox.queue.empty();
        };

        if (!_bus._arrived.wait_for(lock, timeout, ready))
            return std::unexpected { DatagramWait::TimedOut };

        // Closed wins over a queued datagram: a socket told to stop should stop,
        // and draining first would make shutdown depend on how much happened to
        // be in flight.
        if (inbox.closed)
            return std::unexpected { DatagramWait::Closed };

        auto datagram = std::move(inbox.queue.front());
        inbox.queue.pop_front();
        return datagram;
    }

    void Close() noexcept override
    {
        {
            std::scoped_lock const lock { _bus._mutex };
            _bus._inboxes[_endpoint].closed = true;
        }
        _bus._arrived.notify_all();
    }

    [[nodiscard]] std::string BoundEndpoint() const override
    {
        return _endpoint;
    }

  private:
    DatagramBus& _bus;
    std::string _endpoint;
};

std::unique_ptr<IDatagramSocket> DatagramBus::Open(std::string endpoint)
{
    return std::make_unique<InMemoryDatagramSocket>(*this, std::move(endpoint));
}

void DatagramBus::Attach(std::string const& endpoint)
{
    std::scoped_lock const lock { _mutex };
    _inboxes[endpoint];
}

void DatagramBus::Detach(std::string const& endpoint)
{
    std::scoped_lock const lock { _mutex };
    _inboxes.erase(endpoint);
}

void DatagramBus::DropNext(std::string_view endpoint, std::size_t count)
{
    std::scoped_lock const lock { _mutex };
    _inboxes[std::string { endpoint }].dropsRemaining += count;
}

std::size_t DatagramBus::SendCount() const
{
    std::scoped_lock const lock { _mutex };
    return _sendCount;
}

void DatagramBus::Deliver(std::span<std::byte const> payload, std::string_view to, std::string_view from)
{
    {
        std::scoped_lock const lock { _mutex };
        ++_sendCount;

        // A broadcast reaches every inbox INCLUDING the sender's, which is what a
        // real one does and is exactly the case `PeerDirectory` has to ignore: a
        // node that recorded its own beacon would believe it had a peer and
        // propose a membership change to admit itself.
        auto const deliverTo = [&](Inbox& inbox) {
            if (inbox.dropsRemaining != 0)
            {
                --inbox.dropsRemaining;
                return;
            }
            inbox.queue.push_back(
                ReceivedDatagram { .payload = { payload.begin(), payload.end() }, .from = std::string { from } });
        };

        if (to == BroadcastAddress)
            for (auto& [address, inbox]: _inboxes)
                deliverTo(inbox);
        else if (auto found = _inboxes.find(std::string { to }); found != _inboxes.end())
            deliverTo(found->second);
        // A datagram to an endpoint nobody holds is silently discarded, which is
        // what UDP does. Reporting it would give the layer above a delivery
        // signal the real network cannot provide.
    }
    _arrived.notify_all();
}

} // namespace FastCache
