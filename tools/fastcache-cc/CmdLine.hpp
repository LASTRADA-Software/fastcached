// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Which MSVC-style compiler the launcher is fronting. Decides which stream
/// `/showIncludes` lands on (cl → stderr, clang-cl → stdout) and how the
/// command line is interpreted.
enum class Flavor : std::uint8_t
{
    Unknown,
    Cl,      ///< MSVC cl.exe.
    ClangCl, ///< clang-cl.exe (MSVC-compatible driver).
};

/// The pieces of a compile command line the launcher needs to key, cache, and
/// reproduce a compilation.
struct ParsedCommand
{
    Flavor flavor { Flavor::Unknown };
    std::string compiler;            ///< argv[0] — the real compiler to exec.
    std::string source;              ///< The translation-unit source path.
    std::string objPath;             ///< The requested object output (/Fo or -o).
    bool wantShowIncludes { false }; ///< True if /showIncludes was requested.
    bool parsedOk { false };         ///< False if the line is not a cacheable compile.
};

/// Parse a compiler invocation (`argv[0]` = compiler, rest = its arguments).
/// Recognises cl / clang-cl command lines, extracting the source file, the
/// object output path (`/Fo<path>`, `/Fo <path>`, or `-o <path>`), whether
/// `/showIncludes` was requested, and the compiler flavor. `parsedOk` is false
/// when the line has no single source file (e.g. a link or preprocess-only
/// step) — the launcher then falls back to a plain exec.
/// @param argv The full invocation, argv[0] being the compiler.
/// @return The parsed command; `parsedOk` gates cacheability.
[[nodiscard]] ParsedCommand ParseCommand(std::span<std::string const> argv);

} // namespace FastCache::Cc
