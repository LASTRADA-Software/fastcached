// SPDX-License-Identifier: Apache-2.0
#include "MarkerDiscovery.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <tests/Unwrap.hpp>

using FastCache::Cc::IMarkerDiscovery;
using FastCache::Cc::MarkerFromProbeOutput;
using FastCache::Cc::MarkerSource;
using FastCache::Cc::ResolveIncludeNoteMarker;

namespace
{
/// A discovery that answers whatever it was built with, and COUNTS being asked.
///
/// The count is not decoration: precedence is the property under test, and "the
/// operator's value won" and "the probe was never consulted" are different facts. A
/// fixture asserting only the returned string passes when an invocation spawns a
/// compiler it had no business spawning.
class ScriptedDiscovery: public IMarkerDiscovery
{
  public:
    explicit ScriptedDiscovery(std::optional<std::string> answer):
        _answer { std::move(answer) }
    {
    }

    [[nodiscard]] std::optional<std::string> Discover() override
    {
        ++_calls;
        return _answer;
    }

    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::optional<std::string> _answer;
    int _calls { 0 };
};

constexpr std::string_view ProbeHeader = R"(C:\Users\dev\AppData\Local\Temp\fastcache-cc-marker-1\fc-marker-probe.h)";
} // namespace

TEST_CASE("A note's prefix is read back out of the probe's own output")
{
    // The English case, and the one that proves nothing on its own: an English `cl`
    // discovers English, which is what the default already said. It is here as the
    // control for the localized case below, which is the case that matters and the one
    // no machine in this project can produce (#878).
    std::string const output = std::string { "Note: including file: " } + std::string { ProbeHeader } + "\n";
    auto const learned = MarkerFromProbeOutput(output, ProbeHeader);
    REQUIRE(learned.has_value());
    CHECK(FastCache::Testing::Unwrap(learned) == FastCache::PathCanon::IncludeNoteMarker);
}

TEST_CASE("A localized note's prefix is read back, which is the whole point")
{
    // What a German `cl` prints. Synthetic: both toolsets installed on this project's
    // Windows host carry resource directory 1033 only, and `VSLANG` selects among
    // INSTALLED language packs rather than conjuring one -- measured at 1033, 1031,
    // 1041 and 2052, byte-identical English from both. So the string below is the
    // shape, not a capture, and this case asserts the PARSE rather than the toolchain.
    std::string const output = std::string { "Hinweis: Einlesen der Datei: " } + std::string { ProbeHeader } + "\r\n";
    auto const learned = MarkerFromProbeOutput(output, ProbeHeader);
    REQUIRE(learned.has_value());
    CHECK(FastCache::Testing::Unwrap(learned) == "Hinweis: Einlesen der Datei:");
}

TEST_CASE("The depth padding is trimmed off what is learned")
{
    // The measured hazard, and the reason this is not a plain substring subtraction.
    // `cl` renders inclusion depth as blanks BETWEEN the marker and the path -- one per
    // level, marker at column zero, measured on MSVC 14.44.35207 and 14.51.36231 and on
    // clang-cl, under /c and /EP, at depths one to six. A derivation that kept them
    // would learn a prefix whose length depends on which note it happened to read, so
    // two invocations of the same launcher against the same compiler would disagree.
    //
    // Distinguishing rather than merely shaped: an implementation that forgot the trim
    // answers `Note: including file:` followed by four spaces, which compares unequal
    // here AND is a prefix no `RewriteIncludeNoteMarker` on a depth-zero note matches.
    std::string const output = std::string { "Note: including file:    " } + std::string { ProbeHeader } + "\n";
    auto const learned = MarkerFromProbeOutput(output, ProbeHeader);
    REQUIRE(learned.has_value());
    CHECK(FastCache::Testing::Unwrap(learned) == FastCache::PathCanon::IncludeNoteMarker);
}

TEST_CASE("A line with blanks in front of the marker teaches nothing")
{
    // The #1270 anchor, on the writing side. A note begins at column zero, so a
    // candidate with anything in front of it is refused rather than trimmed -- trimming
    // would learn a prefix from a line no reader in this tree would then call a note.
    std::string const output = std::string { "  Note: including file: " } + std::string { ProbeHeader } + "\n";
    CHECK_FALSE(MarkerFromProbeOutput(output, ProbeHeader).has_value());
}

TEST_CASE("A bare path line teaches nothing, because it names no prefix")
{
    std::string const output = std::string { ProbeHeader } + "\n";
    CHECK_FALSE(MarkerFromProbeOutput(output, ProbeHeader).has_value());
}

TEST_CASE("Two candidates that disagree teach nothing rather than one of them")
{
    // The rule matched something that is not a note, so the honest answer is silence.
    // A wrong marker is worse than no marker: it normalizes nothing on the way in and
    // re-spells a prefix nobody looks for on the way out, both silently.
    std::string const output = std::string { "Note: including file: " } + std::string { ProbeHeader } + "\n" + "see "
                               + std::string { ProbeHeader } + "\n";
    CHECK_FALSE(MarkerFromProbeOutput(output, ProbeHeader).has_value());
}

TEST_CASE("Two candidates that AGREE still teach, which is the ordinary deep case")
{
    // The accepting direction of the case above, and not decoration: a rule that
    // refused any second candidate would pass that one and break every real probe,
    // since `cl` emits a note per header and a real TU has more than one.
    std::string const output = std::string { "Note: including file: " } + std::string { ProbeHeader } + "\n"
                               + "Note: including file:  " + std::string { ProbeHeader } + "\n";
    auto const learned = MarkerFromProbeOutput(output, ProbeHeader);
    REQUIRE(learned.has_value());
    CHECK(FastCache::Testing::Unwrap(learned) == FastCache::PathCanon::IncludeNoteMarker);
}

TEST_CASE("A path the driver spelled differently is still recognised")
{
    // A driver echoes a path as IT resolved it, so the case and the separators can
    // differ from what the launcher handed it. More permissive than a byte compare, and
    // justified only because this probe runs on the MSVC family -- where those two
    // spellings genuinely name one file.
    std::string const echoed = R"(c:/users/dev/appdata/local/temp/fastcache-cc-marker-1/FC-MARKER-PROBE.H)";
    std::string const output = "Note: including file: " + echoed + "\n";
    auto const learned = MarkerFromProbeOutput(output, ProbeHeader);
    REQUIRE(learned.has_value());
    CHECK(FastCache::Testing::Unwrap(learned) == FastCache::PathCanon::IncludeNoteMarker);
}

TEST_CASE("A stream naming a DIFFERENT path teaches nothing")
{
    // The control the case above needs. Without it, "folds case and separators" and
    // "matches any line ending in any path" are one passing assertion.
    std::string const output = R"(Note: including file: C:\Windows\Kits\10\um\windows.h)"
                               "\n";
    CHECK_FALSE(MarkerFromProbeOutput(output, ProbeHeader).has_value());
}

TEST_CASE("An empty stream and an empty known path teach nothing")
{
    CHECK_FALSE(MarkerFromProbeOutput("", ProbeHeader).has_value());
    CHECK_FALSE(MarkerFromProbeOutput("Note: including file: x\n", "").has_value());
}

TEST_CASE("The operator's value outranks a probe, and the probe is not even asked")
{
    ScriptedDiscovery discovery { std::string { "Hinweis: Einlesen der Datei:" } };
    auto const resolved = ResolveIncludeNoteMarker("Anmerkung: Datei:", &discovery);
    CHECK(resolved.marker == "Anmerkung: Datei:");
    CHECK(resolved.source == MarkerSource::Operator);
    // The half an assertion on the string alone cannot make: a build that named its own
    // prefix must not pay for a compiler spawn to be told something it will discard.
    CHECK(discovery.Calls() == 0);
}

TEST_CASE("The discovery request is an ASK, not a prefix")
{
    // `FASTCACHE_MSVC_DEPS_PREFIX=auto` is how an operator opts IN to the probe, and it
    // names no prefix -- so the `Operator` row must decline it exactly as it declines an
    // unset variable, or the build would emit notes prefixed with the literal `auto`.
    //
    // Read HERE rather than mapped at the call site: one value, one reading. A caller
    // that turned the sentinel into an empty string itself would be a second place for
    // the meaning to drift, and the drift is silent in the direction that matters.
    ScriptedDiscovery discovery { std::string { "Hinweis: Einlesen der Datei:" } };
    auto const resolved = ResolveIncludeNoteMarker(FastCache::Cc::MarkerDiscoveryRequest, &discovery);
    CHECK(resolved.marker == "Hinweis: Einlesen der Datei:");
    CHECK(resolved.source == MarkerSource::Discovered);
    CHECK(discovery.Calls() == 1);

    // And with no probe handed in it is the DEFAULT, never the sentinel: an operator who
    // asked for discovery on a build that cannot do it gets English, not `auto`.
    auto const withoutProbe = ResolveIncludeNoteMarker(FastCache::Cc::MarkerDiscoveryRequest, nullptr);
    CHECK(withoutProbe.marker == FastCache::PathCanon::IncludeNoteMarker);
    CHECK(withoutProbe.source == MarkerSource::Default);
}

TEST_CASE("A probe answers when the operator named nothing")
{
    ScriptedDiscovery discovery { std::string { "Hinweis: Einlesen der Datei:" } };
    auto const resolved = ResolveIncludeNoteMarker("", &discovery);
    CHECK(resolved.marker == "Hinweis: Einlesen der Datei:");
    CHECK(resolved.source == MarkerSource::Discovered);
    CHECK(discovery.Calls() == 1);
}

TEST_CASE("A probe that would not say falls back to English and SAYS it was asked")
{
    // Fails OPEN, and the direction is safe as far as it goes: an unmatched marker
    // rewrites nothing, so such a build is where it was rather than worse.
    ScriptedDiscovery declines { std::nullopt };
    auto const fromDeclined = ResolveIncludeNoteMarker("", &declines);
    CHECK(fromDeclined.marker == FastCache::PathCanon::IncludeNoteMarker);
    CHECK(declines.Calls() == 1);

    // The assertion that distinguishes, and the reason the marker check above cannot be
    // the whole case: asked-and-declined carries the SAME string as never-asked, so a
    // build that collapsed the two passes every assertion about the value. What separates
    // them is the remedy each sends an operator to -- and reaching here means they wrote
    // `FASTCACHE_MSVC_DEPS_PREFIX=auto`, so `override with FASTCACHE_MSVC_DEPS_PREFIX` is
    // advice they have already taken.
    CHECK(fromDeclined.source == MarkerSource::DiscoveryDeclined);
    CHECK(fromDeclined.source != MarkerSource::Default);

    // An empty answer is the same answer. A prefix that matches every line is not one
    // Ninja could match a note against, so it must not reach the caller as a value.
    ScriptedDiscovery empty { std::string {} };
    CHECK(ResolveIncludeNoteMarker("", &empty).source == MarkerSource::DiscoveryDeclined);
}

TEST_CASE("Never asked and asked-and-declined are told apart, and read differently")
{
    // The control the case above needs. Both arms answer the English marker, so a
    // fixture reading only `marker` cannot fail in either direction -- this one asserts
    // the two provenances are different values AND that an operator meets two different
    // sentences, since the label is the entire user-visible consequence.
    ScriptedDiscovery declines { std::nullopt };
    auto const asked = ResolveIncludeNoteMarker("", &declines);
    auto const neverAsked = ResolveIncludeNoteMarker("", nullptr);

    REQUIRE(asked.marker == neverAsked.marker); // The premise: the VALUE cannot separate them.
    CHECK(asked.source != neverAsked.source);
    CHECK(FastCache::Cc::MarkerSourceLabel(asked.source) != FastCache::Cc::MarkerSourceLabel(neverAsked.source));

    // And the never-asked probe really was never asked -- a fixture that consulted it
    // anyway would satisfy every line above while spawning a compiler nobody asked for.
    CHECK(declines.Calls() == 1);
}

TEST_CASE("No probe at all is the English default, without a crash")
{
    // What an invocation that must not spawn gets: a link step, a compile that deals in
    // no `/showIncludes`, or a GNU driver.
    auto const resolved = ResolveIncludeNoteMarker("", nullptr);
    CHECK(resolved.marker == FastCache::PathCanon::IncludeNoteMarker);
    CHECK(resolved.source == MarkerSource::Default);
}
