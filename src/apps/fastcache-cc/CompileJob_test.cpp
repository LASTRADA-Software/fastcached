// SPDX-License-Identifier: Apache-2.0
#include "CompileJob.hpp"
#include "ScratchPathTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Cc;

namespace
{

/// A process runner that records the argv it was handed and writes a canned
/// object, so a job can be driven to completion without a compiler installed.
class ScriptedRunner final: public IProcessRunner
{
  public:
    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        _argv.assign(argv.begin(), argv.end());
        if (_exitCode == 0 && _writeObject)
        {
            Test::WriteStubObject(_argv);
        }
        return CompileRun { .exitCode = _exitCode, .out = _stdoutText, .err = _stderrText };
    }

    [[nodiscard]] std::vector<std::string> const& Argv() const noexcept
    {
        return _argv;
    }

    /// Script the next run. Setters rather than public fields, which clang-tidy's
    /// non-private-member rule requires of a class with any private state.
    void ScriptExit(int code) noexcept
    {
        _exitCode = code;
    }
    void ScriptNoObject() noexcept
    {
        _writeObject = false;
    }
    void ScriptStderr(std::string text)
    {
        _stderrText = std::move(text);
    }

  private:
    int _exitCode { 0 };
    bool _writeObject { true };
    std::string _stdoutText;
    std::string _stderrText;
    std::vector<std::string> _argv;
};

/// A scratch root that cleans itself up.
struct ScratchDir
{
    std::filesystem::path path;

    ScratchDir()
    {
        // UniqueScratchPath, not a bare counter: every TEST_CASE is its own
        // PROCESS under catch_discover_tests, so a per-process counter hands two
        // concurrent cases the same directory and the second wipes the first.
        path = Test::UniqueScratchPath("fc-jobtest");
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ScratchDir(ScratchDir const&) = delete;
    ScratchDir& operator=(ScratchDir const&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
};

[[nodiscard]] CompileJob Job(std::vector<std::string> args = { "-O2" })
{
    return CompileJob {
        .fingerprint = "gcc-13", .args = std::move(args), .preprocessed = "int main() { return 0; }", .sourceName = "a.cpp"
    };
}

} // namespace

TEST_CASE("A worker takes its compiler from its own configuration, never from the job", "[compile-job]")
{
    // The single most important property here. A job that could name its own
    // compiler would let anyone who can reach the port run an arbitrary program --
    // not a hardening detail, but the difference between a build accelerator and a
    // remote shell.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "/opt/real/g++" } } };

    REQUIRE(jobs.Run(Job()).has_value());
    REQUIRE_FALSE(runner.Argv().empty());
    CHECK(runner.Argv().front() == "/opt/real/g++");
}

TEST_CASE("An unknown fingerprint is refused, never served with a default", "[compile-job]")
{
    // The scheduler should not have sent it, but a client that reached this port
    // directly did not go through scheduling at all.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "/opt/real/g++" } } };

    auto job = Job();
    job.fingerprint = "clang-19";
    auto const result = jobs.Run(job);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::UnknownFingerprint);
    // Nothing was spawned.
    CHECK(runner.Argv().empty());
}

TEST_CASE("A worker with no toolchains serves nothing", "[compile-job]")
{
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, {} };
    CHECK(jobs.Run(Job()).error() == JobRefusal::UnknownFingerprint);
    CHECK(jobs.Fingerprints().empty());
}

TEST_CASE("An argument that could name a file is refused by the worker too", "[compile-job]")
{
    // Checked again on the receiving side. The client's filter protects an honest
    // client from dispatching something that would not work; this one protects the
    // worker from a client that is not honest. Trusting the client's check would
    // mean the worker is secured by code running on the caller's machine.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "/opt/real/g++" } } };

    for (auto const& hostile: { "-fplugin=/tmp/evil.so", "@/tmp/args.rsp", "--sysroot=/", R"(-IC:\x)" })
    {
        INFO("argument " << hostile);
        auto const result = jobs.Run(Job({ std::string { hostile } }));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == JobRefusal::RejectedArgument);
    }
    CHECK(runner.Argv().empty()); // nothing was ever spawned
}

TEST_CASE("The worker decides where a byte lands, whatever the client asked to call it", "[compile-job]")
{
    // The client now names its file, so this string genuinely arrives over a socket
    // and becomes a path -- where before it was a theoretical defence for a field
    // nothing encoded. What it may decide is the NAME; what it may never decide is
    // the directory.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto job = Job();
    job.sourceName = "../../../etc/passwd.cpp";
    REQUIRE(jobs.Run(job).has_value());

    // The LEAF may be what the client asked for -- that is the feature -- and the
    // directory may not. So the assertion is containment, not absence: every path
    // on the command line lies under this worker's own scratch root, and no
    // parent-directory segment survived to take one back out of it.
    auto const root = std::filesystem::weakly_canonical(scratch.path).generic_string();
    for (auto const& arg: runner.Argv())
    {
        INFO("argv entry: " << arg);
        CHECK_FALSE(arg.contains(".."));
        if (arg.contains("passwd"))
            CHECK(std::filesystem::weakly_canonical(arg).generic_string().starts_with(root));
    }
}

TEST_CASE("The worker gives its scratch file the name the client asked for", "[compile-job]")
{
    // A compiler records the name of the file it was handed, so a worker that
    // invents one produces an object differing from a locally compiled one in that
    // name and nothing else -- seven bytes on clang-cl, and the reason this travels
    // at all.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto job = Job();
    job.sourceName = "Widget.cpp";
    REQUIRE(jobs.Run(job).has_value());

    CHECK(std::ranges::any_of(runner.Argv(), [](std::string const& a) { return a.ends_with("Widget.cpp"); }));
}

TEST_CASE("A successful compile returns the object the compiler wrote", "[compile-job]")
{
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto const result = jobs.Run(Job());
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 0);
    CHECK(std::string(reinterpret_cast<char const*>(result->object.data()), result->object.size()) == "OBJECT");
}

TEST_CASE("A failing compile returns its diagnostics and no object", "[compile-job]")
{
    // The compiler RAN and rejected the code. That is the client's answer, and it
    // must be distinguishable from the worker being broken.
    ScriptedRunner runner;
    runner.ScriptExit(1);
    runner.ScriptStderr("error: no");
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto const result = jobs.Run(Job());
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 1);
    CHECK(result->stderrText == "error: no");
    CHECK(result->object.empty());
}

TEST_CASE("A compiler that cannot be spawned is not a failed compile", "[compile-job]")
{
    // The client must be able to tell "this worker is broken" from "your code does
    // not compile", because only the second is its answer -- and the first should
    // send the job somewhere else rather than fail the build.
    ScriptedRunner runner;
    runner.ScriptExit(-1);
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto const result = jobs.Run(Job());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::SpawnFailed);
}

TEST_CASE("A compiler claiming success but writing nothing is refused", "[compile-job]")
{
    // Returned as an empty object, the client would write it to disk and cache it.
    ScriptedRunner runner;
    runner.ScriptNoObject();
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    auto const result = jobs.Run(Job());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::ScratchUnavailable);
}

TEST_CASE("Each job gets its own scratch directory, and it is removed", "[compile-job]")
{
    // A worker leaking a directory per job fills its disk during a long build.
    ScriptedRunner runner;
    ScratchDir scratch;
    CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "g++" } } };

    REQUIRE(jobs.Run(Job()).has_value());
    auto const first = runner.Argv();
    REQUIRE(jobs.Run(Job()).has_value());
    CHECK(first != runner.Argv()); // a different scratch path each time

    CHECK(std::filesystem::is_empty(scratch.path));
}

TEST_CASE("SafeSourceName keeps a name that is one, and replaces every name that is not", "[compile-job]")
{
    // Kept: an ordinary base name with a known extension.
    CHECK(SafeSourceName("a.cpp") == "a.cpp");
    CHECK(SafeSourceName("Widget.cc") == "Widget.cc");
    CHECK(SafeSourceName("legacy_module-2.c") == "legacy_module-2.c");

    // One component only, in either separator style, so nothing decides a directory.
    CHECK(SafeSourceName("../../../etc/passwd.cpp") == "passwd.cpp");
    CHECK(SafeSourceName("..\\..\\windows\\system32\\evil.cpp") == "evil.cpp");
    CHECK(SafeSourceName("/absolute/a.cpp") == "a.cpp");
    // A colon counts as a separator: `C:a.cpp` is drive-relative on a Windows worker.
    CHECK(SafeSourceName("C:a.cpp") == "a.cpp");
    // And a name that is nothing BUT an escape has nothing left to keep.
    CHECK(SafeSourceName("..") == "tu.cpp");
    CHECK(SafeSourceName("../..") == "tu.cpp");

    // An unrecognised extension becomes the common case rather than reaching a
    // compiler's command line.
    CHECK(SafeSourceName("a.exe") == "a.cpp");
    CHECK(SafeSourceName("a") == "a.cpp");
    CHECK(SafeSourceName("") == "tu.cpp");

    // A stem that is not an allow-listed shape is replaced whole, so nothing a
    // shell or a compiler treats specially survives.
    CHECK(SafeSourceName("a.cpp; rm -rf /") == "tu.cpp");
    CHECK(SafeSourceName("a b.cpp") == "tu.cpp");
    CHECK(SafeSourceName("$(whoami).cpp") == "tu.cpp");
    CHECK(SafeSourceName(".hidden.cpp") == "tu.cpp");
    CHECK(SafeSourceName(std::string(65, 'x') + ".cpp") == "tu.cpp");

    // Windows device names, with and without an extension and in any case: on a
    // Windows worker `CON.cpp` is the console, so the translation unit would be
    // written to a terminal and the compile would find nothing to read.
    CHECK(SafeSourceName("CON.cpp") == "tu.cpp");
    CHECK(SafeSourceName("con.cpp") == "tu.cpp");
    CHECK(SafeSourceName("NUL.c") == "tu.c");
    CHECK(SafeSourceName("com1.cpp") == "tu.cpp");
    CHECK(SafeSourceName("LPT9.cc") == "tu.cc");
    // ... and a name that merely starts like one is ordinary.
    CHECK(SafeSourceName("console.cpp") == "console.cpp");
}

TEST_CASE("IsAcceptableJobArgument applies the strictest reading", "[compile-job]")
{
    // Stricter than the client's filter, and deliberately: a worker is told a
    // fingerprint rather than a command line, so it has no driver to ask which
    // characters introduce an option. A job it refuses is compiled locally.
    CHECK(IsAcceptableJobArgument("-O2"));
    CHECK(IsAcceptableJobArgument("-std=c++23"));
    CHECK(IsAcceptableJobArgument("/O2"));
    CHECK(IsAcceptableJobArgument(""));

    CHECK_FALSE(IsAcceptableJobArgument("@rsp"));
    CHECK_FALSE(IsAcceptableJobArgument("-I/usr/include"));
    CHECK_FALSE(IsAcceptableJobArgument(R"(-IC:\x)"));
    CHECK_FALSE(IsAcceptableJobArgument("/some/path"));
}

TEST_CASE("The output flag follows the worker's own driver family", "[compile-job]")
{
    // `cl` does not accept `-o`. Hard-coding it meant MSVC wrote `tu.obj` beside
    // the source, exited 0, and the worker then found nothing at the path it had
    // asked for -- so it refused the job and distribution never worked on Windows
    // at all, while reporting "storage write failed" about it.
    //
    // The family comes from the worker's OWN configured compiler, never from
    // anything the client sent, which is the same rule that decides which program
    // runs at all.
    SECTION("an MSVC toolchain gets /Fo, fused")
    {
        ScriptedRunner runner;
        ScratchDir scratch;
        CompileJobRunner jobs { runner, scratch.path, { { "msvc", R"(C:\MSVC\bin\cl.exe)" } } };

        auto job = Job({});
        job.fingerprint = "msvc";
        (void) jobs.Run(job);

        auto const& argv = runner.Argv();
        REQUIRE(!argv.empty());
        CHECK(std::ranges::any_of(argv, [](std::string const& a) { return a.starts_with("/Fo"); }));
        // A bare `-o` would be consumed by cl as something else entirely.
        CHECK(std::ranges::find(argv, "-o") == argv.end());
    }

    SECTION("a GNU toolchain still gets -o")
    {
        ScriptedRunner runner;
        ScratchDir scratch;
        CompileJobRunner jobs { runner, scratch.path, { { "gcc-13", "/opt/real/g++" } } };

        (void) jobs.Run(Job({}));

        auto const& argv = runner.Argv();
        REQUIRE(!argv.empty());
        CHECK(std::ranges::any_of(argv, [](std::string const& a) { return a.starts_with("-o"); }));
        CHECK(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("/Fo"); }));
    }
}
