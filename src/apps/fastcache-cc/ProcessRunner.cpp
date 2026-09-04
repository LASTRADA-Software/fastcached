// SPDX-License-Identifier: Apache-2.0
//
// Real-process implementations of IProcessRunner: CreateProcess + pipes on
// Windows, fork/exec + pipes on POSIX. Both capture stdout and stderr
// separately and drain them CONCURRENTLY — see the interface docs for why a
// sequential drain is a correctness bug, not a performance one.

#include "IProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

    #include <fcntl.h>
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

    /// A `CreateProcess` attribute list carrying the handles a child may inherit.
    ///
    /// `CreateProcess` with `bInheritHandles = TRUE` hands the child EVERY inheritable
    /// handle the process owns -- not the ones this call set up. That is a
    /// process-wide fact, so it only became visible once a worker ran several
    /// compiles at once: a sibling's child holds another job's pipe write-end open,
    /// that job's drain never sees EOF, and the compile hangs until the unrelated
    /// sibling exits. Accepted sockets are the same hazard, and worse -- Windows
    /// `::socket()` returns an inheritable handle, so a compiler process was pinning
    /// client connections open after the client had gone.
    ///
    /// The list turns the question around: the child inherits exactly what is named
    /// here and nothing else, so a handle added anywhere else in the process cannot
    /// leak into a compile by being forgotten.
    class InheritList
    {
      public:
        /// @param handles The complete set the child may inherit.
        explicit InheritList(std::span<HANDLE const> handles)
        {
            SIZE_T bytes = 0;
            // The first call always "fails"; it is how the size is asked for.
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            _storage.resize(bytes);
            _list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data());
            if (!InitializeProcThreadAttributeList(_list, 1, 0, &bytes))
            {
                _list = nullptr;
                return;
            }
            // UpdateProcThreadAttribute does not copy: the array must outlive the
            // spawn, which is why it is a member rather than the caller's temporary.
            _handles.assign(handles.begin(), handles.end());
            if (!UpdateProcThreadAttribute(_list,
                                           0,
                                           PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           _handles.data(),
                                           _handles.size() * sizeof(HANDLE),
                                           nullptr,
                                           nullptr))
            {
                DeleteProcThreadAttributeList(_list);
                _list = nullptr;
            }
        }

        ~InheritList()
        {
            if (_list != nullptr)
                DeleteProcThreadAttributeList(_list);
        }

        InheritList(InheritList const&) = delete;
        InheritList& operator=(InheritList const&) = delete;
        InheritList(InheritList&&) = delete;
        InheritList& operator=(InheritList&&) = delete;

        /// Whether the list was built; a false value means do not spawn.
        [[nodiscard]] bool Valid() const noexcept
        {
            return _list != nullptr;
        }

        /// The list to hand to `STARTUPINFOEX`.
        [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept
        {
            return _list;
        }

      private:
        std::vector<std::byte> _storage;
        std::vector<HANDLE> _handles;
        LPPROC_THREAD_ATTRIBUTE_LIST _list { nullptr };
    };

    /// Windows process runner: CreateProcess with two inherited pipes.
    class WindowsProcessRunner final: public IProcessRunner
    {
      public:
        [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> argv) override
        {
            // Both child streams share one pipe write-end, so the merge happens in
            // the kernel and the ordering matches what a console would show.
            return Spawn(argv, Merge::Yes, {});
        }

        [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> argv,
                                                    std::span<EnvironmentAssignment const> environment) override
        {
            return Spawn(argv, Merge::Yes, environment);
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv) override
        {
            return Spawn(argv, Merge::No, {});
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv,
                                                 std::span<EnvironmentAssignment const> environment) override
        {
            return Spawn(argv, Merge::No, environment);
        }

      private:
        enum class Merge : std::uint8_t
        {
            No,
            Yes
        };

        /// The environment block `CreateProcess` wants, built from this process's own
        /// plus the caller's additions.
        ///
        /// Additive: the inherited block is copied wholesale and only a variable the
        /// caller NAMES is replaced. Dropping the rest would lose `INCLUDE` and turn
        /// every spawn into `C1034`.
        ///
        /// Empty when there is nothing to add, and the caller then passes `nullptr`
        /// -- inherit-everything, which is byte for byte the behaviour before this
        /// existed rather than a reconstruction of it that could differ.
        ///
        /// The block's shape is `NAME=VALUE\0NAME=VALUE\0\0`, and names compare
        /// case-insensitively because Windows environment variables do.
        /// @param environment Variables to add or override.
        /// @return The block, or empty when there is nothing to add.
        [[nodiscard]] static std::vector<char> EnvironmentBlock(std::span<EnvironmentAssignment const> environment)
        {
            std::vector<char> block;
            if (environment.empty())
                return block;

            auto const overridden = [&environment](std::string_view entry) {
                auto const eq = entry.find('=');
                if (eq == std::string_view::npos)
                    return false;
                auto const name = entry.substr(0, eq);
                // ASCII-only fold, spelled here rather than borrowed: this file is the
                // process seam and depends on nothing above it, and every environment
                // variable name a caller of this may pass is ASCII. `std::tolower`
                // would make the comparison locale-dependent, which is the family of
                // defect this whole change is about.
                auto const sameName = [name](EnvironmentAssignment const& assignment) {
                    auto const lower = [](char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
                    };
                    return assignment.name.size() == name.size()
                           && std::ranges::equal(
                               assignment.name, name, [lower](char a, char b) { return lower(a) == lower(b); });
                };
                return std::ranges::any_of(environment, sameName);
            };

            // Collected first, so the whole set can be ordered once; the block's own
            // shape is built at the end.
            std::vector<std::string> entries;
            auto const append = [&entries](std::string_view text) {
                entries.emplace_back(text);
            };

            // `GetEnvironmentStringsA`, not `environ`: the CRT copy is a snapshot
            // taken at startup, while this is what the OS would hand a child anyway.
            char* const inherited = GetEnvironmentStringsA();
            if (inherited != nullptr)
            {
                for (char const* cursor = inherited; *cursor != '\0';)
                {
                    std::string_view const entry { cursor };
                    cursor += entry.size() + 1;
                    // A leading `=` marks Windows' hidden per-drive current
                    // directories (`=C:=C:\path`). Kept, because dropping them changes
                    // where a child resolves a drive-relative path, and never read as
                    // a name the caller might be overriding.
                    if (entry.starts_with('=') || !overridden(entry))
                        append(entry);
                }
                FreeEnvironmentStringsA(inherited);
            }

            for (auto const& [name, value]: environment)
            {
                // Built by appending rather than by two `operator+` calls, each of
                // which allocates a temporary this would discard.
                std::string entry = name;
                entry += '=';
                entry += value;
                append(entry);
            }

            // Sorted before serializing, because that is `CreateProcess`'s documented
            // contract for the block it is handed. `GetEnvironmentStringsA` returns
            // one already in that order, so appending the additions after every
            // inherited entry would hand a child something no OS-built block looks
            // like. The fold is ASCII and spelled out for the reason the name
            // comparison above is: a locale-dependent one is the family of defect
            // this whole change is about.
            auto const fold = [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
            };
            std::ranges::sort(entries, [fold](std::string_view a, std::string_view b) {
                return std::ranges::lexicographical_compare(a, b, [fold](char x, char y) { return fold(x) < fold(y); });
            });

            for (auto const& entry: entries)
            {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back('\0');
            }
            block.push_back('\0');
            return block;
        }

        /// Spawn `argv`, capturing output either merged or split.
        /// @param argv Full invocation; argv[0] is the executable.
        /// @param merge Whether stderr shares stdout's pipe.
        /// @param environment Variables added to the inherited environment.
        /// @return Exit code plus captured streams.
        [[nodiscard]] static CompileRun Spawn(std::span<std::string const> argv,
                                              Merge merge,
                                              std::span<EnvironmentAssignment const> environment)
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

            // Everything the child is allowed to inherit, named explicitly. A std
            // handle passed here MUST also appear in the list, or CreateProcess
            // refuses the call outright.
            std::vector<HANDLE> inheritable { outW };
            if (errW != nullptr)
                inheritable.push_back(errW);

            // The parent's stdin is passed through when it is something a child can
            // inherit -- under a service there is no console, and naming a handle the
            // list cannot carry would fail every spawn.
            HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
            DWORD stdInFlags = 0;
            if (stdIn == INVALID_HANDLE_VALUE || stdIn == nullptr || !GetHandleInformation(stdIn, &stdInFlags)
                || (stdInFlags & HANDLE_FLAG_INHERIT) == 0)
                stdIn = nullptr;
            else
                inheritable.push_back(stdIn);

            STARTUPINFOEXA si {};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = outW;
            si.StartupInfo.hStdError = merge == Merge::Yes ? outW : errW;
            si.StartupInfo.hStdInput = stdIn;

            InheritList const allowed { inheritable };
            if (!allowed.Valid())
            {
                CloseHandle(outR);
                CloseHandle(outW);
                if (errR != nullptr)
                {
                    CloseHandle(errR);
                    CloseHandle(errW);
                }
                return result;
            }
            si.lpAttributeList = allowed.Get();

            std::string const cmd = JoinCommand(argv);
            std::vector<char> mutableCmd { cmd.begin(), cmd.end() };
            mutableCmd.push_back('\0');

            // Empty means "add nothing", and `nullptr` is what says that to
            // CreateProcess -- inherit everything, exactly as before this parameter
            // existed. Passing a rebuilt block for the no-additions case would put a
            // reconstruction on the path every ordinary spawn takes.
            std::vector<char> block = EnvironmentBlock(environment);
            void* const childEnvironment = block.empty() ? nullptr : block.data();

            PROCESS_INFORMATION pi {};
            BOOL const ok = CreateProcessA(nullptr,
                                           mutableCmd.data(),
                                           nullptr,
                                           nullptr,
                                           TRUE,
                                           EXTENDED_STARTUPINFO_PRESENT,
                                           childEnvironment,
                                           nullptr,
                                           &si.StartupInfo,
                                           &pi);
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
            return Spawn(argv, Merge::Yes, {});
        }

        [[nodiscard]] CompileRun RunCaptureCombined(std::span<std::string const> argv,
                                                    std::span<EnvironmentAssignment const> environment) override
        {
            return Spawn(argv, Merge::Yes, environment);
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv) override
        {
            return Spawn(argv, Merge::No, {});
        }

        [[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv,
                                                 std::span<EnvironmentAssignment const> environment) override
        {
            return Spawn(argv, Merge::No, environment);
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

        /// Serializes pipe creation with spawning, so no child is created while a
        /// descriptor exists that has not yet been marked close-on-exec.
        ///
        /// Held only across those two steps -- never across the compile itself -- so
        /// concurrent jobs still run concurrently. `pipe2(O_CLOEXEC)` would make the
        /// window disappear on Linux, but macOS has no `pipe2`, and one mechanism
        /// that holds everywhere beats two that differ by platform.
        [[nodiscard]] static std::mutex& SpawnLock()
        {
            static std::mutex lock;
            return lock;
        }

        /// Create a pipe whose ends are both close-on-exec.
        ///
        /// Close-on-exec is what keeps ONE job's pipe out of ANOTHER job's child.
        /// Without it a sibling compile inherits this pipe's write end, holds it open
        /// for as long as it runs, and the drain below never sees EOF -- so a job
        /// blocks until an unrelated one finishes. Invisible while a worker served
        /// one compile at a time; a hang once it serves several.
        ///
        /// The child's own stdout/stderr are unaffected: `dup2` clears the flag on
        /// the descriptor it creates.
        ///
        /// @param fds Filled with {read, write} on success.
        /// @return Whether the pipe was created and marked.
        [[nodiscard]] static bool MakePipe(std::array<int, 2>& fds)
        {
            if (::pipe(fds.data()) != 0)
                return false;
            for (int const fd: fds)
            {
                int const flags = ::fcntl(fd, F_GETFD);
                if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
                {
                    ::close(fds[0]);
                    ::close(fds[1]);
                    return false;
                }
            }
            return true;
        }

        /// Spawn `argv`, capturing output either merged or split.
        ///
        /// Both read-ends are polled in one loop, so neither stream can block the
        /// other by filling its pipe buffer.
        ///
        /// @param argv Full invocation; argv[0] is the executable.
        /// @param merge Whether stderr shares stdout's pipe.
        /// @param environment Variables added to the inherited environment.
        /// @return Exit code plus captured streams.
        [[nodiscard]] static CompileRun Spawn(std::span<std::string const> argv,
                                              Merge merge,
                                              std::span<EnvironmentAssignment const> environment)
        {
            CompileRun result;
            if (argv.empty())
                return result;

            Fd outRead;
            Fd outWrite;
            Fd errRead;
            Fd errWrite;
            ::pid_t pid = -1;

            // Everything up to and including the spawn happens under the lock; the
            // drain below deliberately does not, because the drain IS the compile.
            {
                auto const spawning = std::scoped_lock { SpawnLock() };

                std::array<int, 2> outPipe { -1, -1 };
                std::array<int, 2> errPipe { -1, -1 };
                if (!MakePipe(outPipe))
                    return result;
                outRead = Fd { outPipe[0] };
                outWrite = Fd { outPipe[1] };

                if (merge == Merge::No)
                {
                    if (!MakePipe(errPipe))
                        return result;
                    errRead = Fd { errPipe[0] };
                    errWrite = Fd { errPipe[1] };
                }

                // Child stdout -> outWrite; stderr -> errWrite (or outWrite when merged).
                //
                // When merged, BOTH dup2 actions target the same descriptor. File
                // actions run in order, so nothing may close outWrite between them:
                // adding an addclose(outWrite) below would leave the second dup2
                // duplicating a closed descriptor and silently lose all stderr. The
                // parent closes the write ends after the spawn instead, where the
                // ordering cannot matter.
                int const childErrWrite = merge == Merge::Yes ? outWrite.Get() : errWrite.Get();

                posix_spawn_file_actions_t actions {};
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

                // Additions are layered ON that, never substituted for it. With none
                // to make, `inherited` is handed over untouched, so the ordinary
                // spawn is byte for byte what it was before this parameter existed.
                //
                // A name the parent already carries is dropped from the copy rather
                // than appended after it: POSIX leaves a duplicate's resolution to
                // the implementation, and `getenv` returning the first match would
                // make the addition silently ineffective on some libc.
                std::vector<std::string> merged;
                std::vector<char*> mergedPointers;
                char** childEnvironment = inherited;
                if (!environment.empty())
                {
                    for (char** entry = inherited; entry != nullptr && *entry != nullptr; ++entry)
                    {
                        std::string_view const text { *entry };
                        auto const eq = text.find('=');
                        auto const name = eq == std::string_view::npos ? text : text.substr(0, eq);
                        if (std::ranges::none_of(environment,
                                                 [name](EnvironmentAssignment const& a) { return a.name == name; }))
                            merged.emplace_back(text);
                    }
                    for (auto const& [name, value]: environment)
                    {
                        // Appended rather than concatenated, as on the Windows side and
                        // for the same reason: two `operator+` calls allocate a
                        // temporary each.
                        std::string entry = name;
                        entry += '=';
                        entry += value;
                        merged.push_back(std::move(entry));
                    }

                    // Reserved before any `data()` is taken: a reallocation would
                    // leave every pointer already stored dangling.
                    mergedPointers.reserve(merged.size() + 1);
                    for (auto& entry: merged)
                        mergedPointers.push_back(entry.data());
                    mergedPointers.push_back(nullptr);
                    childEnvironment = mergedPointers.data();
                }

                int const spawned = ::posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), childEnvironment);
                ::posix_spawn_file_actions_destroy(&actions);
                if (spawned != 0)
                    return result;

                // Parent closes the write ends so its reads see EOF when the child
                // exits.
                outWrite.Close();
                errWrite.Close();
            }

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
