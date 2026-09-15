// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// @file MachineName.hpp
/// The one spelling a name a PROGRAM reads is written in: kebab-case.
///
/// **One predicate for every table of such names**, because two tables with a spelling rule each are two rules. The
/// leader's fleet columns were kebab-case (`cpu-busy`, `raft-endpoint`) while the CLI's piped keys were snake_case
/// (`cpu_busy_ratio`), so a script reading both had to know which surface it was on (#1445). Each table's names are
/// `static_assert`ed through here, so a row spelled any other way fails the build.

/// Whether the name @p parts join into is kebab-case: runs of `a`-`z` and `0`-`9`, each separated from the next by
/// exactly one `-`, with none at either end.
///
/// **The parts of a COMPOSED name** -- a tier, a separator, a column -- are judged as the one string they join
/// into, so a name no table holds whole (`memory-items`) meets the same walk as one that is, with nothing
/// allocated and inside a constant expression.
/// @param parts The pieces, in order; the name is their concatenation.
/// @return True when that name is non-empty and kebab-case.
[[nodiscard]] constexpr bool IsKebabJoin(std::initializer_list<std::string_view> parts) noexcept
{
    auto named = false;
    auto afterHyphen = true; // At the start, so a leading `-` is refused as a doubled one is.
    for (auto const part: parts)
        for (auto const c: part)
        {
            if (c == '-')
            {
                if (afterHyphen)
                    return false;
                afterHyphen = true;
                continue;
            }
            if ((c < 'a' || c > 'z') && (c < '0' || c > '9'))
                return false;
            afterHyphen = false;
            named = true;
        }
    return named && !afterHyphen;
}

/// Whether @p name is kebab-case.
/// @param name The name.
/// @return As `IsKebabJoin` over the one part.
[[nodiscard]] constexpr bool IsKebabName(std::string_view name) noexcept
{
    return IsKebabJoin({ name });
}

/// Every name one table gives a program, joined as that program reads them, under what the table is called.
struct MachineNameTable
{
    std::string_view table {};         ///< What the table is called, for a refusal to name; static storage.
    std::vector<std::string> names {}; ///< Its names, a composed one as it is joined.
};

/// The first name in @p tables that is not kebab-case, as a refusal naming its table.
///
/// The RUN-TIME half of the rule each table `static_assert`s: a table's own assertion can only be watched refusing
/// by breaking the build, so this is what a case plants a misspelled name into -- one table at a time -- to see the
/// refusal, and to see that it says WHICH table.
/// @param tables The tables, in the order to report them.
/// @return Nothing when every name is kebab-case; otherwise the table, the name, and the rule it breaks.
[[nodiscard]] inline std::optional<std::string> MisspelledMachineName(std::span<MachineNameTable const> tables)
{
    for (auto const& table: tables)
        for (auto const& name: table.names)
            if (!IsKebabName(name))
                return std::format("table `{}` names `{}`, which is not kebab-case", table.table, name);
    return std::nullopt;
}

} // namespace FastCache
