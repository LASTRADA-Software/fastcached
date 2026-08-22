// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "ToolchainProbe.hpp"

#include <filesystem>
#include <system_error>

namespace FastCache::Cc
{

namespace
{
    /// The literal the driver prints before its system include list.
    constexpr std::string_view SearchListBegin = "#include <...> search starts here:";
    /// The literal that closes it.
    constexpr std::string_view SearchListEnd = "End of search list.";
    /// Suffix a driver appends to a macOS framework search path.
    constexpr std::string_view FrameworkSuffix = " (framework directory)";

    /// Trim ASCII spaces and tabs from both ends.
    ///
    /// Spaces and tabs only, and `\r` handled by the caller: this runs over lines
    /// already split on `\n`, and a driver's own indentation is the only leading
    /// whitespace it has to cope with.
    [[nodiscard]] std::string_view Trim(std::string_view text) noexcept
    {
        auto const first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return {};
        auto const last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }
} // namespace

std::vector<std::string> ParseGnuIncludeSearchPaths(std::string_view verboseOutput)
{
    std::vector<std::string> paths;
    bool inList = false;

    for (std::size_t pos = 0; pos <= verboseOutput.size();)
    {
        auto const newline = verboseOutput.find('\n', pos);
        auto line = verboseOutput.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
        pos = newline == std::string_view::npos ? verboseOutput.size() + 1 : newline + 1;

        // A capture taken on Windows, or piped through a tool that rewrote the
        // line endings, carries a trailing `\r`. Left on, it becomes part of the
        // last path and every root test against it fails -- silently, since a
        // path that does not exist is skipped rather than reported.
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        auto const trimmed = Trim(line);
        if (trimmed == SearchListBegin)
        {
            inList = true;
            continue;
        }
        if (trimmed == SearchListEnd)
            // Returns rather than merely clearing the flag: a driver prints one
            // list, and continuing would let a second "search starts here" later
            // in the output (an inner invocation the driver echoes) append paths
            // that are not this compile's.
            return paths;
        if (!inList || trimmed.empty())
            continue;

        auto entry = trimmed;
        if (entry.ends_with(FrameworkSuffix))
            entry.remove_suffix(FrameworkSuffix.size());
        entry = Trim(entry);
        if (!entry.empty())
            paths.emplace_back(entry);
    }

    // Reaching here means the closing marker never arrived -- a truncated capture
    // or a driver that failed partway. What was collected is returned rather than
    // discarded: a partial list still identifies the toolchain more precisely than
    // the banner alone, and the alternative is silently falling back to nothing.
    return paths;
}

std::vector<std::string> ParseIncludeEnvironment(std::string_view value)
{
    std::vector<std::string> paths;
    for (std::size_t pos = 0; pos <= value.size();)
    {
        auto const sep = value.find(';', pos);
        auto const entry = Trim(value.substr(pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos));
        pos = sep == std::string_view::npos ? value.size() + 1 : sep + 1;
        if (!entry.empty())
            paths.emplace_back(entry);
    }
    return paths;
}

std::vector<ToolchainFile> ProbeToolchainFiles(std::span<std::string const> roots)
{
    std::vector<ToolchainFile> files;

    for (auto const& root: roots)
    {
        std::error_code ec;
        auto const base = std::filesystem::path { root };
        if (!std::filesystem::is_directory(base, ec) || ec)
            continue;

        // `skip_permission_denied` because a search path the driver lists is not
        // necessarily one this process can read all of, and one unreadable
        // subdirectory must not cost the whole fingerprint. Every call takes an
        // error_code: a toolchain tree can contain a broken symlink, and the
        // throwing overloads would turn that into an exception on a path whose
        // whole job is to degrade quietly.
        auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it { base, options, ec };
        if (ec)
            continue;

        std::filesystem::recursive_directory_iterator const end;
        for (; it != end; it.increment(ec))
        {
            if (ec)
                break;

            // `is_regular_file`, so a directory symlink loop cannot be followed
            // and a device node is not read. The iterator does not follow
            // directory symlinks by default, which is what keeps an SDK's
            // `Current -> A` framework links from being walked twice.
            if (!it->is_regular_file(ec) || ec)
            {
                ec.clear();
                continue;
            }

            auto const relative = std::filesystem::relative(it->path(), base, ec);
            if (ec)
            {
                ec.clear();
                continue;
            }

            auto hash = HashFileContents(it->path().string());
            if (hash.empty())
                // Unreadable. Skipped rather than recorded as empty: an entry
                // whose hash is "" would make two DIFFERENT unreadable files look
                // identical, which is a false match in the one direction that
                // dispatches to the wrong toolchain.
                continue;

            // `/`-separated, always. The relative path is part of the digest, and
            // `std::filesystem` spells it with the HOST's preferred separator --
            // so a Windows machine and a POSIX machine holding byte-identical
            // toolchains would otherwise derive different fingerprints and refuse
            // to share work, which is the exact failure this relativization exists
            // to prevent.
            auto spelling = relative.generic_string();
            files.emplace_back(ToolchainFile { .relativePath = std::move(spelling), .contentHash = std::move(hash) });
        }
    }

    return files;
}

} // namespace FastCache::Cc
