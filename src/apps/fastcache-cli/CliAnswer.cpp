// SPDX-License-Identifier: Apache-2.0
#include "CliAnswer.hpp"

#include <algorithm>
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

void PrependRemarks(Answer& answer, std::span<std::string const> remarks)
{
    auto said = std::vector<std::string> {};
    said.reserve(remarks.size() + answer.advisories.size());
    auto const sayOnce = [&said](std::string const& remark) {
        if (std::ranges::find(said, remark) == said.end())
            said.push_back(remark);
    };
    std::ranges::for_each(remarks, sayOnce);
    std::ranges::for_each(answer.advisories, sayOnce);
    answer.advisories = std::move(said);
}

} // namespace FastCache::Cli
