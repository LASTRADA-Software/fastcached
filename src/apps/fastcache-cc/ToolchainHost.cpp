// SPDX-License-Identifier: Apache-2.0
#include "ToolchainHost.hpp"

#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    // `<windows.h>` ALONE, and the single header is the fix rather than a tidy-up.
    // This block was `<processthreadsapi.h>` beside it, written in that order --
    // and clang-format sorts an include block alphabetically, so it put
    // `processthreadsapi.h` FIRST and deleted the blank line separating them.
    // `winnt.h` then arrives without the architecture `windows.h` sets up and stops
    // the build with `#error "No Target Architecture"`. Invisible on Linux, where
    // neither header exists and the block is preprocessed away, so the formatter
    // broke a platform it could not compile for. `windows.h` declares
    // `IsWow64Process2` and `GetNativeSystemInfo` on its own with
    // `_WIN32_WINNT=0x0A00`, which this build sets, so nothing is lost by dropping
    // the second include -- and with one header there is no order left to sort.
    #include <windows.h>
#else
    #include <sys/utsname.h>
#endif

namespace FastCache::Cc
{

namespace
{
    /// The separator this host splits a search-path variable on.
    ///
    /// A `#if` rather than a runtime test because it is a property of the host's
    /// convention rather than of anything observable: `;` on Windows, where a
    /// drive letter's colon rules out the POSIX spelling, and `:` everywhere else.
    constexpr char SearchPathSeparator =
#if defined(_WIN32)
        ';';
#else
        ':';
#endif

    /// The suffix a bare executable name gains on this host, if any.
    constexpr std::string_view ExecutableSuffix =
#if defined(_WIN32)
        ".exe";
#else
        {};
#endif

    /// Whether @p name already spells a location rather than a bare name.
    ///
    /// Both separators, on both platforms: a Windows build system writes either,
    /// and a POSIX path never contains a backslash that is not part of a filename
    /// nobody sensible has.
    ///
    /// @param name The candidate.
    /// @return True when it carries a directory separator.
    [[nodiscard]] bool LooksLikePath(std::string_view name) noexcept
    {
        return name.contains('/') || name.contains('\\');
    }

    /// The names of a directory's entries matching a predicate.
    ///
    /// One walk shared by the two listing methods, which differ only in what they
    /// keep -- two hand-written iterations differing by one call is the repetition
    /// the guidelines name outright.
    ///
    /// Non-throwing throughout: `directory_iterator` on a path that is not a
    /// directory, was removed underneath the walk, or cannot be opened is ordinary
    /// here, and the answer to all three is the same empty list.
    ///
    /// @param path Directory to walk.
    /// @param keep Applied to each entry; true keeps its name.
    /// @return The matching entry names.
    template <typename Predicate>
    [[nodiscard]] std::vector<std::string> EntryNames(std::string_view path, Predicate keep)
    {
        // Through `PathFromNarrowText`, like every leaf probe in this file: a
        // directory named by bytes this process cannot read is one it cannot walk,
        // which is the same answer as "not there" -- and building the path directly
        // would THROW, before the `error_code` this call takes is ever consulted.
        auto const directory = PathFromNarrowText(path);
        if (!directory.has_value())
            return {};

        std::error_code ec;
        std::filesystem::directory_iterator entry { *directory,
                                                    std::filesystem::directory_options::skip_permission_denied,
                                                    ec };
        if (ec)
            return {};

        // Stepped with `increment(ec)` rather than range-for, because `operator++`
        // THROWS on an entry it cannot stat -- and a directory that changes under a
        // walk is ordinary. An exception escaping here would take down a startup
        // whose whole job is to survey a machine best-effort.
        std::vector<std::string> names;
        std::filesystem::directory_iterator const done {};
        while (entry != done)
        {
            std::error_code entryEc;
            if (keep(*entry, entryEc) && !entryEc)
                names.push_back(entry->path().filename().string());
            entry.increment(ec);
            if (ec)
                break;
        }
        return names;
    }
} // namespace

namespace
{
    /// `IToolchainHost` over the machine this process is running on.
    class HostToolchainHost final: public IToolchainHost
    {
      public:
        /// The one place the `#if` lives. Everything above it asks the HOST.
        [[nodiscard]] bool LeadingSlashIsDriveRelative() const noexcept override
        {
#if defined(_WIN32)
            return true;
#else
            return false;
#endif
        }

        /// Ask the OS, never the preprocessor -- this is the one host fact where
        /// build time and run time genuinely disagree.
        ///
        /// **`GetSystemInfo` is the wrong call and it is the one to reach for
        /// first.** Under WOW64 it reports the EMULATED architecture, so an x64
        /// process on an ARM64 host is told `x64` -- which reproduces #146 exactly
        /// while looking like a fix, and passes any test written against a scripted
        /// host. `GetNativeSystemInfo` is the documented way to see past the
        /// emulation, and `IsWow64Process2` reports it as a machine type directly.
        ///
        /// On POSIX `uname -m` is the machine, since nothing here emulates a
        /// different one behind the kernel's back.
        [[nodiscard]] HostArchitecture NativeArchitecture() const noexcept override
        {
#if defined(_WIN32)
            // `nativeMachine` is filled in whether or not this process is under
            // WOW64, which is what makes one call sufficient for both cases.
            USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
            if (::IsWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine) != 0)
                switch (nativeMachine)
                {
                    case IMAGE_FILE_MACHINE_ARM64:
                        return HostArchitecture::Arm64;
                    case IMAGE_FILE_MACHINE_AMD64:
                        return HostArchitecture::X64;
                    case IMAGE_FILE_MACHINE_I386:
                        return HostArchitecture::X86;
                    default:
                        break;
                }

            // The fallback is still a NATIVE reading rather than the emulated one.
            SYSTEM_INFO info {};
            ::GetNativeSystemInfo(&info);
            switch (info.wProcessorArchitecture)
            {
                case PROCESSOR_ARCHITECTURE_ARM64:
                    return HostArchitecture::Arm64;
                case PROCESSOR_ARCHITECTURE_AMD64:
                    return HostArchitecture::X64;
                default:
                    return HostArchitecture::X86;
            }
#else
            utsname info {};
            if (::uname(&info) != 0)
                return HostArchitecture::X86;
            auto const machine = std::string_view { static_cast<char const*>(info.machine) };
            if (machine == "aarch64" || machine == "arm64")
                return HostArchitecture::Arm64;
            if (machine == "x86_64" || machine == "amd64")
                return HostArchitecture::X64;
            return HostArchitecture::X86;
#endif
        }

        bool DirectoryExists(std::string_view path) override
        {
            auto const directory = PathFromNarrowText(path);
            if (!directory.has_value())
                return false;

            std::error_code ec;
            return std::filesystem::is_directory(*directory, ec) && !ec;
        }

        bool ExecutableExists(std::string_view path) override
        {
            auto const opened = PathFromNarrowText(path);
            if (!opened.has_value())
                return false;

            auto const& file = *opened;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec) || ec)
                return false;

#if defined(_WIN32)
            // Windows carries no execute bit, so a regular file is as much as this
            // can honestly say. The layout table names the binaries it is looking
            // for, so nothing here has to guess from an extension either.
            return true;
#else
            auto const status = std::filesystem::status(file, ec);
            if (ec)
                return false;
            using std::filesystem::perms;
            return (status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
#endif
        }

        std::vector<std::string> ListDirectories(std::string_view path) override
        {
            return EntryNames(path, [](auto const& entry, std::error_code& ec) { return entry.is_directory(ec); });
        }

        std::vector<std::string> ListFiles(std::string_view path) override
        {
            return EntryNames(path, [](auto const& entry, std::error_code& ec) { return entry.is_regular_file(ec); });
        }

        std::optional<std::string> ReadTextFile(std::string_view path) override
        {
            // Asked BEFORE the stream is opened, because opening is not the check
            // it looks like: on Linux an `ifstream` opens a DIRECTORY perfectly
            // happily and only the read fails, so `if (!in)` lets one through and
            // this returns an empty string -- "a version file that says nothing"
            // rather than "there is no version file", which are different answers
            // to a caller deciding whether a layout is present.
            auto const opened = PathFromNarrowText(path);
            if (!opened.has_value())
                return std::nullopt;

            auto const& file = *opened;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec) || ec)
                return std::nullopt;

            std::ifstream in { file, std::ios::binary };
            if (!in)
                return std::nullopt;
            std::ostringstream text;
            text << in.rdbuf();
            return std::move(text).str();
        }

        std::optional<std::string> RegistryString(RegistryHive hive,
                                                  std::string_view subKey,
                                                  std::string_view valueName,
                                                  RegistryView view) override
        {
            return ReadRegistryString(hive, subKey, valueName, view);
        }

        std::optional<std::string> Environment(std::string_view name) override
        {
            return ReadEnvironmentVariable(name);
        }

        std::optional<std::string> ResolveOnSearchPath(std::string_view name) override
        {
            if (name.empty())
                return std::nullopt;
            if (LooksLikePath(name))
                return std::string { name };

            auto const searchPath = ReadEnvironmentVariable("PATH");
            if (!searchPath.has_value())
                return std::nullopt;

            // Split by hand rather than through a view pipeline: an empty entry is
            // ordinary in a real PATH (a trailing separator, or two concatenated by
            // a script) and means the working directory, which is not a place a
            // toolchain is looked for.
            std::string_view remaining { *searchPath };
            while (!remaining.empty())
            {
                auto const end = remaining.find(SearchPathSeparator);
                auto const directory = remaining.substr(0, end);
                remaining = end == std::string_view::npos ? std::string_view {} : remaining.substr(end + 1);
                if (directory.empty())
                    continue;

                // Not const, so returning one moves rather than copies.
                auto candidate = (std::filesystem::path { directory } / std::filesystem::path { name }).string();

                // The SUFFIXED spelling is tried FIRST on Windows, and the order is
                // load-bearing rather than a preference. `ExecutableExists` cannot
                // see an execute bit there -- the filesystem has none -- so it
                // accepts any regular file, and an extensionless wrapper script
                // beside the real binary (ordinary in an MSYS2 or Cygwin bindir)
                // would otherwise win and yield a path nothing can launch. Windows
                // itself resolves a bare name through PATHEXT and never runs the
                // extensionless file, so this is what the OS does.
                //
                // `ExecutableSuffix` is empty on POSIX, so the block short-circuits
                // there rather than probing the same path twice; on Windows a name
                // that already carries the suffix does the same.
                if (!ExecutableSuffix.empty() && !name.ends_with(ExecutableSuffix))
                    if (auto suffixed = candidate + std::string { ExecutableSuffix }; ExecutableExists(suffixed))
                        return suffixed;

                if (ExecutableExists(candidate))
                    return candidate;
            }
            return std::nullopt;
        }
    };
} // namespace

std::string JoinPath(std::string_view directory, std::string_view relative)
{
    while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\'))
        directory.remove_suffix(1);

    std::string joined { directory };
    if (!relative.empty())
    {
        joined += '/';
        joined += relative;
    }
    std::ranges::replace(joined, '\\', '/');
    return joined;
}

std::unique_ptr<IToolchainHost> MakeToolchainHost()
{
    return std::make_unique<HostToolchainHost>();
}

} // namespace FastCache::Cc
