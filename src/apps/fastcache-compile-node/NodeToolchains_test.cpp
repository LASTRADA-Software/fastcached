// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"

#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <ToolchainHostTestUtils.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Cc::Testing::ScriptedToolchainHost;
using FastCache::Testing::Unwrap;

namespace
{
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

    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        if (!argv.empty() && std::ranges::contains(_unspawnable, argv.front()))
            // -1 is the ONE answer meaning "could not spawn". A compiler that ran
            // and rejected `--version` -- which `cl` does -- has run.
            return Cc::CompileRun { .exitCode = -1, .out = {}, .err = {} };

        // The banner names the compiler, so two distinct compilers get two distinct
        // identities. A constant banner would make every fake compiler fingerprint
        // identically and collapse in the result map -- which is correct behaviour
        // for genuinely identical toolchains and would quietly hide whether a case
        // resolved one entry or two.
        auto const named = argv.empty() ? std::string {} : argv.front();
        return Cc::CompileRun { .exitCode = 0, .out = "fake 1.0 (" + named + ")\n", .err = {} };
    }

    Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        return RunCaptureCombined(argv);
    }

  private:
    std::vector<std::string> _unspawnable;
};

/// @param compiler Where it lives.
/// @param layout Which row found it.
/// @return A candidate a discovery can report.
[[nodiscard]] Cc::ToolchainCandidate Candidate(std::string compiler, std::string layout = "usr")
{
    return Cc::ToolchainCandidate { .compiler = std::move(compiler),
                                    .flavor = Cc::Flavor::Gcc,
                                    .layout = std::move(layout) };
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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->second == "/opt/gcc=13/bin/gcc");
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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->second == "/usr/bin/gcc");

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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    REQUIRE(Unwrap(resolved).size() == 1);
    CHECK(Unwrap(resolved).begin()->first == "deadbeef");
    CHECK(Unwrap(resolved).begin()->second == "/opt/cross/bin/aarch64-none-elf-gcc");
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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).size() == 1);

    // And discovery is not even asked: it costs process spawns and directory walks.
    CHECK(discovery.Calls() == 0);
}

TEST_CASE("NodeToolchains: discovery turned off is a null discovery, not an empty one", "[node][toolchains]")
{
    NodeConfig const cfg = Startable();
    SpawnScript runner;
    ScriptedToolchainHost host;
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, nullptr, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).empty());
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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).empty());
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
    CapturingLogger logger;

    auto const resolved = ResolveToolchains(cfg, &discovery, runner, host, logger);
    REQUIRE(resolved.has_value());
    CHECK(Unwrap(resolved).empty());
}

TEST_CASE("NodeToolchains: a malformed --toolchain is refused, naming the value", "[node][toolchains]")
{
    auto cfg = Startable();
    cfg.toolchains = { "=/usr/bin/gcc" };
    FixedDiscovery discovery { {} };
    SpawnScript runner;
    ScriptedToolchainHost host;
    CapturingLogger logger;

    CHECK_FALSE(ResolveToolchains(cfg, &discovery, runner, host, logger).has_value());
    CHECK(Logged(logger, "malformed --toolchain"));
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
