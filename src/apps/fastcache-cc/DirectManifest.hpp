// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/EnumTable.hpp>

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

/// Failure modes of manifest DECODING: what is wrong with a stored byte string.
///
/// Deliberately not the vocabulary a refusal to BUILD one speaks. Those two
/// failures share no audience: this one names bytes that came back from the
/// daemon, `ManifestFault` names a path on this machine's disk. They were one enum
/// until issue #68, and the conflation is half of why a build refusal could not say
/// what it was — `Malformed` meant both "the encoded form is invalid" and "a file
/// could not be read", so the two printed identically.
enum class DirectError : std::uint8_t
{
    Malformed,      ///< Encoded bytes are structurally invalid.
    UnknownVersion, ///< Encoded with a manifest version this build cannot read.
};

/// Why one path stopped a manifest being built.
///
/// A DIAGNOSTIC vocabulary, not a decision: nothing branches on it, and every
/// enumerator refuses the manifest just as completely as the next. They exist so a
/// refusal can be *named*, because it is the only account a translation unit that
/// silently never caches ever gets (issue #68).
///
/// Five rather than the two the refusal paths happen to have, and the split is the
/// point: each names a different thing to go and fix. `Unanchored` is a working
/// directory, `OutsideRoots` is a layout, `ToolchainLike` is a TU somewhere nobody
/// expected to compile from, `Uncanonical` is a root spelled almost right,
/// `Unreadable` is a file. `PathDisposition` in DependencyProbe.hpp draws
/// the same lines for the key's dependency set, for the same reason (issue #105) —
/// separate because a manifest's outcomes are not a key's: there is no `Keyed` and
/// no `Toolchain` here, since a toolchain header is dropped rather than refused,
/// and a key never opens the file it is classifying so it has no `Unreadable`.
enum class ManifestFault : std::uint8_t
{
    Unanchored,    ///< Still relative after ResolveAgainst: the working directory could
                   ///< not place it, so the file it names is unknown.
    OutsideRoots,  ///< Absolute and under neither root, so it has no portable form. An
                   ///< ordinary layout for a TU (`add_subdirectory(../shared)`), which
                   ///< is why this is a refusal to shortcut and not a fault in the build.
    ToolchainLike, ///< Under a root, but matching one of `IsToolchainHeader`'s markers
                   ///< -- a TU inside a vendored tree such as `vcpkg_installed/`. Split
                   ///< from OutsideRoots because the roots are fine and saying otherwise
                   ///< sends an operator to fix what is not broken.
    Uncanonical,   ///< Under no root, and yet a character-wise prefix match for one --
                   ///< `/x/build-other/a.h` against a `/x/build` root. A root spelled
                   ///< almost right, and the only path outside the roots this refuses
                   ///< over rather than dropping: nothing covers it, so a manifest
                   ///< without it would revalidate everything but the header being
                   ///< edited. Read off `ClassifyAgainstRoots` since issue #562.
    Unreadable,    ///< Could not be read, so it could not be hashed. A manifest entry
                   ///< is only worth recording if its content hash is real.
    NoProjectDeps, ///< The compile reported dependencies and every one of them was
                   ///< dropped as toolchain, so the manifest would revalidate the TU
                   ///< and nothing else -- and would keep doing so forever.
    Last,          ///< Not a fault, and has no row: the table's length.
};

/// One row per fault: the enumerator, and the word the note uses for it.
struct FaultRow
{
    ManifestFault fault;    ///< The refusal this row names.
    std::string_view label; ///< How the launcher's note spells it.
};

/// The fault table, in enumerator order so a fault indexes its own row.
///
/// A table rather than a `switch`, for the reason `DependencyProbe`'s
/// `DispositionTable` and `Metrics/MetricsCatalog` are ones: a refusal that can be
/// returned but not named renders as nothing, which is the defect this whole
/// vocabulary exists to close. Declared through `Core/EnumTable.hpp`, which is
/// where that idiom lives once (issue #112).
///
/// `EnumTable` is what gets the length right, and the length is the half that has
/// to be. A fault appended after `Unreadable` while `Last` still ended the enum
/// would leave a table one row short that a `size() == Unreadable + 1` assert
/// happily accepts — and `DescribeManifestFailure` indexes this by the enumerator,
/// so the new fault would read one past the end. Taking the extent from the enum
/// instead makes the missing row value-initialize to `{ Unanchored, "" }` at a
/// non-zero index, which `RowsInEnumeratorOrder` rejects.
inline constexpr EnumTable<ManifestFault, FaultRow> FaultTable { {
    { .fault = ManifestFault::Unanchored, .label = "unanchored" },
    { .fault = ManifestFault::OutsideRoots, .label = "under no root" },
    { .fault = ManifestFault::ToolchainLike, .label = "matches a toolchain marker" },
    { .fault = ManifestFault::Uncanonical, .label = "no canonical form" },
    { .fault = ManifestFault::Unreadable, .label = "unreadable" },
    { .fault = ManifestFault::NoProjectDeps, .label = "every reported dependency was dropped as toolchain" },
} };

static_assert(RowsInEnumeratorOrder(FaultTable, &FaultRow::fault),
              "FaultTable must hold one row per ManifestFault, in enumerator order -- the order is what lets a fault "
              "index its own row");

/// One refusal to build a manifest: what went wrong, and the path it went wrong on.
///
/// The path is what makes this worth carrying. A manifest refusal is invisible from
/// every other direction — the compile succeeds, the object still caches under the
/// ordinary preprocessed key, and only the shortcut is quietly gone — so "why does
/// this TU never cache" is otherwise a whole investigation, and the answer is one
/// path. The same reasoning `Cc::MissingReplayedDependency`'s note is built on.
struct ManifestFailure
{
    ManifestFault fault; ///< Which refusal this was.

    /// The offending path, as `ResolveAgainst` left it — the value the
    /// classification actually rejected, rather than the spelling it arrived in.
    /// For `OutsideRoots` and `Uncanonical` that is what the reader has to compare
    /// against the roots; for `Unanchored` resolution changed nothing, so it is
    /// still recognisably what the driver reported.
    std::string path;

    [[nodiscard]] friend bool operator==(ManifestFailure const&, ManifestFailure const&) = default;
};

/// Render a refusal as one path and one reason, for the launcher's note.
///
/// Lives here rather than in main.cpp because main.cpp is in no test target, the
/// lesson `CacheProtocol.cpp`, `RootReconciler.cpp` and `DescribeDropped` are each
/// recorded as having been extracted for.
///
/// @param failure The refusal to describe.
/// @return `"no canonical form: /x/build-other/a.h"`.
[[nodiscard]] std::string DescribeManifestFailure(ManifestFailure const& failure);

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
/// Judged by a toolchain MARKER rather than by "outside the build roots": a vcpkg
/// tree nested inside the build tree is still toolchain-like, and misclassifying a
/// project header as toolchain would let an edit go undetected. Failing that, by
/// whether the path lies under a root — asked of `PathCanon::RelateToLayout`, the
/// one definition of that, so this classifier and the canonicalizer that has to
/// produce a token for whatever it calls project content cannot disagree about
/// which paths those are (issue #562).
///
/// The key filter, the manifest and the replay guard all judge by this function, so
/// what it means is what all three mean — through `ClassifyAgainstRoots`, whose
/// three-way answer they take rather than asking this and a second predicate in
/// turn. Kept as the name the rulebook and this tree's comments are written around.
///
/// @param absolutePath Native-form absolute include path.
/// @param layout       This build's roots.
/// @return True when the path is a toolchain header.
[[nodiscard]] bool IsToolchainHeader(std::string_view absolutePath, PathCanon::Layout const& layout);

/// What an absolute path is to this build's roots: the three-way answer
/// `IsToolchainHeader` collapses into two.
enum class PathClass : std::uint8_t
{
    Project,      ///< Under a root and matching no toolchain marker: content this
                  ///< build owns, canonicalizable to a token and hashed.
    NearMissRoot, ///< Under NO root, matching no marker, and yet a character-wise
                  ///< prefix of one — `<root>-other/a.hpp` against `<root>`. A root
                  ///< spelled almost right, and the one root fault of the three an
                  ///< operator repairs by editing a root rather than moving a file.
    Toolchain,    ///< Everything else outside the roots, plus anything matching a
                  ///< toolchain marker wherever it sits: content the compiler
                  ///< identity in the key covers collectively.
};

/// Classify an absolute path against this build's roots.
///
/// One call answers both questions the three consumers ask, and that is the point
/// rather than a convenience: the marker scan wins over the roots (a vendored tree
/// nested under the build tree is toolchain content however well rooted it is), so
/// asking "near miss?" separately, of a path a marker had already claimed, reported
/// `<root>-deps/vcpkg_installed/.../core.h` as a misspelled root and refused a
/// manifest over it. There is deliberately no `IsNearMissRoot` predicate beside
/// `IsToolchainHeader` either: a name shaped like its peer invites a caller to ask
/// the two in turn, which is that same defect written one call site further out.
///
/// @param absolutePath Native-form absolute path.
/// @param layout       This build's roots.
/// @return What the path is to this build.
[[nodiscard]] PathClass ClassifyAgainstRoots(std::string_view absolutePath, PathCanon::Layout const& layout);

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

/// Extract the rule TARGETS from a GNU-style Makefile depfile (`-MF`).
///
/// The other half of the same grammar, and the distinction matters because a
/// depfile is the one place where paths from two authors meet: a rule's target is
/// an output the BUILD SYSTEM named (`-o`, or `-MT`/`-MQ` when given), while its
/// dependencies are what the COMPILER reported. The launcher reconciles a driver's
/// spelling and must leave the build system's alone — respelling a target hands
/// the build back an output it never asked for, which Ninja rejects outright and
/// make matches against no rule at all.
///
/// Structural rather than a comparison against the command line, and that is the
/// point: a compile may pass `-MT` more than once (gcc concatenates the targets),
/// and `-MQ` escapes make-special characters on the way out, so a token in the
/// file need not equal any argument the launcher parsed.
///
/// A PHONY rule — `header:` with nothing after the colon, which `-MP` emits per
/// dependency — is excluded: its target is a path the compiler reported, so it
/// must be reconciled like any other, or a consumer's replayed depfile points
/// `-MP`'s deleted-header protection at files it cannot stat.
///
/// @param depFileText The depfile contents.
/// @return Every non-phony rule target, in emission order, with duplicates kept.
[[nodiscard]] std::vector<std::string> ParseDepFileTargets(std::string_view depFileText);

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
/// @return The `<SRCROOT>`/`<BUILDTREE>` token, or a ManifestFailure naming the
///         resolved source and why it has no token (direct mode is then unavailable
///         for this TU, which is the safe outcome — the ordinary preprocessed key
///         still works). It carries the path because this is the refusal a real
///         build meets: `RecordManifest` asks here first and returns before
///         BuildManifest ever runs, so without it the commonest reason a TU never
///         caches has nowhere to be said (issue #68).
[[nodiscard]] std::expected<std::string, ManifestFailure> CanonicalSourceToken(std::string_view sourcePath,
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
/// Every refusal names the path it refused over, and no two refusals share a
/// reason. Both halves are load-bearing and neither is decoration: the compile
/// succeeds either way, so this note is the only thing standing between an operator
/// and bisecting a build to find out why one translation unit stopped shortcutting
/// (issue #68).
///
/// @param inputs This compile's source, dependencies, working directory, stamp and
///               object key.
/// @param layout This build's roots.
/// @return The manifest, or a ManifestFailure naming the offending path and which
///         of `FaultTable`'s faults it was.
[[nodiscard]] std::expected<DirectManifest, ManifestFailure> BuildManifest(ManifestInputs const& inputs,
                                                                           PathCanon::Layout const& layout);

/// Whether a manifest asserts nothing at all about the files it was recorded from.
///
/// One owner for a rule two places have to act on: `ValidateManifest` refuses such a
/// manifest, and the launcher names the refusal in its note. Spelling the predicate
/// at both sites would let the rule and the sentence explaining it drift apart --
/// which is the shape of the defect this guard exists for.
///
/// `all_of` over no entries is true, so an empty manifest does not pass a check, it
/// skips one, and the object it points at is then served however the sources move.
/// `BuildManifest` cannot produce one -- the TU is always entry one and its presence
/// is that function's own precondition (issue #49 / issue #51) -- so an empty entry
/// set is a decode artefact or an older format, and a matching toolchain stamp says
/// nothing about the sources.
///
/// Deliberately `empty()` and not "fewer than two": a manifest holding only the TU is
/// what a translation unit including nothing legitimately records, and after
/// `BuildManifest`'s `NoProjectDeps` refusal it is the only way one gets written.
/// @param manifest The manifest to judge.
/// @return True when it has no entries.
[[nodiscard]] bool ManifestAssertsNothing(DirectManifest const& manifest) noexcept;

/// Re-check every entry against the filesystem.
///
/// Localizes each canonical path to this machine and compares a fresh content
/// hash with the recorded one. Any mismatch, missing file, or stamp change means
/// the cached object may not correspond to today's headers.
///
/// A manifest that `ManifestAssertsNothing` reports on is refused before either
/// question is asked: `all_of` over no entries is true, so such a manifest does not
/// pass the filesystem check, it skips it.
/// @param manifest       The recorded manifest.
/// @param layout         This machine's roots.
/// @param toolchainStamp This machine's current toolchain identity.
/// @return True when the manifest asserts something, every entry still matches and
///         the stamp agrees.
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
