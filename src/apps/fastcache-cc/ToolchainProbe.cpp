// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "KeyDigest.hpp"
#include "Stats.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Platform/Environment.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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

namespace
{
    /// An input that is always present and always empty, for the verbose probe.
    ///
    /// The driver has to be given something to preprocess or it prints usage
    /// instead of a search list, and it must be empty so the run costs nothing.
    /// This is a genuine per-platform difference in what the OS provides, not two
    /// spellings of one thing, so it is a `#if` rather than a table row.
    [[nodiscard]] constexpr std::string_view NullInputPath() noexcept
    {
#if defined(_WIN32)
        return "NUL";
#else
        return "/dev/null";
#endif
    }

    /// This process's id, for a temp filename no concurrent writer will reuse.
    ///
    /// A `#if` for the same reason `NullInputPath` is one: the OSes genuinely
    /// provide this differently rather than spelling one call two ways. Used only
    /// to make a name unique -- nothing depends on the value -- so it needs no
    /// injected seam and a collision would cost a rewritten cache entry, not
    /// correctness.
    [[nodiscard]] std::uint64_t CurrentProcessId() noexcept
    {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }

    /// The environment variable an MSVC toolchain publishes its search list in.
    constexpr std::string_view MsvcIncludeVariable = "INCLUDE";

    /// Schema tag for the validity stamp.
    ///
    /// Separate from `FingerprintSchema`, because the two version different
    /// things: this one versions what the stamp COVERS. Adding an input to the
    /// stamp without moving it would let a cache entry written under the old rules
    /// validate under the new ones -- a stale fingerprint that looks fresh.
    constexpr std::string_view StampSchema = "toolchain-stamp-v1";

    /// A path's last-write time as its native tick count, or 0 when unreadable.
    ///
    /// The FULL resolution the filesystem offers, deliberately, and not truncated
    /// to seconds. The two error directions are not symmetric: a stamp that is too
    /// coarse fails to notice a change and serves a stale fingerprint, while one
    /// that is too fine merely recomputes something that had not changed. A
    /// one-second granularity would blind the stamp to everything a toolchain
    /// installer does within a second of the last read, which is exactly when an
    /// upgrade happens.
    ///
    /// `file_time_type`'s epoch and period are implementation-defined, so a
    /// standard-library update can change this value for an unchanged file. That
    /// costs one rewalk per toolchain and then re-stamps -- the harmless
    /// direction, which is why it is not worth defending against by discarding
    /// resolution. Filesystems whose timestamps really are second-granular (FAT,
    /// HFS+) simply get the coarser behaviour back; nothing here can improve on
    /// what the filesystem records.
    [[nodiscard]] std::int64_t LastWriteTicks(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto const stamp = std::filesystem::last_write_time(path, ec);
        if (ec)
            return 0;
        return static_cast<std::int64_t>(stamp.time_since_epoch().count());
    }

    /// Byte size of a file, or 0 when it cannot be read.
    [[nodiscard]] std::uint64_t FileSizeOrZero(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto const size = std::filesystem::file_size(path, ec);
        return ec ? 0 : size;
    }

    /// Where this compiler's cached fingerprint lives.
    ///
    /// Named by a digest of the compiler PATH rather than by the path itself: a
    /// path contains separators and characters no filename may carry, and two
    /// compilers can differ only in a component a sanitizer would flatten.
    /// @return The cache file path, or empty when there is no state directory.
    [[nodiscard]] std::filesystem::path CacheFilePath(std::string const& compiler)
    {
        auto const dir = StateDirectory();
        if (dir.empty())
            return {};

        KeyDigest name { "toolchain-cache-name-v1" };
        name.Field(compiler);

        std::error_code ec;
        auto const sub = dir / "toolchains";
        std::filesystem::create_directories(sub, ec);
        if (ec)
            return {};
        return sub / (name.ToHex() + ".fingerprint");
    }

    /// Write `stamp` and `fingerprint` so a concurrent reader sees both or neither.
    ///
    /// Temp file plus rename, because sixteen launchers on a cold cache all write
    /// this at once. A reader that caught a half-written file would either fail to
    /// parse it -- costing a needless 2-second rewalk -- or, worse, read a stamp
    /// paired with a truncated fingerprint and dispatch against a toolchain
    /// identity no other machine will ever produce.
    void WriteCacheAtomically(std::filesystem::path const& path, std::string_view stamp, std::string_view fingerprint)
    {
        std::error_code ec;
        // The temp name carries the pid so two writers do not share one temp file
        // and interleave into it; the rename is what makes the result atomic.
        auto const temp =
            path.parent_path() / (path.filename().string() + "." + std::to_string(CurrentProcessId()) + ".tmp");
        bool written = false;
        {
            std::ofstream out { temp, std::ios::binary | std::ios::trunc };
            if (out)
            {
                out << stamp << '\n' << fingerprint << '\n';
                out.flush();
                written = out.good();
            }
        }

        // Every path that does not end in a rename removes the temp file. Returning
        // early on a write failure instead -- which is what this did -- leaves one
        // behind per failure, in a directory nothing ever sweeps, so a machine with
        // a full disk or a permissions problem accumulates them indefinitely while
        // the fingerprint silently recomputes on every invocation.
        if (!written)
        {
            std::filesystem::remove(temp, ec);
            return;
        }

        std::filesystem::rename(temp, path, ec);
        if (ec)
            std::filesystem::remove(temp, ec);
    }

    /// Read a cache file written by `WriteCacheAtomically`.
    /// @return {stamp, fingerprint}, both empty when unreadable or malformed.
    [[nodiscard]] std::pair<std::string, std::string> ReadCache(std::filesystem::path const& path)
    {
        std::ifstream in { path, std::ios::binary };
        if (!in)
            return {};
        std::string stamp;
        std::string fingerprint;
        if (!std::getline(in, stamp) || !std::getline(in, fingerprint))
            return {};
        // A `\r` survives a file written on Windows and read with a text-mode
        // getline elsewhere; left on, the stamp never compares equal and the cache
        // silently never hits.
        for (auto* field: { &stamp, &fingerprint })
            if (!field->empty() && field->back() == '\r')
                field->pop_back();
        if (stamp.empty() || fingerprint.empty())
            return {};
        return { std::move(stamp), std::move(fingerprint) };
    }
} // namespace

std::string CompilerBanner(IProcessRunner& runner, std::string const& compiler)
{
    // Combined capture, because the drivers disagree about which stream this goes
    // to: clang and gcc print it on stdout, while `cl` with no input prints its
    // banner on stderr. Asking for both is what makes one call cover every driver
    // instead of a per-family rule that would need its own table row.
    std::array<std::string, 2> const probe { compiler, "--version" };
    auto const run = runner.RunCaptureCombined(probe);
    if (run.exitCode == 0 && !run.out.empty())
        return run.out.substr(0, run.out.find('\n'));

    // NORMALIZED, not the basename as spelled -- and that is the whole point of
    // this branch rather than a tidy-up of it.
    //
    // Only MSVC reaches here: `cl` has no `--version`, so it errors and every
    // MSVC fingerprint is built on this fallback. Returning the spelling meant the
    // digest depended on HOW the compiler was named rather than on which compiler
    // it is -- `cl` gave one identity and `C:\...\cl.exe` another. A worker is
    // configured with a path and a build system may invoke the bare name, so the
    // two computed different fingerprints and the scheduler matched nothing:
    // "rejected (no-worker): no worker matches this toolchain", on a fleet where
    // both ends were pointed at the same compiler. Measured in CI, and reproduced
    // here with a compiler that refuses `--version` under two spellings.
    //
    // Lowercased and de-suffixed exactly as `ClassifyCompiler` does, so "which
    // driver is this" and "what do we call it" cannot disagree about `CL.EXE`.
    auto const slash = compiler.find_last_of("/\\");
    auto base = slash == std::string::npos ? compiler : compiler.substr(slash + 1);
    std::ranges::transform(base, base.begin(), [](char c) { return PathCanon::AsciiLower(c); });
    if (base.ends_with(".exe"))
        base.resize(base.size() - 4);
    return base;
}

std::vector<std::string> DiscoverIncludePaths(IProcessRunner& runner, std::string const& compiler, DriverSpec const& spec)
{
    // No `default:`, so a mechanism added to the table fails to compile here
    // rather than silently returning nothing -- which would present as a
    // fingerprint that quietly stopped covering the include tree.
    switch (spec.includeDiscovery)
    {
        case IncludeDiscovery::None:
            return {};

        case IncludeDiscovery::GnuVerbose: {
            std::vector<std::string> argv;
            argv.reserve(spec.includeProbeFlags.size() + 2);
            argv.emplace_back(compiler);
            for (auto const& flag: spec.includeProbeFlags)
                argv.emplace_back(flag);
            argv.emplace_back(NullInputPath());

            auto const run = runner.RunCaptureSplit(argv);
            // The exit code is deliberately NOT checked. The list is printed
            // before anything that could fail, and a driver can exit non-zero for
            // reasons that leave it perfectly valid -- a missing SDK component, a
            // warning promoted by a wrapper script. Parsing decides whether the
            // output is usable; an exit code cannot.
            //
            // Read from stderr, which is where every GNU-family driver prints it.
            return ParseGnuIncludeSearchPaths(run.err);
        }

        case IncludeDiscovery::MsvcEnvironment: {
            auto const value = ReadEnvironmentVariable(std::string { MsvcIncludeVariable });
            if (!value.has_value())
                return {};
            return ParseIncludeEnvironment(*value);
        }
    }
    return {};
}

std::string ComputeToolchainStamp(std::string_view banner, std::string const& compiler, std::span<std::string const> roots)
{
    std::filesystem::path const binary { compiler };
    auto const size = FileSizeOrZero(binary);
    auto const mtime = LastWriteTicks(binary);
    if (size == 0 && mtime == 0)
        // Not stattable: a bare `cc` resolved through PATH, or a wrapper that is
        // not a file. Refusing to stamp means refusing to cache, which costs a
        // rewalk rather than risking a stamp that cannot detect any change.
        return {};

    KeyDigest digest { StampSchema };
    digest.Field(banner);
    digest.Path(compiler);
    digest.Field(std::to_string(size));
    digest.Field(std::to_string(mtime));
    for (auto const& root: roots)
    {
        // The root's own mtime changes when an entry is added or removed in it.
        // Both the path and the time are folded, so a root REPLACED by one with
        // the same timestamp still restamps.
        digest.Path(root);
        digest.Field(std::to_string(LastWriteTicks(std::filesystem::path { root })));
    }
    return digest.ToHex();
}

std::string CachedToolchainFingerprint(
    IProcessRunner& runner, std::string const& compiler, std::string_view banner, DriverSpec const& spec, bool forceRefresh)
{
    auto const roots = DiscoverIncludePaths(runner, compiler, spec);
    auto const stamp = ComputeToolchainStamp(banner, compiler, roots);
    auto const cachePath = CacheFilePath(compiler);

    if (!forceRefresh && !stamp.empty() && !cachePath.empty())
    {
        auto const [cachedStamp, cachedFingerprint] = ReadCache(cachePath);
        if (!cachedStamp.empty() && cachedStamp == stamp)
            return cachedFingerprint;
    }

    // The expensive part, reached only on a miss or a forced refresh.
    auto const fingerprint = ComputeToolchainFingerprint(banner, ProbeToolchainFiles(roots));

    if (!stamp.empty() && !cachePath.empty())
        WriteCacheAtomically(cachePath, stamp, fingerprint);

    return fingerprint;
}

} // namespace FastCache::Cc
