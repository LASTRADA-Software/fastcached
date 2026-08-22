// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// A direct-mode manifest: what a translation unit included, recorded so a later
/// compile can decide the cached object is still valid WITHOUT preprocessing.
///
/// Preprocessing an MFC translation unit costs ~1.4 s because it expands ~25 MB
/// of headers; hashing the ones that can actually change costs ~18 ms. The
/// manifest is what makes that trade available: on a miss the launcher records
/// the include set, and on the next compile it re-hashes that set and reuses the
/// result when every hash still matches.
///
/// **Two tiers, and why.** Of a real TU's 635 unique headers, 476 come from the
/// immutable toolchain (MSVC/ATLMFC, the Windows SDK, vcpkg) and account for 24
/// of its 25.6 MB. Hashing them individually costs 91 ms and — worse — their
/// absolute paths lie outside the build's roots, so PathCanon cannot canonicalize
/// them and a manifest naming them would be machine-specific. They are therefore
/// covered collectively by `toolchainStamp` (the compiler identity), not by
/// per-file entries. The remaining 159 project headers all live under the source
/// root, canonicalize to `<SRCROOT>/...` tokens, and are hashed individually.
///
/// That split is what keeps a manifest **portable**: every path it names is a
/// canonical token, so a manifest produced on one machine validates correctly on
/// another with a different checkout location. (sccache's equivalent direct mode
/// is restricted to local disk storage for exactly the reason this split avoids.)
struct DirectManifest
{
    /// One recorded include: a canonical path token and its content hash.
    struct Entry
    {
        std::string canonicalPath; ///< `<SRCROOT>/...` or `<BUILDTREE>/...` token.
        std::string contentHash;   ///< Hash of the file's bytes when recorded.

        [[nodiscard]] friend bool operator==(Entry const&, Entry const&) = default;
    };

    /// Identity standing in for every toolchain header, which are not listed
    /// individually. Changing compiler or SDK changes this and so invalidates the
    /// manifest wholesale.
    std::string toolchainStamp;

    /// The key the compiled object is stored under — the ordinary preprocessed key.
    ///
    /// A pointer rather than a second copy: storing the object again under a
    /// manifest-derived key would double the cached bytes, and because the L1 tier
    /// keeps values UNCOMPRESSED that doubling lands squarely on RAM (measured
    /// 27 GB -> 54 GB of working set for this codebase) where compression cannot
    /// help. Indirecting costs one extra fetch on a direct hit — tens of
    /// milliseconds — against an eviction, which costs a whole recompile.
    std::string objectKey;

    /// The project headers, sorted by canonical path so encoding is deterministic
    /// and two machines produce byte-identical manifests for the same state.
    std::vector<Entry> entries;

    [[nodiscard]] friend bool operator==(DirectManifest const&, DirectManifest const&) = default;
};

/// Failure modes of manifest encoding/decoding and validation.
enum class DirectError : std::uint8_t
{
    Malformed,      ///< Encoded bytes are structurally invalid.
    UnknownVersion, ///< Encoded with a manifest version this build cannot read.
    NotCanonical,   ///< A path could not be canonicalized (outside every root).
};

/// Serialize a manifest to a portable byte string. Entries are emitted in their
/// stored order, so callers that want determinism should keep `entries` sorted
/// (BuildManifest does).
/// @param manifest The manifest to encode.
/// @return The encoded bytes.
[[nodiscard]] std::string EncodeManifest(DirectManifest const& manifest);

/// Parse a manifest previously produced by EncodeManifest.
/// @param bytes The encoded form.
/// @return The manifest, or DirectError when the input is malformed or versioned
///         beyond this implementation.
[[nodiscard]] std::expected<DirectManifest, DirectError> DecodeManifest(std::string_view bytes);

/// Collapse a compiler-emitted dependency path to one stable spelling.
///
/// A driver echoes the path as *resolved*, which preserves whatever the
/// `#include` spelling and the `-I` search path contained: `D:\src\A\..\b\W.hpp`,
/// mixed separators and doubled slashes all occur in real output. Two spellings
/// of one header must collapse to one string, or the same file is recorded (and,
/// since issue #56, *keyed*) twice — and two machines whose generators spell an
/// include directory differently stop sharing entries entirely.
///
/// Deliberately LEXICAL, not `weakly_canonical`: touching the filesystem also
/// rewrites 8.3 short components to their long form, and a path so rewritten no
/// longer shares a prefix with a root spelled the other way.
///
/// @param rawPath A path as the compiler spelled it.
/// @return The normalized, native-separator form.
[[nodiscard]] std::string NormalizePath(std::string_view rawPath);

/// Normalize a path and put the LAYOUT's separator convention back.
///
/// `std::filesystem` normalizes to the **host's** preferred separator, but every
/// root and absoluteness test here is the layout's. On a Windows host a POSIX path
/// comes back backslash-separated, and `PathCanon::AnchorForLayout` — which for a
/// POSIX layout asks only about a leading `/` — then reports an absolute path as
/// `Anchor::WorkingDirectory`. That is the host coupling PathCanon.hpp forbids in as many words
/// ("Derived from the LAYOUT, never from the host").
///
/// Spelled once because the manifest's own callers need exactly this correction and
/// must agree: `ResolveAgainst` and `AnchorWorkingDirectory`. The rule started here
/// as an inline copy in DependencyProbe's `PortableForm`, and was hoisted when a
/// Windows CI run showed the manifest side was missing it.
///
/// `PortableForm` no longer calls this. It answers the same host coupling more
/// strongly, by folding to `/` on BOTH sides of the lexical pass (`LexicalForm`):
/// `std::filesystem` treats `\` as a separator only on a Windows *host*, so a
/// Windows path normalized on POSIX keeps every `..` segment and the collapse this
/// function exists to perform never happens at all. Correcting the separator
/// afterwards, as here, cannot recover a collapse that did not occur. The two
/// spellings are therefore not duplicates of one rule but answers to two different
/// halves of it, and the divergence is deliberate — see `LexicalForm`.
///
/// @param rawPath A path as the compiler or the environment spelled it.
/// @param layout  The layout whose path conventions apply.
/// @return The normalized path, separated the way `layout` separates its roots.
[[nodiscard]] std::string NormalizeForLayout(std::string_view rawPath, PathCanon::Layout const& layout);

/// Classify whether an absolute include path belongs to the immutable toolchain
/// (and is therefore covered by the toolchain stamp rather than hashed).
///
/// Judged by path prefix rather than by "outside the build roots": a vcpkg tree
/// nested inside the build tree is still toolchain-like, and misclassifying a
/// project header as toolchain would let an edit go undetected.
/// @param absolutePath Native-form absolute include path.
/// @param layout       This build's roots.
/// @return True when the path is a toolchain header.
[[nodiscard]] bool IsToolchainHeader(std::string_view absolutePath, PathCanon::Layout const& layout);

/// Hash a file's contents for manifest entry comparison. A missing or unreadable
/// file yields an empty string, which never equals a recorded hash and so forces
/// the safe outcome (treat the manifest as stale).
///
/// The same 128-bit digest the keys use. It was one CRC32C paired with the byte
/// count, which left 32 bits against precisely the case that matters here — a
/// header edit that preserves length — and this value is what a direct hit
/// revalidates against, so a collision does not miss: it decides an edited header
/// is unchanged and serves the stale object (issue #63).
/// @param absolutePath The file to hash.
/// @return The hash as hex, or empty on any read failure.
[[nodiscard]] std::string HashFileContents(std::string_view absolutePath);

/// The text `cl` and `clang-cl` prefix every `/showIncludes` note with.
///
/// Spelled once because two readers need it and they must agree exactly:
/// ParseIncludePaths, which collects the paths, and DependencyProbe's
/// SplitIncludeNotes, which removes those same lines from a stream that is also
/// carrying preprocessed text. A note the splitter failed to recognise would be
/// hashed into the cache key as if it were source.
inline constexpr std::string_view IncludeNoteMarker = "Note: including file:";

/// The path one line names, when that line is a `/showIncludes` note.
///
/// The *recognition rule*, not just the marker, is what the two readers have to
/// share — and it is anchored to the start of the line, after leading blanks
/// only. Both halves of that are load-bearing:
///
/// - Blanks are skipped because `cl` indents a note by inclusion depth, so the
///   marker is not at column 0 for anything a header pulls in transitively.
/// - Nothing else may precede it, because SplitIncludeNotes applies this to a
///   stream that *also* carries preprocessed SOURCE. A rule that matched the
///   marker anywhere in the line deletes an ordinary source line that merely
///   contains the text — `char const* s = "Note: including file: x";` — from the
///   bytes the cache key is computed over, so two revisions differing only in
///   that literal key identically and the second is served the first's object.
///   That is a silent wrong build, and this repository's own sources contain the
///   literal, so it is not a hypothetical.
///
/// A trailing `\r` is stripped, so a note is recognised on either line ending.
///
/// @param line One line, with or without its `\r` terminator.
/// @return The path the note names (possibly empty), or nullopt when `line` is
///         not a note at all.
[[nodiscard]] std::optional<std::string_view> IncludeNotePath(std::string_view line) noexcept;

/// Extract the include paths from captured `/showIncludes` text.
///
/// Reads the *localized* form the compiler emitted (absolute native paths), not
/// the canonical form: the manifest is built at the moment of a real compile, so
/// the paths are still this machine's.
/// @param showIncludesText Captured compiler output containing include notes.
/// @return Every included path, in emission order, with duplicates preserved.
[[nodiscard]] std::vector<std::string> ParseIncludePaths(std::string_view showIncludesText);

/// Extract the dependency paths from a GNU-style Makefile depfile (`-MF`).
///
/// The GNU drivers report header dependencies here rather than on a stream, so
/// this is the direct-mode counterpart to ParseIncludePaths on POSIX: without
/// it, direct mode can never populate a manifest for gcc/clang and costs a
/// wasted lookup per compile while never yielding a hit.
///
/// Understands the format's real syntax: a `target: dep dep` rule (the target,
/// before the unescaped colon, is an output and is NOT a dependency),
/// backslash-newline continuations, `\ ` escapes for spaces inside a path, and
/// the phony `dep:` lines `-MP` emits (which have no dependencies of their own).
///
/// @param depFileText The depfile contents.
/// @return Every dependency path, in emission order, with duplicates preserved.
[[nodiscard]] std::vector<std::string> ParseDepFilePaths(std::string_view depFileText);

/// Everything one compile contributes to its manifest.
///
/// A struct rather than six positional arguments, for the reason `Cc::KeyInputs`
/// is one: the fields are strings that no compiler would stop you from swapping,
/// and the set grows as the recording rules do.
struct ManifestInputs
{
    /// The translation unit's own source path, exactly as the command line spelled
    /// it — relative or absolute.
    ///
    /// Separate from `includePaths`, and mandatory, because a manifest that does
    /// not name its TU revalidates everything except the file being compiled:
    /// neither `/showIncludes` nor a GNU depfile's rule target names the primary
    /// source, so editing a `.cpp` body while leaving every header untouched would
    /// be invisible to ValidateManifest and a stale object replayed forever
    /// (issue #49 / issue #51). It used to be appended to `includePaths` by the
    /// caller, which made the invariant a comment rather than something
    /// BuildManifest could enforce — and left it to be silently dropped when it was
    /// spelled relatively (issue #57).
    std::string sourcePath;

    /// The dependency paths the compile reported, as the driver spelled them:
    /// `/showIncludes` notes or a GNU depfile's dependency list.
    std::vector<std::string> includePaths;

    /// The directory the compile ran in, used to resolve every relative path above.
    ///
    /// Injected rather than read from the process: `HashFileContents` on a relative
    /// path would silently resolve it against whatever the ambient working
    /// directory happened to be, which is the coupling the project's DI rule exists
    /// to remove — and which no test could exercise without mutating it.
    std::string workingDirectory;

    /// Identity standing in for every toolchain header, which are not listed
    /// individually.
    std::string toolchainStamp;

    /// Key the compiled object is already stored under; recorded by value so the
    /// object is never stored twice.
    std::string objectKey;
};

/// Resolve a compiler-reported path to this machine's absolute, normalized form.
///
/// Normalization happens first and unconditionally, through `NormalizeForLayout`
/// so the separator convention is the layout's; a path the layout already calls
/// absolute is then returned as is, and anything else is joined onto
/// `workingDirectory`.
///
/// The anchor is decided by `PathCanon::AnchorForLayout`, never by the host:
/// `std::filesystem::path::is_absolute()` reads `D:\src\a.hpp` as relative
/// everywhere but Windows, and would then glue a working directory in front of it.
/// Only `Anchor::WorkingDirectory` is joined — a **drive-relative** path (`C:foo`)
/// is returned unresolved, because it is anchored to that drive's own current
/// directory and joining it would be a different wrong answer rather than a
/// resolution (issue #65).
///
/// The *join* itself is the host's path arithmetic, deliberately and unlike the
/// decision above: this function resolves against a directory on this machine and
/// its result is handed straight to `HashFileContents`, so it only ever runs where
/// the layout and the host agree. A relative path under a foreign layout (a Windows
/// layout on a POSIX host) is therefore outside its contract — collapsing `..`
/// across foreign separators would need a whole path arithmetic of its own, for a
/// case that cannot arise where the result is a file to open.
///
/// @param rawPath          A path as the compiler spelled it.
/// @param workingDirectory The directory the compile ran in; when empty, or when
///                         the join still does not produce an absolute path, the
///                         result stays relative and the caller must treat it as
///                         unanchored rather than as toolchain content.
/// @param layout           The layout whose path conventions apply.
/// @return The normalized path, absolute whenever it could be made so.
[[nodiscard]] std::string ResolveAgainst(std::string_view rawPath,
                                         std::string_view workingDirectory,
                                         PathCanon::Layout const& layout);

/// Re-spell a directory in the vocabulary the layout's roots use.
///
/// `std::filesystem::current_path()` answers with the kernel's *resolved* path —
/// `getcwd(3)` follows every symlink — while a layout's roots are spelled however
/// the build system was configured. On macOS a build under `/tmp` has a working
/// directory of `/private/tmp/...` against a root of `/tmp/...`; a checkout reached
/// through any symlinked `/home` or `/mnt` prefix has the same shape; and on
/// Windows a root carrying an 8.3 short component never matches a long-form cwd.
///
/// Every root test in this file and in PathCanon is a string prefix comparison, so
/// a working directory spelled the other way lies under no root — and a relative
/// path resolved against it then canonicalizes to nothing, which silently costs the
/// build direct mode. Matching by filesystem *identity* and handing back the root's
/// own spelling is what keeps the one value that comes from the environment
/// speaking the same language as the ones that come from configuration.
///
/// Falls back to the directory as given whenever the filesystem cannot answer, and
/// resolves the longest matching root first, as PathCanon::Canonicalize does, so a
/// build tree nested inside the source root anchors to the build tree.
///
/// @param directory The directory to re-spell, typically the process's cwd.
/// @param layout    This build's roots.
/// @return The directory spelled under a root when it lies within one, else the
///         normalized input.
[[nodiscard]] std::string AnchorWorkingDirectory(std::string_view directory, PathCanon::Layout const& layout);

/// Reduce a compile's source path to the canonical token that identifies it.
///
/// The single source of truth for that token: it is both the TU's own manifest
/// entry and the `canonicalSource` field of `ComputeManifestKey`, so the recording
/// and lookup sides cannot spell it differently. Deriving it from the *resolved*
/// path is also what keeps the manifest key unambiguous — `cc -c ../src/t.cpp` run
/// from `build/` and from `build/sub/` relativizes to the same argument list, so
/// without resolution the two would share a manifest key while naming different
/// files.
///
/// @param sourcePath       The TU's source path as the command line spelled it.
/// @param layout           This build's roots.
/// @param workingDirectory The directory the compile ran in.
/// @return The `<SRCROOT>`/`<BUILDTREE>` token, or NotCanonical when the source
///         lies under neither root (direct mode is then unavailable for this TU,
///         which is the safe outcome — the ordinary preprocessed key still works).
[[nodiscard]] std::expected<std::string, DirectError> CanonicalSourceToken(std::string_view sourcePath,
                                                                           PathCanon::Layout const& layout,
                                                                           std::string_view workingDirectory);

/// Build a manifest from one compile's source and include set.
///
/// Every path is resolved against `inputs.workingDirectory` first, then
/// classified: project content is canonicalized and hashed, toolchain content is
/// dropped (the stamp covers it), and a path that could not be made absolute at
/// all refuses the whole manifest rather than being dropped. Entries are
/// deduplicated and sorted by canonical path, so the same build state yields
/// byte-identical manifests on any machine.
///
/// **The anchor is classified before the toolchain test, and that order is the
/// whole subtlety.** `IsToolchainHeader` reports every path outside both roots as
/// toolchain, and a relative path lies under no root — so asking it first reports
/// *every* relative path as toolchain and silently drops it. A GNU build whose
/// depfile carries relative header paths (a relative `-I`, or a compile run from
/// the source directory) then recorded a manifest of its absolute entries alone;
/// edit one of the dropped headers, leave the `.cpp` untouched, and the direct hit
/// fires against a manifest that never named the edited file, serving a stale
/// object under a zero exit code. `Cc::IsCheckable` and `Cc::PortableForm` are the
/// two other consumers of that classifier and both already order it this way.
///
/// A resolved relative path is recorded as a *canonical token*, not kept relative.
/// `KeyDependencySet` keeps its relative paths, and the difference is what the two
/// values are for: a key input is only ever digested, while a manifest entry has
/// to be localized back to a file on the validating machine. A relative entry
/// could only be resolved against that machine's working directory, which is
/// exactly the ambiguity CanonicalSourceToken records — and it would also
/// introduce a second kind of entry into a format whose portability rests on every
/// path in it being a token.
///
/// @param inputs This compile's source, dependencies, working directory, stamp and
///               object key.
/// @param layout This build's roots.
/// @return The manifest, or DirectError when the source or an entry cannot be
///         canonicalized, or when a file could not be read.
[[nodiscard]] std::expected<DirectManifest, DirectError> BuildManifest(ManifestInputs const& inputs,
                                                                       PathCanon::Layout const& layout);

/// Re-check every entry against the filesystem.
///
/// Localizes each canonical path to this machine and compares a fresh content
/// hash with the recorded one. Any mismatch, missing file, or stamp change means
/// the cached object may not correspond to today's headers.
/// @param manifest       The recorded manifest.
/// @param layout         This machine's roots.
/// @param toolchainStamp This machine's current toolchain identity.
/// @return True when every entry still matches and the stamp agrees.
[[nodiscard]] bool ValidateManifest(DirectManifest const& manifest,
                                    PathCanon::Layout const& layout,
                                    std::string_view toolchainStamp);

/// Derive the key under which a manifest is stored.
///
/// Deliberately independent of header contents: this key must be computable
/// *before* the headers are known, from only the things available up front. The
/// manifest it retrieves then supplies the header hashes.
///
/// Shares one implementation with ComputeKey (`KeyDigest`), which is what keeps
/// the two key spaces the same shape without keeping two copies of the digest;
/// they are separated by their leading schema tag, not by the salt bytes that
/// used to distinguish them.
/// @param canonicalSource Canonical token for the translation unit's path.
/// @param relativizedArgs The compile arguments, already relativized.
/// @param toolchainStamp  The toolchain identity.
/// @return A stable 32-hex-char digest.
[[nodiscard]] std::string ComputeManifestKey(std::string_view canonicalSource,
                                             std::vector<std::string> const& relativizedArgs,
                                             std::string_view toolchainStamp);

/// Derive a digest over a manifest's header state.
///
/// Not the object key — the object lives under `manifest.objectKey`. This digest
/// exists so a *changed* header set is detectable even when the manifest key is
/// unchanged: the validator compares recorded hashes file by file, and this folds
/// the same information into one comparable value for tests and diagnostics.
/// @param manifestKey The key the manifest was found under.
/// @param manifest    The manifest to digest.
/// @return A stable 32-hex-char digest over the manifest key plus every entry.
[[nodiscard]] std::string ComputeHeaderStateDigest(std::string_view manifestKey, DirectManifest const& manifest);

} // namespace FastCache::Cc
