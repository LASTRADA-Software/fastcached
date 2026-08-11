// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Core/Ranges.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using FastCache::ConfigCandidate;
using FastCache::ConfigErrorCode;
using FastCache::ConfigScope;
using FastCache::DefaultConfigCandidates;
using FastCache::EffectiveConfigPath;
using FastCache::ExpandConfigCandidate;
using FastCache::IConfigPathProbe;
using FastCache::ResolveDefaultConfigPath;
using FastCache::SeedConfigFile;
using FastCache::SeedOutcome;
using FastCache::SystemConfigPath;

namespace
{

/// A scripted environment and filesystem, so the lookup is exercised without
/// touching either. The candidate table is platform-specific, so the tests
/// select rows by shape (has a base variable, has none) rather than by name —
/// what is under test is the priority rule, which is the same everywhere.
class FakeProbe: public IConfigPathProbe
{
  public:
    /// Give every candidate's base variable a distinct value, so each row
    /// expands to a different path.
    void SetAllBaseVars()
    {
        for (auto const& candidate: DefaultConfigCandidates())
            if (!candidate.baseVar.empty())
                _env[std::string { candidate.baseVar }] = std::format("/base/{}", candidate.baseVar);
    }

    /// @param name Variable to define.
    /// @param value Its value.
    void SetEnv(std::string_view name, std::string value)
    {
        _env[std::string { name }] = std::move(value);
    }

    /// @param name Variable to remove.
    void UnsetEnv(std::string_view name)
    {
        _env.erase(std::string { name });
    }

    /// Declare a path readable.
    /// @param path Path to mark.
    void MakeReadable(std::filesystem::path const& path)
    {
        _readable.insert(path.string());
    }

    [[nodiscard]] std::optional<std::string> GetEnv(std::string_view name) const override
    {
        auto const it = _env.find(name);
        return it == _env.end() ? std::nullopt : std::optional { it->second };
    }

    [[nodiscard]] bool IsReadableFile(std::filesystem::path const& path) const override
    {
        return _readable.contains(path.string());
    }

  private:
    std::map<std::string, std::string, std::less<>> _env;
    std::set<std::string> _readable;
};

/// Expand every candidate against a fully populated environment.
/// @param probe Environment source.
/// @return One path per candidate row, in table order.
[[nodiscard]] std::vector<std::filesystem::path> ExpandAll(FakeProbe const& probe)
{
    std::vector<std::filesystem::path> out;
    for (auto const& candidate: DefaultConfigCandidates())
        if (auto const path = ExpandConfigCandidate(candidate, probe))
            out.push_back(*path);
    return out;
}

/// Find the first candidate matching `predicate`; the predicate counterpart to
/// FastCache::FindOrNull, and a pointer for the same reason. See Ranges.hpp.
/// @param predicate Applied to each row in table order.
/// @return The first match, or nullptr.
[[nodiscard]] ConfigCandidate const* FindCandidateIf(auto predicate)
{
    for (auto const& candidate: DefaultConfigCandidates())
        if (predicate(candidate))
            return &candidate;
    return nullptr;
}

/// @return The first machine-wide row, which the table's own static_assert
///         guarantees exists.
[[nodiscard]] ConfigCandidate const* SystemCandidate()
{
    return FastCache::FindOrNull(DefaultConfigCandidates(), ConfigScope::System, &ConfigCandidate::scope);
}

/// A scratch directory for the seeding tests, removed again on destruction so a
/// test run leaves nothing behind in the system temp directory.
class TempDir
{
  public:
    /// @param name Discriminator identifying the case that owns the directory.
    explicit TempDir(std::string_view name)
    {
        // A random suffix, not just the case name: two test binaries running
        // concurrently (the ordinary CI shape) would otherwise share a path and
        // delete each other's scratch tree. Same reasoning as Testing::TempFile.
        std::mt19937_64 rng { std::random_device {}() };
        _path = std::filesystem::temp_directory_path() / "fastcached-test" / std::format("{}-{}", name, rng());
        std::filesystem::create_directories(_path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }

    TempDir(TempDir const&) = delete;
    TempDir& operator=(TempDir const&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    /// @param leaf Name inside the directory.
    /// @return The full path to `leaf`.
    [[nodiscard]] std::filesystem::path operator/(std::string_view leaf) const
    {
        return _path / leaf;
    }

  private:
    std::filesystem::path _path;
};

/// @param path File to create.
/// @param text Contents to write.
void WriteFile(std::filesystem::path const& path, std::string_view text)
{
    std::ofstream out { path, std::ios::binary };
    out << text;
}

/// @param path File to read.
/// @return Its full contents.
[[nodiscard]] std::string ReadFile(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
}

} // namespace

// The table's own invariants (user rows first, a system row exists, every row
// populated) are static_asserts next to the table in DefaultConfigPath.cpp —
// they are properties of a constexpr array, so they fail the build rather than
// a test run. What is left here is the behaviour built on top of them.

TEST_CASE("DefaultConfigPath: the first readable candidate wins", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();
    auto const paths = ExpandAll(probe);
    REQUIRE(paths.size() == DefaultConfigCandidates().size());

    // Make every candidate readable: the resolver must still pick the first.
    for (auto const& path: paths)
        probe.MakeReadable(path);

    auto const resolved = ResolveDefaultConfigPath(probe);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == paths.front());
}

TEST_CASE("DefaultConfigPath: a candidate that exists but cannot be read is skipped", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();
    auto const paths = ExpandAll(probe);
    REQUIRE(paths.size() >= 2);

    // Only the last one is readable. This is the macOS LaunchAgent case: the
    // system config is mode 0640 root:_fastcached, and a per-user daemon has to
    // fall through it rather than fail to start.
    probe.MakeReadable(paths.back());

    auto const resolved = ResolveDefaultConfigPath(probe);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == paths.back());
}

TEST_CASE("DefaultConfigPath: no readable candidate resolves to nothing, not an error", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing was marked readable, so the daemon runs on its built-in defaults.
    REQUIRE(!ResolveDefaultConfigPath(probe).has_value());
}

TEST_CASE("EffectiveConfigPath: a named path is taken verbatim, readable or not", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing is marked readable, and the answer is still the named file: the
    // operator asserted it was there, so a typo has to fail loudly downstream
    // rather than silently start on a different location's settings.
    REQUIRE(EffectiveConfigPath("/etc/typo.yaml", probe) == std::filesystem::path { "/etc/typo.yaml" });

    // ...and it outranks a discovered candidate that *is* readable.
    auto const paths = ExpandAll(probe);
    probe.MakeReadable(paths.front());
    REQUIRE(EffectiveConfigPath("/etc/named.yaml", probe) == std::filesystem::path { "/etc/named.yaml" });
}

TEST_CASE("EffectiveConfigPath: with no named path it discovers, and only when readable", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing readable: an empty path, meaning the built-in defaults apply.
    REQUIRE(EffectiveConfigPath("", probe).empty());

    auto const paths = ExpandAll(probe);
    probe.MakeReadable(paths.back());
    REQUIRE(EffectiveConfigPath("", probe) == paths.back());
}

TEST_CASE("DefaultConfigPath: a base variable that is unset or empty skips its row", "[config][defaultpath]")
{
    auto const* const withVar = FindCandidateIf([](ConfigCandidate const& c) { return !c.baseVar.empty(); });
    REQUIRE(withVar != nullptr);

    FakeProbe probe;
    probe.SetAllBaseVars();

    SECTION("unset")
    {
        probe.UnsetEnv(withVar->baseVar);
    }
    SECTION("set but empty")
    {
        // An empty base would make the suffix relative to the working directory
        // — C:\Windows\System32 for a service. Skipping is the only safe reading.
        probe.SetEnv(withVar->baseVar, "");
    }

    REQUIRE(!ExpandConfigCandidate(*withVar, probe).has_value());
}

TEST_CASE("DefaultConfigPath: a candidate with no base variable is used verbatim", "[config][defaultpath]")
{
    auto const* const absolute = FindCandidateIf([](ConfigCandidate const& c) { return c.baseVar.empty(); });
    if (absolute == nullptr)
    {
        SUCCEED("this platform resolves every location from the environment");
        return;
    }

    FakeProbe const probe; // Deliberately empty: the row must not consult it.
    auto const path = ExpandConfigCandidate(*absolute, probe);
    REQUIRE(path.has_value());
    REQUIRE(*path == std::filesystem::path { absolute->suffix });
}

TEST_CASE("DefaultConfigPath: SystemConfigPath names the machine-wide file even when absent", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing is readable — an installer needs the destination precisely
    // because the file is not there yet.
    auto const system = SystemConfigPath(probe);
    REQUIRE(system.has_value());

    auto const* const firstSystem = SystemCandidate();
    REQUIRE(firstSystem != nullptr);
    REQUIRE(*system == ExpandConfigCandidate(*firstSystem, probe));
}

TEST_CASE("DefaultConfigPath: SystemConfigPath reports the variable it could not expand", "[config][defaultpath]")
{
    auto const* const systemRow = SystemCandidate();
    REQUIRE(systemRow != nullptr);

    if (systemRow->baseVar.empty())
    {
        SUCCEED("this platform's machine-wide location is a compiled-in absolute path and cannot fail");
        return;
    }

    FakeProbe probe;
    probe.SetAllBaseVars();
    probe.UnsetEnv(systemRow->baseVar);

    // A caller that wants to *write* a config has nowhere to go, so this is an
    // error with a diagnosis rather than an empty optional the caller has to
    // explain for itself.
    auto const system = SystemConfigPath(probe);
    REQUIRE(!system.has_value());
    REQUIRE(system.error().code == ConfigErrorCode::UndefinedVariable);
    REQUIRE(system.error().context.find(systemRow->baseVar) != std::string::npos);
}

TEST_CASE("DefaultConfigPath: the platform's locations are the ones the packaging installs", "[config][defaultpath]")
{
    auto const lists = [](std::string_view location) {
        return FastCache::FindOrNull(DefaultConfigCandidates(), location, &ConfigCandidate::display) != nullptr;
    };

#if defined(_WIN32)
    REQUIRE(lists("%ProgramData%\\fastcached\\fastcached.yaml"));
    REQUIRE(lists("%APPDATA%\\fastcached\\fastcached.yaml"));
#else
    // FC_SYSCONF_DIR is handed down from CMake, from the same variable that
    // decides where packaging/ installs the file.
    REQUIRE(lists(FC_SYSCONF_DIR "/fastcached.yaml"));
    REQUIRE(lists("~/.config/fastcached/fastcached.yaml"));
#endif
}

TEST_CASE("SeedConfigFile: writes the template when the destination is absent", "[config][seed]")
{
    TempDir const dir { "seed-writes" };
    auto const source = dir / "fastcached.yaml.default";
    auto const destination = dir / "live" / "fastcached.yaml";
    WriteFile(source, "#port: 6674\n");

    auto const result = SeedConfigFile(source, destination);
    REQUIRE(result.has_value());
    REQUIRE(*result == SeedOutcome::Written);
    REQUIRE(std::filesystem::exists(destination)); // parent directory created on demand
    REQUIRE(ReadFile(destination) == "#port: 6674\n");
}

TEST_CASE("SeedConfigFile: leaves an existing destination untouched", "[config][seed]")
{
    TempDir const dir { "seed-keeps" };
    auto const source = dir / "fastcached.yaml.default";
    auto const destination = dir / "fastcached.yaml";
    WriteFile(source, "#port: 6674\n");
    WriteFile(destination, "port: 7777 # operator edit\n");

    auto const result = SeedConfigFile(source, destination);
    REQUIRE(result.has_value());
    REQUIRE(*result == SeedOutcome::AlreadyPresent);

    // The property the whole seed-once rule exists for: an upgrade re-runs this
    // and must not discard what the operator wrote.
    REQUIRE(ReadFile(destination) == "port: 7777 # operator edit\n");
}

TEST_CASE("SeedConfigFile: reports a missing template rather than creating an empty config", "[config][seed]")
{
    TempDir const dir { "seed-missing" };
    auto const result = SeedConfigFile(dir / "absent.default", dir / "fastcached.yaml");
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == ConfigErrorCode::FileNotFound);
    REQUIRE(!std::filesystem::exists(dir / "fastcached.yaml"));
}
