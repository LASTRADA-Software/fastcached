// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"

#include <FastCache/Core/Crc32c.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
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

    /// Collapse a compiler-emitted include path to one stable form.
    ///
    /// `/showIncludes` echoes the path as *resolved*, which preserves whatever the
    /// `#include` spelling contained: `D:\src\LabBase\../ctrllib/ListOb.hpp`, mixed
    /// separators, and doubled slashes all occur in real output. Two spellings of
    /// the same header must produce one canonical token, or a manifest entry
    /// recorded via one spelling would never validate against the other.
    ///
    /// Deliberately LEXICAL, not `weakly_canonical`: touching the filesystem also
    /// rewrites 8.3 short components to their long form (`YTAMAS~1.JPS` becomes
    /// `y.tamashevich.JPSC`), and a path so rewritten no longer shares a prefix with
    /// a source root spelled the other way — which silently drops every project
    /// header from the manifest. Lexical normalization removes `..` and unifies
    /// separators without altering the path's identity.
    [[nodiscard]] std::string NormalizePath(std::string_view rawPath)
    {
        return std::filesystem::path { rawPath }.lexically_normal().make_preferred().string();
    }

    /// Fold a path to a comparable form: lowercased, with every separator unified.
    ///
    /// The two sides of a prefix test arrive in different spellings — CMake exports
    /// `FASTCACHE_SRCROOT` as `D:/Lastrada` (forward slashes) while `cl` emits
    /// includes as `D:\Lastrada\...` (backslashes). Comparing them raw makes every
    /// project header look like it lies outside the root, which silently classifies
    /// the whole manifest as toolchain content and yields an empty manifest.
    [[nodiscard]] std::string ToComparable(std::string_view text)
    {
        std::string out { text };
        std::ranges::transform(out, out.begin(), [](unsigned char c) {
            auto const lowered = static_cast<char>(std::tolower(c));
            return lowered == '/' ? '\\' : lowered;
        });
        return out;
    }

    /// One CRC32C lane over `data`, salted so lanes are independent. Mirrors
    /// CacheKey.cpp's construction so both key spaces are the same shape.
    [[nodiscard]] std::uint32_t Lane(std::uint8_t salt, std::string_view data)
    {
        std::uint32_t state = Crc32c::Seed;
        std::array<std::byte, 1> const saltByte { static_cast<std::byte>(salt) };
        Crc32c::Update(state, saltByte);
        Crc32c::Update(state, std::span<std::byte const> { reinterpret_cast<std::byte const*>(data.data()), data.size() });
        return Crc32c::Finalise(state);
    }

    /// Four independent lanes over one blob, rendered as 32 hex chars — a 128-bit
    /// key, wide enough that an accidental collision across a build is negligible.
    [[nodiscard]] std::string WideDigest(std::string_view blob)
    {
        std::array<std::uint32_t, 4> const lanes {
            Lane(0xE5, blob),
            Lane(0xF6, blob),
            Lane(0x17, blob),
            Lane(0x28, blob),
        };
        std::string key;
        key.reserve(32);
        for (auto const lane: lanes)
            key += std::format("{:08x}", lane);
        return key;
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

    std::uint32_t state = Crc32c::Seed;
    std::uint64_t length = 0;
    std::array<std::byte, 64 * 1024> buffer {};
    while (file)
    {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        auto const read = static_cast<std::size_t>(file.gcount());
        if (read == 0)
            break;
        length += read;
        Crc32c::Update(state, std::span<std::byte const> { buffer.data(), read });
    }
    if (file.bad())
        return {};

    // Length is folded in alongside the digest: CRC32C is not collision-resistant
    // by design, and pairing it with the exact byte count makes an accidental
    // same-length collision the only way to mistake two headers for each other.
    return std::format("{:08x}{:016x}", Crc32c::Finalise(state), length);
}

std::vector<std::string> ParseIncludePaths(std::string_view showIncludesText)
{
    // cl and clang-cl both prefix include notes with this text; the remainder of
    // the line is the path, indented by nesting depth.
    constexpr std::string_view Marker = "Note: including file:";

    std::vector<std::string> paths;
    std::size_t offset = 0;
    while (offset < showIncludesText.size())
    {
        auto lineEnd = showIncludesText.find('\n', offset);
        if (lineEnd == std::string_view::npos)
            lineEnd = showIncludesText.size();
        auto line = showIncludesText.substr(offset, lineEnd - offset);
        offset = lineEnd + 1;

        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        auto const marker = line.find(Marker);
        if (marker == std::string_view::npos)
            continue;

        auto path = line.substr(marker + Marker.size());
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
            path.remove_prefix(1);
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
            path.remove_suffix(1);
        if (!path.empty())
            paths.emplace_back(path);
    }
    return paths;
}

std::expected<DirectManifest, DirectError> BuildManifest(std::vector<std::string> const& includePaths,
                                                         PathCanon::Layout const& layout,
                                                         std::string_view toolchainStamp,
                                                         std::string_view objectKey)
{
    DirectManifest manifest { .toolchainStamp = std::string { toolchainStamp },
                              .objectKey = std::string { objectKey },
                              .entries = {} };

    for (auto const& rawPath: includePaths)
    {
        auto const path = NormalizePath(rawPath);

        if (IsToolchainHeader(path, layout))
            continue;

        auto canonical = PathCanon::Canonicalize(path, layout);
        if (!canonical.has_value())
            return std::unexpected(DirectError::NotCanonical);

        // A project header that canonicalizes to itself lies under no root, so it
        // has no portable form; refuse rather than store a machine-specific path.
        if (*canonical == path)
            return std::unexpected(DirectError::NotCanonical);

        auto hash = HashFileContents(path);
        if (hash.empty())
            return std::unexpected(DirectError::Malformed);

        manifest.entries.push_back({ .canonicalPath = std::move(*canonical), .contentHash = std::move(hash) });
    }

    // Deduplicate then sort: `/showIncludes` repeats a header once per inclusion
    // site, and two machines must agree on the order for the derived key to match.
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
        return localized.has_value() && HashFileContents(*localized) == entry.contentHash;
    });
}

std::string ComputeManifestKey(std::string_view canonicalSource,
                               std::vector<std::string> const& relativizedArgs,
                               std::string_view toolchainStamp)
{
    std::string blob;
    blob += "manifest-v1";
    blob.push_back('\x00');
    blob += toolchainStamp;
    blob.push_back('\x00');
    blob += canonicalSource;
    blob.push_back('\x00');
    for (auto const& arg: relativizedArgs)
    {
        blob += arg;
        blob.push_back('\x01');
    }
    return WideDigest(blob);
}

std::string ComputeHeaderStateDigest(std::string_view manifestKey, DirectManifest const& manifest)
{
    std::string blob;
    blob += "header-state-v1";
    blob.push_back('\x00');
    blob += manifestKey;
    blob.push_back('\x00');
    for (auto const& entry: manifest.entries)
    {
        blob += entry.canonicalPath;
        blob.push_back('\x02');
        blob += entry.contentHash;
        blob.push_back('\x01');
    }
    return WideDigest(blob);
}

} // namespace FastCache::Cc
