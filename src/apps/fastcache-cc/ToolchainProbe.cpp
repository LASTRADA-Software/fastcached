// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "IParallelFor.hpp"
#include "KeyDigest.hpp"
#include "Stats.hpp"
#include "ToolchainProbe.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
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

ToolchainFileScan ProbeToolchainFiles(std::span<std::string const> roots, IParallelFor& parallel)
{
    ToolchainFileScan scan;

    // Two phases, and the split is what keeps this reviewable. Enumeration stays
    // exactly as it was -- serial, single-threaded, every completeness signal
    // unchanged -- and only the CONTENT HASHING is spread out. That is also where
    // the cost is: 4,771 files and 345 MB for one Windows toolchain, at 5.00 ms per
    // file cold against 0.21 ms warm, so what is being overlapped is the per-file
    // open, not the digesting.
    struct Pending
    {
        std::string absolute; ///< What to open.
        std::string spelling; ///< Its `/`-separated path relative to its root.
    };
    std::vector<Pending> pending;

    for (auto const& root: roots)
    {
        // A root the DRIVER printed, so its bytes are the driver's encoding and
        // not necessarily this process's -- and a `std::filesystem::path` built
        // from bytes this process cannot read THROWS, before the `error_code`
        // below is ever consulted. A root that cannot be read is one this
        // fingerprint does not cover, exactly as an absent one is.
        //
        // NOT a completeness signal, unlike every I/O failure below, and the
        // difference is determinism. Whether these bytes decode is a property of the
        // bytes and of this project's own narrow encoding, which every executable
        // here pins to UTF-8 -- so a launcher and a worker reach the SAME answer and
        // digest the same narrower tree, which still matches. The failures below are
        // accidents of one moment on one machine, and that is what makes them worth
        // refusing over.
        auto const base = PathFromNarrowText(root);
        if (!base.has_value())
            continue;

        // Keyed on the resolved TYPE rather than on `ec`, and that distinction is the
        // whole correctness of this test. An absent root sets `ec` on some standard
        // libraries and clears it on others -- MSVC reports `no_such_file_or_directory`
        // for a path that is merely not there -- so "`ec` is set" would have called
        // every machine without `/usr/local/include` incomplete and refused its
        // toolchain. `file_type::none` is the one answer that means the status could
        // not be determined at all; `not_found` is a real answer and an ordinary one.
        std::error_code ec;
        auto const status = std::filesystem::status(*base, ec);
        if (status.type() == std::filesystem::file_type::none)
        {
            scan.complete = false;
            continue;
        }
        if (!std::filesystem::is_directory(status))
            // Not there, or there and not a directory. A driver lists search paths it
            // would use IF they existed, so this is a layout this install does not
            // have rather than content that failed to arrive.
            continue;

        // `skip_permission_denied` because a search path the driver lists is not
        // necessarily one this process can read all of, and one unreadable
        // subdirectory must not cost the whole fingerprint. Every call takes an
        // error_code: a toolchain tree can contain a broken symlink, and the
        // throwing overloads would turn that into an exception on a path whose
        // whole job is to degrade quietly.
        auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it { *base, options, ec };
        if (ec)
        {
            // The root IS a directory and could not be opened. Nothing under it
            // reached the digest, and nothing about the stamp records that.
            scan.complete = false;
            continue;
        }

        std::filesystem::recursive_directory_iterator const end;
        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                // The walk stopped PARTWAY, so what was collected is a prefix of
                // this root rather than the root. The most valuable of these
                // signals: everything after the failing entry is missing, and the
                // digest is short by an unknown amount.
                scan.complete = false;
                break;
            }

            // `is_regular_file`, so a directory symlink loop cannot be followed
            // and a device node is not read. The iterator does not follow
            // directory symlinks by default, which is what keeps an SDK's
            // `Current -> A` framework links from being walked twice.
            // NOT a completeness signal, and that exclusion is deliberate. This
            // query fails on a dangling symlink, which the comment above records as
            // an ordinary thing to find in a toolchain tree -- so treating its
            // failure as a gap would refuse toolchains that are entirely fine, which
            // is a worse error than the one being caught.
            if (!it->is_regular_file(ec) || ec)
            {
                ec.clear();
                continue;
            }

            auto const relative = std::filesystem::relative(it->path(), *base, ec);
            if (ec)
            {
                // A file that was found and cannot be NAMED relative to its root has
                // no spelling the digest can carry, so it is content that silently
                // did not arrive.
                scan.complete = false;
                ec.clear();
                continue;
            }

            // `/`-separated, always. The relative path is part of the digest, and
            // `std::filesystem` spells it with the HOST's preferred separator --
            // so a Windows machine and a POSIX machine holding byte-identical
            // toolchains would otherwise derive different fingerprints and refuse
            // to share work, which is the exact failure this relativization exists
            // to prevent.
            // Recorded now, hashed below. Nothing about WHICH files are covered
            // changes here -- only when their bytes are read.
            pending.emplace_back(Pending { .absolute = it->path().string(), .spelling = relative.generic_string() });
        }
    }

    // `char` and not `bool`: `std::vector<bool>` is bit-packed, so two slices
    // writing adjacent entries would write the same word. That is a genuine data
    // race rather than a theoretical one, and it is invisible in the result.
    std::vector<ToolchainFile> hashed(pending.size());
    std::vector<char> unreadable(pending.size(), 0);

    // One slice per file rather than per root. The roots are wildly unequal -- an
    // MSVC `include` against `/usr/local/include` -- so a slice per root would leave
    // one thread holding the SDK while the rest had finished.
    auto const everySliceRan = parallel.Run(pending.size(), [&](std::size_t index) {
        auto hash = HashFileContents(pending[index].absolute);
        if (hash.empty())
        {
            // Unreadable. Skipped rather than recorded as empty: an entry whose
            // hash is "" would make two DIFFERENT unreadable files look identical,
            // which is a false match in the one direction that dispatches to the
            // wrong toolchain.
            //
            // And reported, because this is a REGULAR FILE that is there and whose
            // bytes did not arrive -- a scanner holding a header, a permission, a
            // share violation. It is the likeliest of these on Windows and the one
            // a stamp is blindest to.
            unreadable[index] = 1;
            return;
        }
        hashed[index] = ToolchainFile { .relativePath = pending[index].spelling, .contentHash = std::move(hash) };
    });

    // `complete` is the AND of three things, and dropping any one of them writes a
    // TRUNCATED identity under a stamp that still validates -- forever, because the
    // stamp folds each root's path and mtime and never its contents. Enumeration
    // already cleared it for a root that could not be read or a walk that stopped;
    // this adds the files whose bytes did not arrive, and the slices that did not
    // finish at all.
    if (!everySliceRan)
        scan.complete = false;

    scan.files.reserve(pending.size());
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        if (unreadable[index] != 0)
        {
            scan.complete = false;
            continue;
        }
        scan.files.emplace_back(std::move(hashed[index]));
    }

    return scan;
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

    /// Build a probe invocation: the compiler, its table row's flags, an empty input.
    ///
    /// Shared by the two probes that ask a driver a question over a file that is not
    /// really there, because the five lines were otherwise identical but for which
    /// span they read -- and a copy that drifts is how one probe silently stops
    /// appending an input the other still does.
    ///
    /// The input is appended here rather than carried in the table for the reason the
    /// table's own columns give: "empty and always exists" has no portable spelling.
    ///
    /// @param compiler The driver to run.
    /// @param flags Its probe flags.
    /// @return The full argv, input included.
    [[nodiscard]] std::vector<std::string> ProbeArgv(std::string const& compiler, std::span<std::string_view const> flags)
    {
        std::vector<std::string> argv;
        argv.reserve(flags.size() + 2);
        argv.emplace_back(compiler);
        for (auto const& flag: flags)
            argv.emplace_back(flag);
        argv.emplace_back(NullInputPath());
        return argv;
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

    /// The registry key recording which Windows SDKs are installed and where.
    constexpr std::string_view InstalledRootsKey = R"(SOFTWARE\Microsoft\Windows Kits\Installed Roots)";

    /// The value naming the Windows 10/11 SDK's install prefix.
    constexpr std::string_view KitsRootValue = "KitsRoot10";

    /// The directory name a `VC\Tools\MSVC\<version>` toolset sits directly under.
    ///
    /// Matched case-insensitively, and this is what locates the toolset root
    /// instead of counting directory levels: `bin\Hostx64\x64` is three levels but
    /// a cross-targeting `bin\Hostx64\arm64` toolset is the same depth today and
    /// there is nothing promising it stays that way.
    constexpr std::string_view MsvcToolsetParent = "msvc";

    /// How far above the compiler the toolset root may be.
    ///
    /// Four is the real depth (`<ver>/bin/Host<a>/<b>/cl.exe`); six leaves room for
    /// a layout with one more level without letting the walk escape into
    /// `C:\Program Files` and match something that merely happens to be called
    /// MSVC.
    constexpr int MaxToolsetAncestors = 6;

    /// The include directories a Windows SDK kit is made of, in the order
    /// `vcvarsall` lists them in `INCLUDE`.
    ///
    /// A table so the next one the SDK grows is a row. The order does not reach the
    /// fingerprint -- that sorts -- but it is what an operator compares against
    /// their own `INCLUDE` when a fingerprint disagrees.
    constexpr std::array<std::string_view, 5> WindowsKitIncludeSubdirectories {
        "ucrt", "um", "shared", "winrt", "cppwinrt"
    };

    /// The VC include directories relative to a toolset root, in `INCLUDE` order.
    constexpr std::array<std::string_view, 2> MsvcToolsetIncludeSubdirectories { "include", "atlmfc/include" };

    /// What a clang driver is asked for the directory its own headers live in.
    ///
    /// Asking beats deriving here, and the difference is not style. The resource
    /// tree is `<prefix>/lib/clang/<version>/`, and neither half is reliably
    /// recoverable from the driver's path: `/usr/bin/clang-cl-20` has `/usr` as its
    /// prefix, whose `lib/clang` on an ordinary Debian holds `20`, `20.1.2`, `22`
    /// and `22.1.8` -- four candidates, of which "the newest" is the wrong one.
    /// The driver knows, and says so on stdout, exactly once.
    ///
    /// Accepted in cl-driver mode as well as GNU mode, which is what makes one
    /// spelling serve `clang-cl`: measured against clang-cl 20 and 22, both
    /// answering their own `/usr/lib/llvm-<v>/lib/clang/<v>`.
    constexpr std::string_view ClangResourceDirFlag = "-print-resource-dir";

    /// The header directory inside a resource tree.
    ///
    /// The tree also holds `lib` and `share`, which a compile links against rather
    /// than includes -- folding those in would put a machine's link-time artefacts
    /// into an identity that answers "what will this compile like". Its presence is
    /// also what tells a real answer from a driver that did not understand the
    /// question.
    constexpr std::string_view ClangResourceIncludeSubdirectory = "include";

    /// What marks a driver-printed line as the FRONTEND invocation.
    constexpr std::string_view FrontendMarker = "-cc1";

    /// The frontend option whose value is the target triple.
    constexpr std::string_view TripleOption = "-triple";

    /// The header a GNU driver prints its own target on.
    constexpr std::string_view TargetHeaderPrefix = "Target:";

    /// The first line of @p text that @p extract gets an answer out of.
    ///
    /// Shared by the two target readers rather than written out twice, and the
    /// trailing carriage return is why it earns its keep: a capture taken on Windows,
    /// or piped through a tool that rewrote the line endings, carries one, and
    /// stripping it is the step whose omission fails SILENTLY -- the shape check
    /// rejects an otherwise perfect answer and the probe reports nothing at all.
    ///
    /// By value rather than by forwarding reference, and that is a correctness choice
    /// rather than a style one: @p extract is invoked once per LINE, so forwarding it
    /// would move from it on the first iteration and leave every later call reading a
    /// moved-from object. The standard algorithms take a callable by value for the
    /// same reason.
    ///
    /// @param text The driver output to walk.
    /// @param extract Called per line; returns the answer, or empty to keep looking.
    /// @return The first non-empty answer, or empty when no line had one.
    template <typename Extract>
    [[nodiscard]] std::string FirstLineAnswering(std::string_view text, Extract extract)
    {
        for (std::size_t pos = 0; pos <= text.size();)
        {
            auto const newline = text.find('\n', pos);
            auto line = text.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
            pos = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            if (auto answer = extract(line); !answer.empty())
                return answer;
        }
        return {};
    }

    /// The longest triple worth believing.
    ///
    /// A real one runs to about thirty characters (`x86_64-pc-windows-msvc19.51.36252`).
    /// The ceiling is what stops a mis-parse from folding an entire command line into
    /// a cache key.
    constexpr std::size_t MaxTargetTripleLength = 128;

    /// Split one driver-printed command line into its arguments, honouring quotes.
    ///
    /// clang quotes every token it prints, so a Windows path arrives as
    /// `"C:\Program Files\...\clang-cl.exe"` -- ONE argument containing a space.
    /// Splitting on whitespace alone would break it in two and shift every token after
    /// it, and the triple is read POSITIONALLY (the argument following `-triple`). A
    /// shift therefore does not fail loudly: it returns a neighbouring token that looks
    /// like an answer.
    ///
    /// Backslashes are left as they are rather than unescaped. Nothing here needs the
    /// original path back, and a triple has no backslash to unescape.
    ///
    /// @param line One line of driver output.
    /// @return Its arguments, quotes stripped, as views into @p line.
    [[nodiscard]] std::vector<std::string_view> SplitDriverLine(std::string_view line)
    {
        std::vector<std::string_view> tokens;
        for (std::size_t pos = 0; pos < line.size();)
        {
            if (line[pos] == ' ' || line[pos] == '\t')
            {
                ++pos;
                continue;
            }
            if (line[pos] == '"')
            {
                auto const start = pos + 1;
                auto const close = line.find('"', start);
                tokens.push_back(
                    line.substr(start, close == std::string_view::npos ? std::string_view::npos : close - start));
                pos = close == std::string_view::npos ? line.size() : close + 1;
                continue;
            }
            auto const end = line.find_first_of(" \t", pos);
            tokens.push_back(line.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
            pos = end == std::string_view::npos ? line.size() : end;
        }
        return tokens;
    }

    /// Whether a token could be a target triple.
    ///
    /// Validated rather than trusted, because this value becomes BOTH a cache key
    /// input and a command-line argument. A mis-parse that returned a path would split
    /// the fleet's keys by install location and hand a worker an argument its own
    /// filter has to refuse -- one silent, one loud, neither of them the answer. A
    /// triple is letters, digits, dots, underscores and dashes, with at least one dash
    /// separating its components -- and it never LEADS with one, which is the check
    /// that carries the weight here. `-triple` is read positionally, so a driver line
    /// carrying the option with no value leaves the next FLAG in the value's place,
    /// and `-emit-obj` satisfies every other rule on this list. The result would be a
    /// cache key and a `--target=` argument built out of somebody else's option.
    ///
    /// @param token The candidate.
    /// @return True when it is shaped like a triple.
    [[nodiscard]] bool LooksLikeTargetTriple(std::string_view token) noexcept
    {
        return !token.empty() && !token.starts_with('-') && token.size() <= MaxTargetTripleLength && token.contains('-')
               && std::ranges::all_of(token, [](char c) {
                      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'
                             || c == '_' || c == '-';
                  });
    }

    /// Where the shared VS headers live, relative to the Visual Studio root.
    ///
    /// Present in a developer prompt's `INCLUDE` and easy to miss: leaving it out
    /// would make the layout-derived root set differ from the environment-derived
    /// one by exactly one directory, and a fingerprint that differs by one
    /// directory matches nothing while looking entirely reasonable.
    constexpr std::string_view VisualStudioSharedInclude = "VC/Auxiliary/VS/include";

    /// Order two dotted versions by their numeric components.
    ///
    /// Numerically rather than lexicographically, because the SDK's own numbering
    /// makes the difference load-bearing: `10.0.9` sorts ABOVE `10.0.22621.0` as
    /// text, so a string compare picks a kit years out of date and derives the
    /// fingerprint from headers the compiler will not use.
    ///
    /// @param left One version.
    /// @param right The other.
    /// @return True when @p left is older than @p right.
    [[nodiscard]] bool VersionLess(std::string_view left, std::string_view right)
    {
        auto next = [](std::string_view& text) -> unsigned long long {
            auto const dot = text.find('.');
            auto const head = text.substr(0, dot);
            text = dot == std::string_view::npos ? std::string_view {} : text.substr(dot + 1);
            unsigned long long value = 0;
            std::from_chars(head.data(), head.data() + head.size(), value);
            return value;
        };

        while (!left.empty() || !right.empty())
        {
            auto const leftPart = next(left);
            auto const rightPart = next(right);
            if (leftPart != rightPart)
                return leftPart < rightPart;
        }
        return false;
    }

    /// Append @p relative to @p root when the directory is really there.
    /// @param host The machine's filesystem.
    /// @param roots Where to append.
    /// @param root The prefix.
    /// @param relative What to hang under it.
    void AppendIfDirectory(IToolchainHost& host,
                           std::vector<std::string>& roots,
                           std::string_view root,
                           std::string_view relative)
    {
        auto joined = JoinPath(root, relative);
        if (host.DirectoryExists(joined))
            roots.push_back(std::move(joined));
    }

    /// The `INCLUDE` variable's search paths, read through the injected host.
    /// @param host The machine's environment.
    /// @return The search paths, in order; empty when the variable is unset.
    [[nodiscard]] std::vector<std::string> IncludeEnvironmentRoots(IToolchainHost& host)
    {
        auto const value = host.Environment(MsvcIncludeVariable);
        if (!value.has_value())
            return {};
        return ParseIncludeEnvironment(*value);
    }

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

std::vector<std::string> VersionProbeCommand(std::string const& compiler)
{
    // The flags come from the driver table, because "how is this driver asked its
    // version" is a per-driver fact and `cl` has no `--version` at all. Classified
    // HERE rather than taken as a parameter: the launcher and the compile node both
    // reach this, and a spec passed in is a second chance for the two to disagree
    // about one compiler -- which is invisible from both ends, as a scheduler that
    // simply never matches. See `DriverSpec::versionFlags`.
    auto const& driver = DriverOf(ClassifyCompiler(compiler));
    std::vector<std::string> probe;
    probe.reserve(1 + driver.versionFlags.size());
    probe.emplace_back(compiler);
    for (auto const flag: driver.versionFlags)
        probe.emplace_back(flag);
    return probe;
}

std::string CompilerBanner(IProcessRunner& runner, std::string const& compiler)
{
    auto const probe = VersionProbeCommand(compiler);

    // Combined capture, because the drivers disagree about which stream this goes
    // to: clang and gcc print it on stdout, while `cl` prints its banner on stderr.
    // Asking for both is what makes one call cover every driver instead of a
    // per-family rule that would need its own table row.
    //
    // And asked IN ENGLISH, because since issue #195 this line is the compiler's
    // identity -- it is folded into `ComputeKey`, `ComputeManifestKey` and
    // `ComputeToolchainFingerprint`. `cl` localizes it, so one MSVC toolset under two
    // Visual Studio UI languages produced two identities: two machines holding the
    // same compiler shared no cache entry and never matched each other in the fleet,
    // and both symptoms are what a healthy estate looks like from outside -- a lower
    // hit rate and a `NoWorker` (issue #200).
    //
    // `VSLANG` rather than parsing the banner. No rule over "the version-looking
    // token and the last one" survives a locale nobody here has read, and the
    // alternative to a rule is a per-language table that is wrong the moment a pack
    // ships. Asking the compiler to speak English needs neither.
    //
    // Set for EVERY driver rather than only the MSVC family, and that is the
    // deliberate choice: gcc and clang ignore a variable they do not know, so the
    // cost is nothing -- while gating it on `ClassifyCompiler` would make a
    // compiler's IDENTITY depend on a classification that can itself be wrong, and a
    // misclassification would then silently move a cache key rather than merely
    // pick the wrong probe flag.
    //
    // Best effort by nature: `cl` falls back to whatever language pack IS installed
    // when 1033 is absent, so a machine can still answer in another language.
    //
    // What that leaves is stated rather than smoothed over, because the comfortable
    // version of this sentence is wrong. It is NOT true that such a host is "as
    // consistent with itself as before": before this, two hosts sharing a UI language
    // matched each other. An estate whose machines differ only in which packs are
    // INSTALLED -- German UI throughout, en-US present on some -- now splits into an
    // English identity and a German one that share no cache entry and match nothing
    // in the fleet. That is #200's own symptom, newly introduced for exactly that
    // shape of estate, and it is the cost of collapsing the much commoner split
    // between whole machines.
    //
    // And unlike the `/showIncludes` twin, this path has NO detector. There the
    // launcher can see that a request did not take, because it parses the answer and
    // `Cc::CarriesUnreadableIncludeNotes` reports notes it could not read. A banner
    // is one opaque line: nothing here can tell "this is English" from "this is a
    // language that happens to be shaped like the last one", short of a second probe
    // spawn per compile to compare against. So the residual case is silent by
    // construction, which is the half most likely to be rediscovered later and
    // "fixed".
    //
    // Argued in full under `## Accepted trade-offs` in `.agent/rules/compile-cache.md`,
    // and that is where it has to live rather than only here: nobody debugging a
    // split estate is reading this file. They are reading the rulebook, because that
    // is where this project keeps its arguments -- and a cost recorded only beside
    // the code is one only somebody who already knows where to look will find.
    std::array<EnvironmentAssignment, 1> const english { {
        { .name = "VSLANG", .value = "1033" },
    } };
    auto const run = runner.RunCaptureCombined(probe, english);
    if (run.exitCode == 0 && !run.out.empty())
    {
        auto line = run.out.substr(0, run.out.find('\n'));
        // A Windows child writes CRLF, and this line becomes a cache key input and
        // a fingerprint input. Left on, the identity would carry a byte describing
        // the HOST's line-ending convention rather than the compiler -- the same
        // reason `ReadCache` below trims one. Measured: `cl` ends its banner with
        // CRLF, `clang-cl` with LF, so this separates two things that are the same
        // and nothing else.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return line;
    }

    // NORMALIZED, not the basename as spelled -- and that is the whole point of
    // this branch rather than a tidy-up of it.
    //
    // Reached now only by a driver that cannot be run at all, or answers nothing:
    // the table gives every driver a probe it exits ZERO from, which is what took
    // MSVC off this branch (issue #195 -- every `cl` identified as the string `cl`,
    // so one toolset's object was served to another's compile). Returning the
    // spelling meant the digest depended on HOW the compiler was named rather than
    // on which compiler it is -- `cl` gave one identity and `C:\...\cl.exe`
    // another. A worker is configured with a path and a build system may invoke the
    // bare name, so the two computed different fingerprints and the scheduler
    // matched nothing: "rejected (no-worker): no worker matches this toolchain", on
    // a fleet where both ends were pointed at the same compiler.
    //
    // Lowercased and de-suffixed exactly as `ClassifyCompiler` does, so "which
    // driver is this" and "what do we call it" cannot disagree about `CL.EXE`.
    return NormalizedCompilerName(compiler);
}

std::string ToolchainLabel(std::string_view compiler, std::string_view banner)
{
    auto tool = NormalizedCompilerName(compiler);
    if (tool.empty())
        return {};

    // The first token that STARTS with a digit, with trailing punctuation trimmed.
    //
    // Starting with one is what makes this safe, and skipping leading characters
    // instead was a real bug: a GNU driver prints its own name first, so
    // `g++-13 (Ubuntu 13.3.0-6ubuntu2) 13.3.0` would have had `g++-13` stripped down
    // to `13` and been labelled `g++-13 13` -- and two nodes on 13.2.0 and 13.3.0
    // would then have rendered identically, which is the confusion this whole feature
    // exists to remove. `aarch64-none-elf-gcc` became `64` the same way, and a
    // version-suffixed driver is a first-class discovered case rather than a curiosity.
    //
    // A dot is required as well, so a stray number in a banner -- a year, a word size
    // -- is not mistaken for a version. The cost is that a compiler whose version has
    // no dot goes unlabelled, which is the safe direction: the tool's name alone is
    // still true, while a wrong number is worse than none.
    for (auto const word: std::views::split(banner, ' '))
    {
        std::string_view token { word.begin(), word.end() };
        if (token.empty() || std::isdigit(static_cast<unsigned char>(token.front())) == 0)
            continue;
        while (!token.empty() && std::isdigit(static_cast<unsigned char>(token.back())) == 0)
            token.remove_suffix(1);
        if (LooksLikeVersion(token) && token.contains('.'))
            return std::format("{} {}", tool, token);
    }

    // No version in the banner is not a failure. `cl` on a locale that translates its
    // banner, a wrapper that prints its own name, and the basename fallback
    // `CompilerBanner` returns for a compiler that could not be run all land here --
    // and the tool's name alone still tells two DIFFERENT compilers apart, which is
    // more than the fingerprint alone was doing.
    return tool;
}

bool LooksLikeVersion(std::string_view name) noexcept
{
    return !name.empty() && std::ranges::any_of(name, [](char c) { return c >= '0' && c <= '9'; })
           && std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

std::vector<std::string> MsvcToolsetIncludeRoots(IToolchainHost& host, std::string const& compiler)
{
    // Resolved first, so a bare `cl` from a build system and an absolute path from
    // a worker's configuration reach the same layout -- the rule the whole feature
    // turns on, since a disagreement here is a fingerprint disagreement and those
    // are invisible from both ends.
    auto const resolved = host.ResolveOnSearchPath(compiler);
    if (!resolved.has_value())
        return {};

    // Walk up looking for the `MSVC/<version>` pair rather than counting levels;
    // `directory` starts at the compiler itself so its first parent is the bindir.
    std::filesystem::path directory { *resolved };
    std::filesystem::path toolset;
    for ([[maybe_unused]] auto const level: std::views::iota(0, MaxToolsetAncestors))
    {
        auto const parent = directory.parent_path();
        if (parent.empty() || parent == directory)
            break;
        if (PathCanon::AsciiLower(parent.parent_path().filename().string()) == MsvcToolsetParent)
        {
            toolset = parent;
            break;
        }
        directory = parent;
    }
    if (toolset.empty())
        return {};

    // `include` is what makes this a toolset rather than a directory that happens
    // to sit under one called MSVC -- and without it there is nothing to fingerprint
    // anyway, so falling back to `INCLUDE` is strictly better than half an answer.
    auto const toolsetRoot = toolset.generic_string();
    if (!host.DirectoryExists(JoinPath(toolsetRoot, MsvcToolsetIncludeSubdirectories.front())))
        return {};

    std::vector<std::string> roots;
    for (auto const& relative: MsvcToolsetIncludeSubdirectories)
        AppendIfDirectory(host, roots, toolsetRoot, relative);

    // `<vs>/VC/Auxiliary/VS/include` -- four levels above the toolset root, which
    // is `<vs>/VC/Tools/MSVC/<version>`.
    auto const visualStudioRoot = toolset.parent_path().parent_path().parent_path().parent_path();
    if (!visualStudioRoot.empty())
        AppendIfDirectory(host, roots, visualStudioRoot.generic_string(), VisualStudioSharedInclude);

    return roots;
}

std::vector<std::string> WindowsKitIncludeRoots(IToolchainHost& host)
{
    // The 32-bit view first and the native one as a fallback: `Installed Roots` is
    // written by a 32-bit installer, so it lands in `WOW6432Node` and a native read
    // from this 64-bit process finds nothing at all -- silently, as a kit the
    // machine has that discovery never sees.
    auto kitsRoot =
        host.RegistryString(RegistryHive::LocalMachine, InstalledRootsKey, KitsRootValue, RegistryView::ThirtyTwoBit);

    // Present-and-EMPTY counts as absent for that fallback, which is the third
    // answer `RegistryString` can give and the easy one to miss: a zero-length
    // value comes back as an empty string rather than as `nullopt`. Testing only
    // `has_value()` skipped the native read on a machine whose 32-bit value had
    // been emptied, and dropped the SDK half of its identity in silence.
    if (!kitsRoot.has_value() || kitsRoot->empty())
        kitsRoot = host.RegistryString(RegistryHive::LocalMachine, InstalledRootsKey, KitsRootValue, RegistryView::Native);
    if (!kitsRoot.has_value() || kitsRoot->empty())
        return {};

    auto const includeRoot = JoinPath(*kitsRoot, "Include");

    // The DIRECTORIES are the source, and the registry's value names are
    // deliberately not consulted -- which is a correction, not a shortcut.
    //
    // They were, on the reasoning that some machines record each kit as a
    // version-named value while others (this one included) record `KitsRoot10` and
    // two hundred GUIDs. But a candidate only survives if `<Include>/<version>`
    // exists as a directory, and `ListDirectories` already returns every such
    // directory -- so a registry-sourced name could never contribute one the listing
    // had not. It was two `RegQueryInfoKey` sweeps of ~200 value names each, per
    // translation unit on the launcher's dispatch path, that could not change the
    // answer.
    //
    // What the registry WOULD be good for is the opposite question -- telling an
    // installed kit from a directory an uninstall left behind -- and that would mean
    // treating its names as authoritative when it lists any, not unioning them in.
    // That changes which kit is chosen on such a machine, and therefore the
    // fingerprint, so it is not a change to make in passing.
    std::string best;
    for (auto const& candidate: host.ListDirectories(includeRoot))
        if (LooksLikeVersion(candidate) && (best.empty() || VersionLess(best, candidate)))
            best = candidate;
    if (best.empty())
        return {};

    auto const versionRoot = JoinPath(includeRoot, best);
    std::vector<std::string> roots;
    for (auto const& subdirectory: WindowsKitIncludeSubdirectories)
        AppendIfDirectory(host, roots, versionRoot, subdirectory);
    return roots;
}

IncludeSearchRoots ClangResourceIncludeRoots(IProcessRunner& runner, IToolchainHost& host, std::string const& compiler)
{
    std::array<std::string, 2> const argv { compiler, std::string { ClangResourceDirFlag } };

    // Split rather than combined, so a driver that prints a warning first cannot
    // put it where a path is expected. `-print-resource-dir` writes one line to
    // stdout and nothing else.
    auto const run = runner.RunCaptureSplit(argv);

    // The one exit code that is read, and read for the opposite reason to the next
    // paragraph: nothing was printed, so every test below would reject it and hand
    // back the same empty list a wrapper that understood nothing gives. See
    // `IncludeSearchRoots::answered`.
    if (run.exitCode == NotSpawned)
        return IncludeSearchRoots { .roots = {}, .answered = false };

    // Any OTHER exit code is not consulted, for the reason the GNU arm below states:
    // a driver can exit non-zero for reasons that leave its answer perfectly good.
    // What decides is whether the answer NAMES A DIRECTORY -- which is also what
    // rejects a wrapper that did not understand the flag, since its diagnostic is
    // not a path that exists.
    auto line = std::string_view { run.out }.substr(0, run.out.find('\n'));

    // A `\r` left on the end becomes part of the path, and every directory test
    // against it then fails silently -- a path that does not exist is skipped
    // rather than reported. `ParseGnuIncludeSearchPaths` strips it for the same
    // reason, and this output is read on the platform that produces it.
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);

    auto const resourceDir = Trim(line);
    if (resourceDir.empty())
        // Answered, badly. A driver that printed nothing usable is still served, on a
        // banner-only fingerprint -- which is what the case above must not collapse
        // into.
        return IncludeSearchRoots { .roots = {}, .answered = true };

    std::vector<std::string> roots;
    AppendIfDirectory(host, roots, resourceDir, ClangResourceIncludeSubdirectory);
    return IncludeSearchRoots { .roots = std::move(roots), .answered = true };
}

std::string ParseDriverTargetTriple(std::string_view driverOutput)
{
    // No frontend line, or one naming nothing this can trust, yields empty -- the
    // safe answer, because it leaves the key spelled as it is today and the dispatch
    // line unpinned. See the header for the one direction in which that is not
    // enough.
    return FirstLineAnswering(driverOutput, [](std::string_view line) -> std::string {
        // ONLY the frontend line is read, and that is the point of the marker rather
        // than a tidy-up. The `Target:` header near the top of this same output names
        // a triple too -- `x86_64-pc-windows-msvc` -- but names it WITHOUT the version
        // suffix that carries `-fms-compatibility-version`. A reader that took
        // whichever triple came first would pin the architecture, drop the code
        // generation contract, and look entirely correct doing it.
        //
        // A substring test before tokenizing, so the banner lines above the frontend
        // invocation are skipped without being split apart. It is a FILTER and not the
        // decision: `-cc1` can appear inside a path, so the exact token match below is
        // still what decides.
        if (!line.contains(FrontendMarker))
            return {};

        auto const tokens = SplitDriverLine(line);
        if (!std::ranges::contains(tokens, FrontendMarker))
            return {};

        auto const option = std::ranges::find(tokens, TripleOption);
        if (option == tokens.end() || std::next(option) == tokens.end())
            return {};

        auto const value = *std::next(option);
        return LooksLikeTargetTriple(value) ? std::string { value } : std::string {};
    });
}

std::string ParseDriverTargetHeader(std::string_view driverOutput)
{
    return FirstLineAnswering(driverOutput, [](std::string_view line) -> std::string {
        auto const trimmed = Trim(line);
        if (!trimmed.starts_with(TargetHeaderPrefix))
            return {};

        // Validated by the same rule the frontend reader uses, and for the same
        // reason: this value becomes a cache key input, so a header naming a path --
        // which a `Configured with:` line is full of -- must not be taken for an
        // answer.
        auto const value = Trim(trimmed.substr(TargetHeaderPrefix.size()));
        return LooksLikeTargetTriple(value) ? std::string { value } : std::string {};
    });
}

std::string DiscoverTargetTriple(IProcessRunner& runner, std::string const& compiler, DriverSpec const& spec)
{
    // No `default:`, so a mechanism added to the table fails to compile here rather
    // than silently returning nothing -- which would present as a cache key that had
    // quietly stopped covering the target.
    switch (spec.targetDiscovery)
    {
        case TargetDiscovery::None:
            return {};

        case TargetDiscovery::ClangDriverLine: {
            auto const run = runner.RunCaptureSplit(ProbeArgv(compiler, spec.targetProbeFlags));
            // Read from STDERR, which is where `-###` prints all of it; stdout stays
            // empty. The exit code is deliberately not consulted, for the reason the
            // include probe gives: the frontend line is printed before anything that
            // could fail, and parsing is what decides whether the output is usable.
            return ParseDriverTargetTriple(run.err);
        }

        case TargetDiscovery::GnuTargetLine: {
            // The same spawn and the same stream as above; only which line is
            // authoritative differs, and that is the table's answer rather than this
            // function's. GCC writes its header to stderr exactly as clang does.
            auto const run = runner.RunCaptureSplit(ProbeArgv(compiler, spec.targetProbeFlags));
            return ParseDriverTargetHeader(run.err);
        }
    }
    return {};
}

IncludeSearchRoots DiscoverIncludePaths(IProcessRunner& runner,
                                        IToolchainHost& host,
                                        std::string const& compiler,
                                        DriverSpec const& spec)
{
    // No `default:`, so a mechanism added to the table fails to compile here
    // rather than silently returning nothing -- which would present as a
    // fingerprint that quietly stopped covering the include tree.
    switch (spec.includeDiscovery)
    {
        case IncludeDiscovery::None:
            // Nothing is asked, so nothing is missing: a driver with no mechanism is
            // not one whose mechanism failed. See `IncludeSearchRoots::answered`.
            return IncludeSearchRoots { .roots = {}, .answered = true };

        case IncludeDiscovery::GnuVerbose: {
            auto const run = runner.RunCaptureSplit(ProbeArgv(compiler, spec.includeProbeFlags));

            // Nothing was printed, so the parse below would return the same empty
            // list a driver that ran and listed nothing gives -- and that one is
            // legitimately served. See `IncludeSearchRoots::answered` (issue #225).
            if (run.exitCode == NotSpawned)
                return IncludeSearchRoots { .roots = {}, .answered = false };

            // Any OTHER exit code is deliberately NOT checked. The list is printed
            // before anything that could fail, and a driver can exit non-zero for
            // reasons that leave it perfectly valid -- a missing SDK component, a
            // warning promoted by a wrapper script. Parsing decides whether the
            // output is usable; an exit code cannot.
            //
            // Read from stderr, which is where every GNU-family driver prints it.
            return IncludeSearchRoots { .roots = ParseGnuIncludeSearchPaths(run.err), .answered = true };
        }

        case IncludeDiscovery::MsvcLayout: {
            // Every source below is a filesystem, registry or environment read, so
            // there is no spawn to fail and this mechanism always answers -- including
            // when the answer is nothing at all, which is the underivable-layout case
            // `ToolchainIdentity` deliberately serves on a banner-only fingerprint.
            auto roots = MsvcToolsetIncludeRoots(host, compiler);

            // The fallback is gated on the TOOLSET half, not on the merged list, and
            // the difference is a false match rather than a missing root.
            // `WindowsKitIncludeRoots` answers from the registry alone, so it knows
            // nothing about which compiler is being identified -- on any machine with
            // an SDK installed, a `cl.exe` outside the `VC\Tools\MSVC\<version>`
            // layout (a shim, a wrapper, a VS2015-era `<vs>\VC\bin\cl.exe`) would come
            // back with SDK roots only, never reach the fallback, and drop the VC
            // headers from its identity. Two such toolchains then digest IDENTICALLY,
            // which is exactly the false match this mechanism exists to prevent.
            if (roots.empty())
                return IncludeSearchRoots { .roots = IncludeEnvironmentRoots(host), .answered = true };

            // A partial layout answer IS kept rather than topped up from `INCLUDE`:
            // both ends of a dispatch run this same code and so reach the same partial
            // answer, whereas mixing in a variable only one of them has is precisely
            // how the two stop agreeing.
            auto kits = WindowsKitIncludeRoots(host);
            roots.insert(roots.end(), std::make_move_iterator(kits.begin()), std::make_move_iterator(kits.end()));
            return IncludeSearchRoots { .roots = std::move(roots), .answered = true };
        }

        case IncludeDiscovery::ClangResourceLayout:
            // No `INCLUDE` fallback, deliberately, and it is the difference that
            // makes this mechanism symmetric where `MsvcLayout` can only be
            // symmetric where a layout is derivable. A driver that does not answer
            // gets a banner-only fingerprint -- weaker, but `clang-cl` announces a
            // real version, so it still tells one clang from another. Reaching for
            // the variable would buy such a driver nothing and hand a service and a
            // developer prompt two different answers again.
            return ClangResourceIncludeRoots(runner, host, compiler);
    }
    return {};
}

std::string ComputeToolchainStamp(std::string_view banner, std::string const& compiler, std::span<std::string const> roots)
{
    auto const binary = PathFromNarrowText(compiler);
    if (!binary.has_value())
        // Bytes this process cannot read name nothing it can stat: the case below,
        // reached one step earlier.
        return {};

    auto const size = FileSizeOrZero(*binary);
    auto const mtime = LastWriteTicks(*binary);
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
        // A root whose bytes this process cannot read is one this stamp cannot
        // cover, and `ProbeToolchainFiles` skipped its contents for the same
        // reason. Folding a constant in would be worse than folding nothing: the
        // stamp would then be STABLE across every change under that root, which is
        // a toolchain quietly matching one it is not. Refusing to stamp is refusing
        // to cache, exactly as the unstattable binary above.
        auto const rootPath = PathFromNarrowText(root);
        if (!rootPath.has_value())
            return {};

        digest.Path(root);
        digest.Field(std::to_string(LastWriteTicks(*rootPath)));
    }
    return digest.ToHex();
}

ToolchainIdentity CachedToolchainFingerprint(IProcessRunner& runner,
                                             IToolchainHost& host,
                                             std::string const& compiler,
                                             std::string_view banner,
                                             DriverSpec const& spec,
                                             IParallelFor& parallel,
                                             bool forceRefresh)
{
    auto const discovered = DiscoverIncludePaths(runner, host, compiler, spec);
    auto const& roots = discovered.roots;

    // Decided here, where the banner and the roots are both in hand, rather than left
    // for a caller to reconstruct -- see `IdentityDefect` for what each one means.
    //
    // An unrun probe is asked FIRST, and the order is load-bearing rather than
    // stylistic. It implies the other test's roots half, and on a driver that could
    // not be run the banner has usually fallen back too -- so both would hold, and
    // only one of them names the cause. Reporting "no include roots were found" for
    // a probe that was never run sends an operator looking for a broken install.
    auto const probeDefect = [&] {
        if (!discovered.answered)
            return IdentityDefect::UnrunProbe;
        if (spec.includeDiscovery != IncludeDiscovery::None && roots.empty() && banner == NormalizedCompilerName(compiler))
            return IdentityDefect::NoEvidence;
        return IdentityDefect::None;
    }();

    // Resolved for the STAMP and the CACHE FILE, which is what makes the cache work
    // at all for a compiler invoked by bare name. `ComputeToolchainStamp` stats the
    // binary, and a bare `cl` or `gcc` cannot be stat'd from an arbitrary working
    // directory -- so it produced no stamp, nothing was ever cached, and the
    // multi-second walk of the include tree ran again for every translation unit.
    // It also gives the two spellings of one compiler ONE cache file, rather than
    // two entries whose contents are identical and each of which the other misses.
    //
    // The banner is deliberately NOT recomputed from the resolved path: it is the
    // caller's, taken from the compiler as invoked, and a GNU driver prints its own
    // argv[0] -- so re-deriving it here would make `cc` identify as `gcc` and part
    // company with the clients that call it `cc`.
    auto const resolved = host.ResolveOnSearchPath(compiler).value_or(compiler);
    auto const stamp = ComputeToolchainStamp(banner, resolved, roots);
    auto const cachePath = CacheFilePath(resolved);

    // Handed back rather than left for the caller to derive a second time (#259).
    // Every field is the one this function just used, so a caller recording what the
    // digest rests on records exactly that, and does it without a spawn.
    //
    // Built here, ABOVE the cache-hit return, because a hit is precisely the case the
    // caller was paying for: the entry it returns says nothing about the roots, so the
    // caller's second probe ran on every warm start forever.
    //
    // Disengaged when the probe could not be spawned, which is the guard moved out of
    // the caller and into the type -- see `ToolchainIdentity::evidence`. Copied rather
    // than moved out of `roots`: the walk below still needs them, and one vector of
    // root paths against the spawn this removes is not a trade worth reasoning about.
    auto evidence = [&]() -> std::optional<ToolchainEvidence> {
        if (!discovered.answered)
            return std::nullopt;
        return ToolchainEvidence { .compiler = resolved, .banner = std::string { banner }, .roots = roots, .stamp = stamp };
    }();

    // A probe that never ran touches the cache in NEITHER direction, and both halves
    // are needed (issue #225).
    //
    // Not WRITTEN, because the value describes no toolchain and the cache is what
    // makes it outlive the moment that produced it. The roots feed the stamp as well
    // as the digest, so a failure stamps differently -- which sounds self-correcting
    // and is not: a machine whose probe fails repeatedly stamps that failure
    // CONSISTENTLY, hits its own entry, and settles on the wrong fingerprint
    // permanently, with no walk to notice.
    //
    // Not READ, for the same reason from the other end: an earlier failure may
    // already have written one, and the stamp cannot tell it from a good entry.
    //
    // `NoEvidence` is deliberately still cached. It is a stable property of the
    // machine rather than an accident of one moment, and re-walking a toolchain that
    // will reach the same answer buys nothing.
    //
    // A cache HIT needs only this test, because a hit did no walk: the entry it
    // returns was written by a run that completed one.
    if (!forceRefresh && discovered.answered && !stamp.empty() && !cachePath.empty())
    {
        auto const [cachedStamp, cachedFingerprint] = ReadCache(cachePath);
        if (!cachedStamp.empty() && cachedStamp == stamp)
            return ToolchainIdentity { .fingerprint = cachedFingerprint,
                                       .defect = probeDefect,
                                       .evidence = std::move(evidence) };
    }

    // The expensive part, reached only on a miss or a forced refresh.
    auto scan = ProbeToolchainFiles(roots, parallel);
    auto const complete = scan.complete;
    auto const fingerprint = ComputeToolchainFingerprint(banner, std::move(scan.files));

    // A short walk is excluded from the cache for a HARSHER reason than an unrun
    // probe is, and this is the line that matters most in this function. An unrun
    // probe empties the root list, so it moves the stamp and a later good run
    // recomputes. A walk that stopped inside a root that IS there leaves every
    // stamped input identical -- the path and the mtime of each root, never their
    // contents -- so the short digest would validate against its own stamp forever,
    // and no later run would ever walk again to notice.
    if (discovered.answered && complete && !stamp.empty() && !cachePath.empty())
        WriteCacheAtomically(cachePath, stamp, fingerprint);

    // Mutually exclusive in practice -- every probe-level defect leaves the root list
    // empty, and an empty list has nothing to walk incompletely -- but ordered
    // anyway, so the more specific cause survives if that stops being true.
    auto const defect = [&] {
        if (probeDefect != IdentityDefect::None)
            return probeDefect;
        return complete ? IdentityDefect::None : IdentityDefect::PartialTree;
    }();
    return ToolchainIdentity { .fingerprint = fingerprint, .defect = defect, .evidence = std::move(evidence) };
}

} // namespace FastCache::Cc
