// SPDX-License-Identifier: Apache-2.0
#include "RefusedArguments.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include <CompileJob.hpp>

namespace FastCache::Node
{

namespace
{
    /// Whether the worker, given @p allowed, now passes @p argument to the driver that refused it.
    ///
    /// Asked of the worker's OWN predicate, `IsAcceptableJobArgument`, and never of a copy of its
    /// rules: an operator entry is consulted LAST, after the side-artefact table, the introducer
    /// rule and every `Deny` row, so a list naming `-fmodule-output=x.pcm` admits nothing -- and a
    /// copy that knew only the `Deny` rows took it off the row while the worker went on refusing
    /// it. The same function also means a rule added there reaches this answer without an edit.
    ///
    /// A refusal that cannot say which driver judged it is never admitted: there is no question
    /// to re-ask, and keeping it named is the vague right answer where dropping it would be a
    /// confident wrong one.
    /// @param argument An argument as a refusal named it.
    /// @param judgedFor The driver whose rules refused it, as the refusal said.
    /// @param allowed The operator's list.
    /// @return True when the list now admits it.
    [[nodiscard]] bool AdmittedBy(std::string_view argument,
                                  std::optional<Cc::Flavor> judgedFor,
                                  std::vector<std::string> const& allowed)
    {
        return judgedFor.has_value() && Cc::IsAcceptableJobArgument(argument, Cc::DriverOf(*judgedFor), allowed);
    }
} // namespace

RefusedArgumentsReport::RefusedArgumentsReport(NodeConditions& conditions,
                                               ILogger& logger,
                                               std::vector<std::string> allowed):
    _conditions { conditions },
    _logger { logger },
    _allowed { std::move(allowed) }
{
    // Checked and benign rather than undecided: this worker runs, and it has refused nothing.
    _conditions.Clear(NodeCondition::RefusedCompileArguments);
}

void RefusedArgumentsReport::OnJobRefused(Cc::JobError const& error)
{
    if (error.reason != Cc::JobRefusal::RejectedArgument)
        return;

    // The argument alone when the refusal names one; otherwise the refusal's own sentence, which
    // for a half-given path-mapping pair is fixed text of this build's and names the problem.
    auto const& what = error.subject.empty() ? error.detail : error.subject;

    auto const guard = std::scoped_lock { _mutex };

    // A job judged by an OLDER list. `CompileJobRunner::Run` copies the operator list when a job
    // starts and this runs after it returns, so a reload landing in between delivers a refusal the
    // list now in force would not make. Reported, it would name as refused an argument the
    // operator has just admitted -- the confident wrong signal a live row must never give.
    if (AdmittedBy(what, error.judgedFor, _allowed))
        return;

    if (auto const named = std::ranges::find(_named, what, &Refused::argument); named != _named.end())
    {
        ++named->refusals;
        // One spelling can be refused by more than one driver on a worker serving several, and
        // each is asked again on a change: the argument leaves the row only when all of them admit.
        if (!std::ranges::contains(named->judgedFor, error.judgedFor))
            named->judgedFor.push_back(error.judgedFor);
        RaiseLocked();
        return;
    }

    if (_named.size() < MaxNamedArguments)
    {
        _named.push_back(Refused { .argument = what, .refusals = 1, .judgedFor = { error.judgedFor } });
        // Once per argument, so a build refusing it thousands of times writes one line.
        _logger.Logf(LogLevel::Warn,
                     "refused a compile over the argument {}: this worker will not pass it to its compiler, so every "
                     "compile carrying it runs on its own client instead. Said once per argument; the "
                     "refused-compile-arguments condition keeps the count and says what to do",
                     what);
    }
    else
    {
        ++_unnamed;
        if (!_saidOverflow)
        {
            _saidOverflow = true;
            _logger.Logf(LogLevel::Warn,
                         "refused compiles over more than {} distinct arguments; further ones are counted in the "
                         "refused-compile-arguments condition and not named",
                         MaxNamedArguments);
        }
    }
    RaiseLocked();
}

void RefusedArgumentsReport::AllowlistChanged(std::vector<std::string> nowAllowed)
{
    auto const guard = std::scoped_lock { _mutex };
    _allowed = std::move(nowAllowed);

    // Re-judged, never cleared by fiat: the arguments the new list admits leave the row and every
    // other stays. A change that admits something else -- an unrelated entry, a misspelled one --
    // must not read `clear`, which says "checked and benign" about an argument still refused.
    std::erase_if(_named, [this](Refused const& entry) {
        return std::ranges::all_of(entry.judgedFor, [&](std::optional<Cc::Flavor> const driver) {
            return AdmittedBy(entry.argument, driver, _allowed);
        });
    });

    if (_named.empty() && _unnamed == 0)
        _conditions.Clear(NodeCondition::RefusedCompileArguments);
    else
        RaiseLocked();
}

void RefusedArgumentsReport::RaiseLocked()
{
    std::uint64_t refusals = _unnamed;
    std::vector<std::string> arguments;
    arguments.reserve(_named.size());
    for (auto const& entry: _named)
    {
        refusals += entry.refusals;
        arguments.push_back(entry.argument);
    }
    auto const lead =
        std::format("{} compile(s) refused over arguments this worker still will not pass to its compiler{}:",
                    refusals,
                    _unnamed > 0 ? std::format(" ({} of them over arguments not named)", _unnamed) : std::string {});
    _conditions.Raise(NodeCondition::RefusedCompileArguments, ListDetail(lead, arguments));
}

} // namespace FastCache::Node
