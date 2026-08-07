// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>

namespace FastCache::Cc
{
namespace
{

    /// Lower-case an ASCII string copy (for case-insensitive suffix/basename match).
    [[nodiscard]] std::string AsciiLower(std::string_view s)
    {
        std::string out { s };
        std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    /// The last path component of `path` (after the final `/` or `\`).
    [[nodiscard]] std::string_view Basename(std::string_view path)
    {
        auto const slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    /// True if `lowerName` ends with one of the recognised C/C++ source suffixes.
    [[nodiscard]] bool IsSourceSuffix(std::string_view lowerName)
    {
        constexpr std::array<std::string_view, 6> Suffixes { ".cpp", ".cc", ".cxx", ".c++", ".c", ".m" };
        return std::ranges::any_of(Suffixes, [&](std::string_view s) { return lowerName.ends_with(s); });
    }

    // --- driver descriptors -------------------------------------------------
    //
    // The single source of truth for how each compiler driver spells the things
    // the launcher cares about. A new driver is a new row here plus a name
    // pattern below; the parsing logic itself never grows a branch.

    // Both spellings must suppress line markers. `#line` / `# 1 "file"` markers
    // embed the ABSOLUTE source path, which would make the preprocessed text —
    // and therefore the cache key — differ between two checkouts of the same
    // content at different paths, defeating cross-machine sharing entirely.
    // MSVC: /EP writes to stdout without #line. GNU: -P suppresses markers.
    constexpr std::array<std::string_view, 2> MsvcPreprocess { "/EP", "/P" };
    constexpr std::array<std::string_view, 2> GnuPreprocess { "-E", "-P" };

    /// Flags dropped when preprocessing: compile-only and object-output flags
    /// (we want text on stdout, not an object), plus dependency flags, which
    /// would otherwise overwrite the real depfile during the key probe.
    constexpr std::array<std::string_view, 4> MsvcDrop { "/c", "/showIncludes", "/Fo", "-o" };
    constexpr std::array<std::string_view, 8> GnuDrop { "-c", "-o", "-MD", "-MMD", "-MF", "-MT", "-MQ", "-MP" };

    constexpr std::array<DriverSpec, 5> Drivers { {
        { .flavor = Flavor::Unknown,
          .optionPrefixes = "",
          .objectFlag = "",
          .preprocessFlags = {},
          .preprocessDropFlags = {},
          .includeStream = IncludeStream::None,
          .usesDepfile = false },
        { .flavor = Flavor::Cl,
          .optionPrefixes = "/-",
          .objectFlag = "/Fo",
          .preprocessFlags = MsvcPreprocess,
          .preprocessDropFlags = MsvcDrop,
          .includeStream = IncludeStream::Stderr,
          .usesDepfile = false },
        { .flavor = Flavor::ClangCl,
          .optionPrefixes = "/-",
          .objectFlag = "/Fo",
          .preprocessFlags = MsvcPreprocess,
          .preprocessDropFlags = MsvcDrop,
          .includeStream = IncludeStream::Stdout,
          .usesDepfile = false },
        { .flavor = Flavor::Gcc,
          .optionPrefixes = "-",
          .objectFlag = "-o",
          .preprocessFlags = GnuPreprocess,
          .preprocessDropFlags = GnuDrop,
          .includeStream = IncludeStream::None,
          .usesDepfile = true },
        { .flavor = Flavor::Clang,
          .optionPrefixes = "-",
          .objectFlag = "-o",
          .preprocessFlags = GnuPreprocess,
          .preprocessDropFlags = GnuDrop,
          .includeStream = IncludeStream::None,
          .usesDepfile = true },
    } };

    /// How a driver's basename is recognised. Order matters: the first match
    /// wins, so longer, more specific stems precede their prefixes
    /// ("clang-cl" before "clang", "c++" before "cc").
    struct NamePattern
    {
        std::string_view stem;
        Flavor flavor;
    };

    constexpr std::array<NamePattern, 8> NamePatterns { {
        { .stem = "clang-cl", .flavor = Flavor::ClangCl },
        { .stem = "clang++", .flavor = Flavor::Clang },
        { .stem = "clang", .flavor = Flavor::Clang },
        { .stem = "g++", .flavor = Flavor::Gcc },
        { .stem = "gcc", .flavor = Flavor::Gcc },
        { .stem = "c++", .flavor = Flavor::Gcc },
        { .stem = "cc", .flavor = Flavor::Gcc },
        { .stem = "cl", .flavor = Flavor::Cl },
    } };

    /// Classify the compiler flavor from its basename.
    ///
    /// Tolerates version suffixes (`g++-14`, `clang-18`) and the `.exe`
    /// extension, both of which are ordinary on real build systems.
    ///
    /// @param compiler argv[0] as invoked.
    /// @return The matching flavor, or Unknown.
    [[nodiscard]] Flavor ClassifyCompiler(std::string_view compiler)
    {
        std::string base = AsciiLower(Basename(compiler));
        if (base.ends_with(".exe"))
            base.resize(base.size() - 4);

        // `auto const` and not `auto const*`: std::array's iterator is a raw
        // pointer only on libstdc++/libc++. MSVC's STL returns a class-type
        // iterator, which does not bind to a pointer declaration.
        auto const match = std::ranges::find_if(NamePatterns, [&base](NamePattern const& pattern) {
            if (!base.starts_with(pattern.stem))
                return false;
            // Anything after the stem must be a version suffix ("-14", "-18"),
            // never more name — so "clanger" does not read as clang.
            auto const rest = std::string_view { base }.substr(pattern.stem.size());
            return rest.empty() || rest.front() == '-';
        });
        return match != NamePatterns.end() ? match->flavor : Flavor::Unknown;
    }

    /// True if `a` is an option (starts with one of the driver's introducers).
    [[nodiscard]] bool IsOption(std::string_view a, DriverSpec const& driver)
    {
        return !a.empty() && driver.optionPrefixes.contains(a.front());
    }

    /// Flags that carry their value as the NEXT argument when given bare, so
    /// dropping the flag must drop the value with it.
    constexpr std::array<std::string_view, 5> ValueFlags { "-o", "/Fo", "-MF", "-MT", "-MQ" };

    /// True if `flag`, given bare, consumes the following argument as its value.
    /// @param flag The flag as it appeared on the command line.
    /// @return True when the next argument belongs to it.
    [[nodiscard]] bool TakesValue(std::string_view flag)
    {
        return std::ranges::contains(ValueFlags, flag);
    }

    /// Characters that may separate a flag from a value fused onto it.
    ///
    /// Only a value-taking flag may appear in joined form at all, and the join
    /// must be a value — never more flag name. `-MFdep.d` and `-o=x.o` are the
    /// joined forms; `-MP` merely starts with `-M`, and `-coverage` with `-c`.
    constexpr std::string_view JoinSeparators = "=:";

    /// True if `arg` is `flag` carrying a fused value, rather than a different
    /// flag that merely begins with the same characters.
    ///
    /// Getting this wrong is expensive and silent: matching on `starts_with`
    /// alone makes `-c` swallow `-coverage` and `/c` swallow `/clr`, dropping a
    /// real flag from the preprocess line (and stranding its value as a stray
    /// input file), so the probe fails and every such TU compiles uncached
    /// forever while all unit tests still pass.
    ///
    /// A value-taking flag joins directly (`-MFdep.d`, `/Fox.obj`) or through a
    /// separator (`-MF=dep.d`). A flag that takes no value has no joined form,
    /// so anything longer than it is a different flag.
    ///
    /// @param arg  The argument as it appeared on the command line.
    /// @param flag The candidate flag.
    /// @return True when `arg` is `flag` with a value fused onto it.
    [[nodiscard]] bool IsJoinedValue(std::string_view arg, std::string_view flag)
    {
        if (!arg.starts_with(flag) || arg.size() <= flag.size())
            return false;
        if (!TakesValue(flag))
            return false;
        auto const tail = arg.substr(flag.size());
        // `/Fo` and `-o` fuse their value directly; a separator is also accepted
        // so `-MF=dep.d` is not mistaken for an unrelated flag.
        return !JoinSeparators.contains(tail.front()) || tail.size() > 1;
    }

    /// Drop a leading join separator from a fused value, so `-MF=dep.d` and
    /// `-MFdep.d` both yield `dep.d`.
    /// @param tail The text following the flag in its joined form.
    /// @return The value proper.
    [[nodiscard]] std::string_view StripJoinSeparator(std::string_view tail)
    {
        if (!tail.empty() && JoinSeparators.contains(tail.front()))
            tail.remove_prefix(1);
        return tail;
    }

    /// True if `arg` is `flag` exactly, or `flag` with a fused value.
    /// @param arg  The argument as it appeared on the command line.
    /// @param flag The candidate flag.
    /// @return True on either the bare or the joined form.
    [[nodiscard]] bool MatchesFlag(std::string_view arg, std::string_view flag)
    {
        return arg == flag || IsJoinedValue(arg, flag);
    }

} // namespace

DriverSpec const& DriverOf(Flavor flavor)
{
    // `auto const`, not `auto const*` — see the note in ClassifyCompiler:
    // std::array's iterator is only a raw pointer on libstdc++/libc++.
    auto const match = std::ranges::find_if(Drivers, [flavor](DriverSpec const& spec) { return spec.flavor == flavor; });
    return match != Drivers.end() ? *match : Drivers.front();
}

ParsedCommand ParseCommand(std::span<std::string const> argv)
{
    ParsedCommand out;
    if (argv.empty())
        return out;

    out.compiler = argv.front();
    out.flavor = ClassifyCompiler(out.compiler);
    auto const& driver = DriverOf(out.flavor);
    if (out.flavor == Flavor::Unknown)
        return out;

    auto const args = argv.subspan(1);
    bool sawCompileOnly = false;
    bool preprocessOnly = false;
    std::size_t skipUntil = 0; // index the next iteration must not re-process

    for (auto const i: std::views::iota(std::size_t { 0 }, args.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = args[i];

        // Inline dependency reporting (MSVC drivers only).
        if (a == "/showIncludes" || a == "-showIncludes")
        {
            out.wantShowIncludes = true;
            continue;
        }

        // Compile-only / preprocess-only markers decide cacheability below.
        if (a == "/c" || a == "-c")
        {
            sawCompileOnly = true;
            continue;
        }
        if (a == "-E" || a == "/EP" || a == "/P")
        {
            preprocessOnly = true;
            continue;
        }

        // Object output, joined (`/Fo<path>`) or separated (`/Fo <path>`, `-o <path>`).
        if (!driver.objectFlag.empty() && MatchesFlag(a, driver.objectFlag))
        {
            if (a.size() > driver.objectFlag.size())
            {
                out.objPath = std::string { StripJoinSeparator(a.substr(driver.objectFlag.size())) };
                continue;
            }
            if (i + 1 < args.size())
            {
                out.objPath = args[i + 1];
                skipUntil = i + 2; // consume the value argument
            }
            continue;
        }
        // MSVC drivers also accept `-o <path>`; GNU drivers never see `/Fo`.
        if (a == "-o" && i + 1 < args.size())
        {
            out.objPath = args[i + 1];
            skipUntil = i + 2;
            continue;
        }

        // Depfile destination (GNU drivers): -MF <path> or -MF<path>.
        constexpr std::string_view DepFileFlag = "-MF";
        if (driver.usesDepfile && MatchesFlag(a, DepFileFlag))
        {
            if (a.size() > DepFileFlag.size())
            {
                out.depPath = std::string { StripJoinSeparator(a.substr(DepFileFlag.size())) };
                continue;
            }
            if (i + 1 < args.size())
            {
                out.depPath = args[i + 1];
                skipUntil = i + 2;
            }
            continue;
        }

        // A bare argument ending in a source suffix is the translation unit.
        if (!IsOption(a, driver) && IsSourceSuffix(AsciiLower(a)))
        {
            // First source wins; a second source means a multi-TU line we do
            // not cache (leave parsedOk false).
            if (out.source.empty())
                out.source = std::string { a };
            else
                out.source.clear(); // ambiguous — force fallback
        }
    }

    // A cacheable line compiles exactly one TU to an object. A preprocess-only
    // run produces text, not an object, so it is never cached; a line with no
    // -c/​/c is a link (or a compile-and-link) and is likewise left alone.
    //
    // The object path must be known, too. `g++ -c a.cpp` (no -o) is a perfectly
    // ordinary compile that defaults its output to ./a.o, but the launcher has
    // no path to read the object back from or write it to — treating it as
    // cacheable makes every such compile report a MISS and then fail to store,
    // forever, and would hand an empty path to the file writer on a hit.
    out.parsedOk = !out.source.empty() && !out.objPath.empty() && sawCompileOnly && !preprocessOnly;
    return out;
}

std::vector<std::string> PreprocessCommand(ParsedCommand const& cmd, std::span<std::string const> argv)
{
    auto const& driver = DriverOf(cmd.flavor);

    std::vector<std::string> out;
    out.reserve(argv.size() + driver.preprocessFlags.size());
    out.emplace_back(cmd.compiler);
    for (auto const& flag: driver.preprocessFlags)
        out.emplace_back(flag);

    std::size_t skipUntil = 1; // argv[0] is the compiler, already emitted
    for (auto const i: std::views::iota(std::size_t { 1 }, argv.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = argv[i];

        // Drop a dropped flag together with its separated value, so a `-MF dep.d`
        // pair never leaves a stray "dep.d" argument behind. Matching is exact or
        // joined-with-a-value only — see IsJoinedValue: a prefix match would drop
        // `-coverage` for `-c` and `/clr` for `/c`.
        auto const dropped =
            std::ranges::find_if(driver.preprocessDropFlags, [a](std::string_view flag) { return MatchesFlag(a, flag); });
        if (dropped != driver.preprocessDropFlags.end())
        {
            // Only the bare form takes the NEXT argument; a joined form already
            // carries its value, so consuming the successor would eat a real flag.
            if (a == *dropped && TakesValue(*dropped) && i + 1 < argv.size())
                skipUntil = i + 2;
            continue;
        }
        out.emplace_back(a);
    }
    return out;
}

} // namespace FastCache::Cc
