// SPDX-License-Identifier: Apache-2.0
#include "CliAnswer.hpp"

#include <cstddef>

namespace FastCache::Cli
{

OutcomeSpec const* DescriptorOf(Outcome outcome) noexcept
{
    auto const index = static_cast<std::size_t>(outcome);
    if (index >= OutcomeTable.size())
        return nullptr;
    return &OutcomeTable[index];
}

int ExitCodeOf(Outcome outcome) noexcept
{
    auto const index = static_cast<std::size_t>(outcome);
    if (index < OutcomeTable.size())
        return OutcomeTable[index].code;
    // An outcome outside the table is a programming error rather than an operator's,
    // and `Usage` is the honest thing to claim about a command we cannot classify.
    // Indexed rather than reached through `DescriptorOf`, so there is no pointer here
    // that a reader -- or the analyser -- has to prove non-null.
    return OutcomeTable[static_cast<std::size_t>(Outcome::Usage)].code;
}

Answer Concluded(Outcome outcome, std::string advisory)
{
    Answer answer { .outcome = outcome };
    if (!advisory.empty())
        answer.advisories.push_back(std::move(advisory));
    return answer;
}

Answer Answered(Value value, Outcome outcome)
{
    return Answer { .value = std::move(value), .outcome = outcome };
}

} // namespace FastCache::Cli
