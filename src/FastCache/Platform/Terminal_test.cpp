// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
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
/// can produce is a scripted step here, including the four no real terminal can
/// be made to produce on demand: never answering, closing mid-reply, answering
/// late but inside the budget, and dribbling a reply one byte at a time.
///
/// ## This fake PARKS, and it could not have been written not to
///
/// It supplies `TryRead` -- one look -- and inherits `ITerminalChannel::Read`,
/// which is the production parking loop. So a case driving this terminal is
/// driving the real suspension protocol rather than a rehearsal of it. That is
/// deliberate and it is the reason the loop lives on the base class:
/// `.agent/rules/testing.md` names a fake that resolves synchronously what
/// production suspends on as the harder half of the fake rules, precisely
/// because nothing about such a fake looks wrong while every property defined by
/// parking is vacuous over it. Here that fake is not expressible.
///
/// ## A step's time is measured from the QUERY going out
///
/// Steps are grouped one script per `Write`, so "the DA1 query gets nothing and
/// the DECRQM query gets an answer" is sayable -- a single flat list would hand
/// whichever answer came first to whichever query asked first, which is how a
/// two-query case ends up asserting something other than what it reads like.
///
/// Nothing here advances the clock. The clock is the TEST's, driven by
/// `RunParked` in poll-interval steps the way real time would move, so an
/// "arrives at 240 ms" step is observed at the first look at or after 240 ms and
/// not a microsecond earlier.
class ScriptedTerminal final: public FastCache::ITerminalChannel
{
  public:
    struct Step
    {
        std::chrono::milliseconds after { 0 };     ///< When it becomes available, from the query going out.
        FastCache::TerminalPollOutcome outcome {}; ///< How the look ends once it is available.
        std::string bytes {};                      ///< What it hands back.
    };

    using Script = std::vector<Step>;

    /// @param clock       The clock a step's arrival time is measured against.
    /// @param interactive What `IsInteractive` answers.
    /// @param scripts     One script per query, in the order the queries go out.
    ScriptedTerminal(FastCache::IClock& clock, bool interactive, std::vector<Script> scripts):
        _clock { clock },
        _interactive { interactive },
        _scripts { std::move(scripts) }
    {
    }

    [[nodiscard]] bool IsInteractive() const noexcept override
    {
        return _interactive;
    }

    [[nodiscard]] bool Write(std::string_view bytes) override
    {
        _written.emplace_back(bytes);
        // The origin every step's `after` is measured from, re-taken per query.
        _writtenAt = _clock.Now();
        _step = 0;
        return !_writeFails;
    }

    [[nodiscard]] FastCache::TerminalPollResult TryRead() override
    {
        ++_looks;

        // Off the end of the script: behave like a terminal that has gone quiet.
        // A fake that answered something here would make every "never answers"
        // case pass for the wrong reason.
        if (_written.empty() || _written.size() > _scripts.size())
            return { .outcome = FastCache::TerminalPollOutcome::NothingYet, .bytes = {} };

        Script const& script = _scripts.at(_written.size() - 1);
        if (_step >= script.size())
            return { .outcome = FastCache::TerminalPollOutcome::NothingYet, .bytes = {} };

        Step const& step = script.at(_step);
        if (_clock.Now() - _writtenAt < step.after)
            return { .outcome = FastCache::TerminalPollOutcome::NothingYet, .bytes = {} };

        ++_step;
        return { .outcome = step.outcome, .bytes = step.bytes };
    }

    /// Every query written, in order. What a case asserts to see that the seam
    /// wrote nothing at a pipe, and that the two queries went out in table order.
    /// @return The queries.
    [[nodiscard]] std::vector<std::string> const& Written() const noexcept
    {
        return _written;
    }

    /// How many times the parking loop has LOOKED.
    ///
    /// The cost the poll interval is chosen against, counted at the only place
    /// that can count it. Zero is the load-bearing value: it says the seam never
    /// reached the terminal at all.
    /// @return The count.
    [[nodiscard]] std::size_t Looks() const noexcept
    {
        return _looks;
    }

    /// Make `Write` report failure, so the query never leaves.
    void FailWrites() noexcept
    {
        _writeFails = true;
    }

  private:
    FastCache::IClock& _clock;
    std::vector<std::string> _written;
    FastCache::TimePoint _writtenAt {};
    std::size_t _looks = 0;
    std::size_t _step = 0;
    bool _writeFails = false;
    bool _interactive;
    std::vector<Script> _scripts;
};

/// A step that hands back bytes once @p after has elapsed since the query left.
/// @param bytes What the terminal says.
/// @param after When it becomes readable.
/// @return The step.
[[nodiscard]] ScriptedTerminal::Step Says(std::string bytes, std::chrono::milliseconds after = 0ms)
{
    return { .after = after, .outcome = FastCache::TerminalPollOutcome::Bytes, .bytes = std::move(bytes) };
}

/// A step where the channel ends.
/// @param after When the end becomes visible.
/// @return The step.
[[nodiscard]] ScriptedTerminal::Step Closes(std::chrono::milliseconds after = 0ms)
{
    return { .after = after, .outcome = FastCache::TerminalPollOutcome::Closed, .bytes = {} };
}

/// What driving a parked task cost, and -- if it never finished -- which kind of
/// failure that was.
///
/// Two numbers rather than a bool, because a wait that did not finish has two
/// causes that are fixed in different places: it is still parked and needed more
/// time, or nothing is left to resume it and it is lost. `stillParked` is what
/// tells them apart, and a case that only asserted `finished` would report the
/// same red for both.
struct ParkedRun
{
    std::size_t steps = 0;       ///< Clock steps taken; equals the times the read parked.
    bool finished = false;       ///< Whether the task ran to completion.
    std::size_t stillParked = 0; ///< Timers pending when the drive gave up.
};

/// Submit a task and drive it the way real time would.
///
/// Advances the manual clock one poll interval at a time and runs the reactor at
/// each step, so the steps taken ARE the wakeups the interval costs. A
/// synchronous implementation cannot produce that number at all, which is what
/// makes asserting on it worth doing.
///
/// The reactor outlives the task in every case here, and a task that did not
/// finish is holding only BORROWED parked work (`Submit(task.Native())`), so an
/// unfinished drive is a red rather than a crash.
///
/// @param reactor      Where the task parks.
/// @param clock        The clock to advance.
/// @param task         The task to drive; not started yet.
/// @param pollInterval The step size, which must match what the read was given.
/// @param maxSteps     Runaway guard.
/// @return What it cost, and whether it finished.
[[nodiscard]] ParkedRun RunParked(FastCache::TestReactor& reactor,
                                  FastCache::ManualClock& clock,
                                  FastCache::Task<void>& task,
                                  std::chrono::milliseconds pollInterval,
                                  std::size_t maxSteps = 1000)
{
    ParkedRun run {};
    reactor.Submit(task.Native());
    reactor.Run();
    while (!task.IsReady() && run.steps < maxSteps)
    {
        clock.Advance(pollInterval);
        reactor.Run();
        ++run.steps;
    }
    run.finished = task.IsReady();
    run.stillParked = reactor.PendingTimers();
    return run;
}

/// Run one query and store its result where a test can read it.
/// @param channel      The terminal.
/// @param reactor      Where the waiting parks.
/// @param kind         Which question.
/// @param budget       Total budget.
/// @param pollInterval How long a parked read sleeps between looks.
/// @param out          Where the result lands.
/// @return The driving task.
FastCache::Task<void> QueryInto(FastCache::ITerminalChannel& channel,
                                FastCache::IReactor& reactor,
                                FastCache::TerminalQueryKind kind,
                                std::chrono::milliseconds budget,
                                std::chrono::milliseconds pollInterval,
                                FastCache::TerminalQueryResult* out)
{
    *out = co_await FastCache::RunTerminalQuery(channel, reactor, kind, budget, pollInterval);
}

/// Probe the whole record and store it where a test can read it.
/// @param channel      The terminal, or nullptr.
/// @param reactor      Where the waiting parks.
/// @param budget       Per-query budget.
/// @param pollInterval How long a parked read sleeps between looks.
/// @param out          Where the record lands.
/// @return The driving task.
FastCache::Task<void> ProbeInto(FastCache::ITerminalChannel* channel,
                                FastCache::IReactor& reactor,
                                std::chrono::milliseconds budget,
                                std::chrono::milliseconds pollInterval,
                                FastCache::TerminalCapabilities* out)
{
    *out = co_await FastCache::ProbeTerminalCapabilities(channel, reactor, budget, pollInterval);
}

/// One query, driven to completion, with what the waiting cost beside it.
struct QueryRun
{
    FastCache::TerminalQueryResult result {};
    ParkedRun run {};
};

/// Put one question and drive the answer out.
/// @param terminal     The scripted terminal.
/// @param reactor      Where the waiting parks.
/// @param clock        The clock to advance.
/// @param kind         Which question.
/// @param budget       Total budget.
/// @param pollInterval How long a parked read sleeps between looks.
/// @return The answer and the cost.
[[nodiscard]] QueryRun Ask(ScriptedTerminal& terminal,
                           FastCache::TestReactor& reactor,
                           FastCache::ManualClock& clock,
                           FastCache::TerminalQueryKind kind,
                           std::chrono::milliseconds budget,
                           std::chrono::milliseconds pollInterval = FastCache::ProbeTerminalPollInterval)
{
    QueryRun out {};
    auto task = QueryInto(terminal, reactor, kind, budget, pollInterval, &out.result);
    out.run = RunParked(reactor, clock, task, pollInterval);
    return out;
}

/// A channel whose every look claims bytes and hands back none.
///
/// A contract violation rather than a terminal anybody has: both platform looks
/// gate on a positive count, so neither can produce this. It exists because the
/// violation is not inert -- a `Bytes` carrying no bytes sends the driver round
/// again having spent no time and parked nothing, which against an injected
/// clock is a loop that never ends, since nothing advances the clock the
/// deadline is measured on.
///
/// The case that uses it asks `Read` directly rather than going through the
/// driver, deliberately: at the `Read` level a missing guard is an immediate
/// wrong ANSWER, where through the driver it is a hang, and a test that hangs
/// when its property is removed has not reported anything.
class AlwaysEmptyTerminal final: public FastCache::ITerminalChannel
{
  public:
    [[nodiscard]] bool IsInteractive() const noexcept override
    {
        return true;
    }

    [[nodiscard]] bool Write(std::string_view /*bytes*/) override
    {
        return true;
    }

    [[nodiscard]] FastCache::TerminalPollResult TryRead() override
    {
        return { .outcome = FastCache::TerminalPollOutcome::Bytes, .bytes = {} };
    }
};

/// Read once and store the result where a test can read it.
/// @param channel      The terminal.
/// @param reactor      Where the waiting parks.
/// @param budget       Total budget.
/// @param pollInterval How long a parked read sleeps between looks.
/// @param out          Where the result lands.
/// @return The driving task.
FastCache::Task<void> ReadInto(FastCache::ITerminalChannel& channel,
                               FastCache::IReactor& reactor,
                               std::chrono::milliseconds budget,
                               std::chrono::milliseconds pollInterval,
                               FastCache::TerminalRead* out)
{
    *out = co_await channel.Read(reactor, budget, pollInterval);
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
    CHECK_FALSE(FastCache::PermitsUse(FastCache::TerminalQueryAnswer::Abandoned));
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
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?6z", 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Refused);
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
// The wait SUSPENDS -- both directions, because one direction proves nothing
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a wait that must park is REFUSED by SyncRun", "[platform][terminal][parking]")
{
    // The control for every parked case in this file, and the one assertion
    // nothing else can make. `SyncRun` throws when a task is still suspended
    // after one resume, so this passes only while the read genuinely parks.
    //
    // What it catches is the failure `.agent/rules/testing.md` says reading
    // assertions cannot: a `Task` that resolves synchronously, or a fake that
    // answers inline. Either would make this call RETURN, and every case
    // asserting an elapsed or a wakeup count would go on passing with its
    // property vacuous.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 50ms) } } };

    REQUIRE_THROWS_AS(FastCache::SyncRun(FastCache::RunTerminalQuery(
                          terminal, reactor, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms)),
                      std::logic_error);
}

TEST_CASE("Terminal: a reply already waiting is answered without parking at all", "[platform][terminal][parking]")
{
    // The ACCEPTING direction of the case above, and a property in its own
    // right: the loop looks BEFORE it parks, so a terminal that has already
    // answered -- the common case on a local terminal, which replies inside a
    // round trip -- is not charged a poll interval it did not need.
    //
    // A guard nobody has watched accept is not known to work, and here the two
    // cases share one mechanism: if this one threw, the case above would be
    // passing for the wrong reason.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }) } } };

    auto const result = FastCache::SyncRun(
        FastCache::RunTerminalQuery(terminal, reactor, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms));

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(result.elapsed == 0ms);
    CHECK(terminal.Looks() == static_cast<std::size_t>(1));
    CHECK(reactor.PendingTimers() == static_cast<std::size_t>(0));
}

TEST_CASE("Terminal: the poll interval changes what waiting COSTS and not what it answers", "[platform][terminal][parking]")
{
    // Why the interval is a parameter rather than a constant, asserted rather
    // than argued. The same silence is put to the same budget twice, once at the
    // probe's interval and once at the input loop's: the answer and the measured
    // elapsed are identical, and the number of wakeups is not.
    //
    // Both numbers are derived here rather than written down, so a change to
    // either constant moves the expectation with it instead of reddening a case
    // that was only ever restating the header.
    auto const silentRun = [](std::chrono::milliseconds interval) {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        ScriptedTerminal terminal { clock, true, {} };
        return Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, interval);
    };

    auto const tight = silentRun(FastCache::ProbeTerminalPollInterval);
    auto const coarse = silentRun(FastCache::InputTerminalPollInterval);

    REQUIRE(tight.run.finished);
    REQUIRE(coarse.run.finished);

    // Same answer, same measured time: the interval is a cost, not a verdict.
    CHECK(tight.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(coarse.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(tight.result.elapsed == coarse.result.elapsed);
    CHECK(tight.result.elapsed == 500ms);

    // And the cost the header is chosen against: one wakeup per interval, so the
    // coarse one wakes proportionally less. The strict inequality is what would
    // fail if somebody folded the two constants into one.
    CHECK(tight.run.steps == static_cast<std::size_t>(500ms / FastCache::ProbeTerminalPollInterval));
    CHECK(coarse.run.steps == static_cast<std::size_t>(500ms / FastCache::InputTerminalPollInterval));
    CHECK(coarse.run.steps < tight.run.steps);
}

TEST_CASE("Terminal: an answer landing between two looks is seen at the NEXT look", "[platform][terminal][parking]")
{
    // The latency the poll interval costs, pinned. The reply becomes readable at
    // 7 ms and the loop is looking every 5, so it is observed at 10 -- never
    // earlier, and never rounded away. Every other timing case in this file
    // deliberately uses arrival times that are multiples of the interval so
    // their assertions are exact; this is the one that says what happens when
    // they are not, which is the property those cases are quietly relying on.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 7ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(asked.result.elapsed == 10ms);
    CHECK(asked.run.steps == static_cast<std::size_t>(2));
}

TEST_CASE("Terminal: a look that claims bytes and has none is not an answer", "[platform][terminal][parking]")
{
    // Asked of `Read` rather than of the driver, because that is where the two
    // outcomes differ: here a missing guard answers `Bytes` immediately and this
    // case goes red, while through the driver it would spin forever against a
    // clock nobody advances and report nothing at all.
    //
    // Found by NEUTERING rather than by review: the loop was written, read and
    // green before anybody asked what an empty `Bytes` would do to it.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    AlwaysEmptyTerminal terminal;

    FastCache::TerminalRead read {};
    auto task = ReadInto(terminal, reactor, 50ms, 5ms, &read);
    auto const run = RunParked(reactor, clock, task, 5ms);

    REQUIRE(run.finished);
    // It waited its budget out rather than answering from an empty look.
    CHECK(read.outcome == FastCache::TerminalReadOutcome::Timeout);
    CHECK(read.outcome != FastCache::TerminalReadOutcome::Bytes);
    CHECK(read.bytes.empty());
    CHECK(run.steps == static_cast<std::size_t>(50ms / 5ms));
}

TEST_CASE("Terminal: a cancelled wait is Abandoned -- our silence, not the terminal's", "[platform][terminal][parking]")
{
    // `CancelRead` is the only way to abandon a parked read that is not tearing
    // the channel down, and the answer it produces must not be `NoReply`: the
    // terminal was never given its budget, so reporting it as silent would
    // blame it for a shutdown.
    //
    // The positive control is inside the fixture: this terminal WOULD have
    // answered `Yes` at 400 ms, so a cancel that did nothing produces a `Yes`
    // here rather than a hang, and the case fails for the right reason.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 400ms) } } };

    FastCache::TerminalQueryResult result {};
    auto task = QueryInto(terminal, reactor, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms, &result);
    reactor.Submit(task.Native());
    reactor.Run();

    for (int step = 0; step < 4; ++step)
    {
        clock.Advance(5ms);
        reactor.Run();
    }

    // Genuinely parked, or the cancel below would be cancelling nothing.
    REQUIRE_FALSE(task.IsReady());
    REQUIRE(reactor.PendingTimers() == static_cast<std::size_t>(1));

    terminal.CancelRead();
    clock.Advance(5ms);
    reactor.Run();

    REQUIRE(task.IsReady());
    CHECK(result.answer == FastCache::TerminalQueryAnswer::Abandoned);
    CHECK(result.answer != FastCache::TerminalQueryAnswer::NoReply);
    CHECK_FALSE(FastCache::PermitsUse(result.answer));

    // It stopped where it was stopped, not at the budget and not at the answer
    // the terminal was about to give.
    CHECK(result.elapsed == 25ms);
    CHECK(reactor.PendingTimers() == static_cast<std::size_t>(0));
}

TEST_CASE("Terminal: a cancel aimed at a finished read does not abandon the next one", "[platform][terminal][parking]")
{
    // The flag is cleared when a read STARTS rather than remembered. A sticky
    // one would answer `Abandoned` for a query that was never cancelled, which
    // is a wrong answer nobody would look for because the cancel really did
    // happen -- just to something else.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 10ms) } } };

    terminal.CancelRead();

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::Abandoned);
}

// ---------------------------------------------------------------------------
// Putting the question -- scripted channel, manual clock, no terminal
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: a terminal that reports sixel answers Yes", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(FastCache::PermitsUse(asked.result.answer));
    CHECK(asked.result.elapsed == 5ms);
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
    FastCache::TestReactor answeringReactor { answering };
    ScriptedTerminal saysNo { answering, true, { { Says(std::string { Da1WithoutSixel }, 5ms) } } };
    auto const no = Ask(saysNo, answeringReactor, answering, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    FastCache::ManualClock silent;
    FastCache::TestReactor silentReactor { silent };
    ScriptedTerminal saysNothing { silent, true, {} };
    auto const quiet = Ask(saysNothing, silentReactor, silent, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(no.run.finished);
    REQUIRE(quiet.run.finished);

    CHECK(no.result.answer == FastCache::TerminalQueryAnswer::No);
    CHECK(quiet.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(no.result.answer != quiet.result.answer);

    // Both refuse the capability -- that is the safe direction and it must hold.
    CHECK_FALSE(FastCache::PermitsUse(no.result.answer));
    CHECK_FALSE(FastCache::PermitsUse(quiet.result.answer));

    // And their diagnoses differ, which is what an operator reads.
    CHECK(FastCache::SpecFor(no.result.answer).diagnosis != FastCache::SpecFor(quiet.result.answer).diagnosis);
}

TEST_CASE("Terminal: a silent terminal expires at its budget and reports the MEASURED elapsed", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, {} };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 250ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    // The elapsed is what the clock recorded, not the number that was requested.
    // Those coincide here by construction; the case below is where they part.
    CHECK(asked.result.elapsed == 250ms);
}

TEST_CASE("Terminal: an answer that is late but inside the budget is still an answer", "[platform][terminal][da1]")
{
    // The direction a too-tight deadline gets wrong. The reply arrives at 240 ms
    // of a 500 ms budget -- a plausible trans-continental round trip -- and must
    // be honoured rather than discarded.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }, 240ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Yes);
    CHECK(asked.result.elapsed == 240ms);
    // MEASURED rather than requested: the two differ here, which is the whole
    // reason the field is not simply the budget.
    CHECK(asked.result.elapsed < 500ms);
    // And it waited for it rather than answering from nowhere.
    CHECK(asked.run.steps == static_cast<std::size_t>(240ms / FastCache::ProbeTerminalPollInterval));
}

TEST_CASE("Terminal: a channel that closes mid-reply is Closed, not NoReply", "[platform][terminal][da1]")
{
    // A terminal that went away is diagnosed and fixed somewhere else entirely
    // from one that is merely slow, so folding the two would send a reader to
    // the budget for a problem the budget cannot cause.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?62;", 5ms), Closes(10ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Closed);
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::NoReply);
    CHECK(asked.result.elapsed == 10ms);
}

TEST_CASE("Terminal: garbage is Refused, and Refused is not No", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?6z2c", 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Refused);
    // Something ANSWERED -- that is a different fact from a terminal that
    // considered the question and said no, and an operator debugging a terminal
    // needs to know which.
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::No);
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::NoReply);
}

TEST_CASE("Terminal: a non-interactive channel is NotAsked and nothing is written", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, false, { { Says(std::string { Da1WithSixel }) } } };

    // `SyncRun` rather than the reactor, and that is an assertion in itself: a
    // query that is never asked must not park, so this call must not throw.
    auto const result = FastCache::SyncRun(
        FastCache::RunTerminalQuery(terminal, reactor, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms));

    CHECK(result.answer == FastCache::TerminalQueryAnswer::NotAsked);
    // The load-bearing half: writing an escape sequence at a pipe corrupts it.
    // Asserting only the answer would pass for an implementation that wrote the
    // query and then ignored the reply.
    CHECK(terminal.Written().empty());
    CHECK(terminal.Looks() == static_cast<std::size_t>(0));
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
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?2026;1$y", 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Refused);
}

TEST_CASE("Terminal: a write that fails ends the exchange rather than waiting out the budget", "[platform][terminal][da1]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says(std::string { Da1WithSixel }) } } };
    terminal.FailWrites();

    // As with the non-interactive case: this must not park, so `SyncRun`
    // returning at all is half the assertion.
    auto const result = FastCache::SyncRun(
        FastCache::RunTerminalQuery(terminal, reactor, FastCache::TerminalQueryKind::DeviceAttributes, 500ms, 5ms));

    CHECK(result.answer == FastCache::TerminalQueryAnswer::Closed);
    // Nothing was read: waiting 500 ms for a reply to a query that never left is
    // the shape a per-read timeout produces and this must not.
    CHECK(terminal.Looks() == static_cast<std::size_t>(0));
}

TEST_CASE("Terminal: the budget is a total, not a per-read timeout", "[platform][terminal][da1]")
{
    // A terminal dribbling one byte at a time re-arms a per-read timeout forever
    // and never expires -- the same defect as a per-call SO_RCVTIMEO standing in
    // for a deadline. Each byte here arrives 100 ms after the query and none of
    // them completes the reply, so a correct driver gives up at the budget.
    //
    // The DISCRIMINATING assertion is the elapsed. A driver that handed each read
    // the whole budget afresh would answer `NoReply` too -- just 350 ms later,
    // having pushed its deadline out on every byte.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal {
        clock,
        true,
        { { Says("\x1b", 100ms), Says("[", 200ms), Says("?", 300ms), Says("6", 400ms), Says("2", 500ms), Says(";", 600ms) } }
    };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 250ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(asked.result.elapsed == 250ms);
    CHECK(asked.result.elapsed <= 250ms);
    // The query did go out, so this is not a case that passed by never reaching
    // the terminal at all -- and bytes did come back, which is what makes the
    // deadline the only thing that can have ended it.
    REQUIRE(terminal.Written().size() == 1);
    CHECK(terminal.Looks() > static_cast<std::size_t>(0));
    CHECK(asked.run.steps == static_cast<std::size_t>(250ms / FastCache::ProbeTerminalPollInterval));
}

TEST_CASE("Terminal: a partial reply arriving exactly at the deadline is NoReply", "[platform][terminal][da1]")
{
    // The driver's own deadline check, which the read's cannot cover. A read
    // that finds bytes reports them without consulting the deadline -- rightly,
    // it has an answer -- so a reply that lands at the last instant and does not
    // COMPLETE leaves the driver holding an incomplete decode with its whole
    // budget spent. That arm is what answers, and the next `Read` would
    // otherwise be handed a negative budget.
    //
    // This case exists because NEUTERING found nothing watching that arm: folding
    // its `NoReply` into `No` left all 40 cases passing, which is a state
    // distinction no test could have lost, since no test reached it.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?62;", 250ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::DeviceAttributes, 250ms);

    REQUIRE(asked.run.finished);
    // Nothing was asked twice and nothing was invented: the terminal said
    // something, it was not an answer, and the budget is gone.
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::NoReply);
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::No);
    CHECK(asked.result.answer != FastCache::TerminalQueryAnswer::Refused);
    CHECK(asked.result.elapsed == 250ms);
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
        FastCache::TestReactor reactor { clock };
        ScriptedTerminal terminal { clock, true, { { Says("\x1b[?2026;" + std::to_string(value) + "$y", 5ms) } } };

        auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);

        REQUIRE(asked.run.finished);
        CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::Yes);
    }
}

TEST_CASE("Terminal: DECRPM mode value 0 means the terminal does not know the mode", "[platform][terminal][decrqm]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?2026;0$y", 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);

    // `No`, not `Refused`: the terminal answered the question correctly and the
    // answer was negative. The positive control is the case above -- without it,
    // an implementation that reported `No` for every DECRPM reply would pass.
    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::No);
    CHECK(terminal.Written().front() == "\x1b[?2026$p");
}

TEST_CASE("Terminal: a DECRPM reply too short to carry its answer is not a defaulted yes", "[platform][terminal][decrqm]")
{
    // `CSI ? 2026 $ y` echoes the mode and answers nothing. Reading a missing
    // position as a defaulted 0 would be luck rather than design here; what the
    // seam must not do is invent a `Yes`.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { { Says("\x1b[?2026$y", 5ms) } } };

    auto const asked = Ask(terminal, reactor, clock, FastCache::TerminalQueryKind::SynchronizedOutput, 500ms);

    REQUIRE(asked.run.finished);
    CHECK(asked.result.answer == FastCache::TerminalQueryAnswer::No);
    CHECK_FALSE(FastCache::PermitsUse(asked.result.answer));
}

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

TEST_CASE("Terminal: with no channel every query is NotAsked rather than a plausible no",
          "[platform][terminal][capabilities]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };

    // No channel means no query and therefore no waiting, so this resolves
    // without touching the reactor at all.
    auto const caps = FastCache::SyncRun(FastCache::ProbeTerminalCapabilities(nullptr, reactor));

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

TEST_CASE("Terminal: the record is a VALUE a caller can supply instead of probing", "[platform][terminal][capabilities]")
{
    // Probing and the record are separable, and this is what that buys: a caller
    // that already knows what it renders to constructs the record and asks no
    // terminal anything. Nothing downstream may put a question of its own, so a
    // fixture, a golden-output test and an operator override all reach the same
    // rungs the probe would have chosen.
    FastCache::TerminalCapabilities const supplied { .interactive = true,
                                                     .color = FastCache::ColorDepth::TrueColor,
                                                     .unicode = true,
                                                     .sixel = FastCache::TerminalQueryAnswer::Yes,
                                                     .synchronizedOutput = FastCache::TerminalQueryAnswer::No,
                                                     .altScreen = true,
                                                     .size = { .columns = 120, .rows = 40 } };

    CHECK(FastCache::PermitsUse(supplied.sixel));
    CHECK_FALSE(FastCache::PermitsUse(supplied.synchronizedOutput));
    CHECK(supplied.size.columns == 120);

    // And the default is the closed one: a record nobody filled in claims
    // nothing, rather than claiming a terminal was asked and said no.
    FastCache::TerminalCapabilities const unfilled {};
    CHECK(unfilled.sixel == FastCache::TerminalQueryAnswer::NotAsked);
    CHECK(unfilled.synchronizedOutput == FastCache::TerminalQueryAnswer::NotAsked);
    CHECK_FALSE(unfilled.interactive);
}

TEST_CASE("Terminal: both queries are put to an interactive channel, in table order", "[platform][terminal][capabilities]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock,
                                true,
                                { { Says(std::string { Da1WithSixel }, 5ms) }, { Says("\x1b[?2026;2$y", 5ms) } } };

    FastCache::TerminalCapabilities caps {};
    auto task = ProbeInto(&terminal, reactor, 500ms, 5ms, &caps);
    auto const run = RunParked(reactor, clock, task, 5ms);

    REQUIRE(run.finished);
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
    //
    // The first script is EMPTY, which is how this fake spells a query that gets
    // nothing: scripts are per query, so the second query's answer cannot be
    // consumed by the first one asking earlier.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedTerminal terminal { clock, true, { {}, { Says("\x1b[?2026;1$y", 5ms) } } };

    FastCache::TerminalCapabilities caps {};
    auto task = ProbeInto(&terminal, reactor, 500ms, 5ms, &caps);
    auto const run = RunParked(reactor, clock, task, 5ms);

    REQUIRE(run.finished);
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
