// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// The operand spelling that makes an option row path-valued.
///
/// One constant rather than a literal in each binary's coverage case: the two are
/// asking the same question of two tables, and a spelling one of them updated alone
/// would silently reduce that binary's guard to a vacuous pass over zero rows.
inline constexpr std::string_view PathOperand = "=<path>";

/// Where a binary's path-valued flags and its two classification tables disagree.
///
/// **Three lists rather than one, and that is the point.** "A flag nobody classified",
/// "a flag both tables claim" and "a table row naming no flag at all" are three
/// different mistakes with three different repairs, and a single `problems` vector --
/// or worse, a count -- is the state collapse this repository keeps paying for. A
/// failure has to say which one happened.
struct PathFlagCoverage
{
    /// `=<path>` rows in NEITHER table. The defect the guard exists for: a new
    /// secret-bearing flag added without anybody deciding whether it is one.
    std::vector<std::string> unclassified;

    /// `=<path>` rows in BOTH tables. A flag cannot be secret and public at once, and
    /// an XOR written as two independent `CHECK`s would let this pass unnoticed.
    std::vector<std::string> classifiedTwice;

    /// Table entries whose flag is in no `=<path>` option row. The other direction,
    /// and it is not decoration: a flag renamed in the option table -- or one whose
    /// operand stopped being `=<path>` -- leaves a row here covering nothing, and the
    /// file it used to cover goes unasked about with both tables still looking full.
    ///
    /// Judged against the PATH-VALUED rows rather than against every row, so the
    /// operand case is NAMED here instead of showing up only as `pathRows`
    /// disagreeing with the two table sizes, which names no flag at all.
    std::vector<std::string> namingNoRow;

    /// How many `=<path>` rows the option table actually had.
    ///
    /// **The positive control.** A scan that matched nothing leaves all three lists
    /// empty and reads exactly like complete coverage, so a caller asserts this is
    /// non-zero rather than concluding from the absence of findings.
    std::size_t pathRows { 0 };
};

/// The `--flag` spellings a classification table names, in table order.
///
/// A template over the row type because the two tables hold different rows -- a
/// `SecretFileRow<ConfigT, PathT>` and a `PublicPathFlag` -- and both spell the
/// key `flag`.
/// @param table Either classification table.
/// @return Its flags, in order.
template <typename Table>
[[nodiscard]] std::vector<std::string_view> FlagsOf(Table const& table)
{
    std::vector<std::string_view> flags;
    for (auto const& row: table)
        flags.push_back(row.flag);
    return flags;
}

/// Whether @p table names @p flag.
///
/// One predicate over either table rather than a lambda per Catch2 case: both
/// binaries' coverage cases ask it, and two spellings of "is this flag in that
/// table" is one more place for them to drift.
/// @param table Either classification table.
/// @param flag The `--flag` spelling to look for.
/// @return Whether a row names it.
template <typename Table>
[[nodiscard]] bool Names(Table const& table, std::string_view flag)
{
    return std::ranges::any_of(table, [flag](auto const& row) { return row.flag == flag; });
}

/// Which of @p options' path-valued rows @p secretFlags and @p publicFlags fail to
/// classify, and which of those flags name no row.
///
/// **One implementation for both binaries.** The daemon and the worker each keep a
/// secret-file table and a public-path table over their own option table, and the
/// rule joining the three is identical -- so it is written here rather than twice in
/// two Catch2 cases that would then drift on which directions they check
/// ([#864](https://github.com/LASTRADA-Software/fastcached/issues/864)). It lives in
/// `src/tests/` and not in the library because nothing in production asks it: it is a
/// property of the tables, checked at build time by whoever owns them.
///
/// @tparam Result The parse result the option table fills.
/// @param options The binary's own option table.
/// @param secretFlags The flags its secret-file table names.
/// @param publicFlags The flags its public-path table names.
/// @return What disagrees, plus how many path-valued rows were seen at all.
template <typename Result>
[[nodiscard]] PathFlagCoverage ClassifyPathFlags(std::span<OptionSpec<Result> const> options,
                                                 std::span<std::string_view const> secretFlags,
                                                 std::span<std::string_view const> publicFlags)
{
    auto const names = [](std::span<std::string_view const> flags, std::string_view flag) {
        return std::ranges::find(flags, flag) != flags.end();
    };

    PathFlagCoverage coverage;
    for (auto const& spec: options)
    {
        if (spec.operand != PathOperand)
            continue;
        ++coverage.pathRows;

        auto const secret = names(secretFlags, spec.primary);
        auto const known = names(publicFlags, spec.primary);
        if (secret && known)
            coverage.classifiedTwice.emplace_back(spec.primary);
        else if (!secret && !known)
            coverage.unclassified.emplace_back(spec.primary);
    }

    // Against the PATH-VALUED rows, not against every row. A flag whose operand
    // stopped being `=<path>` is the same defect as one that was renamed away -- the
    // file it used to cover goes unasked about -- and matching it against the whole
    // option table would leave it visible only as an arithmetic disagreement in
    // `pathRows`, which names nothing.
    auto const isAPathRow = [options](std::string_view flag) {
        return std::ranges::any_of(options,
                                   [flag](auto const& spec) { return spec.primary == flag && spec.operand == PathOperand; });
    };
    auto const collectStrays = [&coverage, &isAPathRow](std::span<std::string_view const> flags) {
        for (auto const& flag: flags)
            if (!isAPathRow(flag))
                coverage.namingNoRow.emplace_back(flag);
    };
    collectStrays(secretFlags);
    collectStrays(publicFlags);

    return coverage;
}

/// @param items Whatever a failing check wants to name.
/// @return Them, comma-separated, for a Catch2 `INFO`.
[[nodiscard]] inline std::string Join(std::vector<std::string> const& items)
{
    std::string joined;
    for (auto const& item: items)
    {
        if (!joined.empty())
            joined += ", ";
        joined += item;
    }
    return joined;
}

} // namespace FastCache::Testing
