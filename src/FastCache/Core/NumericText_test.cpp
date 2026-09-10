// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/NumericText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <locale>
#include <string>
#include <string_view>

using FastCache::ParseFiniteDouble;
using namespace std::string_view_literals;

// `ParseFiniteDouble` was `RedisResp.cpp`'s private `ParseDouble` until it acquired a
// second caller (`fastcache-cli`'s RESP3 double). Its behaviour is covered from the
// wire in `RedisResp_test.cpp` -- `INCRBYFLOAT` on `1,5`, on `1e+308` and on `2.5e1`
// -- and that coverage is what makes the move safe. This file covers it DIRECTLY,
// which those cases cannot: they reach it through one caller that only ever hands it
// text a client sent, so the grammar's edges and the locale property are invisible
// there.

namespace
{
/// A `numpunct` whose decimal point is `,`.
///
/// Written rather than looked up: `std::locale("de_DE.UTF-8")` exists on some hosts
/// and not others, so a test resting on it either skips (proving nothing on the
/// runner that skipped) or is red for an environment reason. A facet is
/// constructible on every platform, which makes the locale property below an
/// assertion rather than a hope.
class CommaDecimalPoint final: public std::numpunct<char>
{
  protected:
    /// @return The decimal separator this facet imposes.
    [[nodiscard]] char do_decimal_point() const override
    {
        return ',';
    }
};

/// Restores the global locale on the way out, so a failing assertion cannot leave
/// the rest of the case running under a locale it did not ask for.
class ScopedGlobalLocale final
{
  public:
    /// @param locale The locale to install globally.
    explicit ScopedGlobalLocale(std::locale const& locale):
        _previous { std::locale::global(locale) }
    {
    }

    ScopedGlobalLocale(ScopedGlobalLocale const&) = delete;
    ScopedGlobalLocale(ScopedGlobalLocale&&) = delete;
    ScopedGlobalLocale& operator=(ScopedGlobalLocale const&) = delete;
    ScopedGlobalLocale& operator=(ScopedGlobalLocale&&) = delete;

    /// Puts the previous global locale back.
    ~ScopedGlobalLocale()
    {
        std::locale::global(_previous);
    }

  private:
    std::locale _previous;
};
} // namespace

TEST_CASE("NumericText: ParseFiniteDouble reads the whole numeric grammar", "[core][numeric][parse]")
{
    struct Case
    {
        std::string_view text;
        double expected;
    };

    // Every shape the grammar admits, including the two `strtod` accepts that a naive
    // reading of "digits, dot, digits" would not: a trailing dot and a leading one.
    static constexpr Case accepted[] = {
        { "0"sv, 0.0 },      { "-0"sv, 0.0 },     { "1"sv, 1.0 },      { "+1"sv, 1.0 },      { "-1"sv, -1.0 },
        { "1.5"sv, 1.5 },    { "-1.5"sv, -1.5 },  { "+1.5"sv, 1.5 },   { "3."sv, 3.0 },      { ".5"sv, 0.5 },
        { "-.5"sv, -0.5 },   { "2.5e1"sv, 25.0 }, { "2.5E1"sv, 25.0 }, { "1e+3"sv, 1000.0 }, { "1e-3"sv, 0.001 },
        { "1E-3"sv, 0.001 }, { "0.0"sv, 0.0 },    { "100"sv, 100.0 },  { "1e308"sv, 1e308 },
    };

    for (auto const& one: accepted)
    {
        double value = -12345.0;
        CHECK(ParseFiniteDouble(one.text, value));
        CHECK(value == one.expected);
    }
}

TEST_CASE("NumericText: ParseFiniteDouble refuses everything outside the grammar", "[core][numeric][parse]")
{
    // `1\0` and `1\0 2` are the reason a validation pass exists at all: a stream stops
    // at the NUL and reports success for a prefix, so a peer can hide bytes behind one.
    static std::string const embeddedNul { "1\0"
                                           "2",
                                           3 };
    static std::string const trailingNul { "1\0", 2 };

    std::string_view const refused[] = {
        ""sv,
        " "sv,
        " 1"sv,
        "1 "sv,
        "1\t"sv,
        "\n1"sv,
        "1,5"sv,
        "1.2.3"sv,
        "1e2e3"sv,
        "1e"sv,
        "e5"sv,
        "1e+"sv,
        "1e-"sv,
        ".e5"sv,
        "."sv,
        "-"sv,
        "+"sv,
        "--1"sv,
        "1-2"sv,
        "1+2"sv,
        "abc"sv,
        "0x10"sv,
        "1x"sv,
        "x1"sv,
        // The three RESP3 spellings of a non-finite: refused HERE by name, because a
        // caller that wants them matches the words itself rather than having a number
        // parser guess which of the two it has.
        "inf"sv,
        "-inf"sv,
        "nan"sv,
        "Infinity"sv,
        "NaN"sv,
        // Grammatically fine; not a finite double.
        "1e400"sv,
        "-1e400"sv,
        std::string_view { embeddedNul },
        std::string_view { trailingNul },
    };

    for (auto const& text: refused)
    {
        double value = -12345.0;
        CHECK_FALSE(ParseFiniteDouble(text, value));
        // The contract says the output is untouched on failure, and a caller that
        // ignores the `bool` is a different bug from one that reads a stale value.
        CHECK(value == -12345.0);
    }
}

TEST_CASE("NumericText: ParseFiniteDouble reads `.` whatever the host's locale says", "[core][numeric][locale]")
{
    // The distinguishing case for the `imbue`, and the only one in this file that
    // fails when it is removed: `istringstream` takes its facets from the GLOBAL
    // locale at construction, so on a host whose `LC_NUMERIC` is `de_DE.UTF-8` an
    // un-imbued stream reads `1.5` as `1` with `.5` left over -- rejecting the exact
    // format this project's own `std::format` writes.
    ScopedGlobalLocale const scoped { std::locale { std::locale::classic(), new CommaDecimalPoint } };

    // The facet really is in force, or the assertions below prove nothing: a locale
    // that failed to install leaves this case green under the very defect it covers.
    REQUIRE(std::use_facet<std::numpunct<char>>(std::locale {}).decimal_point() == ',');

    double value = 0.0;
    CHECK(ParseFiniteDouble("1.5"sv, value));
    CHECK(value == 1.5);

    CHECK(ParseFiniteDouble("-2.25e2"sv, value));
    CHECK(value == -225.0);

    // And the mirror: the separator the host prefers is still not a wire format here.
    double unchanged = -1.0;
    CHECK_FALSE(ParseFiniteDouble("1,5"sv, unchanged));
    CHECK(unchanged == -1.0);
}
