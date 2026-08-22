// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"
#include "ToolchainFingerprint.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Discovering a toolchain's include tree, and digesting it into a fingerprint.
///
/// Split the way `PathCanon` and `CompileValue` already are: the parsing is pure
/// and unit-tested against captured driver output, and the filesystem walk is a
/// separate function that the pure half never calls. That matters more here than
/// usual, because the inputs this has to get right — an Xcode SDK layout, a
/// Windows developer prompt's `INCLUDE`, a GCC install with a version-suffixed
/// directory — cannot all exist on whatever machine runs the tests.

/// Extract the system include search paths a GNU driver prints under `-v`.
///
/// The driver frames the list between two literal markers and indents each entry
/// by one space. Everything outside those markers is other verbose noise — the
/// target triple, the configure line, the assembler invocation — and must not be
/// read as a path.
///
/// A `(framework directory)` suffix is stripped and the path kept: on macOS the
/// SDK's framework directories are genuine search paths, and dropping them would
/// silently narrow the fingerprint on the one platform where they carry the
/// system headers for Objective-C++ translation units.
///
/// @param verboseOutput The driver's stderr (the list is not printed on stdout).
/// @return The search paths in the order the driver listed them, which is
///         significant to the compiler and preserved here even though the digest
///         sorts — this function answers "what does it search", not "what is the
///         fingerprint".
[[nodiscard]] std::vector<std::string> ParseGnuIncludeSearchPaths(std::string_view verboseOutput);

/// Split an MSVC `INCLUDE` environment variable into search paths.
///
/// Semicolon-separated, which is the separator the variable uses on Windows
/// regardless of what the host running this parser thinks a path list looks like.
/// Empty entries are dropped: a trailing or doubled separator is ordinary in a
/// value built up by several `vcvars` invocations, and an empty path is not a
/// directory anyone can walk.
///
/// @param value The raw variable value.
/// @return The search paths, in order.
[[nodiscard]] std::vector<std::string> ParseIncludeEnvironment(std::string_view value);

/// Walk include search roots and digest every file under them.
///
/// **This is the I/O half** — it opens files, and it is deliberately not
/// something the pure digest calls.
///
/// Each file is recorded by its path RELATIVE to the root it was found under,
/// which is what lets two machines running the same toolchain at different
/// install prefixes agree. A root that does not exist is skipped rather than
/// failing: a driver lists search paths it would use if they existed, and a
/// missing one is normal (`/usr/local/include` on a machine that has none).
///
/// Cost is the reason the caller must cache this. Measured on an Xcode
/// toolchain: 14,600 files and 288 MB, about 2 s warm — per launcher invocation
/// that would dwarf the compile it is trying to accelerate.
///
/// @param roots Include search paths, as a driver reported them.
/// @return One entry per readable file, unsorted (the digest sorts).
[[nodiscard]] std::vector<ToolchainFile> ProbeToolchainFiles(std::span<std::string const> roots);

} // namespace FastCache::Cc
