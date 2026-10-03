// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <WorkerProtocol.hpp>

/// @file RefusedArguments.hpp
/// Which arguments this worker refused compiles over, said where an operator looks.
///
/// `fastcache_worker_jobs_refused_rejected_argument_total` says how many jobs a worker turned away
/// over an argument, and until this existed nothing on the node said WHICH: not its log, not its
/// conditions. A cl-debug build had 372 of them refused over one flag, and the only way to learn
/// which flag was to reproduce a dispatch by hand. The remedy -- `--allow-compile-arg`, or leaving
/// those compiles local -- needs exactly the name nothing reported.

namespace FastCache::Node
{

/// Raises `refused-compile-arguments` naming what this worker refused, and logs each argument once.
///
/// **A row AND a line, and the line is rate-limited by what it says rather than by a clock**: an
/// argument is logged the first time it is refused and never again, so a build refusing one flag
/// ten thousand times writes one line, and the row keeps the count. After `MaxNamedArguments`
/// distinct arguments nothing further is logged or named -- one line says so -- because a client
/// sending an endless stream of distinct arguments must not be able to fill this node's log or
/// grow its memory. The row's detail is clamped by `NodeConditions::Raise` in any case.
///
/// **Live, and RE-JUDGED when the allowlist changes.** A reload that changes `--allow-compile-arg`
/// is the operator acting on this row, so the arguments the new list admits leave it and every
/// other stays -- the row reads `clear` only when nothing it named is still refused, which is how
/// *fixed* and *still refused* come to look different. A job that started under the old list and
/// reports after the change is judged by the list now in force.
///
/// Thread-safe: `OnJobRefused` runs on whichever pool thread ran the job.
class RefusedArgumentsReport final: public Cc::IJobRefusalObserver
{
  public:
    /// How many distinct arguments are named and logged before the rest are only counted.
    static constexpr std::size_t MaxNamedArguments = 16;

    /// Evaluates the row: a worker that has refused nothing has CHECKED, so it is `clear`.
    /// @param conditions Where the row is answered; must outlive this.
    /// @param logger Where each newly refused argument is said once; must outlive this.
    /// @param allowed The operator's `--allow-compile-arg` list in force as the worker starts.
    RefusedArgumentsReport(NodeConditions& conditions, ILogger& logger, std::vector<std::string> allowed);

    RefusedArgumentsReport(RefusedArgumentsReport const&) = delete;
    RefusedArgumentsReport& operator=(RefusedArgumentsReport const&) = delete;
    RefusedArgumentsReport(RefusedArgumentsReport&&) = delete;
    RefusedArgumentsReport& operator=(RefusedArgumentsReport&&) = delete;
    ~RefusedArgumentsReport() override = default;

    /// A job was refused; only a `RejectedArgument` refusal is this row's business.
    /// @param error The refusal, as the runner reported it.
    void OnJobRefused(Cc::JobError const& error) override;

    /// The operator changed `--allow-compile-arg`: drop what the new list admits, keep the rest.
    /// @param nowAllowed The list now in force, as `CompileJobRunner` was just given it.
    void AllowlistChanged(std::vector<std::string> nowAllowed);

  private:
    /// Say the row as it now stands. Caller holds `_mutex`.
    void RaiseLocked();

    /// One named argument and how many jobs it cost.
    struct Refused
    {
        std::string argument;   ///< As the refusal named it.
        std::uint64_t refusals; ///< Jobs refused over it.
        /// Every driver that refused it, as each refusal said; never empty.
        std::vector<std::optional<Cc::Flavor>> judgedFor;
    };

    NodeConditions& _conditions;
    ILogger& _logger;

    std::mutex _mutex;
    std::vector<std::string> _allowed; ///< The operator's list in force. Guarded by `_mutex`.
    std::vector<Refused> _named;       ///< Distinct arguments, first seen first. Guarded by `_mutex`.
    std::uint64_t _unnamed {};         ///< Refusals over arguments past `MaxNamedArguments`. Guarded.
    bool _saidOverflow {};             ///< Whether the past-the-cap line was written. Guarded.
};

} // namespace FastCache::Node
