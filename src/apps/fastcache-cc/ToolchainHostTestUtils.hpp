// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ToolchainHost.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/// A machine described by a test rather than by whatever the runner happens to
/// have installed. Header-only so each test translation unit gets its own
/// `inline` copies without an extra link target, the same arrangement as
/// `Platform/EnvironmentTestUtils.hpp`.
namespace FastCache::Cc::Testing
{

/// `IToolchainHost` over an in-memory filesystem, registry and environment.
///
/// This is what makes the layout table testable at all. A Visual Studio install,
/// a Windows SDK's `Installed Roots`, an Xcode toolchain, a version-suffixed GCC
/// prefix and an MSYS2 root are five shapes that cannot coexist on one machine,
/// and four of the five are absent from every CI runner this project uses -- so a
/// discovery tested against the real host is a discovery tested on one row of its
/// own table.
///
/// **Separators are normalized to `/` on both store and lookup**, which is not
/// tidiness. A layout row spells a Windows path with backslashes, a registry value
/// comes back with backslashes, and the code joining them uses
/// `std::filesystem::path`, whose preferred separator differs by host -- so the
/// same location arrives here spelled three ways, and a fake that compared them
/// literally would report "not found" for a file the test just added. The real
/// host has no such problem: Windows accepts either separator and POSIX only ever
/// sees one.
class ScriptedToolchainHost final: public IToolchainHost
{
  public:
    /// The suffix a bare executable name gains on a Windows-shaped machine.
    ///
    /// Named here rather than taken from the host this test binary runs on: a
    /// scripted machine is whatever the case says it is, and a Windows layout has
    /// to be describable from a Linux runner.
    static constexpr std::string_view WindowsExecutableSuffix = ".exe";

    /// Add a file, creating every directory above it.
    /// @param path Where the file lives.
    /// @param contents What reading it yields.
    /// @return This host, for chaining.
    ScriptedToolchainHost& AddFile(std::string_view path, std::string contents)
    {
        auto const key = Normalize(path);
        AddParents(key);
        _files.insert_or_assign(key, std::move(contents));
        return *this;
    }

    /// Add an executable file, creating every directory above it.
    ///
    /// Separate from `AddFile` because the distinction is one discovery turns on:
    /// a POSIX bindir holds `gcc` beside `gcc.1`, and a layout that offered the
    /// manual page as a compiler would register a toolchain that cannot run.
    ///
    /// @param path Where the executable lives.
    /// @return This host, for chaining.
    ScriptedToolchainHost& AddExecutable(std::string_view path)
    {
        auto const key = Normalize(path);
        AddParents(key);
        _files.insert_or_assign(key, std::string {});
        _executables.insert(key);
        return *this;
    }

    /// Add a directory, creating every directory above it.
    /// @param path The directory.
    /// @return This host, for chaining.
    ScriptedToolchainHost& AddDirectory(std::string_view path)
    {
        auto const key = Normalize(path);
        AddParents(key);
        _directories.insert(key);
        return *this;
    }

    /// Add a registry value, creating the key.
    /// @param hive Which root it lives under.
    /// @param subKey Key path.
    /// @param valueName The value's name.
    /// @param data The value.
    /// @param view Which registry view holds it.
    /// @return This host, for chaining.
    ScriptedToolchainHost& AddRegistryValue(
        RegistryHive hive, std::string_view subKey, std::string_view valueName, std::string data, RegistryView view)
    {
        _registry.insert_or_assign(RegistryKeyOf(hive, subKey, valueName, view), std::move(data));
        return *this;
    }

    /// Set an environment variable.
    /// @param name Variable name.
    /// @param value Its value.
    /// @return This host, for chaining.
    ScriptedToolchainHost& SetEnvironment(std::string_view name, std::string value)
    {
        _environment.insert_or_assign(std::string { name }, std::move(value));
        return *this;
    }

    /// Set the directories `ResolveOnSearchPath` walks, in order.
    /// @param directories The search path.
    /// @return This host, for chaining.
    ScriptedToolchainHost& SetSearchPath(std::vector<std::string> directories)
    {
        _searchPath = std::move(directories);
        return *this;
    }

    bool DirectoryExists(std::string_view path) override
    {
        return _directories.contains(Normalize(path));
    }

    bool ExecutableExists(std::string_view path) override
    {
        return _executables.contains(Normalize(path));
    }

    std::vector<std::string> ListDirectories(std::string_view path) override
    {
        return ChildrenOf(path, _directories);
    }

    std::vector<std::string> ListFiles(std::string_view path) override
    {
        return ChildrenOf(path, std::views::keys(_files));
    }

    std::optional<std::string> ReadTextFile(std::string_view path) override
    {
        auto const found = _files.find(Normalize(path));
        if (found == _files.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<std::string> RegistryString(RegistryHive hive,
                                              std::string_view subKey,
                                              std::string_view valueName,
                                              RegistryView view) override
    {
        auto const found = _registry.find(RegistryKeyOf(hive, subKey, valueName, view));
        if (found == _registry.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<std::string> Environment(std::string_view name) override
    {
        auto const found = _environment.find(std::string { name });
        if (found == _environment.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<std::string> ResolveOnSearchPath(std::string_view name) override
    {
        if (name.empty())
            return std::nullopt;
        if (name.contains('/') || name.contains('\\'))
            return std::string { name };

        for (auto const& directory: _searchPath)
        {
            auto candidate = Normalize(directory) + "/" + std::string { name };

            // `.exe` is tried first and is NOT conditioned on the host running the
            // test. This fake describes the machine a *case* wrote down, and the
            // machine that matters most here is a Windows one -- a bare `cl` on the
            // search path resolving to `cl.exe` is the load-bearing case for
            // fingerprint agreement, and gating it on `_WIN32` would leave that case
            // untested on the two platforms where most of the suite runs. A
            // POSIX-shaped scripted machine has no `.exe` entry, so nothing changes
            // there. Same ordering as `HostToolchainHost`, for the same reason.
            if (!name.ends_with(WindowsExecutableSuffix))
                if (auto suffixed = candidate + std::string { WindowsExecutableSuffix }; _executables.contains(suffixed))
                    return suffixed;

            if (_executables.contains(candidate))
                return candidate;
        }
        return std::nullopt;
    }

    /// Collapse every spelling of one path to the one this host stores.
    ///
    /// Exposed because a test asserting on a discovered path is comparing against
    /// something this normalized, and spelling the rule twice is how the two drift.
    ///
    /// @param path Any spelling.
    /// @return `/`-separated, with no trailing separator.
    [[nodiscard]] static std::string Normalize(std::string_view path)
    {
        std::string result { path };
        std::ranges::replace(result, '\\', '/');
        while (result.size() > 1 && result.back() == '/')
            result.pop_back();
        return result;
    }

  private:
    /// One registry value's identity.
    using RegistryEntry = std::tuple<RegistryHive, std::string, std::string, RegistryView>;

    /// Registry key paths are case-insensitive and separator-fixed on the real
    /// thing; here they are compared verbatim apart from a leading or trailing
    /// backslash, which a caller building one by concatenation easily leaves.
    /// @param subKey Any spelling of the key path.
    /// @return The stored spelling.
    [[nodiscard]] static std::string NormalizeRegistryKey(std::string_view subKey)
    {
        std::string result { subKey };
        while (!result.empty() && result.back() == '\\')
            result.pop_back();
        return result;
    }

    /// @param hive The hive.
    /// @param subKey The key path.
    /// @param valueName The value name.
    /// @param view The registry view.
    /// @return The map key identifying that value.
    [[nodiscard]] static RegistryEntry RegistryKeyOf(RegistryHive hive,
                                                     std::string_view subKey,
                                                     std::string_view valueName,
                                                     RegistryView view)
    {
        return RegistryEntry { hive, NormalizeRegistryKey(subKey), std::string { valueName }, view };
    }

    /// Record every directory on the way to @p path, so a test naming a deep file
    /// does not also have to name the five directories above it.
    /// @param path An already-normalized path.
    void AddParents(std::string const& path)
    {
        auto slash = path.find('/');
        while (slash != std::string::npos)
        {
            if (slash > 0)
                _directories.insert(path.substr(0, slash));
            slash = path.find('/', slash + 1);
        }
    }

    /// The immediate child names of @p parent among @p paths.
    ///
    /// A template over the range rather than one overload per container, so
    /// listing files can hand it a keys view of the file map instead of
    /// materializing a second set of every path on every call.
    ///
    /// @param parent Directory whose children are wanted.
    /// @param paths Every path of the kind being listed.
    /// @return The child names.
    template <typename Paths>
    [[nodiscard]] static std::vector<std::string> ChildrenOf(std::string_view parent, Paths const& paths)
    {
        auto const prefix = Normalize(parent) + "/";
        std::vector<std::string> names;
        for (auto const& path: paths)
        {
            if (!path.starts_with(prefix))
                continue;
            auto const remainder = std::string_view { path }.substr(prefix.size());
            if (remainder.empty() || remainder.contains('/'))
                continue;
            names.emplace_back(remainder);
        }
        return names;
    }

    std::set<std::string> _directories;
    std::set<std::string> _executables;
    std::map<std::string, std::string> _files;
    std::map<RegistryEntry, std::string> _registry;
    std::map<std::string, std::string> _environment;
    std::vector<std::string> _searchPath;
};

} // namespace FastCache::Cc::Testing
