// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Server/DaemonStartupGate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using FastCache::CliOutcome;

namespace
{

/// The configuration `fastcached` assembles from @p args and no file.
/// @param args The command line, program name excluded.
/// @return The assembly; the case has failed when it does not assemble.
[[nodiscard]] FastCache::EffectiveConfig AssembledFrom(std::vector<std::string> args)
{
    auto assembled = FastCache::AssembleEffectiveConfig({}, FastCache::ConfigSources { .args = std::move(args) });
    REQUIRE(assembled.has_value());
    return *std::move(assembled);
}

/// One outcome, and whether the serving rules may refuse it.
struct OutcomeVerdict
{
    CliOutcome outcome; ///< What the command line asked for.
    bool judged;        ///< Whether a line the rules refuse is refused for it.
};

/// Every outcome, written out: a start and an install are held to the serving rules, and a verb
/// that acts on something else and exits is not.
constexpr auto Verdicts = std::to_array<OutcomeVerdict>({
    { .outcome = CliOutcome::Run, .judged = true },
    { .outcome = CliOutcome::ShowHelp, .judged = false },
    { .outcome = CliOutcome::ShowVersion, .judged = false },
    { .outcome = CliOutcome::InstallService, .judged = true },
    { .outcome = CliOutcome::UninstallService, .judged = false },
    { .outcome = CliOutcome::HealthCheck, .judged = false },
    { .outcome = CliOutcome::SeedConfig, .judged = false },
    { .outcome = CliOutcome::MigrateStorage, .judged = false },
});

} // namespace

TEST_CASE("Only a start and an install are refused by the serving rules, each rule by name", "[config][startup][gate]")
{
    // Every outcome has a verdict here, so one added later without a decision fails.
    for (auto const outcome: FastCache::Enumerators<CliOutcome>())
        CHECK(std::ranges::count(Verdicts, outcome, &OutcomeVerdict::outcome) == 1);

    struct Line
    {
        std::vector<std::string> args; ///< What the operator typed.
        std::string_view refusal;      ///< Text the refusal must contain.
    };
    auto const lines = std::vector<Line> {
        { .args = { "--bind=1.2.3.4", "--listen=1.2.3.4:6379" }, .refusal = "--bind" },
        { .args = { "--notify-keyspace-events=KZ" }, .refusal = "invalid --notify-keyspace-events 'KZ'" },
        { .args = { "--listen=127.0.0.1:6379", "--listen=127.0.0.1:6379" },
          .refusal = "duplicate listener endpoint 127.0.0.1:6379" },
    };
    for (auto const& line: lines)
    {
        INFO("refusal: " << line.refusal);
        auto const assembled = AssembledFrom(line.args);
        for (auto const& verdict: Verdicts)
        {
            INFO("outcome " << static_cast<int>(verdict.outcome));
            auto const rejection = FastCache::ServingRulesRejection(verdict.outcome, assembled);
            CHECK(rejection.has_value() == verdict.judged);
            if (verdict.judged && rejection.has_value())
                CHECK(FastCache::Testing::Unwrap(rejection).contains(line.refusal));
        }
    }

    SECTION("the control: a line every rule accepts proceeds for every outcome")
    {
        auto const assembled = AssembledFrom({ "--listen=127.0.0.1:6379", "--notify-keyspace-events=KEA" });
        for (auto const& verdict: Verdicts)
            CHECK_FALSE(FastCache::ServingRulesRejection(verdict.outcome, assembled).has_value());
    }
}

TEST_CASE("fastcached's main asks the serving rules through the gate and nowhere else", "[config][startup][gate]")
{
    // `main` is in no test target, so what holds it to the gate is its text: one call, and none of
    // the three rules asked beside it, where an outcome the gate excuses would meet them anyway.
    auto const path = std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcached" / "main.cpp";
    REQUIRE(std::filesystem::exists(path));
    // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
    std::ifstream in { path, std::ios::binary };
    REQUIRE(in);
    std::ostringstream contents;
    contents << in.rdbuf();
    auto const text = std::move(contents).str();

    /// How often @p needle occurs in `text`; an empty needle counts nothing.
    auto const occurrences = [&text](std::string_view needle) {
        auto count = std::size_t { 0 };
        if (needle.empty())
            return count;
        auto at = text.find(needle);
        while (at != std::string::npos)
        {
            ++count;
            at = text.find(needle, at + needle.size());
        }
        return count;
    };
    CHECK(occurrences("ServingRulesRejection(parsed->outcome, *assembled)") == 1);
    CHECK(occurrences("ValidateBindFlagShape(") == 0);
    CHECK(occurrences("DaemonStartupRejection(") == 0);
    CHECK(occurrences("JudgedByServingRules(") == 0);
}
