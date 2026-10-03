// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "SharedCacheHost.hpp"
#include "SharedCacheTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/NodeConditionWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/SharedTierFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::MemoryOpener;
using FastCache::Testing::NamingSharedCache;

namespace
{

/// Records a refusal and RETURNS, so the case survives to assert on it.
class RecordingRefusal final: public IOutlivedHostRefusal
{
  public:
    void Refuse(std::size_t borrowers) noexcept override
    {
        ++calls;
        lastBorrowers = borrowers;
    }

    std::size_t calls { 0 };         ///< How many times the host refused.
    std::size_t lastBorrowers { 0 }; ///< The count the last refusal named.
};

} // namespace

TEST_CASE("A shared-cache host destroyed under a live borrower refuses, naming how many", "[node][shared-cache][host]")
{
    // Production ends the process here; the seam is what lets a case watch it decide. Two borrowers
    // rather than one, so a refusal that named a constant would not pass.
    RecordingRefusal refusal;
    MemoryOpener opener;
    NullLogger logger;
    std::optional<SharedCacheHost::Borrow> first;
    std::optional<SharedCacheHost::Borrow> second;
    std::optional<SharedCacheHost> host;
    host.emplace("cache-c", opener, nullptr, logger, ReconcileOn::Caller, refusal);
    first.emplace(*host);
    second.emplace(*host);

    host.reset();
    CHECK(refusal.calls == 1);
    CHECK(refusal.lastBorrowers == 2);

    // Released after the host is gone: the count is shared, so this touches nothing freed.
    first.reset();
    second.reset();
    CHECK(refusal.calls == 1);
}

TEST_CASE("A shared-cache host its borrowers let go of first is destroyed without a refusal", "[node][shared-cache][host]")
{
    RecordingRefusal refusal;
    MemoryOpener opener;
    NullLogger logger;
    std::optional<SharedCacheHost> host;
    host.emplace("cache-c", opener, nullptr, logger, ReconcileOn::Caller, refusal);
    {
        SharedCacheHost::Borrow const borrow { *host };
        CHECK(host->Borrowers() == 1);
    }
    host.reset();
    CHECK(refusal.calls == 0);
}

TEST_CASE("The refusal to destroy a borrowed shared-cache host names the count and the remedy", "[node][shared-cache][host]")
{
    auto const message = OutlivedHostMessage(3);
    CHECK(message.contains("while 3 reader(s) still borrow it"));
    CHECK(message.contains("construct the host first"));
    CHECK(message.contains("declare the shared-cache service before the cache tier"));
    // The newline is what delivers the line before `std::abort` on a line-buffered stderr.
    CHECK(message.ends_with('\n'));
}

TEST_CASE("The shared-cache host serves only while the state names this machine", "[node][shared-cache][host]")
{
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    CHECK(host.Current() == nullptr);

    host.Applied(NamingSharedCache("cache-c"));
    // Desire is recorded at apply; nothing opens until the reconciler runs.
    CHECK(opener.opens == 0);
    host.Reconcile();
    CHECK(host.Current() != nullptr);
    CHECK(host.Status().serving);

    host.Applied(NamingSharedCache("cache-d"));
    host.Reconcile();
    CHECK(host.Current() == nullptr);
    CHECK_FALSE(host.Status().named);

    host.Applied(NamingSharedCache(""));
    host.Reconcile();
    CHECK(host.Current() == nullptr);
    CHECK(opener.opens == 1);
}

TEST_CASE("A shared tier named again while still held is revived, not opened twice", "[node][shared-cache][host]")
{
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    auto const inFlight = host.Current(); // an answer that has not finished

    host.Applied(NamingSharedCache("cache-d"));
    host.Reconcile();
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();

    // The same object: a second Open against a file this process still holds would be refused InUse.
    CHECK(host.Current() == inFlight);
    CHECK(opener.opens == 1);
}

TEST_CASE("A retired shared tier closes on the reconciling thread once its last answer lets go",
          "[node][shared-cache][host]")
{
    MemoryOpener opener;
    NullLogger logger;
    SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::Caller };
    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();

    auto inFlight = host.Current();
    std::weak_ptr<SharedCacheTier> const watch = inFlight;
    host.Applied(NamingSharedCache("cache-d"));
    host.Reconcile();
    CHECK_FALSE(watch.expired()); // still answering

    // The answer finishes -- on a reactor, in production. Dropping the last holder there must NOT be
    // what frees the store: the host still holds it retired.
    std::thread reactor { [held = std::move(inFlight)]() mutable { held.reset(); } };
    reactor.join();
    CHECK_FALSE(watch.expired());
    CHECK(opener.closedOn == std::thread::id {});

    host.Reconcile();
    CHECK(watch.expired());
    CHECK(opener.closedOn == std::this_thread::get_id());
}

TEST_CASE("A shared tier that will not open raises its condition and stays dormant",
          "[node][shared-cache][host][conditions]")
{
    MemoryOpener opener;
    opener.fail = true;
    NullLogger logger;
    NodeConditions conditions;
    SharedCacheHost host { "cache-c", opener, &conditions, logger, ReconcileOn::Caller };
    // Evaluated from the start: a node that runs consensus never reports this row undecided.
    CHECK(conditions.StateOf(NodeCondition::SharedCacheUnavailable) == CompileCacheWire::ConditionState::Clear);

    host.Applied(NamingSharedCache("cache-c"));
    host.Reconcile();
    CHECK(host.Current() == nullptr);
    CHECK(host.Status().named);
    CHECK(host.Status().unavailable.contains("another process"));
    CHECK(conditions.StateOf(NodeCondition::SharedCacheUnavailable) == CompileCacheWire::ConditionState::Raised);

    // Moving the setting away clears it: a Live row describes now.
    host.Applied(NamingSharedCache("cache-d"));
    host.Reconcile();
    CHECK(conditions.StateOf(NodeCondition::SharedCacheUnavailable) == CompileCacheWire::ConditionState::Clear);
}

TEST_CASE("The shared-cache host's own thread opens the tier an apply names and stops with the host",
          "[node][shared-cache][host]")
{
    // The production mode, once: the apply callback records and wakes, and the store is opened on
    // the host's OWN thread -- never on the thread that applied, which in production is consensus's
    // apply callback, where a store walked while opening is a voter missing heartbeats.
    MemoryOpener opener;
    NullLogger logger;
    auto served = false;
    {
        SharedCacheHost host { "cache-c", opener, nullptr, logger, ReconcileOn::OwnThread };
        host.Applied(NamingSharedCache("cache-c"));
        served = Testing::WaitUntil(
            "the host's thread to open the tier the apply named",
            [&host] { return host.Current() != nullptr; },
            [&host] { return std::format("named {}, serving {}", host.Status().named, host.Status().serving); });
    }
    CHECK(served);
    CHECK(opener.opens == 1);
    CHECK(opener.openedOn != std::thread::id {});
    CHECK(opener.openedOn != std::this_thread::get_id());
}
