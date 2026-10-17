// SPDX-License-Identifier: Apache-2.0
#include "RefusedArguments.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <span>
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

    // The arguments when the refusal names them; otherwise the refusal's own sentence, which for a
    // half-given path-mapping pair is fixed text of this build's and names the problem.
    //
    // A NAMED fallback and a choice between two spans, as `main.cpp`'s banner makes it. A conditional
    // over two vectors copied the named list on every refusal, and its braced arm is what GCC 16
    // reports at -O2 as a dangling pointer to an unnamed temporary at this function's end.
    std::array const unnamedSubject { Cc::RefusedArgumentName { .argument = error.detail, .judgedFor = std::nullopt } };
    auto const subjects = error.subjects.empty() ? std::span<Cc::RefusedArgumentName const> { unnamedSubject }
                                                 : std::span<Cc::RefusedArgumentName const> { error.subjects };

    auto const guard = std::scoped_lock { _mutex };

    ArgumentSet carried = 0;
    bool unnamed = false;
    for (auto const& subject: subjects)
    {
        // A job judged by an OLDER list. `CompileJobRunner::Run` copies the operator list when a job
        // starts and this runs after it returns, so a reload landing in between delivers a refusal
        // the list now in force would not make. Reported, it would name as refused an argument the
        // operator has just admitted -- the confident wrong signal a live row must never give.
        if (AdmittedBy(subject.argument, subject.judgedFor, _allowed))
            continue;

        if (auto const slot = SlotOfLocked(subject.argument); slot.has_value())
        {
            auto& named = _slots.at(*slot);
            // One spelling can be refused by more than one driver on a worker serving several, and
            // each is asked again on a change: the argument leaves the row only when all of them admit.
            if (named.has_value() && !std::ranges::contains(named->judgedFor, subject.judgedFor))
                named->judgedFor.push_back(subject.judgedFor);
            carried |= ArgumentSet { 1 } << *slot;
            continue;
        }

        if (auto const slot = FreeSlotLocked(); slot.has_value())
        {
            _slots.at(*slot) =
                Refused { .argument = subject.argument, .firstSeen = ++_namesGiven, .judgedFor = { subject.judgedFor } };
            carried |= ArgumentSet { 1 } << *slot;
            // Once per argument, so a build refusing it thousands of times writes one line.
            _logger.Logf(LogLevel::Warn,
                         "refused a compile over the argument {}: this worker will not pass it to its compiler, so "
                         "every compile carrying it runs on its own client instead. Said once per argument; the "
                         "refused-compile-arguments condition keeps the count and says what to do",
                         subject.argument);
            continue;
        }

        unnamed = true;
        if (!_saidOverflow)
        {
            _saidOverflow = true;
            _logger.Logf(LogLevel::Warn,
                         "refused compiles over more than {} distinct arguments; further ones are counted in the "
                         "refused-compile-arguments condition and not named",
                         MaxNamedArguments);
        }
    }

    // Every argument it names is one the list now in force admits: not a refusal this row reports.
    if (carried == 0 && !unnamed)
        return;
    // A compile carrying an argument this report could not name cannot be re-judged by its set, so
    // it is counted apart and stays counted; every other compile is filed under what it carried.
    if (unnamed)
        ++_unnamed;
    else
        ++_compiles[carried];
    RaiseLocked();
}

void RefusedArgumentsReport::AllowlistChanged(std::vector<std::string> nowAllowed)
{
    auto const guard = std::scoped_lock { _mutex };
    _allowed = std::move(nowAllowed);

    // Re-judged, never cleared by fiat: the arguments the new list admits leave the row and every
    // other stays. A change that admits something else -- an unrelated entry, a misspelled one --
    // must not read `clear`, which says "checked and benign" about an argument still refused.
    ArgumentSet admitted = 0;
    for (auto const slot: std::views::iota(std::size_t { 0 }, _slots.size()))
    {
        auto& named = _slots.at(slot);
        if (named.has_value() && std::ranges::all_of(named->judgedFor, [&](std::optional<Cc::Flavor> const driver) {
                return AdmittedBy(named->argument, driver, _allowed);
            }))
        {
            named.reset();
            admitted |= ArgumentSet { 1 } << slot;
        }
    }

    // A compile stays refused while ANY argument it carried still is, so the admitted ones leave
    // each set, and only a compile whose set empties leaves the count. Sets that become equal merge.
    std::map<ArgumentSet, std::uint64_t> stillRefused;
    for (auto const& [carried, compiles]: _compiles)
        if (auto const rest = carried & ~admitted; rest != 0)
            stillRefused[rest] += compiles;
    _compiles = std::move(stillRefused);

    if (_compiles.empty() && _unnamed == 0)
        _conditions.Clear(NodeCondition::RefusedCompileArguments);
    else
        RaiseLocked();
}

std::optional<std::size_t> RefusedArgumentsReport::FreeSlotLocked() const
{
    for (auto const slot: std::views::iota(std::size_t { 0 }, _slots.size()))
        if (!_slots.at(slot).has_value())
            return slot;
    return std::nullopt;
}

std::optional<std::size_t> RefusedArgumentsReport::SlotOfLocked(std::string_view argument) const
{
    for (auto const slot: std::views::iota(std::size_t { 0 }, _slots.size()))
        if (auto const& named = _slots.at(slot); named.has_value() && named->argument == argument)
            return slot;
    return std::nullopt;
}

void RefusedArgumentsReport::RaiseLocked()
{
    std::uint64_t refusals = _unnamed;
    for (auto const& [carried, compiles]: _compiles)
        refusals += compiles;

    // Named first seen first, whichever slot each landed in.
    std::vector<Refused const*> named;
    for (auto const& slot: _slots)
        if (slot.has_value())
            named.push_back(&*slot);
    std::ranges::sort(named, {}, &Refused::firstSeen);
    std::vector<std::string> arguments;
    arguments.reserve(named.size());
    for (auto const* entry: named)
        arguments.push_back(entry->argument);

    auto const lead =
        std::format("{} compile(s) refused over arguments this worker still will not pass to its compiler{}:",
                    refusals,
                    _unnamed > 0 ? std::format(" ({} of them over arguments not named)", _unnamed) : std::string {});
    _conditions.Raise(NodeCondition::RefusedCompileArguments, ListDetail(lead, arguments));
}

} // namespace FastCache::Node
