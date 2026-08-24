// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

class IReactor;

/// The candidates for a host, or why there are none.
///
/// `NetError` rather than the blocking seam's `std::string`, because this feeds
/// a connector: mapping a message onto a code once, here, is what keeps four
/// connectors from each inventing their own answer.
using ResolveResult = std::expected<std::vector<ResolvedEndpoint>, NetError>;

/// Build the one error a failed lookup produces.
///
/// A free function rather than four call sites formatting their own, so the
/// message an operator reads is the same whichever connector produced it, and it
/// names what was being looked up -- "resolution failed" without the host is the
/// one thing the reader already knew.
/// @param host The host that could not be resolved.
/// @param port The port it was to be paired with.
/// @param why The underlying resolver's own message.
/// @return The structured error, coded `AddressNotAvail`.
[[nodiscard]] inline NetError ResolveFailure(std::string_view host, std::uint16_t port, std::string_view why)
{
    return NetError { .code = NetErrorCode::AddressNotAvail,
                      .systemCode = 0,
                      .context = std::format("cannot resolve {}:{}: {}", host, port, why) };
}

/// Name resolution a coroutine can await.
///
/// A seam beside `IAddressResolver` rather than a replacement for it. Binding
/// happens once, at start-up, on a thread that is allowed to block, so the bind
/// path keeps the blocking primitive; this decorates it for the dial path, where
/// the caller may be a reactor thread with thousands of connections on it.
///
/// The distinction matters because `getaddrinfo` is the one genuinely unbounded
/// step in opening a connection -- it takes no timeout, and a wedged resolver
/// therefore parks whoever called it for as long as the platform's resolver
/// library feels like. On a reactor that is every connection; in the compiler
/// launcher it is every translation unit in the build.
class IAsyncAddressResolver
{
  public:
    IAsyncAddressResolver() = default;
    IAsyncAddressResolver(IAsyncAddressResolver const&) = delete;
    IAsyncAddressResolver(IAsyncAddressResolver&&) = delete;
    IAsyncAddressResolver& operator=(IAsyncAddressResolver const&) = delete;
    IAsyncAddressResolver& operator=(IAsyncAddressResolver&&) = delete;
    virtual ~IAsyncAddressResolver() = default;

    /// Resolve `host:port` into candidate endpoints in preference order.
    ///
    /// @param host Hostname or literal address, unbracketed. Taken **by value**
    ///        because an implementation may hand it to another thread, and
    ///        because this produces a coroutine whose frame outlives the call
    ///        expression -- the hazard `Net/TcpClient.hpp` records for reference
    ///        parameters.
    /// @param port TCP port in host byte order.
    /// @param reactor Where the result is delivered. A **null** pointer means
    ///        "resolve inline on this thread and never suspend", the same
    ///        nullable-reactor convention `Async/SleepUntil` uses, and what keeps
    ///        `BlockingConnector` drivable by `SyncRun`. When non-null the
    ///        returned task resumes on that reactor's loop thread and nowhere
    ///        else.
    /// @return The candidates, or why the lookup produced none.
    [[nodiscard]] virtual Task<ResolveResult> Resolve(std::string host, std::uint16_t port, IReactor* reactor) = 0;
};

/// An async resolver that is not async: it calls the blocking seam inline.
///
/// For callers that are already on a thread allowed to block --
/// `BlockingConnector`, the compile node's heartbeat, a one-shot CLI. Because it
/// never suspends, a task built on it is never left suspended, which is exactly
/// what `SyncRun` requires.
///
/// Header-only deliberately: `fastcache-cc` compiles this in, and a `.cpp` row
/// carrying a thread would put a `pthread_create` reference into a binary that
/// links no thread library.
class InlineAddressResolver final: public IAsyncAddressResolver
{
  public:
    /// @param inner The blocking resolver to delegate to.
    explicit InlineAddressResolver(IAddressResolver& inner = DefaultAddressResolver()) noexcept:
        _inner { inner }
    {
    }

    /// @copydoc IAsyncAddressResolver::Resolve
    ///
    /// `reactor` is accepted and ignored: this resolver has nothing to hand back
    /// from, so honouring it would mean suspending for no reason.
    [[nodiscard]] Task<ResolveResult> Resolve(std::string host, std::uint16_t port, IReactor* /*reactor*/) override
    {
        auto resolved = _inner.Resolve(host, port);
        if (!resolved.has_value())
            co_return std::unexpected(ResolveFailure(host, port, resolved.error()));
        co_return std::move(*resolved);
    }

  private:
    IAddressResolver& _inner;
};

} // namespace FastCache
