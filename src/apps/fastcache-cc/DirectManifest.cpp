// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "KeyDigest.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace FastCache::Cc
{
namespace
{

    /// Manifest wire version. Bumped whenever the encoding changes shape, so an
    /// older launcher rejects a newer manifest instead of misreading it.
    constexpr std::uint8_t ManifestVersion = 1;

    /// Path prefixes whose headers are treated as immutable toolchain content.
    /// Matched case-insensitively: Windows paths reach us in whatever case the
    /// compiler emitted, which is not stable across include-directory spellings.
    constexpr std::array<std::string_view, 4> ToolchainMarkers {
        R"(\windows kits\)",
        R"(\microsoft visual studio\)",
        R"(\vcpkg_installed\)",
        R"(\vcpkg\installed\)",
    };

    /// Fold a path to a comparable form: lowercased, with every separator unified.
    ///
    /// The two sides of a prefix test arrive in different spellings — CMake exports
    /// `FASTCACHE_SOURCE_DIR` as `D:/Project` (forward slashes) while `cl` emits
    /// includes as `D:\Project\...` (backslashes). Comparing them raw makes every
    /// project header look like it lies outside the root, which silently classifies
    /// the whole manifest as toolchain content and yields an empty manifest.
    [[nodiscard]] std::string ToComparable(std::string_view text)
    {
        std::string out { text };
        // PathCanon's fold, not std::tolower: case-folding decides whether a
        // header lies under a root, and a locale-dependent fold lets two machines
        // classify byte-identical content differently. See PathCanon::AsciiLower.
        std::ranges::transform(out, out.begin(), [](char c) {
            auto const lowered = PathCanon::AsciiLower(c);
            return lowered == '/' ? '\\' : lowered;
        });
        return out;
    }

    void AppendU32(std::string& out, std::uint32_t value)
    {
        // Big-endian, matching the compile-cache framing so both codecs read the
        // same way on every host.
        out.push_back(static_cast<char>((value >> 24) & 0xFFU));
        out.push_back(static_cast<char>((value >> 16) & 0xFFU));
        out.push_back(static_cast<char>((value >> 8) & 0xFFU));
        out.push_back(static_cast<char>(value & 0xFFU));
    }

    void AppendField(std::string& out, std::string_view field)
    {
        AppendU32(out, static_cast<std::uint32_t>(field.size()));
        out.append(field);
    }

    /// Sequential reader over the encoded form; every read is bounds-checked so a
    /// truncated or hostile manifest yields Malformed rather than reading past the
    /// buffer.
    class Cursor
    {
      public:
        explicit Cursor(std::string_view bytes) noexcept:
            _bytes { bytes }
        {
        }

        [[nodiscard]] bool ReadU8(std::uint8_t& out) noexcept
        {
            if (_offset + 1 > _bytes.size())
                return false;
            out = static_cast<std::uint8_t>(_bytes[_offset]);
            ++_offset;
            return true;
        }

        [[nodiscard]] bool ReadU32(std::uint32_t& out) noexcept
        {
            if (_offset + 4 > _bytes.size())
                return false;
            out = (static_cast<std::uint32_t>(static_cast<unsigned char>(_bytes[_offset])) << 24)
                  | (static_cast<std::uint32_t>(static_cast<unsigned char>(_bytes[_offset + 1])) << 16)
                  | (static_cast<std::uint32_t>(static_cast<unsigned char>(_bytes[_offset + 2])) << 8)
                  | static_cast<std::uint32_t>(static_cast<unsigned char>(_bytes[_offset + 3]));
            _offset += 4;
            return true;
        }

        [[nodiscard]] bool ReadField(std::string& out)
        {
            std::uint32_t length = 0;
            if (!ReadU32(length) || _offset + length > _bytes.size())
                return false;
            out.assign(_bytes.substr(_offset, length));
            _offset += length;
            return true;
        }

        [[nodiscard]] bool AtEnd() const noexcept
        {
            return _offset == _bytes.size();
        }

      private:
        std::string_view _bytes;
        std::size_t _offset { 0 };
    };

    /// What a resolved dependency path is to a manifest.
    ///
    /// A three-way answer rather than IsToolchainHeader's two, because "outside
    /// both roots" and "not anchored anywhere" are different facts with opposite
    /// consequences: the first is toolchain content the stamp already covers, the
    /// second is a path this build could not place at all, and conflating them is
    /// what silently dropped every relative path from a manifest.
    enum class PathRole : std::uint8_t
    {
        Project,    ///< Under a root: canonicalizes to a token and is hashed.
        Toolchain,  ///< Covered collectively by the toolchain stamp; dropped.
        Unanchored, ///< Neither absolute nor resolvable: has no portable form.
    };

    /// Classify a path that ResolveAgainst has already resolved.
    ///
    /// The anchor is decided FIRST, and that order is load-bearing:
    /// IsToolchainHeader reports every path outside both roots as toolchain, and a
    /// relative path lies under no root, so asking it first answers "toolchain" for
    /// every unanchored path and drops it silently. Cc::IsCheckable and
    /// Cc::PortableForm are the two other callers of that classifier and both
    /// already order it this way.
    ///
    /// Switches without a `default:`, as those two do, so a fourth Anchor state is
    /// a compile error here rather than a silent fall-through.
    ///
    /// @param resolved A path as ResolveAgainst returned it.
    /// @param layout   This build's roots.
    /// @return What the manifest should do with it.
    [[nodiscard]] PathRole ClassifyResolved(std::string_view resolved, PathCanon::Layout const& layout)
    {
        if (resolved.empty())
            return PathRole::Unanchored;

        switch (PathCanon::AnchorForLayout(resolved, layout))
        {
            case PathCanon::Anchor::WorkingDirectory:
                // Still relative after ResolveAgainst, so the working directory could
                // not place it and the file it names is unknown.
                return PathRole::Unanchored;
            case PathCanon::Anchor::DriveRelative:
            case PathCanon::Anchor::Absolute:
                // Both fall through to the root tests, siding with the KEY FILTER
                // rather than with the replay guard, which skips a drive-relative
                // path outright. The two differ by what each needs and by what being
                // wrong costs: the guard needs a path to STAT and answers "present"
                // when it cannot, so probing `C:foo` against the wrong anchor would
                // discard every hit carrying it. A manifest entry is OPENED rather
                // than probed, and an unreadable one is a refusal on the recording
                // side (HashFileContents yields nothing -> Malformed) and a
                // non-validation on the reading side -- safe both ways. So the
                // stronger question is worth asking: under a drive-relative root
                // (`C:src\proj`) such a path canonicalizes to a token that is
                // portable precisely because the consumer substitutes its own root,
                // and dropping it on the anchor alone would silently un-cover a
                // project header, which is the defect this classification exists to
                // close. What must never happen is the branch above -- resolving
                // `C:foo` against the compile's working directory, which is not the
                // directory it is anchored to (issue #65).
                break;
        }
        return IsToolchainHeader(resolved, layout) ? PathRole::Toolchain : PathRole::Project;
    }

    /// Canonicalize a path already classified as Project.
    /// @param resolved An absolute, normalized path under one of the roots.
    /// @param layout   This build's roots.
    /// @return The token, or nullopt when Canonicalize declined to rewrite it.
    [[nodiscard]] std::optional<std::string> ProjectToken(std::string const& resolved, PathCanon::Layout const& layout)
    {
        auto canonical = PathCanon::Canonicalize(resolved, layout);
        // Canonicalize returns its input verbatim for a path it did not rewrite, so
        // inequality is what says a token was produced -- and it is the only test
        // there is, nothing in PathCanon being able to fail.
        if (canonical == resolved)
            return std::nullopt;
        return canonical;
    }

} // namespace

std::string EncodeManifest(DirectManifest const& manifest)
{
    std::string out;
    out.push_back(static_cast<char>(ManifestVersion));
    AppendField(out, manifest.toolchainStamp);
    AppendField(out, manifest.objectKey);
    AppendU32(out, static_cast<std::uint32_t>(manifest.entries.size()));
    for (auto const& entry: manifest.entries)
    {
        AppendField(out, entry.canonicalPath);
        AppendField(out, entry.contentHash);
    }
    return out;
}

std::expected<DirectManifest, DirectError> DecodeManifest(std::string_view bytes)
{
    Cursor cursor { bytes };

    std::uint8_t version = 0;
    if (!cursor.ReadU8(version))
        return std::unexpected(DirectError::Malformed);
    if (version != ManifestVersion)
        return std::unexpected(DirectError::UnknownVersion);

    DirectManifest manifest;
    if (!cursor.ReadField(manifest.toolchainStamp) || !cursor.ReadField(manifest.objectKey))
        return std::unexpected(DirectError::Malformed);

    std::uint32_t count = 0;
    if (!cursor.ReadU32(count))
        return std::unexpected(DirectError::Malformed);

    manifest.entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        DirectManifest::Entry entry;
        if (!cursor.ReadField(entry.canonicalPath) || !cursor.ReadField(entry.contentHash))
            return std::unexpected(DirectError::Malformed);
        manifest.entries.push_back(std::move(entry));
    }

    // Trailing bytes mean the encoding is not what this version wrote; refuse it
    // rather than silently ignoring a field a newer writer appended.
    if (!cursor.AtEnd())
        return std::unexpected(DirectError::Malformed);

    return manifest;
}

bool IsToolchainHeader(std::string_view absolutePath, PathCanon::Layout const& layout)
{
    auto const comparable = ToComparable(absolutePath);

    // A vcpkg tree nested under the build tree is toolchain content even though it
    // is canonicalizable, so the marker check comes first.
    for (auto const marker: ToolchainMarkers)
        if (comparable.contains(marker))
            return true;

    // Anything under a build root is project content; anything else has no
    // canonical form, so it cannot be listed per-file and must ride the stamp.
    auto const underRoot = [&comparable](std::string const& root) {
        return !root.empty() && comparable.starts_with(ToComparable(root));
    };
    return !underRoot(layout.sourceRoot) && !underRoot(layout.buildTree);
}

std::string HashFileContents(std::string_view absolutePath)
{
    std::string const path { absolutePath };
    // std::ifstream rather than std::fopen: the stream owns the handle, so every
    // early return closes it without a hand-written fclose on each path.
    std::ifstream file { path, std::ios::binary };
    if (!file)
        return {};

    MurmurHash3 digest;
    std::array<std::byte, 64 * 1024> buffer {};
    while (file)
    {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        auto const read = static_cast<std::size_t>(file.gcount());
        if (read == 0)
            break;
        digest.Update(std::span<std::byte const> { buffer.data(), read });
    }
    if (file.bad())
        return {};

    // A 128-bit digest, and the byte count no longer has to be carried beside it.
    //
    // This used to be one CRC32C paired with the exact length, on the reasoning
    // that the pairing made "an accidental same-length collision the only way to
    // mistake two headers for each other". True, and still only 32 bits against
    // exactly that case -- which is the one that matters, because a header edit
    // that preserves length is ordinary. This value is what a direct hit
    // revalidates against, so a collision here does not miss: it decides an
    // edited header is unchanged and serves the stale object under a zero exit
    // code. Same defect as issue #63 on the key itself, and fixed with the same
    // digest, which mixes the length in during finalisation anyway.
    //
    // Deliberately distinct from the empty string returned above for a file that
    // could not be read: ValidateManifest compares this value for equality, and
    // an unreadable header must not compare equal to anything, including another
    // unreadable one.
    return digest.ToHex();
}

std::string NormalizePath(std::string_view rawPath)
{
    return std::filesystem::path { rawPath }.lexically_normal().make_preferred().string();
}

std::string NormalizeForLayout(std::string_view rawPath, PathCanon::Layout const& layout)
{
    auto path = NormalizePath(rawPath);
    // make_preferred() above answered with the HOST's separator; every test that
    // follows is the LAYOUT's. See the header for the failure this closes.
    if (!PathCanon::IsWindowsLayout(layout))
        std::ranges::replace(path, '\\', '/');
    return path;
}

std::optional<std::string_view> IncludeNotePath(std::string_view line) noexcept
{
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);

    auto const start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return std::nullopt;

    auto path = line.substr(start);
    if (!path.starts_with(IncludeNoteMarker))
        return std::nullopt;

    path.remove_prefix(IncludeNoteMarker.size());
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
        path.remove_prefix(1);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
        path.remove_suffix(1);
    return path;
}

std::vector<std::string> ParseIncludePaths(std::string_view showIncludesText)
{
    // Recognition lives in IncludeNotePath, not here: SplitIncludeNotes removes
    // exactly the lines this collects, and a rule spelled twice is a rule the two
    // can drift apart on — which for the splitter means a note hashed as source.
    std::vector<std::string> paths;
    std::size_t offset = 0;
    while (offset < showIncludesText.size())
    {
        auto lineEnd = showIncludesText.find('\n', offset);
        if (lineEnd == std::string_view::npos)
            lineEnd = showIncludesText.size();
        auto const line = showIncludesText.substr(offset, lineEnd - offset);
        offset = lineEnd + 1;

        if (auto const path = IncludeNotePath(line); path.has_value() && !path->empty())
            paths.emplace_back(*path);
    }
    return paths;
}

namespace
{
    /// Which half of a depfile rule a walk collects.
    enum class RuleSide : std::uint8_t
    {
        Targets,      ///< Before the unescaped `:` — outputs the build system named.
        Dependencies, ///< After it — the files the compiler reported.
    };

    /// Walk a GNU depfile and collect one side of every rule.
    ///
    /// One walker for both sides, because the splice, the rule-separator rule and
    /// the escaping rule are the same question asked twice; a second copy is a
    /// copy that can come to disagree about a drive letter or a `\ ` escape.
    /// @param depFileText The depfile contents.
    /// @param side        Which half to collect.
    /// @return Every path token on that side, in emission order, duplicates kept.
    [[nodiscard]] std::vector<std::string> ParseDepFile(std::string_view depFileText, RuleSide side)
    {
        std::vector<std::string> paths;

        // Splice out line continuations first, so a dependency list wrapped across
        // several lines reads as one rule. Real depfiles wrap aggressively — gcc
        // breaks at ~76 columns — so a parser that ignored this would see only the
        // first handful of headers.
        std::string spliced;
        spliced.reserve(depFileText.size());
        for (std::size_t i = 0; i < depFileText.size(); ++i)
        {
            if (depFileText[i] == '\\' && i + 1 < depFileText.size() && depFileText[i + 1] == '\n')
            {
                spliced.push_back(' ');
                ++i;
                continue;
            }
            if (depFileText[i] == '\\' && i + 2 < depFileText.size() && depFileText[i + 1] == '\r'
                && depFileText[i + 2] == '\n')
            {
                spliced.push_back(' ');
                i += 2;
                continue;
            }
            spliced.push_back(depFileText[i]);
        }

        // Walk each rule: everything before the unescaped ':' is a target (an output,
        // not a dependency); everything after it is a dependency list.
        for (auto const& rule: std::views::split(std::string_view { spliced }, '\n'))
        {
            std::string_view line { rule.begin(), rule.end() };
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            // Find the rule separator, skipping ':' that is escaped or part of a
            // Windows drive letter ("C:\..." must not read as a target boundary).
            std::size_t colon = std::string_view::npos;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '\\')
                {
                    ++i; // skip the escaped character
                    continue;
                }
                if (line[i] != ':')
                    continue;
                // The letter rule is PathCanon's, so all four drive tests share one
                // definition; what follows the colon is deliberately not asked, because
                // the question here is where a rule ends and a drive-relative "C:foo"
                // is still one token.
                bool const driveLetter = i == 1 && PathCanon::IsDriveLetter(line[0]);
                if (!driveLetter)
                {
                    colon = i;
                    break;
                }
            }
            if (colon == std::string_view::npos)
                continue; // not a rule line (blank, or a stray continuation remnant)

            auto const deps = line.substr(colon + 1);

            // A phony rule (`header:` with nothing after the colon, which `-MP`
            // emits per header) names a path the COMPILER reported, not one the
            // build system did, so it is not a target for this purpose — the very
            // distinction a caller asking for targets is asking about.
            if (side == RuleSide::Targets && deps.find_first_not_of(" \t") == std::string_view::npos)
                continue;

            auto const span = side == RuleSide::Targets ? line.substr(0, colon) : deps;

            // Split the dependency list on unescaped whitespace. `\ ` is a literal
            // space inside a path, which is ordinary on Windows and in any checkout
            // under a directory with a space in its name.
            std::string current;
            auto const flush = [&paths, &current]() {
                if (!current.empty())
                    paths.push_back(current);
                current.clear();
            };
            for (std::size_t i = 0; i < span.size(); ++i)
            {
                char const c = span[i];
                if (c == '\\' && i + 1 < span.size() && (span[i + 1] == ' ' || span[i + 1] == '\\' || span[i + 1] == ':'))
                {
                    current.push_back(span[i + 1]);
                    ++i;
                    continue;
                }
                if (c == ' ' || c == '\t')
                {
                    flush();
                    continue;
                }
                current.push_back(c);
            }
            flush();
        }

        return paths;
    }
} // namespace

std::vector<std::string> ParseDepFilePaths(std::string_view depFileText)
{
    return ParseDepFile(depFileText, RuleSide::Dependencies);
}

std::vector<std::string> ParseDepFileTargets(std::string_view depFileText)
{
    return ParseDepFile(depFileText, RuleSide::Targets);
}

std::string ResolveAgainst(std::string_view rawPath, std::string_view workingDirectory, PathCanon::Layout const& layout)
{
    // Normalized before anything is decided, for the reason NormalizePath states:
    // a driver echoes a path as resolved, so `..` segments and mixed separators
    // arrive verbatim and unnormalized spellings become distinct entries. Through
    // NormalizeForLayout, so the anchor test below reads the layout's separators
    // rather than this host's.
    auto normalized = NormalizeForLayout(rawPath, layout);
    if (normalized.empty() || workingDirectory.empty())
        return normalized;

    switch (PathCanon::AnchorForLayout(normalized, layout))
    {
        case PathCanon::Anchor::Absolute:
            return normalized;
        case PathCanon::Anchor::DriveRelative:
            // Returned unresolved, deliberately: `C:foo` is anchored to drive C's
            // own current directory, so gluing the compile's working directory in
            // front of it is not a resolution but a different wrong answer
            // (issue #65). ClassifyResolved then puts it to the root tests, exactly
            // as the key filter does.
            return normalized;
        case PathCanon::Anchor::WorkingDirectory:
            break;
    }

    // Joined and normalized again: the join is what collapses a leading `..`
    // against the working directory's last component.
    return NormalizeForLayout((std::filesystem::path { workingDirectory } / std::filesystem::path { normalized }).string(),
                              layout);
}

std::string AnchorWorkingDirectory(std::string_view directory, PathCanon::Layout const& layout)
{
    auto normalized = NormalizeForLayout(directory, layout);
    std::error_code ec;
    auto const canonicalDirectory = std::filesystem::weakly_canonical(std::filesystem::path { normalized }, ec);
    if (ec)
        return normalized;

    // Longest root first, matching CanonicalizeOne's rule: a build tree nested
    // inside the source root must anchor to the build tree, or the tail spliced
    // back below would be relative to the wrong one.
    std::array<std::string const*, 2> roots { &layout.buildTree, &layout.sourceRoot };
    if (layout.buildTree.size() < layout.sourceRoot.size())
        std::ranges::reverse(roots);

    for (auto const* root: roots)
    {
        if (root->empty())
            continue;
        auto const canonicalRoot = std::filesystem::weakly_canonical(std::filesystem::path { *root }, ec);
        if (ec)
            continue;

        // lexically_relative, not a string prefix: both sides are now filesystem
        // identities, and this is what says "inside" without another spelling test.
        auto const tail = canonicalDirectory.lexically_relative(canonicalRoot);
        if (tail.empty() || *tail.begin() == "..")
            continue;
        // The ROOT's spelling with the tail appended, never the canonical form: the
        // point is to speak the layout's language, not the filesystem's.
        return NormalizeForLayout((std::filesystem::path { *root } / tail).string(), layout);
    }
    return normalized;
}

std::expected<std::string, DirectError> CanonicalSourceToken(std::string_view sourcePath,
                                                             PathCanon::Layout const& layout,
                                                             std::string_view workingDirectory)
{
    auto const resolved = ResolveAgainst(sourcePath, workingDirectory, layout);
    if (ClassifyResolved(resolved, layout) != PathRole::Project)
        return std::unexpected(DirectError::NotCanonical);

    auto token = ProjectToken(resolved, layout);
    if (!token.has_value())
        return std::unexpected(DirectError::NotCanonical);
    return *std::move(token);
}

std::expected<DirectManifest, DirectError> BuildManifest(ManifestInputs const& inputs, PathCanon::Layout const& layout)
{
    DirectManifest manifest { .toolchainStamp = inputs.toolchainStamp, .objectKey = inputs.objectKey, .entries = {} };

    // The TU first, and its absence is fatal rather than merely thin. A manifest
    // that does not name the file being compiled revalidates everything except it,
    // so editing a `.cpp` body while leaving every header untouched replays a stale
    // object forever (issue #49 / issue #51). This used to be the caller's job and
    // therefore a comment; here it is the function's own precondition.
    auto const resolvedSource = ResolveAgainst(inputs.sourcePath, inputs.workingDirectory, layout);
    auto const sourceToken = CanonicalSourceToken(inputs.sourcePath, layout, inputs.workingDirectory);
    if (!sourceToken.has_value())
        return std::unexpected(sourceToken.error());

    auto const record = [&manifest](std::string token, std::string const& resolved) -> std::expected<void, DirectError> {
        auto hash = HashFileContents(resolved);
        if (hash.empty())
            return std::unexpected(DirectError::Malformed);
        manifest.entries.push_back({ .canonicalPath = std::move(token), .contentHash = std::move(hash) });
        return {};
    };

    if (auto const stored = record(*sourceToken, resolvedSource); !stored.has_value())
        return std::unexpected(stored.error());

    for (auto const& rawPath: inputs.includePaths)
    {
        auto const resolved = ResolveAgainst(rawPath, inputs.workingDirectory, layout);
        switch (ClassifyResolved(resolved, layout))
        {
            case PathRole::Toolchain:
                // Covered by the stamp; listing it would make the manifest
                // machine-specific for no gain in what a hit revalidates.
                continue;
            case PathRole::Unanchored:
                // Refused, not dropped. Reaching here means the path is relative and
                // the working directory could not place it, so the file it names is
                // unknown -- and a dropped project header is the silent stale serve
                // this whole classification exists to prevent. Refusing costs this
                // compile direct mode; the ordinary preprocessed key still serves it.
                return std::unexpected(DirectError::NotCanonical);
            case PathRole::Project: {
                auto token = ProjectToken(resolved, layout);
                if (!token.has_value())
                    return std::unexpected(DirectError::NotCanonical);
                if (auto const stored = record(*std::move(token), resolved); !stored.has_value())
                    return std::unexpected(stored.error());
                break;
            }
        }
    }

    // Deduplicate then sort: `/showIncludes` repeats a header once per inclusion
    // site, a GNU depfile names the TU among its own dependencies, and two machines
    // must agree on the order for the derived key to match.
    std::ranges::sort(manifest.entries, [](auto const& a, auto const& b) { return a.canonicalPath < b.canonicalPath; });
    auto const duplicates = std::ranges::unique(
        manifest.entries, [](auto const& a, auto const& b) { return a.canonicalPath == b.canonicalPath; });
    manifest.entries.erase(duplicates.begin(), duplicates.end());

    return manifest;
}

bool ValidateManifest(DirectManifest const& manifest, PathCanon::Layout const& layout, std::string_view toolchainStamp)
{
    // A different toolchain invalidates the whole manifest: its headers are not
    // listed individually, so nothing else here would notice they changed.
    if (manifest.toolchainStamp != toolchainStamp)
        return false;

    // HashFileContents returns empty for a deleted or unreadable header, which
    // cannot equal a recorded hash — so removal invalidates the manifest too.
    return std::ranges::all_of(manifest.entries, [&layout](DirectManifest::Entry const& entry) {
        auto const localized = PathCanon::Localize(entry.canonicalPath, layout);
        return HashFileContents(localized) == entry.contentHash;
    });
}

std::string ComputeManifestKey(std::string_view canonicalSource,
                               std::vector<std::string> const& relativizedArgs,
                               std::string_view toolchainStamp)
{
    // The tag tracks `objkey-v*` and MUST be bumped with it, even when nothing
    // about the manifest's own shape changed. A manifest stores the object key BY
    // VALUE, and its own key is a function of (canonical source, relativized
    // args, toolchain stamp) — none of which the object-key schema touches — so a
    // manifest written by an older launcher is still found, still validates, and
    // still points at an older object. Direct mode is on by default and
    // short-circuits before the preprocessed path, so without this bump the
    // re-key that `objkey-v*` exists to force never happens on the default path:
    // for v2 that meant the Windows entries written while `/EP /P` sent the
    // preprocessed text to a file — the ones carrying no content from the source
    // at all — kept being served indefinitely.
    //
    // v3 has a second, independent reason, and either alone would require it: the
    // key's own construction changed (issue #63 — four salted CRC32C runs over one
    // blob, which carried 32 bits rather than 128, to a real 128-bit digest). A v2
    // manifest is therefore not merely pointing at a v2 object; its key is not
    // reachable by this build at all.
    //
    // v4 moves this tag ALONE, which the lock-step above permits: the coupling is
    // one-way. An `objkey` bump must drag this tag with it, because a manifest
    // points at an object key BY VALUE and would otherwise keep resolving to a
    // pre-bump object. The reverse costs nothing — an unreachable manifest is
    // re-recorded on the next compile and points at objects that are still valid
    // under whatever tag they were written with — so this tag moving did not
    // require the object key to move. (`objkey` reached v4 separately, for issue
    // #64's dependency-set re-key; see ComputeKey for why it was taken rather than
    // forced.)
    //
    // The bump is required because the defect it retires is invisible to the key.
    // A build whose TU source was absolute but whose header paths were relative
    // (a relative `-I`, or a compile run from the source directory) recorded a
    // manifest naming the TU and nothing else: BuildManifest asked
    // IsToolchainHeader before classifying the anchor, and every relative path
    // lies under no root, so every one of them was dropped as toolchain content.
    // This fix changes neither `canonicalSource` nor the args for such a build, so
    // those manifests keep the same key, keep being found, and keep validating —
    // and a direct hit never reaches RecordManifest, so nothing would ever
    // overwrite one. Edit a dropped header and the stale object is served under a
    // zero exit code, indefinitely. Re-keying is the only thing that retires them.
    KeyDigest digest { "manifest-v4" };
    digest.Field(toolchainStamp);
    digest.Field(canonicalSource);
    for (auto const& arg: relativizedArgs)
        digest.Item(arg);
    return digest.ToHex();
}

std::string ComputeHeaderStateDigest(std::string_view manifestKey, DirectManifest const& manifest)
{
    // The tag stays at v1 while `objkey`/`manifest` move to v3, deliberately.
    // Those two move in lock-step because each names entries that outlive the
    // other's schema; nothing is stored under this digest at all, so there is no
    // stale entry for a bump to fix and it would be a version with no work to do.
    // The trigger to revisit: the day anything is persisted or sent under this
    // value, it joins the lock-step group.
    KeyDigest digest { "header-state-v1" };
    digest.Field(manifestKey);
    for (auto const& entry: manifest.entries)
    {
        digest.Path(entry.canonicalPath);
        digest.Item(entry.contentHash);
    }
    return digest.ToHex();
}

} // namespace FastCache::Cc
