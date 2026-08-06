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

    /// True if `lowerName` ends with one of the recognised C++ source suffixes.
    [[nodiscard]] bool IsSourceSuffix(std::string_view lowerName)
    {
        constexpr std::array<std::string_view, 4> suffixes { ".cpp", ".cc", ".cxx", ".c" };
        return std::ranges::any_of(suffixes, [&](std::string_view s) { return lowerName.ends_with(s); });
    }

    /// Classify the compiler flavor from its basename.
    [[nodiscard]] Flavor ClassifyCompiler(std::string_view compiler)
    {
        std::string const base = AsciiLower(Basename(compiler));
        if (base.starts_with("clang-cl"))
            return Flavor::ClangCl;
        if (base.starts_with("cl"))
            return Flavor::Cl;
        return Flavor::Unknown;
    }

} // namespace

ParsedCommand ParseCommand(std::span<std::string const> argv)
{
    ParsedCommand out;
    if (argv.empty())
        return out;

    out.compiler = argv.front();
    out.flavor = ClassifyCompiler(out.compiler);

    auto const args = argv.subspan(1);
    std::size_t skipUntil = 0; // index the next iteration must not re-process
    for (auto const i: std::views::iota(std::size_t { 0 }, args.size()))
    {
        if (i < skipUntil)
            continue;
        std::string_view const a = args[i];

        if (a == "/showIncludes" || a == "-showIncludes")
        {
            out.wantShowIncludes = true;
            continue;
        }
        // Object output: /Fo<path>, /Fo <path>, or -o <path>.
        if (a.starts_with("/Fo") && a.size() > 3)
        {
            out.objPath = std::string { a.substr(3) };
            continue;
        }
        if (a == "/Fo" || a == "-o")
        {
            if (i + 1 < args.size())
            {
                out.objPath = args[i + 1];
                skipUntil = i + 2; // consume the value argument
            }
            continue;
        }
        // A bare argument ending in a source suffix is the translation unit.
        if (!a.empty() && a.front() != '/' && a.front() != '-' && IsSourceSuffix(AsciiLower(a)))
        {
            // First source wins; a second source means a multi-TU line we do
            // not cache (leave parsedOk false).
            if (out.source.empty())
                out.source = std::string { a };
            else
                out.source.clear(); // ambiguous — force fallback
        }
    }

    out.parsedOk = !out.source.empty();
    return out;
}

} // namespace FastCache::Cc
