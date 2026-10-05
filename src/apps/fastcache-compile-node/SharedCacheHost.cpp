// SPDX-License-Identifier: Apache-2.0
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/SharedCacheTarget.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Node
{

std::string OutlivedHostMessage(std::size_t borrowers)
{
    return std::format("fastcache-compile-node: the shared-cache host is being destroyed while {} reader(s) still "
                       "borrow it -- a private tier's in-process upstream outlived the host it reads. Whoever owns "
                       "both must construct the host first, so it is destroyed last (in main: declare the shared-cache "
                       "service before the cache tier); ending the process rather than freeing the shared tier under "
                       "a read\n",
                       borrowers);
}

void AbortOnOutlivedHost::Refuse(std::size_t borrowers) noexcept
{
    // A message that cannot be built throws out of a `noexcept` function, which terminates -- the
    // process still ends, which is the half that matters.
    std::fputs(OutlivedHostMessage(borrowers).c_str(), stderr);
    std::abort();
}

IOutlivedHostRefusal& DefaultOutlivedHostRefusal() noexcept
{
    static AbortOnOutlivedHost instance;
    return instance;
}

SharedCacheHost::SharedCacheHost(std::string selfId,
                                 ISharedTierOpener& opener,
                                 NodeConditions* conditions,
                                 ILogger& logger,
                                 ReconcileOn on,
                                 IOutlivedHostRefusal& refusal):
    _selfId { std::move(selfId) },
    _opener { opener },
    _conditions { conditions },
    _logger { logger },
    _refusal { refusal }
{
    // Evaluated at construction, so the row is never left `Undecided` on a node that runs
    // consensus: nothing is named until the cluster applies a state that says so.
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::SharedCacheUnavailable);
    _reported = _status;

    if (on == ReconcileOn::OwnThread)
        _reconciler = std::jthread { [this](std::stop_token const& stop) { Run(stop); } };
}

SharedCacheHost::~SharedCacheHost()
{
    // Before anything is torn down: a borrower still alive is an owner that destroyed this host
    // first, and every way on from here reads or frees under it.
    if (auto const held = _borrowers->load(std::memory_order_acquire); held != 0)
        _refusal.Refuse(held);

    // Joined BEFORE the tiers go, so no reconciliation is running while they are freed.
    if (_reconciler.joinable())
    {
        _reconciler.request_stop();
        _reconciler.join();
    }
}

void SharedCacheHost::Applied(Cluster::ClusterState const& state)
{
    auto const named = Cluster::ResolveSharedCache(state, _selfId).resolution == Cluster::SharedCacheResolution::ThisMachine;
    {
        std::scoped_lock const lock { _mutex };
        _wantServing = named;
        _dirty = true;
    }
    _wake.notify_one();
}

void SharedCacheHost::Reconcile()
{
    std::unique_lock lock { _mutex };
    _dirty = false;
    auto const want = _wantServing;

    if (want && _current == nullptr)
    {
        // Revive before opening: a retired tier still holds the store's exclusive claim, and a
        // second Open from this very process would be refused InUse.
        if (!_retired.empty())
        {
            _current = std::move(_retired.back());
            _retired.pop_back();
        }
        else
        {
            // Open may walk the store; meanwhile only `Current()` and `Applied()` wait on this
            // mutex, and neither should wait for a disk.
            lock.unlock();
            auto opened = _opener.Open();
            lock.lock();
            if (opened.has_value())
                _current = *std::move(opened);
            else
                _status.unavailable = std::move(opened).error();
        }
    }
    else if (!want && _current != nullptr)
        _retired.push_back(std::exchange(_current, nullptr));

    if (!want || _current != nullptr)
        _status.unavailable.clear();
    _status.named = want;
    _status.serving = _current != nullptr;

    // Freed HERE, on the reconciling thread, and only once nothing but this list holds it. Nothing
    // can copy a pointer that is no longer published, so a count of one cannot rise again. Taken
    // out under the lock and destroyed after it, so a store's final flush never blocks `Current()`.
    auto closing = std::vector<std::shared_ptr<SharedCacheTier>> {};
    std::erase_if(_retired, [&closing](std::shared_ptr<SharedCacheTier>& tier) {
        if (tier.use_count() != 1)
            return false;
        closing.push_back(std::move(tier));
        return true;
    });
    auto const report = _status;
    lock.unlock();

    closing.clear();
    Report(report);
}

std::shared_ptr<SharedCacheTier> SharedCacheHost::Current() const
{
    std::scoped_lock const lock { _mutex };
    return _current;
}

SharedCacheHost::Borrow::Borrow(SharedCacheHost const& host) noexcept:
    _host { host },
    _count { host._borrowers }
{
    _count->fetch_add(1, std::memory_order_acq_rel);
}

SharedCacheHost::Borrow::~Borrow()
{
    _count->fetch_sub(1, std::memory_order_acq_rel);
}

std::size_t SharedCacheHost::Borrowers() const noexcept
{
    return _borrowers->load(std::memory_order_acquire);
}

SharedCacheHostStatus SharedCacheHost::Status() const
{
    std::scoped_lock const lock { _mutex };
    return _status;
}

void SharedCacheHost::Run(std::stop_token const& stop)
{
    while (!stop.stop_requested())
    {
        {
            std::unique_lock lock { _mutex };
            // Bounded, so a retired tier whose last answer finished between applies still closes.
            std::ignore = _wake.wait_for(lock, stop, RetiredSweepInterval, [this] { return _dirty; });
        }
        if (!stop.stop_requested())
            Reconcile();
    }
}

void SharedCacheHost::Report(SharedCacheHostStatus const& status)
{
    if (_reported == status)
        return;
    auto const was = _reported.value_or(SharedCacheHostStatus {});
    _reported = status;

    if (status.serving && !was.serving)
        _logger.Log(LogLevel::Info, "shared cache: this machine is named; serving the fleet's shared tier");
    else if (status.named && !status.serving)
        _logger.Logf(LogLevel::Error, "shared cache: this machine is named and cannot serve it: {}", status.unavailable);
    else if (!status.named && was.named)
        _logger.Log(LogLevel::Info, "shared cache: no longer named; closing the tier once its answers finish");

    if (_conditions == nullptr)
        return;
    if (status.named && !status.serving)
        _conditions->Raise(NodeCondition::SharedCacheUnavailable, status.unavailable);
    else
        _conditions->Clear(NodeCondition::SharedCacheUnavailable);
}

} // namespace FastCache::Node
