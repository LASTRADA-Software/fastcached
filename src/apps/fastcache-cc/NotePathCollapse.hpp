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
/// ## Both sides, since #1593
///
/// #1592 collapsed the EMIT side only -- the bytes handed to a build system -- and deferred the
/// ingest side so the stored-value bump it needs would be paid once, with company. It rode value
/// generation 6, and the ingest side is now two places, each doing it once:
///
/// - **The probe's dependency list**, collapsed at the probe boundary (`RootReconciler::DependencyList`,
///   the one call every dependency list makes). It feeds the key's neighbours, the manifest and the
///   dispatch path's depfile and notes, so one header reached by two include chains has ONE
///   spelling in all of them -- and `RenderShowIncludes`' byte-exact dedup, which ran before any
///   collapse, now sees two spellings of one header as one. The key itself does not move: it
///   hashes `LexicalForm`, which collapsed already.
/// - **The stored regions**, collapsed by the SERVER: `PathCanon::CanonicalizeRegion` collapses a
///   span before matching the roots. The launcher still sends the streams as the compiler wrote
///   them, which is what keeps "every server canonicalizes identically" true by construction.
///
/// The invariant, stated so a later reader can check it in one pass: **every note path fastcached
/// hands a build system is collapsed; every dependency list it carries is collapsed wherever a
/// collapse is lexically possible; and every stored path is collapsed wherever the collapse lands
/// under a root.** The last is narrower on purpose: a stored path outside every root is replayed
/// on another machine as the producer spelled it, so the server leaves its spelling alone.
///
/// ## The path rule is PathCanon's
///
/// `PathCanon::CollapseRelativeSegments` is the one lexical `..` collapse, and it is in the
/// library rather than here because the servers' canonicalizer uses it too. It is host-neutral AND
/// separator-preserving AND byte-exact when there is nothing to collapse, which none of the other
/// normalizers is:
///
/// - `NormalizePath` (`DirectManifest.hpp`) preserves native separators, but on a POSIX host it
///   does not collapse a Windows path at all -- `std::filesystem` treats `\` as a separator only
///   on a Windows host.
/// - `LexicalForm` (`DependencyProbe.cpp`) is host-neutral but folds to `/` by contract, which on
///   the emit side would rewrite the separators of every note for no benefit. It is also a
///   KEY-path helper: sharing it would mean an edit made for the emit side silently moving the
///   cache key.

/// Collapse the path of every `/showIncludes` note in a captured text region.
///
/// Recognition is `PathCanon`'s, so the notes found here are exactly the notes the canonicalizer
/// and the marker rewrites find -- anchored at column zero, which is where `cl` puts the marker
/// (it pads by inclusion depth AFTER it). Anything that is not a note, including a line that
/// merely quotes the marker, survives byte-for-byte.
///
/// @p grammar gates the whole transform: only a grammar that `PathCanon::CarriesIncludeNotes` is
/// length-limited, because only Ninja's `/showIncludes` reader carries that check -- its depfile
/// reader does not. The gate lives here rather than at each call site so "only `/showIncludes` is
/// length-limited" is one testable property instead of an `if` repeated at every seam. And only
/// the NOTES are rewritten, under any such grammar: `Grammar::MsvcStream` finds diagnostic paths
/// too, and a diagnostic is printed rather than parsed, so nothing limits its length.
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
