// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <apps/fastcache-cc/Dispatch.hpp>
#include <apps/fastcache-cc/TicketCredentials.hpp>
#include <tests/Unwrap.hpp>

namespace FastCache::Testing
{

/// @file TicketFakes.hpp
/// The machine-ticket fakes the launcher's and the node's cases share: this machine's node as a
/// minter, and a credential source that presents nothing.
///
/// Shared rather than copied per binary, because a WRONG fake makes its cases pass: a copy that
/// minted for the wrong audience would agree with the code it was copied beside.

/// The secret a ticket `MintsForItsAudience` minted for @p audience carries.
/// @param audience The endpoint the ticket names.
/// @return The ticket's bytes, as text.
[[nodiscard]] inline std::string TicketFor(std::string_view audience)
{
    return "ticket-for:" + std::string { audience };
}

/// This machine's node, minting a ticket that names the audience it was asked for (`TicketFor`).
class MintsForItsAudience final: public Cc::IEndpointExchange
{
  public:
    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& /*credential*/,
                                            Cc::ExchangeBudget /*budget*/) override
    {
        auto const audience = CompileCacheWire::DecodeMintTicketPayload(
            std::span<std::byte const> { frame }.subspan(CompileCacheWire::RequestHeaderSize));
        REQUIRE(audience.has_value());
        sources.emplace_back(hostPort);
        audiences.push_back(Unwrap(audience));
        // The mint for THIS audience does not answer: its exchange goes out with no ticket.
        if (unreachableAfter.has_value() && audiences.size() > *unreachableAfter)
            return Cc::CacheOutcome {};
        auto const ticket = TicketFor(Unwrap(audience));
        auto const bytes = CompileCacheWire::AsBytes(ticket);
        auto outcome = Cc::CacheOutcome {};
        outcome.kind = Cc::CacheOutcomeKind::Hit;
        outcome.value.assign(bytes.begin(), bytes.end());
        return outcome;
    }

    std::vector<std::string> audiences; ///< Every audience a ticket was minted for, in order.
    std::vector<std::string> sources;   ///< Where each mint was asked, in the same order.
    /// When engaged, every mint after this many fails as unreachable.
    std::optional<std::size_t> unreachableAfter;
};

/// A credential source that presents nothing anywhere, for a case whose subject is not credentials.
class PresentsNothing final: public Cc::ICredentialFor
{
  public:
    /// @copydoc Cc::ICredentialFor::Present
    [[nodiscard]] Cc::PresentedCredential Present(std::string_view /*audience*/) override
    {
        return {};
    }
};

} // namespace FastCache::Testing
