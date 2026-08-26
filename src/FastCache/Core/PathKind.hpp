// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>

namespace FastCache
{

/// Whether @p path denotes a single FILE rather than a directory.
///
/// What exists answers for itself; for a path that does not exist yet there is
/// nothing to go on but the spelling, so a file extension is read as naming a
/// file and an extension-less name as naming a directory.
///
/// One definition rather than one per caller, because two decisions have to agree
/// on it and are made in different places:
///
/// - `fastcached` resolves its physical shard count from it. A `storage_path` that
///   names one file stays single-file for backward compatibility (`cache.cow`);
///   anything else fans out into `shard-NN.cow` files inside a directory.
/// - The `--install-service` handover creates an absent `ServiceSpec::ownedPaths`
///   entry, and may only do that for one that is meant to be a directory.
///
/// They disagreed once, on the same value. `--storage=D:\fc\cache.cow` was
/// `create_directories`-ed by the handover, so the very next start saw an existing
/// DIRECTORY and silently fanned out into shards beside the single cache file the
/// operator had asked for.
///
/// The spelling rule only ever has to hold for the FIRST use of a path: from the
/// second on, the file or the directory is there and answers for itself.
///
/// @param path The path to classify. An empty path has no extension and is
///        therefore reported as a directory; callers that can be handed one are
///        expected to have rejected it already.
/// @return True for an existing regular file, or a not-yet-existing path carrying
///         a file extension. False for an existing directory, or a
///         not-yet-existing path without one.
[[nodiscard]] bool PathNamesAFile(std::filesystem::path const& path) noexcept;

} // namespace FastCache
