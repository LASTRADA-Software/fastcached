// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/TicketVerifier.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Testing
{

/// A ticket audience that is exactly one endpoint, as the presenter dialled it.
///
/// What a case asks is what a surface does with the audience's answer, not how `NodeAudience`
/// computes it from a node's names, ports and addresses -- `NodeAudience_test` owns that. Shared,
/// because a copy per test file is a fake whose mistakes nobody compares.
class ExactAudience final: public Distributed::IAudience
{
  public:
    /// @param endpoint The one audience matched, e.g. `"office.corp:6674"`.
    explicit ExactAudience(std::string endpoint):
        _endpoint { std::move(endpoint) }
    {
    }

    /// @copydoc Distributed::IAudience::Matches
    [[nodiscard]] bool Matches(std::string_view audience) const override
    {
        return audience == _endpoint;
    }

  private:
    std::string _endpoint;
};

} // namespace FastCache::Testing
