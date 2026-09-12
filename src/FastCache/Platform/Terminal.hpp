// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Determine whether the process's standard output is an interactive terminal
/// that can render ANSI color, and, on Windows, enable virtual-terminal
/// processing so SGR escape sequences are interpreted rather than printed
/// literally.
///
/// Implementation per platform:
///   - Windows: GetStdHandle + GetFileType (rejects pipes/files) and
///              GetConsoleMode/SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
///   - POSIX:   isatty(STDOUT_FILENO)
///
/// The conventional `NO_COLOR` environment variable is honored: when it is set
/// to a non-empty value, color is suppressed regardless of TTY state.
///
/// @return true if colored output should be emitted to stdout.
[[nodiscard]] bool StdoutSupportsColor() noexcept;

// ---------------------------------------------------------------------------
// What a terminal can do
// ---------------------------------------------------------------------------

/// What SGR a terminal will honour.
///
/// TRANSMITTED/PERSISTED: no. This is a private enum -- it crosses no wire and
/// is written to no file -- so it carries no explicit ordinals and enumerators
/// may be inserted. Said out loud because this tree holds both kinds and *no
/// comment* means both.
enum class ColorDepth : std::uint8_t
{
    None,      ///< Emit no SGR at all.
    Ansi16,    ///< The original eight colours and their bright forms.
    Ansi256,   ///< `38;5;n`.
    TrueColor, ///< `38;2;r;g;b`.
    Last,
};

/// The visible grid, in cells.
struct TerminalSize
{
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
};

/// How a question put to the terminal turned out.
///
/// **This is the shape of the whole seam, so it is worth reading before the
/// rest.** A terminal query is not a predicate: writing `CSI c` and reading a
/// reply has FIVE distinguishable endings and only one of them is *yes*. A
/// `bool` answers with `false` for four different facts, and this repository has
/// paid for that collapse often enough to name it: an outcome that can be *not
/// attempted* is an enum, never a bool, and absence is not zero.
///
/// The four negatives are not interchangeable to a human:
///
///   * `NotAsked` -- nothing was written, so the terminal is not implicated at
///     all. This is what a pipe, a CI runner and `TERM=dumb` produce, and it is
///     the common case rather than an error.
///   * `NoReply` -- the query went out and the budget expired in silence. The
///     terminal may support the feature perfectly and sit behind a slow link.
///   * `Closed` -- the channel ended mid-exchange. The terminal went away; that
///     is fixed somewhere else entirely from a slow one.
///   * `Refused` -- something answered and it was not a well-formed reply.
///
/// Folding any pair of those would make a platform limitation look like a
/// terminal saying no, which is #1321's `unattributable` lesson arriving in a
/// different room.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class TerminalQueryAnswer : std::uint8_t
{
    Yes,      ///< The terminal answered and the reply carries the capability.
    No,       ///< The terminal answered and the reply does not carry it.
    NotAsked, ///< No query was written. Not a failure -- see above.
    NoReply,  ///< The query was written and the budget expired in silence.
    Closed,   ///< The channel ended before a complete reply arrived.
    Refused,  ///< Something answered and it was not a well-formed reply.
    Last,
};

/// One row per `TerminalQueryAnswer`.
struct TerminalQueryAnswerSpec
{
    TerminalQueryAnswer answer; ///< The answer this row describes.

    /// Whether this answer is permission to USE the capability.
    ///
    /// A column rather than `answer == Yes` at each site, and the reason is the
    /// ASYMMETRY rather than tidiness. The two ways of being wrong here cost
    /// wildly different things:
    ///
    ///   * A wrong `true` emits sixel at a terminal that cannot render it, which
    ///     prints a screenful of binary garbage over the operator's session.
    ///   * A wrong `false` draws a Unicode sparkline instead of a picture.
    ///
    /// So every uncertainty resolves to *do not use it*, and this table is where
    /// that is stated once instead of being re-derived at each reader. Exactly
    /// one row is `true`, asserted below, which is what makes "fails closed" a
    /// property of the type rather than a habit.
    bool permitsUse;

    std::string_view key;       ///< Machine-readable spelling, for diagnostics and tests.
    std::string_view diagnosis; ///< What a human should conclude.
};

/// Every answer, in enumerator order.
inline constexpr EnumTable<TerminalQueryAnswer, TerminalQueryAnswerSpec> TerminalQueryAnswerTable { {
    { .answer = TerminalQueryAnswer::Yes,
      .permitsUse = true,
      .key = "yes",
      .diagnosis = "the terminal answered and reported the capability" },
    { .answer = TerminalQueryAnswer::No,
      .permitsUse = false,
      .key = "no",
      .diagnosis = "the terminal answered and did not report the capability" },
    { .answer = TerminalQueryAnswer::NotAsked,
      .permitsUse = false,
      .key = "not-asked",
      .diagnosis = "no query was written; stdout is not an interactive terminal" },
    { .answer = TerminalQueryAnswer::NoReply,
      .permitsUse = false,
      .key = "no-reply",
      .diagnosis = "the query was written and nothing answered within the budget; "
                   "the terminal may support this and be behind a slow link" },
    { .answer = TerminalQueryAnswer::Closed,
      .permitsUse = false,
      .key = "closed",
      .diagnosis = "the terminal ended the channel before a complete reply arrived" },
    { .answer = TerminalQueryAnswer::Refused,
      .permitsUse = false,
      .key = "refused",
      .diagnosis = "something answered and it was not a well-formed reply" },
} };

static_assert(RowsInEnumeratorOrder(TerminalQueryAnswerTable, &TerminalQueryAnswerSpec::answer),
              "TerminalQueryAnswerTable must hold one row per TerminalQueryAnswer, in enumerator order");

/// The row for an answer.
/// @param answer The answer.
/// @return Its row.
[[nodiscard]] constexpr TerminalQueryAnswerSpec const& SpecFor(TerminalQueryAnswer answer) noexcept
{
    return TerminalQueryAnswerTable.at(static_cast<std::size_t>(answer));
}

/// Whether an answer is permission to use the capability it describes.
/// @param answer The answer.
/// @return true only for `Yes`.
[[nodiscard]] constexpr bool PermitsUse(TerminalQueryAnswer answer) noexcept
{
    return SpecFor(answer).permitsUse;
}

// Exactly one answer is permission. If a second ever becomes `true` this stops
// compiling, which is the point: "fails closed" must not be re-decidable by
// editing one row quietly.
static_assert(std::ranges::count_if(TerminalQueryAnswerTable,
                                    [](TerminalQueryAnswerSpec const& row) { return row.permitsUse; })
                  == 1,
              "exactly one TerminalQueryAnswer may permit use; every uncertainty fails closed");

/// What this terminal can do, asked once.
///
/// A record rather than four independent questions in four places: the render
/// code decides its rung once and the panels never branch on a capability again.
struct TerminalCapabilities
{
    bool interactive = false;                                               ///< stdout is a terminal at all.
    ColorDepth color = ColorDepth::None;                                    ///< What SGR it will honour.
    bool unicode = false;                                                   ///< Box drawing and block elements are safe.
    TerminalQueryAnswer sixel = TerminalQueryAnswer::NotAsked;              ///< DA1 parameter 4.
    TerminalQueryAnswer synchronizedOutput = TerminalQueryAnswer::NotAsked; ///< DECRQM 2026, so a frame does not tear.
    bool altScreen = false;                                                 ///< DECSET 1049 is safe.
    TerminalSize size {};
};

// ---------------------------------------------------------------------------
// Asking the terminal
// ---------------------------------------------------------------------------

/// How a single read from the terminal ended.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class TerminalReadOutcome : std::uint8_t
{
    Bytes,   ///< Some bytes arrived; `bytes` holds them.
    Timeout, ///< The budget expired with nothing readable.
    Closed,  ///< End of input: the terminal will send nothing more.
    Failed,  ///< The read itself failed.
    Last,
};

/// The result of one read.
struct TerminalRead
{
    TerminalReadOutcome outcome = TerminalReadOutcome::Failed;
    std::string bytes;
};

/// A terminal control channel: somewhere to write a query and read its reply.
///
/// **Injected, because the alternative is untestable by construction.** What
/// this hides is a write to a device plus a bounded read plus a mode change --
/// I/O and time, the two dependencies this project never reaches for
/// concretely. Behind this seam a fake can script the cases a real terminal
/// cannot be made to produce on demand: an answer with the parameter, one
/// without it, one that arrives late but inside the budget, one that never
/// comes, one that is garbage, and a channel that closes mid-reply.
///
/// Raw mode is the implementation's business, not the caller's: a real channel
/// enters it when it is constructed and restores the previous mode when it is
/// destroyed, because a process that dies between those two points leaves the
/// operator's shell with echo off.
///
/// ## SCOPE: this is THE terminal read seam, and `Read` BLOCKS today
///
/// Stated because the signature alone cannot say it, and the two readings pull
/// opposite ways on a real trade. This is not a probe-only object: raw mode is
/// SESSION-scoped rather than read-scoped, so one channel held for the whole run
/// gives one clean mode transition, where a separate probe channel would enter
/// and restore twice at startup and leave a window in between with the terminal
/// cooked. And a second channel means a second read path on ONE descriptor,
/// which is the hazard `ISocket`'s one-read-slot rule exists for -- arming a
/// read while another is parked drops the parked one, and a terminal fd is not a
/// socket but the failure mode transfers.
///
/// So the dashboard's input path is meant to be THIS object, and that carries an
/// obligation this file does not yet discharge: **`Read` blocks, so it must gain
/// a SUSPENDING form before any render loop reads through it.** A loop built on
/// the blocking one either cannot use it or must run it on a thread it then has
/// to join, which is a thread the caller does not own.
///
/// Why it is not suspending already, so the next reader does not assume it was
/// an oversight: MEASURED at `6e9b2456`, `IReactor`'s entire public surface is
/// `Run`, `Stop`, `Submit`, `Schedule`, `CancelPending`, `Clock` and `RunLoop`
/// -- there is **no descriptor-parking API at all**, and readiness reaches a
/// reactor only through `ISocket` and the platform socket classes. A terminal fd
/// therefore cannot be readiness-parked without extending `IReactor` and all
/// three platform reactors. The reachable answer inside this file is a
/// timer-parked coroutine: `co_await` a wakeup scheduled through
/// `IReactor::Schedule`, a non-blocking read, re-park until the reply completes
/// or the deadline passes. That suspends for real, holds no thread and is
/// cancellable -- and swapping in readiness-parking later changes no caller.
///
/// What must NOT be done instead is a `Task<>` that resolves synchronously. It
/// would satisfy the signature and make every property defined by parking
/// vacuous, which is worse than blocking honestly: a fake that resolves what
/// production suspends on cannot exercise a suspension protocol.
///
/// The probe is one-shot at startup, before any loop exists, so blocking costs
/// nothing TODAY. The obligation is about the second caller, not the first.
class ITerminalChannel
{
  public:
    ITerminalChannel() = default;
    ITerminalChannel(ITerminalChannel const&) = delete;
    ITerminalChannel(ITerminalChannel&&) = delete;
    ITerminalChannel& operator=(ITerminalChannel const&) = delete;
    ITerminalChannel& operator=(ITerminalChannel&&) = delete;
    virtual ~ITerminalChannel() = default;

    /// Whether this channel reaches an interactive terminal at all.
    /// @return true when a query is worth writing.
    [[nodiscard]] virtual bool IsInteractive() const noexcept = 0;

    /// Write a query to the terminal.
    /// @param bytes The query.
    /// @return true when every byte was written.
    [[nodiscard]] virtual bool Write(std::string_view bytes) = 0;

    /// Read whatever is available, waiting at most @p budget.
    /// @param budget How long to wait.
    /// @return What arrived, and how the read ended.
    [[nodiscard]] virtual TerminalRead Read(std::chrono::milliseconds budget) = 0;
};

// ---------------------------------------------------------------------------
// The reply grammar
// ---------------------------------------------------------------------------

/// The most parameters a reply may carry before it is refused.
///
/// A bound rather than a growable buffer, because the bytes come from outside
/// the process: a terminal (or something pretending to be one) that answers
/// `1;1;1;1;...` forever must be refused rather than accommodated. VT525's DA1
/// lists ten attributes, so sixteen is slack rather than a limit anyone meets.
inline constexpr std::size_t MaxCsiReplyParameters = 16;

/// The most bytes a reply may occupy before it is refused.
///
/// The same argument one level up: the parameter cap alone does not bound a
/// single parameter's digits, and `999999999...` is a reply that never ends.
inline constexpr std::size_t MaxCsiReplyBytes = 128;

/// A parsed CSI reply: `ESC [ <private> <params> <intermediate> <final>`.
struct CsiReply
{
    char privateMarker = '\0'; ///< `?` for the replies here, or `\0` if absent.
    char intermediate = '\0';  ///< `$` for DECRPM, or `\0` if absent.
    char finalByte = '\0';     ///< `c` for DA1, `y` for DECRPM.
    std::size_t count = 0;     ///< How many parameters were parsed.
    std::array<std::uint32_t, MaxCsiReplyParameters> parameters {};

    /// The parameters actually parsed.
    /// @return A view of the first `count` entries.
    [[nodiscard]] std::span<std::uint32_t const> Parameters() const noexcept
    {
        return { parameters.data(), count };
    }

    /// Whether a parameter with this value is present.
    /// @param value The value to look for.
    /// @return true when some parameter equals it.
    [[nodiscard]] bool HasParameter(std::uint32_t value) const noexcept;
};

/// How far a decoder has got.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class CsiDecodeState : std::uint8_t
{
    NeedMore,  ///< Nothing wrong yet, and no terminator seen.
    Complete,  ///< A whole reply is available.
    Malformed, ///< These bytes are not a CSI reply, or they overran a bound.
    Last,
};

/// Incremental recogniser for a CSI reply, byte at a time.
///
/// **Pure: no clock, no I/O, no allocation.** The split is deliberate and is the
/// same one `ci-pr-required.sh` makes between acquisition and decision -- the
/// grammar is exhaustively testable without a terminal, and the driver below is
/// testable without a grammar.
///
/// Bytes arriving BEFORE the reply are skipped rather than refused. A terminal
/// hands back whatever the user typed as well, so a stray keystroke ahead of the
/// answer is ordinary traffic, not a malformed reply -- refusing it would turn a
/// keypress into "this terminal has no sixel".
class CsiReplyDecoder
{
  public:
    /// Feed bytes.
    /// @param bytes The bytes read from the terminal.
    /// @return The state after consuming them.
    CsiDecodeState Feed(std::string_view bytes) noexcept;

    /// The reply, valid once `Feed` has returned `Complete`.
    /// @return The parsed reply.
    [[nodiscard]] CsiReply const& Reply() const noexcept
    {
        return _reply;
    }

    /// The state without feeding anything.
    /// @return The current state.
    [[nodiscard]] CsiDecodeState State() const noexcept
    {
        return _state;
    }

  private:
    enum class Phase : std::uint8_t
    {
        SeekingEscape,
        SawEscape,
        InParameters,
        InIntermediate,
    };

    /// Close off the parameter being accumulated.
    /// @return false when the parameter cap was reached, having set `Malformed`.
    bool PushParameter() noexcept;

    CsiDecodeState _state = CsiDecodeState::NeedMore;
    Phase _phase = Phase::SeekingEscape;
    CsiReply _reply {};
    std::size_t _consumed = 0;
    std::uint32_t _accumulator = 0;
    bool _accumulating = false;
};

// ---------------------------------------------------------------------------
// The queries
// ---------------------------------------------------------------------------

/// Which question is being put to the terminal.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class TerminalQueryKind : std::uint8_t
{
    DeviceAttributes,   ///< DA1: does this terminal do sixel?
    SynchronizedOutput, ///< DECRQM 2026: may a frame be wrapped so it does not tear?
    Last,
};

/// One row per question: what to write, and how to read the answer.
///
/// A table rather than two functions, because the two differ only in bytes: the
/// raw-mode session, the budget, the deadline arithmetic and every one of the
/// five endings are shared. A third question is a row.
struct TerminalQuerySpec
{
    TerminalQueryKind kind;      ///< The question this row describes.
    char finalByte;              ///< The reply's final byte.
    char intermediate;           ///< The reply's intermediate byte, or `\0`.
    std::uint32_t wantParameter; ///< The parameter whose presence means *yes*.

    /// Which parameter position carries the ANSWER, or `npos` when presence anywhere is the answer.
    ///
    /// DA1 lists its attributes in no particular order, so sixel is *present or
    /// absent*. DECRPM instead answers positionally -- `CSI ? 2026 ; Ps $ y`,
    /// where `Ps` is 1 (set) or 2 (reset) and both mean supported, while 0 means
    /// *no such mode*. Those are different questions and a single "does
    /// parameter N appear" predicate answers the second one wrongly.
    std::size_t answerIndex;

    std::string_view request; ///< The bytes to write.
    std::string_view key;     ///< Machine-readable spelling, for diagnostics and tests.
};

/// `npos` for `answerIndex`: presence anywhere is the answer.
inline constexpr std::size_t AnswerByPresence = static_cast<std::size_t>(-1);

/// Every query, in enumerator order.
inline constexpr EnumTable<TerminalQueryKind, TerminalQuerySpec> TerminalQueryTable { {
    // DA1. The reply is `CSI ? 62;4;6;22 c` and parameter 4 is sixel. Order is
    // not defined, so this asks whether 4 appears at all.
    { .kind = TerminalQueryKind::DeviceAttributes,
      .finalByte = 'c',
      .intermediate = '\0',
      .wantParameter = 4,
      .answerIndex = AnswerByPresence,
      .request = "\x1b[c",
      .key = "da1" },
    // DECRQM for mode 2026. The reply is `CSI ? 2026 ; Ps $ y`. `Ps` is 0 when
    // the terminal does not recognise the mode, 1 or 2 when it does -- so the
    // ANSWER is the second parameter's value, not the presence of 2026, which
    // the terminal echoes back either way.
    { .kind = TerminalQueryKind::SynchronizedOutput,
      .finalByte = 'y',
      .intermediate = '$',
      .wantParameter = 0,
      .answerIndex = 1,
      .request = "\x1b[?2026$p",
      .key = "decrqm-2026" },
} };

static_assert(RowsInEnumeratorOrder(TerminalQueryTable, &TerminalQuerySpec::kind),
              "TerminalQueryTable must hold one row per TerminalQueryKind, in enumerator order");

/// The row for a query.
/// @param kind The query.
/// @return Its row.
[[nodiscard]] constexpr TerminalQuerySpec const& SpecFor(TerminalQueryKind kind) noexcept
{
    return TerminalQueryTable.at(static_cast<std::size_t>(kind));
}

/// How long to wait for a terminal to answer one query.
///
/// **A chosen number, and here is what it is chosen against.**
///
/// MEASURED (2026-09-12, WSL2 5.15 on this host, 300 round trips over a pty with
/// echo disabled): the mechanism's own cost -- write, `select`, read -- is
/// **0.005 ms median and 0.089 ms at the maximum**. So none of this budget is
/// ours; all of it is the terminal's think time plus whatever link sits between.
///
/// MEASURED on the same host: a terminal that accepts raw mode and **never
/// answers DA1 at all** is not hypothetical. This session's own `/dev/tty` is
/// one, so the expiry path is the ordinary path somewhere, not an edge case.
///
/// INFERRED, and not measured here because no remote terminal was reachable: the
/// dominant term for a remote operator is one round trip, which is single-digit
/// milliseconds on a LAN and 100-300 ms across continents. 500 ms covers that
/// with margin.
///
/// The two ways of being wrong are again not symmetric, and the asymmetry is why
/// this is generous rather than tight:
///
///   * TOO SHORT costs a wrong `NoReply` for an operator on a slow link, which
///     silently removes the best rung of the ladder from the people most likely
///     to want it.
///   * TOO LONG costs a one-off pause, paid ONCE per run and only by an
///     interactive terminal that does not answer -- never by a pipe, a CI runner
///     or `TERM=dumb`, which are `NotAsked` and never write a byte.
///
/// That last clause is what keeps the number affordable: the population that
/// pays the whole budget is "interactive terminal that ignores DA1", which is
/// small, and the population that would pay it on every scripted run is excluded
/// before the query is written.
inline constexpr std::chrono::milliseconds DefaultTerminalQueryBudget { 500 };

/// What one query produced.
struct TerminalQueryResult
{
    TerminalQueryAnswer answer = TerminalQueryAnswer::NotAsked;

    /// How long the exchange actually took.
    ///
    /// MEASURED, not the budget that was requested. A bound that reports the
    /// number it was given cannot tell a slow terminal from one that is not
    /// there, and those are fixed in different places.
    std::chrono::milliseconds elapsed { 0 };

    CsiReply reply {}; ///< The parsed reply, meaningful when `answer` is `Yes` or `No`.
};

/// Put one question to the terminal and wait for its answer.
///
/// Writes the query, then reads until the reply completes, the budget expires,
/// the channel closes, or the bytes stop being a reply. Every ending is one of
/// `TerminalQueryAnswer`'s and none of them throws.
///
/// @param channel Where to write and read. Not asked anything when it reports
///                itself non-interactive, which is `NotAsked`.
/// @param clock   The clock the deadline is measured against. Injected, or every
///                deadline test is a real sleep.
/// @param kind    Which question.
/// @param budget  How long to wait in total.
/// @return The answer, the measured elapsed time, and the reply if there was one.
[[nodiscard]] TerminalQueryResult RunTerminalQuery(ITerminalChannel& channel,
                                                   IClock& clock,
                                                   TerminalQueryKind kind,
                                                   std::chrono::milliseconds budget = DefaultTerminalQueryBudget);

// ---------------------------------------------------------------------------
// Size
// ---------------------------------------------------------------------------

/// The terminal's visible grid right now.
///
/// @return The size, or a zeroed `TerminalSize` when stdout is not a terminal.
///
/// **Windows reads `srWindow`, never `dwSize`.** `CONSOLE_SCREEN_BUFFER_INFO`
/// carries both and they are routinely different: `dwSize` is the scrollback
/// BUFFER, conventionally 9001 rows deep, while `srWindow` is the visible
/// rectangle. A renderer sized from the buffer draws a frame far taller than the
/// window and the operator sees the middle of it.
[[nodiscard]] TerminalSize QueryTerminalSize() noexcept;

/// How this platform learns that the terminal was resized.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class ResizeNotification : std::uint8_t
{
    Signal,        ///< POSIX `SIGWINCH`.
    ConsoleEvent,  ///< Windows `WINDOW_BUFFER_SIZE_EVENT` on the console input handle.
    PollEachFrame, ///< Nothing tells us; re-query.
    Last,
};

/// Which resize mechanism this platform offers.
///
/// **This corrects an inference rather than carrying it forward.** #134 §10 says
/// Windows has no equivalent of `SIGWINCH` and *infers* the size must be
/// re-queried per frame via `GetConsoleScreenBufferInfo`, marking it explicitly
/// as unmeasured. It is measured now, and the inference is wrong:
///
/// MEASURED against the Windows SDK on this machine (10.0.26100.0):
///   * `consoleapi.h` defines `ENABLE_WINDOW_INPUT` as `0x0008`
///   * `wincontypes.h` defines `WINDOW_BUFFER_SIZE_EVENT` as `0x0004` and the
///     matching `_WINDOW_BUFFER_SIZE_RECORD`
///
/// So Windows does have an edge-triggered resize event: set `ENABLE_WINDOW_INPUT`
/// on the console INPUT handle and `ReadConsoleInput` delivers a record when the
/// window changes. That is strictly better than polling, for the reason polling
/// is always worse -- a per-frame query cannot distinguish *nothing changed* from
/// *nothing was asked*, and it ties the resize latency to the frame interval.
///
/// What is NOT established here, stated so the next reader does not inherit a
/// second inference in place of the first: whether the legacy console fires that
/// event for a window change that leaves the buffer size alone. The event's own
/// name is about the BUFFER. Whoever wires the render loop should drive a real
/// resize on both the legacy console and Windows Terminal before relying on it,
/// and `PollEachFrame` remains the honest fallback rather than a dead enumerator.
///
/// @return The mechanism available on this platform.
[[nodiscard]] constexpr ResizeNotification ResizeNotificationMechanism() noexcept
{
#if defined(_WIN32)
    return ResizeNotification::ConsoleEvent;
#else
    return ResizeNotification::Signal;
#endif
}

// ---------------------------------------------------------------------------
// The whole record
// ---------------------------------------------------------------------------

/// Open a control channel to this process's terminal.
///
/// @return A channel, or `nullptr` when stdout is not an interactive terminal.
///         A real channel holds the terminal in raw mode for its lifetime and
///         restores the previous mode when it is destroyed.
[[nodiscard]] std::unique_ptr<ITerminalChannel> OpenTerminalChannel();

/// Ask this terminal everything, once.
///
/// @param channel Where the queries go. A null channel means every query is
///                `NotAsked` and the record is the non-interactive one.
/// @param clock   The clock deadlines are measured against.
/// @param budget  Per-query budget.
/// @return The record.
[[nodiscard]] TerminalCapabilities ProbeTerminalCapabilities(ITerminalChannel* channel,
                                                             IClock& clock,
                                                             std::chrono::milliseconds budget = DefaultTerminalQueryBudget);

/// What colour depth the environment advertises.
///
/// Separated from the record so it is testable without a terminal: it reads
/// `NO_COLOR`, `COLORTERM` and `TERM` and nothing else.
///
/// @param interactive Whether stdout is a terminal at all.
/// @return The depth.
[[nodiscard]] ColorDepth DetectColorDepth(bool interactive);

/// Whether the environment says this terminal can render non-ASCII safely.
///
/// @param interactive Whether stdout is a terminal at all.
/// @return true when block elements and box drawing are safe to emit.
[[nodiscard]] bool DetectUnicodeSupport(bool interactive);

} // namespace FastCache
