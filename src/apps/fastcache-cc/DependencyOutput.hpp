// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Write the dependency record a compile would have written, from paths somebody
/// else discovered.
///
/// ## Why this exists
///
/// A remote worker compiles **preprocessed** text. Preprocessed text has no
/// `#include` left in it, so the worker's compiler reports no dependencies —
/// there are none to report. The build system, meanwhile, asked for them: Ninja
/// reads a depfile (or `/showIncludes` under `deps = msvc`) and uses it to decide
/// when this translation unit must be rebuilt.
///
/// Handing back an object with no dependency record is therefore not a missing
/// nicety, it is the same defect the hit path already guards against in as many
/// words: the build sees no header dependencies for this TU and **stops rebuilding
/// it when its headers change**. That is a wrong build with a zero exit code, and
/// it persists until someone cleans.
///
/// The client already has the answer. Its own preprocess probe opened every one of
/// those headers to compute the cache key, and `SourceProbe::dependencyPaths` is
/// exactly the set the local compile's depfile would have listed. So the client
/// reproduces the record rather than the worker inventing one.
///
/// ## Why the paths are not rewritten here
///
/// These are the paths *this machine* resolved, and they are being written into
/// *this machine's* build directory for *this machine's* build system to read. They
/// are deliberately not canonicalized: a token like `<SRCROOT>/inc/a.hpp` is what a
/// STORE carries, and Ninja cannot stat it. Canonicalization happens on the way
/// into the cache, and localization on the way out — neither belongs on the local
/// filesystem path this build is about to consume.

/// Render a GNU-style depfile.
///
/// `<target>: <dep> <dep> ...`, one dependency per continued line, in the shape
/// `-MD` produces. Spaces inside a path are escaped with a backslash, as make
/// requires — a path containing one is otherwise read as two dependencies, and the
/// second is a file that does not exist, which makes the build rebuild forever.
///
/// The target is spelled exactly as the caller gives it, which must be exactly the
/// `-o` path the build passed: Ninja compares the depfile's rule target against
/// what it asked for and fails outright ("expected depfile ... to mention ...")
/// when they differ, while make silently matches no rule and drops every
/// dependency.
///
/// @param target The rule target, i.e. the object path the build asked for.
/// @param dependencyPaths The headers, as the driver spelled them.
/// @return The depfile text, ending in a newline.
[[nodiscard]] std::string RenderDepFile(std::string_view target, std::span<std::string const> dependencyPaths);

/// Render MSVC `/showIncludes` notes.
///
/// One `Note: including file: <path>` line per dependency, which is the form Ninja
/// parses under `deps = msvc`. Emitted on the stream the build expects them on by
/// the caller; this only produces the text.
///
/// No nesting indentation is reproduced. `cl` indents by inclusion depth, and the
/// probe's flattened set no longer carries that structure — but nothing consumes
/// it: Ninja's parser takes the path after the marker and ignores leading blanks,
/// which is the same rule `IncludeNotePath` implements on the reading side.
///
/// @param dependencyPaths The headers, as the driver spelled them.
/// @return The notes, each line ending in a newline; empty for an empty set.
[[nodiscard]] std::string RenderShowIncludes(std::span<std::string const> dependencyPaths);

} // namespace FastCache::Cc
