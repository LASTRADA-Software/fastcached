// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Platform/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
    #include <sys/stat.h>

    #include <unistd.h>
#endif

using FastCache::ConfigCandidate;
using FastCache::ConfigErrorCode;
using FastCache::ConfigScope;
using FastCache::DefaultConfigCandidates;
using FastCache::DirectoryPolicy;
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

    /// Declare a path one a non-administrator could have written. Trust is the
    /// default, so a test that does not care about it reads as if the check
    /// were not there.
    /// @param path Path to mark.
    void MakeUntrusted(std::filesystem::path const& path)
    {
        _untrusted.insert(path.string());
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

    [[nodiscard]] bool IsTrustedSystemLocation(std::filesystem::path const& path) const override
    {
        return !_untrusted.contains(path.string());
    }

  private:
    std::map<std::string, std::string, std::less<>> _env;
    std::set<std::string> _readable;
    std::set<std::string> _untrusted;
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
    /// @param root Where to put it. The default is the system temp directory;
    ///        the trust cases override it because what they are testing is a
    ///        property of the *containing* directory's permissions, which the
    ///        temp directory does not have.
    explicit TempDir(std::string_view name, std::filesystem::path const& root = std::filesystem::temp_directory_path())
    {
        // A random suffix, not just the case name: two test binaries running
        // concurrently (the ordinary CI shape) would otherwise share a path and
        // delete each other's scratch tree. Same reasoning as Testing::TempFile.
        std::mt19937_64 rng { std::random_device {}() };
        _path = root / "fastcached-test" / std::format("{}-{}", name, rng());
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

    /// @return The directory itself.
    [[nodiscard]] std::filesystem::path const& Path() const noexcept
    {
        return _path;
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
    // Via the stream buffer rather than istreambuf_iterator: GCC inlines the
    // iterator's sbumpc() far enough to see a path where the buffer pointer
    // could be null and rejects it under -Werror=null-dereference, which for an
    // ifstream it never is. Inserting a streambuf* handles null by setting
    // failbit, so there is nothing for it to complain about.
    std::ifstream in { path, std::ios::binary };
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
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
    REQUIRE(resolved.path == paths.front());
    REQUIRE(resolved.rejected.empty());
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
    REQUIRE(resolved.path == paths.back());

    // An unreadable candidate is ordinary, so nothing is reported about it.
    REQUIRE(resolved.rejected.empty());
}

TEST_CASE("DefaultConfigPath: no readable candidate resolves to nothing, not an error", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing was marked readable, so the daemon runs on its built-in defaults.
    REQUIRE(ResolveDefaultConfigPath(probe).path.empty());
}

TEST_CASE("EffectiveConfigPath: a named path is taken verbatim, readable or not", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing is marked readable, and the answer is still the named file: the
    // operator asserted it was there, so a typo has to fail loudly downstream
    // rather than silently start on a different location's settings.
    REQUIRE(EffectiveConfigPath("/etc/typo.yaml", probe).path == std::filesystem::path { "/etc/typo.yaml" });

    // ...and it outranks a discovered candidate that *is* readable.
    auto const paths = ExpandAll(probe);
    probe.MakeReadable(paths.front());
    REQUIRE(EffectiveConfigPath("/etc/named.yaml", probe).path == std::filesystem::path { "/etc/named.yaml" });

    // A named path is not trust-checked either: the operator pointed at that
    // file, which is theirs to decide, and second-guessing it would make
    // --config unusable for exactly the recovery case it exists for.
    probe.MakeUntrusted("/etc/named.yaml");
    auto const named = EffectiveConfigPath("/etc/named.yaml", probe);
    REQUIRE(named.path == std::filesystem::path { "/etc/named.yaml" });
    REQUIRE(named.rejected.empty());
}

TEST_CASE("EffectiveConfigPath: with no named path it discovers, and only when readable", "[config][defaultpath]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();

    // Nothing readable: an empty path, meaning the built-in defaults apply.
    REQUIRE(EffectiveConfigPath("", probe).path.empty());

    auto const paths = ExpandAll(probe);
    probe.MakeReadable(paths.back());
    REQUIRE(EffectiveConfigPath("", probe).path == paths.back());
}

TEST_CASE("DefaultConfigPath: a machine-wide candidate a non-admin could have written is refused",
          "[config][defaultpath][trust]")
{
    auto const* const systemRow = SystemCandidate();
    REQUIRE(systemRow != nullptr);

    FakeProbe probe;
    probe.SetAllBaseVars();

    // value_or rather than a dereference after REQUIRE: Catch2's macros are
    // opaque to clang-tidy's optional analysis, so an empty path is the way to
    // say "expansion failed" in a form it can follow. Same everywhere below.
    auto const systemPath = ExpandConfigCandidate(*systemRow, probe).value_or(std::filesystem::path {});
    REQUIRE_FALSE(systemPath.empty());

    // Readable, and the only candidate there is — but in a directory anyone can
    // write, which is how a standard account plants the configuration that a
    // LocalSystem service would then obey.
    probe.MakeReadable(systemPath);
    probe.MakeUntrusted(systemPath);

    auto const resolved = ResolveDefaultConfigPath(probe);
    REQUIRE(resolved.path.empty()); // built-in defaults, not the planted file

    // And loudly: this is the one skip the operator has to be told about, since
    // the file is sitting right there and being ignored.
    REQUIRE(resolved.rejected.size() == 1);
    REQUIRE(resolved.rejected.front().path == systemPath);
    REQUIRE_FALSE(resolved.rejected.front().reason.empty());

    // The message has to be actionable, so it names the directory at fault.
    REQUIRE(resolved.rejected.front().reason.contains(systemPath.parent_path().string()));
}

TEST_CASE("DefaultConfigPath: the trust rule applies to machine-wide rows only", "[config][defaultpath][trust]")
{
    auto const* const userRow = FindCandidateIf([](ConfigCandidate const& c) { return c.scope == ConfigScope::User; });
    REQUIRE(userRow != nullptr);

    FakeProbe probe;
    probe.SetAllBaseVars();
    auto const userPath = ExpandConfigCandidate(*userRow, probe).value_or(std::filesystem::path {});
    REQUIRE_FALSE(userPath.empty());

    // A per-user config lives in the account's own directory, so "someone other
    // than an administrator can write here" is its normal state, not a finding.
    // Checking it would reject every per-user config on every platform.
    probe.MakeReadable(userPath);
    probe.MakeUntrusted(userPath);

    auto const resolved = ResolveDefaultConfigPath(probe);
    REQUIRE(resolved.path == userPath);
    REQUIRE(resolved.rejected.empty());
}

TEST_CASE("DefaultConfigPath: an untrusted machine-wide row does not shadow a good one", "[config][defaultpath][trust]")
{
    FakeProbe probe;
    probe.SetAllBaseVars();
    auto const paths = ExpandAll(probe);
    REQUIRE(paths.size() >= 2);

    auto const* const systemRow = SystemCandidate();
    REQUIRE(systemRow != nullptr);
    auto const systemPath = ExpandConfigCandidate(*systemRow, probe).value_or(std::filesystem::path {});
    REQUIRE_FALSE(systemPath.empty());

    // Every candidate readable, the machine-wide one untrusted. The user row
    // outranks it anyway, so the rejection must not be reported: nothing was
    // withheld from the operator, because that file was never going to be used.
    for (auto const& path: paths)
        probe.MakeReadable(path);
    probe.MakeUntrusted(systemPath);

    auto const resolved = ResolveDefaultConfigPath(probe);
    REQUIRE(resolved.path == paths.front());
    REQUIRE(resolved.rejected.empty());
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
    REQUIRE(ExpandConfigCandidate(*absolute, probe) == std::optional { std::filesystem::path { absolute->suffix } });
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
    REQUIRE(system.error().context.contains(systemRow->baseVar));
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

    auto const result = SeedConfigFile(source, destination, DirectoryPolicy::Inherit);
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

    auto const result = SeedConfigFile(source, destination, DirectoryPolicy::Inherit);
    REQUIRE(result.has_value());
    REQUIRE(*result == SeedOutcome::AlreadyPresent);

    // The property the whole seed-once rule exists for: an upgrade re-runs this
    // and must not discard what the operator wrote.
    REQUIRE(ReadFile(destination) == "port: 7777 # operator edit\n");
}

TEST_CASE("SeedConfigFile: reports a missing template rather than creating an empty config", "[config][seed]")
{
    TempDir const dir { "seed-missing" };
    auto const result = SeedConfigFile(dir / "absent.default", dir / "fastcached.yaml", DirectoryPolicy::Inherit);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == ConfigErrorCode::FileNotFound);
    REQUIRE(!std::filesystem::exists(dir / "fastcached.yaml"));
}

TEST_CASE("SeedConfigFile: AdministratorsOnly either secures the directory or refuses to seed", "[config][seed][trust]")
{
    // Seeding by hand is the one path that creates the machine-wide config
    // directory without the installer, and a directory created under
    // %ProgramData% inherits create-file for every standard account. Left that
    // way, --seed-config would write a config the daemon then refuses — the
    // tool defeating itself — so the policy tightens the directory first.
    //
    // Which branch runs is decided by the rights of whoever is running the
    // suite, and BOTH are properties worth pinning: with administrative rights
    // the seeded config must come out trusted, and without them the seeding
    // must fail rather than plant a machine-wide config an unprivileged account
    // would still own. CI takes the first branch, a developer's shell the
    // second.
    TempDir const dir { "seed-secures" };

    auto const source = dir / "fastcached.yaml.default";
    auto const destination = dir / "live" / "fastcached.yaml";
    WriteFile(source, "#port: 6674\n");

    auto const result = SeedConfigFile(source, destination, DirectoryPolicy::AdministratorsOnly);

    if (!result.has_value())
    {
        REQUIRE(result.error().code == ConfigErrorCode::WriteFailed);

        // Nothing half-done: the refusal happens before the copy, so no config
        // is left sitting in a directory that could not be secured.
        REQUIRE_FALSE(std::filesystem::exists(destination));

        // And the message has to say what to do about it.
        REQUIRE(result.error().context.contains("administrative rights"));
        return;
    }

    REQUIRE(*result == SeedOutcome::Written);

    FastCache::SystemConfigPathProbe const probe;
    REQUIRE(probe.IsReadableFile(destination));
    REQUIRE(probe.IsTrustedSystemLocation(destination));
}

// The production probe. Everything above runs against FakeProbe, which is what
// makes the decision logic testable — but the rules those decisions encode
// ("readable", "only an administrator could have put this here") live in
// SystemConfigPathProbe, and a fake cannot get them wrong on its behalf.

TEST_CASE("SystemConfigPathProbe: readability means an ordinary file this account can open", "[config][probe]")
{
    FastCache::SystemConfigPathProbe const probe;
    TempDir const dir { "readable" };

    auto const file = dir / "fastcached.yaml";
    WriteFile(file, "port: 6674\n");
    REQUIRE(probe.IsReadableFile(file));

    // Absent is the ordinary case for a candidate location, and the one that
    // has to stay quiet rather than become an error.
    REQUIRE_FALSE(probe.IsReadableFile(dir / "not-there.yaml"));

    // A directory is not a config file, however openable it looks: fopen() on
    // one succeeds on Linux, so opening alone would accept it and the lookup
    // would settle on something that was never a candidate.
    REQUIRE_FALSE(probe.IsReadableFile(dir.Path()));
}

#if !defined(_WIN32)
TEST_CASE("SystemConfigPathProbe: a file this account may not read is not readable", "[config][probe]")
{
    if (::geteuid() == 0)
    {
        SUCCEED("running as root, for whom the permission bits are advisory");
        return;
    }

    FastCache::SystemConfigPathProbe const probe;
    TempDir const dir { "unreadable" };

    // The macOS LaunchAgent case in miniature, and the whole reason the test is
    // readability rather than existence: the system config is mode 0640
    // root:_fastcached, so an agent running as the operator has to fall through
    // it rather than fail to start.
    auto const file = dir / "fastcached.yaml";
    WriteFile(file, "port: 6674\n");
    REQUIRE(::chmod(file.string().c_str(), 0) == 0);

    REQUIRE_FALSE(probe.IsReadableFile(file));

    // Restore, or TempDir cannot clean up after itself.
    REQUIRE(::chmod(file.string().c_str(), 0600) == 0);
}
#endif

TEST_CASE("SystemConfigPathProbe: a directory a standard account can write is not trusted", "[config][trust][probe]")
{
    FastCache::SystemConfigPathProbe const probe;

#if defined(_WIN32)
    // Under %ProgramData% on purpose. Its ACL grants BUILTIN\Users create-file
    // and create-folder, inherited by every subdirectory, so this test performs
    // the exact setup the check exists to refuse — and performs it with no
    // privileges at all, which is the reason it has to be refused.
    auto const programData = FastCache::ReadEnvironmentVariable("ProgramData").value_or(std::string {});
    REQUIRE_FALSE(programData.empty());
    TempDir const squat { "trust-loose", std::filesystem::path { programData } };
#else
    // The POSIX shape of the same thing. 0777 rather than merely
    // non-root-owned, so the case still holds when the suite runs as root —
    // which it does in most CI containers.
    TempDir const squat { "trust-loose" };
    REQUIRE(::chmod(squat.Path().string().c_str(), 0777) == 0);
#endif

    auto const planted = squat / "fastcached.yaml";
    WriteFile(planted, "port: 6674\n");
    REQUIRE(probe.IsReadableFile(planted));

    // Readable, and still not something to obey.
    REQUIRE_FALSE(probe.IsTrustedSystemLocation(planted));
}

TEST_CASE("SystemConfigPathProbe: a file only administrators can replace is trusted", "[config][trust][probe]")
{
    FastCache::SystemConfigPathProbe const probe;

    // A file the platform itself installs, in the directory it installs it
    // into: administrators and SYSTEM may write, everyone else may only read.
    // If this ever stops being true the check has become useless, so asserting
    // against a real system path is the point rather than an inconvenience.
#if defined(_WIN32)
    auto const systemRoot = FastCache::ReadEnvironmentVariable("SystemRoot").value_or(std::string {});
    REQUIRE_FALSE(systemRoot.empty());
    auto const wellKnown = std::filesystem::path { systemRoot } / "System32/drivers/etc/hosts";
#else
    auto const wellKnown = std::filesystem::path { "/etc/hosts" };
#endif

    if (!probe.IsReadableFile(wellKnown))
    {
        SUCCEED("no readable /etc/hosts equivalent on this machine");
        return;
    }

    REQUIRE(probe.IsTrustedSystemLocation(wellKnown));
}
