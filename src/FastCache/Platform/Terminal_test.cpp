// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{
/// Set the NO_COLOR environment variable for the duration of a test,
/// restoring (clearing) it on destruction. Env mutation is process-local
/// and each Catch test case runs in its own ctest process, so this stays
/// isolated.
struct ScopedNoColor
{
    ScopedNoColor()
    {
#if defined(_WIN32)
        ::_putenv_s("NO_COLOR", "1");
#else
        ::setenv("NO_COLOR", "1", /*overwrite=*/1);
#endif
    }

    ~ScopedNoColor()
    {
#if defined(_WIN32)
        ::_putenv_s("NO_COLOR", "");
#else
        ::unsetenv("NO_COLOR");
#endif
    }

    ScopedNoColor(ScopedNoColor const&) = delete;
    ScopedNoColor& operator=(ScopedNoColor const&) = delete;
    ScopedNoColor(ScopedNoColor&&) = delete;
    ScopedNoColor& operator=(ScopedNoColor&&) = delete;
};

/// A terminal that answers however a case tells it to.
///
/// The whole point of the `ITerminalChannel` seam. Every ending a real terminal
/// can produce is a scripted step here, including the three no real terminal can
/// be made to produce on demand: never answering, closing mid-reply, and
/// answering late but inside the budget.
///
/// Each step ADVANCES THE INJECTED CLOCK before it answers, which is what makes
/// "late" mean anything without a real sleep -- and what lets a case assert the
/// MEASURED elapsed rather than the budget that was requested.
class ScriptedTerminal final: public FastCache::ITerminalChannel
{
  public:
    struct Step
    {
        std::chrono::milliseconds takes { 0 };     ///< How long this read appears to take.
        FastCache::TerminalReadOutcome outcome {}; ///< How it ends.
        std::string bytes {};                      ///< What it hands back.
    };

    ScriptedTerminal(FastCache::ManualClock& clock, bool interactive, std::vector<Step> steps):
        _clock { clock },
        _interactive { interactive },
        _steps { std::move(steps) }
    {
    }

    [[nodiscard]] bool IsInteractive() const noexcept override
    {
        return _interactive;
    }

    [[nodiscard]] bool Write(std::string_view bytes) override
    {
        _written.emplace_back(bytes);
        return !_writeFails;
    }

    [[nodiscard]] FastCache::TerminalRead Read(std::chrono::milliseconds budget) override
    {
        _budgets.push_back(budget);

        // Out of script: behave like a terminal that has gone quiet, spending the
        // whole remaining budget. A fake that answered something here would make
        // every "never answers" case pass for the wrong reason.
        if (_next >= _steps.size())
        {
            _clock.Advance(budget);
            return { .outcome = FastCache::TerminalReadOutcome::Timeout, .bytes = {} };
        }

        Step const& step = _steps.at(_next);
        ++_next;

        // A step that would outlast the budget is a TIMEOUT, exactly as a real
        // read is: the fake must not hand back bytes that arrived after the
        // deadline, or it would test a driver that cannot be written.
        if (step.takes > budget)
        {
            _clock.Advance(budget);
            return { .outcome = FastCache::TerminalReadOutcome::Timeout, .bytes = {} };
        }

        _clock.Advance(step.takes);
        return { .outcome = step.outcome, .bytes = step.bytes };
    }

    /// Every query written, in order. What a case asserts to see that the seam
    /// wrote nothing at a pipe, and that the two queries went out in table order.
    /// @return The queries.
    [[nodiscard]] std::vector<std::string> const& Written() const noexcept
    {
        return _written;
    }

    /// The budget each read was handed, in order. A shrinking sequence is what
    /// proves the deadline was carried rather than re-armed per read.
    /// @return The budgets.
    [[nodiscard]] std::vector<std::chrono::milliseconds> const& Budgets() const noexcept
    {
        return _budgets;
    }

    /// Make `Write` report failure, so the query never leaves.
    void FailWrites() noexcept
    {
        _writeFails = true;
    }

  private:
    FastCache::ManualClock& _clock;
    std::vector<std::string> _written;
    std::vector<std::chrono::milliseconds> _budgets;
    bool _writeFails = false;
    bool _interactive;
    std::vector<Step> _steps;
    std::size_t _next = 0;
};

/// A step that hands back bytes immediately.
/// @param bytes What the terminal says.
/// @param takes How long it appears to take.
/// @return The step.
[[nodiscard]] ScriptedTerminal::Step Says(std::string bytes, std::chrono::milliseconds takes = 0ms)
{
    return { .takes = takes, .outcome = FastCache::TerminalReadOutcome::Bytes, .bytes = std::move(bytes) };
}

/// A step where the channel ends.
/// @param takes How long it appears to take.
/// @return The step.
[[nodiscard]] ScriptedTerminal::Step Closes(std::chrono::milliseconds takes = 0ms)
{
    return { .takes = takes, .outcome = FastCache::TerminalReadOutcome::Closed, .bytes = {} };
}

/// A DA1 reply listing sixel (parameter 4).
constexpr std::string_view Da1WithSixel = "\x1b[?62;4;6;22c";
/// A DA1 reply listing no sixel.
constexpr std::string_view Da1WithoutSixel = "\x1b[?62;6;22c";

} // namespace

// ---------------------------------------------------------------------------
// The original colour predicate, unchanged
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: StdoutSupportsColor honors NO_COLOR", "[platform][terminal][color]")
{
    ScopedNoColor const guard;
    // With NO_COLOR set, color must be suppressed regardless of TTY state.
    REQUIRE_FALSE(FastCache::StdoutSupportsColor());
}

TEST_CASE("Terminal: StdoutSupportsColor is false for a non-terminal stdout", "[platform][terminal][color]")
{
    // The test runner's stdout is a pipe/file under ctest, never an interactive
    // terminal, so color detection must report false (and must not crash).
    REQUIRE_FALSE(FastCache::StdoutSupportsColor());
}

// ---------------------------------------------------------------------------
// The answer vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: exactly one answer permits use, and every row says what it means", "[platform][terminal][capabilities]")
{
    // The static_assert in the header already pins the count at compile time.
    // This walks the table for the REST of the row's contract, which no
    // static_assert covers: a row with an empty diagnosis is a state nobody can
    // act on, and two rows sharing a key make a diagnostic ambiguous.
    std::vector<std::string_view> keys;
    std::size_t permitting = 0;
    for (auto const& row: FastCache::TerminalQueryAnswerTable)
    {
        CHECK_FALSE(row.key.empty());
        CHECK_FALSE(row.diagnosis.empty());
        keys.push_back(row.key);
        if (row.permitsUse)
            ++permitting;
    }
    CHECK(permitting == 1);

    std::ranges::sort(keys);
    CHECK(std::ranges::adjacent_find(keys) == keys.end());

    // The one that permits is `Yes`, named rather than inferred from the count.
    CHECK(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::Yes));
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::No));
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::NotAsked));
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::NoReply));
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::Closed));
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::Refused));
}

// ---------------------------------------------------------------------------
// The reply grammar -- pure, no clock and no channel
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a DA1 reply is parsed and its parameters are readable", "[platform][terminal][da1]")
{
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed(Da1WithSixel) == FastCache::CsiDecodeState::Complete);
    CHECK(decoder.Reply().finalByte == 'c');
    CHECK(decoder.Reply().privateMarker == '?');
    CHECK(decoder.Reply().intermediate == '\0');
    CHECK(decoder.Reply().HasParameter(4));
    CHECK(decoder.Reply().HasParameter(62));
    CHECK_FALSE(decoder.Reply().HasParameter(9));
}

TEST_CASE("Terminal: a DA1 reply without sixel parses and does not carry parameter 4", "[platform][terminal][da1]")
{
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed(Da1WithoutSixel) == FastCache::CsiDecodeState::Complete);
    // The DISTINGUISHING assertion: it parsed (so this is not a malformed-reply
    // pass) and the parameter is absent. Asserting only "not Complete-with-4"
    // would also pass for a reply that failed to parse at all.
    CHECK(decoder.Reply().finalByte == 'c');
    CHECK_FALSE(decoder.Reply().HasParameter(4));
    CHECK(decoder.Reply().HasParameter(62));
}

TEST_CASE("Terminal: a reply split across reads parses the same as one arriving whole", "[platform][terminal][da1]")
{
    // A real terminal's reply arrives in whatever chunks the tty hands over, and
    // a decoder that only works on a whole reply works on a developer's machine
    // and fails over ssh.
    FastCache::CsiReplyDecoder decoder;
    for (std::size_t i = 0; i + 1 < Da1WithSixel.size(); ++i)
        REQUIRE(decoder.Feed(Da1WithSixel.substr(i, 1)) == FastCache::CsiDecodeState::NeedMore);
    REQUIRE(decoder.Feed(Da1WithSixel.substr(Da1WithSixel.size() - 1)) == FastCache::CsiDecodeState::Complete);
    CHECK(decoder.Reply().HasParameter(4));
}

TEST_CASE("Terminal: bytes ahead of the reply are skipped rather than refused", "[platform][terminal][da1]")
{
    // A terminal hands back what the operator typed as well. Refusing a stray
    // keystroke would turn a keypress into "this terminal has no sixel", which
    // is a wrong answer produced by an unrelated event.
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed("q\n\x1b\x1b[?62;4c") == FastCache::CsiDecodeState::Complete);
    CHECK(decoder.Reply().HasParameter(4));
}

TEST_CASE("Terminal: a byte the CSI grammar does not allow is Malformed", "[platform][terminal][da1]")
{
    // The grammar, not a guess at it. ECMA-48 gives CSI three byte classes:
    // parameters 0x30-0x3F, intermediates 0x20-0x2F, and a FINAL byte anywhere
    // in 0x40-0x7E. A C0 control in the parameter position is in none of them.
    //
    // The first draft of this case fed `\x1b[?6z2c` and asserted Malformed, on
    // the reasoning that "a letter where a parameter belongs is not a CSI
    // reply". That is false -- `z` is 0x7A, a perfectly good final byte, so the
    // sequence is well formed and the decoder was right to say Complete. The
    // test was wrong, and loosening the decoder to satisfy it would have taught
    // it to refuse valid sequences. The case below carries what that reasoning
    // was actually reaching for.
    FastCache::CsiReplyDecoder decoder;
    CHECK(decoder.Feed("\x1b[?6\a2c") == FastCache::CsiDecodeState::Malformed);
}

TEST_CASE("Terminal: a well-formed CSI sequence that is not OUR reply parses, and the driver refuses it",
          "[platform][terminal][da1]")
{
    // Two layers, two answers, and keeping them apart is the point. The GRAMMAR
    // is satisfied: `ESC [ ? 6 z` terminates on a legal final byte. Whether it
    // answers the question we asked is not the decoder's business -- it is the
    // driver's, which compares the final byte against the query's own row.
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed("\x1b[?6z") == FastCache::CsiDecodeState::Complete);
    CHECK(decoder.Reply().finalByte == 'z');

    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?6z", 1ms) } };
    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);
    CHECK(result.answer == FastCache::TerminalQueryAnswer::Refused);
}

TEST_CASE("Terminal: a reply that never terminates is refused at a bound", "[platform][terminal][da1]")
{
    // The bytes come from outside the process, so "wait for the terminator" has
    // to have an end. Both bounds are driven, because they catch different
    // shapes: many parameters, and one impossibly long parameter.
    SECTION("too many parameters")
    {
        std::string reply = "\x1b[?";
        for (std::size_t i = 0; i < FastCache::MaxCsiReplyParameters + 4; ++i)
            reply += "1;";
        FastCache::CsiReplyDecoder decoder;
        CHECK(decoder.Feed(reply) == FastCache::CsiDecodeState::Malformed);
    }
    SECTION("one parameter of unbounded length")
    {
        std::string reply = "\x1b[?";
        reply += std::string(FastCache::MaxCsiReplyBytes + 8, '9');
        FastCache::CsiReplyDecoder decoder;
        CHECK(decoder.Feed(reply) == FastCache::CsiDecodeState::Malformed);
    }
}

TEST_CASE("Terminal: a DECRPM reply parses its intermediate and final bytes", "[platform][terminal][decrqm]")
{
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed("\x1b[?2026;2$y") == FastCache::CsiDecodeState::Complete);
    CHECK(decoder.Reply().intermediate == '$');
    CHECK(decoder.Reply().finalByte == 'y');
    REQUIRE(decoder.Reply().Parameters().size() == 2);
    CHECK(decoder.Reply().Parameters()[0] == 2026);
    CHECK(decoder.Reply().Parameters()[1] == 2);
}

TEST_CASE("Terminal: an omitted parameter is the default rather than absent", "[platform][terminal][da1]")
{
    // `1;;3` is three parameters in CSI, the middle one defaulted. A decoder
    // that dropped it would shift every later parameter left by one, which for a
    // POSITIONAL reply like DECRPM changes the answer.
    FastCache::CsiReplyDecoder decoder;
    REQUIRE(decoder.Feed("\x1b[?1;;3c") == FastCache::CsiDecodeState::Complete);
    REQUIRE(decoder.Reply().Parameters().size() == 3);
    CHECK(decoder.Reply().Parameters()[1] == 0);
    CHECK(decoder.Reply().Parameters()[2] == 3);
}

// ---------------------------------------------------------------------------
// Putting the question -- scripted channel, manual clock, no terminal
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a terminal that reports sixel answers Yes", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says(std::string { Da1WithSixel }, 3ms) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(FastCache::PermitsUse(result.answer));
    CHECK(result.elapsed == 3ms);
    REQUIRE(terminal.Written().size() == 1);
    CHECK(terminal.Written().front() == "\x1b[c");
}

TEST_CASE("Terminal: answering no sixel and never answering are DIFFERENT answers", "[platform][terminal][da1]")
{
    // THE clause this seam exists for. Both end in "do not draw sixel", so a
    // `bool` record makes them one fact -- and they are not: the first is a
    // terminal that told us, the second may support sixel perfectly and sit
    // behind a slow link. A case asserting only `!PermitsUse` on both passes
    // under exactly the collapse this is written to refuse.
    FastCache::ManualClock answering;
    ScriptedTerminal saysNo { answering, true, { Says(std::string { Da1WithoutSixel }, 2ms) } };
    auto const no = FastCache::RunTerminalQuery(saysNo, answering, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    FastCache::ManualClock silent;
    ScriptedTerminal saysNothing { silent, true, {} };
    auto const quiet =
        FastCache::RunTerminalQuery(saysNothing, silent, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(no.answer == FastCache::TerminalQueryAnswer::No);
    CHECK(quiet.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(no.answer != quiet.answer);

    // Both refuse the capability -- that is the safe direction and it must hold.
    CHECK_FALSE(FastCache::PermitsUse(no.answer));
    CHECK_FALSE(FastCache::PermitsUse(quiet.answer));

    // And their diagnoses differ, which is what an operator reads.
    CHECK(FastCache::SpecFor(no.answer).diagnosis != FastCache::SpecFor(quiet.answer).diagnosis);
}

TEST_CASE("Terminal: a silent terminal expires at its budget and reports the MEASURED elapsed", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, {} };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 250ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::NoReply);
    // The elapsed is what the clock recorded, not the number that was requested.
    // Those coincide here by construction; the case below is where they part.
    CHECK(result.elapsed == 250ms);
}

TEST_CASE("Terminal: an answer that is late but inside the budget is still an answer", "[platform][terminal][da1]")
{
    // The direction a too-tight deadline gets wrong. The reply arrives at 240 ms
    // of a 500 ms budget -- a plausible trans-continental round trip -- and must
    // be honoured rather than discarded.
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says(std::string { Da1WithSixel }, 240ms) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(result.elapsed == 240ms);
    // MEASURED rather than requested: the two differ here, which is the whole
    // reason the field is not simply the budget.
    CHECK(result.elapsed < 500ms);
}

TEST_CASE("Terminal: a channel that closes mid-reply is Closed, not NoReply", "[platform][terminal][da1]")
{
    // A terminal that went away is diagnosed and fixed somewhere else entirely
    // from one that is merely slow, so folding the two would send a reader to
    // the budget for a problem the budget cannot cause.
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?62;", 1ms), Closes(1ms) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Closed);
    CHECK(result.answer != FastCache::TerminalQueryAnswer::NoReply);
    CHECK(result.elapsed == 2ms);
}

TEST_CASE("Terminal: garbage is Refused, and Refused is not No", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?6z2c", 1ms) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Refused);
    // Something ANSWERED -- that is a different fact from a terminal that
    // considered the question and said no, and an operator debugging a terminal
    // needs to know which.
    CHECK(result.answer != FastCache::TerminalQueryAnswer::No);
    CHECK(result.answer != FastCache::TerminalQueryAnswer::NoReply);
}

TEST_CASE("Terminal: a non-interactive channel is NotAsked and nothing is written", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, false, { Says(std::string { Da1WithSixel }) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::NotAsked);
    // The load-bearing half: writing an escape sequence at a pipe corrupts it.
    // Asserting only the answer would pass for an implementation that wrote the
    // query and then ignored the reply.
    CHECK(terminal.Written().empty());
    CHECK(terminal.Budgets().empty());
    CHECK(result.elapsed == 0ms);
}

TEST_CASE("Terminal: a well-formed reply to a DIFFERENT question is Refused", "[platform][terminal][da1]")
{
    // A terminal answering a query somebody else sent, or answering out of
    // order. Reading DECRPM's `CSI ? 2026;1 $ y` as a DA1 reply and looking for
    // parameter 4 is how one question's answer becomes a wrong verdict on
    // another -- and here it would be a wrong `No` rather than a wrong `Yes`,
    // which is why the check is on the FINAL byte and not on the parameters.
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?2026;1$y", 1ms) } };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Refused);
}

TEST_CASE("Terminal: a write that fails ends the exchange rather than waiting out the budget", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says(std::string { Da1WithSixel }) } };
    terminal.FailWrites();

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Closed);
    // Nothing was read: waiting 500 ms for a reply to a query that never left is
    // the shape a per-read timeout produces and this must not.
    CHECK(terminal.Budgets().empty());
}

TEST_CASE("Terminal: the budget is a total, not a per-read timeout", "[platform][terminal][da1]")
{
    // A terminal dribbling one byte at a time re-arms a per-read timeout forever
    // and never expires -- the same defect as a per-call SO_RCVTIMEO standing in
    // for a deadline. Each step here costs 100 ms and none of them completes the
    // reply, so a correct driver gives up at the budget.
    FastCache::ManualClock clock;
    ScriptedTerminal terminal {
        clock,
        true,
        { Says("\x1b", 100ms), Says("[", 100ms), Says("?", 100ms), Says("6", 100ms), Says("2", 100ms), Says(";", 100ms) }
    };

    auto const result = FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::DeviceAttributes, 250ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(result.elapsed == 250ms);

    // And the budget handed to each read SHRANK, which is what proves the
    // deadline was carried rather than re-armed.
    REQUIRE(terminal.Budgets().size() >= 2);
    CHECK(terminal.Budgets()[1] < terminal.Budgets()[0]);
}

// ---------------------------------------------------------------------------
// Synchronized output, which answers POSITIONALLY
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: DECRPM reports supported for every non-zero mode value", "[platform][terminal][decrqm]")
{
    // 1 (set), 2 (reset), 3 and 4 (permanently) all mean the terminal knows the
    // mode. Reading the answer as "is it 1" would report a terminal that
    // supports synchronized output and has it currently off as not supporting
    // it -- the wrong answer in the direction that silently removes a feature.
    for (auto const value: { 1, 2, 3, 4 })
    {
        FastCache::ManualClock clock;
        ScriptedTerminal terminal { clock, true, { Says("\x1b[?2026;" + std::to_string(value) + "$y", 1ms) } };
        auto const result =
            FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);
        CHECK(result.answer == FastCache::TerminalQueryAnswer::Yes);
    }
}

TEST_CASE("Terminal: DECRPM mode value 0 means the terminal does not know the mode", "[platform][terminal][decrqm]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?2026;0$y", 1ms) } };

    auto const result =
        FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);

    // `No`, not `Refused`: the terminal answered the question correctly and the
    // answer was negative. The positive control is the case above -- without it,
    // an implementation that reported `No` for every DECRPM reply would pass.
    CHECK(result.answer == FastCache::TerminalQueryAnswer::No);
    CHECK(terminal.Written().front() == "\x1b[?2026$p");
}

TEST_CASE("Terminal: a DECRPM reply too short to carry its answer is not a defaulted yes", "[platform][terminal][decrqm]")
{
    // `CSI ? 2026 $ y` echoes the mode and answers nothing. Reading a missing
    // position as a defaulted 0 would be luck rather than design here; what the
    // seam must not do is invent a `Yes`.
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("\x1b[?2026$y", 1ms) } };

    auto const result =
        FastCache::RunTerminalQuery(terminal, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);

    CHECK(result.answer == FastCache::TerminalQueryAnswer::No);
    CHECK_FALSE(FastCache::PermitsUse(result.answer));
}

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: with no channel every query is NotAsked rather than a plausible no",
          "[platform][terminal][capabilities]")
{
    FastCache::ManualClock clock;
    auto const caps = FastCache::ProbeTerminalCapabilities(nullptr, clock);

    CHECK_FALSE(caps.interactive);
    CHECK(caps.color == FastCache::ColorDepth::None);
    CHECK_FALSE(caps.unicode);
    CHECK_FALSE(caps.altScreen);

    // The distinguishing assertion. `NotAsked` and `No` both refuse the
    // capability, so asserting `!PermitsUse` would pass under a record that
    // claimed the terminal had been asked and said no.
    CHECK(caps.sixel == FastCache::TerminalQueryAnswer::NotAsked);
    CHECK(caps.synchronizedOutput == FastCache::TerminalQueryAnswer::NotAsked);
    CHECK(caps.sixel != FastCache::TerminalQueryAnswer::No);
}

TEST_CASE("Terminal: both queries are put to an interactive channel, in table order", "[platform][terminal][capabilities]")
{
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says(std::string { Da1WithSixel }, 1ms), Says("\x1b[?2026;2$y", 1ms) } };

    auto const caps = FastCache::ProbeTerminalCapabilities(&terminal, clock);

    CHECK(caps.interactive);
    CHECK(caps.sixel == FastCache::TerminalQueryAnswer::Yes);
    CHECK(caps.synchronizedOutput == FastCache::TerminalQueryAnswer::Yes);

    // The queries themselves, so a reordering or a dropped one is visible rather
    // than inferred from an answer that happened to come out right.
    REQUIRE(terminal.Written().size() == 2);
    CHECK(terminal.Written()[0] == FastCache::SpecFor(FastCache::TerminalQueryKind::DeviceAttributes).request);
    CHECK(terminal.Written()[1] == FastCache::SpecFor(FastCache::TerminalQueryKind::SynchronizedOutput).request);
}

TEST_CASE("Terminal: one query's failure does not decide the other", "[platform][terminal][capabilities]")
{
    // The queries are independent facts. An implementation that abandoned the
    // record on the first non-answer would report `NotAsked` for synchronized
    // output on every terminal that ignores DA1 -- a state that would then read
    // as "not interactive".
    FastCache::ManualClock clock;
    ScriptedTerminal terminal { clock, true, { Says("", 500ms), Says("\x1b[?2026;1$y", 1ms) } };

    auto const caps = FastCache::ProbeTerminalCapabilities(&terminal, clock, 500ms);

    CHECK(caps.sixel == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(caps.synchronizedOutput == FastCache::TerminalQueryAnswer::Yes);
}

TEST_CASE("Terminal: the query table describes every query, with distinct requests", "[platform][terminal][capabilities]")
{
    std::vector<std::string_view> requests;
    for (auto const& row: FastCache::TerminalQueryTable)
    {
        CHECK_FALSE(row.request.empty());
        CHECK_FALSE(row.key.empty());
        // Every request this seam writes is a CSI sequence. A row whose request
        // did not start with one would be writing arbitrary bytes at a terminal.
        CHECK(row.request.starts_with("\x1b["));
        CHECK(row.finalByte != '\0');
        requests.push_back(row.request);
    }
    std::ranges::sort(requests);
    CHECK(std::ranges::adjacent_find(requests) == requests.end());
}

// ---------------------------------------------------------------------------
// Colour and Unicode, which read the environment rather than the terminal
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a non-interactive stdout gets no colour whatever the environment says", "[platform][terminal][color]")
{
    CHECK(FastCache::DetectColorDepth(false) == FastCache::ColorDepth::None);
    CHECK_FALSE(FastCache::DetectUnicodeSupport(false));
}

TEST_CASE("Terminal: NO_COLOR outranks an interactive terminal", "[platform][terminal][color]")
{
    ScopedNoColor const guard;
    CHECK(FastCache::DetectColorDepth(true) == FastCache::ColorDepth::None);
}

// ---------------------------------------------------------------------------
// Size and resize
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a non-terminal stdout reports a zero size rather than a guess", "[platform][terminal][size]")
{
    // Under ctest stdout is a pipe. A renderer handed a fabricated 80x24 would
    // draw a frame for a terminal that is not there; zero is the honest answer
    // and is what a caller can test.
    auto const size = FastCache::QueryTerminalSize();
    CHECK(size.columns == 0);
    CHECK(size.rows == 0);
}

TEST_CASE("Terminal: every platform names a resize mechanism", "[platform][terminal][size]")
{
    // #134 §10 INFERRED that Windows must poll per frame. Measured against the
    // Windows SDK on this machine, `ENABLE_WINDOW_INPUT` and
    // `WINDOW_BUFFER_SIZE_EVENT` both exist, so the platform has an
    // edge-triggered event and the inference was wrong. This pins the corrected
    // answer so a later change back to polling is a visible decision.
    auto const mechanism = FastCache::ResizeNotificationMechanism();
    CHECK(mechanism != FastCache::ResizeNotification::Last);
#if defined(_WIN32)
    CHECK(mechanism == FastCache::ResizeNotification::ConsoleEvent);
    CHECK(mechanism != FastCache::ResizeNotification::PollEachFrame);
#else
    CHECK(mechanism == FastCache::ResizeNotification::Signal);
#endif
}
