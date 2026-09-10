// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "RespClient.hpp"
#include "StatsSource.hpp"

#include <optional>
#include <string>
#include <vector>

namespace FastCache::Cli
{

/// Asks every stats source this build knows about.
///
/// The impure half of the ladder, kept apart from `StatsSource.hpp`'s decision so that
/// `ChooseStats` -- which is where all the interesting behaviour is -- needs no socket
/// to test. This class does the opposite: it is all socket and no decision.
class LadderGatherer final: public IStatsGatherer
{
  public:
    /// @param admin Where `/metrics` is; an unconfigured endpoint means *do not ask*,
    ///        which is reported as `asked == false` rather than as a failure.
    /// @param timeouts How long to wait.
    /// @param bearer A credential for the admin surface, or nullopt. `/metrics` needs
    ///        none today -- it short-circuits above the credential gate -- but a
    ///        deployment may still sit behind something that does.
    /// @param resp The already-open cache connection, or null when there is none.
    LadderGatherer(Endpoint admin, DialTimeouts timeouts, std::optional<std::string> bearer, IExchange* resp) noexcept;

    [[nodiscard]] std::vector<StatsAttempt> Gather() override;

  private:
    /// Ask the admin surface for `/metrics`.
    /// @return What happened.
    [[nodiscard]] StatsAttempt AskMetrics();

    /// Ask the cache for `INFO`.
    /// @return What happened.
    [[nodiscard]] StatsAttempt AskInfo();

    Endpoint _admin;
    DialTimeouts _timeouts;
    std::optional<std::string> _bearer;
    IExchange* _resp;
};

} // namespace FastCache::Cli
