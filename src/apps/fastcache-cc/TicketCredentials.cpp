// SPDX-License-Identifier: Apache-2.0
#include "TicketCredentials.hpp"

#include <FastCache/Core/SecureBytes.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Cc
{

namespace Wire = CompileCacheWire;

namespace
{
    /// @param missing Why an exchange presented no ticket, if it did not.
    /// @param cause What its refusal means to an operator.
    /// @return The missing ticket's reason when a ticket would have answered the refusal.
    [[nodiscard]] std::optional<std::string_view> MissingTicketExplains(std::optional<MintFailure> missing,
                                                                        DeclineCause cause) noexcept
    {
        if (!missing.has_value() || cause != DeclineCause::NotPermitted)
            return std::nullopt;
        return ReasonFor(*missing);
    }
} // namespace

TicketCredentials::TicketCredentials(IEndpointExchange& exchange,
                                     std::optional<std::string> mintFrom,
                                     Credential password,
                                     std::string passwordFor,
                                     ExchangeBudget budget,
                                     std::function<void(std::string_view)> sink) noexcept:
    _exchange { exchange },
    _mintFrom { std::move(mintFrom) },
    _password { std::move(password) },
    _passwordFor { std::move(passwordFor) },
    _budget { budget },
    _sink { std::move(sink) }
{
}

PresentedCredential TicketCredentials::Present(std::string_view audience)
{
    // Asked here, where the table's private rows are in scope and the class is complete.
    static_assert(RowsInEnumeratorOrder(Choices, &ChoiceRow::choice),
                  "TicketCredentials::Choices must hold one row per CredentialChoice, in enumerator order");
    auto const passwordFor = _password.Configured() ? std::string_view { _passwordFor } : std::string_view {};
    auto const choice = ChooseCredential(audience, passwordFor);
    return (this->*Choices[static_cast<std::size_t>(choice)].present)(audience);
}

PresentedCredential TicketCredentials::NoCredential(std::string_view /*audience*/)
{
    return {};
}

PresentedCredential TicketCredentials::PasswordCredential(std::string_view /*audience*/)
{
    return PresentedCredential { .credential = _password };
}

PresentedCredential TicketCredentials::MintedCredential(std::string_view audience)
{
    if (!_mintFrom.has_value())
        return Failed(MintFailure::NoSource, "FASTCACHE_ADDR for the launcher, --listen-node for a node");

    // Presented with NO credential: the mint is admitted as this machine, by the connection.
    auto outcome = _exchange.Exchange(*_mintFrom, Wire::EncodeMintTicketRequest(audience), Credential {}, _budget);
    switch (outcome.kind)
    {
        case CacheOutcomeKind::Hit:
            break;
        case CacheOutcomeKind::Miss:
            return Failed(MintFailure::NoTicket, std::format("at {}", *_mintFrom));
        case CacheOutcomeKind::Rejected: {
            auto const* const described = Wire::Describe(outcome.code);
            return Failed(MintFailure::Refused,
                          std::format("at {}: {} ({})",
                                      *_mintFrom,
                                      described != nullptr ? described->name : std::string_view { "unknown code" },
                                      outcome.message));
        }
        case CacheOutcomeKind::Transport:
            return Failed(MintFailure::Unreachable,
                          std::format("at {}: it {}", *_mintFrom, DescribeTransportFailure(outcome.transportFailure)));
    }
    if (outcome.value.empty())
        return Failed(MintFailure::NoTicket, std::format("at {}", *_mintFrom));

    auto presented =
        PresentedCredential { .credential = Credential { .kind = Wire::AuthKind::MachineTicket,
                                                         .username = {},
                                                         .secret = SecureString { Wire::AsStringView(outcome.value) } } };
    // The reply buffer held the ticket in the clear; it is wiped before it is freed, so the one
    // copy left is the credential's.
    SecureZero(outcome.value.data(), outcome.value.size());
    return presented;
}

PresentedCredential TicketCredentials::Failed(MintFailure failure, std::string_view detail)
{
    if (!_said && _sink)
        _sink(std::format("{} ({}); exchanges with other machines go unauthenticated and are refused, so they "
                          "compile locally",
                          ReasonFor(failure),
                          detail));
    _said = true;
    return PresentedCredential { .credential = {}, .missing = failure };
}

CredentialedExchange::CredentialedExchange(IEndpointExchange& inner, ICredentialFor& credentials) noexcept:
    _inner { inner },
    _credentials { credentials }
{
}

CacheOutcome CredentialedExchange::Exchange(std::string_view hostPort,
                                            std::vector<std::byte> frame,
                                            Credential const& /*ignored*/,
                                            ExchangeBudget budget)
{
    auto const presented = _credentials.Present(hostPort);
    // Read before the frame is moved away. A frame with no header files nothing, which is the
    // answer that blames no mint.
    auto const header = frame.size() >= Wire::RequestHeaderSize
                            ? Wire::DecodeRequestHeader(std::span<std::byte const> { frame }.first(Wire::RequestHeaderSize))
                            : std::nullopt;
    auto outcome = _inner.Exchange(hostPort, std::move(frame), presented.credential, budget);
    if (outcome.kind == CacheOutcomeKind::Rejected && header.has_value())
        _refusals.Note(ExchangeSite { .endpoint = std::string { hostPort }, .opcode = header->opRaw }, presented.missing);
    return outcome;
}

void RefusalRecord::Note(ExchangeSite site, std::optional<MintFailure> missing)
{
    auto const filed = std::ranges::find(_rows, site, &Filed::site);
    if (filed != _rows.end())
        filed->missing = missing;
    else
        _rows.push_back(Filed { .site = std::move(site), .missing = missing });
}

std::optional<MintFailure> RefusalRecord::MissingAt(ExchangeSite const& site) const noexcept
{
    auto const* const filed = core::findOrNull(_rows, site, &Filed::site);
    return filed != nullptr ? filed->missing : std::nullopt;
}

std::string RecordedReason(CacheOutcome const& outcome, std::optional<MintFailure> missing)
{
    if (outcome.kind == CacheOutcomeKind::Rejected)
        if (auto const why = MissingTicketExplains(missing, DeclineCauseFor(outcome.code)); why.has_value())
            return std::string { *why };
    return DescribeOutcome(outcome);
}

DispatchRecording RecordedReason(DispatchResult const& result, RefusalRecord const& refusals) noexcept
{
    auto recording = RecordingFor(result.status, result.decline);
    if (result.status != DispatchStatus::Declined || !result.declinedAt.has_value())
        return recording;
    if (auto const why = MissingTicketExplains(refusals.MissingAt(*result.declinedAt), result.decline); why.has_value())
        recording.reason = *why;
    return recording;
}

} // namespace FastCache::Cc
