// SPDX-License-Identifier: Apache-2.0
//
// Real-process implementations of IProcessRunner: CreateProcess + pipes on
// Windows, fork/exec + pipes on POSIX. Both capture stdout and stderr
// separately and drain them CONCURRENTLY — see the interface docs for why a
// sequential drain is a correctness bug, not a performance one.

#include "IProcessRunner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #include <thread>

    #include <windows.h>
#else
    #include <sys/wait.h>

    #include <cerrno>

    #include <poll.h>
    #include <spawn.h>
    #include <unistd.h>

    #if defined(__APPLE__)
        // On macOS `environ` is not available to anything but the main
        // executable; _NSGetEnviron() is the documented way to reach it.
        #include <crt_externs.h>
    #endif
#endif

namespace FastCache::Cc
{

namespace
{

#if defined(_WIN32)

    /// Quote one argument for a Windows command line following the
    /// CommandLineToArgvW rules: backslashes are literal except when they precede a
    /// `"`, where each must be doubled, and a run of backslashes immediately before
    /// the closing quote must also be doubled — otherwise a path ending in `\`
    /// (common for MSVC/SDK include dirs) would escape the closing quote and split
    /// the argument.
    /// @param a The raw argument.
    /// @return The argument, quoted if it contains whitespace or a quote.
    [[nodiscard]] std::string Quote(std::string_view a)
    {
        if (!a.empty() && a.find_first_of(" \t\"") == std::string_view::npos)
            return std::string { a };
        std::string out = "\"";
        std::size_t backslashes = 0;
        for (char const c: a)
        {
            if (c == '\\')
            {
                ++backslashes;
                out += c;
                continue;
            }
            if (c == '"')
            {
                // Double the run of backslashes that precede this quote, then escape it.
                out.append(backslashes, '\\');
                out += '\\';
                out += '"';
            }
            else
            {
                out += c;
            }
            backslashes = 0;
        }
        // Double any trailing backslashes so they do not escape the closing quote.
        out.append(backslashes, '\\');
        out += '"';
        return out;
    }

    /// Join argv into a single Windows command-line string.
    /// @param argv The invocation to join.
    /// @return The quoted, space-joined command line.
    [[nodiscard]] std::string JoinCommand(std::span<std::string const> argv)
    {
        std::string cmd;
        for (auto const& a: argv)
        {
            if (!cmd.empty())
                cmd += ' ';
            cmd += Quote(a);
        }
        return cmd;
    }

    /// Drain a pipe read-end fully into `dst`.
    /// @param readEnd The pipe handle to read until EOF.
    /// @param dst Destination string, appended to.
    void DrainPipe(HANDLE readEnd, std::string& dst)
    {
        std::array<char, 4096> buf {};
        DWORD n = 0;
        while (ReadFile(readEnd, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
            dst.append(buf.data(), n);
    }

    /// Windows process runner: CreateProcess with two inherited pipes.
    class WindowsProcessRunner final: public IProcessRunner
    {
      public:
        [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> argv) override
        {
            // Both child streams share one pipe write-end, so the merge happens in
            // the kernel and the ordering matches what a console would show.
            return Spawn(argv, Merge::Yes);
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv) override
        {
            return Spawn(argv, Merge::No);
        }

      private:
        enum class Merge : std::uint8_t
        {
            No,
            Yes
        };

        /// Spawn `argv`, capturing output either merged or split.
        /// @param argv Full invocation; argv[0] is the executable.
        /// @param merge Whether stderr shares stdout's pipe.
        /// @return Exit code plus captured streams.
        [[nodiscard]] static CompileRun Spawn(std::span<std::string const> argv, Merge merge)
        {
            CompileRun result;
            if (argv.empty())
                return result;

            SECURITY_ATTRIBUTES sa {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;

            HANDLE outR = nullptr;
            HANDLE outW = nullptr;
            HANDLE errR = nullptr;
            HANDLE errW = nullptr;
            if (!CreatePipe(&outR, &outW, &sa, 0))
                return result;
            if (merge == Merge::No && !CreatePipe(&errR, &errW, &sa, 0))
            {
                CloseHandle(outR);
                CloseHandle(outW);
                return result;
            }
            SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
            if (errR != nullptr)
                SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = outW;
            si.hStdError = merge == Merge::Yes ? outW : errW;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            std::string const cmd = JoinCommand(argv);
            std::vector<char> mutableCmd { cmd.begin(), cmd.end() };
            mutableCmd.push_back('\0');

            PROCESS_INFORMATION pi {};
            BOOL const ok =
                CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
            // Parent closes the write ends so its reads see EOF when the child exits.
            CloseHandle(outW);
            if (errW != nullptr)
                CloseHandle(errW);
            if (!ok)
            {
                CloseHandle(outR);
                if (errR != nullptr)
                    CloseHandle(errR);
                return result;
            }

            // Drain both pipes CONCURRENTLY. Sequential draining deadlocks (and then
            // captures a truncated, run-varying amount) whenever the not-yet-drained
            // stream fills its ~64 KB pipe buffer — which preprocessed output routinely
            // does. A truncated/varying stdout capture would make the cache key
            // nondeterministic and defeat all caching, so this must be correct.
            if (errR != nullptr)
            {
                std::thread errThread { [&] { DrainPipe(errR, result.err); } };
                DrainPipe(outR, result.out);
                errThread.join();
            }
            else
            {
                DrainPipe(outR, result.out);
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exit = 0;
            GetExitCodeProcess(pi.hProcess, &exit);
            result.exitCode = static_cast<int>(exit);
            CloseHandle(outR);
            if (errR != nullptr)
                CloseHandle(errR);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return result;
        }
    };

#else

    /// POSIX process runner: posix_spawnp with two pipes, drained with poll().
    class PosixProcessRunner final: public IProcessRunner
    {
      public:
        [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> argv) override
        {
            return Spawn(argv, Merge::Yes);
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv) override
        {
            return Spawn(argv, Merge::No);
        }

      private:
        enum class Merge : std::uint8_t
        {
            No,
            Yes
        };

        /// RAII wrapper for a file descriptor, so every early return closes it.
        class Fd
        {
          public:
            Fd() = default;
            explicit Fd(int fd) noexcept:
                _fd { fd }
            {
            }
            Fd(Fd const&) = delete;
            Fd& operator=(Fd const&) = delete;
            Fd(Fd&& other) noexcept:
                _fd { other._fd }
            {
                other._fd = -1;
            }
            Fd& operator=(Fd&& other) noexcept
            {
                if (this != &other)
                {
                    Close();
                    _fd = other._fd;
                    other._fd = -1;
                }
                return *this;
            }
            ~Fd()
            {
                Close();
            }

            [[nodiscard]] int Get() const noexcept
            {
                return _fd;
            }
            [[nodiscard]] bool Valid() const noexcept
            {
                return _fd >= 0;
            }
            void Close() noexcept
            {
                if (_fd >= 0)
                    ::close(_fd);
                _fd = -1;
            }

          private:
            int _fd { -1 };
        };

        /// Spawn `argv`, capturing output either merged or split.
        ///
        /// Both read-ends are polled in one loop, so neither stream can block the
        /// other by filling its pipe buffer.
        ///
        /// @param argv Full invocation; argv[0] is the executable.
        /// @param merge Whether stderr shares stdout's pipe.
        /// @return Exit code plus captured streams.
        [[nodiscard]] static CompileRun Spawn(std::span<std::string const> argv, Merge merge)
        {
            CompileRun result;
            if (argv.empty())
                return result;

            std::array<int, 2> outPipe { -1, -1 };
            std::array<int, 2> errPipe { -1, -1 };
            if (::pipe(outPipe.data()) != 0)
                return result;
            Fd outRead { outPipe[0] };
            Fd outWrite { outPipe[1] };

            Fd errRead;
            Fd errWrite;
            if (merge == Merge::No)
            {
                if (::pipe(errPipe.data()) != 0)
                    return result;
                errRead = Fd { errPipe[0] };
                errWrite = Fd { errPipe[1] };
            }

            // Child stdout -> outWrite; stderr -> errWrite (or outWrite when merged).
            int const childErrWrite = merge == Merge::Yes ? outWrite.Get() : errWrite.Get();

            posix_spawn_file_actions_t actions;
            if (::posix_spawn_file_actions_init(&actions) != 0)
                return result;
            // The read ends must not leak into the child: if the child holds them
            // open, the parent never sees EOF and the drain loop hangs forever.
            ::posix_spawn_file_actions_addclose(&actions, outRead.Get());
            if (errRead.Valid())
                ::posix_spawn_file_actions_addclose(&actions, errRead.Get());
            ::posix_spawn_file_actions_adddup2(&actions, outWrite.Get(), STDOUT_FILENO);
            ::posix_spawn_file_actions_adddup2(&actions, childErrWrite, STDERR_FILENO);

            // posix_spawn takes a non-const argv; the strings are copied by the
            // child's exec, so this vector only needs to outlive the spawn call.
            std::vector<std::string> owned { argv.begin(), argv.end() };
            std::vector<char*> cargv;
            cargv.reserve(owned.size() + 1);
            for (auto& a: owned)
                cargv.push_back(a.data());
            cargv.push_back(nullptr);

            // The child inherits our environment: the compiler needs INCLUDE,
            // PATH and friends exactly as the build system set them.
    #if defined(__APPLE__)
            char** const inherited = *::_NSGetEnviron();
    #else
            char** const inherited = environ;
    #endif

            ::pid_t pid = -1;
            int const spawned = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), inherited);
            ::posix_spawn_file_actions_destroy(&actions);
            if (spawned != 0)
                return result;

            // Parent closes the write ends so its reads see EOF when the child exits.
            outWrite.Close();
            errWrite.Close();

            DrainBoth(outRead, errRead, result);

            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
                continue;
            if (WIFEXITED(status))
                result.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exitCode = 128 + WTERMSIG(status);
            else
                result.exitCode = 1;
            return result;
        }

        /// Read both pipes until each reports EOF, never blocking on one while the
        /// other has data pending. This is the POSIX counterpart of the two-thread
        /// drain on Windows and is required for the same reason.
        /// @param outRead stdout read-end.
        /// @param errRead stderr read-end (may be invalid when merged).
        /// @param result Destination for the captured bytes.
        static void DrainBoth(Fd& outRead, Fd& errRead, CompileRun& result)
        {
            struct Stream
            {
                int fd;
                std::string* dst;
                bool open;
            };
            std::array<Stream, 2> streams { Stream { .fd = outRead.Get(), .dst = &result.out, .open = outRead.Valid() },
                                            Stream { .fd = errRead.Get(), .dst = &result.err, .open = errRead.Valid() } };

            std::array<char, 4096> buf {};
            while (streams[0].open || streams[1].open)
            {
                std::array<::pollfd, 2> fds {};
                std::array<std::size_t, 2> slot {};
                ::nfds_t count = 0;
                for (std::size_t i = 0; i < streams.size(); ++i)
                {
                    if (!streams[i].open)
                        continue;
                    fds[count] = ::pollfd { .fd = streams[i].fd, .events = POLLIN, .revents = 0 };
                    slot[count] = i;
                    ++count;
                }
                if (count == 0)
                    break;

                if (::poll(fds.data(), count, -1) < 0)
                {
                    if (errno == EINTR)
                        continue;
                    break; // Unrecoverable; treat what we have as the capture.
                }

                for (::nfds_t i = 0; i < count; ++i)
                {
                    if (fds[i].revents == 0)
                        continue;
                    auto& stream = streams[slot[i]];
                    ::ssize_t const n = ::read(stream.fd, buf.data(), buf.size());
                    if (n > 0)
                        stream.dst->append(buf.data(), static_cast<std::size_t>(n));
                    else if (n == 0)
                        stream.open = false; // EOF
                    else if (errno != EINTR && errno != EAGAIN)
                        stream.open = false;
                }
            }
        }
    };

#endif

} // namespace

std::unique_ptr<IProcessRunner> MakeProcessRunner()
{
#if defined(_WIN32)
    return std::make_unique<WindowsProcessRunner>();
#else
    return std::make_unique<PosixProcessRunner>();
#endif
}

} // namespace FastCache::Cc
