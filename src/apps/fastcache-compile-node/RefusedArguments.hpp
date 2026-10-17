// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Core/Logger.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
/// **A refusal names EVERY argument its job was refused over (#1681), and is still ONE compile.**
/// So compiles are counted by the SET of named arguments they carried, and a compile leaves the
/// count only once every argument of its set is admitted: admitting one of a compile's four
/// arguments leaves it refused over the other three, and a count that dropped then would say
/// fewer compiles are refused than are.
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

    /// A job was refused; only a `RejectedArgument` refusal is this row's business, and it is one
    /// compile however many arguments it names.
    /// @param error The refusal, as the runner reported it.
    void OnJobRefused(Cc::JobError const& error) override;

    /// The operator changed `--allow-compile-arg`: drop what the new list admits, keep the rest.
    /// @param nowAllowed The list now in force, as `CompileJobRunner` was just given it.
    void AllowlistChanged(std::vector<std::string> nowAllowed);

  private:
    /// Say the row as it now stands. Caller holds `_mutex`.
    void RaiseLocked();

    /// One named argument.
    struct Refused
    {
        std::string argument;    ///< As the refusal named it.
        std::uint64_t firstSeen; ///< Its place among every argument named so far; the row names in this order.
        /// Every driver that refused it, as each refusal said; never empty.
        std::vector<std::optional<Cc::Flavor>> judgedFor;
    };

    /// Which named arguments one refused compile carried: bit `i` is `_slots[i]`.
    using ArgumentSet = std::uint32_t;
    static_assert(MaxNamedArguments <= std::numeric_limits<ArgumentSet>::digits,
                  "every named argument needs a bit of its own in a compile's set");

    /// The lowest slot naming nothing, if one is left. Caller holds `_mutex`.
    /// @return Its index.
    [[nodiscard]] std::optional<std::size_t> FreeSlotLocked() const;

    /// The slot already naming @p argument, if one does. Caller holds `_mutex`.
    /// @param argument An argument as a refusal named it.
    /// @return Its slot.
    [[nodiscard]] std::optional<std::size_t> SlotOfLocked(std::string_view argument) const;

    NodeConditions& _conditions;
    ILogger& _logger;

    std::mutex _mutex;
    std::vector<std::string> _allowed; ///< The operator's list in force. Guarded by `_mutex`.
    /// The named arguments; a slot frees when the list admits its argument. Guarded by `_mutex`.
    std::array<std::optional<Refused>, MaxNamedArguments> _slots;
    /// Refused compiles by the set of named arguments they carried. Guarded by `_mutex`.
    std::map<ArgumentSet, std::uint64_t> _compiles;
    /// Compiles refused over at least one argument past `MaxNamedArguments`. Guarded by `_mutex`.
    std::uint64_t _unnamed {};
    std::uint64_t _namesGiven {}; ///< Arguments named so far; orders the named. Guarded by `_mutex`.
    bool _saidOverflow {};        ///< Whether the past-the-cap line was written. Guarded by `_mutex`.
};

} // namespace FastCache::Node
