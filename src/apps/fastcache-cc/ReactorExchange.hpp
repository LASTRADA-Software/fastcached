// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "Dispatch.hpp"

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Net/IConnector.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Drive one framed request/reply exchange on a reactor.
///
/// ## One reactor per exchange, and that is a property rather than a limitation
///
/// `IReactor::Run` returns only when `Stop()` has been called, and no reactor here
/// ever clears that flag -- so a second `Run()` on a stopped one returns
/// immediately. A reused instance would therefore perform the first exchange and
/// silently skip every later one, and the launcher would fall back to a local
/// compile with nothing anywhere saying why. Creating one per exchange costs an
/// `epoll_create1` and an `eventfd` -- microseconds against a 45 ms preprocess --
/// and makes that mistake unreachable.
///
/// ## Injected, so the rules are testable without a socket
///
/// Both collaborators come in from outside, so the happy path, an expired budget
/// and a peer that accepts and then goes quiet are all `TestReactor` unit tests.
class ReactorExchange
{
  public:
    /// @param reactor The loop this exchange runs on. Single use: see the class note.
    /// @param connector How to dial; its sockets must belong to `reactor`.
    ReactorExchange(IReactor& reactor, IConnector& connector) noexcept;

    ReactorExchange(ReactorExchange const&) = delete;
    ReactorExchange(ReactorExchange&&) = delete;
    ReactorExchange& operator=(ReactorExchange const&) = delete;
    ReactorExchange& operator=(ReactorExchange&&) = delete;
    ~ReactorExchange() = default;

    /// Dial `hostPort`, send `frame`, and read the reply.
    ///
    /// @param hostPort `host:port`; a bare port is refused rather than assumed to
    ///        mean this machine.
    /// @param frame A complete framed request.
    /// @param credential Presented with the request; default-constructed sends none.
    /// @param budget The two deadlines.
    /// @return The outcome. A budget that runs out reports `Transport`, which every
    ///         caller already treats as "no cache" and answers by compiling -- so a
    ///         slow daemon costs a build time, never correctness.
    [[nodiscard]] CacheOutcome Run(std::string_view hostPort,
                                   std::vector<std::byte> frame,
                                   Credential credential,
                                   ExchangeBudget budget);

  private:
    IReactor& _reactor;
    IConnector& _connector;
    bool _used { false };
};

/// Build a reactor and a connector, and run one exchange on them.
///
/// The platform wiring, separated from the rules above so the rules can be tested
/// with neither. Constructed at the point of the dial rather than at process start:
/// a launcher with no cache configured, or one answering `--help`, dials nothing and
/// must pay nothing.
///
/// @param hostPort Where to dial.
/// @param frame A complete framed request.
/// @param credential Presented with the request.
/// @param budget The two deadlines.
/// @return The outcome; `Transport` when the endpoint could not be reached.
[[nodiscard]] CacheOutcome RunOneExchange(std::string_view hostPort,
                                          std::vector<std::byte> frame,
                                          Credential credential,
                                          ExchangeBudget budget);

/// The `IEndpointExchange` a dispatch uses in production: `RunOneExchange`, once
/// per verb.
///
/// It lives here rather than in `Dispatch.cpp` because it needs a reactor, and
/// `Dispatch.cpp` is compiled into the compile NODE as well -- which wants
/// `DecodeArgs` and the request types and never dials anybody. Declaring the
/// factory beside the machinery it needs keeps that link honest.
///
/// It carries no deadline of its own: every budget arrives with the call, so the
/// one object serves a lease, a compile and a release without a caller having to
/// remember which of them it was built for.
/// @return An exchange; never null.
[[nodiscard]] std::unique_ptr<IEndpointExchange> MakeTcpExchange();

} // namespace FastCache::Cc
