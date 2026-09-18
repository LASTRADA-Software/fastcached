// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Owner.hpp>

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

/// @file DurableFile.hpp
/// Reading a file whole and replacing one indivisibly and durably, for every file a node keeps
/// in its state directory and reads back as a unit: the Raft state and snapshot, and the roster
/// a worker adopted (#178).
///
/// One copy, because every step here fails for its own reason -- a path that is a directory,
/// a permission, a disk that gave up mid-read, a power loss between the write and the platter
/// -- and a second author of the same four steps gets one of them wrong.
namespace FastCache::Consensus
{

/// Open a file in binary mode, spelling the path the way the platform wants.
///
/// `std::fopen` takes a narrow path, which on Windows is converted through
/// the active code page — so a directory containing a character that page
/// cannot represent would fail to open for a reason having nothing to do with
/// the storage. `_wfopen` takes the `wstring` the path already holds there.
/// @param path File to open.
/// @param mode An `fopen` mode string.
/// @return The stream, or nullptr.
[[nodiscard]] gsl::owner<std::FILE*> OpenBinary(std::filesystem::path const& path, char const* mode);

/// Flush a stream all the way to the platter.
///
/// `fflush` alone only pushes the C library's buffer into the kernel, which a
/// power loss still discards -- so it is the pair that makes a write durable,
/// and the reason this is one helper rather than two calls at each site.
/// @param file The open stream.
/// @return True when both stages succeeded.
[[nodiscard]] bool FlushToDisk(std::FILE* file) noexcept;

/// Read a whole file into memory.
///
/// Returns the reason rather than a bare failure flag. Every step here can
/// fail for a different and actionable cause -- the path is a directory, the
/// permissions are wrong, the disk gave up mid-read -- and a caller handed
/// only "false" can say no more than "cannot read <path>", which is the one
/// thing the operator already knew. `std::filesystem` reports through
/// `error_code` and `fopen` through `errno`, so both are translated here where
/// they are still in scope; a caller cannot recover them afterwards.
/// @param path What to read.
/// @return The bytes; NOTHING when there is no file at all, which is not the same answer as
///         an empty one; or why it could not be read.
[[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, ConsensusError> ReadFileIfPresent(
    std::filesystem::path const& path);

/// Replace `path` with `body`, indivisibly.
///
/// Written beside the target and renamed over it: rename is the only single
/// filesystem operation that replaces a file's contents in one step, so a
/// crash leaves either the whole previous file or the whole new one.
/// @param path What to replace.
/// @param body The new contents.
/// @return Nothing, or why it could not be replaced.
[[nodiscard]] std::expected<void, ConsensusError> ReplaceFileAtomically(std::filesystem::path const& path,
                                                                        std::span<std::byte const> body);

} // namespace FastCache::Consensus
