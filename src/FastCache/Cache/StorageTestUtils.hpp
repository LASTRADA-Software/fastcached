// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEntry.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// Shared helpers for the storage-layer unit tests. Header-only so each test
/// translation unit gets its own `inline` copies without an extra link target.
namespace FastCache::Testing
{

/// Convert ASCII text to the byte vector the storage API consumes.
/// @param text Source text.
/// @return Bytes with the same contents as `text`.
[[nodiscard]] inline std::vector<std::byte> MakeBytes(std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (auto const c: text)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

/// Copy a cache entry's (reference-counted, immutable) payload into an owning
/// byte vector, so tests can use the full `std::vector` interface (`==`,
/// `.empty()`, `.size()`, `operator[]`) against the value regardless of how
/// storage represents it internally.
/// @param entry Entry whose payload to materialize.
/// @return A vector holding a copy of the entry's value bytes.
[[nodiscard]] inline std::vector<std::byte> ValueOf(CacheEntry const& entry)
{
    auto const bytes = entry.ValueBytes();
    return std::vector<std::byte> { bytes.begin(), bytes.end() };
}

/// Convert a byte span back to a string for value comparisons.
/// @param bytes Source bytes.
/// @return String with the same contents as `bytes`.
[[nodiscard]] inline std::string Decode(std::span<std::byte const> bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (auto const b: bytes)
        out.push_back(static_cast<char>(b));
    return out;
}

/// RAII helper that owns a unique temporary file path and removes it on
/// construction (in case of a stale leftover) and destruction.
struct TempFile
{
    std::filesystem::path path;

    /// @param prefix Filename prefix identifying the test that owns the file.
    /// @param extension File extension including the dot (default `.cow`).
    explicit TempFile(std::string_view prefix = "fastcache-storage-test-", std::string_view extension = ".cow")
    {
        std::mt19937_64 rng { std::random_device {}() };
        path = std::filesystem::temp_directory_path()
               / (std::string { prefix } + std::to_string(rng()) + std::string { extension });
        std::filesystem::remove(path);
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempFile(TempFile const&) = delete;
    TempFile& operator=(TempFile const&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

} // namespace FastCache::Testing
