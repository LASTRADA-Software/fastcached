// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"
#include "Dispatch.hpp"
#include "Stats.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/TicketChoice.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// @file TicketCredentials.hpp
/// A machine ticket per exchange, minted by this machine's own node and presented in a pipelined
/// AUTH.
///
/// The launcher carries a ticket as OPAQUE bytes from `MINT-TICKET`'s reply into `AUTH`: it never
/// parses one, so it needs `CompileCacheWire.hpp` and nothing of `Distributed/`, which it does not
/// link.

/// What ONE exchange presents, and -- when a ticket was due and could not be minted -- why not.
///
/// The failure travels with the credential of the exchange it belongs to, rather than being
/// remembered by the minter: a refusal is explained by the mint of THAT exchange and no other, so
/// a NotAMember answered to a valid ticket can never be recorded as a missing one.
struct PresentedCredential
{
    Credential credential;                 ///< What to present; default-constructed presents nothing.
    std::optional<MintFailure> missing {}; ///< Engaged when a ticket was due and none could be minted.
};

/// Which credential ONE exchange presents, chosen by where it is going.
class ICredentialFor
{
  public:
    ICredentialFor() = default;
    ICredentialFor(ICredentialFor const&) = delete;
    ICredentialFor& operator=(ICredentialFor const&) = delete;
    ICredentialFor(ICredentialFor&&) = delete;
    ICredentialFor& operator=(ICredentialFor&&) = delete;
    virtual ~ICredentialFor() = default;

    /// @param audience The endpoint this exchange dials, as `host:port`.
    /// @return What to present there, and why no ticket is among it when one was due.
    [[nodiscard]] virtual PresentedCredential Present(std::string_view audience) = 0;

    /// @param audience The endpoint this exchange dials, as `host:port`.
    /// @return What to present there; default-constructed presents nothing.
    [[nodiscard]] Credential For(std::string_view audience)
    {
        return Present(audience).credential;
    }
};

/// Ceiling on CONNECTING to this machine's node for a mint (#1664).
///
/// A mint source is a loopback literal by construction (`TicketSourceFor`), so it is bounded by
/// what loopback costs and not by a LAN exchange's connect budget. That matters on Windows, where a
/// connect to a CLOSED loopback port is not refused at once: the stack retries the SYN and answers
/// `WSAECONNREFUSED` after ~2 s, so a machine with no node used to wait out the whole exchange
/// budget (a second) on every mint.
///
/// **The figures it was chosen from, and their conditions** -- 32-thread hosts, each sampled idle and
/// under 64 CPU-bound processes, 3,000 connects per row (closed port on Windows: 5), timed in Python
/// around the connect alone, the worse of idle and loaded shown:
///
/// | host | instrument | live listener, max (p99.9) | closed port |
/// |---|---|---|---|
/// | Windows 11 26200 | `asyncio` Proactor (IOCP `ConnectEx`) | 0.76 ms (0.60 ms) | 2,016--2,042 ms, refused |
/// | Windows 11 26200 | blocking `socket.connect` | 0.58 ms (0.58 ms) | 2,031--2,056 ms, refused |
/// | WSL2 Ubuntu 24.04, kernel 6.6 | `asyncio` selector (`epoll`) | 13.5 ms (7.2 ms) | 0.15--0.47 ms, refused |
/// | WSL2 Ubuntu 24.04, kernel 6.6 | blocking `socket.connect` | 1.45 ms (0.44 ms) | 0.14 ms max, refused |
///
/// The `asyncio` Linux tail is the interpreter's event loop as much as the kernel's -- the blocking
/// connect on the same host and load tops out at 1.45 ms -- and Linux refuses a closed port at once,
/// so there the bound is never paid. Fifty milliseconds is over 60 times the Windows maximum and over
/// 30 times the blocking Linux one, and it is what a machine with no node pays, ONCE per invocation
/// (`MintRetry::NotAgain`).
/// A live node slower than this to ACCEPT costs no build: the exchange goes without a ticket, which
/// a node-backed cache refuses and the launcher answers by compiling locally.
inline constexpr std::chrono::milliseconds LoopbackConnectBound { 50 };

/// The budget a mint runs under, from the exchange budget the launcher was configured with.
///
/// Only the CONNECT is tightened, to `LoopbackConnectBound` or the configured one if that is
/// tighter: a node that accepted but is busy may answer slowly, so the round trip keeps the
/// exchange's own total -- and a node that does not answer in time only means no ticket.
/// @param exchange The launcher's exchange budget.
/// @return @p exchange with its connect bounded by loopback.
[[nodiscard]] constexpr ExchangeBudget MintBudget(ExchangeBudget exchange) noexcept
{
    // Non-positive is `DialOptions`' spelling of *the platform default*, which is no bound at all.
    exchange.connect = exchange.connect > std::chrono::milliseconds::zero()
                           ? std::min(exchange.connect, LoopbackConnectBound)
                           : LoopbackConnectBound;
    return exchange;
}

/// Mints one ticket per call from this machine's node, over `exchange`, which must be the RAW
/// exchange -- never a `CredentialedExchange`, which would ask this object for the mint's own
/// credential.
///
/// **One ticket per exchange, never one per process.** A node spends a ticket once, so a second
/// exchange presenting the first's ticket is refused `replayed`; and each ticket names ONE
/// audience, so the lease's would be refused at the worker. Minting is a loopback round trip, far
/// below the exchange it admits.
///
/// **A mint that fails leaves that exchange unauthenticated, and says why on two channels.** One is
/// `sink`, which prints at most two lines per process: one on the first failure, and one more if a
/// later failure is remembered (`MintRetry::NotAgain`), since that says something new about every
/// later exchange. The other is the `missing` half of each failed exchange's
/// `PresentedCredential`, which `RecordedReason` records in place of a refusal it causes. Whether
/// the exchange IS refused is its audience's decision: a cache with authentication off serves it,
/// one backed by a node refuses it `NotAMember`, and every refusal ends in a local compile, so a
/// missing node costs speed, never a build.
///
/// **And a failure whose row says `MintRetry::NotAgain` is not asked again by this process**
/// (#1664): without a node, every exchange used to wait out a connect of its own before it went
/// anywhere. Each later exchange still carries the failure as its `missing`, so a refusal it causes
/// is explained exactly as the first one's is.
///
/// **Precondition: every owner is ONE-SHOT.** The memo never expires, which is sound only because
/// each object lives for one short unit of work -- the launcher's one compile, a node's one operator
/// verb -- so a node started meanwhile is asked by the next. A long-lived owner would hold a stale
/// "no node" for its whole life and must not reuse this object: it would need the memo refreshed on
/// an interval or a host event, never on a miss (AGENT.md, "Caching an expensive repeated answer").
class TicketCredentials final: public ICredentialFor
{
  public:
    /// @param exchange How to reach this machine's node; the RAW exchange. Borrowed.
    /// @param mintFrom Where this machine's node answers `MINT-TICKET` (`TicketSourceFor`), or
    ///        nullopt when the cache address names no endpoint.
    /// @param password The one password configured; presented to @p passwordFor alone.
    /// @param passwordFor The endpoint @p password belongs to (`FASTCACHE_ADDR`); ignored when
    ///        @p password is not configured.
    /// @param budget The launcher's exchange budget; a mint runs under `MintBudget` of it.
    /// @param sink Where a failed mint is said: on the first failure, and again if one is remembered;
    ///        may be empty.
    TicketCredentials(IEndpointExchange& exchange,
                      std::optional<std::string> mintFrom,
                      Credential password,
                      std::string passwordFor,
                      ExchangeBudget budget,
                      std::function<void(std::string_view)> sink) noexcept;

    /// @copydoc ICredentialFor::Present
    [[nodiscard]] PresentedCredential Present(std::string_view audience) override;

  private:
    [[nodiscard]] PresentedCredential NoCredential(std::string_view audience);
    [[nodiscard]] PresentedCredential PasswordCredential(std::string_view audience);
    [[nodiscard]] PresentedCredential MintedCredential(std::string_view audience);

    /// Say @p failure once per process, and present nothing, naming it.
    /// @param failure Why.
    /// @param detail What the line adds to the fixed reason: the endpoint, the node's words.
    /// @return No credential, and @p failure.
    [[nodiscard]] PresentedCredential Failed(MintFailure failure, std::string_view detail);

    /// One row of `Choices`.
    struct ChoiceRow
    {
        CredentialChoice choice;                                             ///< Which choice.
        PresentedCredential (TicketCredentials::*present)(std::string_view); ///< How it is presented.
    };

    /// How each choice is presented.
    static constexpr EnumTable<CredentialChoice, ChoiceRow> Choices { {
        { .choice = CredentialChoice::None, .present = &TicketCredentials::NoCredential },
        { .choice = CredentialChoice::Password, .present = &TicketCredentials::PasswordCredential },
        { .choice = CredentialChoice::Ticket, .present = &TicketCredentials::MintedCredential },
    } };

    IEndpointExchange& _exchange;
    std::optional<std::string> _mintFrom;
    Credential _password;
    std::string _passwordFor;
    ExchangeBudget _budget;
    std::function<void(std::string_view)> _sink;
    bool _said { false };
    /// A failure this process does not ask the node about again (`MintRetry::NotAgain`).
    std::optional<MintFailure> _remembered;
};

/// What each exchange that was REFUSED presented, filed under where it went and what it asked.
///
/// Per exchange SITE rather than one "last refusal", because a dispatch does not end on the exchange
/// that declined it: a refused COMPILE is always followed by the RELEASE, and a release refused for
/// a failed mint would otherwise explain a compile that presented a valid ticket.
class RefusalRecord
{
  public:
    /// File what one refused exchange presented; a later refusal at the same site replaces it.
    /// @param site Where the exchange went, and what it asked.
    /// @param missing Why it presented no ticket; nullopt when it presented one, or none was due.
    void Note(ExchangeSite site, std::optional<MintFailure> missing);

    /// @param site An exchange that was refused.
    /// @return Why it presented no ticket; nullopt when it presented one, when none was due, or
    ///         when nothing refused was filed there -- the answer that blames no mint.
    [[nodiscard]] std::optional<MintFailure> MissingAt(ExchangeSite const& site) const noexcept;

  private:
    /// One refused exchange site, and what its latest refusal presented.
    struct Filed
    {
        ExchangeSite site;                  ///< Where it went, and what it asked.
        std::optional<MintFailure> missing; ///< Why it presented no ticket.
    };

    std::vector<Filed> _rows;
};

/// An exchange that asks `credentials` for EACH endpoint it dials -- a LEASE, the COMPILE at the
/// worker the grant names, the RELEASE at whoever issued it, each redirect -- whatever its caller
/// handed it.
///
/// **The caller's credential is ignored on purpose.** `Dispatch` holds one credential for a whole
/// conversation that visits several endpoints, and a ticket names one of them; asking per dial is
/// the only place that knows where each frame is going. It must sit ABOVE anything that rewrites
/// the address dialled, or a ticket names the endpoint before the rewrite.
///
/// **And it files, per REFUSED exchange, what that exchange presented** (`Refusals`), under the
/// endpoint it dialled and the verb it asked, so a dispatch's decline is explained by the exchange
/// that declined it (`DispatchResult::declinedAt`) and by no exchange after it.
class CredentialedExchange final: public IEndpointExchange
{
  public:
    /// @param inner Where the frames go. Borrowed.
    /// @param credentials What each endpoint is shown. Borrowed.
    CredentialedExchange(IEndpointExchange& inner, ICredentialFor& credentials) noexcept;

    /// @copydoc IEndpointExchange::Exchange
    [[nodiscard]] CacheOutcome Exchange(std::string_view hostPort,
                                        std::vector<std::byte> frame,
                                        Credential const& ignored,
                                        ExchangeBudget budget) override;

    /// @return What each refused exchange presented, by where it went and what it asked.
    [[nodiscard]] RefusalRecord const& Refusals() const noexcept
    {
        return _refusals;
    }

  private:
    IEndpointExchange& _inner;
    ICredentialFor& _credentials;
    RefusalRecord _refusals;
};

/// The reason a cache exchange's failure is recorded and said under.
///
/// **The one decision, and it is here rather than at each call site in `main.cpp`**, which is in no
/// test target: a refusal a ticket would have answered -- one an operator fixes where this client
/// stands (`DeclineCause::NotPermitted`) -- whose exchange presented no ticket is recorded under
/// WHY no ticket was presented, the thing to fix. Every other outcome keeps `DescribeOutcome`'s
/// words, a refusal answered to a valid ticket included.
/// @param outcome How the exchange ended.
/// @param missing Why THAT exchange presented no ticket, from its `PresentedCredential`.
/// @return The reason.
[[nodiscard]] std::string RecordedReason(CacheOutcome const& outcome, std::optional<MintFailure> missing);

/// The recording of a dispatch, with a decline explained by its declining exchange's missing ticket.
///
/// `RecordingFor`, except that a `NotPermitted` decline whose DECLINING exchange -- the one
/// `result.declinedAt` names, read out of @p refusals -- presented no ticket is tallied under WHY:
/// the same decision `RecordedReason` makes for the cache. What any other exchange presented, the
/// release that follows a refused compile included, explains nothing.
/// @param result What `Dispatch` returned.
/// @param refusals `CredentialedExchange::Refusals` of the exchange the dispatch ran over.
/// @return The state to record and its fixed reason.
[[nodiscard]] DispatchRecording RecordedReason(DispatchResult const& result, RefusalRecord const& refusals) noexcept;

} // namespace FastCache::Cc
