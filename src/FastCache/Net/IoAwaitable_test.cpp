// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for IoAwaitable's synchronous-completion path: a backend
// (e.g. the TLS decorator with a record already buffered, or a wrapped transport
// that resolves its raw I/O inline) may call Complete() from inside the suspend
// callback. await_suspend must observe that and report "do not suspend" rather
// than resuming the consumer re-entrantly from within await_suspend — which is
// UB and, in a loop, recurses one stack frame per operation to overflow.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <cstddef>

using namespace FastCache;

namespace
{

/// Backend that completes an IoAwaitable SYNCHRONOUSLY from its suspend callback
/// — the exact shape of TlsSocket's buffered-record fast path.
struct SyncCompleter
{
    std::size_t value { 0 };

    [[nodiscard]] IoAwaitable MakeRead()
    {
        IoAwaitable awaitable;
        awaitable.SetSuspendCallback(
            [](IoAwaitable* self, std::coroutine_handle<>) {
                auto* const me = static_cast<SyncCompleter*>(self->CallbackState());
                self->Complete(IoResult { me->value });
            },
            this);
        return awaitable;
    }
};

[[nodiscard]] Task<std::size_t> AwaitOnce(SyncCompleter* completer)
{
    auto const result = co_await completer->MakeRead();
    co_return result.value_or(0);
}

[[nodiscard]] Task<std::size_t> AwaitMany(SyncCompleter* completer, int count)
{
    std::size_t total = 0;
    for (int i = 0; i < count; ++i)
    {
        auto const result = co_await completer->MakeRead();
        total += result.value_or(0);
    }
    co_return total;
}

} // namespace

TEST_CASE("IoAwaitable: synchronous Complete in the suspend callback resolves the await", "[net][ioawaitable]")
{
    SyncCompleter completer { .value = 42 };
    CHECK(SyncRun(AwaitOnce(&completer)) == 42);
}

TEST_CASE("IoAwaitable: many synchronous completions in a loop do not recurse to overflow", "[net][ioawaitable]")
{
    // Mirrors a TLS connection draining many pipelined records, each decrypted
    // from an already-buffered BIO so SSL_read completes with no real await.
    // Pre-fix this resumed the consumer re-entrantly from inside await_suspend,
    // nesting one frame per record; now each co_await resumes via await_suspend
    // returning false, so the loop iterates in constant stack space.
    SyncCompleter completer { .value = 1 };
    constexpr int Iterations = 200000;
    CHECK(SyncRun(AwaitMany(&completer, Iterations)) == static_cast<std::size_t>(Iterations));
}
