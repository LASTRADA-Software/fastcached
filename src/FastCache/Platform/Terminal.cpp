// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <algorithm>
#include <array>
#include <cerrno>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/ioctl.h>

    #include <fcntl.h>
    #include <poll.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
    /// @return true when `NO_COLOR` is present and non-empty. Per the NO_COLOR
    ///         convention, a variable that is set but *empty* does not disable
    ///         color, which is why this tests the value rather than presence.
    [[nodiscard]] bool NoColorRequested()
    {
        auto const value = ReadEnvironmentVariable("NO_COLOR");
        return value.has_value() && !value->empty();
    }
} // namespace

#if defined(_WIN32)

bool StdoutSupportsColor() noexcept
{
    if (NoColorRequested())
        return false;

    HANDLE const handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;

    // Only true consoles get color; a redirected pipe or file does not.
    if (::GetFileType(handle) != FILE_TYPE_CHAR)
        return false;

    DWORD mode = 0;
    if (!::GetConsoleMode(handle, &mode))
        return false;

    // Turn on ANSI escape interpretation. Harmless if it is already set; if
    // the call fails (legacy console) we fall back to no color.
    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

#else

bool StdoutSupportsColor() noexcept
{
    if (NoColorRequested())
        return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

#endif

// ---------------------------------------------------------------------------
// The reply grammar
// ---------------------------------------------------------------------------

bool CsiReply::HasParameter(std::uint32_t value) const noexcept
{
    return std::ranges::find(Parameters(), value) != Parameters().end();
}

CsiDecodeState CsiReplyDecoder::Feed(std::string_view bytes) noexcept
{
    if (_state != CsiDecodeState::NeedMore)
        return _state;

    for (char const ch: bytes)
    {
        // The byte cap covers everything AFTER the escape: a terminal answering
        // `1;1;1;...` forever must be refused rather than accommodated, and the
        // parameter cap alone does not bound one parameter's digits.
        if (_phase != Phase::SeekingEscape)
        {
            ++_consumed;
            if (_consumed > MaxCsiReplyBytes)
            {
                _state = CsiDecodeState::Malformed;
                return _state;
            }
        }

        switch (_phase)
        {
            case Phase::SeekingEscape:
                // Bytes ahead of the reply are SKIPPED, never refused. A terminal
                // hands back what the operator typed as well, so a stray keystroke
                // arriving first is ordinary traffic -- refusing it would turn a
                // keypress into "this terminal has no sixel".
                if (ch == '\x1b')
                    _phase = Phase::SawEscape;
                break;

            case Phase::SawEscape:
                if (ch == '[')
                    _phase = Phase::InParameters;
                else if (ch == '\x1b')
                    _phase = Phase::SawEscape; // ESC ESC: start over on the second.
                else
                {
                    // Some other escape sequence entirely. Rewind rather than
                    // refuse, for the same reason as above.
                    _phase = Phase::SeekingEscape;
                    _consumed = 0;
                }
                break;

            case Phase::InParameters:
                if (ch >= '0' && ch <= '9')
                {
                    // Bounded so a long run of digits cannot overflow. The value
                    // saturates rather than wrapping: a wrapped parameter would be
                    // a DIFFERENT number that might match `wantParameter`, which is
                    // a wrong `Yes` -- the one direction this seam must not fail in.
                    _accumulating = true;
                    if (_accumulator < 1'000'000)
                        _accumulator = (_accumulator * 10) + static_cast<std::uint32_t>(ch - '0');
                }
                else if (ch == ';')
                {
                    if (!PushParameter())
                        return _state;
                }
                else if (ch == '?' && _reply.count == 0 && !_accumulating && _reply.privateMarker == '\0')
                    _reply.privateMarker = ch;
                else if (ch == '$' || ch == '"' || ch == ' ')
                {
                    _reply.intermediate = ch;
                    if (!PushParameter())
                        return _state;
                    _phase = Phase::InIntermediate;
                }
                else if (ch >= '@' && ch <= '~')
                {
                    if (!PushParameter())
                        return _state;
                    _reply.finalByte = ch;
                    _state = CsiDecodeState::Complete;
                    return _state;
                }
                else
                {
                    _state = CsiDecodeState::Malformed;
                    return _state;
                }
                break;

            case Phase::InIntermediate:
                if (ch >= '@' && ch <= '~')
                {
                    _reply.finalByte = ch;
                    _state = CsiDecodeState::Complete;
                    return _state;
                }
                _state = CsiDecodeState::Malformed;
                return _state;
        }
    }
    return _state;
}

bool CsiReplyDecoder::PushParameter() noexcept
{
    // An empty parameter position is a real thing in CSI (`1;;3`), and it is
    // DEFAULT rather than absent -- recorded as 0, which is what the standard
    // says a defaulted parameter means.
    if (_reply.count >= MaxCsiReplyParameters)
    {
        _state = CsiDecodeState::Malformed;
        return false;
    }
    _reply.parameters.at(_reply.count) = _accumulator;
    ++_reply.count;
    _accumulator = 0;
    _accumulating = false;
    return true;
}

// ---------------------------------------------------------------------------
// Putting a question to the terminal
// ---------------------------------------------------------------------------

namespace
{
    /// Read a POSITIONAL answer out of a reply.
    ///
    /// DECRPM answers `CSI ? <mode> ; Ps $ y`, and `Ps` is the answer: 0 means
    /// *no such mode*, while 1 (set), 2 (reset), 3 (permanently set) and 4
    /// (permanently reset) all mean the terminal knows it. So supported is
    /// `Ps != 0` rather than any particular value -- reading it as "is it 1"
    /// would report a terminal that supports synchronized output and currently
    /// has it off as not supporting it.
    ///
    /// A reply too short to carry that position is NOT a defaulted zero: the
    /// terminal did not answer the question asked, and guessing would invent an
    /// answer. It comes back false, which is the closed direction.
    ///
    /// @param reply The parsed reply.
    /// @param index Which parameter carries the answer.
    /// @return true when the terminal reported the capability.
    [[nodiscard]] bool AnswerAtIndex(CsiReply const& reply, std::size_t index) noexcept
    {
        auto const parameters = reply.Parameters();
        if (index >= parameters.size())
            return false;
        return parameters[index] != 0;
    }
} // namespace

TerminalQueryResult RunTerminalQuery(ITerminalChannel& channel,
                                     IClock& clock,
                                     TerminalQueryKind kind,
                                     std::chrono::milliseconds budget)
{
    TerminalQuerySpec const& spec = SpecFor(kind);
    TerminalQueryResult result {};

    // Not asked is a STATE, not a failure: a pipe, a CI runner and `TERM=dumb`
    // all land here, and none of them has said anything about the capability.
    // Writing an escape sequence at a file would also corrupt it.
    if (!channel.IsInteractive())
        return result;

    auto const started = clock.Now();
    auto const elapsedSince = [&clock, started]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now() - started);
    };

    if (!channel.Write(spec.request))
    {
        result.answer = TerminalQueryAnswer::Closed;
        result.elapsed = elapsedSince();
        return result;
    }

    CsiReplyDecoder decoder;
    for (;;)
    {
        // THE DEADLINE, not a per-read timeout. A per-read budget re-arms on every
        // byte, so a terminal dribbling one byte at a time would never expire --
        // the same defect a per-call `SO_RCVTIMEO` is in this tree's dispatch rules.
        auto const spent = elapsedSince();
        if (spent >= budget)
        {
            result.answer = TerminalQueryAnswer::NoReply;
            result.elapsed = spent;
            return result;
        }

        TerminalRead const read = channel.Read(budget - spent);
        switch (read.outcome)
        {
            case TerminalReadOutcome::Bytes:
                break;
            case TerminalReadOutcome::Timeout:
                result.answer = TerminalQueryAnswer::NoReply;
                result.elapsed = elapsedSince();
                return result;
            case TerminalReadOutcome::Closed:
                // Distinguished from `NoReply` on purpose: the terminal went away,
                // which is fixed somewhere else entirely from one that is slow.
                result.answer = TerminalQueryAnswer::Closed;
                result.elapsed = elapsedSince();
                return result;
            case TerminalReadOutcome::Failed:
            case TerminalReadOutcome::Last:
                result.answer = TerminalQueryAnswer::Closed;
                result.elapsed = elapsedSince();
                return result;
        }

        switch (decoder.Feed(read.bytes))
        {
            case CsiDecodeState::NeedMore:
                continue;
            case CsiDecodeState::Malformed:
                result.answer = TerminalQueryAnswer::Refused;
                result.elapsed = elapsedSince();
                return result;
            case CsiDecodeState::Complete:
                break;
            case CsiDecodeState::Last:
                result.answer = TerminalQueryAnswer::Refused;
                result.elapsed = elapsedSince();
                return result;
        }

        result.elapsed = elapsedSince();
        result.reply = decoder.Reply();

        // A well-formed CSI reply that answers a DIFFERENT question is not this
        // question's answer. Reading `CSI ? 2026;1 $ y` as a DA1 reply and looking
        // for parameter 4 is how a terminal's answer to one query becomes a wrong
        // `Yes` to another.
        if (result.reply.finalByte != spec.finalByte || result.reply.intermediate != spec.intermediate)
        {
            result.answer = TerminalQueryAnswer::Refused;
            return result;
        }

        bool const yes = spec.answerIndex == AnswerByPresence ? result.reply.HasParameter(spec.wantParameter)
                                                              : AnswerAtIndex(result.reply, spec.answerIndex);
        result.answer = yes ? TerminalQueryAnswer::Yes : TerminalQueryAnswer::No;
        return result;
    }
}

// ---------------------------------------------------------------------------
// What the environment says
// ---------------------------------------------------------------------------

namespace
{
    /// Case-insensitive substring test over ASCII.
    /// @param haystack The text to search.
    /// @param needle The text to find, lower-case.
    /// @return true when it is present.
    [[nodiscard]] bool ContainsFold(std::string_view haystack, std::string_view needle) noexcept
    {
        auto const lower = [](char c) noexcept {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        };
        return std::ranges::search(haystack, needle, {}, lower).begin() != haystack.end();
    }

    /// An environment variable's value, or empty when unset.
    /// @param name The variable.
    /// @return Its value.
    [[nodiscard]] std::string EnvOrEmpty(char const* name)
    {
        auto const value = ReadEnvironmentVariable(name);
        return value.value_or(std::string {});
    }
} // namespace

ColorDepth DetectColorDepth(bool interactive)
{
    // `NO_COLOR` outranks everything, including a terminal that plainly could.
    // That is the convention's whole point: the operator asked.
    if (NoColorRequested())
        return ColorDepth::None;
    if (!interactive)
        return ColorDepth::None;

    // `COLORTERM` is the only one of these that means what it says. `TERM` is a
    // terminfo name and is routinely wrong in both directions -- a `screen`
    // inside a truecolor terminal, a `xterm` that is anything at all -- so it is
    // consulted second and only for the 256-colour hint it does carry reliably.
    auto const colorTerm = EnvOrEmpty("COLORTERM");
    if (ContainsFold(colorTerm, "truecolor") || ContainsFold(colorTerm, "24bit"))
        return ColorDepth::TrueColor;

    auto const term = EnvOrEmpty("TERM");
    if (ContainsFold(term, "256color"))
        return ColorDepth::Ansi256;

    // A terminal that is interactive and says nothing useful still honours the
    // original sixteen. Degrading to None here would strip colour from every
    // plain `xterm`, which is the wrong direction: a wrong Ansi16 costs nothing
    // a terminal cannot ignore, while a wrong None is a visible loss.
    return ColorDepth::Ansi16;
}

bool DetectUnicodeSupport(bool interactive)
{
    if (!interactive)
        return false;

#if defined(_WIN32)
    // Every Windows executable in this tree declares the UTF-8 process code page
    // (see AGENT.md), so a `char` here is UTF-8 by construction and the console
    // is in the matching mode. There is no locale variable to consult.
    return true;
#else
    // The locale decides, and the order is the one POSIX specifies: LC_ALL wins,
    // then LC_CTYPE, then LANG. A terminal in a latin-1 locale renders `▁` as
    // mojibake, which is worse than the ASCII rung it would otherwise get.
    for (char const* name: { "LC_ALL", "LC_CTYPE", "LANG" })
    {
        auto const value = EnvOrEmpty(name);
        if (value.empty())
            continue;
        return ContainsFold(value, "utf-8") || ContainsFold(value, "utf8");
    }
    return false;
#endif
}

// ---------------------------------------------------------------------------
// The platform channel and the size query
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace
{
    /// A console channel: raw input for the duration, restored on destruction.
    ///
    /// The mode change is RAII because the failure it prevents is not ours to
    /// survive -- a process that exits between "echo off" and "echo on" leaves
    /// the operator's shell mute, and they have to type `stty sane` blind.
    class ConsoleChannel final: public ITerminalChannel
    {
      public:
        ConsoleChannel(HANDLE input, HANDLE output, DWORD previousMode):
            _input { input },
            _output { output },
            _previousMode { previousMode }
        {
        }

        ~ConsoleChannel() override
        {
            ::SetConsoleMode(_input, _previousMode);
        }

        ConsoleChannel(ConsoleChannel const&) = delete;
        ConsoleChannel(ConsoleChannel&&) = delete;
        ConsoleChannel& operator=(ConsoleChannel const&) = delete;
        ConsoleChannel& operator=(ConsoleChannel&&) = delete;

        [[nodiscard]] bool IsInteractive() const noexcept override
        {
            return true;
        }

        [[nodiscard]] bool Write(std::string_view bytes) override
        {
            DWORD written = 0;
            if (::WriteFile(_output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) == 0)
                return false;
            return written == bytes.size();
        }

        [[nodiscard]] TerminalRead Read(std::chrono::milliseconds budget) override
        {
            TerminalRead result {};
            auto const waited = ::WaitForSingleObject(_input, static_cast<DWORD>(budget.count()));
            if (waited == WAIT_TIMEOUT)
            {
                result.outcome = TerminalReadOutcome::Timeout;
                return result;
            }
            if (waited != WAIT_OBJECT_0)
            {
                result.outcome = TerminalReadOutcome::Failed;
                return result;
            }

            std::array<char, 256> buffer {};
            DWORD read = 0;
            if (::ReadFile(_input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == 0)
            {
                result.outcome = TerminalReadOutcome::Failed;
                return result;
            }
            if (read == 0)
            {
                result.outcome = TerminalReadOutcome::Closed;
                return result;
            }
            result.outcome = TerminalReadOutcome::Bytes;
            result.bytes.assign(buffer.data(), read);
            return result;
        }

      private:
        HANDLE _input;
        HANDLE _output;
        DWORD _previousMode;
    };
} // namespace

std::unique_ptr<ITerminalChannel> OpenTerminalChannel()
{
    HANDLE const output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE const input = ::GetStdHandle(STD_INPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return nullptr;
    if (input == nullptr || input == INVALID_HANDLE_VALUE)
        return nullptr;
    if (::GetFileType(output) != FILE_TYPE_CHAR)
        return nullptr;

    DWORD previousMode = 0;
    if (::GetConsoleMode(input, &previousMode) == 0)
        return nullptr;

    // Line input and echo OFF, so the reply arrives as bytes rather than being
    // echoed at the operator and held until they press Return.
    // `ENABLE_VIRTUAL_TERMINAL_INPUT` is what makes the console hand back the
    // reply as the VT bytes the decoder expects rather than as key records.
    DWORD const raw =
        (previousMode & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (::SetConsoleMode(input, raw) == 0)
        return nullptr;

    return std::make_unique<ConsoleChannel>(input, output, previousMode);
}

TerminalSize QueryTerminalSize() noexcept
{
    TerminalSize size {};
    HANDLE const output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return size;

    CONSOLE_SCREEN_BUFFER_INFO info {};
    if (::GetConsoleScreenBufferInfo(output, &info) == 0)
        return size;

    // `srWindow`, NEVER `dwSize`. `dwSize` is the scrollback BUFFER -- commonly
    // 9001 rows -- while `srWindow` is the rectangle the operator can see. A
    // renderer sized from the buffer draws a frame the window shows the middle of.
    size.columns = static_cast<std::uint16_t>(info.srWindow.Right - info.srWindow.Left + 1);
    size.rows = static_cast<std::uint16_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
    return size;
}

#else

namespace
{
    /// A tty channel: raw mode for the duration, restored on destruction.
    ///
    /// RAII for the reason the Windows twin gives: a process that exits with the
    /// terminal still raw leaves the operator typing blind.
    class TtyChannel final: public ITerminalChannel
    {
      public:
        TtyChannel(int fd, termios previous):
            _fd { fd },
            _previous { previous }
        {
        }

        ~TtyChannel() override
        {
            ::tcsetattr(_fd, TCSADRAIN, &_previous);
            ::close(_fd);
        }

        TtyChannel(TtyChannel const&) = delete;
        TtyChannel(TtyChannel&&) = delete;
        TtyChannel& operator=(TtyChannel const&) = delete;
        TtyChannel& operator=(TtyChannel&&) = delete;

        [[nodiscard]] bool IsInteractive() const noexcept override
        {
            return true;
        }

        [[nodiscard]] bool Write(std::string_view bytes) override
        {
            std::size_t sent = 0;
            while (sent < bytes.size())
            {
                auto const wrote = ::write(_fd, bytes.data() + sent, bytes.size() - sent);
                if (wrote < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                sent += static_cast<std::size_t>(wrote);
            }
            return true;
        }

        [[nodiscard]] TerminalRead Read(std::chrono::milliseconds budget) override
        {
            TerminalRead result {};

            pollfd waiting {};
            waiting.fd = _fd;
            waiting.events = POLLIN;
            auto const ready = ::poll(&waiting, 1, static_cast<int>(budget.count()));
            if (ready == 0)
            {
                result.outcome = TerminalReadOutcome::Timeout;
                return result;
            }
            if (ready < 0)
            {
                // EINTR is not an ENDING -- the caller's deadline is still the
                // bound, so this reports nothing-yet rather than a failure and
                // the loop re-arms against whatever budget remains. Reporting
                // `Failed` here would turn an ordinary signal into "this
                // terminal has no sixel".
                result.outcome = errno == EINTR ? TerminalReadOutcome::Timeout : TerminalReadOutcome::Failed;
                return result;
            }

            std::array<char, 256> buffer {};
            auto const got = ::read(_fd, buffer.data(), buffer.size());
            if (got < 0)
            {
                result.outcome = errno == EINTR ? TerminalReadOutcome::Timeout : TerminalReadOutcome::Failed;
                return result;
            }
            if (got == 0)
            {
                result.outcome = TerminalReadOutcome::Closed;
                return result;
            }
            result.outcome = TerminalReadOutcome::Bytes;
            result.bytes.assign(buffer.data(), static_cast<std::size_t>(got));
            return result;
        }

      private:
        int _fd;
        termios _previous;
    };
} // namespace

std::unique_ptr<ITerminalChannel> OpenTerminalChannel()
{
    if (::isatty(STDOUT_FILENO) == 0)
        return nullptr;

    // `/dev/tty` rather than stdout, because the REPLY arrives on the terminal's
    // input side and stdout may be write-only. `O_NOCTTY` so opening it cannot
    // make this the controlling terminal of a process that had none.
    int const fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return nullptr;

    termios previous {};
    if (::tcgetattr(fd, &previous) != 0)
    {
        ::close(fd);
        return nullptr;
    }

    termios raw = previous;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    // Reads return whatever is there immediately: `poll` above is what bounds the
    // wait, so VMIN/VTIME must not add a second, invisible timeout on top of the
    // caller's deadline.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &raw) != 0)
    {
        ::close(fd);
        return nullptr;
    }

    return std::make_unique<TtyChannel>(fd, previous);
}

TerminalSize QueryTerminalSize() noexcept
{
    TerminalSize size {};
    winsize window {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0)
        return size;
    size.columns = window.ws_col;
    size.rows = window.ws_row;
    return size;
}

#endif

// ---------------------------------------------------------------------------
// The whole record
// ---------------------------------------------------------------------------

TerminalCapabilities ProbeTerminalCapabilities(ITerminalChannel* channel, IClock& clock, std::chrono::milliseconds budget)
{
    TerminalCapabilities caps {};
    caps.interactive = channel != nullptr && channel->IsInteractive();
    caps.color = DetectColorDepth(caps.interactive);
    caps.unicode = DetectUnicodeSupport(caps.interactive);
    caps.size = QueryTerminalSize();

    // DECSET 1049 is not queryable, and it is near-universal among terminals
    // that are interactive at all -- so this is stated as an ASSUMPTION rather
    // than dressed as a measurement. It is also the cheap direction: a terminal
    // that ignores 1049 prints nothing and scrolls, where a wrong `false` costs
    // the operator their scrollback.
    caps.altScreen = caps.interactive;

    if (channel == nullptr)
        return caps;

    caps.sixel = RunTerminalQuery(*channel, clock, TerminalQueryKind::DeviceAttributes, budget).answer;
    caps.synchronizedOutput = RunTerminalQuery(*channel, clock, TerminalQueryKind::SynchronizedOutput, budget).answer;
    return caps;
}

} // namespace FastCache
