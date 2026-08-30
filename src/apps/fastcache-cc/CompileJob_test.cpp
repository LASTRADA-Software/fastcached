// SPDX-License-Identifier: Apache-2.0
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
#include <latch>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Cc;
using FastCache::Testing::ScratchDirectory;

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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), {} };
    CHECK(jobs.Run(Job()).error() == JobRefusal::UnknownFingerprint);
    CHECK(jobs.Fingerprints().empty());
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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

    auto job = Job();
    job.sourceName = "../../../etc/passwd.cpp";
    REQUIRE(jobs.Run(job).has_value());

    // The LEAF may be what the client asked for -- that is the feature -- and the
    // directory may not. So the assertion is containment, not absence: every path
    // on the command line lies under this worker's own scratch root, and no
    // parent-directory segment survived to take one back out of it.
    auto const root = std::filesystem::weakly_canonical(scratch.Path()).generic_string();
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
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

    auto job = Job();
    job.sourceName = "Widget.cpp";
    REQUIRE(jobs.Run(job).has_value());

    CHECK(std::ranges::any_of(runner.Argv(), [](std::string const& a) { return a.ends_with("Widget.cpp"); }));
}

TEST_CASE("A successful compile returns the object the compiler wrote", "[compile-job]")
{
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

    auto const result = jobs.Run(Job());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == JobRefusal::ScratchUnavailable);
}

TEST_CASE("Each job gets its own scratch directory, and it is removed", "[compile-job]")
{
    // A worker leaking a directory per job fills its disk during a long build.
    ScriptedRunner runner;
    ScratchDirectory scratch { "fc-jobtest" };
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "mystery", "/opt/weird/xlc" } } };

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
        CompileJobRunner jobs { runner, scratch.Path(), { { "msvc", R"(C:\MSVC\bin\cl.exe)" } } };

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
        CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };

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
    CompileJobRunner jobs { runner, scratch.Path(), { { "gcc-13", "/opt/before/g++" } } };

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
    CompileJobRunner replaced { after, scratch.Path(), { { "gcc-13", "/opt/before/g++" } } };
    replaced.ReplaceToolchains({ { "gcc-13", "/opt/after/g++" } });
    REQUIRE(replaced.Run(Job()).has_value());
    REQUIRE_FALSE(after.Argv().empty());
    CHECK(after.Argv().front() == "/opt/after/g++");
}

TEST_CASE("Two node PROCESSES on one host do not share a scratch directory", "[compile-job][scratch-root]")
{
    // #279, and the cross-PROCESS half of what a22e056 closed in-process. A worker
    // derives its scratch root from `temp_directory_path() / "fastcache-compile-node"`
    // (`fastcache-compile-node/main.cpp`), which is one path per user per machine, and
    // its job counter starts at 1 in every process. So a second node on the same host
    // -- a service plus a hand-started debug run, two instances an operator started,
    // a container sharing a mount -- derives the IDENTICAL `job-1`, and every path
    // below it: the source, and the hard-coded `tu.o`.
    //
    // Two runners over one root is exactly that, and it is a faithful model rather
    // than an approximation: everything else `CompileJobRunner` reads is fixed at
    // construction, so a second process differs from a second runner in nothing this
    // defect touches. `_nextJob` is per instance and starts at 1 in both.
    //
    // Unlike the in-process case this is NOT a race and does not need rounds to
    // catch: there is no lost update to lose. Both runners take `job-1` with
    // certainty, so the collision is structural and this fails every run.
    ScratchDirectory scratch { "fc-jobtest-crossproc" };
    OverlappingEchoRunner runner;

    // Two runners, one root -- two processes, one machine.
    CompileJobRunner first { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };
    CompileJobRunner second { runner, scratch.Path(), { { "gcc-13", "/opt/real/g++" } } };
    std::array<CompileJobRunner*, OverlappingEchoRunner::Overlap> const runners { &first, &second };

    // Same source NAME, different source TEXT: what two machines compiling their own
    // `main.cpp` against one host look like.
    std::array<std::string, OverlappingEchoRunner::Overlap> const texts { "int a = 1;", "long b = 2;" };

    auto const asText = [](std::vector<std::byte> const& bytes) {
        std::string out;
        out.reserve(bytes.size());
        for (auto const byte: bytes)
            out.push_back(static_cast<char>(byte));
        return out;
    };

    std::array<std::expected<CompileOutcome, JobError>, OverlappingEchoRunner::Overlap> results {};
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
                results.at(index) = runners.at(index)->Run(job);
            });
    } // joined

    for (auto const index: std::views::iota(std::size_t { 0 }, OverlappingEchoRunner::Overlap))
    {
        INFO("process " << index);
        // Either failure mode is this defect: one runner answering with the other's
        // object, or one runner's ScratchGuard deleting the directory under the
        // other and the job being blamed on the disk as ScratchUnavailable.
        REQUIRE(results.at(index).has_value());
        REQUIRE(asText(results.at(index)->object) == texts.at(index));
    }
}
