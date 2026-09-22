// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Collapsing `..` out of the note paths a build system reads.
///
/// ## The defect this answers is Ninja's, and it is a LENGTH check
///
/// `IncludesNormalize::Normalize()` (Ninja, `src/includes_normalize-win32.cc`) refuses a
/// `/showIncludes` path before it canonicalizes one:
///
/// ```
/// char copy[_MAX_PATH + 1];
/// size_t len = input.size();
/// if (len > _MAX_PATH) { *err = "path too long"; return false; }
/// ```
///
/// A string-length compare against a compile-time 260, guarding a fixed stack buffer. **No
/// filesystem call happens**, so a host's long-path policy cannot reach it and neither can
/// Ninja's own `longPathAware` manifest, which it already declares. The build fails with a bare
/// `path too long` and no compiler diagnostic at all, which reads as a toolchain fault rather
/// than a path one.
///
/// Paths get that long because MSVC and clang-cl resolve a quoted `#include` against the
/// TEXTUAL path of the including file and report the concatenation verbatim, so a chain of
/// nested relative includes ACCUMULATES `..` segments. Measured on the build that prompted this
/// (Windows 11, VS 18, MSVC 14.51.36231, a 60-byte build root): a 299-byte note path that
/// collapses to 88, and 28 translation units over the limit (#1592).
///
/// ## Why doing it first is safe
///
/// Ninja applies this identical lexical collapse itself, in the statement after the length
/// check, via `GetFullPathNameA`. Collapsing before the check rather than after changes no
/// dependency edge -- it only stops a path being refused for a length it does not really have.
///
/// ## Why the EMIT side only
///
/// Every byte here goes to a build system; none of it goes to the cache. The store side reads
/// the untouched streams a few lines further down (`main.cpp` around the `storedOut`/`storedErr`
/// pair), and that separation is the whole design: canonicalizing at `RootReconciler::Path` --
/// the funnel `.agent/rules/compile-cache.md` names -- would feed the key, the manifest and the
/// stored regions, which is a `CompileValueVersion` bump, a conformance-corpus row and a
/// `GenerationBumps` entry, costing every fleet one cold compile cache (#879, #891). The ingest
/// side is deferred to its own ticket so that bump is paid once, with company.
///
/// The invariant, stated so a later reader can check it in one pass: **every byte fastcached
/// hands a build system has its note paths collapsed; every byte it hands the cache does not.**
///
/// ## Why a fourth normalizer
///
/// Three already exist and none of them fits this half of the problem:
///
/// - `NormalizePath` (`DirectManifest.hpp`) preserves native separators, but on a POSIX host it
///   does not collapse a Windows path at all -- `std::filesystem` treats `\` as a separator only
///   on a Windows host. `NormalizeForLayout` corrects the separator afterwards, which as its own
///   comment says cannot recover a collapse that did not occur.
/// - `LexicalForm` (`DependencyProbe.cpp`) is host-neutral but folds to `/` by contract, which on
///   the emit side would rewrite the separators of every note on the dominant path for no
///   benefit. It is also a KEY-path helper: sharing it would mean an edit made for the emit side
///   silently moving the cache key.
///
/// `DirectManifest.hpp` already blesses that divergence in as many words -- "not duplicates of
/// one rule but answers to two different halves of it". This is a third half: host-neutral AND
/// separator-preserving AND byte-exact when there is nothing to collapse.

/// Collapse `..` segments out of one path, lexically, preserving how it was spelled.
///
/// Byte-exact when there is nothing to collapse, which is the overwhelmingly common case: a path
/// with no `..` SEGMENT is returned verbatim without being parsed. `a..b` is not a `..` segment
/// and is left alone, which a `contains("..")` test would get wrong.
///
/// Host-neutral by construction rather than by correction. The separators are folded to `/` for
/// the lexical pass so it runs identically on either host, a leading anchor is split off and kept
/// verbatim, and the original separator style is restored afterwards:
///
/// - a UNC root (`\\host\share`) keeps both leading separators;
/// - a drive specifier (`D:`) is held aside, so `D:\..\x.hpp` is `D:\x.hpp` on every host rather
///   than POSIX's bare `x.hpp` -- a drive root cannot be ascended past, and that is a property of
///   the path, not of the machine reading it.
///
/// A path that still carries a `..` segment after the pass (a genuinely relative `..\..\x.hpp`,
/// which nothing lexical can resolve) comes back as the INPUT, so a caller never pays a
/// separator change for a collapse that did not happen.
///
/// Purely lexical: nothing here asks the filesystem, so it behaves the same on a machine where
/// the path does not exist.
///
/// @param path A path as a compiler spelled it.
/// @return The collapsed path, or @p path verbatim when there was nothing to collapse.
[[nodiscard]] std::string CollapseRelativeSegments(std::string_view path);

/// Collapse the path of every `/showIncludes` note in a captured text region.
///
/// Recognition is `PathCanon`'s, so the notes found here are exactly the notes the canonicalizer
/// and the marker rewrites find -- anchored at column zero, which is where `cl` puts the marker
/// (it pads by inclusion depth AFTER it). Anything that is not a note, including a line that
/// merely quotes the marker, survives byte-for-byte.
///
/// @p grammar gates the whole transform: only `Grammar::ShowIncludes` is length-limited, because
/// only Ninja's `/showIncludes` reader carries that check -- its depfile reader does not. The gate
/// lives here rather than at each call site so "only `/showIncludes` is length-limited" is one
/// testable property instead of an `if` repeated at every seam.
///
/// @p marker is required and undefaulted for #700's reason, applied to this seam: `PathCanon`'s
/// span finder matches the CANONICAL marker only, so a localized build's notes are invisible to
/// it. The marker is normalized in and restored out around the rewrite. A default would let the
/// next call site omit the build's own prefix and silently collapse nothing -- the failure mode
/// that is indistinguishable from working.
///
/// A build that has named no prefix on a localized toolchain still collapses nothing, because the
/// marker it believes in matches no line. That is the same limitation the store side already
/// carries, closed by the same door (#878), and it degrades to byte-identical output rather than
/// to wrong output.
///
/// @param text    The captured region bytes.
/// @param grammar The grammar identifying path spans within @p text.
/// @param marker  The prefix this build's notes carry (`msvc_deps_prefix`).
/// @return The region with each note's path collapsed; @p text unchanged when nothing matched.
[[nodiscard]] std::string CollapseNotePaths(std::string_view text, PathCanon::Grammar grammar, std::string_view marker);

} // namespace FastCache::Cc
