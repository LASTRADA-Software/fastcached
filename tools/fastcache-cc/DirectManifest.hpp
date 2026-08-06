// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <cstdint>
#include <expected>
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
/// @param absolutePath The file to hash.
/// @return The hash as hex, or empty on any read failure.
[[nodiscard]] std::string HashFileContents(std::string_view absolutePath);

/// Extract the include paths from captured `/showIncludes` text.
///
/// Reads the *localized* form the compiler emitted (absolute native paths), not
/// the canonical form: the manifest is built at the moment of a real compile, so
/// the paths are still this machine's.
/// @param showIncludesText Captured compiler output containing include notes.
/// @return Every included path, in emission order, with duplicates preserved.
[[nodiscard]] std::vector<std::string> ParseIncludePaths(std::string_view showIncludesText);

/// Build a manifest from one compile's include set.
///
/// Toolchain headers are dropped (the stamp covers them); project headers are
/// canonicalized and hashed. Entries are deduplicated and sorted by canonical
/// path, so the same build state yields byte-identical manifests on any machine.
/// @param includePaths   Absolute include paths, as emitted by the compiler.
/// @param layout         This build's roots.
/// @param toolchainStamp Identity standing in for the toolchain headers.
/// @param objectKey      Key the compiled object is already stored under.
/// @return The manifest, or DirectError when a project header cannot be canonicalized.
[[nodiscard]] std::expected<DirectManifest, DirectError> BuildManifest(std::vector<std::string> const& includePaths,
                                                                       PathCanon::Layout const& layout,
                                                                       std::string_view toolchainStamp,
                                                                       std::string_view objectKey);

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
/// @param canonicalSource Canonical token for the translation unit's path.
/// @param relativizedArgs The compile arguments, already relativized.
/// @param toolchainStamp  The toolchain identity.
/// @return A stable hex digest.
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
/// @return A stable hex digest over the manifest key plus every entry.
[[nodiscard]] std::string ComputeHeaderStateDigest(std::string_view manifestKey, DirectManifest const& manifest);

} // namespace FastCache::Cc
