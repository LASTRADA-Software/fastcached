// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/EnvironmentTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <ToolchainHostTestUtils.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Cc::Testing::ScriptedToolchainHost;
using FastCache::Testing::Unwrap;

namespace
{
/// The variable `StateDirectory()` resolves the fingerprint cache from.
///
/// The same `#if` `StateDirectory` itself carries: these are genuinely different
/// variables per platform rather than one spelled two ways.
#if defined(_WIN32)
constexpr char const* StateVariable = "LOCALAPPDATA";
#else
constexpr char const* StateVariable = "XDG_STATE_HOME";
#endif

/// A private fingerprint cache, for the duration of one case.
///
/// `ResolveToolchains` fingerprints through `CachedToolchainFingerprint`, which
/// READS AND WRITES a cache under the user's state directory. Without this, these
/// cases scribble in the developer's real one -- and worse, take a different path
/// depending on whether it already holds an entry for `/usr/bin/gcc`, which is a
/// test whose result depends on what the machine did yesterday.
class ScopedStateDir
{
  public:
    ScopedStateDir():
        _dir { "fc-node-tc" },
        _env { StateVariable, _dir.Path().string() }
    {
    }

  private:
    FastCache::Testing::ScratchDirectory _dir;
    FastCache::Testing::ScopedEnv _env;
};

/// A discovery that reports whatever a case wrote down.
///
/// The reason `IToolchainDiscovery` is an interface rather than a call to the
/// layout walk: presenting a fleet's worth of toolchains here would otherwise mean
/// scripting a whole filesystem for each of them.
class FixedDiscovery final: public Cc::IToolchainDiscovery
{
  public:
    /// @param candidates What `Discover` should report.
    explicit FixedDiscovery(std::vector<Cc::ToolchainCandidate> candidates):
        _candidates { std::move(candidates) }
    {
    }

    std::vector<Cc::ToolchainCandidate> Discover() override
    {
        ++_calls;
        return _candidates;
    }

    /// @return How many times discovery was asked.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::vector<Cc::ToolchainCandidate> _candidates;
    int _calls { 0 };
};

/// A runner that reports whether each compiler could be spawned, and answers
/// `--version` for the ones that can.
class SpawnScript final: public Cc::IProcessRunner
{
  public:
    /// Refuse to spawn @p compiler, the way a stub or a foreign binary does.
    /// @param compiler The path that cannot be launched.
    /// @return This runner, for chaining.
    SpawnScript& Unspawnable(std::string compiler)
    {
        _unspawnable.push_back(std::move(compiler));
        return *this;
    }

    /// Let @p compiler run but say nothing, and fail while doing it.
    ///
    /// This is what drives `CompilerBanner` onto its FALLBACK, and that fallback is
    /// half of what makes an identity degenerate. A stub, a wrapper script, or a
    /// driver whose version probe this build does not know -- no longer `cl`, which
    /// is asked bare and answers (issue #195).
    ///
    /// @param compiler The path with no version banner.
    /// @return This runner, for chaining.
    SpawnScript& Speechless(std::string compiler)
    {
        _speechless.push_back(std::move(compiler));
        return *this;
    }

    /// Make @p compiler report @p banner rather than one naming itself.
    ///
    /// Two compilers CAN report the same version line: `clang` and `clang++` do,
    /// because clang's banner does not echo its own argv[0] the way a GNU driver's
    /// does. Verified: both print `Ubuntu clang version 20.1.2 (...)`, while `gcc`,
    /// `g++`, `cc` and `c++` each name themselves.
    ///
    /// @param compiler The path.
    /// @param banner What it should say.
    /// @return This runner, for chaining.
    SpawnScript& Banner(std::string compiler, std::string banner)
    {
        _banners.emplace_back(std::move(compiler), std::move(banner));
        return *this;
    }

    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        if (!argv.empty() && std::ranges::contains(_unspawnable, argv.front()))
            // -1 is the ONE answer meaning "could not spawn". A compiler that ran
            // and then failed has run, and takes the other branch.
            return Cc::CompileRun { .exitCode = -1, .out = {}, .err = {} };

        // The banner names the compiler, so two distinct compilers get two distinct
        // identities. A constant banner would make every fake compiler fingerprint
        // identically and collapse in the result map -- which is correct behaviour
        // for genuinely identical toolchains and would quietly hide whether a case
        // resolved one entry or two.
        auto const named = argv.empty() ? std::string {} : argv.front();
        if (std::ranges::contains(_speechless, named))
            return Cc::CompileRun { .exitCode = 2, .out = {}, .err = {} };
        for (auto const& [compiler, banner]: _banners)
            if (compiler == named)
                return Cc::CompileRun { .exitCode = 0, .out = banner + "\n", .err = {} };
        return Cc::CompileRun { .exitCode = 0, .out = "fake 1.0 (" + named + ")\n", .err = {} };
    }

    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        return RunCaptureCombined(argv);
    }

  private:
    std::vector<std::string> _unspawnable;
    std::vector<std::string> _speechless;
    std::vector<std::pair<std::string, std::string>> _banners;
};

/// A `cl.exe` in the layout every Visual Studio since 2017 installs.
constexpr std::string_view MsvcCompiler =
    "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe";

/// Give @p host the VC layout that compiler sits in.
///
/// Only the directories `MsvcToolsetIncludeRoots` emits, because that is the whole
/// question here: whether ANY include root was found, never what is inside one.
///
/// @param host The scripted machine, configured in place (`IToolchainHost` is
///             neither copyable nor movable).
void DescribeMsvcLayout(ScriptedToolchainHost& host)
{
    constexpr std::string_view vs = "C:/Program Files/Microsoft Visual Studio/18/Community";
    constexpr std::string_view toolset = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231";

    host.AddExecutable(std::string { MsvcCompiler });
    host.AddDirectory(std::string { toolset } + "/include");
    host.AddDirectory(std::string { toolset } + "/atlmfc/include");
    host.AddDirectory(std::string { vs } + "/VC/Auxiliary/VS/include");
}

/// @param compiler Where it lives.
/// @param layout Which row found it.
/// @return A candidate a discovery can report.
[[nodiscard]] Cc::ToolchainCandidate Candidate(std::string compiler, std::string layout = "usr")
{
    return Cc::ToolchainCandidate { .compiler = std::move(compiler), .layout = std::move(layout) };
}

/// @return A config a worker could start from, with no toolchain named.
[[nodiscard]] NodeConfig Startable()
{
    NodeConfig cfg;
    cfg.scheduler = "cache.internal:6675";
    cfg.advertise = "worker-01.internal:6676";
    return cfg;
}

/// Whether any captured line contains @p text.
/// @param logger The capturing logger.
/// @param text What to look for.
/// @return True when some line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view text)
{
    auto const records = logger.Snapshot();
    return std::ranges::any_of(records,
                               [&](CapturingLogger::Record const& record) { return record.message.contains(text); });
}
} // namespace

TEST_CASE("NodeToolchains: a discovered compiler is not re-parsed as an override", "[node][toolchains]")
{
    // `SplitToolchain` reserves the first `=`, which is right for something an
    // operator typed and wrong for a path this process found itself. Round-tripping
    // a discovered path through it registers the fingerprint `/opt/gcc` for the
    // compiler `13/bin/gcc` -- a worker serving an identity nothing will ever match,
    // with nothing anywhere saying so.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/opt/gcc=13/bin/gcc") } };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->second.compiler == "/opt/gcc=13/bin/gcc");
    CHECK(Unwrap(resolved).begin()->first != "/opt/gcc");
}

TEST_CASE("NodeToolchains: a discovered path that would abort the operator parser is served", "[node][toolchains]")
{
    // A leading or trailing `=` is a malformed *flag value* and a perfectly ordinary
    // *path*. Sent through the operator grammar it aborts startup with "malformed
    // --toolchain", naming a flag nobody passed.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/opt/=weird/bin/gcc"), Candidate("/opt/trailing=") } };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).size() == 2);
    CHECK_FALSE(Logged(logger, "malformed"));
}

TEST_CASE("NodeToolchains: a discovered compiler that cannot be spawned is dropped", "[node][toolchains]")
{
    // A stub shipped by a package whose payload is missing, a binary for another
    // architecture, a broken symlink. Registering it hands the scheduler a worker
    // that fails everything sent to it -- the `SpawnFailed` refusal a client
    // currently meets at job time, moved to where an operator can see it.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc"), Candidate("/usr/bin/broken-cc", "usr-local") } };
    SpawnScript runner;
    runner.Unspawnable("/usr/bin/broken-cc");
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->second.compiler == "/usr/bin/gcc");

    // Named, with the layout that found it: an operator surprised by a missing
    // toolchain needs to know it was found and rejected, not merely absent.
    CHECK(Logged(logger, "/usr/bin/broken-cc"));
    CHECK(Logged(logger, "usr-local"));
}

TEST_CASE("NodeToolchains: an operator-named compiler is never spawn-probed", "[node][toolchains]")
{
    // The `<fingerprint>=<compiler>` override exists precisely for a compiler this
    // process cannot execute -- a cross-compiler, or a wrapper that must not run at
    // configuration time. Probing it would refuse the one case the override is for.
    auto cfg = Startable();
    cfg.toolchains = { "deadbeef=/opt/cross/bin/aarch64-none-elf-gcc" };
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc") } };
    SpawnScript runner;
    runner.Unspawnable("/opt/cross/bin/aarch64-none-elf-gcc");
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->first == "deadbeef");
    CHECK(Unwrap(resolved).begin()->second.compiler == "/opt/cross/bin/aarch64-none-elf-gcc");
}

TEST_CASE("NodeToolchains: the operator's list wins whole", "[node][toolchains]")
{
    // Never merged with what the machine holds. A merged set would quietly re-add a
    // compiler an operator had deliberately narrowed away -- and the narrowing is
    // the entire reason a build farm names its toolchains.
    auto cfg = Startable();
    cfg.toolchains = { "cafe=/opt/curated/bin/g++" };
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc"), Candidate("/usr/bin/clang") } };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).size() == 1);

    // And discovery is not even asked: it costs process spawns and directory walks.
    CHECK(discovery.Calls() == 0);
}

TEST_CASE("NodeToolchains: discovery turned off with nothing named is refused", "[node][toolchains]")
{
    // The one shape that provably cannot work, and the reason
    // `--no-toolchain-discovery` is not a null flag.
    NodeConfig const cfg = Startable();
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, nullptr, runner, host, logger).has_value());
    CHECK(Logged(logger, "told to serve nothing"));

    // And it does NOT recite the search list: nothing was searched.
    CHECK_FALSE(Logged(logger, "Searched:"));
}

TEST_CASE("NodeToolchains: a machine with no compiler resolves to nothing", "[node][toolchains]")
{
    // Reported as an empty set rather than an error: nothing is malformed about a
    // machine with no compiler. Refusing to START on it is the caller's decision,
    // and `main` makes it -- because a worker that comes up serving nothing looks
    // healthy to a supervisor and to the fleet while every build compiles locally.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { {} };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, &discovery, runner, host, logger).has_value());

    // The search list is what an operator whose compiler was not found actually
    // needs -- the verdict alone says nothing about where to install one.
    CHECK(Logged(logger, "Searched:"));
    for (auto const& layout: Cc::ToolchainLayouts())
    {
        INFO("layout: " << layout.name);
        CHECK(Logged(logger, layout.name));
    }
}

TEST_CASE("NodeToolchains: every discovered compiler that cannot run leaves nothing", "[node][toolchains]")
{
    // The other route to an empty set, and the more alarming one: compilers WERE
    // found, so a reader of the first few log lines would think the worker is fine.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc"), Candidate("/usr/bin/g++") } };
    SpawnScript runner;
    runner.Unspawnable("/usr/bin/gcc").Unspawnable("/usr/bin/g++");
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, &discovery, runner, host, logger).has_value());
}

TEST_CASE("NodeToolchains: a malformed --toolchain is refused, naming the value", "[node][toolchains]")
{
    auto cfg = Startable();
    cfg.toolchains = { "=/usr/bin/gcc" };
    FixedDiscovery discovery { {} };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, &discovery, runner, host, logger).has_value());
    CHECK(Logged(logger, "malformed --toolchain"));
}

TEST_CASE("NodeToolchains: many toolchains are all identified, and reported in order", "[node][toolchains]")
{
    // Fingerprinting runs on a small pool, because a cold walk is seconds per
    // toolchain and a surveyed machine routinely holds several -- sequentially that
    // is a node sitting silent for half a minute before it reaches its scheduler.
    // Two things must survive the concurrency: every entry gets ITS OWN answer, and
    // the log does not depend on which thread finished first.
    NodeConfig const cfg = Startable();
    std::vector<Cc::ToolchainCandidate> candidates;
    for (auto const index: std::views::iota(0, 9))
        candidates.push_back(Candidate("/usr/bin/cc-" + std::to_string(index)));

    FixedDiscovery discovery { candidates };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());

    // Nine distinct compilers, nine distinct identities: a shared result slot or a
    // shared runner would show up here as a fingerprint attributed to the wrong
    // compiler, which is the failure that matches nothing and says nothing.
    CHECK(Unwrap(resolved).size() == candidates.size());
    for (auto const& candidate: candidates)
    {
        INFO("compiler: " << candidate.compiler);
        CHECK(std::ranges::any_of(Unwrap(resolved),
                                  [&](auto const& served) { return served.second.compiler == candidate.compiler; }));
    }

    // And the "serving" lines come out in table order however the work was
    // scheduled -- this log is where two machines' digests get compared.
    std::vector<std::string> served;
    for (auto const& record: logger.Snapshot())
        if (record.message.starts_with("serving "))
            served.push_back(record.message);
    REQUIRE(served.size() == candidates.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, candidates.size()))
        CHECK(served[index].contains(candidates[index].compiler));
}

TEST_CASE("NodeToolchains: the operator grammar splits on the first equals", "[node][toolchains]")
{
    auto const bare = SplitToolchain("/usr/bin/g++");
    REQUIRE(bare.has_value());
    CHECK(Unwrap(bare).fingerprint.empty());
    CHECK(Unwrap(bare).compiler == "/usr/bin/g++");

    auto const pinned = SplitToolchain("abc123=/usr/bin/g++");
    REQUIRE(pinned.has_value());
    CHECK(Unwrap(pinned).fingerprint == "abc123");
    CHECK(Unwrap(pinned).compiler == "/usr/bin/g++");

    CHECK_FALSE(SplitToolchain("").has_value());
    CHECK_FALSE(SplitToolchain("=/usr/bin/g++").has_value());
    CHECK_FALSE(SplitToolchain("abc123=").has_value());
}

TEST_CASE("NodeToolchains: the search list comes off the layout table", "[node][toolchains]")
{
    // What a refusal names, so an operator whose compiler was not found gets the
    // search list rather than the verdict. Derived rather than written out, so a
    // layout added to the table necessarily appears here.
    auto const searched = SearchedLayouts();
    CHECK_FALSE(searched.empty());
    for (auto const& layout: Cc::ToolchainLayouts())
    {
        INFO("layout: " << layout.name);
        CHECK(searched.contains(layout.name));
    }
}

TEST_CASE("NodeToolchains: an identity that names no compiler is refused, not registered", "[node][toolchains]")
{
    // Issue #140. A compiler that cannot be asked its version AND whose include tree
    // could not be located fingerprints as a digest over its own basename -- a value
    // this process could print with nothing installed at all, and one that EVERY
    // MSVC toolset on earth produces. Registering it is worse than registering
    // nothing: the worker comes up healthy, heartbeats happily, and is then matched
    // by clients on a completely different toolchain, or by none at all. Both
    // directions are silent from both ends, which is the failure the fingerprint
    // exists to prevent rather than to cause.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Cc::ToolchainCandidate { .compiler = std::string { MsvcCompiler },
                                                          .layout = "visual-studio" } } };
    SpawnScript runner;
    runner.Speechless(std::string { MsvcCompiler });

    // A service's view of the machine: the compiler is right there, and nothing says
    // where its headers are -- no layout, no `INCLUDE`.
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, &discovery, runner, host, logger).has_value());

    // Named, and pointed at the way out. An operator who genuinely wants this
    // toolchain served can pin an identity by hand, which is the one mechanism that
    // makes a compiler this process cannot interrogate usable at all.
    CHECK(Logged(logger, std::string { MsvcCompiler }));
    CHECK(Logged(logger, "--toolchain="));
}

TEST_CASE("NodeToolchains: an unaskable compiler with a locatable include tree is served", "[node][toolchains]")
{
    // The other side of the same rule, and what keeps it from being a regression.
    // A driver this build cannot interrogate is not thereby unusable: where the
    // include tree WAS located, that tree carries the identity, and refusing on the
    // banner alone would drop a toolchain the fleet can serve perfectly well.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Cc::ToolchainCandidate { .compiler = std::string { MsvcCompiler },
                                                          .layout = "visual-studio" } } };
    SpawnScript runner;
    runner.Speechless(std::string { MsvcCompiler });
    ScriptedToolchainHost host;
    DescribeMsvcLayout(host);
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->second.compiler == MsvcCompiler);
}

TEST_CASE("NodeToolchains: an operator's pinned identity is never second-guessed", "[node][toolchains]")
{
    // The check asks how a fingerprint was COMPUTED, so it has no business judging
    // one that was not. `<fingerprint>=<compiler>` is the documented escape hatch
    // for precisely the compiler this process cannot interrogate -- refusing it here
    // would close the one door deliberately left open.
    auto cfg = Startable();
    cfg.toolchains = { "deadbeef=" + std::string { MsvcCompiler } };
    SpawnScript runner;
    runner.Speechless(std::string { MsvcCompiler });
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, nullptr, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->first == "deadbeef");
}

TEST_CASE("NodeToolchains: a driver that answers is kept whatever its roots", "[node][toolchains]")
{
    // A real banner IS an identity: two GCC installs of different versions differ in
    // it, which is the property `ToolchainProbe` leans on when a walk finds nothing.
    // Only the fallback carries no information, so only the fallback is checked --
    // and re-deriving roots for a toolchain that answered would spawn the verbose
    // probe a second time for nothing.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc") } };
    SpawnScript runner;
    ScriptedToolchainHost host;
    ScopedStateDir const state;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).size() == 1);
    CHECK_FALSE(Logged(logger, "refusing"));
}

TEST_CASE("NodeToolchains: two names for one toolchain are served once, and said so", "[node][toolchains]")
{
    // `clang` and `clang++` reach the SAME fingerprint. Clang's banner does not
    // echo its own argv[0] the way a GNU driver's does -- both print
    // `Ubuntu clang version 20.1.2 (...)` -- and both classify as `Flavor::Clang`,
    // whose include probe forces `-x c++`, so the roots match too. The map keeps
    // the first, which is right: the worker really does serve one toolchain.
    //
    // What was wrong was the LOG. It said "serving" for each in turn, naming a
    // binding the worker does not have, in the one place an operator looks to find
    // out why a fingerprint is not matching.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/usr/bin/clang"), Candidate("/usr/bin/clang++") } };
    SpawnScript runner;
    runner.Banner("/usr/bin/clang", "Ubuntu clang version 20.1.2").Banner("/usr/bin/clang++", "Ubuntu clang version 20.1.2");
    ScopedStateDir const state;
    ScriptedToolchainHost host;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);

    // One "serving" line, not two, and the second is reported as what it is.
    auto const records = logger.Snapshot();
    auto const serving =
        std::ranges::count_if(records, [](CapturingLogger::Record const& r) { return r.message.starts_with("serving "); });
    CHECK(serving == 1);
    CHECK(Logged(logger, "is the same toolchain as"));

    // And the count an operator reads is the count of what is served.
    CHECK(Logged(logger, "discovered 1 toolchain(s)"));
}

TEST_CASE("NodeToolchains: two distinct compilers stay two", "[node][toolchains]")
{
    // The other side, and why the collision above is not simply deduplicated away
    // at discovery: `gcc` and `g++` DO name themselves in their banners, so they
    // are two toolchains a client can ask for separately, and a worker that offered
    // only one would silently never match the other.
    NodeConfig const cfg = Startable();
    FixedDiscovery discovery { { Candidate("/usr/bin/gcc"), Candidate("/usr/bin/g++") } };
    SpawnScript runner;
    ScopedStateDir const state;
    ScriptedToolchainHost host;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).size() == 2);
    CHECK_FALSE(Logged(logger, "is the same toolchain as"));
}
