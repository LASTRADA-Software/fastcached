// SPDX-License-Identifier: Apache-2.0
//
// A stub compiler that saves a file in the middle of one launcher invocation -- #1650's
// reproduction, end to end.
//
// [#1650](https://github.com/LASTRADA-Software/fastcached/issues/1650): an editor's save that
// lands WHILE `fastcache-cc` compiles is a moment the launcher cannot see from outside. It took
// its key from the content before the save and, before the fix, recorded its manifest from the
// content after it, so the saved content became a direct HIT on the object of the content it
// replaced -- exit 0, in every checkout whose files matched. No fixture can type a save into
// that window by timing, so this stub IS the compiler the launcher runs: it forwards every
// invocation to the real compiler and, at one chosen point, performs the save itself.
//
// The e2e fixtures (`scripts/compile-cache-e2e.sh`, `src/apps/fastcache-cc/run-launcher-e2e.ps1`)
// copy it into a directory of its own under the REAL compiler's file name, because the launcher
// picks its driver grammar by that name. It needs no FastCache library and no Catch2, and it is
// never installed.
//
// ## Driven by the environment
//
//   FC_EDIT_STUB_REAL      absolute path of the real compiler to forward to (required)
//   FC_EDIT_STUB_AT        `after-compile`: save after a real compile exits 0;
//                          `before-dispatch-preprocess`: save before forwarding a
//                          line-marker preprocess. Unset: forward only.
//   FC_EDIT_STUB_TRIGGER   a file; the save happens only while it exists, and the stub
//                          deletes it, so the save is ONE shot and a fixture can ask
//                          whether it fired
//   FC_EDIT_STUB_FROM/TO   the save: FROM's bytes overwrite TO IN PLACE
//
// The save opens TO for a truncating write, the way an editor saves, and never copies the
// file: on Windows `CopyFileW` (which `std::filesystem::copy_file` is) carries the source's
// change time across, so a copied file can look unchanged to a clock (measured, #1650).
//
// ## How an invocation is classified (`ClassifyTable` below)
//
// A REAL COMPILE names `-c` or `/c` and no preprocess-only or probe switch. A DISPATCH
// PREPROCESS names `-E` or `/E` and none of the switches that make it the key's preprocess
// (`-E -P`, `/EP`), the include probe (`-E -v`) or the target probe (`-###`). Everything else --
// the key's preprocess, the banner probe, `-###` -- is forwarded untouched.
//
// Exit status: the real compiler's; 125 when the stub cannot do what it was asked (a missing
// variable, a save that failed), with a line on stderr naming why, so a fixture fails loudly
// rather than testing nothing.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/types.h>
    #include <sys/wait.h>

    #include <spawn.h>
#endif

namespace
{

/// The stub's own failure: something it was asked to do could not be done.
constexpr int StubFailure = 125;

/// What one invocation of the compiler is, as far as the save is concerned. Private.
enum class Invocation : std::uint8_t
{
    RealCompile,
    DispatchPreprocess,
    Other,
    Last, ///< The enumerator count; never an invocation.
};

/// Where in the invocation the save is placed (`FC_EDIT_STUB_AT`). Private.
enum class SaveAt : std::uint8_t
{
    Never,
    AfterCompile,
    BeforeDispatchPreprocess,
    Last, ///< The enumerator count; never a placement.
};

/// One row of the classification: an invocation kind, the switches one of which it needs,
/// and the switches none of which it may carry.
struct ClassifyRow
{
    /// The kind this row recognises.
    Invocation kind { Invocation::Other };
    /// At least one of these must be present.
    std::span<std::string_view const> anyOf;
    /// None of these may be present.
    std::span<std::string_view const> noneOf;
};

constexpr std::array<std::string_view, 2> CompileSwitches { "-c", "/c" };
constexpr std::array<std::string_view, 7> NotACompile { "-E", "/E", "-EP", "/EP", "-P", "/P", "-###" };
constexpr std::array<std::string_view, 2> PreprocessSwitches { "-E", "/E" };
constexpr std::array<std::string_view, 6> NotADispatchPreprocess { "-P", "/P", "-EP", "/EP", "-v", "-###" };

/// The classification, first match wins; no row matching is `Invocation::Other`.
constexpr std::array<ClassifyRow, 2> ClassifyTable {
    ClassifyRow { .kind = Invocation::RealCompile, .anyOf = CompileSwitches, .noneOf = NotACompile },
    ClassifyRow { .kind = Invocation::DispatchPreprocess, .anyOf = PreprocessSwitches, .noneOf = NotADispatchPreprocess },
};

/// The spelling of one `SaveAt` in `FC_EDIT_STUB_AT`.
struct SaveAtRow
{
    /// The variable's value.
    std::string_view spelling;
    /// What it asks for.
    SaveAt at { SaveAt::Never };
};

constexpr std::array<SaveAtRow, 2> SaveAtSpellings {
    SaveAtRow { .spelling = "after-compile", .at = SaveAt::AfterCompile },
    SaveAtRow { .spelling = "before-dispatch-preprocess", .at = SaveAt::BeforeDispatchPreprocess },
};

/// Classifies one invocation by its arguments.
/// @param arguments The arguments after the program name.
/// @return The invocation's kind.
[[nodiscard]] Invocation Classify(std::span<char* const> arguments)
{
    auto const names = [&arguments](std::span<std::string_view const> switches) {
        return std::ranges::any_of(arguments, [&switches](char const* argument) {
            return std::ranges::find(switches, std::string_view { argument }) != switches.end();
        });
    };
    for (auto const& row: ClassifyTable)
        if (names(row.anyOf) && !names(row.noneOf))
            return row.kind;
    return Invocation::Other;
}

/// Reads one environment variable naming a path. A test stub reads its instructions from the
/// environment its fixture set, which is the only channel through the launcher.
/// @param name The variable's name.
/// @return Its value, or nothing when it is unset or empty.
[[nodiscard]] std::optional<std::filesystem::path> ReadVariable(std::string_view name)
{
#if defined(_WIN32)
    // Wide, so a path that is not ASCII survives: this names files, not text.
    std::wstring const wide(name.begin(), name.end());
    auto const size = ::GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0)
        return std::nullopt;
    std::wstring value(size, L'\0');
    auto const written = ::GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    if (written == 0 || written >= size)
        return std::nullopt;
    value.resize(written);
    return std::filesystem::path { value };
#else
    std::string const key { name };
    char const* const value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0')
        return std::nullopt;
    return std::filesystem::path { value };
#endif
}

/// Reads `FC_EDIT_STUB_AT`.
/// @return Where the save is placed, `SaveAt::Never` when the variable is unset; nothing when
///         it names no placement.
[[nodiscard]] std::optional<SaveAt> ReadSaveAt()
{
    auto const value = ReadVariable("FC_EDIT_STUB_AT");
    if (!value)
        return SaveAt::Never;
    auto const spelling = value->string();
    // A scan for the VALUE, naming no iterator: `readability-qualified-auto`'s fix for a named
    // `std::array` iterator does not compile on MSVC (build-and-toolchain.md).
    for (auto const& row: SaveAtSpellings)
        if (row.spelling == spelling)
            return row.at;
    return std::nullopt;
}

/// Performs the save once, while the trigger exists: FROM's bytes overwrite TO in place, then
/// the trigger is removed.
/// @return True when the save was done or the trigger is gone; false when it failed, said why.
[[nodiscard]] bool SaveOnce()
{
    auto const trigger = ReadVariable("FC_EDIT_STUB_TRIGGER");
    auto const from = ReadVariable("FC_EDIT_STUB_FROM");
    auto const to = ReadVariable("FC_EDIT_STUB_TO");
    if (!trigger || !from || !to)
    {
        std::println(stderr,
                     "mid-compile-edit-stub: FC_EDIT_STUB_AT needs FC_EDIT_STUB_TRIGGER, FC_EDIT_STUB_FROM and "
                     "FC_EDIT_STUB_TO");
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(*trigger, error))
        return true;

    std::ifstream input { *from, std::ios::binary };
    if (!input)
    {
        std::println(stderr, "mid-compile-edit-stub: cannot read {}", from->string());
        return false;
    }
    // Read through the stream buffer, never `istreambuf_iterator` (#1029: GCC -O2 -Wnull-dereference).
    std::ostringstream buffered;
    buffered << input.rdbuf();
    std::string const bytes = std::move(buffered).str();
    {
        // A truncating write into the SAME file, the way an editor saves: never a copy.
        std::ofstream output { *to, std::ios::binary | std::ios::trunc };
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output)
        {
            std::println(stderr, "mid-compile-edit-stub: cannot write {}", to->string());
            return false;
        }
    }
    if (!std::filesystem::remove(*trigger, error) || error)
    {
        std::println(stderr, "mid-compile-edit-stub: cannot remove the trigger {}", trigger->string());
        return false;
    }
    return true;
}

#if defined(_WIN32)

/// The command line after the program name, verbatim, so every argument reaches the real
/// compiler exactly as the launcher quoted it. The program name ends at its closing quote when
/// it opens with one, at the first blank otherwise -- `CommandLineToArgvW`'s rule for argv[0].
/// @param line The whole command line.
/// @return Everything after the program name and the blanks following it.
[[nodiscard]] std::wstring_view CommandLineTail(std::wstring_view line)
{
    auto const isBlank = [](wchar_t c) {
        return c == L' ' || c == L'\t';
    };
    auto rest = line;
    if (rest.starts_with(L'"'))
    {
        auto const close = rest.find(L'"', 1);
        rest = close == std::wstring_view::npos ? std::wstring_view {} : rest.substr(close + 1);
    }
    else
    {
        auto const blank = std::ranges::find_if(rest, isBlank);
        rest = rest.substr(static_cast<std::size_t>(std::ranges::distance(rest.begin(), blank)));
    }
    auto const first = std::ranges::find_if_not(rest, isBlank);
    return rest.substr(static_cast<std::size_t>(std::ranges::distance(rest.begin(), first)));
}

/// Runs the real compiler with this process's own command line and waits for it.
/// @param real The real compiler.
/// @return Its exit code, or `StubFailure` when it could not be started.
[[nodiscard]] int Forward(std::filesystem::path const& real)
{
    std::wstring commandLine = L"\"" + real.wstring() + L"\"";
    auto const tail = CommandLineTail(::GetCommandLineW());
    if (!tail.empty())
        commandLine += L" " + std::wstring { tail };

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process {};
    // Inheriting is what hands the child this stub's standard handles; a test stub that waits
    // for its one child holds no other handle worth keeping from it.
    if (::CreateProcessW(real.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)
        == 0)
    {
        std::println(stderr, "mid-compile-edit-stub: cannot start {} (error {})", real.string(), ::GetLastError());
        return StubFailure;
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = StubFailure;
    if (::GetExitCodeProcess(process.hProcess, &code) == 0)
        code = StubFailure;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

#else

/// Runs the real compiler with this process's own arguments and environment and waits for it.
/// @param real        The real compiler.
/// @param arguments   The arguments after the program name.
/// @param environment This process's environment, as `main` received it.
/// @return Its exit code; 128 plus the signal when a signal ended it; `StubFailure` when it
///         could not be started.
[[nodiscard]] int Forward(std::filesystem::path const& real, std::span<char* const> arguments, char** environment)
{
    std::string program = real.string();
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(program.data());
    std::ranges::copy(arguments, std::back_inserter(argv));
    argv.push_back(nullptr);

    pid_t child = 0;
    if (auto const error = ::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), environment); error != 0)
    {
        std::println(stderr, "mid-compile-edit-stub: cannot start {} (errno {})", program, error);
        return StubFailure;
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return StubFailure;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return StubFailure;
}

#endif

} // namespace

#if defined(_WIN32)
int main(int argc, char** argv)
#else
int main(int argc, char** argv, char** environment)
#endif
{
    auto const arguments = std::span<char* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    auto const real = ReadVariable("FC_EDIT_STUB_REAL");
    if (!real)
    {
        std::println(stderr, "mid-compile-edit-stub: FC_EDIT_STUB_REAL names no compiler to forward to");
        return StubFailure;
    }
    auto const at = ReadSaveAt();
    if (!at)
    {
        std::println(stderr,
                     "mid-compile-edit-stub: FC_EDIT_STUB_AT is neither after-compile nor before-dispatch-preprocess");
        return StubFailure;
    }
    auto const kind = Classify(arguments);

    if (*at == SaveAt::BeforeDispatchPreprocess && kind == Invocation::DispatchPreprocess && !SaveOnce())
        return StubFailure;
#if defined(_WIN32)
    auto const code = Forward(*real);
#else
    auto const code = Forward(*real, arguments, environment);
#endif
    if (*at == SaveAt::AfterCompile && kind == Invocation::RealCompile && code == 0 && !SaveOnce())
        return StubFailure;
    return code;
}
