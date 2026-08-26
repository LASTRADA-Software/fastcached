// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/Registry.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// The ambient facts a machine answers about which toolchains it holds.
///
/// One seam rather than four, because the question is one question: a Visual
/// Studio install is a registry value naming a directory, a version file inside
/// it, a subdirectory named after that version, and a `cl.exe` beneath that -- and
/// splitting those across a filesystem interface, a registry interface and an
/// environment interface would mean a layout row could not be tested end to end
/// without assembling three fakes.
///
/// **The reason it is a seam at all** is that none of the layouts this has to get
/// right can exist on the machine running the tests. A Visual Studio install, a
/// Windows SDK's `Installed Roots`, an Xcode toolchain, a version-suffixed GCC
/// prefix and an MSYS2 root are five mutually exclusive shapes; a discovery that
/// consulted the real host directly would be tested on whichever one the runner
/// happened to have, which is to say tested nowhere. `ScriptedToolchainHost`
/// (ToolchainHostTestUtils.hpp) is the whole point of this interface, not a
/// convenience laid over it.
///
/// Every method is total and reports absence rather than failing: a directory that
/// is not there lists nothing, a registry value that is not there is `nullopt`.
/// Discovery is best-effort by construction -- a machine simply does not have most
/// of these layouts -- so an error channel would carry "this host is not a Mac" on
/// every call and mean nothing.
class IToolchainHost
{
  public:
    IToolchainHost() = default;
    virtual ~IToolchainHost() = default;
    IToolchainHost(IToolchainHost const&) = delete;
    IToolchainHost& operator=(IToolchainHost const&) = delete;
    IToolchainHost(IToolchainHost&&) = delete;
    IToolchainHost& operator=(IToolchainHost&&) = delete;

    /// Whether @p path names an existing directory.
    /// @param path Directory path.
    /// @return True when it exists and is a directory.
    [[nodiscard]] virtual bool DirectoryExists(std::string_view path) = 0;

    /// Whether @p path names a file this host would run.
    ///
    /// On POSIX that is a regular file with an execute bit set for somebody; on
    /// Windows, where the filesystem carries no such bit, it is a regular file.
    /// The difference is stated rather than smoothed over: a POSIX bindir holds
    /// `gcc` beside `gcc.1`, and a discovery that took every entry would offer the
    /// manual page as a compiler.
    ///
    /// @param path File path.
    /// @return True when it exists and could be executed.
    [[nodiscard]] virtual bool ExecutableExists(std::string_view path) = 0;

    /// The immediate subdirectory names of @p path.
    ///
    /// Names, not paths, because that is what a caller matching a version number
    /// against them needs, and rebuilding the full path is a join the caller was
    /// going to do anyway.
    ///
    /// @param path Directory to list.
    /// @return Its subdirectory names, in no guaranteed order; empty when the
    ///         directory does not exist or cannot be read.
    [[nodiscard]] virtual std::vector<std::string> ListDirectories(std::string_view path) = 0;

    /// The immediate file names of @p path.
    /// @param path Directory to list.
    /// @return Its file names, in no guaranteed order; empty when the directory
    ///         does not exist or cannot be read.
    [[nodiscard]] virtual std::vector<std::string> ListFiles(std::string_view path) = 0;

    /// Read a small text file whole.
    ///
    /// For the version stamps a toolchain layout leaves behind --
    /// `Microsoft.VCToolsVersion.default.txt` is one line. Not for anything large:
    /// there is no streaming here and no size cap, because every caller names a
    /// file whose shape it already knows.
    ///
    /// @param path File to read.
    /// @return Its contents, or `std::nullopt` when it cannot be read.
    [[nodiscard]] virtual std::optional<std::string> ReadTextFile(std::string_view path) = 0;

    /// Read a string value from the Windows registry.
    ///
    /// Hive and view are parameters rather than a policy inside the
    /// implementation, so a layout row can *say* which it means. They are not
    /// interchangeable and trying all four would be wrong in both directions: a
    /// per-user LLVM install is genuinely in `HKCU`, and `Windows Kits\Installed
    /// Roots` is genuinely in the 32-bit view, so a lookup that fell back would
    /// report a toolchain from whichever key answered first rather than from the
    /// one the layout describes.
    ///
    /// @param hive Which root to start from.
    /// @param subKey Key path beneath it, backslash-separated.
    /// @param valueName The value to read; empty names the key's default value.
    /// @param view Which of a 64-bit host's two registry views to read.
    /// @return The value, or `std::nullopt` (always, off Windows).
    [[nodiscard]] virtual std::optional<std::string> RegistryString(RegistryHive hive,
                                                                    std::string_view subKey,
                                                                    std::string_view valueName,
                                                                    RegistryView view) = 0;

    /// List the value names directly under a registry key.
    ///
    /// Needed because one key discovery reads is a *set*: the Windows SDK records
    /// each installed kit as a value under `Installed Roots`.
    ///
    /// @param hive Which root to start from.
    /// @param subKey Key path beneath it, backslash-separated.
    /// @param view Which of a 64-bit host's two registry views to read.
    /// @return The value names; empty when the key is absent (always, off Windows).
    [[nodiscard]] virtual std::vector<std::string> RegistryValueNames(RegistryHive hive,
                                                                      std::string_view subKey,
                                                                      RegistryView view) = 0;

    /// Read a variable from the process environment.
    /// @param name Variable name.
    /// @return Its value, or `std::nullopt` when unset.
    [[nodiscard]] virtual std::optional<std::string> Environment(std::string_view name) = 0;

    /// Where a bare executable name resolves to on this host's search path.
    ///
    /// Here because the same compiler must fingerprint identically however it was
    /// named. A build system invokes `cl` while a worker is configured with the
    /// absolute path, and deriving an MSVC toolchain's include roots from its
    /// install layout needs the absolute one -- so a bare name that is never
    /// resolved would give the two ends different roots and, through them,
    /// different fingerprints, which presents as a scheduler that matches nothing.
    ///
    /// A name that already contains a separator is returned as given: it is not a
    /// search-path lookup and treating it as one would be a second, wrong answer.
    /// On Windows the executable suffix is appended when the name carries none.
    ///
    /// @param name An executable name, or a path.
    /// @return The resolved path, or `std::nullopt` when nothing on the search
    ///         path matches.
    [[nodiscard]] virtual std::optional<std::string> ResolveOnSearchPath(std::string_view name) = 0;
};

/// Create the host-backed implementation.
///
/// Reaches the real filesystem, the real registry (through `Platform/Registry`)
/// and the real environment (through `Platform/Environment`) -- never a second
/// copy of any of the three.
///
/// @return A host answering for the machine this process runs on.
[[nodiscard]] std::unique_ptr<IToolchainHost> MakeToolchainHost();

} // namespace FastCache::Cc
