// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Net/SocketDeadline.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <tests/BoundedWait.hpp>

// #1553: what `InMemorySocket` answers in every closed state, pinned against a real
// loopback pair driven through the same steps.
//
// **The fake is the transport nearly every protocol, server and consensus test runs
// on**, so a state it answers more permissively than a real socket is a state no test
// can see: a write to a peer that had closed used to succeed, and so no in-memory case
// could notice a session the other end had ended. The rule this enforces is the testing
// rulebook's *a fake more permissive than the thing it stands for*.
//
// Each sequence below is a table of steps, each naming the end that runs it and what the
// model answers. The case runs the table twice -- over `InMemorySocketPair`, and over a
// `PlatformListener` socket (the kind the fake stands in for) facing a blocking client --
// and asserts that the fake answered the table and that the real pair answered it too.
//
// **Where the platforms disagree, the model takes the stricter answer, and that is
// Windows's.** Measured on loopback (Windows 11, Windows Server 2025, Linux under WSL2,
// macOS 14 on a CI runner; #1553):
//   - after a reset, Linux and macOS hand over bytes that were already buffered and then
//     report EOF again; Windows fails every read, buffered bytes included;
//   - a write after a FIN or a reset fails on every platform, as `EPIPE` on POSIX and as
//     `WSAECONNABORTED` or `WSAECONNRESET` on Windows -- and POSIX reports a pending reset
//     to whichever call comes first, so the code a later call gets depends on the order.
// Every failing row names the code `ISocket` maps its error to. A row marked
// `CodeMayDiffer` lets Linux and macOS fail under another code, and one marked
// `PlatformMayAnswer` lets them answer where the model fails. On Windows neither is
// granted: the model IS Windows, error codes included, and a Windows run that differs means
// the justification above has stopped being true.

namespace
{

using namespace std::chrono_literals;
using FastCache::Testing::AwaitUntil;
using FastCache::Testing::OffThreadWaits;
using FastCache::Testing::ReactorWaitOptions;
using FastCache::Testing::WaitOptions;

/// Which socket of the pair a step runs on. Private to this file: never stored or sent.
enum class End : std::uint8_t
{
    Observed, ///< The accepted socket: a reactor socket in the real run, the kind the fake stands in for.
    Peer,     ///< The dialling socket: a blocking one in the real run.
    Last,
};

/// What a step does. Private to this file: never stored or sent.
enum class Act : std::uint8_t
{
    Write,
    WriteVectored,
    Read,
    WaitReadable,
    ShutdownWrite,
    Close,
    Last,
};

/// How an operation answered, in terms two transports can be compared in. Private to
/// this file: never stored or sent.
enum class Answer : std::uint8_t
{
    NotRun, ///< The run ended before this step: a wait ran out, or a deadline closed the socket.
    Done,   ///< `Close` or `ShutdownWrite`, which answer nothing.
    Bytes,  ///< A read or a write moved bytes.
    Ready,  ///< `WaitReadable` reported data.
    Eof,    ///< A read or `WaitReadable` reported the end of the stream.
    Failed, ///< Any other error: a reset, `EPIPE`, `WSAESHUTDOWN`.
    Closed, ///< `BadFileHandle`: this end had closed.
    Retry,  ///< `WouldBlock`, the one failure a caller may retry.
    Last,
};

/// Where a platform may answer differently from the model. Private to this file: never
/// stored or sent. On Windows every row is `Exact`, whatever it says: the model is Windows.
enum class Latitude : std::uint8_t
{
    Exact,             ///< Every platform answers what the model does, error code included.
    CodeMayDiffer,     ///< Every platform fails; Linux and macOS may name the failure another way.
    PlatformMayAnswer, ///< The model fails; Linux and macOS may answer, or fail another way.
};

/// Each end's name, for the transcript.
struct EndRow
{
    End end;
    std::string_view name;
};

constexpr FastCache::EnumTable<End, EndRow> EndRows { {
    { .end = End::Observed, .name = "observed" },
    { .end = End::Peer, .name = "peer" },
} };
static_assert(FastCache::RowsInEnumeratorOrder(EndRows, &EndRow::end));

/// Each act's name, and whether it sends.
struct ActRow
{
    Act act;
    std::string_view name;
    bool sends; ///< Puts something on the wire, so the real run lets it land before the next step.
};

constexpr FastCache::EnumTable<Act, ActRow> ActRows { {
    { .act = Act::Write, .name = "write", .sends = true },
    { .act = Act::WriteVectored, .name = "write-vectored", .sends = true },
    { .act = Act::Read, .name = "read", .sends = false },
    { .act = Act::WaitReadable, .name = "wait-readable", .sends = false },
    { .act = Act::ShutdownWrite, .name = "shutdown-write", .sends = true },
    { .act = Act::Close, .name = "close", .sends = true },
} };
static_assert(FastCache::RowsInEnumeratorOrder(ActRows, &ActRow::act));

/// Each answer's name, whether a platform may give it where the model fails, and whether
/// it carries an error code.
struct AnswerRow
{
    Answer answer;
    std::string_view name;
    bool answered; ///< An answer a platform may give where the model fails, on a `PlatformMayAnswer` row.
    bool failure;  ///< An error, so its code is part of the answer.
};

constexpr FastCache::EnumTable<Answer, AnswerRow> AnswerRows { {
    { .answer = Answer::NotRun, .name = "not-run", .answered = false, .failure = false },
    { .answer = Answer::Done, .name = "done", .answered = false, .failure = false },
    { .answer = Answer::Bytes, .name = "bytes", .answered = true, .failure = false },
    { .answer = Answer::Ready, .name = "ready", .answered = true, .failure = false },
    { .answer = Answer::Eof, .name = "eof", .answered = true, .failure = false },
    { .answer = Answer::Failed, .name = "failed", .answered = false, .failure = true },
    { .answer = Answer::Closed, .name = "closed", .answered = false, .failure = true },
    { .answer = Answer::Retry, .name = "retry", .answered = false, .failure = true },
} };
static_assert(FastCache::RowsInEnumeratorOrder(AnswerRows, &AnswerRow::answer));

/// The errors that are answers of their own; every other error is `Failed`.
constexpr std::array ErrorAnswers {
    std::pair { FastCache::NetErrorCode::BadFileHandle, Answer::Closed },
    std::pair { FastCache::NetErrorCode::WouldBlock, Answer::Retry },
};

/// Whether the model is this platform's own answers, so no row's latitude applies.
#if defined(_WIN32)
constexpr bool ModelIsThisPlatform = true;
#else
constexpr bool ModelIsThisPlatform = false;
#endif

/// How long the real run lets whatever a step sent -- bytes, a FIN, a reset -- land before
/// the next step. Loopback delivers well inside it on every platform measured; the margin
/// is for a loaded runner.
constexpr auto Settle = std::chrono::milliseconds { 50 };

/// How long one real operation may take. Every step is chosen so it answers at once, so
/// this bounds only a platform that parks where the table expects an answer.
constexpr auto OperationBound = std::chrono::milliseconds { 5000 };

/// Big enough for every read a table expects.
constexpr std::size_t ReadBufferBytes = 16;

// The codes `ISocket` maps the platforms' errors to, by what they mean here.
/// The peer closed over bytes it had not read: `ECONNRESET`, `WSAECONNRESET`.
constexpr auto Reset = FastCache::NetErrorCode::ConnReset;
/// A write after the peer's FIN drew the reset, or this end half-closed and wrote:
/// `EPIPE`, `WSAECONNABORTED`, `WSAESHUTDOWN`.
constexpr auto Refused = FastCache::NetErrorCode::SystemError;
/// This end closed.
constexpr auto Gone = FastCache::NetErrorCode::BadFileHandle;
/// No error: the step answers.
constexpr auto None = FastCache::NetErrorCode::Ok;

/// One operation of a sequence: which end runs it, what it does, and what the model answers.
struct Step
{
    End end;                               ///< Which end runs it.
    Act act;                               ///< What it does.
    std::size_t bytes;                     ///< What a write sends, and what a step answering `Bytes` moves.
    Answer model;                          ///< What `InMemorySocket` answers.
    FastCache::NetErrorCode code { None }; ///< The error it answers with, when it fails.
    Latitude latitude { Latitude::Exact }; ///< Where a platform may answer differently.
};

/// A named table of steps, run from the first to the last.
struct Sequence
{
    std::string_view name;       ///< What the sequence exercises.
    std::span<Step const> steps; ///< Its steps, in order.
};

/// A step that moves @p bytes, or answers with nothing to count.
constexpr Step Moves(End end, Act act, std::size_t bytes, Answer model = Answer::Bytes)
{
    return Step { .end = end, .act = act, .bytes = bytes, .model = model, .code = None, .latitude = Latitude::Exact };
}

/// A step the model fails with @p code, and how far a platform may stray from it.
constexpr Step Fails(End end, Act act, FastCache::NetErrorCode code, Latitude latitude = Latitude::Exact)
{
    auto const answer = code == Gone ? Answer::Closed : Answer::Failed;
    return Step { .end = end, .act = act, .bytes = 1, .model = answer, .code = code, .latitude = latitude };
}

constexpr auto Observed = End::Observed;
constexpr auto Peer = End::Peer;

// The peer closes, having read everything it was sent: a FIN. The FIRST write after it is
// accepted and lost, and draws the reset that fails everything after.
constexpr std::array PeerClosesGracefully {
    Moves(Peer, Act::Write, 3),
    Moves(Observed, Act::Read, 3),
    Moves(Peer, Act::Close, 0, Answer::Done),
    Moves(Observed, Act::Read, 0, Answer::Eof),
    Moves(Observed, Act::WaitReadable, 0, Answer::Eof),
    Moves(Observed, Act::Read, 0, Answer::Eof),
    Moves(Observed, Act::Write, 1),
    Fails(Observed, Act::Write, Refused),
    Fails(Observed, Act::WriteVectored, Refused),
    // macOS reports the reset to the first read after it and EOF to the second; Linux reads
    // EOF throughout. Windows fails both, as it does the writes.
    Fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
    Fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
};

// The peer closes with bytes still buffered for US: a FIN, and our first write draws the
// reset. Linux and macOS still hand those bytes over; Windows drops them.
constexpr std::array PeerClosesWithBytesForUs {
    Moves(Peer, Act::Write, 3),
    Moves(Peer, Act::Close, 0, Answer::Done),
    Moves(Observed, Act::Write, 1),
    Fails(Observed, Act::Read, Refused, Latitude::PlatformMayAnswer),
    // Pending as `ECONNRESET` on macOS, where Linux and Windows name it the other way.
    Fails(Observed, Act::Write, Refused, Latitude::CodeMayDiffer),
};

// The peer closes with OUR bytes unread: a reset at once, so no write is ever accepted.
constexpr std::array PeerResets {
    Moves(Observed, Act::Write, 4),
    Moves(Peer, Act::Write, 3),
    Moves(Peer, Act::Close, 0, Answer::Done),
    // Linux and macOS hand over the three bytes that arrived before the reset.
    Fails(Observed, Act::Read, Reset, Latitude::PlatformMayAnswer),
    // The reset itself, which every platform reports, and names alike.
    Fails(Observed, Act::Read, Reset),
    // Reported once on Linux and macOS, EOF after it.
    Fails(Observed, Act::Read, Reset, Latitude::PlatformMayAnswer),
    // `EPIPE` on Linux and macOS once a read has reported the reset; Windows repeats it.
    Fails(Observed, Act::Write, Reset, Latitude::CodeMayDiffer),
    Fails(Observed, Act::Write, Reset, Latitude::CodeMayDiffer),
};

// The peer HALF-closes: it has finished sending, not gone, so it still reads -- and its own
// writes fail.
constexpr std::array PeerHalfCloses {
    Moves(Peer, Act::Write, 3),
    Moves(Peer, Act::ShutdownWrite, 0, Answer::Done),
    Moves(Observed, Act::Read, 3),
    Moves(Observed, Act::Read, 0, Answer::Eof),
    Moves(Observed, Act::WaitReadable, 0, Answer::Eof),
    Moves(Observed, Act::Write, 2),
    Moves(Peer, Act::Read, 2),
    Moves(Observed, Act::WriteVectored, 2),
    Moves(Peer, Act::Read, 2),
    Fails(Peer, Act::Write, Refused),
};

// THIS end half-closes: its writes fail, and it still reads.
constexpr std::array ThisEndHalfCloses {
    Moves(Observed, Act::ShutdownWrite, 0, Answer::Done),
    Fails(Observed, Act::Write, Refused),
    Fails(Observed, Act::WriteVectored, Refused),
    Moves(Peer, Act::Read, 0, Answer::Eof),
    Moves(Peer, Act::Write, 3),
    Moves(Observed, Act::Read, 3),
    Moves(Peer, Act::Close, 0, Answer::Done),
    Moves(Observed, Act::Read, 0, Answer::Eof),
};

// THIS end closes, having read everything: every operation on it is refused as a closed
// handle, `ShutdownWrite` does nothing, and the peer meets the FIN and then the reset.
constexpr std::array ThisEndCloses {
    Moves(Observed, Act::Close, 0, Answer::Done),
    Fails(Observed, Act::Read, Gone),
    Fails(Observed, Act::Write, Gone),
    Fails(Observed, Act::WriteVectored, Gone),
    Fails(Observed, Act::WaitReadable, Gone),
    // A `void` on the interface, and a silent no-op on every platform socket after a close.
    Moves(Observed, Act::ShutdownWrite, 0, Answer::Done),
    Moves(Peer, Act::Read, 0, Answer::Eof),
    Moves(Peer, Act::Write, 1),
    Fails(Peer, Act::Write, Refused),
    Fails(Peer, Act::Read, Refused, Latitude::PlatformMayAnswer),
};

// THIS end closes with the peer's bytes unread: a reset at once. This is the shape a
// server that answers and then closes on a request it did not finish reading has, and why
// a refusal written just before such a close can be lost.
constexpr std::array ThisEndResets {
    Moves(Peer, Act::Write, 3),
    Moves(Observed, Act::Close, 0, Answer::Done),
    Fails(Peer, Act::Read, Reset),
    Fails(Peer, Act::Write, Reset, Latitude::CodeMayDiffer),
};

constexpr std::array Sequences {
    Sequence { .name = "the peer closes, having read everything", .steps = PeerClosesGracefully },
    Sequence { .name = "the peer closes with bytes buffered for us", .steps = PeerClosesWithBytesForUs },
    Sequence { .name = "the peer closes with our bytes unread", .steps = PeerResets },
    Sequence { .name = "the peer half-closes", .steps = PeerHalfCloses },
    Sequence { .name = "this end half-closes", .steps = ThisEndHalfCloses },
    Sequence { .name = "this end closes, having read everything", .steps = ThisEndCloses },
    Sequence { .name = "this end closes with the peer's bytes unread", .steps = ThisEndResets },
};

/// One step's answer, as either run recorded it.
struct Recorded
{
    Answer answer { Answer::NotRun };      ///< How it answered.
    std::size_t bytes { 0 };               ///< What it moved, when it moved bytes.
    FastCache::NetErrorCode code { None }; ///< The error it answered with, when it failed.
    std::string detail;                    ///< The error's code and native number, or why the step did not run.
};

/// Classify one operation's result.
/// @param result What the operation completed with.
/// @param act What the operation was, since `WaitReadable` reports readiness rather than bytes.
/// @return The answer.
[[nodiscard]] Recorded Classify(FastCache::IoResult const& result, Act act)
{
    if (!result.has_value())
    {
        auto const& error = result.error();
        auto answer = Answer::Failed;
        for (auto const& [code, answered]: ErrorAnswers)
            if (code == error.code)
                answer = answered;
        return Recorded { .answer = answer,
                          .bytes = 0,
                          .code = error.code,
                          .detail = std::format("{}/{}", FastCache::ToStringView(error.code), error.systemCode) };
    }
    if (*result == 0)
        return Recorded { .answer = Answer::Eof, .bytes = 0, .code = None, .detail = {} };
    return Recorded {
        .answer = act == Act::WaitReadable ? Answer::Ready : Answer::Bytes, .bytes = *result, .code = None, .detail = {}
    };
}

/// Run one step on `socket` and classify its answer. The one place either run touches a
/// socket, so the two cannot drift into asking different questions.
/// @param socket The socket the step names.
/// @param step The step.
/// @return Its answer.
FastCache::Task<Recorded> Perform(FastCache::ISocket* socket, Step step)
{
    std::vector<std::byte> const payload(step.bytes, std::byte { 0x5a });
    auto const bytes = std::span<std::byte const> { payload };
    std::array<std::byte, ReadBufferBytes> buffer {};
    switch (step.act)
    {
        case Act::Write:
            co_return Classify(co_await socket->write(bytes), step.act);
        case Act::WriteVectored: {
            auto const split = std::min<std::size_t>(1, bytes.size());
            std::array<std::span<std::byte const>, 2> const segments { bytes.first(split), bytes.subspan(split) };
            co_return Classify(co_await socket->writeVectored(segments), step.act);
        }
        case Act::Read:
            co_return Classify(co_await socket->read(std::span<std::byte> { buffer }), step.act);
        case Act::WaitReadable:
            co_return Classify(co_await socket->waitReadable(), step.act);
        case Act::ShutdownWrite:
            socket->shutdownWrite();
            co_return Recorded { .answer = Answer::Done, .bytes = 0, .code = None, .detail = {} };
        case Act::Close:
            socket->close();
            co_return Recorded { .answer = Answer::Done, .bytes = 0, .code = None, .detail = {} };
        case Act::Last:
            break;
    }
    co_return Recorded { .answer = Answer::NotRun, .bytes = 0, .code = None, .detail = "no such act" };
}

/// @return Whether @p recorded is exactly what @p step says the model answers, error code included.
[[nodiscard]] bool Matches(Recorded const& recorded, Step const& step) noexcept
{
    auto const& row = AnswerRows[static_cast<std::size_t>(recorded.answer)];
    return recorded.answer == step.model && (recorded.answer != Answer::Bytes || recorded.bytes == step.bytes)
           && (!row.failure || recorded.code == step.code);
}

/// @return Whether a real socket's @p recorded answer is one the model allows for @p step.
[[nodiscard]] bool Admits(Recorded const& recorded, Step const& step) noexcept
{
    if (Matches(recorded, step))
        return true;
    if (ModelIsThisPlatform)
        return false;
    // The ways a platform may differ, and only on a row that says so. Never by answering
    // where the model fails on a row that does not grant it, which would make the fake the
    // more permissive of the two.
    auto const failedAlike = recorded.answer == step.model && AnswerRows[static_cast<std::size_t>(step.model)].failure;
    switch (step.latitude)
    {
        case Latitude::Exact:
            return false;
        case Latitude::CodeMayDiffer:
            return failedAlike;
        case Latitude::PlatformMayAnswer:
            return failedAlike || AnswerRows[static_cast<std::size_t>(recorded.answer)].answered;
    }
    return false;
}

/// @return @p recorded in words, for the transcript.
[[nodiscard]] std::string Describe(Recorded const& recorded)
{
    auto text = std::string { AnswerRows[static_cast<std::size_t>(recorded.answer)].name };
    if (recorded.answer == Answer::Bytes || recorded.answer == Answer::Ready)
        text += std::format("({})", recorded.bytes);
    if (!recorded.detail.empty())
        text += " " + recorded.detail;
    return text;
}

/// Both runs side by side, one line per step.
[[nodiscard]] std::string Transcript(std::span<Step const> steps,
                                     std::span<Recorded const> fake,
                                     std::span<Recorded const> real)
{
    auto text = std::string { "step  end       act             model                 fake                  real" };
    for (auto const index: std::views::iota(std::size_t { 0 }, steps.size()))
    {
        auto const& step = steps[index];
        auto model = std::string { AnswerRows[static_cast<std::size_t>(step.model)].name };
        if (step.model == Answer::Bytes)
            model += std::format("({})", step.bytes);
        if (AnswerRows[static_cast<std::size_t>(step.model)].failure)
            model += std::format(" {}", FastCache::ToStringView(step.code));
        text += std::format("\n{:>4}  {:<8}  {:<14}  {:<20}  {:<20}  {}",
                            index,
                            EndRows[static_cast<std::size_t>(step.end)].name,
                            ActRows[static_cast<std::size_t>(step.act)].name,
                            model,
                            Describe(fake[index]),
                            Describe(real[index]));
    }
    return text;
}

/// Run @p steps over an `InMemorySocketPair`.
[[nodiscard]] std::vector<Recorded> RunInMemory(std::span<Step const> steps)
{
    auto pair = FastCache::InMemorySocketPair::Create();
    std::vector<Recorded> answers;
    for (auto const& step: steps)
    {
        auto* const socket = step.end == End::Observed ? pair.server.get() : pair.client.get();
        answers.push_back(FastCache::SyncRun(Perform(socket, step)));
    }
    return answers;
}

/// What the two threads of a real run share.
struct RealRun
{
    explicit RealRun(std::size_t steps):
        answers(steps)
    {
    }

    std::vector<Recorded> answers;         ///< Sized up front; each element written only by the end its step names.
    std::atomic<std::size_t> next { 0 };   ///< The step whose turn it is.
    std::atomic<bool> abandoned { false }; ///< Set by whichever end stops early, so the other stops too.
    std::string observerAccount;           ///< Why the observer stopped early; written by the observer alone.
};

/// @return How far @p run has got, for a wait's account.
[[nodiscard]] std::string Progress(RealRun const& run)
{
    return std::format("step {} of {} is next, abandoned {}", run.next.load(), run.answers.size(), run.abandoned.load());
}

/// The observed end of a real run, on the reactor. By pointer throughout, because a
/// coroutine's frame outlives the expression that created it.
/// @param reactor Stopped once the run is over, so `Run()` returns.
/// @param listener Where the peer dials.
/// @param steps The sequence.
/// @param run Shared with the peer's thread.
FastCache::DetachedTask Observe(FastCache::PlatformReactor* reactor,
                                FastCache::IListener* listener,
                                std::span<Step const> steps,
                                RealRun* run)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        run->observerAccount = "accept failed: " + accepted.error().ToString();
        run->abandoned.store(true);
        reactor->stop();
        co_return;
    }
    auto socket = std::move(*accepted);
    auto const progress = [run] {
        return Progress(*run);
    };

    for (auto const index: std::views::iota(std::size_t { 0 }, steps.size()))
    {
        if (steps[index].end != End::Observed)
            continue;
        auto const turn = co_await AwaitUntil(
            reactor,
            std::format("step {} to be the observer's", index),
            [run, index] { return run->next.load() == index || run->abandoned.load(); },
            progress,
            ReactorWaitOptions { .context = {}, .bound = FastCache::Testing::WaitHangGuard, .rest = 1ms });
        if (!turn.reached || run->abandoned.load())
        {
            run->observerAccount = turn.account;
            run->abandoned.store(true);
            break;
        }

        FastCache::SocketDeadlineTarget target { .socket = socket.get(), .expired = false };
        {
            auto const bound = FastCache::ArmSocketDeadline(reactor, OperationBound, &target);
            run->answers[index] = co_await Perform(socket.get(), steps[index]);
        }
        if (target.expired)
        {
            run->answers[index].detail += " (outlived its bound; the socket was closed)";
            run->abandoned.store(true);
            break;
        }
        if (ActRows[static_cast<std::size_t>(steps[index].act)].sends)
            co_await FastCache::SleepFor(*reactor, Settle);
        run->next.store(index + 1);
    }

    // Held open until the peer has run its own last step: closing it earlier would change
    // what the peer meets.
    (void) co_await AwaitUntil(
        reactor,
        "the peer to finish",
        [run, total = steps.size()] { return run->next.load() == total || run->abandoned.load(); },
        progress,
        ReactorWaitOptions { .context = {}, .bound = FastCache::Testing::WaitHangGuard, .rest = 1ms });
    socket->close();
    reactor->stop();
}

/// Run @p steps over a real loopback pair.
/// @param steps The sequence.
/// @param observerAccount Set to why the observer stopped early, when it did.
/// @param waitsReached Set to whether every wait the peer made got there.
/// @return One answer per step.
[[nodiscard]] std::vector<Recorded> RunOnLoopback(std::span<Step const> steps,
                                                  std::string& observerAccount,
                                                  bool& waitsReached)
{
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());
    auto const port = listener->BoundPort();

    RealRun run { steps.size() };
    Observe(&reactor, listener.get(), steps, &run);

    // Declared before the peer, so it outlives the thread that waits through it.
    OffThreadWaits waits;
    std::jthread peer { [port, steps, &run, &waits] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
        {
            run.abandoned.store(true);
            return;
        }
        // A blocking read the table did not expect to park is bounded rather than hung.
        (*socket)->setReceiveDeadline(OperationBound);
        auto const progress = [&run] {
            return Progress(run);
        };
        auto const options = WaitOptions { .step = {}, .context = {}, .bound = FastCache::Testing::WaitHangGuard };

        for (auto const index: std::views::iota(std::size_t { 0 }, steps.size()))
        {
            if (steps[index].end != End::Peer)
                continue;
            auto const turn = waits.Wait(
                std::format("step {} to be the peer's", index),
                [&run, index] { return run.next.load() == index || run.abandoned.load(); },
                progress,
                options);
            if (!turn || run.abandoned.load())
            {
                run.abandoned.store(true);
                break;
            }
            run.answers[index] = FastCache::SyncRun(Perform(socket->get(), steps[index]));
            if (ActRows[static_cast<std::size_t>(steps[index].act)].sends)
                std::this_thread::sleep_for(Settle);
            run.next.store(index + 1);
        }
        // Held open until the observer has run its own last step, for the observer's reason.
        (void) waits.Wait(
            "the observer to finish",
            [&run, total = steps.size()] { return run.next.load() == total || run.abandoned.load(); },
            progress,
            options);
    } };

    reactor.run();
    peer.join();
    observerAccount = run.observerAccount;
    waitsReached = waits.AllReached();
    return std::move(run.answers);
}

} // namespace

TEST_CASE("InMemorySocket answers every closed state the way a loopback TCP socket does", "[net][socket][closed-states]")
{
    for (auto const& sequence: Sequences)
    {
        DYNAMIC_SECTION(sequence.name)
        {
            auto const fake = RunInMemory(sequence.steps);
            std::string observerAccount;
            bool waitsReached = false;
            auto const real = RunOnLoopback(sequence.steps, observerAccount, waitsReached);

            INFO(Transcript(sequence.steps, fake, real));
            // First, so a run that never finished is read as that and not as a disagreement.
            CHECK(observerAccount.empty());
            CHECK(waitsReached);
            for (auto const index: std::views::iota(std::size_t { 0 }, sequence.steps.size()))
            {
                INFO("step " << index);
                auto const& step = sequence.steps[index];
                // The model is what the table says...
                CHECK(Matches(fake[index], step));
                // ...and a real socket answers it too: never failing where the fake answers,
                // and answering where the fake fails only on a row that grants it.
                CHECK(Admits(real[index], step));
            }
        }
    }
}
