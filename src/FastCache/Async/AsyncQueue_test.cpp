// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <cstddef>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <tests/FrameSentinel.hpp>
#include <tests/Unwrap.hpp>

namespace
{

using Queue = FastCache::AsyncQueue<int>;

/// Consumes until the queue closes, recording every value it saw. The shape
/// every real consumer has, so the tests exercise the same path production does.
FastCache::Task<void> Consume(Queue* queue, std::vector<int>* seen, bool* ended)
{
    while (true)
    {
        auto item = co_await queue->Pop();
        if (!item.has_value())
            break;
        seen->push_back(*item);
    }
    *ended = true;
    co_return;
}

} // namespace

TEST_CASE("A push while the consumer is parked does not resume it inline", "[async][queue]")
{
    // THE invariant. A producer commonly pushes while holding a lock of its own
    // -- Raft's driver mutex is the case this type was built for -- so a queue
    // that resumed the consumer from inside Push would run the consumer's next
    // step under that lock, on the producer's thread, and deadlock the moment the
    // consumer touched the producer back.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();
    REQUIRE(queue.HasWaiter());

    auto const pushed = queue.Push(7);
    CHECK(pushed.accepted);
    CHECK(pushed.displaced == 0);

    // Not yet: the handle went to the reactor, not to a resume().
    CHECK(seen.empty());
    CHECK(reactor.PendingSubmissions() == 1);

    reactor.Drain();
    REQUIRE(seen == std::vector { 7 });

    queue.Close();
    reactor.Drain();
    CHECK(ended);
    CHECK_FALSE(queue.HasWaiter());
}

TEST_CASE("A pop with an item already queued does not suspend", "[async][queue]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    CHECK(queue.Push(1).accepted);
    CHECK(queue.Push(2).accepted);

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();

    // Both taken without ever parking, and then parked on the empty queue.
    CHECK(seen == std::vector { 1, 2 });
    CHECK(queue.HasWaiter());

    queue.Close();
    reactor.Drain();
    CHECK(ended);
}

TEST_CASE("Close wakes a parked consumer at once and discards what is held", "[async][queue]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();

    CHECK(queue.Push(1).accepted);
    CHECK(queue.Push(2).accepted);
    // Close before the consumer runs: what is queued is dropped rather than
    // drained, so teardown does not depend on the depth or on the consumer's
    // backpressure.
    queue.Close();
    reactor.Drain();

    CHECK(seen.empty());
    CHECK(ended);
    CHECK(queue.Size() == 0);
    CHECK(queue.IsClosed());
}

TEST_CASE("A push after Close is refused and is not counted as displaced", "[async][queue]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    queue.Close();
    auto const pushed = queue.Push(1);
    CHECK_FALSE(pushed.accepted);
    // Refused is not the same fact as displaced: one says the queue is gone, the
    // other that it was full, and an operator does something different about each.
    CHECK(pushed.displaced == 0);
    CHECK(queue.Displaced() == 0);
}

TEST_CASE("DropOldest keeps the newest and reports what it displaced", "[async][queue]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor,
                  FastCache::AsyncQueueOptions { .capacity = 2, .overflow = FastCache::AsyncQueueOverflow::DropOldest } };

    CHECK(queue.Push(1).displaced == 0);
    CHECK(queue.Push(2).displaced == 0);
    auto const third = queue.Push(3);
    CHECK(third.accepted);
    CHECK(third.displaced == 1);
    CHECK(queue.Size() == 2);
    CHECK(queue.Displaced() == 1);

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();
    // The oldest went, and order is otherwise preserved.
    CHECK(seen == std::vector { 2, 3 });

    queue.Close();
    reactor.Drain();
}

TEST_CASE("DropNewest refuses the push instead", "[async][queue]")
{
    // The other row of the table, so the enum is not one used value and one
    // decorative one.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor,
                  FastCache::AsyncQueueOptions { .capacity = 2, .overflow = FastCache::AsyncQueueOverflow::DropNewest } };

    CHECK(queue.Push(1).accepted);
    CHECK(queue.Push(2).accepted);
    auto const third = queue.Push(3);
    CHECK_FALSE(third.accepted);
    CHECK(third.displaced == 1);
    CHECK(queue.Size() == 2);

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();
    CHECK(seen == std::vector { 1, 2 });

    queue.Close();
    reactor.Drain();
}

TEST_CASE("A push landing between await_ready and await_suspend does not park", "[async][queue]")
{
    // `await_ready` and `await_suspend` are two separate acquisitions of the
    // mutex, so a producer can slip in between them. Parking then would be a park
    // nothing ever wakes: the push that would have woken it has already happened
    // and already found no waiter. The awaiter is driven directly here because
    // there is no other way to be inside that window.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    auto awaiter = queue.Pop();
    REQUIRE_FALSE(awaiter.await_ready());

    queue.Push(42);

    // false == "do not suspend, resume through the normal path".
    CHECK_FALSE(awaiter.await_suspend(std::noop_coroutine()));
    CHECK_FALSE(queue.HasWaiter());

    auto const item = awaiter.await_resume();
    REQUIRE(item.has_value());
    CHECK(FastCache::Testing::Unwrap(item) == 42);
}

TEST_CASE("A producer on another thread reaches a consumer on the reactor", "[async][queue]")
{
    // The headline property, and the reason TestReactor had to be given the mutex
    // its interface already promised: a hand-off from a thread that is not the
    // reactor's is the whole point of this type.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    Queue queue { reactor, FastCache::AsyncQueueOptions {} };

    std::vector<int> seen;
    auto ended = false;
    auto consumer = Consume(&queue, &seen, &ended);
    reactor.Submit(consumer.Native());
    reactor.Drain();
    REQUIRE(queue.HasWaiter());

    constexpr int Count = 256;
    {
        std::jthread const producer { [&] {
            for (auto i = 0; i < Count; ++i)
                queue.Push(i);
        } };
    }

    reactor.Drain();
    REQUIRE(seen.size() == Count);
    // FIFO across the hand-off: a queue that lost or reordered under contention
    // would show up here rather than as a count that happens to match.
    for (auto i = 0; i < Count; ++i)
        REQUIRE(seen[static_cast<std::size_t>(i)] == i);

    queue.Close();
    reactor.Drain();
    CHECK(ended);
    CHECK_FALSE(queue.HasWaiter());
}

// ---------------------------------------------------------------------------
// #1041: what the queue owes a chain it hands to the reactor and nobody dequeues.
//
// `Push` and `Close` both `Submit(waiter)` a BORROWED handle carrying no ownership,
// so a reactor destroyed before the post is dequeued frees nothing. That is correct
// for a consumer some `Task` owns and a leak for a detached one, and the two are
// indistinguishable at the call site today because the handle is erased in
// `await_suspend` before the queue ever sees it.
//
// Counted with a frame sentinel rather than left to LeakSanitizer, for the reason
// `ParkedWork_test.cpp` gives: a leak only a sanitizer reports is a red once in N
// runs and reads as a flake.
namespace
{

using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;

/// A chain owned by NOBODY, parked on an empty queue.
FastCache::DetachedTask ConsumeDetached(Queue* queue, FrameSentinel sentinel, FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto item = co_await queue->Pop();
    (void) item;
    co_return;
}

/// The same shape, owned by the `Task` the caller holds. The control.
FastCache::Task<void> ConsumeOwned(Queue* queue, FrameSentinel sentinel, FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto item = co_await queue->Pop();
    (void) item;
    co_return;
}

} // namespace

TEST_CASE("A detached consumer submitted by Push and never dequeued is freed at teardown", "[async][queue][parkedwork]")
{
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        Queue queue { reactor, FastCache::AsyncQueueOptions {} };

        ConsumeDetached(&queue, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);
        REQUIRE(queue.HasWaiter());

        // AsyncQueue.hpp `Push`: exchanges the waiter out and submits it.
        auto const pushed = queue.Push(7);
        REQUIRE(pushed.accepted);
        REQUIRE_FALSE(queue.HasWaiter());
        // The reactor is destroyed WITHOUT draining, so the post is never dequeued.
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A detached consumer submitted by Close and never dequeued is freed at teardown", "[async][queue][parkedwork]")
{
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        Queue queue { reactor, FastCache::AsyncQueueOptions {} };

        ConsumeDetached(&queue, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);

        // AsyncQueue.hpp `Close`: the other site, and the stop path rather than the
        // hot one -- a queue closed while its consumer is parked is the ordinary way
        // a peer sender ends.
        queue.Close();
        REQUIRE_FALSE(queue.HasWaiter());
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A consumer some Task owns is left alone by the reactor", "[async][queue][parkedwork]")
{
    // The control, and it is mandatory rather than decoration: freeing everything at
    // teardown is as wrong as freeing nothing. This frame is owned by the `Task`
    // below, so a reactor that freed what it merely borrows would double free here --
    // and this case is what would catch it.
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        Queue queue { reactor, FastCache::AsyncQueueOptions {} };

        auto consumer = ConsumeOwned(&queue, FrameSentinel { &counters }, &counters);
        reactor.Submit(consumer.Native());
        reactor.Drain();
        REQUIRE(counters.parked == 1);
        REQUIRE(queue.HasWaiter());

        auto const pushed = queue.Push(7);
        REQUIRE(pushed.accepted);
        // Reactor destroyed with the post undrained, exactly as above -- but this
        // frame has an owner, so it must survive until `consumer` goes out of scope.
        CHECK(counters.destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(counters.destroyed == 1);
}
