// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"
#include "CompileJob.hpp"
#include "StubObjectTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <latch>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache::Cc;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::Unwrap;

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
        // Before the stub object is written, so an observer sees exactly what the WORKER
        // created rather than what this fake added. The scratch directory is removed
        // when `Run` returns, so a test that looked afterwards would find nothing and
        // pass whatever the worker had done.
        if (_onSpawn)
            _onSpawn();
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
    /// Look at the world at the moment the compiler would have been spawned.
    ///
    /// The only point at which what the worker WROTE is observable: `ScratchGuard`
    /// removes the directory however `Run` returns, so a test inspecting the filesystem
    /// afterwards finds an empty parent and passes regardless.
    /// @param observer Called with no arguments, before the stub object is written.
    void ScriptOnSpawn(std::function<void()> observer)
    {
        _onSpawn = std::move(observer);
    }

  private:
    int _exitCode { 0 };
    bool _writeObject { true };
    std::string _stdoutText;
    std::string _stderrText;
    std::vector<std::string> _argv;
    std::function<void()> _onSpawn;
};

/// A runner that echoes each invocation's own SOURCE back as its object, and does
/// not return until `Overlap` of them are inside it at once.
///
/// Both halves are the test. The echo makes "this job got another job's bytes"
/// visible, where a canned object would look identical either way. The barrier
/// makes the overlap a fact rather than a hope -- a race reproduced by timing is a
/// test that passes on the machine that has the bug.
class OverlappingEchoRunner final: public IProcessRunner
{
  public:
    static constexpr std::size_t Overlap = 2;

    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        // argv is [compiler, ...args, "-c", <source>, "-o<object>"].
        std::string source;
        std::string object;
        for (auto const& arg: argv)
        {
            if (arg.starts_with("-o") && arg.size() > 2)
                object = arg.substr(2);
            else if (arg.ends_with(".cpp"))
                source = arg;
        }

        // Everyone waits until every job has written its source, so a shared scratch
        // directory has certainly been shared by the time anything is read back.
        {
            auto guard = std::unique_lock { _mutex };
            ++_inside;
            _changed.notify_all();
            // Bounded, and it says what it waited for: the other job reaching the
            // compiler. Unbounded here would hang the suite rather than fail it.
            (void) _changed.wait_for(guard, std::chrono::seconds { 5 }, [this] { return _inside >= Overlap; });
        }

        // Sized and read in one go rather than through `istreambuf_iterator`, for
        // the reason `CompileJob.cpp` already records: GCC at -O3 inlines that
        // iterator's `sgetc` and reports a potential null dereference inside
        // libstdc++'s own `streambuf`, which `-Werror` turns into a failed build on
        // one compiler only. Invisible under the debug preset this was written
        // against.
        std::ifstream in { source, std::ios::binary | std::ios::ate };
        auto const size = in.tellg();
        std::string text;
        if (size > 0)
        {
            text.resize(static_cast<std::size_t>(size));
            in.seekg(0, std::ios::beg);
            in.read(text.data(), size);
        }
        std::ofstream { object, std::ios::binary } << text;
        return CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

    /// Rearm for the next pair. Called from the test thread with nothing inside.
    void NextRound()
    {
        auto const guard = std::scoped_lock { _mutex };
        _inside = 0;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _inside { 0 };
};

/// A scratch root that cleans itself up.

[[nodiscard]] CompileJob Job(std::vector<std::string> args = { "-O2" })
{
    return CompileJob { .fingerprint = "gcc-13",
                        .args = std::move(args),
                        .preprocessed = "int main() { return 0; }",
                        .sourceName = "a.cpp",
                        .compileDir = {},
                        .compileDirReplacement = {} };
}

} // namespace

TEST_CASE("A worker takes its compiler from its own configuration, never from the job", "[compile-job]")
{
    // The single most important property here. A job that could name its own
    // compiler would let anyone who can reach the port run an arbitrary program --
    // not a hardening detail, but the difference between a build accelerator and a
    // remote shell.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    REQUIRE(jobs.Run(Job()).has_value());
    REQUIRE_FALSE(runner.Argv().empty());
    CHECK(runner.Argv().front() == "/opt/real/g++");
}

TEST_CASE("An unknown fingerprint is refused, never served with a default", "[compile-job]")
{
    // The scheduler should not have sent it, but a client that reached this port
    // directly did not go through scheduling at all.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), {}, ToolchainSurvey::Completed() };
    CHECK(jobs.Run(Job()).error() == JobRefusal::UnknownFingerprint);
    CHECK(jobs.Fingerprints().empty());
}

TEST_CASE("A worker still surveying refuses by name, not as an unknown fingerprint", "[compile-job]")
{
    // The case above and this one hold the SAME empty map, and that is the point:
    // nothing about the map distinguishes "this machine serves nothing" from "this
    // machine has not been asked yet", so a test that only checked the empty case
    // would pass under either answer. A node serves its cache tier while it walks its
    // include trees (#365, over 300 s on a cold Windows runner per #354), so a client
    // reaching the compile port during that window is a real state and not a
    // hypothetical.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), {}, ToolchainSurvey::InFlight() };

    auto const refused = jobs.Run(Job());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == JobRefusal::ToolchainSurveyInFlight);
    // Refused before anything was spawned, and before a scratch directory was made:
    // a worker that has not been surveyed cannot know which program to run.
    CHECK(runner.Argv().empty());

    // And the survey ARRIVING is what ends it -- the same call the heartbeat's
    // re-survey makes, so there is one way into the serving state rather than two.
    jobs.ReplaceToolchains({ { "gcc-13", "/opt/real/g++" } });
    REQUIRE(jobs.Run(Job()).has_value());
    REQUIRE_FALSE(runner.Argv().empty());
    CHECK(runner.Argv().front() == "/opt/real/g++");
}

TEST_CASE("A survey that answers with nothing is not a survey in flight", "[compile-job]")
{
    // The other direction, and the one an operator meets on a machine whose only
    // compiler was uninstalled: surveyed, serving nothing. It must NOT keep reporting
    // "still starting" forever -- that is a node that never becomes diagnosable.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), {}, ToolchainSurvey::InFlight() };
    jobs.ReplaceToolchains({});

    auto const refused = jobs.Run(Job());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == JobRefusal::UnknownFingerprint);
}

TEST_CASE("An argument the worker's allowlist does not admit is refused", "[compile-job]")
{
    // Checked again on the receiving side, against an allowlist. The client's filter
    // protects an honest client from dispatching something that would not work; this
    // one protects the worker from a client that is not honest. Trusting the client's
    // check would mean the worker is secured by code running on the caller's machine.
    //
    // The program-invoking options are the reason the check is an allowlist and not a
    // denylist (issue #240): `-wrapper` runs an arbitrary program, `-fplugin=` and
    // `-Xclang -load` load code, and NONE carries a path separator, so the old shape
    // filter admitted every one. `-fplugin=/tmp/evil.so` and `-IC:\x` are the
    // path-shaped classics the denylist did catch, kept here so the allowlist is shown
    // to be a superset rather than a replacement that lost ground.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    for (auto const& hostile: { "-wrapper",
                                "-fplugin=evil",
                                "-fplugin=/tmp/evil.so",
                                "-Xclang",
                                "-Wa,--defsym,x=1",
                                "-specs=evil",
                                "@/tmp/args.rsp",
                                "--sysroot=/",
                                R"(-IC:\x)" })
    {
        INFO("argument " << hostile);
        auto const result = jobs.Run(Job({ std::string { hostile } }));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == JobRefusal::RejectedArgument);
        // The refusal names the offending flag, so a client's local fallback can say
        // which argument the fleet would not take rather than only that one existed.
        CHECK(result.error().detail.contains(hostile));
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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto job = Job();
    job.sourceName = "../../../etc/passwd.cpp";

    // Everything the worker created, captured while the directory still exists:
    // `ScratchGuard` removes it however `Run` returns, so a test looking afterwards
    // finds an empty parent and passes whatever happened.
    std::vector<std::string> created;
    runner.ScriptOnSpawn([&] {
        for (auto const& entry: std::filesystem::recursive_directory_iterator { scratch.Path() })
            created.push_back(std::filesystem::weakly_canonical(entry.path()).generic_string());
    });

    REQUIRE(jobs.Run(job).has_value());

    auto const root = std::filesystem::weakly_canonical(scratch.Path()).generic_string();

    // **What the worker WROTE, which is the guarantee.** Every path it created lies
    // under its own scratch root and carries no parent-directory segment that could
    // take one back out. This is the assertion the invariant is about: the client may
    // decide a NAME, and may never decide a directory.
    REQUIRE_FALSE(created.empty());
    for (auto const& path: created)
    {
        INFO("created: " << path);
        CHECK(path.starts_with(root));
        CHECK_FALSE(path.contains(".."));
    }

    // And the traversal target does not exist, which is the same statement said the
    // other way round -- a check that would still pass if the worker had created
    // nothing at all, which is why it stands beside the one above rather than instead
    // of it.
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path { root }.parent_path().parent_path() / "etc" / "passwd.cpp"));

    // **The paths on the command line, told apart from the strings on it.** An operand
    // is a path the compiler opens; a `-fdebug-prefix-map` rule's right-hand side is a
    // string it records, and since #660 that side carries the client's own spelling --
    // `../../../etc/passwd.cpp` and all. The two are not the same kind of thing, and a
    // blanket "no argv entry contains `..`" cannot tell them apart: it passed before
    // #660 only because nothing yet put a client-sent string on the line.
    for (auto const& arg: runner.Argv())
    {
        INFO("argv entry: " << arg);
        auto const rule = arg.starts_with("-fdebug-prefix-map=");
        if (!rule)
        {
            CHECK_FALSE(arg.contains(".."));
            if (arg.contains("passwd"))
                CHECK(std::filesystem::weakly_canonical(arg).generic_string().starts_with(root));
            continue;
        }

        // The rule's LEFT-hand side is the path the compiler matches, and the worker
        // derives it from its own scratch. Only the right-hand side is the client's.
        auto const body = std::string_view { arg }.substr(std::string_view { "-fdebug-prefix-map=" }.size());
        auto const split = body.find('=');
        REQUIRE(split != std::string_view::npos);
        auto const from = body.substr(0, split);
        INFO("rule <from>: " << from);
        CHECK_FALSE(from.contains(".."));
        CHECK(std::filesystem::weakly_canonical(from).generic_string().starts_with(root));
    }
}

TEST_CASE("A dispatched object records the client's source spelling, not the worker's scratch",
          "[compile-job][prefix-map][source-name]")
{
    // **[#660](https://github.com/LASTRADA-Software/fastcached/issues/660).** clang takes
    // `DW_AT_name` from the input file path, so a dispatched object recorded
    // `<scratch>/job-N/tu.cpp` -- a directory on no developer's machine, and one whose
    // counter advances between two dispatches of the SAME translation unit, giving two
    // byte-differing objects under one cache key. gcc is unaffected: it takes the name
    // from the `#line` marker and already records what a local compile does.
    //
    // Measured on clang 22.1.8 and gcc 16.2.1, ELF, `readelf --debug-dump=info`: with
    // this rule the recorded name is the client's spelling exactly, and two dispatches
    // under different job numbers produce byte-identical objects. Unmapped they differ
    // at byte 280.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto job = Job();
    job.sourceName = "../../src/Widget.cpp";
    // A build that also asked for a compilation-directory mapping, because the ORDER
    // against that rule is what this case is here to pin and there is nothing to order
    // against without it.
    job.compileDir = "/home/ci/build";
    job.compileDirReplacement = ".";
    REQUIRE(jobs.Run(job).has_value());

    auto const& argv = runner.Argv();
    auto const rule = std::ranges::find_if(argv, [](std::string const& a) {
        return a.starts_with("-fdebug-prefix-map=") && a.ends_with("=../../src/Widget.cpp");
    });
    REQUIRE(rule != argv.end());

    // **The WHOLE path is mapped, not the directory**, which is the narrowest rule that
    // works: it matches exactly one path, so it cannot reach `DW_AT_comp_dir` or
    // anything else, and it survives `SafeSourceName` having renamed the scratch file.
    // The left-hand side is therefore the input operand, byte for byte.
    auto const input =
        std::ranges::find_if(argv, [](std::string const& a) { return a.ends_with("Widget.cpp") && !a.starts_with("-"); });
    REQUIRE(input != argv.end());
    CHECK(*rule == std::format("-fdebug-prefix-map={}=../../src/Widget.cpp", *input));

    // **LAST, and the order is what makes it work at all.** Both drivers honour the last
    // matching rule, and the worker's own directory rule matches the scratch path
    // whenever the scratch root lies under it. Measured on clang 22.1.8 with both rules
    // present: this one FIRST gives `./scratch/job-7/tu.cpp`, still broken; this one
    // LAST gives `../src/tu.cpp` with `DW_AT_comp_dir` still `.`.
    CHECK(rule > std::ranges::find_if(
              argv, [](std::string const& a) { return a.starts_with("-fdebug-prefix-map=") && a.ends_with("=."); }));
}

TEST_CASE("Two dispatches of one translation unit record the same source name", "[compile-job][prefix-map][source-name]")
{
    // The ticket's second consequence, which is the one that matters: the scratch
    // directory carries a per-job counter, so before #660 two dispatches of the same
    // translation unit to the same worker produced objects differing in their recorded
    // name -- stored under one cache key, so which bytes won was a race and a rebuild
    // that ought to be a no-op was not.
    //
    // It falls out of the rule above rather than needing a mechanism of its own:
    // mapping the scratch path away removes `job-N` from the recorded name entirely.
    // Asserted on the RULE rather than by comparing objects, which is this repository's
    // standing instruction -- every MSVC driver stamps the clock into the COFF header,
    // so a byte comparison reports a wrong object on every hit.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto job = Job();
    job.sourceName = "../../src/Widget.cpp";

    auto const recordedNameOf = [&](std::vector<std::string> const& argv) {
        auto const rule =
            std::ranges::find_if(argv, [](std::string const& a) { return a.starts_with("-fdebug-prefix-map="); });
        REQUIRE(rule != argv.end());
        return rule->substr(rule->find_last_of('=') + 1);
    };

    REQUIRE(jobs.Run(job).has_value());
    auto const first = runner.Argv();
    REQUIRE(jobs.Run(job).has_value());
    auto const second = runner.Argv();

    // The counter DID advance, or this case proves nothing: two runs that happened to
    // share a scratch directory would agree for the wrong reason.
    auto const scratchOf = [](std::vector<std::string> const& argv) {
        auto const input = std::ranges::find_if(
            argv, [](std::string const& a) { return a.ends_with("Widget.cpp") && !a.starts_with("-"); });
        REQUIRE(input != argv.end());
        return std::filesystem::path { *input }.parent_path().filename().string();
    };
    REQUIRE(scratchOf(first) != scratchOf(second));

    CHECK(recordedNameOf(first) == recordedNameOf(second));
    CHECK(recordedNameOf(first) == "../../src/Widget.cpp");
}

TEST_CASE("The worker gives its scratch file the name the client asked for", "[compile-job]")
{
    // A compiler records the name of the file it was handed, so a worker that
    // invents one produces an object differing from a locally compiled one in that
    // name and nothing else -- seven bytes on clang-cl, and the reason this travels
    // at all.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto job = Job();
    job.sourceName = "Widget.cpp";
    REQUIRE(jobs.Run(job).has_value());

    CHECK(std::ranges::any_of(runner.Argv(), [](std::string const& a) { return a.ends_with("Widget.cpp"); }));
}

TEST_CASE("A successful compile returns the object the compiler wrote", "[compile-job]")
{
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto const result = jobs.Run(Job());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::SpawnFailed);
}

TEST_CASE("A compiler claiming success but writing nothing is refused", "[compile-job]")
{
    // Returned as an empty object, the client would write it to disk and cache it.
    ScriptedRunner runner;
    runner.ScriptNoObject();
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    auto const result = jobs.Run(Job());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::ScratchUnavailable);
}

TEST_CASE("Each job gets its own scratch directory, and it is removed", "[compile-job]")
{
    // A worker leaking a directory per job fills its disk during a long build.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } }, ToolchainSurvey::Completed() };

    REQUIRE(jobs.Run(Job()).has_value());
    auto const first = runner.Argv();
    REQUIRE(jobs.Run(Job()).has_value());
    CHECK(first != runner.Argv()); // a different scratch path each time

    CHECK(std::filesystem::is_empty(scratch.Path()));
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

TEST_CASE("IsAcceptableJobArgument admits the code-generation flags a compile carries", "[compile-job]")
{
    // The accepted set is an allowlist keyed on the worker's own driver family. These
    // are the ordinary code-generation, language and diagnostic options a distributed
    // compile carries once the headers are inlined -- everything that changes the
    // object and nothing that reaches a file or a program.
    CHECK(IsAcceptableJobArgument("-O2", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-std=c++23", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-g", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-Wall", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-Werror", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-Wno-unused", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-fPIC", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-fno-exceptions", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-DNDEBUG", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-march=native", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-pthread", DriverOf(Flavor::Gcc)));
    // THIS project's own `PEDANTIC_COMPILER` presets add `-Qunused-arguments` to
    // every clang build, and the client forwards it (it names no file). A table that
    // does not carry it makes the fleet refuse every job it dispatches to itself --
    // the "distributes nothing and goes green anyway" failure, visible only as a
    // counter -- so the row is asserted rather than assumed.
    CHECK(IsAcceptableJobArgument("-Qunused-arguments", DriverOf(Flavor::Clang)));
    CHECK(IsAcceptableJobArgument("-Qunused-arguments", DriverOf(Flavor::ClangCl)));
    // The language the client states for a preprocessed input -- `-x` and its value,
    // which arrive as two separate arguments.
    CHECK(IsAcceptableJobArgument("-x", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("c++-cpp-output", DriverOf(Flavor::Gcc)));

    CHECK(IsAcceptableJobArgument("/O2", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/std:c++20", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/W4", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/WX", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/EHsc", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/MD", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/Z7", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/DNDEBUG", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/permissive-", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/utf-8", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("/TP", DriverOf(Flavor::Cl)));

    // The target the CLIENT states so this worker cannot pick its own. Read out of
    // `TargetPinPrefixFor` rather than restated, so it is accepted by exactly the
    // drivers that are pinned -- both clang drivers -- and by no other. They have to
    // agree about this argument, or every dispatched clang compile becomes a
    // RejectedArgument the moment the pin lands.
    CHECK(IsAcceptableJobArgument("--target=x86_64-pc-windows-msvc19.51.36252", DriverOf(Flavor::ClangCl)));
    CHECK(IsAcceptableJobArgument("--target=x86_64-pc-linux-gnu", DriverOf(Flavor::Clang)));
    // `cl` and `gcc` state no target (`TargetPinPrefixFor` is empty for both), so the
    // pin never reaches them and is not admitted if a client invents one.
    CHECK_FALSE(IsAcceptableJobArgument("--target=x86_64-pc-linux-gnu", DriverOf(Flavor::Cl)));

    // clang-cl is family Msvc and is routinely handed GNU spellings. Refusing them
    // would turn every such build into a silent local fallback, which is why a row
    // stores its spelling WITHOUT an introducer and is matched against the family's
    // own introducer set -- one row covering `/O2` and `-O2` alike.
    CHECK(IsAcceptableJobArgument("-O2", DriverOf(Flavor::ClangCl)));
    CHECK(IsAcceptableJobArgument("-Wall", DriverOf(Flavor::ClangCl)));
    CHECK(IsAcceptableJobArgument("-fno-exceptions", DriverOf(Flavor::ClangCl)));
    // And an MSVC driver takes `-` for every option it spells with `/`.
    CHECK(IsAcceptableJobArgument("-O2", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("-std:c++20", DriverOf(Flavor::Cl)));
    CHECK(IsAcceptableJobArgument("-EHsc", DriverOf(Flavor::Cl)));
    // A GNU driver, by contrast, reads a leading `/` as a path and never as an
    // option, so an MSVC spelling is not admitted there.
    CHECK_FALSE(IsAcceptableJobArgument("/O2", DriverOf(Flavor::Gcc)));

    // An empty argument names nothing and reaches nothing.
    CHECK(IsAcceptableJobArgument("", DriverOf(Flavor::Gcc)));
}

TEST_CASE("IsAcceptableJobArgument refuses everything the allowlist does not name", "[compile-job]")
{
    // The default is refusal. A path-shaped argument is refused as before, but so is
    // any flag the table does not list -- which is the property the old denylist did
    // not have, and the whole point of the inversion.
    CHECK_FALSE(IsAcceptableJobArgument("@rsp", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-I/usr/include", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument(R"(-IC:\x)", DriverOf(Flavor::Cl)));
    CHECK_FALSE(IsAcceptableJobArgument("/some/path", DriverOf(Flavor::Cl)));
    // Not path-shaped, but not a code-generation flag either -- refused because it is
    // not on the list, not because it looks like a file.
    CHECK_FALSE(IsAcceptableJobArgument("-isystem", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-nonsense-option", DriverOf(Flavor::Gcc)));
    // A worker whose compiler classifies to no family accepts no non-empty argument,
    // which fails safe.
    CHECK_FALSE(IsAcceptableJobArgument("-O2", DriverOf(Flavor::Unknown)));
}

TEST_CASE("IsAcceptableJobArgument refuses the program-invoking options a denylist admitted", "[compile-job]")
{
    // The vulnerability this fix closes (issue #240). Every one of these makes the
    // driver run another program or load code into itself, and NONE carries a path
    // separator, so the old shape-based filter accepted them all -- a local process,
    // which loopback admits unconditionally, could reach the credential-free compile
    // port and run arbitrary code as the node's service account.
    //
    // `-wrapper prog,args` was proven against a real gcc to run `prog` around every
    // subprocess; the value below carries no separator, so a shape filter waves it
    // through.
    CHECK_FALSE(IsAcceptableJobArgument("-wrapper", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("env,sh,-c,touch pwned", DriverOf(Flavor::Gcc)));
    // The `-f` space is ENUMERATED, not prefixed, and these three are why. A blanket
    // `-f` prefix with a carve-out for `-fplugin` would still admit the other two:
    // `-fmodule-mapper=|prog` makes GCC spawn a subprocess and `-fpass-plugin=` is
    // Clang's pass-manager loader, and neither begins with `-fplugin` nor carries a
    // path separator. Enumeration is what makes the whole class fail closed.
    CHECK_FALSE(IsAcceptableJobArgument("-fplugin=evil", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-fplugin-arg-evil-x", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-fpass-plugin=evil.so", DriverOf(Flavor::Clang)));
    CHECK_FALSE(IsAcceptableJobArgument("-fmodule-mapper=|evil", DriverOf(Flavor::Gcc)));
    // A side-artefact flag is refused off the maintained table rather than a copy of
    // it, so only the object needing to come back stays one rule.
    CHECK_FALSE(IsAcceptableJobArgument("-fmodule-output=x.pcm", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("/Ycpch.h", DriverOf(Flavor::Cl)));
    // Profile options read and write files whose names the driver derives; not listed,
    // so refused.
    CHECK_FALSE(IsAcceptableJobArgument("-fprofile-generate", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-fprofile-use=x", DriverOf(Flavor::Gcc)));
    // `-gsplit-dwarf` writes a `.dwo` beside the object and only the object comes
    // back, which is why the debug flags are enumerated rather than prefixed on `-g`.
    CHECK_FALSE(IsAcceptableJobArgument("-gsplit-dwarf", DriverOf(Flavor::Gcc)));
    // The MSVC half of that same rule. `/Zi` and `/ZI` write a PDB beside the object,
    // which nothing on the wire carries -- `RemoteCompileArgs` refuses the command
    // line outright for them (`MsvcSharedPdb`), and admitting them here would hand a
    // client that skipped that check an object naming a PDB it never receives.
    CHECK_FALSE(IsAcceptableJobArgument("/Zi", DriverOf(Flavor::Cl)));
    CHECK_FALSE(IsAcceptableJobArgument("/ZI", DriverOf(Flavor::Cl)));
    CHECK_FALSE(IsAcceptableJobArgument("-Zi", DriverOf(Flavor::ClangCl)));
    // `-Wa,`/`-Wl,`/`-Wp,` hand a comma-separated option list to a sub-tool. Denied
    // under the `-W` prefix that admits warnings.
    CHECK_FALSE(IsAcceptableJobArgument("-Wa,--defsym,x=1", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-Wl,-rpath,x", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-Wp,-D,x", DriverOf(Flavor::Gcc)));
    // The clang plugin loader and the sub-tool option passers -- none is a prefix in
    // the table, so all are refused by default.
    CHECK_FALSE(IsAcceptableJobArgument("-Xclang", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-load", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-Xassembler", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-specs=evil", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-B", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument("-Bstage", DriverOf(Flavor::Gcc)));
    // `-mllvm` forwards its (separate) value into the backend's own option parser.
    CHECK_FALSE(IsAcceptableJobArgument("-mllvm", DriverOf(Flavor::Clang)));
    // MSVC's own plugin loader and linker pass-through, refused by absence.
    CHECK_FALSE(IsAcceptableJobArgument("/analyze:plugin", DriverOf(Flavor::Cl)));
    CHECK_FALSE(IsAcceptableJobArgument("/link", DriverOf(Flavor::Cl)));
    // The value-shape rule beneath a prefix: `-W` and `-D` admit a value, and a value
    // carrying a path separator is refused whatever prefix admitted the flag.
    CHECK_FALSE(IsAcceptableJobArgument("-DFOO=/etc/passwd", DriverOf(Flavor::Gcc)));
    CHECK_FALSE(IsAcceptableJobArgument(R"(/DFOO=C:\x)", DriverOf(Flavor::Cl)));
    // The warnings that only LOOK like the sub-tool passers stay accepted, so the
    // Deny rows have not swallowed a real warning.
    CHECK(IsAcceptableJobArgument("-Wall", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-Wattributes", DriverOf(Flavor::Gcc)));
    CHECK(IsAcceptableJobArgument("-Wpedantic", DriverOf(Flavor::Gcc)));
}

TEST_CASE("The allowlist admits the flags this repository's own builds dispatch with", "[compile-job]")
{
    // Too narrow an allowlist is a SILENT local fallback -- the fleet distributes
    // nothing and every build still goes green, which is the failure shape this
    // repository keeps rediscovering. These are taken from the real command lines the
    // presets produce, so a row deleted in a future edit fails here rather than in a
    // hit-rate graph a month later.
    //
    // `-Qunused-arguments` is the one that proves the point: `PedanticCompiler.cmake`
    // adds it to EVERY Clang build, the client forwards it, and it matched nothing --
    // so every job from a `clang-debug` build came back RejectedArgument.
    for (auto const& flag: { "-O2",
                             "-g",
                             "-std=c++23",
                             "-Wall",
                             "-Wextra",
                             "-Werror",
                             "-pedantic",
                             "-Qunused-arguments",
                             "-fsanitize=address",
                             "-fno-omit-frame-pointer",
                             "-pthread",
                             "-DNDEBUG",
                             "-fPIC",
                             "-fno-rtti",
                             "-march=native" })
    {
        INFO("clang flag " << flag);
        CHECK(IsAcceptableJobArgument(flag, DriverOf(Flavor::Clang)));
    }

    for (auto const& flag: { "/O2",
                             "/W4",
                             "/WX",
                             "/EHsc",
                             "/MD",
                             "/MDd",
                             "/Z7",
                             "/utf-8",
                             "/permissive-",
                             "/bigobj",
                             "/nologo",
                             "/DWIN32",
                             "/Zc:__cplusplus",
                             "/wd4996",
                             "/GR-",
                             "/TP",
                             "/std:c++20" })
    {
        INFO("cl flag " << flag);
        CHECK(IsAcceptableJobArgument(flag, DriverOf(Flavor::Cl)));
    }

    // clang-cl is family Msvc and takes the GNU spellings too. Scoping those rows to
    // the Gnu family refused them on the ONE driver that accepts them -- silently, and
    // only for clang-cl workers. `DriverFamily` cannot tell `cl` from `clang-cl`, so
    // a GNU-spelled row is `Any`; the fingerprint is what keeps both ends honest.
    for (auto const& flag: { "-O0",
                             "-O3",
                             "-g",
                             "-std=c++23",
                             "-march=native",
                             "-m64",
                             "-flto",
                             "-fstrict-aliasing",
                             "-fdiagnostics-color=always",
                             "-Wall",
                             "-fno-exceptions",
                             "-Qunused-arguments" })
    {
        INFO("clang-cl flag " << flag);
        CHECK(IsAcceptableJobArgument(flag, DriverOf(Flavor::ClangCl)));
    }
}

TEST_CASE("A compiler this worker cannot classify refuses the JOB, not its arguments", "[compile-job]")
{
    // Two things go wrong if the fail-safe only gates arguments. The refusal surfaces
    // as `RejectedArgument`, so an operator watching that counter climb concludes the
    // fleet's FLAGS are wrong when the answer is this node's `--toolchain` -- and a
    // refusal's wire code and its counter are meant to be one fact. Worse, a job with
    // an EMPTY argument list has nothing to refuse, so it would sail past the argument
    // loop and spawn a driver whose command-line dialect this worker does not know.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "mystery", "/opt/weird/xlc" } }, ToolchainSurvey::Completed() };

    auto job = Job({});
    job.fingerprint = "mystery";
    auto const empty = jobs.Run(job);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == JobRefusal::SpawnFailed);
    CHECK(runner.Argv().empty()); // and nothing was spawned

    // The same answer with arguments present, rather than the argument being blamed.
    job.args = { "-O2" };
    auto const withArgs = jobs.Run(job);
    REQUIRE_FALSE(withArgs.has_value());
    CHECK(withArgs.error() == JobRefusal::SpawnFailed);
}

TEST_CASE("A refusal names the offending argument without echoing arbitrary bytes", "[compile-job]")
{
    // `detail` is encoded into the reply message and lands in the client's fallback
    // log, so client-supplied bytes are capped and reduced to printable ASCII where
    // the refusal is built -- once, rather than at each future producer.
    auto const control = JobError::RejectedArgumentNaming(std::string { "-W" } + '\x1b' + "[31mred" + '\n');
    CHECK(control.reason == JobRefusal::RejectedArgument);
    CHECK(control.detail.contains("-W?[31mred?"));
    // No control byte survives, so no terminal escape reaches a log.
    CHECK(std::ranges::none_of(control.detail, [](char c) { return c >= 0 && c < 0x20; }));

    // A long argument is truncated rather than reflected whole.
    auto const huge = JobError::RejectedArgumentNaming(std::string(4096, 'x'));
    CHECK(huge.detail.size() < 200);
    CHECK(huge.detail.contains("..."));

    // A non-ASCII byte becomes `?`, so the message is valid UTF-8 whatever arrived --
    // which the fleet requires of text a peer sent.
    auto const invalid = JobError::RejectedArgumentNaming("-W\xff\xfe");
    CHECK(invalid.detail.contains("-W??"));
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
        ScratchDirectory scratch { "fc-jobtest" };
        CompileJobRunner jobs {
            runner, scratch.Path(), { { "msvc", R"(C:\MSVC\bin\cl.exe)" } }, ToolchainSurvey::Completed()
        };

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
        ScratchDirectory scratch { "fc-jobtest" };
        CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

        (void) jobs.Run(Job({}));

        auto const& argv = runner.Argv();
        REQUIRE(!argv.empty());
        CHECK(std::ranges::any_of(argv, [](std::string const& a) { return a.starts_with("-o"); }));
        CHECK(std::ranges::none_of(argv, [](std::string const& a) { return a.starts_with("/Fo"); }));
    }
}

TEST_CASE("Concurrent jobs each get their OWN object rather than a sibling's", "[compile-job]")
{
    // The reason this exists: since #213 a worker runs `slots` compiles at once
    // through ONE CompileJobRunner, so the per-job scratch directory has to be
    // unique across threads and not merely across calls. It was derived from a plain
    // `++`, and every path below it -- the source, and the hard-coded `tu.o` -- is
    // inside it. Two jobs landing on the same number therefore compiled into the
    // same file and one returned the OTHER's object, which its client then cached
    // under its own key. A worker that answers with the wrong object is worse than
    // one that answers with nothing.
    //
    // Asserting on the returned BYTES is the point. The counters agree in both
    // worlds; only the payload tells them apart.
    //
    // A few rounds rather than one, but no illusions about what that buys: measured
    // against the defect, one round caught it in 50 runs out of 300 and thirty-two
    // rounds caught it in exactly the same proportion. A lost update is a race, and
    // a black-box test cannot make one certain.
    //
    // What this case IS certain about is the shape: any regression that gives two
    // concurrent jobs the same path -- a shared directory, a fixed object name --
    // fails it every time, because the barrier guarantees the overlap. The race on
    // the counter itself is caught deterministically by ThreadSanitizer, which is
    // why `Run` being callable from several threads is written down in its docs.
    constexpr std::size_t Rounds = 4;

    ScratchDirectory scratch { "fc-jobtest" };
    OverlappingEchoRunner runner;
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    // Same source NAME, different source TEXT -- exactly the case a shared scratch
    // directory collapses, and exactly what a real fleet does when two machines
    // compile their own `main.cpp` on the same worker.
    std::array<std::string, OverlappingEchoRunner::Overlap> const texts { "int a = 1;", "long b = 2;" };

    // Rendered back to text so a failure names the bytes rather than their length.
    auto const asText = [](std::vector<std::byte> const& bytes) {
        std::string out;
        out.reserve(bytes.size());
        for (auto const byte: bytes)
            out.push_back(static_cast<char>(byte));
        return out;
    };

    for (auto const round: std::views::iota(std::size_t { 0 }, Rounds))
    {
        INFO("round " << round);
        std::array<std::expected<CompileOutcome, JobError>, OverlappingEchoRunner::Overlap> results {};

        // Both threads enter Run() together rather than whenever they happen to be
        // scheduled. Without this the first job can finish before the second starts,
        // and the round reports a pass having never overlapped at all.
        std::latch start { static_cast<std::ptrdiff_t>(OverlappingEchoRunner::Overlap) };

        {
            std::vector<std::jthread> threads;
            for (auto const index: std::views::iota(std::size_t { 0 }, OverlappingEchoRunner::Overlap))
                threads.emplace_back([&, index] {
                    auto job = Job();
                    job.preprocessed = texts.at(index);
                    start.arrive_and_wait();
                    // No Catch2 macro on this thread: the assertion macros are not
                    // thread-safe, so the answer is carried back and checked below.
                    results.at(index) = jobs.Run(job);
                });
        } // joined

        for (auto const index: std::views::iota(std::size_t { 0 }, OverlappingEchoRunner::Overlap))
        {
            INFO("job " << index);
            REQUIRE(results.at(index).has_value());
            REQUIRE(asText(results.at(index)->object) == texts.at(index));
        }

        runner.NextRound();
    }
}

namespace
{
/// A runner that parks inside the compile until a case lets it out.
///
/// What makes the case below mean anything. The hazard is a job holding a reference
/// into the toolchain map across the compile, so a test that replaces the map before
/// or after `Run` proves nothing at all -- the replacement has to land while a job is
/// provably inside, which is what the two latches buy.
class ParkingRunner final: public IProcessRunner
{
  public:
    CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        // Recorded BEFORE parking, so the case can see which compiler was chosen
        // while it still has the chance to swap the map.
        _argv.assign(argv.begin(), argv.end());
        _inside.count_down();
        _release.wait();
        Test::WriteStubObject(_argv);
        return CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

    /// Block until a job is inside the compile. Bounded by the latch rather than by
    /// a sleep: this returns when the job arrives and never before.
    void WaitUntilInside()
    {
        _inside.wait();
    }

    /// Let the parked job finish.
    void Release()
    {
        _release.count_down();
    }

    [[nodiscard]] std::vector<std::string> const& Argv() const noexcept
    {
        return _argv;
    }

  private:
    std::latch _inside { 1 };
    std::latch _release { 1 };
    std::vector<std::string> _argv;
};
} // namespace

TEST_CASE("A worker serves the toolchains it was last given, not the ones it started with", "[compile-job]")
{
    // #238. A node fingerprints its machine once at startup and then lives for weeks,
    // so a compiler patched in place -- a distro upgrade, a Windows SDK update --
    // leaves it advertising the pre-upgrade digest while spawning the post-upgrade
    // compiler. Clients then get objects built by a compiler they did not key against
    // and store them in the shared cache under the old key, where the whole fleet
    // reads them.
    //
    // The node's remedy is to re-survey and hand the new set down. Both halves are
    // asserted here, and the first is the load-bearing one: the fingerprint this
    // worker can no longer honour must STOP being served. `UnknownFingerprint` is the
    // existing answer for that, so the client falls back to a local compile with no
    // new refusal, wire code or counter invented for the case.
    ScriptedRunner runner;
    ScratchDirectory const scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    REQUIRE(jobs.Run(Job()).has_value());
    CHECK(jobs.Fingerprints() == std::vector<std::string> { "gcc-13" });

    jobs.ReplaceToolchains({ { "gcc-14", "/opt/real/g++" } });

    // Replaced, never merged: a set that kept the old entry would leave the
    // wrong-object path open forever, which is the whole defect.
    CHECK(jobs.Fingerprints() == std::vector<std::string> { "gcc-14" });
    auto const stale = jobs.Run(Job());
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == JobRefusal::UnknownFingerprint);

    // And the new identity IS served, so a machine rejoins the fleet under the
    // toolchain it now has rather than dropping out of it.
    auto fresh = Job();
    fresh.fingerprint = "gcc-14";
    CHECK(jobs.Run(fresh).has_value());
}

TEST_CASE("A job already inside a compile keeps the compiler it looked up", "[compile-job]")
{
    // The hazard the change above would otherwise have introduced, and it is not a
    // theoretical one: `Run` used to hold the map's ITERATOR and dereference it twice
    // far downstream -- after the scratch directory was created and the whole
    // preprocessed source written -- on the two lines that decide which program
    // executes and which driver family names its output flag. A replacement landing
    // in that window left both reading freed memory.
    //
    // So the compiler is copied out under the lock at lookup, and the copy is what
    // the rest of the job uses. That also gives the honest answer: a job already
    // admitted finishes against the compiler its client was told it would get, rather
    // than half against one and half against another.
    ParkingRunner runner;
    ScratchDirectory const scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/before/g++" } }, ToolchainSurvey::Completed() };

    std::expected<CompileOutcome, JobError> outcome;
    std::thread worker { [&] { outcome = jobs.Run(Job()); } };

    // The replacement lands while the job is provably inside the compile, which is
    // the only arrangement that exercises the window at all. Written the obvious way
    // -- replace, then run -- this case would pass against the dangling iterator too.
    runner.WaitUntilInside();
    jobs.ReplaceToolchains({ { "gcc-13", "/opt/after/g++" } });
    runner.Release();
    worker.join();

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(runner.Argv().empty());
    CHECK(runner.Argv().front() == "/opt/before/g++");

    // And the NEXT job takes the new one, so what the case above proves is that the
    // replacement was survived rather than ignored.
    ScriptedRunner after;
    CompileJobRunner replaced { after, scratch.Path(), { { "gcc-13", "/opt/before/g++" } }, ToolchainSurvey::Completed() };
    replaced.ReplaceToolchains({ { "gcc-13", "/opt/after/g++" } });
    REQUIRE(replaced.Run(Job()).has_value());
    REQUIRE_FALSE(after.Argv().empty());
    CHECK(after.Argv().front() == "/opt/after/g++");
}

// --- the compilation-directory mapping (#506) --------------------------------

TEST_CASE("A worker asked for no mapping adds no rule", "[compile-job][prefix-map]")
{
    // The case the whole design turns on, and it is a SUCCESS rather than a refusal: a
    // build that maps nothing must get an object recording no directory it did not ask
    // for, so an absent pair is not an error and not an invitation for the worker to
    // pick a token of its own.
    auto const rules = WorkerPrefixMapRules("/var/lib/fastcache-node", "", "", DriverFamily::Gnu);
    REQUIRE(rules.has_value());
    CHECK(rules->empty());
}

TEST_CASE("An empty replacement maps a directory to nothing, and is legal", "[compile-job][prefix-map]")
{
    // `-fdebug-prefix-map=<builddir>=` is a standard reproducible-build spelling, and
    // `MatchPathValueFlag` reports it as a directory with an empty replacement -- so a
    // client using it sends exactly that. Refusing it as "half a pair" cost such a build
    // distribution entirely: every TU paid a scheduler grant and a worker round trip and
    // then compiled locally anyway.
    auto const rules = WorkerPrefixMapRules("/scratch", "/home/ci/build", "", DriverFamily::Gnu);
    REQUIRE(rules.has_value());
    CHECK(*rules == std::vector<std::string> { "-fdebug-prefix-map=/home/ci/build=", "-fdebug-prefix-map=/scratch=" });
}

TEST_CASE("A worker refuses a replacement with no directory to replace", "[compile-job][prefix-map]")
{
    // The one genuinely malformed half: a replacement with no left-hand side would map
    // everything. The DIRECTORY is what says whether a mapping is in force, so this is
    // not "maps nothing" and must not be read as it.
    auto const rules = WorkerPrefixMapRules("/scratch", "", ".", DriverFamily::Gnu);
    REQUIRE_FALSE(rules.has_value());
    CHECK(rules.error() == JobRefusal::RejectedArgument);
}

TEST_CASE("A worker drops its own rule when that rule would swallow the client's paths", "[compile-job][prefix-map]")
{
    // THE regression case for the shipped Linux deployment. A prefix-map rule appends
    // the unmatched tail, so a worker directory of `/` rewrites `/home/ci/build` to
    // `.home/ci/build` and every system header to `.usr/include/...`. And `/` is the
    // production value: `fastcache-compile-node.service` sets no `WorkingDirectory=`,
    // and `PosixDaemonHost` calls `chdir("/")` on the daemonize path.
    //
    // Measured on gcc 14.2.0 with both rules present and this one last:
    // `DW_AT_comp_dir` came back `.tmp/l506d/client`, and every system header read
    // `.usr/include/...`. That is a WRONG object under a correct key -- strictly worse
    // than the unmapped directory #506 is about.
    //
    // The client's rule still lands, which is what keeps the gcc case fully mapped.
    auto const atRoot = WorkerPrefixMapRules("/", "/home/ci/build", ".", DriverFamily::Gnu);
    REQUIRE(atRoot.has_value());
    CHECK(*atRoot == std::vector<std::string> { "-fdebug-prefix-map=/home/ci/build=." });

    // Not only the root: any worker directory the client's own lies under.
    auto const above = WorkerPrefixMapRules("/home/ci", "/home/ci/build", ".", DriverFamily::Gnu);
    REQUIRE(above.has_value());
    CHECK(*above == std::vector<std::string> { "-fdebug-prefix-map=/home/ci/build=." });

    // And a worker directory that merely RESEMBLES the client's still gets its rule:
    // the test is containment, because a byte prefix is what the driver applies.
    auto const beside = WorkerPrefixMapRules("/home/cix", "/home/ci/build", ".", DriverFamily::Gnu);
    REQUIRE(beside.has_value());
    CHECK(beside->size() == 2);
}

TEST_CASE("A worker refuses when its OWN directory cannot be spelled", "[compile-job][prefix-map]")
{
    // The value the client did not send, so it is the WORKER's fault -- blaming a client
    // for a property of the machine it was sent to sends an operator to the wrong end of
    // the fleet. Checked at all because the header's argument for the alphabet is that
    // these are spliced onto a command line, and this is the value that used to skip it.
    for (auto const& bad: { std::string { "/scratch dir" }, std::string { "/scratch\"quoted" }, std::string {} })
    {
        INFO("worker directory: " << bad);
        auto const rules = WorkerPrefixMapRules(bad, "/home/ci/build", ".", DriverFamily::Gnu);
        REQUIRE_FALSE(rules.has_value());
        CHECK(rules.error() == JobRefusal::SpawnFailed);
    }
}

TEST_CASE("A worker maps BOTH candidate directories to the client's replacement", "[compile-job][prefix-map]")
{
    // The fix, and the shape the end-to-end fixture corrected. WHICH directory a
    // dispatched object records is the driver's answer: gcc's `-fworking-directory` is
    // implicit under `-g` and puts the CLIENT's directory into the preprocessed text,
    // which the worker's compile then adopts, while clang emits no such marker and the
    // worker's own directory is what shows.
    //
    // Measured on gcc 14.2.0 and clang 20.1.2, reading `DW_AT_comp_dir` off the
    // worker's object: `g++ -E -g` records the CLIENT's directory, `g++ -E` and
    // `clang++ -E`/`-E -g` record the WORKER's. Mapping only the worker's own left gcc
    // recording the client's UNMAPPED path -- which the fleet fixture's object
    // comparison cannot see, and reading `comp_dir` can.
    auto const rules = WorkerPrefixMapRules("/var/lib/fastcache-node", "/home/ci/out/build/x", ".", DriverFamily::Gnu);
    REQUIRE(rules.has_value());
    CHECK(*rules
          == std::vector<std::string> { "-fdebug-prefix-map=/home/ci/out/build/x=.",
                                        "-fdebug-prefix-map=/var/lib/fastcache-node=." });
}

TEST_CASE("Every prefix-map row is spelled with its own separator, both times", "[compile-job][prefix-map]")
{
    // Each rule is assembled from the row's spelling and its `valueTailSeparator`, and
    // that one character stands for two different things: the join between the flag and
    // its value, and the split inside that value. They are the same character for every
    // row the table can hold today, and that is an assumption rather than a fact.
    for (PathValueFlag const& row: PathValueFlags())
    {
        if (row.role != PathValueRole::PrefixMap)
            continue;
        INFO("row: " << row.spelling);
        auto const rules = WorkerPrefixMapRules("/scratch/dir", "/client/dir", "TOKEN", row.families);
        REQUIRE(rules.has_value());
        for (auto const& rule: *rules)
        {
            INFO("rule: " << rule);

            // THE assertion of this case. The round trip below does not pin the join
            // and the first version of this case was exactly that: `StripJoinSeparator`
            // accepts every character in `JoinSeparators` by design, so a rule emitted
            // as `-fdebug-prefix-map:/scratch/dir=TOKEN` parses back with the same head
            // and the same tail -- measured, by spelling the join `:` in the production
            // code and watching all five assertions still pass.
            CHECK(rule.starts_with(std::string { row.spelling } + row.valueTailSeparator));

            // Kept beside it because it pins a different thing -- that the replacement
            // lands after the directory rather than before it -- but it is NOT what
            // catches a wrong join. Neither says what a real driver accepts; nothing in
            // this tree can.
            auto const parsed = MatchPathValueFlag(rule, IntroducersOf(row.families), row.families);
            REQUIRE(parsed.has_value());
            CHECK(Unwrap(parsed).flag.role == PathValueRole::PrefixMap);
            CHECK(Unwrap(parsed).valueTail == std::string { row.valueTailSeparator } + "TOKEN");
        }
    }
}

TEST_CASE("A worker refuses a value carrying the ROW's own separator", "[compile-job][prefix-map]")
{
    // Asked of the row rather than left to the alphabet, so the guard cannot fail OPEN
    // when the table grows. `CmdLine.hpp` names the row it expects next --
    // `-fprofile-prefix-map`, a `<path>:<something>` spelling -- and `:` is IN the
    // alphabet, because a Windows absolute path begins `C:\`. An alphabet-only guard
    // would admit a value containing that new separator and emit a rule the driver
    // splits in the wrong place.
    //
    // Driven off the table for the same reason the production code is: this stays
    // meaningful when a second row arrives.
    for (PathValueFlag const& row: PathValueFlags())
    {
        if (row.role != PathValueRole::PrefixMap)
            continue;
        INFO("row: " << row.spelling);
        auto const carrying = std::string { "/home/ci/a" } + row.valueTailSeparator + "b";

        // A client's value is the CLIENT's fault...
        auto const client = WorkerPrefixMapRules("/scratch", carrying, ".", row.families);
        REQUIRE_FALSE(client.has_value());
        CHECK(client.error() == JobRefusal::RejectedArgument);

        // ...and this worker's own directory is the WORKER's. The attribution is the
        // point: blaming a client for a property of the machine it was sent to sends an
        // operator to the wrong end of the fleet.
        auto const worker = WorkerPrefixMapRules(carrying, "/client/dir", ".", row.families);
        REQUIRE_FALSE(worker.has_value());
        CHECK(worker.error() == JobRefusal::SpawnFailed);
    }
}

TEST_CASE("A worker refuses a value it will not put on a command line", "[compile-job][prefix-map]")
{
    // Peer text that ends up inside an artefact, so both halves are bounded and
    // restricted before they are spelled. Refused rather than dropped: dropping it
    // silently returns an object whose compilation directory disagrees with a locally
    // built one under the same key, which is the defect this closes.
    for (auto const& bad: { std::string { "a b" }, std::string { "a\"b" }, std::string { "a\nb" } })
    {
        INFO("value: " << bad);
        CHECK(WorkerPrefixMapRules("/scratch", "/client/dir", bad, DriverFamily::Gnu).error()
              == JobRefusal::RejectedArgument);
        CHECK(WorkerPrefixMapRules("/scratch", bad, ".", DriverFamily::Gnu).error() == JobRefusal::RejectedArgument);
    }

    // ONE ceiling, and this case used to assert two. It said a replacement is a
    // relative path and a directory an absolute one, so 300 bytes must be refused as
    // one and accepted as the other -- correct about a rule's `<to>`, and about a value
    // that never reaches this function. What arrives is `<to>` plus the client's
    // directory tail (see the note on the bound), so its length tracks the DIRECTORY,
    // and a 300-byte one is exactly as ordinary on this side as on the other. The case
    // above builds such a value through the real producer and requires it accepted;
    // this one keeps the ceiling itself honest.
    //
    // Both halves at 300 -- accepted, because neither is remarkable.
    CHECK(WorkerPrefixMapRules("/scratch", "/client/dir", std::string(300, '.'), DriverFamily::Gnu).has_value());
    CHECK(WorkerPrefixMapRules("/scratch", std::string(300, '.'), ".", DriverFamily::Gnu).has_value());

    // And both halves past it -- refused, so removing the ceiling is still caught.
    CHECK(WorkerPrefixMapRules("/scratch", std::string(5000, '.'), ".", DriverFamily::Gnu).error()
          == JobRefusal::RejectedArgument);
    CHECK(WorkerPrefixMapRules("/scratch", "/client/dir", std::string(5000, '.'), DriverFamily::Gnu).error()
          == JobRefusal::RejectedArgument);
}

TEST_CASE("A shallow rule over a deep build directory still distributes", "[compile-job][prefix-map]")
{
    // The producer and the consumer, in one case, because the defect lived exactly
    // between them and neither half shows it alone.
    //
    // `MappedCompileDirectory` does not send the rule's `<to>`. It sends `<to>` PLUS the
    // working directory's tail past the matched prefix, so the value's length tracks the
    // DIRECTORY, not the rule. Bounding it as though it were a rule's `<to>` refused an
    // ordinary build: mapping at a shallow prefix is standard reproducible-build
    // practice, and a deep-enough build directory under it then produced a replacement
    // over the old 256-byte ceiling. Every COMPILE for that build was refused, the
    // client fell back locally, and the build stopped distributing entirely -- while the
    // refusal blamed a client ARGUMENT for a value the launcher had derived.
    //
    // A hand-made string cannot catch that. This one is built the way the wire builds it.
    auto const deep = std::string { "/home/ci/" } + std::string(270, 'd') + "/build";
    std::vector<std::string> const argv { "g++", "-c", "a.cpp", "-g", "-fdebug-prefix-map=/home/ci=." };

    auto const mapped = MappedCompileDirectory(argv, DriverFamily::Gnu, deep);
    REQUIRE(mapped.has_value());
    auto const& pair = FastCache::Testing::Unwrap(mapped);

    // The value that actually travels is longer than the old ceiling, which is the
    // whole point -- if this ever stops holding the case has stopped testing anything.
    REQUIRE(pair.replacement.size() > 256);
    CHECK(pair.directory == deep);

    // And the worker must accept it. The directory is ordinary and so is the rule.
    CHECK(WorkerPrefixMapRules("/scratch", pair.directory, pair.replacement, DriverFamily::Gnu).has_value());
}

TEST_CASE("A worker accepts a non-ASCII replacement", "[compile-job][prefix-map]")
{
    // Deliberately wider than `SafeSourceName`, and the difference is what each string
    // becomes: a source name becomes a FILE on this worker, neither of these ever does.
    // A build directory with a non-ASCII component is ordinary, and refusing one would
    // cost that build distribution entirely.
    CHECK(WorkerPrefixMapRules("/scratch", "/client/b\xc3\xa4u", "./b\xc3\xa4u", DriverFamily::Gnu).has_value());
}

TEST_CASE("A worker accepts a Windows drive letter in either half", "[compile-job][prefix-map]")
{
    // A GNU-layout driver on Windows -- mingw, or plain clang -- is an ordinary client,
    // and its absolute paths begin `C:\`. Leaving `:` out of the safe set refused every
    // such client before the driver family was even consulted, so the refusal named the
    // wrong end of the fleet. Caught by the case below it, which expected a WORKER
    // refusal and got a client one.
    auto const rules = WorkerPrefixMapRules(R"(C:\scratch)", R"(C:\ci\build)", ".", DriverFamily::Gnu);
    REQUIRE(rules.has_value());
    CHECK(rules->front() == R"(-fdebug-prefix-map=C:\ci\build=.)");
}

TEST_CASE("A worker whose driver has no such flag refuses rather than pretending", "[compile-job][prefix-map]")
{
    // `cl` has no path-map switch and clang-cl's CodeView records are not remapped by
    // one, which is why the table's row is GNU-only. Emitting nothing and compiling
    // anyway would return an object recording a directory a locally mapped one does not,
    // under the same key.
    auto const rules = WorkerPrefixMapRules("C:/scratch", "C:/client", ".", DriverFamily::Msvc);
    REQUIRE_FALSE(rules.has_value());
    CHECK(rules.error() == JobRefusal::SpawnFailed);
}

TEST_CASE("A run appends both mappings after the client's own arguments", "[compile-job][prefix-map]")
{
    // AFTER the client's arguments, because both GNU drivers honour the LAST matching
    // rule. That ordering is defensive rather than reachable today, and saying so is
    // the point: a client's own `-fdebug-prefix-map` is dropped twice over -- by
    // `RemoteCompileArgs` as a path-valued flag, and again by this worker's allowlist,
    // which refuses any argument body carrying a path separator. So no client argument
    // can currently contend with these rules, and a case staging one would be asserting
    // against a command line no worker can receive.
    //
    // One of the two rules names THIS process's directory, which is what the compiler
    // inherits: `IProcessRunner` spawns with no directory of its own and every path the
    // runner passes is absolute. Asking `current_path()` here means the test predicts
    // the subject from the environment, which a construction-time seam on
    // `CompileJobRunner` would remove — and which would also make the "this worker's own
    // directory cannot be spelled" refusal reachable through `Run` rather than only
    // through the free function. That seam is deliberately not taken here: the only
    // production construction is the compile node's `main.cpp`, which is another lane.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    auto job = Job({ "-O2", "-DNDEBUG" });
    job.compileDir = "/home/ci/out/build/x";
    job.compileDirReplacement = ".";
    REQUIRE(jobs.Run(job).has_value());

    auto const own = std::format("-fdebug-prefix-map={}=.", std::filesystem::current_path().string());
    auto const& argv = runner.Argv();
    auto const atOwn = std::ranges::find(argv, own);
    auto const atClient = std::ranges::find(argv, "-fdebug-prefix-map=/home/ci/out/build/x=.");
    REQUIRE(atOwn != argv.end());
    REQUIRE(atClient != argv.end());
    CHECK(atClient > std::ranges::find(argv, "-DNDEBUG"));
    CHECK(atOwn < std::ranges::find(argv, "-c"));
}

TEST_CASE("A run asked for no mapping gets no DIRECTORY rule on the line", "[compile-job][prefix-map]")
{
    // The subject is the compilation-directory pair: a build that asked for no mapping
    // must not have one invented for it, or a worker hands a client that requested
    // nothing an object recording a directory neither machine has -- #506 pointing the
    // other way.
    //
    // **The source-name rule is a different question and is expected here** (#660). It
    // repairs a name this worker's own scratch layout put into the object rather than
    // honouring anything the client asked for, so it is emitted for a build that maps
    // nothing at all -- which is exactly the build whose dispatched object was recording
    // `job-N`. Told apart by its `<from>`, which is a path under this worker's scratch;
    // a directory rule's is the client's.
    //
    // The spelling comes off the table rather than a literal, so this stays true of a
    // second prefix-map row rather than passing because it is looking for the wrong flag.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    REQUIRE(jobs.Run(Job()).has_value());
    auto const root = std::filesystem::weakly_canonical(scratch.Path()).generic_string();
    for (PathValueFlag const& row: PathValueFlags())
    {
        if (row.role != PathValueRole::PrefixMap)
            continue;
        INFO("row: " << row.spelling);
        for (auto const& arg: runner.Argv())
        {
            if (!arg.starts_with(row.spelling))
                continue;
            INFO("argv entry: " << arg);
            auto const body = std::string_view { arg }.substr(std::string_view { row.spelling }.size() + 1);
            auto const from = body.substr(0, body.find(row.valueTailSeparator));
            CHECK(std::filesystem::weakly_canonical(from).generic_string().starts_with(root));
        }
    }
}

TEST_CASE("A run refuses a value the worker will not spell, and spawns nothing", "[compile-job][prefix-map]")
{
    // Counted as a rejected argument, which is what it is -- a value this worker will
    // not put on a command line -- so the existing refusal counter moves and the client
    // compiles locally rather than caching an object with a wrong directory.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } }, ToolchainSurvey::Completed() };

    auto job = Job();
    job.compileDir = "/home/ci/a=b";
    job.compileDirReplacement = ".";
    auto const result = jobs.Run(job);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::RejectedArgument);
    CHECK(runner.Argv().empty());
}
